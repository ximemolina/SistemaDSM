/*
 * tasks.c [Extra 1]: ejecucion distribuida de tareas sobre el DSM.
 *
 *  - dsm_register_task(id, fn): tabla de funciones (mismo binario en todos los nodos, el id viaja por la red).
 *  - dsm_spawn(): elige un nodo (explicito o el menos cargado) y envia MSG_SPAWN_TASK con id de funcion y
 *    argumentos (hasta 256 bytes; los datos grandes viven en el espacio DSM y viajan como punteros).
 *  - El nodo destino la encola; un pool de ejecutores (no los pools de red) corre la funcion.
 *    Avisa MSG_TASK_STATUS(RUNNING) al iniciar y MSG_TASK_DONE (resultado int64) al terminar.
 *  - dsm_wait(): espera el resultado; si el nodo que ejecuta cae (heartbeat) y la tarea es DSM_TASK_RETRY,
 *    la reenvia a otro nodo; si no, devuelve DSM_ERR_NODE_DOWN.
 * Los ejecutores son hilos de aplicacion normales: sus accesos al espacio DSM generan SIGSEGV y MSI los resuelve.
 */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <endian.h>
#include <arpa/inet.h>

#define MAX_FUNCS 64
#define MAX_TASKS 512
#define EXEC_QMAX 4096
enum { TS_OK = 0, TS_NOFUNC = 1, TS_REJECT = 3 };

static const dsm_config_t *C;

/* ---------- registro de funciones ---------- */
static struct { uint32_t id; dsm_task_fn fn; } funcs[MAX_FUNCS];
static int nfuncs;
static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;

dsm_err_t dsm_register_task(uint32_t id, dsm_task_fn fn) {
    if (!fn) return DSM_ERR_BAD_ARG;
    pthread_mutex_lock(&fmu);
    for (int i = 0; i < nfuncs; i++) if (funcs[i].id == id) { funcs[i].fn = fn; pthread_mutex_unlock(&fmu); return DSM_OK; }
    if (nfuncs >= MAX_FUNCS) { pthread_mutex_unlock(&fmu); return DSM_ERR_NOMEM; }
    funcs[nfuncs].id = id; funcs[nfuncs].fn = fn; nfuncs++;
    pthread_mutex_unlock(&fmu);
    return DSM_OK;
}
static dsm_task_fn find_fn(uint32_t id) {
    dsm_task_fn r = NULL;
    pthread_mutex_lock(&fmu);
    for (int i = 0; i < nfuncs; i++) if (funcs[i].id == id) { r = funcs[i].fn; break; }
    pthread_mutex_unlock(&fmu);
    return r;
}

/* ---------- lado ejecutor (nodo destino) ---------- */
typedef struct job { struct job *next; uint64_t id; node_id_t origin; uint32_t func, len; uint8_t args[]; } job_t;
static job_t *qh, *qt; static unsigned qn, running_n;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static pthread_t *exec_th; static unsigned nexec;
static volatile int run;
static atomic_ullong st_spawned, st_executed;

static void publish_load(void) { cluster_set_local_tasks(qn + running_n); }       /* con qmu tomado */

static void send_done(node_id_t origin, uint64_t id, uint8_t status, int64_t result) {
    uint64_t r = htobe64((uint64_t)result);
    msg_header_t h = { .type = MSG_TASK_DONE, .page_id = id, .access = status, .payload_len = sizeof r };
    net_send(origin, &h, &r);
}

static void on_spawn(const msg_header_t *h, const void *payload) {                 /* MSG_POOL: solo encola */
    uint32_t func = 0, len = 0;
    if (h->payload_len >= 8) { memcpy(&func, payload, 4); memcpy(&len, (const char *)payload + 4, 4); func = ntohl(func); len = ntohl(len); }
    if (h->payload_len < 8 || len > DSM_TASK_MAX_ARGS || 8 + len != h->payload_len) { send_done(h->src_node, h->page_id, TS_REJECT, 0); return; }
    job_t *j = malloc(sizeof *j + len);
    if (!j) { send_done(h->src_node, h->page_id, TS_REJECT, 0); return; }
    j->next = NULL; j->id = h->page_id; j->origin = h->src_node; j->func = func; j->len = len;
    memcpy(j->args, (const char *)payload + 8, len);
    pthread_mutex_lock(&qmu);
    if (qn >= EXEC_QMAX || !run) { pthread_mutex_unlock(&qmu); free(j); send_done(h->src_node, h->page_id, TS_REJECT, 0); return; }
    if (qt) qt->next = j; else qh = j;
    qt = j; qn++; publish_load();
    pthread_cond_signal(&qcv);
    pthread_mutex_unlock(&qmu);
}

static void *exec_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qmu);
        while (!qh && run) pthread_cond_wait(&qcv, &qmu);
        if (!qh) { pthread_mutex_unlock(&qmu); break; }
        job_t *j = qh; qh = j->next; if (!qh) qt = NULL;
        qn--; running_n++; publish_load();
        pthread_mutex_unlock(&qmu);

        msg_header_t st = { .type = MSG_TASK_STATUS, .page_id = j->id, .access = DSM_TASK_RUNNING };
        net_send(j->origin, &st, NULL);
        dsm_task_fn fn = find_fn(j->func);
        int64_t res = 0; uint8_t status = TS_OK;
        if (!fn) status = TS_NOFUNC; else res = fn(j->args, j->len);
        atomic_fetch_add(&st_executed, 1);

        pthread_mutex_lock(&qmu); running_n--; publish_load(); pthread_mutex_unlock(&qmu);
        send_done(j->origin, j->id, status, res);
        free(j);
    }
    return NULL;
}

/* ---------- lado origen (quien lanza) ---------- */
typedef struct {
    int used; uint64_t id; uint32_t func, flags, len, retries;
    node_id_t target; dsm_task_state_t state; int64_t result; dsm_err_t err;
    uint8_t args[DSM_TASK_MAX_ARGS];
} task_t;
static task_t tasks[MAX_TASKS];
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tcv = PTHREAD_COND_INITIALIZER;
static uint64_t task_seq = 1;
static unsigned rr;

static task_t *find_task(uint64_t id) {                                            /* con tmu tomado */
    for (int i = 0; i < MAX_TASKS; i++) if (tasks[i].used && tasks[i].id == id) return &tasks[i];
    return NULL;
}

/* Planificador: el nodo vivo con menos tareas (reportadas en los latidos + las propias pendientes);
 * desempata por carga de CPU y luego rota. 'want' >= 0 fija el nodo. Con tmu tomado. */
static int pick_node(int32_t want, uint32_t exclude) {
    if (want >= 0) return (cluster_is_alive((node_id_t)want) && !(exclude & (1u << want))) ? want : -1;
    int best = -1; uint64_t best_score = UINT64_MAX; unsigned n = C->max_nodes, start = rr++ % n;
    for (unsigned k = 0; k < n; k++) {
        node_id_t nd = (start + k) % n;
        if (!cluster_is_alive(nd) || (exclude & (1u << nd))) continue;
        uint64_t sc = cluster_tasks_running(nd);        /* nodo propio: carga local exacta */
        if (nd != cluster_self_id()) {                  /* remotos: max(reportado por latidos, enviado por mi y pendiente) */
            uint64_t mine = 0;                          /* (el reporte ya incluye mis tareas cuando se actualiza: no sumar) */
            for (int i = 0; i < MAX_TASKS; i++)
                if (tasks[i].used && tasks[i].target == nd && (tasks[i].state == DSM_TASK_PENDING || tasks[i].state == DSM_TASK_RUNNING)) mine++;
            if (mine > sc) sc = mine;
        }
        uint32_t cpu = cluster_load100(nd); if (cpu > 999) cpu = 999;
        sc = sc * 1000 + cpu;
        if (sc < best_score) { best_score = sc; best = (int)nd; }
    }
    return best;
}

static dsm_err_t send_spawn(node_id_t target, const task_t *t) {
    uint8_t buf[8 + DSM_TASK_MAX_ARGS];
    uint32_t f = htonl(t->func), l = htonl(t->len);
    memcpy(buf, &f, 4); memcpy(buf + 4, &l, 4); memcpy(buf + 8, t->args, t->len);
    msg_header_t h = { .type = MSG_SPAWN_TASK, .page_id = t->id, .payload_len = 8 + t->len };
    return net_send(target, &h, buf);
}

static void on_task_status(const msg_header_t *h, const void *unused) {           /* INLINE */
    (void)unused;
    pthread_mutex_lock(&tmu);
    task_t *t = find_task(h->page_id);
    if (t && t->target == h->src_node && t->state == DSM_TASK_PENDING) { t->state = DSM_TASK_RUNNING; pthread_cond_broadcast(&tcv); }
    pthread_mutex_unlock(&tmu);
}
static void on_task_done(const msg_header_t *h, const void *payload) {            /* INLINE */
    pthread_mutex_lock(&tmu);
    task_t *t = find_task(h->page_id);
    if (t && t->target == h->src_node && t->state != DSM_TASK_DONE && t->state != DSM_TASK_FAILED) {   /* ignora ecos de un nodo anterior */
        if (h->access == TS_OK && h->payload_len == 8) { uint64_t r; memcpy(&r, payload, 8); t->result = (int64_t)be64toh(r); t->state = DSM_TASK_DONE; }
        else { t->state = DSM_TASK_FAILED; t->err = (h->access == TS_NOFUNC) ? DSM_ERR_BAD_ARG : DSM_ERR_NOMEM; }
        pthread_cond_broadcast(&tcv);
    }
    pthread_mutex_unlock(&tmu);
}

dsm_err_t dsm_spawn(uint32_t func, const void *args, uint32_t len, int32_t node, uint32_t flags, dsm_task_t *out) {
    if (!out || len > DSM_TASK_MAX_ARGS || (len && !args) || node >= (int32_t)C->max_nodes) return DSM_ERR_BAD_ARG;
    if (node >= 0 && !cluster_is_alive((node_id_t)node)) return DSM_ERR_NODE_DOWN;
    pthread_mutex_lock(&tmu);
    task_t *t = NULL;
    for (int i = 0; i < MAX_TASKS; i++) if (!tasks[i].used) { t = &tasks[i]; break; }
    if (!t) { pthread_mutex_unlock(&tmu); return DSM_ERR_NOMEM; }          /* tareas lanzadas y nunca esperadas */
    memset(t, 0, sizeof *t);
    t->used = 1; t->func = func; t->flags = flags; t->len = len; t->state = DSM_TASK_PENDING; t->target = (node_id_t)-1;
    t->id = ((uint64_t)cluster_self_id() << 48) | (task_seq++ & 0xFFFFFFFFFFFFULL);
    if (len) memcpy(t->args, args, len);
    uint32_t excl = 0; dsm_err_t rc = DSM_ERR_NODE_DOWN;
    for (unsigned attempt = 0; attempt < C->max_nodes; attempt++) {
        int tg = pick_node(node, excl);
        if (tg < 0) break;
        t->target = (node_id_t)tg;
        pthread_mutex_unlock(&tmu);
        rc = send_spawn((node_id_t)tg, t);
        pthread_mutex_lock(&tmu);
        if (rc == DSM_OK || node >= 0) break;
        DSM_LOG("spawn: nodo %d no respondio (err %d), probando otro", tg, rc);
        excl |= 1u << tg;                                                 /* ese nodo no responde: probar otro */
    }
    if (rc != DSM_OK) { t->used = 0; pthread_mutex_unlock(&tmu); return rc; }
    *out = t->id;
    DSM_LOG("spawn: tarea %llx -> nodo %u", (unsigned long long)(t->id & 0xFFFFFFFFFFFFULL), t->target);
    pthread_mutex_unlock(&tmu);
    atomic_fetch_add(&st_spawned, 1);
    return DSM_OK;
}

dsm_err_t dsm_wait(dsm_task_t id, int64_t *result) {
    uint64_t limit = dsm_now_ms() + (C->task_timeout_ms ? C->task_timeout_ms : C->barrier_timeout_ms);
    pthread_mutex_lock(&tmu);
    task_t *t = find_task(id);
    if (!t) { pthread_mutex_unlock(&tmu); return DSM_ERR_BAD_ARG; }
    for (;;) {
        if (!t->used || t->id != id) { pthread_mutex_unlock(&tmu); return DSM_ERR_BAD_ARG; }   /* otro hilo ya la recogio */
        if (t->state == DSM_TASK_DONE) { if (result) *result = t->result; t->used = 0; pthread_mutex_unlock(&tmu); return DSM_OK; }
        if (t->state == DSM_TASK_FAILED) { dsm_err_t e = t->err; t->used = 0; pthread_mutex_unlock(&tmu); return e; }
        node_id_t tg = t->target;
        if (tg != cluster_self_id() && !cluster_is_alive(tg)) {           /* el nodo que la ejecutaba cayo */
            int nt = ((t->flags & DSM_TASK_RETRY) && t->retries < C->task_max_retries) ? pick_node(DSM_ANY_NODE, 1u << tg) : -1;
            if (nt < 0) { t->used = 0; pthread_mutex_unlock(&tmu); return DSM_ERR_NODE_DOWN; }
            t->retries++; t->target = (node_id_t)nt; t->state = DSM_TASK_PENDING;
            pthread_mutex_unlock(&tmu);
            dsm_err_t rc = send_spawn((node_id_t)nt, t);
            pthread_mutex_lock(&tmu);
            if (rc != DSM_OK && t->used && t->id == id) { t->state = DSM_TASK_FAILED; t->err = rc; }
            continue;
        }
        if (dsm_now_ms() > limit) { pthread_mutex_unlock(&tmu); return DSM_ERR_TIMEOUT; }       /* se puede volver a esperar */
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&tcv, &tmu, &ts);
    }
}

dsm_err_t dsm_wait_all(const dsm_task_t *ts, uint32_t n, int64_t *results) {
    dsm_err_t first = DSM_OK;
    for (uint32_t i = 0; i < n; i++) {
        dsm_err_t rc = dsm_wait(ts[i], results ? &results[i] : NULL);
        if (rc != DSM_OK && first == DSM_OK) first = rc;
    }
    return first;
}

dsm_err_t dsm_task_status(dsm_task_t id, dsm_task_state_t *state) {
    pthread_mutex_lock(&tmu);
    task_t *t = find_task(id);
    if (t && state) *state = t->state;
    pthread_mutex_unlock(&tmu);
    return t ? DSM_OK : DSM_ERR_BAD_ARG;
}

void tasks_counters(uint64_t *s, uint64_t *e) { *s = st_spawned; *e = st_executed; }

dsm_err_t tasks_init(void) {
    C = config_get();
    net_register(MSG_SPAWN_TASK, on_spawn, MSG_POOL);
    net_register(MSG_TASK_STATUS, on_task_status, MSG_INLINE);
    net_register(MSG_TASK_DONE, on_task_done, MSG_INLINE);
    run = 1; nexec = C->task_pool_size;
    exec_th = calloc(nexec, sizeof(pthread_t));
    if (!exec_th) return DSM_ERR_NOMEM;
    for (unsigned i = 0; i < nexec; i++) pthread_create(&exec_th[i], NULL, exec_main, NULL);
    return DSM_OK;
}

void tasks_shutdown(void) {
    if (!run) return;
    pthread_mutex_lock(&qmu); run = 0; pthread_cond_broadcast(&qcv); pthread_mutex_unlock(&qmu);
    for (unsigned i = 0; i < nexec; i++) pthread_join(exec_th[i], NULL);
    free(exec_th); exec_th = NULL;
}
