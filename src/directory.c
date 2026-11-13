/*
 * directory.c (AMBAS): directorio en el nodo home (por hash) de cada pagina.
 * Prototipo 1: una transaccion por pagina a la vez mediante un mutex por franja
 * (sin cola explicita; ver docs, cambio C6).
 */
#define _GNU_SOURCE
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

#define NSTRIPES 256

typedef struct {
    uint32_t owner_node;    /* quien tiene la version M (si owner_dirty) */
    uint64_t copyset;       /* bitmap de nodos con copia S */
    uint8_t  owner_dirty;   /* 1 = owner_node esta en M */
    uint8_t  init;          /* entrada inicializada (perezosa) */
} dir_entry_t;

typedef struct {            /* espera de una transaccion en curso */
    int        in_use;
    page_id_t  page;
    atomic_int acks_left;
    sem_t      sem;
    char      *buf;         /* page_size: aqui cae FETCH_REPLY */
} txn_t;

static const dsm_config_t *C;
static size_t psz;
static uint64_t npages;
static dir_entry_t *dir;
static pthread_mutex_t stripe[NSTRIPES];
static txn_t *txns;
static unsigned ntxn;
static pthread_mutex_t txn_mu = PTHREAD_MUTEX_INITIALIZER;

#define BIT(n) (1ULL << (n))

static txn_t *txn_alloc(page_id_t p) {
    txn_t *t = NULL;
    pthread_mutex_lock(&txn_mu);
    for (unsigned i = 0; i < ntxn; i++) if (!txns[i].in_use) { t = &txns[i]; t->in_use = 1; t->page = p; break; }
    pthread_mutex_unlock(&txn_mu);
    if (t) atomic_store(&t->acks_left, 0);
    return t;
}
static void txn_free(txn_t *t) { pthread_mutex_lock(&txn_mu); t->in_use = 0; pthread_mutex_unlock(&txn_mu); }
static txn_t *txn_find(page_id_t p) {
    txn_t *t = NULL;
    pthread_mutex_lock(&txn_mu);
    for (unsigned i = 0; i < ntxn; i++) if (txns[i].in_use && txns[i].page == p) { t = &txns[i]; break; }
    pthread_mutex_unlock(&txn_mu);
    return t;
}

static int txn_wait(txn_t *t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += C->request_timeout_ms / 1000;
    ts.tv_nsec += (long)(C->request_timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (sem_timedwait(&t->sem, &ts) != 0) { if (errno == EINTR) continue; return -1; }
    return 0;
}

/* Pide los datos frescos a 'src' y espera el FETCH_REPLY (lo procesa el listener) */
static int do_fetch(txn_t *t, node_id_t src, page_id_t p, access_t acc) {
    msg_header_t h = { .type = MSG_FETCH, .page_id = p, .access = (uint8_t)acc };
    if (net_send(src, &h, NULL) != DSM_OK) return -1;
    return txn_wait(t);
}

static void send_page_data(node_id_t dst, page_id_t p, access_t granted, uint32_t req_id, const void *buf) {
    msg_header_t h = { .type = MSG_PAGE_DATA, .page_id = p, .access = (uint8_t)granted,
                       .req_id = req_id, .payload_len = (uint32_t)psz };
    net_send(dst, &h, buf);
}

void dir_on_page_request(const msg_header_t *h, const void *unused) {
    (void)unused;
    node_id_t req = h->src_node;
    page_id_t p = h->page_id;
    access_t acc = (access_t)h->access;
    if (p >= npages || req >= C->max_nodes) return;

    pthread_mutex_t *mu = &stripe[p % NSTRIPES];
    pthread_mutex_lock(mu);                       /* una transaccion por pagina a la vez */
    dir_entry_t *e = &dir[p];
    node_id_t init = cluster_initial_owner(p);
    if (!e->init) { e->init = 1; e->owner_node = init; e->owner_dirty = 0; e->copyset = 0; }

    txn_t *t = txn_alloc(p);
    if (!t) { DSM_LOG("sin espacio para transacciones"); pthread_mutex_unlock(mu); return; }

    /* Fuente de datos: el owner si esta en M; si no, la copia de referencia del contribuyente */
    node_id_t src = e->owner_dirty ? e->owner_node : init;

    if (acc == ACC_READ) {
        if (do_fetch(t, src, p, ACC_READ) != 0) { DSM_LOG("FETCH pag %llu fallo/timeout", (unsigned long long)p); goto out; }
        if (e->owner_dirty) {                     /* M -> S: write-back perezoso a la copia de referencia */
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
        if (do_fetch(t, src, p, ACC_WRITE) != 0) { DSM_LOG("FETCH pag %llu fallo/timeout", (unsigned long long)p); goto out; }
        uint64_t inv = e->copyset & ~BIT(req) & ~BIT(src);
        if (inv) {
            atomic_store(&t->acks_left, __builtin_popcountll(inv));   /* antes de enviar */
            for (node_id_t n = 0; n < C->max_nodes; n++)
                if (inv & BIT(n)) {
                    msg_header_t i = { .type = MSG_INVALIDATE, .page_id = p };
                    net_send(n, &i, NULL);
                }
            if (txn_wait(t) != 0) { DSM_LOG("ACKs de pag %llu incompletos (timeout)", (unsigned long long)p); goto out; }
        }
        e->owner_node = req; e->owner_dirty = 1; e->copyset = 0;
        send_page_data(req, p, ACC_WRITE, h->req_id, t->buf);
    }
out:
    txn_free(t);
    pthread_mutex_unlock(mu);
}

void dir_on_fetch_reply(const msg_header_t *h, const void *bytes) {   /* INLINE: nunca bloquea */
    txn_t *t = txn_find(h->page_id);
    if (!t || h->payload_len != psz) return;
    memcpy(t->buf, bytes, psz);
    sem_post(&t->sem);
}

void dir_on_invalidate_ack(const msg_header_t *h, const void *unused) {   /* INLINE */
    (void)unused;
    txn_t *t = txn_find(h->page_id);
    if (!t) return;
    if (atomic_fetch_sub(&t->acks_left, 1) == 1) sem_post(&t->sem);
}

/* INLINE (cambio C5): solo memcpy; asi se aplica en orden antes de cualquier FETCH posterior */
void dir_on_writeback(const msg_header_t *h, const void *bytes) {
    if (h->page_id >= npages || h->payload_len != psz) return;
    memcpy(pager_sys_addr(h->page_id), bytes, psz);
}

dsm_err_t dir_init(const dsm_config_t *cfg) {
    C = cfg; psz = cfg->page_size;
    npages = (uint64_t)cfg->max_nodes * cfg->mem_per_node / psz;
    dir = calloc(npages, sizeof *dir);             /* calloc: paginas del SO se asignan al tocarlas */
    ntxn = cfg->worker_pool_size * 2 + 8;
    txns = calloc(ntxn, sizeof *txns);
    if (!dir || !txns) return DSM_ERR_NOMEM;
    for (unsigned i = 0; i < ntxn; i++) {
        sem_init(&txns[i].sem, 0, 0);
        txns[i].buf = malloc(psz);
        if (!txns[i].buf) return DSM_ERR_NOMEM;
    }
    for (int i = 0; i < NSTRIPES; i++) pthread_mutex_init(&stripe[i], NULL);
    net_register(MSG_PAGE_REQUEST,   dir_on_page_request,   MSG_POOL);
    net_register(MSG_FETCH_REPLY,    dir_on_fetch_reply,    MSG_INLINE);
    net_register(MSG_INVALIDATE_ACK, dir_on_invalidate_ack, MSG_INLINE);
    net_register(MSG_PAGE_WRITEBACK, dir_on_writeback,      MSG_INLINE);
    return DSM_OK;
}

void dir_shutdown(void) {}
