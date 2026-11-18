/* network.c (B): listener, DOS pools (con espera / sin espera), framing, RPC, timeouts. */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <endian.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_CONNS 64
#define RPC_SLOTS 64

typedef struct { msg_handler_t fn; msg_class_t cls; } reg_t;
typedef struct job { struct job *next; msg_header_t h; void *payload; msg_handler_t fn; } job_t;
typedef struct { job_t *head, *tail; pthread_mutex_t mu; pthread_cond_t cv; pthread_t *th; unsigned n; } pool_t;
typedef struct { int used; uint32_t req_id; sem_t sem; char *buf; uint32_t len, cap; } rpc_slot_t;

static const dsm_config_t *cfg;
static reg_t reg[MSG_TYPE_MAX];
static int listen_fd = -1;
static pthread_t listener_th;
static pool_t pool_nw = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
static pool_t pool_w  = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };
static volatile int running;
static int started;
static int out_fd[DSM_MAX_NODES];
static pthread_mutex_t out_mu[DSM_MAX_NODES];
static atomic_uint req_counter = 1;
static atomic_ullong c_tx, c_rx, c_txb;
static rpc_slot_t rpcs[RPC_SLOTS];
static pthread_mutex_t rpc_mu = PTHREAD_MUTEX_INITIALIZER;
static char *rbuf;

static int read_exact(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }   /* incluye timeout SO_RCVTIMEO */
        p += n; len -= (size_t)n;
    }
    return 0;
}
static int send_all(int fd, const void *buf, size_t len, int more) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL | (more ? MSG_MORE : 0));
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}
static void hdr_to_net(msg_header_t *o, const msg_header_t *h) {
    o->type = h->type; o->src_node = htonl(h->src_node); o->page_id = htobe64(h->page_id);
    o->req_id = htonl(h->req_id); o->access = h->access; o->payload_len = htonl(h->payload_len);
}
static void hdr_from_net(msg_header_t *h) {
    h->src_node = ntohl(h->src_node); h->page_id = be64toh(h->page_id);
    h->req_id = ntohl(h->req_id); h->payload_len = ntohl(h->payload_len);
}

/* ---------- despacho ---------- */
static void enqueue(pool_t *pl, const msg_header_t *h, const void *payload, msg_handler_t fn) {
    job_t *j = malloc(sizeof *j);
    if (!j) return;
    j->next = NULL; j->h = *h; j->fn = fn; j->payload = NULL;
    if (h->payload_len) {
        j->payload = malloc(h->payload_len);
        if (!j->payload) { free(j); return; }
        memcpy(j->payload, payload, h->payload_len);
    }
    pthread_mutex_lock(&pl->mu);
    if (pl->tail) pl->tail->next = j; else pl->head = j;
    pl->tail = j;
    pthread_cond_signal(&pl->cv);
    pthread_mutex_unlock(&pl->mu);
}
static void dispatch(const msg_header_t *h, const void *payload) {
    if (h->type == 0 || h->type >= MSG_TYPE_MAX || !reg[h->type].fn) { DSM_LOG("mensaje tipo %u sin handler", h->type); return; }
    switch (reg[h->type].cls) {
        case MSG_INLINE:    reg[h->type].fn(h, payload); break;
        case MSG_POOL:      enqueue(&pool_nw, h, payload, reg[h->type].fn); break;
        case MSG_POOL_WAIT: enqueue(&pool_w,  h, payload, reg[h->type].fn); break;
    }
}
void net_register(msg_type_t type, msg_handler_t fn, msg_class_t cls) {
    if (type > 0 && type < MSG_TYPE_MAX) { reg[type].fn = fn; reg[type].cls = cls; }
}

/* ---------- hilos ---------- */
static void *worker_main(void *arg) {
    pool_t *pl = arg;
    for (;;) {
        pthread_mutex_lock(&pl->mu);
        while (!pl->head && running) pthread_cond_wait(&pl->cv, &pl->mu);
        if (!pl->head) { pthread_mutex_unlock(&pl->mu); break; }
        job_t *j = pl->head; pl->head = j->next; if (!pl->head) pl->tail = NULL;
        pthread_mutex_unlock(&pl->mu);
        j->fn(&j->h, j->payload);
        free(j->payload); free(j);
    }
    return NULL;
}

static int recv_one(int fd) {
    msg_header_t h;
    if (read_exact(fd, &h, sizeof h) < 0) return -1;
    hdr_from_net(&h);
    if (h.type == 0 || h.type >= MSG_TYPE_MAX || h.payload_len > cfg->max_payload) {
        DSM_LOG("DSM_ERR_PROTO: tipo=%u payload_len=%u (conexion cerrada)", h.type, h.payload_len);
        return -1;
    }
    if (h.payload_len && read_exact(fd, rbuf, h.payload_len) < 0) return -1;
    atomic_fetch_add(&c_rx, 1);
    dispatch(&h, h.payload_len ? rbuf : NULL);
    return 0;
}

static void *listener_main(void *arg) {
    (void)arg;
    int fds[MAX_CONNS]; int n = 0;
    while (running) {
        struct pollfd p[MAX_CONNS + 1];
        p[0].fd = listen_fd; p[0].events = POLLIN; p[0].revents = 0;
        for (int i = 0; i < n; i++) { p[i+1].fd = fds[i]; p[i+1].events = POLLIN; p[i+1].revents = 0; }
        int r = poll(p, (nfds_t)(n + 1), 200);
        if (r <= 0) continue;
        if (p[0].revents & POLLIN) {
            int c = accept(listen_fd, NULL, NULL);
            if (c >= 0) {
                int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                struct timeval tv = { 3, 0 };   /* un par que deja un mensaje a medias no congela al listener */
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                if (n < MAX_CONNS) fds[n++] = c; else close(c);
            }
        }
        for (int i = n - 1; i >= 0; i--)
            if (p[i+1].revents & (POLLIN | POLLHUP | POLLERR))
                if (recv_one(fds[i]) < 0) { close(fds[i]); fds[i] = fds[--n]; }
    }
    for (int i = 0; i < n; i++) close(fds[i]);
    return NULL;
}

/* ---------- RPC ---------- */
static void rpc_on_reply(const msg_header_t *h, const void *payload) {   /* INLINE */
    pthread_mutex_lock(&rpc_mu);
    for (int i = 0; i < RPC_SLOTS; i++)
        if (rpcs[i].used && rpcs[i].req_id == h->req_id) {
            rpcs[i].len = h->payload_len < rpcs[i].cap ? h->payload_len : rpcs[i].cap;
            if (rpcs[i].len) memcpy(rpcs[i].buf, payload, rpcs[i].len);
            sem_post(&rpcs[i].sem);
            break;
        }
    pthread_mutex_unlock(&rpc_mu);
}

dsm_err_t net_rpc_call(node_id_t dst, msg_type_t type, uint8_t access, uint64_t page_id,
                       const void *payload, uint32_t plen, void *reply, uint32_t *rlen, uint32_t timeout_ms) {
    rpc_slot_t *s = NULL;
    uint32_t rid = atomic_fetch_add(&req_counter, 1); if (!rid) rid = atomic_fetch_add(&req_counter, 1);
    pthread_mutex_lock(&rpc_mu);
    for (int i = 0; i < RPC_SLOTS; i++) if (!rpcs[i].used) { s = &rpcs[i]; s->used = 1; s->req_id = rid; s->len = 0; break; }
    pthread_mutex_unlock(&rpc_mu);
    if (!s) return DSM_ERR_NOMEM;
    while (sem_trywait(&s->sem) == 0) {}
    s->cap = rlen ? *rlen : 0;
    if (s->cap > cfg->max_payload) s->cap = cfg->max_payload;
    msg_header_t h = { .type = (uint8_t)type, .page_id = page_id, .req_id = rid, .access = access, .payload_len = plen };
    dsm_err_t rc = net_send(dst, &h, plen ? payload : NULL);
    if (rc == DSM_OK) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000; ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int r; do { r = sem_timedwait(&s->sem, &ts); } while (r != 0 && errno == EINTR);
        if (r != 0) rc = DSM_ERR_TIMEOUT;
        else { if (reply && s->len) memcpy(reply, s->buf, s->len); if (rlen) *rlen = s->len; }
    }
    pthread_mutex_lock(&rpc_mu); s->used = 0; pthread_mutex_unlock(&rpc_mu);
    return rc;
}

/* ---------- API ---------- */
dsm_err_t net_init(const dsm_config_t *c) {
    cfg = c;
    for (int i = 0; i < DSM_MAX_NODES; i++) { out_fd[i] = -1; pthread_mutex_init(&out_mu[i], NULL); }
    rbuf = malloc(c->max_payload);
    for (int i = 0; i < RPC_SLOTS; i++) { sem_init(&rpcs[i].sem, 0, 0); rpcs[i].buf = malloc(c->max_payload); }
    net_register(MSG_ALLOC_REPLY, rpc_on_reply, MSG_INLINE);
    net_register(MSG_STATS_REPLY, rpc_on_reply, MSG_INLINE);
    return rbuf ? DSM_OK : DSM_ERR_NOMEM;
}

dsm_err_t net_start(void) {
    node_info_t me; cluster_get_node(cluster_self_id(), &me);
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return DSM_ERR_IO;
    int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(me.port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 64) < 0) {
        perror("net_start bind/listen"); close(listen_fd); return DSM_ERR_IO;
    }
    running = 1; started = 1;
    pool_nw.n = cfg->nowait_pool_size; pool_w.n = cfg->worker_pool_size;
    pool_nw.th = calloc(pool_nw.n, sizeof(pthread_t)); pool_w.th = calloc(pool_w.n, sizeof(pthread_t));
    for (unsigned i = 0; i < pool_nw.n; i++) pthread_create(&pool_nw.th[i], NULL, worker_main, &pool_nw);
    for (unsigned i = 0; i < pool_w.n; i++)  pthread_create(&pool_w.th[i],  NULL, worker_main, &pool_w);
    pthread_create(&listener_th, NULL, listener_main, NULL);
    return DSM_OK;
}

void net_shutdown(void) {
    if (!started) return;
    running = 0;
    pool_t *pls[2] = { &pool_nw, &pool_w };
    for (int k = 0; k < 2; k++) { pthread_mutex_lock(&pls[k]->mu); pthread_cond_broadcast(&pls[k]->cv); pthread_mutex_unlock(&pls[k]->mu); }
    pthread_join(listener_th, NULL);
    for (int k = 0; k < 2; k++) { for (unsigned i = 0; i < pls[k]->n; i++) pthread_join(pls[k]->th[i], NULL); free(pls[k]->th); }
    close(listen_fd);
    for (int i = 0; i < DSM_MAX_NODES; i++) if (out_fd[i] >= 0) { close(out_fd[i]); out_fd[i] = -1; }
    started = 0;
}

/* connect() no bloqueante con timeout corto (no congela a quien envia si el nodo cayo) */
static int connect_peer(node_id_t dst) {
    struct sockaddr_in a;
    if (!cluster_get_addr(dst, &a)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        if (poll(&p, 1, (int)cfg->connect_timeout_ms) == 1) {
            int err = 0; socklen_t l = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
            r = err ? -1 : 0;
        } else r = -1;
    }
    if (r < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { cfg->request_timeout_ms / 1000, (cfg->request_timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   /* send() no bloquea para siempre */
    return fd;
}

dsm_err_t net_send(node_id_t dst, msg_header_t *h, const void *payload) {
    if (dst >= cfg->max_nodes) return DSM_ERR_BAD_ARG;
    if ((payload == NULL) != (h->payload_len == 0) || h->payload_len > cfg->max_payload) return DSM_ERR_BAD_ARG;
    h->src_node = cluster_self_id();
    if (h->req_id == 0) { uint32_t r = atomic_fetch_add(&req_counter, 1); h->req_id = r ? r : 1; }
    if (dst == cluster_self_id()) { dispatch(h, payload); return DSM_OK; }
    int exempt = (h->type == MSG_JOIN || h->type == MSG_NODE_LIST || h->type == MSG_HEARTBEAT);
    if (!exempt && !cluster_is_alive(dst)) return DSM_ERR_NODE_DOWN;       /* fallo rapido */

    msg_header_t nh; hdr_to_net(&nh, h);
    dsm_err_t rc = DSM_ERR_IO;
    pthread_mutex_lock(&out_mu[dst]);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (out_fd[dst] < 0) {
            out_fd[dst] = connect_peer(dst);
            if (out_fd[dst] < 0) { rc = DSM_ERR_NODE_DOWN; break; }
        }
        if (send_all(out_fd[dst], &nh, sizeof nh, h->payload_len != 0) == 0 &&
            (h->payload_len == 0 || send_all(out_fd[dst], payload, h->payload_len, 0) == 0)) { rc = DSM_OK; break; }
        close(out_fd[dst]); out_fd[dst] = -1;
    }
    pthread_mutex_unlock(&out_mu[dst]);
    if (rc == DSM_OK) { atomic_fetch_add(&c_tx, 1); atomic_fetch_add(&c_txb, sizeof nh + h->payload_len); }
    return rc;
}

dsm_err_t net_send_page_request(page_id_t page, access_t acc, uint32_t *req_id_out) {
    msg_header_t h = { .type = MSG_PAGE_REQUEST, .page_id = page, .access = (uint8_t)acc };
    dsm_err_t rc = net_send(cluster_home_of(page), &h, NULL);
    if (req_id_out) *req_id_out = h.req_id;
    return rc;
}

void net_get_counters(uint64_t *tx, uint64_t *rx, uint64_t *txb) { *tx = c_tx; *rx = c_rx; *txb = c_txb; }