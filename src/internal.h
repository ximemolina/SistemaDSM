/*
 * internal.h : contrato entre los modulos del DSM (Prototipo 1)
 *
 * Duenos:
 *   pager.c      -> Persona A  (memoria local, handler SIGSEGV, MSI local)
 *   network.c    -> Persona B  (listener, pool, envio, framing)
 *   cluster.c    -> Persona B  (tabla de nodos, hash de home)
 *   config.c     -> Persona B  (parser de dsm.conf)
 *   directory.c  -> AMBAS      (directorio en el nodo home)
 *   dsm.c        -> AMBAS      (API publica, alloc, barrier)
 *
 * Contratos de hilos y locks:
 *  1. NUNCA llamar a net_* teniendo tomado el lock del pager.
 *  2. Handlers MSG_INLINE corren en el listener: no pueden bloquearse ni
 *     esperar respuestas de red (solo copiar, actualizar, sem_post).
 *  3. Handlers MSG_POOL corren en el pool: SI pueden bloquearse, porque las
 *     respuestas las procesa el listener.
 *  4. El hilo de red y el pool escriben paginas SOLO por la vista del sistema.
 *  5. El handler de SIGSEGV solo usa funciones async-signal-safe.
 *
 * Cambios respecto al borrador del Entregable 1: ver docs/CAMBIOS_PROTOTIPO1.md
 */
#ifndef DSM_INTERNAL_H
#define DSM_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define DSM_MAX_NODES        16
#define DSM_IP_LEN           46
#define DSM_DEFAULT_PAGESZ   4096

typedef enum {
    DSM_OK = 0,
    DSM_ERR_NOMEM,
    DSM_ERR_TIMEOUT,
    DSM_ERR_NODE_DOWN,
    DSM_ERR_BAD_ARG,
    DSM_ERR_PROTO,
    DSM_ERR_IO
} dsm_err_t;

typedef uint64_t page_id_t;
typedef uint32_t node_id_t;

typedef enum { ACC_READ = 0, ACC_WRITE = 1 } access_t;

typedef enum {
    PAGE_INVALID = 0,
    PAGE_SHARED,
    PAGE_MODIFIED,
    PAGE_PENDING_R,
    PAGE_PENDING_W
} page_state_t;

typedef enum {
    MSG_PAGE_REQUEST = 1,
    MSG_PAGE_DATA,
    MSG_FETCH,
    MSG_FETCH_REPLY,
    MSG_INVALIDATE,
    MSG_INVALIDATE_ACK,
    MSG_PAGE_WRITEBACK,
    MSG_JOIN,
    MSG_NODE_LIST,
    MSG_HEARTBEAT,
    MSG_ALLOC_REQUEST,
    MSG_ALLOC_REPLY,
    MSG_STATS_REQUEST,
    MSG_STATS_REPLY,
    MSG_BARRIER_ARRIVE,
    MSG_BARRIER_RELEASE,
    MSG_TYPE_MAX
} msg_type_t;

/* Encabezado en host order en memoria; network.c convierte al enviar/recibir.
 * Uso de campos:
 *   PAGE_REQUEST : access = READ/WRITE pedido
 *   PAGE_DATA    : access = permiso concedido
 *   FETCH        : access = para que se piden los datos (READ => owner baja a S,
 *                  WRITE => owner pasa a I)
 *   BARRIER_*    : page_id = id del barrier; req_id (ARRIVE) = n participantes */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t src_node;
    uint64_t page_id;
    uint32_t req_id;
    uint8_t  access;
    uint32_t payload_len;
} msg_header_t;

_Static_assert(sizeof(msg_header_t) == 22, "msg_header_t debe medir 22 bytes");

typedef struct {
    node_id_t node_id;
    char      ip[DSM_IP_LEN];
    uint16_t  port;
    uint64_t  mem_bytes;
    uint64_t  base_addr;
    uint8_t   alive;
    time_t    last_heartbeat;
} node_info_t;

typedef struct {
    size_t   page_size;
    uint64_t mem_per_node;
    uint32_t max_nodes;          /* slots declarados; el hash usa este valor */
    uint32_t num_nodes;          /* [P1] lineas node= leidas (debe == max_nodes) */
    uint64_t global_base_addr;
    uint32_t worker_pool_size;
    uint32_t request_timeout_ms;
    uint32_t barrier_timeout_ms; /* [P1] nuevo */
    uint32_t heartbeat_interval_ms;
    uint32_t heartbeat_timeout_ms;
    uint32_t max_payload;
    uint64_t hash_seed;
    uint16_t port_main, port_control, port_heartbeat;
    node_id_t self_id;
    node_info_t slots[DSM_MAX_NODES];
} dsm_config_t;

/* ---- config.c (B) ---- */
dsm_err_t config_load(const char *path, dsm_config_t *out);
const dsm_config_t *config_get(void);

/* ---- cluster.c (B) ---- */
dsm_err_t   cluster_init(const dsm_config_t *cfg);
node_id_t   cluster_self_id(void);
bool        cluster_is_alive(node_id_t id);
bool        cluster_get_node(node_id_t id, node_info_t *out);
node_id_t   cluster_home_of(page_id_t page);          /* splitmix64(page^seed) % max_nodes */
node_id_t   cluster_initial_owner(page_id_t page);    /* nodo que aporta el rango */
node_id_t   cluster_barrier_coord(uint64_t barrier_id);
uint64_t    cluster_splitmix64(uint64_t x);

/* ---- logging (solo fuera del handler de SIGSEGV) ---- */
extern int dsm_verbose;
#define DSM_LOG(...) do { if (dsm_verbose) { \
    fprintf(stderr, "[dsm n%u] ", (unsigned)cluster_self_id()); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- network.c (B) ---- */
typedef enum { MSG_INLINE, MSG_POOL } msg_class_t;
typedef void (*msg_handler_t)(const msg_header_t *h, const void *payload);

dsm_err_t net_init(const dsm_config_t *cfg);
void      net_shutdown(void);
void      net_register(msg_type_t type, msg_handler_t fn, msg_class_t cls);
dsm_err_t net_start(void);
/* Thread-safe. Completa src_node y req_id (si es 0). El LLAMADOR fija
 * payload_len [P1: cambio respecto al borrador]. Si dst == self, despacha local. */
dsm_err_t net_send(node_id_t dst, msg_header_t *h, const void *payload);
dsm_err_t net_send_page_request(page_id_t page, access_t acc, uint32_t *req_id_out);

/* ---- pager.c (A) ---- */
typedef struct {
    uint64_t faults_read, faults_write;
    uint64_t pages_received, invalidations, fetches_served;
} pager_stats_t;

dsm_err_t pager_init(const dsm_config_t *cfg);   /* antes de net_start */
void      pager_shutdown(void);
void     *pager_app_addr(page_id_t page);
void     *pager_sys_addr(page_id_t page);
page_state_t pager_get_state(page_id_t page);
void      pager_get_stats(pager_stats_t *out);

void      pager_on_page_data(const msg_header_t *h, const void *page_bytes);
dsm_err_t pager_on_invalidate(page_id_t page);
dsm_err_t pager_on_fetch(page_id_t page, access_t for_access, void *out);

/* ---- directory.c (AMBAS) ---- */
dsm_err_t dir_init(const dsm_config_t *cfg);
void dir_shutdown(void);
void dir_on_page_request(const msg_header_t *h, const void *unused);
void dir_on_fetch_reply(const msg_header_t *h, const void *page_bytes);
void dir_on_invalidate_ack(const msg_header_t *h, const void *unused);
void dir_on_writeback(const msg_header_t *h, const void *page_bytes);   /* [P1] ahora INLINE */

/* ---- API publico (dsm.c, AMBAS) ---- */
dsm_err_t dsm_init(const char *config_path);       /* nodo propio: env DSM_NODE_ID */
void     *dsm_alloc(size_t size);
dsm_err_t dsm_free(void *ptr);
dsm_err_t dsm_barrier(uint32_t id, uint32_t n);
dsm_err_t dsm_shutdown(void);
void     *dsm_slot_base(node_id_t node);           /* [P1] base del rango de un nodo */

#endif
