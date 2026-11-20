/* Extra 1: ejecucion distribuida. Nodo 0 = maestro; el ultimo nodo = victima (muere ejecutando una tarea);
 * los demas sirven tareas. Requiere >= 3 nodos. */
#include "../src/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
enum { F_ECHO = 1, F_SLOW, F_SUM, F_NEST, F_DIE };
typedef struct { uint64_t *data, n, *out; } sum_args_t;

static int64_t f_echo(const void *a, uint32_t len) { (void)len; uint64_t x; memcpy(&x, a, 8); return (int64_t)(x * 10 + cluster_self_id()); }
static int64_t f_slow(const void *a, uint32_t len) { (void)a; (void)len; usleep(30000); return cluster_self_id(); }
static int64_t f_sum(const void *a, uint32_t len) {
    (void)len; sum_args_t s; memcpy(&s, a, sizeof s);
    uint64_t t = 0; for (uint64_t i = 0; i < s.n; i++) t += s.data[i];     /* lee del espacio DSM */
    s.out[0] = t;                                                          /* escribe el resultado en el espacio DSM */
    return (int64_t)t;
}
static int64_t f_nest(const void *a, uint32_t len) {
    (void)a; (void)len; dsm_task_t t; uint64_t x = 7; int64_t r = -1;
    if (dsm_spawn(F_ECHO, &x, sizeof x, DSM_ANY_NODE, 0, &t) != DSM_OK || dsm_wait(t, &r) != DSM_OK) return -1;
    return r + 1000;
}
static int64_t f_die(const void *a, uint32_t len) {
    (void)len; uint32_t victim; memcpy(&victim, a, 4);
    if (cluster_self_id() == victim) { _exit(0); }                         /* el nodo muere a mitad de tarea */
    usleep(100000); return 777;
}

static int failures;
static void check(const char *w, int64_t got, int64_t want) { if (got != want) { fprintf(stderr, "FALLO %s: got=%lld want=%lld\n", w, (long long)got, (long long)want); failures++; } }

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    dsm_register_task(F_ECHO, f_echo); dsm_register_task(F_SLOW, f_slow); dsm_register_task(F_SUM, f_sum);
    dsm_register_task(F_NEST, f_nest); dsm_register_task(F_DIE, f_die);
    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); return 1; }
    const dsm_config_t *c = config_get(); uint32_t nn = c->max_nodes; node_id_t me = cluster_self_id(), victim = nn - 1, victim2 = nn >= 4 ? nn - 2 : (node_id_t)-1;   /* victim2: muere SIN reintento */
    if (nn < 3) { fprintf(stderr, "se requieren >= 3 nodos\n"); return 1; }
    uint32_t alive_n = nn - 1 - (victim2 != (node_id_t)-1);
    uint32_t endbar = 9; while (cluster_barrier_coord(endbar) == victim || cluster_barrier_coord(endbar) == victim2) endbar++;   /* coordinador del barrier final: no una victima */
    if (dsm_barrier(1, nn) != DSM_OK) return 2;
    if (me != 0) {                                       /* nodos servidores */
        if (me == victim || me == victim2) { for (int i = 0; i < 60; i++) sleep(1); return 0; }
        if (dsm_barrier(endbar, alive_n) != DSM_OK) return 2;
        dsm_shutdown(); return 0;
    }

    int64_t r; dsm_task_t t;
    /* 1. tarea en un nodo explicito: el resultado revela donde corrio */
    for (node_id_t n = 0; n < nn; n++) {
        uint64_t x = 5 + n; if (dsm_spawn(F_ECHO, &x, sizeof x, (int32_t)n, 0, &t) != DSM_OK || dsm_wait(t, &r) != DSM_OK) { failures++; continue; }
        check("1 echo en nodo explicito", r, (int64_t)(x * 10 + n));
    }
    /* 2. funcion inexistente */
    if (dsm_spawn(99, NULL, 0, 1, 0, &t) != DSM_OK) failures++; else check("2 funcion inexistente", dsm_wait(t, &r), DSM_ERR_BAD_ARG);
    /* 3. balanceo: 30 tareas lentas sobre cualquier nodo */
    { dsm_task_t ts[30]; int64_t rs[30]; int cnt[DSM_MAX_NODES] = { 0 };
      for (int i = 0; i < 30; i++) if (dsm_spawn(F_SLOW, NULL, 0, DSM_ANY_NODE, 0, &ts[i]) != DSM_OK) failures++;
      if (dsm_wait_all(ts, 30, rs) != DSM_OK) failures++;
      for (int i = 0; i < 30; i++) if (rs[i] >= 0 && rs[i] < (int64_t)nn) cnt[rs[i]]++;
      printf("[n0] balanceo de 30 tareas:"); for (uint32_t n = 0; n < nn; n++) { printf(" nodo%u=%d", n, cnt[n]); if (cnt[n] < 1) { fprintf(stderr, "FALLO 3 nodo %u no recibio ninguna tarea\n", n); failures++; } } printf("\n"); }
    /* 4. datos en el espacio DSM: entrada y salida de las tareas */
    { uint64_t N = 3000; uint64_t *data = dsm_alloc_named("td", N * 8), *out = dsm_alloc_named("to", nn * 4096);
      if (!data || !out) { fprintf(stderr, "alloc fallo\n"); return 1; }
      for (uint64_t i = 0; i < N; i++) data[i] = i;
      dsm_task_t ts[DSM_MAX_NODES]; int64_t rs[DSM_MAX_NODES]; uint64_t chunk = N / nn;
      for (uint32_t k = 0; k < nn; k++) { sum_args_t s = { data + k * chunk, k == nn - 1 ? N - k * chunk : chunk, out + k * 512 }; if (dsm_spawn(F_SUM, &s, sizeof s, DSM_ANY_NODE, 0, &ts[k]) != DSM_OK) failures++; }
      if (dsm_wait_all(ts, nn, rs) != DSM_OK) failures++;
      uint64_t total = 0; for (uint32_t k = 0; k < nn; k++) { check("4 parcial en DSM == resultado", (int64_t)out[k * 512], rs[k]); total += (uint64_t)rs[k]; }
      check("4 suma total", (int64_t)total, (int64_t)(N * (N - 1) / 2)); }
    /* 5. tarea que lanza otra tarea y la espera */
    if (dsm_spawn(F_NEST, NULL, 0, 1, 0, &t) != DSM_OK || dsm_wait(t, &r) != DSM_OK) failures++; else { if (r < 1070 || r >= 1070 + (int64_t)nn) { fprintf(stderr, "FALLO 5 anidada: %lld\n", (long long)r); failures++; } }
    /* 6. el nodo victima muere ejecutando la tarea; con DSM_TASK_RETRY se completa en otro nodo */
    { uint32_t v = victim; uint64_t t0 = dsm_now_ms();
      if (dsm_spawn(F_DIE, &v, sizeof v, (int32_t)victim, DSM_TASK_RETRY, &t) != DSM_OK) failures++;
      else { check("6 tarea reintentada tras caida", dsm_wait(t, &r) == DSM_OK ? r : -1, 777);
             printf("[n0] tarea con DSM_TASK_RETRY completada tras la caida del nodo %u en %llu ms\n", victim, (unsigned long long)(dsm_now_ms() - t0)); } }
    /* 6b. sin DSM_TASK_RETRY: la caida del nodo se informa como DSM_ERR_NODE_DOWN */
    if (victim2 != (node_id_t)-1) { uint32_t v = victim2;
      if (dsm_spawn(F_DIE, &v, sizeof v, (int32_t)victim2, 0, &t) != DSM_OK) failures++;
      else { check("6b sin reintento => NODE_DOWN", dsm_wait(t, &r), DSM_ERR_NODE_DOWN); printf("[n0] tarea sin reintento: nodo %u cayo => DSM_ERR_NODE_DOWN\n", victim2); } }
    /* 7. nodo caido: spawn explicito falla; ANY_NODE lo evita */
    check("7 spawn a nodo caido", dsm_spawn(F_ECHO, &(uint64_t){ 1 }, 8, (int32_t)victim, 0, &t), DSM_ERR_NODE_DOWN);
    for (int i = 0; i < 6; i++) { uint64_t x = 1; if (dsm_spawn(F_ECHO, &x, 8, DSM_ANY_NODE, 0, &t) != DSM_OK || dsm_wait(t, &r) != DSM_OK) { failures++; continue; } if ((uint32_t)(r % 10) == victim || (uint32_t)(r % 10) == victim2) { fprintf(stderr, "FALLO 7 tarea enviada al nodo caido\n"); failures++; } }
    if (dsm_barrier(endbar, alive_n) != DSM_OK) failures++;
    dsm_shutdown();
    printf("[n0] test_tasks (%u nodos): %s\n", nn, failures ? "FALLO" : "PASS");
    return failures ? 1 : 0;
}