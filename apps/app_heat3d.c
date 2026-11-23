#include "../src/internal.h"
#include "../lib/libdsm_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define NY 8
#define BAR_INIT 10
#define BAR_CALC 100
#define BAR_LOCK 200
#define BAR_VERIFY 300
#define BAR_DONE 500

static double analytic(int x, int y, uint64_t z) {
    return 20.0 + 0.01 * (double)x + 0.02 * (double)y + 0.03 * (double)z;
}

static void print_stats(uint32_t me, uint32_t nn, const char *ip) {
    dsm_stats_t local;
    if (dsm_stats(&local) == DSM_OK)
        printf("[n%u ip=%s] stats_local: I=%llu S=%llu M=%llu faultR=%llu faultW=%llu rx=%llu inv=%llu fetch=%llu punch=%llu txMsg=%llu txBytes=%llu\n",
               me, ip,
               (unsigned long long)local.v[ST_PAGES_I], (unsigned long long)local.v[ST_PAGES_S],
               (unsigned long long)local.v[ST_PAGES_M], (unsigned long long)local.v[ST_FAULT_R],
               (unsigned long long)local.v[ST_FAULT_W], (unsigned long long)local.v[ST_PAGES_RX],
               (unsigned long long)local.v[ST_INVALIDATIONS], (unsigned long long)local.v[ST_FETCHES],
               (unsigned long long)local.v[ST_PUNCHED], (unsigned long long)local.v[ST_TX_MSGS],
               (unsigned long long)local.v[ST_TX_BYTES]);
    if (me != 0) return;
    for (uint32_t n = 0; n < nn; n++) {
        dsm_stats_t s;
        if (dsm_stats_node(n, &s) != DSM_OK) { printf("[n0] stats n%u no disponibles\n", n); continue; }
        printf("[n0] stats n%u: I=%llu S=%llu M=%llu faultR=%llu faultW=%llu rx=%llu inv=%llu fetch=%llu punch=%llu txMsg=%llu txBytes=%llu\n",
               n,
               (unsigned long long)s.v[ST_PAGES_I], (unsigned long long)s.v[ST_PAGES_S],
               (unsigned long long)s.v[ST_PAGES_M], (unsigned long long)s.v[ST_FAULT_R],
               (unsigned long long)s.v[ST_FAULT_W], (unsigned long long)s.v[ST_PAGES_RX],
               (unsigned long long)s.v[ST_INVALIDATIONS], (unsigned long long)s.v[ST_FETCHES],
               (unsigned long long)s.v[ST_PUNCHED], (unsigned long long)s.v[ST_TX_MSGS],
               (unsigned long long)s.v[ST_TX_BYTES]);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: %s dsm.conf [NZ] [STEPS]\n", argv[0]); return 1; }
    uint64_t nz = argc > 2 ? strtoull(argv[2], NULL, 10) : 2048;
    uint32_t steps = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 8;

    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); return 1; }
    const dsm_config_t *c = config_get();
    uint32_t nn = c->max_nodes, me = cluster_self_id();
    node_info_t self_info;
    cluster_get_node(me, &self_info);
    const char *my_ip = self_info.ip;
    uint64_t plane_elems = c->page_size / sizeof(double);
    uint64_t nx = plane_elems / NY;
    if (nx <= 2 || nz <= nn + 2 || steps == 0) { fprintf(stderr, "parametros invalidos\n"); return 1; }
    if (nx * NY * sizeof(double) != c->page_size) { fprintf(stderr, "page_size no multiple de %zu\n", sizeof(double) * NY); return 1; }

    size_t buf_bytes = (size_t)(nz * c->page_size);
    double *cur = (double *)dsm_alloc_named("heat_cur", buf_bytes);
    double *nxt = (double *)dsm_alloc_named("heat_nxt", buf_bytes);
    double *ctrl = (double *)dsm_alloc_named("heat_ctrl", (size_t)c->page_size * 2);
    if (!cur || !nxt || !ctrl) { fprintf(stderr, "dsm_alloc fallo\n"); return 1; }

    volatile double *checksum = ctrl;
    volatile double *maxerr = ctrl + 1;
    volatile double *failflag = ctrl + 2;
    volatile double *verified = ctrl + c->page_size / sizeof(double);

    uint64_t chunk = (nz - 2 + nn - 1) / nn;
    uint64_t z0 = 1 + (uint64_t)me * chunk;
    uint64_t z1 = z0 + chunk;
    if (z1 > nz - 1) z1 = nz - 1;

    for (uint64_t z = me; z < nz; z += nn) {
        uint64_t base = z * plane_elems;
        for (uint64_t y = 0; y < NY; y++)
            for (uint64_t x = 0; x < nx; x++) {
                double v = analytic((int)x, (int)y, z);
                cur[base + y * nx + x] = v;
                nxt[base + y * nx + x] = v;
            }
    }
    if (me == 0) { *checksum = 0.0; *maxerr = 0.0; *failflag = 0.0; *verified = 0.0; }
    if (dsm_barrier(BAR_INIT, nn) != DSM_OK) return 2;

    printf("[n%u ip=%s] app_heat3d NX=%llu NY=%u NZ=%llu steps=%u nodos=%u memoria=%.2f MiB\n",
           me, my_ip, (unsigned long long)nx, NY, (unsigned long long)nz, steps, nn,
           (double)(2 * buf_bytes + 2 * c->page_size) / 1048576.0);

    for (uint32_t s = 1; s <= steps; s++) {
        if (me == 0) *checksum = 0.0;
        if (dsm_barrier(BAR_CALC + s, nn) != DSM_OK) return 2;
        double local_sum = 0.0;
        for (uint64_t z = z0; z < z1; z++) {
            uint64_t base = z * plane_elems;
            for (uint64_t y = 1; y < NY - 1; y++)
                for (uint64_t x = 1; x < nx - 1; x++) {
                    uint64_t i = base + y * nx + x;
                    double v = cur[i] + 0.125 * (cur[i - 1] + cur[i + 1] + cur[i - nx] +
                               cur[i + nx] + cur[i - plane_elems] + cur[i + plane_elems] - 6.0 * cur[i]);
                    nxt[i] = v;
                    local_sum += v;
                }
        }
        if (dsm_lock(7) != DSM_OK) return 3;
        *checksum += local_sum;
        if (dsm_unlock(7) != DSM_OK) return 3;
        if (dsm_barrier(BAR_LOCK + s, nn) != DSM_OK) return 2;
        if (me == 0) printf("[n0] step=%u suma=%.6f\n", s, *checksum);
        double *tmp = cur; cur = nxt; nxt = tmp;
    }

    double local_err = 0.0;
    for (uint64_t z = z0; z < z1; z++) {
        uint64_t base = z * plane_elems;
        for (uint64_t y = 0; y < NY; y++)
            for (uint64_t x = 0; x < nx; x++) {
                double e = dsm_fabs(cur[base + y * nx + x] - analytic((int)x, (int)y, z));
                if (e > local_err) local_err = e;
            }
    }
    if (dsm_lock(8) != DSM_OK) return 3;
    if (local_err > *maxerr) *maxerr = local_err;
    if (dsm_unlock(8) != DSM_OK) return 3;
    if (dsm_barrier(BAR_VERIFY, nn) != DSM_OK) return 2;

    int bad = 0;
    if (me == 0) {
        double err = *maxerr;
        if (err > 1e-9) bad = 1;
        printf("[n0] error maximo=%.12f manufacturado=%s\n", err, bad ? "FALLO" : "OK");
    }
    print_stats(me, nn, my_ip);
    if (me == 0) *failflag = (double)bad;
    if (dsm_barrier(BAR_DONE, nn) != DSM_OK) return 2;
    bad = *failflag != 0.0;
    dsm_shutdown();
    printf("[n%u ip=%s] RESULTADO: %s\n", me, my_ip, bad ? "FALLO" : "PASS");
    return bad;
}
