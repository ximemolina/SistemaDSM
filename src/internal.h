/*
 * internal.h : contrato entre los modulos del DSM (Prototipo 2)
 *
 * Duenos: pager.c (A) | network.c, cluster.c, config.c, stats.c (B)
 *         directory.c, dsm.c, main.c (AMBAS)
 *
 * Contratos de hilos y locks:
 *  1. NUNCA llamar a net_* teniendo tomado un lock del pager.
 *  2. MSG_INLINE (listener): no bloquea ni envia por red; solo copia/actualiza/sem_post.
 *  3. MSG_POOL (pool "sin espera"): puede enviar mensajes pero NUNCA espera una
 *     respuesta de red ni un lock que mantenga un handler MSG_POOL_WAIT.
 *  4. MSG_POOL_WAIT (pool "con espera"): puede bloquearse esperando respuestas
 *     (FETCH_REPLY, ACK). Sus respuestas las procesan INLINE o POOL, nunca otro
 *     POOL_WAIT. Asi el agotamiento de este pool no puede producir deadlock.
 *  5. El hilo de red y los pools escriben paginas SOLO por la vista del sistema.
 *  6. El handler de SIGSEGV solo usa funciones async-signal-safe.
 */
#ifndef DSM_INTERNAL_H
#define DSM_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <netinet/in.h>

#define DSM_MAX_NODES        16
#define DSM_IP_LEN           64      /* acepta IP o nombre de host */
#define DSM_NAME_LEN         32
#define DSM_DEFAULT_PAGESZ   4096

typedef enum {
    DSM_OK = 0, DSM_ERR_NOMEM, DSM_ERR_TIMEOUT, DSM_ERR_NODE_DOWN,
    DSM_ERR_BAD_ARG, DSM_ERR_PROTO, DSM_ERR_IO
} dsm_err_t;

typedef uint64_t page_id_t;
typedef uint32_t node_id_t;
typedef enum { ACC_READ = 0, ACC_WRITE = 1 } access_t;

typedef enum {
    PAGE_INVALID = 0, PAGE_SHARED, PAGE_MODIFIED, PAGE_PENDING_R, PAGE_PENDING_W
} page_state_t;

typedef enum {
    MSG_PAGE_REQUEST = 1, MSG_PAGE_DATA, MSG_FETCH, MSG_FETCH_REPLY,
    MSG_INVALIDATE, MSG_INVALIDATE_ACK, MSG_PAGE_WRITEBACK,
    MSG_JOIN, MSG_NODE_LIST, MSG_HEARTBEAT,
    MSG_ALLOC_REQUEST, MSG_ALLOC_REPLY, MSG_STATS_REQUEST, MSG_STATS_REPLY,
    MSG_BARRIER_ARRIVE, MSG_BARRIER_RELEASE,
    MSG_PAGE_RELEASE,                       /* [P2] un nodo que sale devuelve sus paginas */
    MSG_LOCK_ACQUIRE, MSG_LOCK_GRANT, MSG_LOCK_RELEASE,   /* [P2] dsm_lock */
    MSG_SPAWN_TASK, MSG_TASK_DONE, MSG_TASK_STATUS,       /* [Extra 1] ejecucion distribuida */
    MSG_TYPE_MAX
} msg_type_t;

/* Encabezado: 22 bytes. Host order en memoria; network.c convierte. */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t src_node;
    uint64_t page_id;       /* tambien id de barrier / id de lock */
    uint32_t req_id;        /* RPC: empareja; directorio: generacion de transaccion */
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
    uint32_t  tasks_running;         /* [Extra 1] tareas en ejecucion + en cola (reportado por latidos) */
    uint32_t  load100;               /* [Extra 1] carga de CPU (loadavg 1 min x 100) */
} node_info_t;

typedef struct {
    size_t   page_size;
    uint64_t mem_per_node;           /* uniforme en todos los nodos (decision documentada) */
    uint32_t max_nodes;              /* slots; el hash usa este valor */
    uint32_t num_nodes;
    uint64_t global_base_addr;
    uint32_t worker_pool_size;       /* pool CON espera (PAGE_REQUEST, PAGE_RELEASE) */
    uint32_t nowait_pool_size;       /* [P2] pool SIN espera (FETCH, INVALIDATE, ...) */
    uint32_t request_timeout_ms;
    uint32_t request_retries;        /* [P2] reenvios del PAGE_REQUEST antes de abortar */
    uint32_t connect_timeout_ms;     /* [P2] connect() no bloqueante */
    uint32_t join_timeout_ms;        /* [P2] espera de todos los nodos en dsm_init */
    uint32_t barrier_timeout_ms;
    uint32_t heartbeat_interval_ms;
    uint32_t heartbeat_timeout_ms;
    uint32_t min_hold_us;            /* [P2] retencion minima de una pagina recien dada en M */
    uint32_t max_payload;
    uint64_t hash_seed;
    uint16_t heartbeat_port_offset;  /* [P2] UDP de latidos = puerto del nodo + offset */
    uint8_t  wait_all_nodes;         /* [P2] 1 = dsm_init espera a los max_nodes nodos */
    uint8_t  release_memory;         /* [P2] 1 = PUNCH_HOLE en paginas invalidadas ajenas */
    uint32_t task_pool_size;         /* [Extra 1] hilos ejecutores de tareas por nodo */
    uint32_t task_timeout_ms;        /* [Extra 1] espera maxima de dsm_wait (0 = barrier_timeout_ms) */
    uint32_t task_max_retries;       /* [Extra 1] reenvios de una tarea DSM_TASK_RETRY si cae su nodo */
    node_id_t self_id;
    node_info_t slots[DSM_MAX_NODES];
} dsm_config_t;

static inline uint64_t dsm_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static inline uint64_t dsm_now_ms(void) { return dsm_now_ns() / 1000000ULL; }

/* ---- estadisticas ---- */
enum { ST_PAGES_I, ST_PAGES_S, ST_PAGES_M, ST_PAGES_PENDING, ST_FAULT_R, ST_FAULT_W,
       ST_PAGES_RX, ST_INVALIDATIONS, ST_FETCHES, ST_FETCH_DELAYED, ST_DIR_ENTRIES,
       ST_TX_MSGS, ST_RX_MSGS, ST_TX_BYTES, ST_PUNCHED, ST_RETRIES,
       ST_TASKS_SPAWNED, ST_TASKS_EXECUTED, STAT_N };
typedef struct { uint32_t node; uint8_t alive; uint64_t v[STAT_N]; } node_stats_t;
typedef node_stats_t dsm_stats_t;

typedef struct {
    uint64_t faults_read, faults_write, pages_received, invalidations, fetches_served;
    uint64_t fetches_delayed, punched, retries;
} pager_stats_t;

typedef struct { uint64_t page; uint32_t owner; uint64_t copyset; uint8_t dirty; } dir_snap_t;

/* Entrada de la tabla de asignaciones (en el coordinador, nodo 0) */
typedef struct {
    uint32_t node, pid, seq; uint8_t live;
    uint64_t addr, size; char name[DSM_NAME_LEN];
} alloc_entry_t;

/* ---- config.c ---- */
dsm_err_t config_load(const char *path, dsm_config_t *out);
const dsm_config_t *config_get(void);

/* ---- cluster.c ---- */
extern int dsm_verbose;
dsm_err_t cluster_init(const dsm_config_t *cfg);
void      cluster_register(void);                 /* handlers JOIN / NODE_LIST */
dsm_err_t cluster_start_heartbeat(void);
dsm_err_t cluster_join(void);                     /* envia JOIN y espera a todos si wait_all_nodes */
void      cluster_shutdown(void);
node_id_t cluster_self_id(void);
bool      cluster_is_alive(node_id_t id);
bool      cluster_get_node(node_id_t id, node_info_t *out);
bool      cluster_get_addr(node_id_t id, struct sockaddr_in *out);
void      cluster_mark_alive(node_id_t id);
void      cluster_mark_dead(node_id_t id);
uint32_t  cluster_alive_count(void);
uint64_t  cluster_splitmix64(uint64_t x);
node_id_t cluster_home_of(page_id_t page);
node_id_t cluster_initial_owner(page_id_t page);
node_id_t cluster_barrier_coord(uint64_t id);
node_id_t cluster_lock_coord(uint64_t id);
node_id_t cluster_alloc_coord(void);
void      cluster_set_local_tasks(uint32_t load);        /* [Extra 1] lo llama tasks.c */
uint32_t  cluster_tasks_running(node_id_t id);
uint32_t  cluster_load100(node_id_t id);

#define DSM_LOG(...) do { if (dsm_verbose) { \
    fprintf(stderr, "[dsm n%u] ", (unsigned)cluster_self_id()); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- network.c ---- */
typedef enum { MSG_INLINE, MSG_POOL, MSG_POOL_WAIT } msg_class_t;
typedef void (*msg_handler_t)(const msg_header_t *h, const void *payload);

dsm_err_t net_init(const dsm_config_t *cfg);
void      net_shutdown(void);
void      net_register(msg_type_t type, msg_handler_t fn, msg_class_t cls);
dsm_err_t net_start(void);
dsm_err_t net_send(node_id_t dst, msg_header_t *h, const void *payload);
dsm_err_t net_send_page_request(page_id_t page, access_t acc, uint32_t *req_id_out);
/* RPC: envia la peticion y espera la respuesta con el mismo req_id (ALLOC_REPLY / STATS_REPLY).
 * *rlen: entra = capacidad de 'reply', sale = bytes recibidos. */
dsm_err_t net_rpc_call(node_id_t dst, msg_type_t type, uint8_t access, uint64_t page_id,
                       const void *payload, uint32_t plen, void *reply, uint32_t *rlen,
                       uint32_t timeout_ms);
void      net_get_counters(uint64_t *tx_msgs, uint64_t *rx_msgs, uint64_t *tx_bytes);

/* ---- pager.c (A) ---- */
dsm_err_t pager_init(const dsm_config_t *cfg);
void      pager_shutdown(void);
void     *pager_app_addr(page_id_t page);
void     *pager_sys_addr(page_id_t page);
page_state_t pager_get_state(page_id_t page);
void      pager_get_stats(pager_stats_t *out);
void      pager_count_states(uint64_t out[4]);           /* I, S, M, PENDING */
void      pager_dump_local(FILE *f, unsigned max);
dsm_err_t pager_prefetch(page_id_t page, access_t acc);  /* pide la pagina sin esperar a un fallo */
void      pager_release_all(void);                       /* write-back al apagar */
void      pager_on_page_data(const msg_header_t *h, const void *page_bytes);
dsm_err_t pager_on_invalidate(page_id_t page);
dsm_err_t pager_on_fetch(page_id_t page, access_t for_access, void *out);

/* ---- directory.c ---- */
dsm_err_t dir_init(const dsm_config_t *cfg);
void dir_shutdown(void);
void dir_on_page_request(const msg_header_t *h, const void *unused);
void dir_on_fetch_reply(const msg_header_t *h, const void *page_bytes);
void dir_on_invalidate_ack(const msg_header_t *h, const void *unused);
void dir_on_writeback(const msg_header_t *h, const void *page_bytes);
void dir_on_release(const msg_header_t *h, const void *page_bytes);
uint32_t dir_snapshot(page_id_t first, dir_snap_t *out, uint32_t max, page_id_t *next);
uint64_t dir_count(void);

/* ---- stats.c ---- */
dsm_err_t stats_init(void);
dsm_err_t dsm_stats(dsm_stats_t *out);                       /* este nodo */
dsm_err_t dsm_stats_node(node_id_t n, dsm_stats_t *out);     /* local o remoto */
void      dsm_print_stats(FILE *f);                          /* todos los nodos */
void      dsm_print_local_pages(FILE *f, unsigned max);
void      dsm_print_global_pages(FILE *f, unsigned max_per_node);
void      dsm_print_allocs(FILE *f);
dsm_err_t dsm_write_html(const char *path);

/* ---- ejecucion distribuida de tareas (tasks.c) [Extra 1] ---- */
#define DSM_TASK_MAX_ARGS 256
#define DSM_ANY_NODE      (-1)
#define DSM_TASK_RETRY    0x1u        /* si cae el nodo que ejecuta, reenviar la tarea (debe ser idempotente) */
typedef uint64_t dsm_task_t;
typedef enum { DSM_TASK_PENDING = 1, DSM_TASK_RUNNING, DSM_TASK_DONE, DSM_TASK_FAILED } dsm_task_state_t;
/* La funcion recibe una copia de los argumentos (punteros al espacio DSM incluidos) y devuelve un resultado. */
typedef int64_t (*dsm_task_fn)(const void *args, uint32_t len);
dsm_err_t tasks_init(void);
void      tasks_shutdown(void);
void      tasks_counters(uint64_t *spawned, uint64_t *executed);
dsm_err_t dsm_register_task(uint32_t func_id, dsm_task_fn fn);   /* en TODOS los nodos (mismo binario) */
dsm_err_t dsm_spawn(uint32_t func_id, const void *args, uint32_t len, int32_t node, uint32_t flags, dsm_task_t *out);
dsm_err_t dsm_wait(dsm_task_t task, int64_t *result);            /* libera el registro de la tarea */
dsm_err_t dsm_wait_all(const dsm_task_t *tasks, uint32_t n, int64_t *results);
dsm_err_t dsm_task_status(dsm_task_t task, dsm_task_state_t *state);

/* ---- API publico (dsm.c) ---- */
dsm_err_t dsm_init(const char *config_path);               /* nodo propio: env DSM_NODE_ID */
void     *dsm_alloc(size_t size);
void     *dsm_alloc_named(const char *name, size_t size);  /* mismo puntero en todos los nodos */
dsm_err_t dsm_free(void *ptr);
dsm_err_t dsm_barrier(uint32_t id, uint32_t n);
dsm_err_t dsm_lock(uint32_t id);
dsm_err_t dsm_unlock(uint32_t id);
dsm_err_t dsm_prefetch_write(void *ptr, size_t len);       /* pide M directamente (evita 2 viajes) */
dsm_err_t dsm_prefetch_read(void *ptr, size_t len);
dsm_err_t dsm_shutdown(void);
void     *dsm_slot_base(node_id_t node);
dsm_err_t dsm_last_error(void);
dsm_err_t dsm_alloc_list(uint64_t first, alloc_entry_t *out, uint32_t max, uint32_t *count, uint64_t *next);

#endif