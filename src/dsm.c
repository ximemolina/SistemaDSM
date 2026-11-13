/* dsm.c (AMBAS): API publica, alloc local y barrier. */
#define _GNU_SOURCE
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

int dsm_verbose;

static const dsm_config_t *C;
static pthread_mutex_t alloc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t alloc_next;            /* offset dentro de mi slot */

/* ---- barrier ---- */
#define MAX_BARRIERS 8
typedef struct { int in_use; uint64_t id; uint32_t count; uint32_t n; uint32_t mask; } bar_t;
static bar_t bars[MAX_BARRIERS];
static pthread_mutex_t bar_mu = PTHREAD_MUTEX_INITIALIZER;     /* estado del coordinador */
static pthread_mutex_t bar_call_mu = PTHREAD_MUTEX_INITIALIZER;/* un dsm_barrier a la vez por nodo (P1) */
static sem_t bar_release;

static void on_barrier_arrive(const msg_header_t *h, const void *unused) {   /* POOL, coordinador */
    (void)unused;
    uint32_t mask = 0; int fire = 0;
    pthread_mutex_lock(&bar_mu);
    bar_t *b = NULL;
    for (int i = 0; i < MAX_BARRIERS; i++) if (bars[i].in_use && bars[i].id == h->page_id) { b = &bars[i]; break; }
    if (!b) for (int i = 0; i < MAX_BARRIERS; i++) if (!bars[i].in_use) {
        b = &bars[i]; memset(b, 0, sizeof *b); b->in_use = 1; b->id = h->page_id; b->n = h->req_id; break;
    }
    if (b) {
        b->count++; b->mask |= 1u << h->src_node;
        if (b->count >= b->n) { mask = b->mask; fire = 1; b->in_use = 0; }   /* limpiar antes de liberar */
    }
    pthread_mutex_unlock(&bar_mu);
    if (!fire) return;
    for (node_id_t n = 0; n < C->max_nodes; n++)
        if (mask & (1u << n)) {
            msg_header_t r = { .type = MSG_BARRIER_RELEASE, .page_id = h->page_id };
            net_send(n, &r, NULL);
        }
}

static void on_barrier_release(const msg_header_t *h, const void *unused) {  /* INLINE: solo sem_post */
    (void)h; (void)unused;
    sem_post(&bar_release);
}

dsm_err_t dsm_barrier(uint32_t id, uint32_t n) {
    if (n == 0 || n > C->max_nodes) return DSM_ERR_BAD_ARG;
    pthread_mutex_lock(&bar_call_mu);
    msg_header_t a = { .type = MSG_BARRIER_ARRIVE, .page_id = id, .req_id = n };
    dsm_err_t rc = net_send(cluster_barrier_coord(id), &a, NULL);
    if (rc == DSM_OK) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += C->barrier_timeout_ms / 1000;
        while (sem_timedwait(&bar_release, &ts) != 0) {
            if (errno == EINTR) continue;
            rc = DSM_ERR_TIMEOUT; break;
        }
    }
    pthread_mutex_unlock(&bar_call_mu);
    return rc;
}

/* ---- API ---- */

dsm_err_t dsm_init(const char *config_path) {
    static dsm_config_t cfg;
    dsm_verbose = getenv("DSM_VERBOSE") != NULL;
    dsm_err_t rc;
    if ((rc = config_load(config_path, &cfg)) != DSM_OK) return rc;
    C = config_get();
    cluster_init(C);
    if ((rc = net_init(C)) != DSM_OK) return rc;
    if ((rc = pager_init(C)) != DSM_OK) return rc;      /* registra handlers del pager */
    if ((rc = dir_init(C)) != DSM_OK) return rc;        /* registra handlers del directorio */
    sem_init(&bar_release, 0, 0);
    net_register(MSG_BARRIER_ARRIVE,  on_barrier_arrive,  MSG_POOL);
    net_register(MSG_BARRIER_RELEASE, on_barrier_release, MSG_INLINE);
    return net_start();
}

void *dsm_slot_base(node_id_t node) {
    if (node >= C->max_nodes) return NULL;
    return pager_app_addr(((uint64_t)node * C->mem_per_node) / C->page_size);
}

/* P1: alloc local con bump pointer, alineado a pagina (mitiga false sharing). Sin alloc remoto. */
void *dsm_alloc(size_t size) {
    if (size == 0) return NULL;
    size_t ps = C->page_size;
    size = (size + ps - 1) / ps * ps;
    pthread_mutex_lock(&alloc_mu);
    void *p = NULL;
    if (alloc_next + size <= C->mem_per_node) {
        p = (char *)dsm_slot_base(cluster_self_id()) + alloc_next;
        alloc_next += size;
    }
    pthread_mutex_unlock(&alloc_mu);
    return p;              /* NULL = DSM_ERR_NOMEM */
}

dsm_err_t dsm_free(void *ptr) { (void)ptr; return DSM_OK; }   /* P1: no-op */

dsm_err_t dsm_shutdown(void) {
    if (dsm_verbose) {
        pager_stats_t s; pager_get_stats(&s);
        fprintf(stderr, "[dsm n%u] fallos R=%llu W=%llu | paginas recibidas=%llu | invalidaciones=%llu | fetch atendidos=%llu\n",
                cluster_self_id(), (unsigned long long)s.faults_read, (unsigned long long)s.faults_write,
                (unsigned long long)s.pages_received, (unsigned long long)s.invalidations,
                (unsigned long long)s.fetches_served);
    }
    pager_shutdown();
    net_shutdown();
    dir_shutdown();
    return DSM_OK;
}
