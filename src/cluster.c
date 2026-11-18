/* cluster.c (B): membresia, JOIN/NODE_LIST, heartbeat UDP, hash de home.
 * Una sola fuente de verdad: la configuracion es inmutable (puntero a config.c);
 * el estado mutable (alive, ultimo latido) vive aqui en atomicos. */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <poll.h>
#include <netdb.h>
#include <endian.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>

static const dsm_config_t *C;
static struct sockaddr_in addr[DSM_MAX_NODES];
static atomic_uchar alive[DSM_MAX_NODES];
static _Atomic uint64_t last_hb_ms[DSM_MAX_NODES];
static _Atomic uint32_t tasks_run[DSM_MAX_NODES], load100_[DSM_MAX_NODES];   /* [Extra 1] carga reportada */
static int hb_sock = -1;
static pthread_t hb_th;
static volatile int hb_run;

uint64_t cluster_splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

dsm_err_t cluster_init(const dsm_config_t *cfg) {
    C = cfg;
    for (uint32_t i = 0; i < C->max_nodes; i++) {         /* IP o nombre de host (getaddrinfo) */
        struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char ps[8]; snprintf(ps, sizeof ps, "%u", C->slots[i].port);
        int rc = getaddrinfo(C->slots[i].ip, ps, &hints, &res);
        if (rc != 0 || !res) { fprintf(stderr, "cluster: no se pudo resolver '%s' (%s)\n", C->slots[i].ip, gai_strerror(rc)); return DSM_ERR_BAD_ARG; }
        memcpy(&addr[i], res->ai_addr, sizeof addr[i]);
        freeaddrinfo(res);
        atomic_store(&alive[i], 0);
    }
    atomic_store(&alive[C->self_id], 1);
    atomic_store(&last_hb_ms[C->self_id], dsm_now_ms());
    return DSM_OK;
}

node_id_t cluster_self_id(void) { return C ? C->self_id : 0; }
bool cluster_is_alive(node_id_t id) { return id < C->max_nodes && atomic_load(&alive[id]); }
bool cluster_get_addr(node_id_t id, struct sockaddr_in *out) { if (id >= C->max_nodes) return false; *out = addr[id]; return true; }

bool cluster_get_node(node_id_t id, node_info_t *out) {
    if (id >= C->max_nodes) return false;
    *out = C->slots[id];
    out->alive = atomic_load(&alive[id]);
    out->last_heartbeat = time(NULL) - (time_t)((dsm_now_ms() - atomic_load(&last_hb_ms[id])) / 1000);
    out->tasks_running = cluster_tasks_running(id); out->load100 = cluster_load100(id);
    return true;
}

void cluster_mark_alive(node_id_t id) {
    if (id >= C->max_nodes || id == C->self_id) return;
    atomic_store(&last_hb_ms[id], dsm_now_ms());
    if (!atomic_exchange(&alive[id], 1)) DSM_LOG("nodo %u ahora VIVO", id);
}
void cluster_mark_dead(node_id_t id) {
    if (id >= C->max_nodes || id == C->self_id) return;
    if (atomic_exchange(&alive[id], 0)) DSM_LOG("nodo %u marcado CAIDO", id);
}
uint32_t cluster_alive_count(void) {
    uint32_t n = 0; for (uint32_t i = 0; i < C->max_nodes; i++) n += atomic_load(&alive[i]); return n;
}

node_id_t cluster_home_of(page_id_t page) { return (node_id_t)(cluster_splitmix64(page ^ C->hash_seed) % C->max_nodes); }
node_id_t cluster_initial_owner(page_id_t page) { return (node_id_t)((page * C->page_size) / C->mem_per_node); }
node_id_t cluster_barrier_coord(uint64_t id) { return (node_id_t)(cluster_splitmix64(id ^ C->hash_seed) % C->max_nodes); }
node_id_t cluster_lock_coord(uint64_t id) { return (node_id_t)(cluster_splitmix64(id ^ C->hash_seed ^ 0x10c4ULL) % C->max_nodes); }
node_id_t cluster_alloc_coord(void) { return 0; }

/* ---- carga (Extra 1): cada nodo la anuncia en sus latidos (req_id = tareas, page_id = loadavg x 100) ---- */
void cluster_set_local_tasks(uint32_t load) { atomic_store(&tasks_run[C->self_id], load); }
uint32_t cluster_tasks_running(node_id_t id) { return id < C->max_nodes ? atomic_load(&tasks_run[id]) : 0; }
static uint32_t local_load100(void) { double la[1]; return getloadavg(la, 1) == 1 ? (uint32_t)(la[0] * 100.0) : 0; }
uint32_t cluster_load100(node_id_t id) { return id == C->self_id ? local_load100() : (id < C->max_nodes ? atomic_load(&load100_[id]) : 0); }

/* ---- JOIN / NODE_LIST ---- */
static uint32_t alive_mask(void) { uint32_t m = 0; for (uint32_t i = 0; i < C->max_nodes; i++) if (atomic_load(&alive[i])) m |= 1u << i; return m; }

static void on_join(const msg_header_t *h, const void *unused) {        /* POOL (sin espera) */
    (void)unused;
    cluster_mark_alive(h->src_node);
    uint32_t m = htonl(alive_mask());
    msg_header_t r = { .type = MSG_NODE_LIST, .payload_len = sizeof m };
    net_send(h->src_node, &r, &m);
}
static void on_node_list(const msg_header_t *h, const void *payload) {  /* INLINE */
    cluster_mark_alive(h->src_node);
    if (h->payload_len != sizeof(uint32_t)) return;
    uint32_t m; memcpy(&m, payload, sizeof m); m = ntohl(m);
    for (uint32_t i = 0; i < C->max_nodes; i++) if (m & (1u << i)) cluster_mark_alive(i);
}
void cluster_register(void) {
    net_register(MSG_JOIN, on_join, MSG_POOL);
    net_register(MSG_NODE_LIST, on_node_list, MSG_INLINE);
}

dsm_err_t cluster_join(void) {
    uint64_t t0 = dsm_now_ms();
    for (;;) {
        int missing = 0;
        for (node_id_t n = 0; n < C->max_nodes; n++) {
            if (n == C->self_id || atomic_load(&alive[n])) continue;
            missing++;
            msg_header_t j = { .type = MSG_JOIN };
            net_send(n, &j, NULL);              /* JOIN esta exento del "fallo rapido" */
        }
        if (!missing) return DSM_OK;
        if (!C->wait_all_nodes) return DSM_OK;
        if (dsm_now_ms() - t0 > C->join_timeout_ms) return DSM_ERR_TIMEOUT;
        usleep(200 * 1000);
        /* los latidos o el NODE_LIST de respuesta pueden haber marcado nodos vivos */
    }
}

/* ---- heartbeat UDP (puerto propio, no compite con el trafico de paginas) ---- */
static void *hb_main(void *arg) {
    (void)arg;
    uint64_t next_send = 0;
    while (hb_run) {
        uint64_t now = dsm_now_ms();
        if (now >= next_send) {
            next_send = now + C->heartbeat_interval_ms;
            msg_header_t h = { .type = MSG_HEARTBEAT, .src_node = htonl(C->self_id),
                               .req_id = htonl(cluster_tasks_running(C->self_id)), .page_id = htobe64(local_load100()) };
            for (node_id_t n = 0; n < C->max_nodes; n++) {
                if (n == C->self_id) continue;
                struct sockaddr_in a = addr[n];
                a.sin_port = htons((uint16_t)(C->slots[n].port + C->heartbeat_port_offset));
                sendto(hb_sock, &h, sizeof h, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
            }
        }
        for (node_id_t n = 0; n < C->max_nodes; n++)
            if (n != C->self_id && atomic_load(&alive[n]) && now - atomic_load(&last_hb_ms[n]) > C->heartbeat_timeout_ms)
                cluster_mark_dead(n);
        struct pollfd p = { .fd = hb_sock, .events = POLLIN };
        if (poll(&p, 1, 100) > 0) {
            msg_header_t h;
            while (recv(hb_sock, &h, sizeof h, MSG_DONTWAIT) == (ssize_t)sizeof h) {
                if (h.type == MSG_HEARTBEAT) {
                    node_id_t src = ntohl(h.src_node);
                    if (src < C->max_nodes && src != C->self_id) {
                        atomic_store(&tasks_run[src], ntohl(h.req_id)); atomic_store(&load100_[src], (uint32_t)be64toh(h.page_id));
                    }
                    cluster_mark_alive(src);
                }
            }
        }
    }
    return NULL;
}

dsm_err_t cluster_start_heartbeat(void) {
    hb_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (hb_sock < 0) return DSM_ERR_IO;
    int one = 1; setsockopt(hb_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)(C->slots[C->self_id].port + C->heartbeat_port_offset)) };
    if (bind(hb_sock, (struct sockaddr *)&a, sizeof a) < 0) { perror("heartbeat bind"); close(hb_sock); hb_sock = -1; return DSM_ERR_IO; }
    hb_run = 1;
    pthread_create(&hb_th, NULL, hb_main, NULL);
    return DSM_OK;
}

void cluster_shutdown(void) {
    if (!hb_run) return;
    hb_run = 0; pthread_join(hb_th, NULL);
    close(hb_sock); hb_sock = -1;
}