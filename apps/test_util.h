/* Utilidades comunes de los tests de la revision. Cada nodo ejecuta el mismo binario: ./test_x dsm.conf */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include "../src/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
static uint32_t g_nn, g_me, g_bar = 1000;   /* todos los nodos incrementan g_bar igual => mismos ids */

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "[n%u] FALLO %s:%d: ", g_me, __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_fail++; } } while (0)

static inline void sync_all(void) {
    if (dsm_barrier(g_bar++, g_nn) != DSM_OK) { fprintf(stderr, "[n%u] barrier fallo\n", g_me); exit(2); }
}

static inline void t_init(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s dsm.conf\n", argv[0]); exit(1); }
    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); exit(1); }
    g_nn = config_get()->max_nodes; g_me = cluster_self_id();
}

/* Suma de un contador de stats en todos los nodos (solo llamar desde el nodo 0, antes del barrier final). */
static inline uint64_t stat_total(int idx) {
    uint64_t t = 0;
    for (uint32_t n = 0; n < g_nn; n++) { dsm_stats_t s; if (dsm_stats_node(n, &s) == DSM_OK) t += s.v[idx]; }
    return t;
}

static int g_last_fail;
#define END_PHASE(name) do { printf("[n%u] %s: %s\n", g_me, name, g_fail == g_last_fail ? "OK" : "FALLO"); \
    g_last_fail = g_fail; } while (0)

static inline int t_done(void) {
    sync_all();
    dsm_shutdown();
    printf("[n%u] RESULTADO: %s\n", g_me, g_fail ? "FALLO" : "PASS");
    return g_fail ? 1 : 0;
}
#endif
