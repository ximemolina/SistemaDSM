/*
 * pager.c (A): memoria local, doble mapeo, handler de SIGSEGV, MSI local.
 * Locks: mutexes por franja (page_id % NSTRIPE) para el estado de pagina y un
 * mutex aparte para la tabla de fallos y la cola de peticiones. Orden: franja -> fallos.
 * Las esperas (pagina que llega, cola de peticiones) se hacen con semaforos POSIX.
 * Los mutexes se toman un instante y nunca durante una espera de red ni de semaforo.
 * Nota: el handler de SIGSEGV es sincrono (lo provoca el propio hilo de la aplicacion,
 * que nunca mantiene estos mutexes al acceder a la memoria compartida), por lo que
 * tomar mutexes dentro del handler no puede autointerbloquearse.
 */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <endian.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define NSTRIPE    64
#define MAX_FAULTS 256
#define REQ_RING   1024

typedef struct { page_state_t state; uint64_t granted_ns; } page_local_t;
typedef struct { page_id_t page; access_t acc; int in_use, completed, refs, timeouts; sem_t done; } fault_ctx_t;
typedef struct { page_id_t page; access_t acc; } req_t;

static const dsm_config_t *C;
static size_t psz;
static uint64_t total, npages;
static char *app_base, *sys_base;
static int mem_fd = -1;
static page_local_t *loc;

static pthread_mutex_t stripe_lk[NSTRIPE];
static pthread_mutex_t fault_lk;
static fault_ctx_t faults[MAX_FAULTS];
static req_t ring[REQ_RING];
static unsigned ring_head, ring_tail;
static sem_t ring_sem;
static pthread_t sender_th;
static volatile int sender_run;
static atomic_ullong st_fr, st_fw, st_rx, st_inv, st_fetch, st_delay, st_punch, st_retry;

#define SLK(p)  pthread_mutex_lock(&stripe_lk[(p) % NSTRIPE])
#define SULK(p) pthread_mutex_unlock(&stripe_lk[(p) % NSTRIPE])
#define FLK()   pthread_mutex_lock(&fault_lk)
#define FULK()  pthread_mutex_unlock(&fault_lk)

/* ---- salida fatal async-signal-safe (sin printf) ---- */
static void put_u64(char *b, size_t *n, uint64_t v) {
    char t[24]; int k = 0; if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[(*n)++] = t[--k];
}
static void fatal_page(const char *msg, page_id_t p, node_id_t home) {
    char b[300]; size_t n = 0;
    for (const char *s = msg; *s && n < 200; s++) b[n++] = *s;
    const char *a = " | pagina="; for (const char *s = a; *s; s++) b[n++] = *s;
    put_u64(b, &n, p);
    const char *c = " home=nodo "; for (const char *s = c; *s; s++) b[n++] = *s;
    put_u64(b, &n, home); b[n++] = '\n';
    ssize_t r = write(2, b, n); (void)r;
    _exit(70);
}

static fault_ctx_t *find_fault(page_id_t p) {
    for (int i = 0; i < MAX_FAULTS; i++)
        if (faults[i].in_use && !faults[i].completed && faults[i].page == p) return &faults[i];
    return NULL;
}
static fault_ctx_t *alloc_fault(page_id_t p) {
    for (int i = 0; i < MAX_FAULTS; i++)
        if (!faults[i].in_use) { faults[i].in_use = 1; faults[i].completed = 0; faults[i].refs = 0; faults[i].timeouts = 0; faults[i].page = p; return &faults[i]; }
    return NULL;
}
static void ring_push(page_id_t p, access_t a) {       /* con fault_lk tomado */
    ring[ring_head % REQ_RING] = (req_t){ p, a }; ring_head++;
}

static inline int satisfied(page_state_t st, access_t acc) {
    return (acc == ACC_READ && (st == PAGE_SHARED || st == PAGE_MODIFIED)) || (acc == ACC_WRITE && st == PAGE_MODIFIED);
}

/* Espera la pagina; si vence el timeout reenvia el PAGE_REQUEST (el directorio trata un
 * duplicado como una peticion normal serializada) y solo despues de N reintentos aborta. */
static void wait_fault(fault_ctx_t *f) {
    for (;;) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint32_t ms = C->request_timeout_ms;
        ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int r; do { r = sem_timedwait(&f->done, &ts); } while (r != 0 && errno == EINTR);
        if (r == 0) return;
        FLK();
        if (f->completed) { FULK(); continue; }
        int n = ++f->timeouts; page_id_t p = f->page; access_t a = f->acc;
        if (n > (int)C->request_retries) { FULK(); fatal_page("[dsm] DSM_ERR_TIMEOUT: la pagina no llego (nodo caido?)", p, cluster_home_of(p)); }
        ring_push(p, a);
        FULK();
        sem_post(&ring_sem);
        atomic_fetch_add(&st_retry, 1);
    }
}

static void resolve_fault(page_id_t p, access_t acc) {
    SLK(p);
    page_state_t st = loc[p].state;
    if (satisfied(st, acc)) { SULK(p); return; }        /* otro hilo ya lo resolvio */
    FLK();
    fault_ctx_t *f = find_fault(p);
    int sender = 0;
    if (!f) {
        f = alloc_fault(p);
        if (!f) { FULK(); SULK(p); fatal_page("[dsm] demasiados fallos simultaneos", p, cluster_home_of(p)); }
        f->acc = acc;
        loc[p].state = (acc == ACC_WRITE) ? PAGE_PENDING_W : PAGE_PENDING_R;
        ring_push(p, acc); sender = 1;
    }
    f->refs++;
    FULK(); SULK(p);
    if (sender) sem_post(&ring_sem);
    wait_fault(f);                                       /* sin ningun lock tomado */
    FLK(); if (--f->refs == 0) f->in_use = 0; FULK();
}

static access_t fault_access(void *ctx, page_id_t p) {
#if defined(__x86_64__) && defined(REG_ERR) && !defined(DSM_STATE_FALLBACK)
    (void)p; return (((ucontext_t *)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2) ? ACC_WRITE : ACC_READ;
#else
    (void)ctx; return loc[p].state == PAGE_SHARED ? ACC_WRITE : ACC_READ;     /* respaldo por estado */
#endif
}

static void sigsegv_handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    int saved = errno;
    char *a = (char *)si->si_addr;
    if (a < app_base || a >= app_base + total) {
        struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &d, NULL);
        return;
    }
    page_id_t p = (page_id_t)(a - app_base) / psz;
    access_t acc = fault_access(ctx, p);
    if (acc == ACC_WRITE) atomic_fetch_add(&st_fw, 1); else atomic_fetch_add(&st_fr, 1);
    resolve_fault(p, acc);
    errno = saved;
}

/* Hilo emisor: saca connect/send del handler de senal */
static void *sender_main(void *arg) {
    (void)arg;
    while (sender_run) {
        if (sem_wait(&ring_sem) != 0) continue;
        if (!sender_run) break;
        FLK();
        if (ring_tail == ring_head) { FULK(); continue; }
        req_t r = ring[ring_tail % REQ_RING]; ring_tail++;
        FULK();
        dsm_err_t rc = net_send_page_request(r.page, r.acc, NULL);
        if (rc != DSM_OK) DSM_LOG("PAGE_REQUEST pagina %llu fallo (err %d); el pager reintentara", (unsigned long long)r.page, rc);
    }
    return NULL;
}

/* ---------- handlers de red ---------- */
static void punch(page_id_t p) {             /* libera la RAM de una pagina ajena ya invalidada */
    if (!C->release_memory || cluster_initial_owner(p) == cluster_self_id()) return;   /* nunca la copia de referencia */
    if (fallocate(mem_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)(p * psz), (off_t)psz) == 0)
        atomic_fetch_add(&st_punch, 1);
}

void pager_on_page_data(const msg_header_t *h, const void *bytes) {
    page_id_t p = h->page_id;
    if (p >= npages || h->payload_len != psz) { DSM_LOG("PAGE_DATA invalido"); return; }
    access_t g = (access_t)h->access;
    SLK(p);
    memcpy(sys_base + p * psz, bytes, psz);                                                      /* 1. datos */
    mprotect(app_base + p * psz, psz, g == ACC_READ ? PROT_READ : (PROT_READ | PROT_WRITE));     /* 2. permiso */
    loc[p].state = (g == ACC_READ) ? PAGE_SHARED : PAGE_MODIFIED;                                /* 3. estado */
    loc[p].granted_ns = dsm_now_ns();
    FLK();
    fault_ctx_t *f = find_fault(p);
    if (f) { f->completed = 1; for (int i = 0; i < f->refs; i++) sem_post(&f->done); }           /* 4. timbre */
    FULK(); SULK(p);
    atomic_fetch_add(&st_rx, 1);
}

dsm_err_t pager_on_invalidate(page_id_t p) {
    if (p >= npages) return DSM_ERR_BAD_ARG;
    SLK(p);
    page_state_t st = loc[p].state;
    if (st == PAGE_SHARED) {
        mprotect(app_base + p * psz, psz, PROT_NONE);
        loc[p].state = PAGE_INVALID;
        punch(p);
    } else if (st == PAGE_PENDING_R || st == PAGE_PENDING_W) {
        mprotect(app_base + p * psz, psz, PROT_NONE);    /* conserva PENDING: el PAGE_DATA siguiente es fresco */
    }
    SULK(p);
    atomic_fetch_add(&st_inv, 1);
    return DSM_OK;
}

static void sleep_ns(uint64_t ns) { struct timespec t = { (time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL) }; nanosleep(&t, NULL); }

dsm_err_t pager_on_fetch(page_id_t p, access_t acc, void *out) {
    if (p >= npages) return DSM_ERR_BAD_ARG;
    uint64_t hold = (uint64_t)C->min_hold_us * 1000ULL;
    SLK(p);
    page_state_t st = loc[p].state;
    if (st == PAGE_MODIFIED && hold) {                   /* anti-livelock: dejar que el hilo use la pagina recien recibida */
        uint64_t el = dsm_now_ns() - loc[p].granted_ns;
        if (el < hold) { SULK(p); sleep_ns(hold - el); atomic_fetch_add(&st_delay, 1); SLK(p); st = loc[p].state; }
    }
    int to_invalid = 0;
    if (st == PAGE_MODIFIED) {                           /* primero el permiso, luego la copia */
        if (acc == ACC_READ) { mprotect(app_base + p * psz, psz, PROT_READ); loc[p].state = PAGE_SHARED; }
        else { mprotect(app_base + p * psz, psz, PROT_NONE); loc[p].state = PAGE_INVALID; to_invalid = 1; }
    } else if (acc == ACC_WRITE) {
        if (st == PAGE_SHARED) { mprotect(app_base + p * psz, psz, PROT_NONE); loc[p].state = PAGE_INVALID; to_invalid = 1; }
        else if (st == PAGE_PENDING_R || st == PAGE_PENDING_W) mprotect(app_base + p * psz, psz, PROT_NONE);
    }
    memcpy(out, sys_base + p * psz, psz);
    if (to_invalid) punch(p);                            /* despues de copiar */
    SULK(p);
    atomic_fetch_add(&st_fetch, 1);
    return DSM_OK;
}

static void h_invalidate(const msg_header_t *h, const void *unused) {     /* MSG_POOL */
    (void)unused;
    pager_on_invalidate(h->page_id);
    msg_header_t a = { .type = MSG_INVALIDATE_ACK, .page_id = h->page_id, .req_id = h->req_id };
    net_send(h->src_node, &a, NULL);
}
static void h_fetch(const msg_header_t *h, const void *unused) {          /* MSG_POOL */
    (void)unused;
    void *buf = malloc(psz); if (!buf) return;
    if (pager_on_fetch(h->page_id, (access_t)h->access, buf) == DSM_OK) {
        msg_header_t r = { .type = MSG_FETCH_REPLY, .page_id = h->page_id, .req_id = h->req_id, .access = h->access, .payload_len = (uint32_t)psz };
        net_send(h->src_node, &r, buf);
    }
    free(buf);
}

/* ---------- API ---------- */
dsm_err_t pager_prefetch(page_id_t p, access_t acc) {
    if (p >= npages) return DSM_ERR_BAD_ARG;
    for (int i = 0; i < 16; i++) {
        SLK(p); int ok = satisfied(loc[p].state, acc); SULK(p);
        if (ok) return DSM_OK;
        resolve_fault(p, acc);
    }
    return DSM_ERR_TIMEOUT;
}

/* write-back al apagar: devuelve al home las paginas M (con datos) y las S (sin datos) */
void pager_release_all(void) {
    node_id_t me = cluster_self_id();
    for (page_id_t p = 0; p < npages; p++) {
        page_state_t st = loc[p].state;
        if (st != PAGE_MODIFIED && st != PAGE_SHARED) continue;
        int dirty = (st == PAGE_MODIFIED);
        if (dirty && cluster_initial_owner(p) == me) continue;      /* su propio rango se va con el nodo */
        msg_header_t h = { .type = MSG_PAGE_RELEASE, .page_id = p, .access = dirty ? ACC_WRITE : ACC_READ,
                           .payload_len = dirty ? (uint32_t)psz : 0 };
        net_send(cluster_home_of(p), &h, dirty ? sys_base + p * psz : NULL);
    }
}

static int make_memfd(void) {
    int fd = -1;
#ifdef MFD_CLOEXEC
    fd = memfd_create("dsm", MFD_CLOEXEC);
#else
    fd = (int)syscall(SYS_memfd_create, "dsm", 1U);
#endif
    if (fd >= 0) return fd;
    char name[64]; snprintf(name, sizeof name, "/dsm.%d.%u", (int)getpid(), (unsigned)cluster_self_id());   /* respaldo: shm_open */
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
    return fd;
}

dsm_err_t pager_init(const dsm_config_t *cfg) {
    C = cfg; psz = cfg->page_size;
    total = (uint64_t)cfg->max_nodes * cfg->mem_per_node; npages = total / psz;
#if !defined(__x86_64__)
    fprintf(stderr, "[dsm] AVISO: arquitectura no x86-64; se usa la deteccion lectura/escritura por estado de pagina\n");
#endif
    for (int i = 0; i < NSTRIPE; i++) pthread_mutex_init(&stripe_lk[i], NULL);
    pthread_mutex_init(&fault_lk, NULL);
    if (npages > (1ULL << 32) || !(loc = calloc(npages, sizeof *loc))) return DSM_ERR_NOMEM;
    for (int i = 0; i < MAX_FAULTS; i++) sem_init(&faults[i].done, 0, 0);
    sem_init(&ring_sem, 0, 0);
    mem_fd = make_memfd();
    if (mem_fd < 0) { perror("memfd_create/shm_open"); return DSM_ERR_NOMEM; }
    if (ftruncate(mem_fd, (off_t)total) < 0) { perror("ftruncate"); return DSM_ERR_NOMEM; }
    sys_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, mem_fd, 0);
    app_base = mmap((void *)cfg->global_base_addr, total, PROT_NONE, MAP_SHARED | MAP_NORESERVE | MAP_FIXED_NOREPLACE, mem_fd, 0);
    if (sys_base == MAP_FAILED || app_base == MAP_FAILED || app_base != (char *)cfg->global_base_addr) {
        perror("mmap (vista del sistema / aplicacion; kernel >= 4.17 para MAP_FIXED_NOREPLACE)"); return DSM_ERR_NOMEM;
    }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sigsegv_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) < 0) { perror("sigaction"); return DSM_ERR_BAD_ARG; }
    net_register(MSG_PAGE_DATA,  pager_on_page_data, MSG_INLINE);
    net_register(MSG_INVALIDATE, h_invalidate,       MSG_POOL);
    net_register(MSG_FETCH,      h_fetch,            MSG_POOL);
    sender_run = 1;
    pthread_create(&sender_th, NULL, sender_main, NULL);
    return DSM_OK;
}

void pager_shutdown(void) {
    if (!sender_run) return;
    sender_run = 0; sem_post(&ring_sem); pthread_join(sender_th, NULL);
}

void *pager_app_addr(page_id_t p) { return app_base + p * psz; }
void *pager_sys_addr(page_id_t p) { return sys_base + p * psz; }
page_state_t pager_get_state(page_id_t p) { SLK(p); page_state_t s = loc[p].state; SULK(p); return s; }

void pager_get_stats(pager_stats_t *o) {
    o->faults_read = st_fr; o->faults_write = st_fw; o->pages_received = st_rx; o->invalidations = st_inv;
    o->fetches_served = st_fetch; o->fetches_delayed = st_delay; o->punched = st_punch; o->retries = st_retry;
}

void pager_count_states(uint64_t out[4]) {       /* lectura sin lock: estadistica aproximada */
    out[0] = out[1] = out[2] = out[3] = 0;
    for (page_id_t p = 0; p < npages; p++) {
        switch (loc[p].state) {
            case PAGE_INVALID: out[0]++; break; case PAGE_SHARED: out[1]++; break;
            case PAGE_MODIFIED: out[2]++; break; default: out[3]++; break;
        }
    }
}

void pager_dump_local(FILE *f, unsigned max) {
    static const char *nm[] = { "I", "S", "M", "PENDING_R", "PENDING_W" };
    unsigned shown = 0; uint64_t more = 0;
    fprintf(f, "%-10s %-10s %-6s %-6s\n", "pagina", "estado", "home", "contrib.");
    for (page_id_t p = 0; p < npages; p++) {
        if (loc[p].state == PAGE_INVALID) continue;
        if (shown >= max) { more++; continue; }
        fprintf(f, "%-10llu %-10s %-6u %-6u\n", (unsigned long long)p, nm[loc[p].state], cluster_home_of(p), cluster_initial_owner(p));
        shown++;
    }
    if (more) fprintf(f, "... y %llu paginas mas\n", (unsigned long long)more);
}
