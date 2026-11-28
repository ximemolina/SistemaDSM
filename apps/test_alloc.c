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

    /* A5: la tabla esta replicada: cada nodo ve las asignaciones de TODOS, no solo las de un coordinador */
    {
        int ok = 0;
        for (int tries = 0; tries < 300 && !ok; tries++) {       /* el anuncio por broadcast es asincrono */
            int named_a = 0, named_b = 0; unsigned blocks[DSM_MAX_NODES] = { 0 };
            uint64_t first = 0;
            while (first != UINT64_MAX) {
                alloc_entry_t e[32]; uint32_t c; uint64_t nx;
                if (dsm_alloc_list(first, e, 32, &c, &nx) != DSM_OK) break;
                for (uint32_t i = 0; i < c; i++) {
                    if (!e[i].live) continue;
                    if (!strcmp(e[i].name, "al_a") && e[i].addr == (uint64_t)(uintptr_t)pa && e[i].size == 3 * ps) named_a = 1;
                    if (!strcmp(e[i].name, "al_b") && e[i].addr == (uint64_t)(uintptr_t)pb && e[i].size == 5 * ps) named_b = 1;
                    if (!e[i].name[0] && e[i].size == ps && e[i].node < g_nn) blocks[e[i].node]++;
                }
                first = nx;
            }
            ok = named_a && named_b;
            for (uint32_t k = 0; k < g_nn; k++) if (blocks[k] < BLOCKS) ok = 0;
            if (!ok) usleep(10000);
        }
        CHECK(ok, "A5 la replica local no refleja las asignaciones de todos los nodos");
    }
    sync_all();
    END_PHASE("A5 tabla replicada en todos los nodos");

    /* A6: dsm_free devuelve el espacio: se reutiliza y un segundo free falla */
    {
        void *p1 = dsm_alloc(ps);
        CHECK(p1 != NULL, "A6 dsm_alloc devolvio NULL");
        CHECK(dsm_free(p1) == DSM_OK, "A6 dsm_free fallo");
        CHECK(dsm_free(p1) == DSM_ERR_BAD_ARG, "A6 el doble free no se detecto");
        void *p2 = dsm_alloc(ps);
        CHECK(p2 == p1, "A6 el espacio liberado no se reutilizo (%p vs %p)", p1, p2);
        CHECK(dsm_free(p2) == DSM_OK, "A6 segundo free fallo");
    }
    sync_all();
    END_PHASE("A6 free y reutilizacion");
