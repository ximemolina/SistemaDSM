/*
 * pager.c (Persona A): memoria local, doble mapeo, handler de SIGSEGV, MSI local.
 *
 * Lock del pager: un spinlock global async-signal-safe (ver docs, cambio C3).
 * Se toma un instante y NUNCA se mantiene durante una espera de red ni de semaforo.
 */
#define _GNU_SOURCE
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>

#define MAX_FAULTS 256
#define REQ_RING   256

/* Una por pagina, en todos los nodos: estado local */
typedef struct {
    page_state_t state;
    uint32_t     pid_owner;
    uint32_t     seq_order;
} page_local_t;

/* Fallo en curso para una pagina (un semaforo por fallo, N hilos pueden esperarlo) */
typedef struct {
    page_id_t page;
    int       in_use;
    int       completed;   /* la pagina ya llego; no se une nadie mas */
    int       refs;        /* hilos esperando */
    sem_t     done;
} fault_ctx_t;

typedef struct { page_id_t page; access_t acc; } req_t;

static const dsm_config_t *C;
static size_t   psz;
static uint64_t total, npages;
static char *app_base, *sys_base;
static page_local_t *loc;

static atomic_flag g_lk = ATOMIC_FLAG_INIT;
static fault_ctx_t faults[MAX_FAULTS];
static req_t ring[REQ_RING];
static unsigned ring_head, ring_tail;
static sem_t ring_sem;
static pthread_t sender_th;
static volatile int sender_run;

static atomic_ullong st_fr, st_fw, st_rx, st_inv, st_fetch;

static inline void lk(void)  { while (atomic_flag_test_and_set_explicit(&g_lk, memory_order_acquire)) sched_yield(); }
static inline void ulk(void) { atomic_flag_clear_explicit(&g_lk, memory_order_release); }

static void fatal(const char *msg) {      /* async-signal-safe */
    size_t n = 0; while (msg[n]) n++;
    ssize_t r = write(2, msg, n); (void)r;
    _exit(70);
}

static fault_ctx_t *find_fault(page_id_t p) {      /* con lk() tomado */
    for (int i = 0; i < MAX_FAULTS; i++)
        if (faults[i].in_use && !faults[i].completed && faults[i].page == p) return &faults[i];
    return NULL;
}

static fault_ctx_t *alloc_fault(page_id_t p) {     /* con lk() tomado */
    for (int i = 0; i < MAX_FAULTS; i++)
        if (!faults[i].in_use) {
            faults[i].in_use = 1; faults[i].completed = 0; faults[i].refs = 0; faults[i].page = p;
            return &faults[i];
        }
    return NULL;
}

static void wait_fault(fault_ctx_t *f) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t ms = C->request_timeout_ms;
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (sem_timedwait(&f->done, &ts) != 0) {
        if (errno == EINTR) continue;
        fatal("[dsm] DSM_ERR_TIMEOUT: la pagina no llego a tiempo (nodo caido?)\n");
    }
}

/* Lectura vs escritura: codigo de error en ucontext (x86-64), con respaldo por estado */
static access_t fault_access(void *ctx, page_id_t p) {
#if defined(__x86_64__) && defined(REG_ERR)
    (void)p;
    return (((ucontext_t *)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2) ? ACC_WRITE : ACC_READ;
#else
    (void)ctx;
    return loc[p].state == PAGE_SHARED ? ACC_WRITE : ACC_READ;
#endif
}

/* Handler minimo: solo funciones async-signal-safe (sin malloc, sin mutex, sin red) */
static void sigsegv_handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    int saved = errno;
    char *a = (char *)si->si_addr;
    if (a < app_base || a >= app_base + total) {          /* fallo real, no es del DSM */
        struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &d, NULL);
        return;
    }
    page_id_t p = (page_id_t)(a - app_base) / psz;
    access_t acc = fault_access(ctx, p);
    if (acc == ACC_WRITE) atomic_fetch_add(&st_fw, 1); else atomic_fetch_add(&st_fr, 1);

    lk();
    page_state_t st = loc[p].state;
    if ((acc == ACC_READ && (st == PAGE_SHARED || st == PAGE_MODIFIED)) ||
        (acc == ACC_WRITE && st == PAGE_MODIFIED)) {      /* otro hilo ya lo resolvio */
        ulk(); errno = saved; return;
    }
    fault_ctx_t *f = find_fault(p);
    int sender = 0;
    if (!f) {
        f = alloc_fault(p);
        if (!f) { ulk(); fatal("[dsm] demasiados fallos simultaneos\n"); }
        loc[p].state = (acc == ACC_WRITE) ? PAGE_PENDING_W : PAGE_PENDING_R;
        ring[ring_head % REQ_RING] = (req_t){ p, acc }; ring_head++;
        sender = 1;
    }
    f->refs++;
    ulk();
    if (sender) sem_post(&ring_sem);

    wait_fault(f);       /* sin ningun lock tomado */

    lk();
    if (--f->refs == 0) f->in_use = 0;
    ulk();
    errno = saved;
}

/* Hilo que envia los PAGE_REQUEST: saca la red del handler de senal (cambio C4) */
static void *sender_main(void *arg) {
    (void)arg;
    while (sender_run) {
        if (sem_wait(&ring_sem) != 0) continue;
        if (!sender_run) break;
        lk();
        req_t r = ring[ring_tail % REQ_RING]; ring_tail++;
        ulk();
        dsm_err_t rc = net_send_page_request(r.page, r.acc, NULL);
        if (rc != DSM_OK) DSM_LOG("PAGE_REQUEST pagina %llu fallo (err %d)", (unsigned long long)r.page, rc);
    }
    return NULL;
}

/* ---------- handlers de red registrados por pager ---------- */

void pager_on_page_data(const msg_header_t *h, const void *bytes) {
    page_id_t p = h->page_id;
    if (p >= npages || h->payload_len != psz) { DSM_LOG("PAGE_DATA invalido"); return; }
    access_t g = (access_t)h->access;
    lk();
    memcpy(sys_base + p * psz, bytes, psz);                                   /* 1. datos (vista del sistema) */
    mprotect(app_base + p * psz, psz, g == ACC_READ ? PROT_READ : (PROT_READ | PROT_WRITE)); /* 2. permiso */
    loc[p].state = (g == ACC_READ) ? PAGE_SHARED : PAGE_MODIFIED;             /* 3. estado */
    fault_ctx_t *f = find_fault(p);
    if (f) { f->completed = 1; for (int i = 0; i < f->refs; i++) sem_post(&f->done); }   /* 4. timbre */
    ulk();
    atomic_fetch_add(&st_rx, 1);
}

dsm_err_t pager_on_invalidate(page_id_t p) {
    if (p >= npages) return DSM_ERR_BAD_ARG;
    lk();
    page_state_t st = loc[p].state;
    if (st == PAGE_SHARED || st == PAGE_PENDING_R || st == PAGE_PENDING_W) {
        mprotect(app_base + p * psz, psz, PROT_NONE);
        if (st == PAGE_SHARED) loc[p].state = PAGE_INVALID;
        /* En PENDING el estado se conserva: el PAGE_DATA que llegue es fresco
         * (el home serializa por pagina y TCP es FIFO por conexion). */
    }
    ulk();
    atomic_fetch_add(&st_inv, 1);
    return DSM_OK;
}

dsm_err_t pager_on_fetch(page_id_t p, access_t acc, void *out) {
    if (p >= npages) return DSM_ERR_BAD_ARG;
    lk();
    page_state_t st = loc[p].state;
    if (st == PAGE_MODIFIED) {                       /* primero el permiso, luego la copia */
        if (acc == ACC_READ) { mprotect(app_base + p * psz, psz, PROT_READ); loc[p].state = PAGE_SHARED; }
        else                 { mprotect(app_base + p * psz, psz, PROT_NONE); loc[p].state = PAGE_INVALID; }
    } else if (acc == ACC_WRITE) {
        if (st == PAGE_SHARED) { mprotect(app_base + p * psz, psz, PROT_NONE); loc[p].state = PAGE_INVALID; }
        else if (st == PAGE_PENDING_R || st == PAGE_PENDING_W) mprotect(app_base + p * psz, psz, PROT_NONE);
    }
    memcpy(out, sys_base + p * psz, psz);
    ulk();
    atomic_fetch_add(&st_fetch, 1);
    return DSM_OK;
}

static void h_invalidate(const msg_header_t *h, const void *unused) {   /* MSG_POOL */
    (void)unused;
    pager_on_invalidate(h->page_id);                  /* sin lock al llamar a la red */
    msg_header_t a = { .type = MSG_INVALIDATE_ACK, .page_id = h->page_id, .req_id = h->req_id };
    net_send(h->src_node, &a, NULL);
}

static void h_fetch(const msg_header_t *h, const void *unused) {        /* MSG_POOL */
    (void)unused;
    void *buf = malloc(psz);
    if (!buf) return;
    if (pager_on_fetch(h->page_id, (access_t)h->access, buf) == DSM_OK) {
        msg_header_t r = { .type = MSG_FETCH_REPLY, .page_id = h->page_id, .req_id = h->req_id,
                           .access = h->access, .payload_len = (uint32_t)psz };
        net_send(h->src_node, &r, buf);
    }
    free(buf);
}

/* ---------- init / utilidades ---------- */

dsm_err_t pager_init(const dsm_config_t *cfg) {
    C = cfg; psz = cfg->page_size;
    total = (uint64_t)cfg->max_nodes * cfg->mem_per_node;
    npages = total / psz;
    if (npages > (1ULL << 32) || !(loc = calloc(npages, sizeof *loc))) return DSM_ERR_NOMEM;
    for (int i = 0; i < MAX_FAULTS; i++) sem_init(&faults[i].done, 0, 0);
    sem_init(&ring_sem, 0, 0);

    int fd = memfd_create("dsm", MFD_CLOEXEC);
    if (fd < 0) { perror("memfd_create"); return DSM_ERR_NOMEM; }
    if (ftruncate(fd, (off_t)total) < 0) { perror("ftruncate"); return DSM_ERR_NOMEM; }
    sys_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
    app_base = mmap((void *)cfg->global_base_addr, total, PROT_NONE,
                    MAP_SHARED | MAP_NORESERVE | MAP_FIXED_NOREPLACE, fd, 0);
    if (sys_base == MAP_FAILED || app_base == MAP_FAILED || app_base != (char *)cfg->global_base_addr) {
        perror("mmap (vista del sistema / aplicacion)"); return DSM_ERR_NOMEM;
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
    sender_run = 0; sem_post(&ring_sem);
    pthread_join(sender_th, NULL);
}

void *pager_app_addr(page_id_t p) { return app_base + p * psz; }
void *pager_sys_addr(page_id_t p) { return sys_base + p * psz; }

page_state_t pager_get_state(page_id_t p) {
    lk(); page_state_t s = loc[p].state; ulk(); return s;
}

void pager_get_stats(pager_stats_t *o) {
    o->faults_read = st_fr; o->faults_write = st_fw; o->pages_received = st_rx;
    o->invalidations = st_inv; o->fetches_served = st_fetch;
}
