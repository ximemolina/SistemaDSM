/* Reserva de memoria compartida.
 *  A1 dsm_alloc_named devuelve la MISMA direccion en todos los nodos
 *  A2 nombres distintos no se solapan y el contenido no se mezcla
 *  A3 dsm_alloc sin nombre desde varios nodos: bloques propios que nadie pisa
 *  A4 pedir mas memoria de la que existe falla limpio (NULL) sin colgarse
 */
#include "test_util.h"
#define BLOCKS 8

int main(int argc, char **argv) {
    t_init(argc, argv);
    const size_t ps = config_get()->page_size, w = ps / sizeof(uint64_t);
    uint64_t *ptrs = dsm_alloc_named("al_ptrs", ps);
    unsigned char *pa = dsm_alloc_named("al_a", 3 * ps), *pb = dsm_alloc_named("al_b", 5 * ps);
    if (!ptrs || !pa || !pb) { fprintf(stderr, "dsm_alloc fallo\n"); return 1; }

    /* A1 */
    ptrs[g_me * 2] = (uint64_t)(uintptr_t)pa; ptrs[g_me * 2 + 1] = (uint64_t)(uintptr_t)pb;
    sync_all();
    for (uint32_t k = 0; k < g_nn; k++) {
        CHECK(ptrs[k * 2] == (uint64_t)(uintptr_t)pa, "A1 'al_a' difiere en nodo %u", k);
        CHECK(ptrs[k * 2 + 1] == (uint64_t)(uintptr_t)pb, "A1 'al_b' difiere en nodo %u", k);
    }
    CHECK(dsm_alloc_named("al_a", 3 * ps) == (void *)pa, "A1 repetir el nombre en el mismo nodo cambio la direccion");
    sync_all();
    END_PHASE("A1 misma direccion en todos los nodos");

    /* A2 */
    CHECK(pa + 3 * ps <= pb || pb + 5 * ps <= pa, "A2 los rangos de al_a y al_b se solapan");
    if (g_me == 0) { memset(pa, 0xAA, 3 * ps); memset(pb, 0x55, 5 * ps); }
    sync_all();
    for (size_t i = 0; i < 3 * ps; i++) if (pa[i] != 0xAA) { CHECK(0, "A2 al_a[%zu]=%02x", i, pa[i]); break; }
    for (size_t i = 0; i < 5 * ps; i++) if (pb[i] != 0x55) { CHECK(0, "A2 al_b[%zu]=%02x", i, pb[i]); break; }
    sync_all();
    END_PHASE("A2 regiones disjuntas");

    /* A3 */
    uint64_t *blk[BLOCKS];
    for (int i = 0; i < BLOCKS; i++) {
        blk[i] = dsm_alloc(ps);
        CHECK(blk[i] != NULL, "A3 dsm_alloc devolvio NULL (bloque %d)", i);
        if (blk[i]) for (size_t j = 0; j < w; j++) blk[i][j] = ((uint64_t)g_me << 32) | (uint64_t)i;
    }
    sync_all();
    for (int i = 0; i < BLOCKS; i++) if (blk[i])
        for (size_t j = 0; j < w; j++)
            if (blk[i][j] != (((uint64_t)g_me << 32) | (uint64_t)i)) { CHECK(0, "A3 bloque %d fue pisado por otro nodo", i); break; }
    sync_all();
    END_PHASE("A3 dsm_alloc concurrente sin solapes");

    /* A4 */
    void *huge = dsm_alloc((size_t)1 << 50);
    CHECK(huge == NULL, "A4 una reserva imposible no devolvio NULL");
    END_PHASE("A4 agotamiento de memoria");
    return t_done();
}
