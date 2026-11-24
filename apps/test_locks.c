/* Locks distribuidos: exclusion mutua, visibilidad de lo escrito dentro de la seccion critica e independencia entre locks.
 *  L1 contador compartido incrementado por todos los nodos bajo lock => total exacto
 *  L2 invariante a == b dentro de la seccion critica (si el lock no entrega el dato mas reciente, se rompe)
 *  L3 dos locks distintos protegiendo paginas distintas al mismo tiempo
 */
#include "test_util.h"
#define ITER 300
#define LK_CNT 20
#define LK_INV 21
#define LK_CNT2 22

int main(int argc, char **argv) {
    t_init(argc, argv);
    const size_t ps = config_get()->page_size, w = ps / sizeof(uint64_t);
    volatile uint64_t *base = dsm_alloc_named("lk_data", 3 * ps);
    if (!base) { fprintf(stderr, "dsm_alloc fallo\n"); return 1; }
    volatile uint64_t *cnt = base, *a = base + w, *b = base + w + 1, *cnt2 = base + 2 * w;
    if (g_me == 0) { *cnt = 0; *a = 0; *b = 0; *cnt2 = 0; }
    sync_all();

    uint64_t t0 = dsm_now_ms();
    for (int i = 0; i < ITER; i++) {
        if (dsm_lock(LK_CNT) != DSM_OK) { fprintf(stderr, "lock fallo\n"); return 3; }
        (*cnt)++;
        if (dsm_unlock(LK_CNT) != DSM_OK) { fprintf(stderr, "unlock fallo\n"); return 3; }
    }
    sync_all();
    CHECK(*cnt == (uint64_t)g_nn * ITER, "L1 contador=%llu esperado=%llu", (unsigned long long)*cnt, (unsigned long long)g_nn * ITER);
    sync_all();
    END_PHASE("L1 contador bajo lock");

    for (int i = 0; i < ITER; i++) {
        if (dsm_lock(LK_INV) != DSM_OK) return 3;
        uint64_t va = *a, vb = *b;
        CHECK(va == vb, "L2 invariante roto dentro de la seccion critica: a=%llu b=%llu", (unsigned long long)va, (unsigned long long)vb);
        *a = va + 1;
        if (i % 50 == 0) usleep(1000);                         /* ventana para que otro nodo intente entrar */
        *b = vb + 1;
        if (dsm_unlock(LK_INV) != DSM_OK) return 3;
    }
    sync_all();
    CHECK(*a == (uint64_t)g_nn * ITER && *b == *a, "L2 final a=%llu b=%llu", (unsigned long long)*a, (unsigned long long)*b);
    sync_all();
    END_PHASE("L2 invariante en seccion critica");

    for (int i = 0; i < ITER; i++) {
        if (dsm_lock(LK_CNT) != DSM_OK) return 3;  (*cnt)++;  if (dsm_unlock(LK_CNT) != DSM_OK) return 3;
        if (dsm_lock(LK_CNT2) != DSM_OK) return 3; (*cnt2) += 2; if (dsm_unlock(LK_CNT2) != DSM_OK) return 3;
    }
    sync_all();
    CHECK(*cnt == 2 * (uint64_t)g_nn * ITER, "L3 cnt=%llu", (unsigned long long)*cnt);
    CHECK(*cnt2 == 2 * (uint64_t)g_nn * ITER, "L3 cnt2=%llu", (unsigned long long)*cnt2);
    sync_all();
    END_PHASE("L3 dos locks independientes");

    if (g_me == 0) printf("[n0] %d incrementos x %u nodos x 3 fases en %llu ms\n", ITER, g_nn, (unsigned long long)(dsm_now_ms() - t0));
    return t_done();
}
