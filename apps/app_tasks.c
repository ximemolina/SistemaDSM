/* Extra 1 - aplicacion: multiplicacion de matrices por TAREAS. El nodo 0 reparte bloques de filas con
 * dsm_spawn (cualquier nodo); las matrices viven en el espacio DSM y la tarea solo recibe punteros.
 * Uso: app_tasks dsm.conf [N] */
#include "../src/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/libdsm_math.h"
typedef struct { double *A, *B, *C; int32_t N, r0, r1; } mm_args_t;
static int64_t mm_task(const void *a, uint32_t len) {
    (void)len; mm_args_t m; memcpy(&m, a, sizeof m);
    for (int i = m.r0; i < m.r1; i++) for (int j = 0; j < m.N; j++) {
        double s = 0; for (int k = 0; k < m.N; k++) s += m.A[(size_t)i * m.N + k] * m.B[(size_t)k * m.N + j];
        m.C[(size_t)i * m.N + j] = s;
    }
    return cluster_self_id();                       /* devuelve donde corrio */
}

/* valor esperado de C[i][j] calculado solo con las formulas (no lee el DSM) */
static double ref_c(int N, int i, int j) {
    double s = 0; for (int k = 0; k < N; k++) s += ((i + k) % 7) * (double)((k * 3 + j) % 5);
    return s;
}
static void print_mat(const char *name, const double *M, int stride, int lim, int N) {
    printf("[n0] %s%s:\n", name, lim < N ? " (esquina superior izquierda)" : "");
    for (int i = 0; i < lim; i++) {
        printf("      ");
        for (int j = 0; j < lim; j++) printf(" %7.0f", M[(size_t)i * stride + j]);
        printf("\n");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    int N = argc > 2 ? atoi(argv[2]) : 96;
    dsm_register_task(1, mm_task);
    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); return 1; }
    const dsm_config_t *c = config_get(); uint32_t nn = c->max_nodes; node_id_t me = cluster_self_id();
    int bad = 0;
    if (me == 0) {
        size_t bytes = (size_t)N * N * sizeof(double);
        double *A = dsm_alloc_named("A", bytes), *B = dsm_alloc_named("B", bytes), *Cm = dsm_alloc_named("C", bytes);
        if (!A || !B || !Cm) { fprintf(stderr, "alloc fallo\n"); return 1; }
        for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { A[(size_t)i * N + j] = (i + j) % 7; B[(size_t)i * N + j] = (i * 3 + j) % 5; }

        int lim = N <= 8 ? N : 4;                       /* N<=8: matrices completas; si no, esquina 4x4 */
        printf("[n0] Multiplicacion C = A x B, N=%d (%.1f KiB por matriz, %.1f KiB en DSM)\n",
               N, bytes / 1024.0, 3 * bytes / 1024.0);
        printf("[n0] A[i][j] = (i+j) mod 7     B[i][j] = (3i+j) mod 5\n");
        print_mat("A", A, N, lim, N);
        print_mat("B", B, N, lim, N);

        int rpp = (int)(c->page_size / (N * sizeof(double))); if (rpp < 1) rpp = 1;          /* bloques alineados a pagina */
        int ntasks = (int)nn * 3, chunk = ((N + ntasks - 1) / ntasks + rpp - 1) / rpp * rpp, nt = 0;
        dsm_task_t ts[256]; int64_t rs[256]; int r0s[256], r1s[256]; uint64_t t0 = dsm_now_ms();
        for (int r0 = 0; r0 < N && nt < 256; r0 += chunk, nt++) {
            mm_args_t m = { A, B, Cm, N, r0, r0 + chunk > N ? N : r0 + chunk };
            r0s[nt] = m.r0; r1s[nt] = m.r1;
            if (dsm_spawn(1, &m, sizeof m, DSM_ANY_NODE, DSM_TASK_RETRY, &ts[nt]) != DSM_OK) { fprintf(stderr, "spawn fallo\n"); return 1; }
        }
        if (dsm_wait_all(ts, (uint32_t)nt, rs) != DSM_OK) { fprintf(stderr, "wait fallo\n"); bad = 1; }
        uint64_t ms = dsm_now_ms() - t0; int per[DSM_MAX_NODES] = { 0 };
        for (int i = 0; i < nt; i++) if (rs[i] >= 0 && rs[i] < (int64_t)nn) per[rs[i]]++;

        printf("[n0] reparto de filas (bloque -> nodo que lo calculo):\n");
        for (int i = 0; i < nt && i < 16; i++) printf("      filas %d-%d -> nodo %lld\n", r0s[i], r1s[i] - 1, (long long)rs[i]);
        if (nt > 16) printf("      ... (%d tareas en total)\n", nt);

        double *E = malloc((size_t)lim * lim * sizeof(double));
        if (E) for (int i = 0; i < lim; i++) for (int j = 0; j < lim; j++) E[i * lim + j] = ref_c(N, i, j);
        print_mat("C calculada por las tareas (leida del DSM)", Cm, N, lim, N);
        if (E) { print_mat("C esperada (calculo secuencial)", E, lim, lim, N); free(E); }

        for (int i = 0; i < N && !bad; i++) for (int j = 0; j < N; j++) {
            if (dsm_fabs(ref_c(N, i, j) - Cm[(size_t)i * N + j]) > 1e-9) { fprintf(stderr, "FALLO C[%d][%d]\n", i, j); bad = 1; break; }
        }
        printf("[n0] app_tasks N=%d, %d tareas en %u nodos (", N, nt, nn);
        for (uint32_t n = 0; n < nn; n++) printf("%snodo%u=%d", n ? " " : "", n, per[n]);
        printf("), %llu ms: %s\n", (unsigned long long)ms, bad ? "FALLO" : "PASS");
    }
    if (me == 0) dsm_write_html("reporte_matrices.html");
    if (dsm_barrier(1, nn) != DSM_OK) return 2;
    dsm_shutdown();
    return bad;
}
