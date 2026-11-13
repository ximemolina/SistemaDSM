/* Prueba del Prototipo 1: 2 nodos comparten memoria via PAGE_REQUEST/PAGE_DATA. */
#include "../src/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define ITER    100
#define STRESS  1000
#define THREADS 2

static volatile uint64_t *pgB;
static int failures;
static uint32_t bar_id = 100;

static void sync_all(void) {
    if (dsm_barrier(bar_id++, 2) != DSM_OK) { fprintf(stderr, "barrier fallo\n"); exit(2); }
}
static void check(const char *what, uint64_t got, uint64_t want) {
    if (got != want) { fprintf(stderr, "FALLO %s: got=%llu want=%llu\n", what, (unsigned long long)got, (unsigned long long)want); failures++; }
}

static void *worker(void *a) {
    int idx = *(int *)a;
    for (int i = 0; i < STRESS; i++) pgB[idx]++;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s dsm.conf\n", argv[0]); return 1; }
    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); return 1; }
    node_id_t me = cluster_self_id(), other = 1 - me;

    /* Pagina 0 y 1 del rango del nodo 0 (reservadas con dsm_alloc en el nodo 0) */
    volatile uint64_t *pgA = (volatile uint64_t *)dsm_slot_base(0);
    if (me == 0) { void *p = dsm_alloc(2 * 4096); if (p != (void *)pgA) { fprintf(stderr, "alloc inesperado\n"); return 1; } }
    pgB = (volatile uint64_t *)((char *)pgA + 4096);
    volatile uint64_t *pgD = (volatile uint64_t *)dsm_slot_base(1);   /* pagina aportada por el nodo 1 */

    /* Test A: ping-pong lectura/escritura entre nodos, 100 repeticiones */
    for (int i = 1; i <= ITER; i++) {
        if (me == 0) pgA[0] = (uint64_t)i; else pgA[8] = (uint64_t)i * 2;
        sync_all();
        if (me == 1) check("A lee de nodo0", pgA[0], (uint64_t)i); else check("A lee de nodo1", pgA[8], (uint64_t)i * 2);
        sync_all();
    }
    printf("[n%u] Test A (ping-pong x%d): %s\n", me, ITER, failures ? "FALLO" : "OK");

    /* Test D: la pagina cuyo contribuyente es el nodo 1, escrita por el nodo 0 y viceversa */
    if (me == 0) pgD[0] = 0xABCDULL;
    sync_all();
    if (me == 1) check("D nodo1 lee lo escrito por nodo0", pgD[0], 0xABCDULL);
    if (me == 1) pgD[1] = 0x1234ULL;
    sync_all();
    if (me == 0) { check("D nodo0 lee lo escrito por nodo1", pgD[1], 0x1234ULL); check("D nodo0 conserva su valor", pgD[0], 0xABCDULL); }
    sync_all();
    printf("[n%u] Test D (pagina del contribuyente): %s\n", me, failures ? "FALLO" : "OK");

    /* Test B: estres. Misma pagina (false sharing), 2 hilos por nodo, palabras distintas */
    pthread_t th[THREADS]; int idx[THREADS];
    sync_all();
    for (int t = 0; t < THREADS; t++) { idx[t] = (int)me * THREADS + t; pthread_create(&th[t], NULL, worker, &idx[t]); }
    for (int t = 0; t < THREADS; t++) pthread_join(th[t], NULL);
    sync_all();
    for (int k = 0; k < 2 * THREADS; k++) check("B contador", pgB[k], STRESS);
    sync_all();
    printf("[n%u] Test B (estres, %d hilos/nodo, misma pagina): %s\n", me, THREADS, failures ? "FALLO" : "OK");
    (void)other;

    dsm_shutdown();
    printf("[n%u] RESULTADO: %s\n", me, failures ? "FALLO" : "PASS");
    return failures ? 1 : 0;
}
