/* Coherencia MSI con >= 3 nodos. Cada fase prueba una transicion y verifica que ningun nodo lea datos viejos.
 *  F1  I->S   : varios nodos leen una pagina escrita por otro
 *  F2  S->I/M : escritor rotativo; todos los lectores deben ver el valor nuevo (invalidacion) y conservar el resto de la pagina
 *  F3  S->M   : upgrade de una pagina que el escritor ya tenia en S
 *  F4  M->S->M: el escritor se degrada al leer otro nodo, y luego vuelve a escribir
 *  F5  anillo : deteccion de lecturas viejas (cada paso exige el valor del paso actual, 100 pasos)
 *  F6  false sharing: todos escriben palabras distintas de la MISMA pagina sin lock
 *  F7  acceso que cruza el limite de pagina
 *  F8  stats  : el protocolo realmente genero fallos e invalidaciones
 */
#include "test_util.h"
#define NPG 4
#define ROUNDS 60
#define RING_STEPS 100
#define FS_STEPS 50

int main(int argc, char **argv) {
    t_init(argc, argv);
    const size_t ps = config_get()->page_size, w = ps / sizeof(uint64_t);
    if (g_nn < 3) fprintf(stderr, "[n%u] AVISO: se recomiendan >= 3 nodos para cubrir MSI\n", g_me);
    if (g_nn > w) { fprintf(stderr, "demasiados nodos para F6\n"); return 1; }

    volatile uint64_t *pg    = dsm_alloc_named("msi_pages", NPG * ps);
    volatile uint64_t *ring  = dsm_alloc_named("msi_ring", (size_t)g_nn * ps);
    volatile uint64_t *shr   = dsm_alloc_named("msi_false", ps);
    char *cross              = dsm_alloc_named("msi_cross", 2 * ps);
    if (!pg || !ring || !shr || !cross) { fprintf(stderr, "dsm_alloc fallo\n"); return 1; }

    /* F1 */
    if (g_me == 0) for (int p = 0; p < NPG; p++) pg[p * w] = 0x1000 + (uint64_t)p;
    sync_all();
    for (int p = 0; p < NPG; p++) CHECK(pg[p * w] == 0x1000 + (uint64_t)p, "F1 pagina %d: %llu", p, (unsigned long long)pg[p * w]);
    sync_all();
    END_PHASE("F1 lectura compartida (I->S)");

    /* F2 */
    for (int r = 0; r < ROUNDS; r++) {
        uint32_t writer = (uint32_t)r % g_nn; int p = r % NPG; uint64_t val = (uint64_t)(r + 1) * 1000003ULL;
        if (g_me == writer) pg[p * w + 3] = val;
        sync_all();
        CHECK(pg[p * w + 3] == val, "F2 ronda %d: leyo %llu, esperaba %llu (dato viejo)", r, (unsigned long long)pg[p * w + 3], (unsigned long long)val);
        CHECK(pg[p * w] == 0x1000 + (uint64_t)p, "F2 ronda %d: se perdio la palabra 0 de la pagina", r);
        sync_all();
    }
    END_PHASE("F2 escritor rotativo + invalidacion");

    /* F3 */
    if (g_me == 0) pg[1 * w + 5] = 7;
    sync_all();
    CHECK(pg[1 * w + 5] == 7, "F3 lectura inicial");          /* todos quedan en S */
    sync_all();
    if (g_me == g_nn - 1) pg[1 * w + 5] = 8;                  /* upgrade S->M */
    sync_all();
    CHECK(pg[1 * w + 5] == 8, "F3 tras upgrade: %llu", (unsigned long long)pg[1 * w + 5]);
    sync_all();
    END_PHASE("F3 upgrade S->M");

    /* F4 */
    uint32_t wr = 1 % g_nn;
    if (g_me == wr) pg[2 * w + 6] = 11;                       /* M */
    sync_all();
    if (g_me == 0) CHECK(pg[2 * w + 6] == 11, "F4 lector remoto ve 11");   /* escritor pasa a S */
    sync_all();
    if (g_me == wr) { CHECK(pg[2 * w + 6] == 11, "F4 el escritor conserva su dato"); pg[2 * w + 6] = 12; }
    sync_all();
    CHECK(pg[2 * w + 6] == 12, "F4 tras reescritura: %llu", (unsigned long long)pg[2 * w + 6]);
    sync_all();
    END_PHASE("F4 M->S->M");

    /* F5 */
    for (int s = 1; s <= RING_STEPS; s++) {
        ring[g_me * w] = (uint64_t)s * 131 + g_me;
        sync_all();
        for (uint32_t k = 0; k < g_nn; k++)
            CHECK(ring[k * w] == (uint64_t)s * 131 + k, "F5 paso %d: nodo %u dato viejo (%llu)", s, k, (unsigned long long)ring[k * w]);
        sync_all();
    }
    END_PHASE("F5 anillo (lecturas viejas)");

    /* F6 */
    for (int s = 1; s <= FS_STEPS; s++) {
        shr[g_me] = (uint64_t)s;
        sync_all();
        for (uint32_t k = 0; k < g_nn; k++) CHECK(shr[k] == (uint64_t)s, "F6 paso %d: palabra %u = %llu", s, k, (unsigned long long)shr[k]);
        sync_all();
    }
    END_PHASE("F6 false sharing (misma pagina)");

    /* F7 */
    unsigned char pat[64], got[64];
    for (int i = 0; i < 64; i++) pat[i] = (unsigned char)(i * 7 + 1);
    if (g_me == 0) memcpy(cross + ps - 32, pat, 64);
    sync_all();
    memcpy(got, cross + ps - 32, 64);
    CHECK(memcmp(got, pat, 64) == 0, "F7 contenido distinto al cruzar el limite de pagina");
    sync_all();
    END_PHASE("F7 acceso cruzando pagina");

    /* F8 */
    if (g_me == 0) {
        uint64_t fr = stat_total(ST_FAULT_R), fw = stat_total(ST_FAULT_W), inv = stat_total(ST_INVALIDATIONS), fe = stat_total(ST_FETCHES);
        printf("[n0] stats cluster: faultR=%llu faultW=%llu invalidaciones=%llu fetch=%llu\n",
               (unsigned long long)fr, (unsigned long long)fw, (unsigned long long)inv, (unsigned long long)fe);
        CHECK(fr > 0 && fw > 0, "F8 no hubo fallos de lectura/escritura");
        CHECK(inv > 0, "F8 no hubo invalidaciones");
    }
    END_PHASE("F8 estadisticas del protocolo");
    return t_done();
}
