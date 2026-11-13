#define _GNU_SOURCE
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <endian.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_CONNS 64

typedef struct { msg_handler_t fn; msg_class_t cls; } reg_t;
typedef struct job { struct job *next; msg_header_t h; void *payload; msg_handler_t fn; } job_t;

static const dsm_config_t *cfg;
static reg_t reg[MSG_TYPE_MAX];
static int listen_fd = -1;
static pthread_t listener_th;
static pthread_t *workers;
static unsigned nworkers;
static volatile int running;
static int started;

static int out_fd[DSM_MAX_NODES];
static pthread_mutex_t out_mu[DSM_MAX_NODES];
static atomic_uint req_counter = 1;

static job_t *qhead, *qtail;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;

/* ---------- utilidades de E/S ---------- */

static int read_exact(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
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
    o->type = h->type;
    o->src_node = htonl(h->src_node);
    o->page_id = htobe64(h->page_id);
    o->req_id = htonl(h->req_id);
    o->access = h->access;
    o->payload_len = htonl(h->payload_len);
}

static void hdr_from_net(msg_header_t *h) {
    h->src_node = ntohl(h->src_node);
    h->page_id = be64toh(h->page_id);
    h->req_id = ntohl(h->req_id);
    h->payload_len = ntohl(h->payload_len);
}

/* ---------- despacho ---------- */

static void enqueue(const msg_header_t *h, const void *payload, msg_handler_t fn) {
    job_t *j = malloc(sizeof *j);
    if (!j) return;
    j->next = NULL; j->h = *h; j->fn = fn; j->payload = NULL;
    if (h->payload_len) {
        j->payload = malloc(h->payload_len);
        if (!j->payload) { free(j); return; }
        memcpy(j->payload, payload, h->payload_len);
    }
    pthread_mutex_lock(&qmu);
    if (qtail) qtail->next = j; else qhead = j;
    qtail = j;
    pthread_cond_signal(&qcv);
    pthread_mutex_unlock(&qmu);
}

static void dispatch(const msg_header_t *h, const void *payload) {
    if (h->type == 0 || h->type >= MSG_TYPE_MAX || !reg[h->type].fn) {
        DSM_LOG("mensaje tipo %u sin handler (descartado)", h->type);
        return;
    }
    if (reg[h->type].cls == MSG_INLINE) reg[h->type].fn(h, payload);
    else enqueue(h, payload, reg[h->type].fn);
}

void net_register(msg_type_t type, msg_handler_t fn, msg_class_t cls) {
    if (type > 0 && type < MSG_TYPE_MAX) { reg[type].fn = fn; reg[type].cls = cls; }
}

/* ---------- hilos ---------- */

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qmu);
        while (!qhead && running) pthread_cond_wait(&qcv, &qmu);
        if (!qhead) { pthread_mutex_unlock(&qmu); break; }
        job_t *j = qhead; qhead = j->next; if (!qhead) qtail = NULL;
        pthread_mutex_unlock(&qmu);
        j->fn(&j->h, j->payload);
        free(j->payload); free(j);
    }
    return NULL;
}

static char *rbuf;

static int recv_one(int fd) {
    msg_header_t h;
    if (read_exact(fd, &h, sizeof h) < 0) return -1;
    hdr_from_net(&h);
    if (h.type == 0 || h.type >= MSG_TYPE_MAX || h.payload_len > cfg->max_payload) {
        DSM_LOG("DSM_ERR_PROTO: tipo=%u payload_len=%u (conexion cerrada)", h.type, h.payload_len);
        return -1;
    }
    if (h.payload_len && read_exact(fd, rbuf, h.payload_len) < 0) return -1;
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
                if (n < MAX_CONNS) fds[n++] = c; else close(c);
            }
        }
        for (int i = n - 1; i >= 0; i--) {   /* descendente: la remocion por swap es segura */
            if (i + 1 < n + 1 && (p[i+1].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (recv_one(fds[i]) < 0) { close(fds[i]); fds[i] = fds[--n]; }
            }
        }
    }
    for (int i = 0; i < n; i++) close(fds[i]);
    return NULL;
}

/* ---------- API ---------- */

dsm_err_t net_init(const dsm_config_t *c) {
    cfg = c;
    for (int i = 0; i < DSM_MAX_NODES; i++) { out_fd[i] = -1; pthread_mutex_init(&out_mu[i], NULL); }
    rbuf = malloc(c->max_payload);
    return rbuf ? DSM_OK : DSM_ERR_NOMEM;
}

dsm_err_t net_start(void) {
    node_info_t me;
    cluster_get_node(cluster_self_id(), &me);
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return DSM_ERR_IO;
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(me.port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 32) < 0) {
        perror("net_start bind/listen"); close(listen_fd); return DSM_ERR_IO;
    }
    running = 1; started = 1;
    nworkers = cfg->worker_pool_size ? cfg->worker_pool_size : 4;
    workers = calloc(nworkers, sizeof *workers);
    for (unsigned i = 0; i < nworkers; i++) pthread_create(&workers[i], NULL, worker_main, NULL);
    pthread_create(&listener_th, NULL, listener_main, NULL);
    return DSM_OK;
}

void net_shutdown(void) {
    if (!started) return;
    running = 0;
    pthread_mutex_lock(&qmu); pthread_cond_broadcast(&qcv); pthread_mutex_unlock(&qmu);
    pthread_join(listener_th, NULL);
    for (unsigned i = 0; i < nworkers; i++) pthread_join(workers[i], NULL);
    free(workers);
    close(listen_fd);
    for (int i = 0; i < DSM_MAX_NODES; i++) if (out_fd[i] >= 0) { close(out_fd[i]); out_fd[i] = -1; }
    started = 0;
}

static int connect_peer(node_id_t dst) {
    node_info_t ni;
    if (!cluster_get_node(dst, &ni)) return -1;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(ni.port) };
        if (inet_pton(AF_INET, ni.ip, &a.sin_addr) != 1) { close(fd); return -1; }
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            return fd;
        }
        close(fd);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > (long)cfg->request_timeout_ms) return -1;
        struct timespec nap = { 0, 50 * 1000 * 1000 };   /* el otro nodo puede no haber arrancado */
        nanosleep(&nap, NULL);
    }
}

dsm_err_t net_send(node_id_t dst, msg_header_t *h, const void *payload) {
    if (dst >= cfg->max_nodes) return DSM_ERR_BAD_ARG;
    if ((payload == NULL) != (h->payload_len == 0) || h->payload_len > cfg->max_payload) return DSM_ERR_BAD_ARG;
    h->src_node = cluster_self_id();
    if (h->req_id == 0) {
        uint32_t r = atomic_fetch_add(&req_counter, 1);
        h->req_id = r ? r : 1;
    }
    if (dst == cluster_self_id()) { dispatch(h, payload); return DSM_OK; }

    msg_header_t nh; hdr_to_net(&nh, h);
    dsm_err_t rc = DSM_ERR_IO;
    pthread_mutex_lock(&out_mu[dst]);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (out_fd[dst] < 0) {
            out_fd[dst] = connect_peer(dst);
            if (out_fd[dst] < 0) { rc = DSM_ERR_NODE_DOWN; break; }
        }
        if (send_all(out_fd[dst], &nh, sizeof nh, h->payload_len != 0) == 0 &&
            (h->payload_len == 0 || send_all(out_fd[dst], payload, h->payload_len, 0) == 0)) {
            rc = DSM_OK; break;
        }
        close(out_fd[dst]); out_fd[dst] = -1;   /* reconectar una vez */
    }
    pthread_mutex_unlock(&out_mu[dst]);
    return rc;
}

dsm_err_t net_send_page_request(page_id_t page, access_t acc, uint32_t *req_id_out) {
    msg_header_t h = { .type = MSG_PAGE_REQUEST, .page_id = page, .access = (uint8_t)acc };
    dsm_err_t rc = net_send(cluster_home_of(page), &h, NULL);
    if (req_id_out) *req_id_out = h.req_id;
    return rc;
}
