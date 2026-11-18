/* dsm.c (AMBAS): API publica, alloc global con coordinador, barrier, locks distribuidos. */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <endian.h>
#include <arpa/inet.h>

int dsm_verbose;
static const dsm_config_t *C;
static int shut;
static __thread dsm_err_t last_err;
dsm_err_t dsm_last_error(void) { return last_err; }

/* =================== barrier =================== */
#define MAX_BARRIERS 16
typedef struct { int in_use; uint64_t id; uint32_t count, n, mask; } bar_t;
static bar_t bars[MAX_BARRIERS];
static pthread_mutex_t bar_mu = PTHREAD_MUTEX_INITIALIZER, bar_call_mu = PTHREAD_MUTEX_INITIALIZER;
static sem_t bar_release;

static void on_barrier_arrive(const msg_header_t *h, const void *unused) {    /* POOL sin espera */
    (void)unused;
    uint32_t mask = 0; int fire = 0;
    pthread_mutex_lock(&bar_mu);
    bar_t *b = NULL;
    for (int i = 0; i < MAX_BARRIERS; i++) if (bars[i].in_use && bars[i].id == h->page_id) { b = &bars[i]; break; }
    if (!b) for (int i = 0; i < MAX_BARRIERS; i++) if (!bars[i].in_use) { b = &bars[i]; memset(b, 0, sizeof *b); b->in_use = 1; b->id = h->page_id; b->n = h->req_id; break; }
    if (b) { b->count++; b->mask |= 1u << h->src_node; if (b->count >= b->n) { mask = b->mask; fire = 1; b->in_use = 0; } }
    pthread_mutex_unlock(&bar_mu);
    if (!fire) return;
    for (node_id_t n = 0; n < C->max_nodes; n++)
        if (mask & (1u << n)) { msg_header_t r = { .type = MSG_BARRIER_RELEASE, .page_id = h->page_id }; net_send(n, &r, NULL); }
}
static void on_barrier_release(const msg_header_t *h, const void *unused) { (void)h; (void)unused; sem_post(&bar_release); }

dsm_err_t dsm_barrier(uint32_t id, uint32_t n) {
    if (n == 0 || n > C->max_nodes) return DSM_ERR_BAD_ARG;
    pthread_mutex_lock(&bar_call_mu);
    msg_header_t a = { .type = MSG_BARRIER_ARRIVE, .page_id = id, .req_id = n };
    dsm_err_t rc = net_send(cluster_barrier_coord(id), &a, NULL);
    if (rc == DSM_OK) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += C->barrier_timeout_ms / 1000;
        int r; do { r = sem_timedwait(&bar_release, &ts); } while (r != 0 && errno == EINTR);
        if (r != 0) rc = DSM_ERR_TIMEOUT;
    }
    pthread_mutex_unlock(&bar_call_mu);
    return rc;
}

/* =================== locks distribuidos =================== */
#define MAX_LOCKS 64
#define LOCKQ 64
#define MAX_WAITERS 64
typedef struct { int used; uint64_t id; int held; uint32_t q[LOCKQ]; unsigned qh, qn; } lock_t;
typedef struct { int used; uint64_t id; sem_t sem; } waiter_t;
static lock_t locks[MAX_LOCKS];
static waiter_t waiters[MAX_WAITERS];
static pthread_mutex_t lock_mu = PTHREAD_MUTEX_INITIALIZER, wait_mu = PTHREAD_MUTEX_INITIALIZER;

static void grant(node_id_t n, uint64_t id) { msg_header_t g = { .type = MSG_LOCK_GRANT, .page_id = id }; net_send(n, &g, NULL); }

static void on_lock_acquire(const msg_header_t *h, const void *unused) {     /* POOL sin espera */
    (void)unused; int do_grant = 0;
    pthread_mutex_lock(&lock_mu);
    lock_t *l = NULL;
    for (int i = 0; i < MAX_LOCKS; i++) if (locks[i].used && locks[i].id == h->page_id) { l = &locks[i]; break; }
    if (!l) for (int i = 0; i < MAX_LOCKS; i++) if (!locks[i].used) { l = &locks[i]; memset(l, 0, sizeof *l); l->used = 1; l->id = h->page_id; break; }
    if (l) {
        if (!l->held) { l->held = 1; do_grant = 1; }
        else if (l->qn < LOCKQ) { l->q[(l->qh + l->qn) % LOCKQ] = h->src_node; l->qn++; }
    }
    pthread_mutex_unlock(&lock_mu);
    if (do_grant) grant(h->src_node, h->page_id);
}
static void on_lock_release(const msg_header_t *h, const void *unused) {     /* POOL sin espera */
    (void)unused; int next = -1;
    pthread_mutex_lock(&lock_mu);
    for (int i = 0; i < MAX_LOCKS; i++) if (locks[i].used && locks[i].id == h->page_id) {
        lock_t *l = &locks[i];
        if (l->qn) { next = (int)l->q[l->qh]; l->qh = (l->qh + 1) % LOCKQ; l->qn--; } else l->held = 0;
        break;
    }
    pthread_mutex_unlock(&lock_mu);
    if (next >= 0) grant((node_id_t)next, h->page_id);
}
static void on_lock_grant(const msg_header_t *h, const void *unused) {       /* INLINE */
    (void)unused;
    pthread_mutex_lock(&wait_mu);
    for (int i = 0; i < MAX_WAITERS; i++) if (waiters[i].used && waiters[i].id == h->page_id) { sem_post(&waiters[i].sem); break; }
    pthread_mutex_unlock(&wait_mu);
}

dsm_err_t dsm_lock(uint32_t id) {
    waiter_t *w = NULL;
    pthread_mutex_lock(&wait_mu);
    for (int i = 0; i < MAX_WAITERS; i++) if (!waiters[i].used) { w = &waiters[i]; w->used = 1; w->id = id; break; }
    pthread_mutex_unlock(&wait_mu);
    if (!w) return DSM_ERR_NOMEM;
    while (sem_trywait(&w->sem) == 0) {}
    msg_header_t a = { .type = MSG_LOCK_ACQUIRE, .page_id = id };
    dsm_err_t rc = net_send(cluster_lock_coord(id), &a, NULL);
    if (rc == DSM_OK) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += C->barrier_timeout_ms / 1000;
        int r; do { r = sem_timedwait(&w->sem, &ts); } while (r != 0 && errno == EINTR);
        if (r != 0) rc = DSM_ERR_TIMEOUT;
    }
    pthread_mutex_lock(&wait_mu); w->used = 0; pthread_mutex_unlock(&wait_mu);
    return rc;
}
dsm_err_t dsm_unlock(uint32_t id) {
    msg_header_t r = { .type = MSG_LOCK_RELEASE, .page_id = id };
    return net_send(cluster_lock_coord(id), &r, NULL);
}

/* =================== alloc global =================== */
/* El coordinador (nodo 0) lleva el cursor de cada slot y la tabla de asignaciones
 * (nodo, proceso, orden, rango, nombre). Si la solicitud no cabe en un slot se reserva
 * un rango contiguo que cruza slots vecinos libres. */
#define MAX_ALLOCS 2048
#define LIST_CHUNK 32
typedef struct __attribute__((packed)) { uint8_t kind; uint32_t pid; uint64_t size; uint64_t addr; char name[DSM_NAME_LEN]; } alloc_req_t;
typedef struct __attribute__((packed)) { int32_t err; uint64_t addr; uint64_t size; } alloc_rep_t;
typedef struct __attribute__((packed)) { uint32_t node, pid, seq; uint8_t live; uint64_t addr, size; char name[DSM_NAME_LEN]; } alloc_wire_t;
enum { AK_ALLOC = 0, AK_FREE = 1, AK_LIST = 2 };

static alloc_entry_t atab[MAX_ALLOCS];
static unsigned acount;
static uint64_t cursor[DSM_MAX_NODES];
static pthread_mutex_t alloc_mu = PTHREAD_MUTEX_INITIALIZER;

static int64_t carve(node_id_t req, uint64_t size) {          /* con alloc_mu tomado; devuelve offset o -1 */
    uint64_t mem = C->mem_per_node; uint32_t n = C->max_nodes;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = (req + i) % n;
        if (!cluster_is_alive(s)) continue;
        if (cursor[s] + size <= mem) { uint64_t off = (uint64_t)s * mem + cursor[s]; cursor[s] += size; return (int64_t)off; }
    }
    for (uint32_t s = 0; s < n; s++) {                         /* rango contiguo que cruza slots */
        if (!cluster_is_alive(s)) continue;
        uint64_t tail = mem - cursor[s];
        if (tail == 0 || tail >= size) continue;
        uint64_t rem = size - tail; uint32_t j = s + 1; int ok = 1;
        while (rem) { if (j >= n || !cluster_is_alive(j) || cursor[j] != 0) { ok = 0; break; } rem -= rem < mem ? rem : mem; j++; }
        if (!ok) continue;
        uint64_t off = (uint64_t)s * mem + cursor[s];
        cursor[s] = mem; rem = size - tail; j = s + 1;
        while (rem) { uint64_t take = rem < mem ? rem : mem; cursor[j] = take; rem -= take; j++; }
        return (int64_t)off;
    }
    return -1;
}

static void on_alloc_request(const msg_header_t *h, const void *payload) {   /* POOL sin espera, en el coordinador */
    char out[sizeof(uint32_t) + sizeof(uint64_t) + LIST_CHUNK * sizeof(alloc_wire_t)];
    uint32_t outlen = 0;
    if (h->payload_len != sizeof(alloc_req_t)) return;
    alloc_req_t rq; memcpy(&rq, payload, sizeof rq);
    uint32_t pid = ntohl(rq.pid); uint64_t size = be64toh(rq.size), addr = be64toh(rq.addr);
    rq.name[DSM_NAME_LEN - 1] = 0;
    alloc_rep_t rep = { 0 };
    pthread_mutex_lock(&alloc_mu);
    if (rq.kind == AK_ALLOC) {
        uint64_t ps = C->page_size; size = (size + ps - 1) / ps * ps;
        alloc_entry_t *ex = NULL;
        if (rq.name[0]) for (unsigned i = 0; i < acount; i++) if (atab[i].live && !strcmp(atab[i].name, rq.name)) { ex = &atab[i]; break; }
        if (ex) {                                                  /* asignacion con nombre ya existente: mismo puntero */
            if (size > ex->size) rep.err = htonl(DSM_ERR_BAD_ARG); else { rep.addr = htobe64(ex->addr); rep.size = htobe64(ex->size); }
        } else if (size == 0 || acount >= MAX_ALLOCS) rep.err = htonl(DSM_ERR_NOMEM);
        else {
            int64_t off = carve(h->src_node, size);
            if (off < 0) rep.err = htonl(DSM_ERR_NOMEM);
            else {
                alloc_entry_t *e = &atab[acount++]; memset(e, 0, sizeof *e);
                e->node = h->src_node; e->pid = pid; e->live = 1; e->size = size;
                e->addr = C->global_base_addr + (uint64_t)off;
                for (unsigned i = 0; i + 1 < acount; i++) if (atab[i].node == e->node && atab[i].pid == pid) e->seq++;
                e->seq++;
                snprintf(e->name, DSM_NAME_LEN, "%s", rq.name);
                rep.addr = htobe64(e->addr); rep.size = htobe64(size);
            }
        }
        memcpy(out, &rep, sizeof rep); outlen = sizeof rep;
    } else if (rq.kind == AK_FREE) {
        int found = 0;
        for (unsigned i = 0; i < acount; i++) if (atab[i].live && atab[i].addr == addr) { atab[i].live = 0; found = 1; break; }
        if (!found) rep.err = htonl(DSM_ERR_BAD_ARG);
        memcpy(out, &rep, sizeof rep); outlen = sizeof rep;
    } else {                                                       /* AK_LIST: addr = indice inicial */
        uint32_t cnt = 0; uint64_t i = addr;
        for (; i < acount && cnt < LIST_CHUNK; i++, cnt++) {
            alloc_wire_t w = { htonl(atab[i].node), htonl(atab[i].pid), htonl(atab[i].seq), atab[i].live,
                               htobe64(atab[i].addr), htobe64(atab[i].size), { 0 } };
            memcpy(w.name, atab[i].name, DSM_NAME_LEN);
            memcpy(out + sizeof(uint32_t) + sizeof(uint64_t) + cnt * sizeof w, &w, sizeof w);
        }
        uint32_t c = htonl(cnt); uint64_t nx = htobe64(i < acount ? i : UINT64_MAX);
        memcpy(out, &c, 4); memcpy(out + 4, &nx, 8);
        outlen = (uint32_t)(12 + cnt * sizeof(alloc_wire_t));
    }
    pthread_mutex_unlock(&alloc_mu);
    msg_header_t r = { .type = MSG_ALLOC_REPLY, .req_id = h->req_id, .payload_len = outlen };
    net_send(h->src_node, &r, out);
}

static dsm_err_t alloc_rpc(uint8_t kind, const char *name, uint64_t size, uint64_t addr, void *reply, uint32_t *rlen) {
    alloc_req_t rq; memset(&rq, 0, sizeof rq);
    rq.kind = kind; rq.pid = htonl((uint32_t)getpid()); rq.size = htobe64(size); rq.addr = htobe64(addr);
    if (name) snprintf(rq.name, DSM_NAME_LEN, "%s", name);
    return net_rpc_call(cluster_alloc_coord(), MSG_ALLOC_REQUEST, 0, 0, &rq, sizeof rq, reply, rlen, C->request_timeout_ms * 2);
}

void *dsm_alloc_named(const char *name, size_t size) {
    alloc_rep_t rep; uint32_t rl = sizeof rep;
    dsm_err_t rc = alloc_rpc(AK_ALLOC, name, size, 0, &rep, &rl);
    if (rc == DSM_OK && rl != sizeof rep) rc = DSM_ERR_PROTO;
    if (rc == DSM_OK && ntohl((uint32_t)rep.err) != 0) rc = (dsm_err_t)ntohl((uint32_t)rep.err);
    last_err = rc;
    return rc == DSM_OK ? (void *)(uintptr_t)be64toh(rep.addr) : NULL;
}
void *dsm_alloc(size_t size) { return dsm_alloc_named(NULL, size); }

dsm_err_t dsm_free(void *ptr) {
    alloc_rep_t rep; uint32_t rl = sizeof rep;
    dsm_err_t rc = alloc_rpc(AK_FREE, NULL, 0, (uint64_t)(uintptr_t)ptr, &rep, &rl);
    if (rc == DSM_OK && ntohl((uint32_t)rep.err)) rc = (dsm_err_t)ntohl((uint32_t)rep.err);
    return rc;                                  /* la memoria no se reutiliza (cursor monotono) */
}

dsm_err_t dsm_alloc_list(uint64_t first, alloc_entry_t *out, uint32_t max, uint32_t *count, uint64_t *next) {
    char buf[12 + LIST_CHUNK * sizeof(alloc_wire_t)]; uint32_t rl = sizeof buf;
    dsm_err_t rc = alloc_rpc(AK_LIST, NULL, 0, first, buf, &rl);
    if (rc != DSM_OK) return rc;
    uint32_t c; uint64_t nx; memcpy(&c, buf, 4); memcpy(&nx, buf + 4, 8);
    c = ntohl(c); *next = be64toh(nx); *count = 0;
    for (uint32_t i = 0; i < c && i < max && 12 + (i + 1) * sizeof(alloc_wire_t) <= rl; i++) {
        alloc_wire_t w; memcpy(&w, buf + 12 + i * sizeof w, sizeof w);
        out[i] = (alloc_entry_t){ ntohl(w.node), ntohl(w.pid), ntohl(w.seq), w.live, be64toh(w.addr), be64toh(w.size), { 0 } };
        memcpy(out[i].name, w.name, DSM_NAME_LEN); (*count)++;
    }
    return DSM_OK;
}

/* =================== prefetch / utilidades =================== */
static dsm_err_t prefetch(void *ptr, size_t len, access_t acc) {
    char *base = pager_app_addr(0); size_t ps = C->page_size;
    uint64_t total = (uint64_t)C->max_nodes * C->mem_per_node;
    if (!len || (char *)ptr < base || (char *)ptr + len > base + total) return DSM_ERR_BAD_ARG;
    page_id_t p0 = (page_id_t)(((char *)ptr - base) / ps), p1 = (page_id_t)(((char *)ptr + len - 1 - base) / ps);
    for (page_id_t p = p0; p <= p1; p++) { dsm_err_t rc = pager_prefetch(p, acc); if (rc != DSM_OK) return rc; }
    return DSM_OK;
}
dsm_err_t dsm_prefetch_write(void *ptr, size_t len) { return prefetch(ptr, len, ACC_WRITE); }
dsm_err_t dsm_prefetch_read(void *ptr, size_t len)  { return prefetch(ptr, len, ACC_READ); }

void *dsm_slot_base(node_id_t node) {
    if (node >= C->max_nodes) return NULL;
    return pager_app_addr(((uint64_t)node * C->mem_per_node) / C->page_size);
}

/* =================== init / shutdown =================== */
dsm_err_t dsm_init(const char *config_path) {
    static dsm_config_t cfg;
    dsm_verbose = getenv("DSM_VERBOSE") != NULL;
    dsm_err_t rc;
    if ((rc = config_load(config_path, &cfg)) != DSM_OK) return rc;
    C = config_get();
    if ((rc = cluster_init(C)) != DSM_OK) return rc;
    if ((rc = net_init(C)) != DSM_OK) return rc;
    if ((rc = pager_init(C)) != DSM_OK) return rc;
    if ((rc = dir_init(C)) != DSM_OK) return rc;
    sem_init(&bar_release, 0, 0);
    for (int i = 0; i < MAX_WAITERS; i++) sem_init(&waiters[i].sem, 0, 0);
    net_register(MSG_BARRIER_ARRIVE,  on_barrier_arrive,  MSG_POOL);
    net_register(MSG_BARRIER_RELEASE, on_barrier_release, MSG_INLINE);
    net_register(MSG_LOCK_ACQUIRE, on_lock_acquire, MSG_POOL);
    net_register(MSG_LOCK_RELEASE, on_lock_release, MSG_POOL);
    net_register(MSG_LOCK_GRANT,   on_lock_grant,   MSG_INLINE);
    net_register(MSG_ALLOC_REQUEST, on_alloc_request, MSG_POOL);
    cluster_register();
    if ((rc = stats_init()) != DSM_OK) return rc;
    if ((rc = tasks_init()) != DSM_OK) return rc;
    if ((rc = net_start()) != DSM_OK) return rc;
    if ((rc = cluster_start_heartbeat()) != DSM_OK) return rc;
    return cluster_join();                       /* espera a todos los nodos si wait_all_nodes = 1 */
}

dsm_err_t dsm_shutdown(void) {
    if (shut) return DSM_OK;
    shut = 1;
    if (dsm_verbose) {
        pager_stats_t s; pager_get_stats(&s);
        fprintf(stderr, "[dsm n%u] fallos R=%llu W=%llu | paginas recibidas=%llu | invalidaciones=%llu | fetch=%llu (retenidos %llu) | punch=%llu | reintentos=%llu\n",
                cluster_self_id(), (unsigned long long)s.faults_read, (unsigned long long)s.faults_write, (unsigned long long)s.pages_received,
                (unsigned long long)s.invalidations, (unsigned long long)s.fetches_served, (unsigned long long)s.fetches_delayed,
                (unsigned long long)s.punched, (unsigned long long)s.retries);
    }
    tasks_shutdown();                     /* primero las tareas: pueden estar usando memoria DSM */
    pager_release_all();
    pager_shutdown();
    cluster_shutdown();
    net_shutdown();
    dir_shutdown();
    return DSM_OK;
}