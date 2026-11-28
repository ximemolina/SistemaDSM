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
    if ((rc = alloc_init(C)) != DSM_OK) return rc;
    for (int i = 0; i < MAX_WAITERS; i++) sem_init(&waiters[i].sem, 0, 0);
    net_register(MSG_BARRIER_ARRIVE,  on_barrier_arrive,  MSG_POOL);
    net_register(MSG_BARRIER_RELEASE, on_barrier_release, MSG_INLINE);
    net_register(MSG_LOCK_ACQUIRE, on_lock_acquire, MSG_POOL);
    net_register(MSG_LOCK_RELEASE, on_lock_release, MSG_POOL);
    net_register(MSG_LOCK_GRANT,   on_lock_grant,   MSG_INLINE);
    cluster_register();
    if ((rc = stats_init()) != DSM_OK) return rc;
    if ((rc = tasks_init()) != DSM_OK) return rc;
    if ((rc = net_start()) != DSM_OK) return rc;
    if ((rc = cluster_start_heartbeat()) != DSM_OK) return rc;
    rc = cluster_join(); if (rc == DSM_OK) alloc_sync(); return rc;
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

void dsm_set_error(dsm_err_t e) { last_err = e; }
