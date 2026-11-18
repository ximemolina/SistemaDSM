/*
 * directory.c (AMBAS): directorio en el home (por hash) de cada pagina.
 * Una transaccion por pagina a la vez (mutex por franja). PAGE_REQUEST corre en el pool CON
 * espera; sus respuestas (FETCH_REPLY, ACK) las procesa el listener, y FETCH/INVALIDATE el
 * pool SIN espera: el agotamiento del pool con espera no puede producir deadlock.
 */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define NSTRIPES 256
#define BIT(n) (1ULL << (n))

typedef struct { uint32_t owner_node; uint64_t copyset; uint8_t owner_dirty; uint8_t init; } dir_entry_t;
typedef struct {
    int in_use; page_id_t page; uint32_t gen;   /* generacion: descarta respuestas de transacciones viejas */
    atomic_int acks_left; sem_t sem; char *buf;
} txn_t;

static const dsm_config_t *C;
static size_t psz;
static uint64_t npages;
static dir_entry_t *dir;
static pthread_mutex_t stripe[NSTRIPES];
static txn_t *txns; static unsigned ntxn;
static pthread_mutex_t txn_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint txn_gen = 1;

static txn_t *txn_alloc(page_id_t p) {
    txn_t *t = NULL;
    pthread_mutex_lock(&txn_mu);
    for (unsigned i = 0; i < ntxn; i++) if (!txns[i].in_use) {
        t = &txns[i]; t->in_use = 1; t->page = p;
        t->gen = atomic_fetch_add(&txn_gen, 1); if (!t->gen) t->gen = atomic_fetch_add(&txn_gen, 1);
        atomic_store(&t->acks_left, 0);
        while (sem_trywait(&t->sem) == 0) {}      /* drena posts atrasados de una transaccion vencida */
        break;
    }
    pthread_mutex_unlock(&txn_mu);
    return t;
}
static void txn_free(txn_t *t) { pthread_mutex_lock(&txn_mu); t->in_use = 0; pthread_mutex_unlock(&txn_mu); }
static txn_t *txn_find(page_id_t p, uint32_t gen) {
    txn_t *t = NULL;
    pthread_mutex_lock(&txn_mu);
    for (unsigned i = 0; i < ntxn; i++) if (txns[i].in_use && txns[i].page == p && txns[i].gen == gen) { t = &txns[i]; break; }
    pthread_mutex_unlock(&txn_mu);
    return t;
}
static int txn_wait(txn_t *t) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += C->request_timeout_ms / 1000; ts.tv_nsec += (long)(C->request_timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (sem_timedwait(&t->sem, &ts) != 0) { if (errno == EINTR) continue; return -1; }
    return 0;
}
static int do_fetch(txn_t *t, node_id_t src, page_id_t p, access_t acc) {
    msg_header_t h = { .type = MSG_FETCH, .page_id = p, .access = (uint8_t)acc, .req_id = t->gen };
    if (net_send(src, &h, NULL) != DSM_OK) return -1;
    return txn_wait(t);
}
static void send_page_data(node_id_t dst, page_id_t p, access_t g, uint32_t req_id, const void *buf) {
    msg_header_t h = { .type = MSG_PAGE_DATA, .page_id = p, .access = (uint8_t)g, .req_id = req_id, .payload_len = (uint32_t)psz };
    net_send(dst, &h, buf);
}
static void entry_init(dir_entry_t *e, page_id_t p) {
    if (!e->init) { e->init = 1; e->owner_node = cluster_initial_owner(p); e->owner_dirty = 0; e->copyset = 0; }
}

void dir_on_page_request(const msg_header_t *h, const void *unused) {
    (void)unused;
    node_id_t req = h->src_node; page_id_t p = h->page_id; access_t acc = (access_t)h->access;
    if (p >= npages || req >= C->max_nodes) return;
    pthread_mutex_t *mu = &stripe[p % NSTRIPES];
    pthread_mutex_lock(mu);
    dir_entry_t *e = &dir[p];
    node_id_t init = cluster_initial_owner(p);
    entry_init(e, p);
    txn_t *t = txn_alloc(p);
    if (!t) { DSM_LOG("sin espacio para transacciones"); pthread_mutex_unlock(mu); return; }
    node_id_t src = e->owner_dirty ? e->owner_node : init;       /* fuente: owner en M, o copia de referencia */

    if (acc == ACC_READ) {
        if (do_fetch(t, src, p, ACC_READ) != 0) { DSM_LOG("FETCH pag %llu fallo/timeout (nodo %u)", (unsigned long long)p, src); goto out; }
        if (e->owner_dirty) {                                      /* M -> S + write-back perezoso */
            e->copyset |= BIT(e->owner_node);
            if (e->owner_node != init) {
                msg_header_t w = { .type = MSG_PAGE_WRITEBACK, .page_id = p, .payload_len = (uint32_t)psz };
                net_send(init, &w, t->buf);
            }
            e->owner_dirty = 0;
        }
        e->copyset |= BIT(req);
        send_page_data(req, p, ACC_READ, h->req_id, t->buf);
    } else {
        if (do_fetch(t, src, p, ACC_WRITE) != 0) { DSM_LOG("FETCH pag %llu fallo/timeout (nodo %u)", (unsigned long long)p, src); goto out; }
        uint64_t inv = e->copyset & ~BIT(req) & ~BIT(src);
        if (inv) {
            atomic_store(&t->acks_left, __builtin_popcountll(inv));
            for (node_id_t n = 0; n < C->max_nodes; n++)
                if (inv & BIT(n)) { msg_header_t i = { .type = MSG_INVALIDATE, .page_id = p, .req_id = t->gen }; net_send(n, &i, NULL); }
            if (txn_wait(t) != 0) { DSM_LOG("ACKs de pag %llu incompletos", (unsigned long long)p); goto out; }
        }
        e->owner_node = req; e->owner_dirty = 1; e->copyset = 0;
        send_page_data(req, p, ACC_WRITE, h->req_id, t->buf);
    }
out:
    txn_free(t);
    pthread_mutex_unlock(mu);
}

void dir_on_fetch_reply(const msg_header_t *h, const void *bytes) {      /* INLINE */
    txn_t *t = txn_find(h->page_id, h->req_id);
    if (!t || h->payload_len != psz) return;                             /* respuesta de transaccion vieja */
    memcpy(t->buf, bytes, psz);
    sem_post(&t->sem);
}
void dir_on_invalidate_ack(const msg_header_t *h, const void *unused) {  /* INLINE */
    (void)unused;
    txn_t *t = txn_find(h->page_id, h->req_id);
    if (!t) return;
    if (atomic_fetch_sub(&t->acks_left, 1) == 1) sem_post(&t->sem);
}
void dir_on_writeback(const msg_header_t *h, const void *bytes) {        /* INLINE: orden FIFO antes de cualquier FETCH posterior */
    if (h->page_id >= npages || h->payload_len != psz) return;
    memcpy(pager_sys_addr(h->page_id), bytes, psz);
}

/* Un nodo que sale devuelve sus paginas (MSG_POOL_WAIT: toma el mutex de la pagina) */
void dir_on_release(const msg_header_t *h, const void *bytes) {
    node_id_t src = h->src_node; page_id_t p = h->page_id;
    if (p >= npages || src >= C->max_nodes) return;
    node_id_t init = cluster_initial_owner(p);
    pthread_mutex_lock(&stripe[p % NSTRIPES]);
    dir_entry_t *e = &dir[p];
    entry_init(e, p);
    if (h->payload_len == psz && e->owner_dirty && e->owner_node == src && init != src) {
        msg_header_t w = { .type = MSG_PAGE_WRITEBACK, .page_id = p, .payload_len = (uint32_t)psz };
        net_send(init, &w, bytes);                  /* la copia de referencia recupera el dato */
        e->owner_dirty = 0;
    }
    e->copyset &= ~BIT(src);
    pthread_mutex_unlock(&stripe[p % NSTRIPES]);
}

uint32_t dir_snapshot(page_id_t first, dir_snap_t *out, uint32_t max, page_id_t *next) {
    uint32_t n = 0; page_id_t p = first;
    for (; p < npages && n < max; p++)
        if (dir[p].init) { out[n++] = (dir_snap_t){ p, dir[p].owner_node, dir[p].copyset, dir[p].owner_dirty }; }
    while (p < npages && !dir[p].init) p++;
    *next = (p < npages) ? p : UINT64_MAX;
    return n;
}
uint64_t dir_count(void) { uint64_t n = 0; for (page_id_t p = 0; p < npages; p++) n += dir[p].init; return n; }

dsm_err_t dir_init(const dsm_config_t *cfg) {
    C = cfg; psz = cfg->page_size;
    npages = (uint64_t)cfg->max_nodes * cfg->mem_per_node / psz;
    dir = calloc(npages, sizeof *dir);
    ntxn = cfg->worker_pool_size * 2 + 8;
    txns = calloc(ntxn, sizeof *txns);
    if (!dir || !txns) return DSM_ERR_NOMEM;
    for (unsigned i = 0; i < ntxn; i++) { sem_init(&txns[i].sem, 0, 0); txns[i].buf = malloc(psz); if (!txns[i].buf) return DSM_ERR_NOMEM; }
    for (int i = 0; i < NSTRIPES; i++) pthread_mutex_init(&stripe[i], NULL);
    net_register(MSG_PAGE_REQUEST,   dir_on_page_request,   MSG_POOL_WAIT);
    net_register(MSG_PAGE_RELEASE,   dir_on_release,        MSG_POOL_WAIT);
    net_register(MSG_FETCH_REPLY,    dir_on_fetch_reply,    MSG_INLINE);
    net_register(MSG_INVALIDATE_ACK, dir_on_invalidate_ack, MSG_INLINE);
    net_register(MSG_PAGE_WRITEBACK, dir_on_writeback,      MSG_INLINE);
    return DSM_OK;
}
void dir_shutdown(void) {}