#include "../src/internal.h"
#include "../lib/libdsm_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define DAMP 0.85
#define EPS 1e-7
#define BAR_INIT 10
#define BAR_CALC 1000
#define BAR_LOCK 2000
#define BAR_CONV 3000
#define BAR_SUM 4000
#define BAR_DONE 9000

static double pref_total(uint64_t n);

static double pref(uint64_t d, uint64_t n) { return (double)((d % 97) + 1) / (49.0 * pref_total(n)); }

static double pref_total(uint64_t n) {
    uint64_t q = n / 97, r = n % 97;
    return (double)q * 4753.0 / 49.0 + (double)(r * (r + 1)) / (2.0 * 49.0);
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
    if (argc < 2) { fprintf(stderr, "uso: %s dsm.conf [N] [DEG] [ITER]\n", argv[0]); return 1; }
    uint64_t want_n = argc > 2 ? strtoull(argv[2], NULL, 10) : 131072;
    uint32_t deg = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 16;
    uint32_t max_iter = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 12;

    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); return 1; }
    const dsm_config_t *c = config_get();
    uint32_t nn = c->max_nodes, me = cluster_self_id();
    node_info_t self_info;
    cluster_get_node(me, &self_info);
    const char *my_ip = self_info.ip;
    uint64_t bytes_per_double_page = c->page_size / sizeof(double);
    uint64_t align = bytes_per_double_page;
    if (c->page_size / sizeof(uint32_t) > align) align = c->page_size / sizeof(uint32_t);
    align *= nn;
    uint64_t n = ((want_n + align - 1) / align) * align;
    if (!n || !deg || deg > 1024 || n % deg || (n / deg) * deg != n ||
        (n / deg) > UINT32_MAX / deg) {
        fprintf(stderr, "N/DEG invalido\n");
        return 1;
    }
    uint64_t edges = n * deg;
    uint64_t pitch = n / deg;

    size_t row_bytes = (size_t)((n + 1) * sizeof(uint32_t));
    size_t src_bytes = (size_t)(edges * sizeof(uint32_t));
    size_t vec_bytes = (size_t)(n * sizeof(double));
    uint32_t *row = (uint32_t *)dsm_alloc_named("pr_rowptr", row_bytes);
    uint32_t *src = (uint32_t *)dsm_alloc_named("pr_src", src_bytes);
    double *rank = (double *)dsm_alloc_named("pr_rank", vec_bytes);
    double *next = (double *)dsm_alloc_named("pr_next", vec_bytes);
    double *ctrl = (double *)dsm_alloc_named("pr_ctrl", (size_t)c->page_size * 2);
    if (!row || !src || !rank || !next || !ctrl) { fprintf(stderr, "dsm_alloc fallo\n"); return 1; }

    volatile double *delta_acc = ctrl;
    volatile double *sum_acc = ctrl + 1;
    volatile double *failflag = ctrl + 2;
    volatile double *conv = ctrl + c->page_size / sizeof(double);

    uint64_t rows_per_node = n / nn;
    uint64_t d0 = (uint64_t)me * rows_per_node;
    uint64_t d1 = d0 + rows_per_node;

    for (uint64_t d = d0; d < d1; d++) {
        row[d] = (uint32_t)(d * deg);
        for (uint32_t k = 0; k < deg; k++) src[d * deg + k] = (uint32_t)((d + (uint64_t)k * pitch) % n);
        rank[d] = 1.0 / (double)n;
        next[d] = 0.0;
    }
    if (d1 == n) row[n] = (uint32_t)edges;
    if (me == 0) { *delta_acc = 0.0; *sum_acc = 0.0; *failflag = 0.0; *conv = 0.0; }
    if (dsm_barrier(BAR_INIT, nn) != DSM_OK) return 2;

    printf("[n%u ip=%s] app_pagerank N=%llu DEG=%u E=%llu nodos=%u memoria=%.2f MiB\n",
           me, my_ip, (unsigned long long)n, deg, (unsigned long long)edges, nn,
           (double)(row_bytes + src_bytes + 2 * vec_bytes + 2 * c->page_size) / 1048576.0);

    uint64_t used_iter = max_iter;
    for (uint32_t it = 1; it <= max_iter; it++) {
        if (me == 0) { *delta_acc = 0.0; *sum_acc = 0.0; *conv = 0.0; }
        if (dsm_barrier(BAR_CALC + it, nn) != DSM_OK) return 2;
        double local_delta = 0.0;
        for (uint64_t d = d0; d < d1; d++) {
            uint64_t base = d * deg;
            double contrib = 0.0;
            for (uint32_t k = 0; k < deg; k++) contrib += rank[src[base + k]];
            double val = (1.0 - DAMP) * pref(d, n) + DAMP * contrib / (double)deg;
            next[d] = val;
            local_delta += dsm_fabs(val - rank[d]);
        }
        if (dsm_lock(5) != DSM_OK) return 3;
        *delta_acc += local_delta;
        if (dsm_unlock(5) != DSM_OK) return 3;
        if (dsm_barrier(BAR_LOCK + it, nn) != DSM_OK) return 2;
        if (me == 0) {
            double delta = *delta_acc;
            printf("[n0] iter=%u L1=%.9f\n", it, delta);
            if (delta < EPS || it == max_iter) *conv = 1.0;
        }
        if (dsm_barrier(BAR_CONV + it, nn) != DSM_OK) return 2;
        int stop = *conv != 0.0;
        double *tmp = rank; rank = next; next = tmp;
        if (stop) { used_iter = it; break; }
    }

    double local_sum = 0.0;
    for (uint64_t d = d0; d < d1; d++) local_sum += rank[d];
    if (dsm_lock(6) != DSM_OK) return 3;
    *sum_acc += local_sum;
    if (dsm_unlock(6) != DSM_OK) return 3;
    if (dsm_barrier(BAR_SUM, nn) != DSM_OK) return 2;

    int bad = 0;
    if (me == 0) {
        double expected = 1.0;
        double got = *sum_acc;
        for (uint64_t d = 0; d < 8; d++)
            if (row[d] != (uint32_t)(d * deg)) bad = 1;
        for (uint64_t d = 0; d < 4; d++)
            for (uint32_t k = 0; k < deg; k++)
                if (src[d * deg + k] != (uint32_t)((d + (uint64_t)k * pitch) % n)) bad = 1;
        if (dsm_fabs(got - expected) > 1e-6) bad = 1;
        printf("[n0] suma=%.9f esperada=%.9f iter=%llu estructura=OK: %s\n",
               got, expected, (unsigned long long)used_iter, bad ? "FALLO" : "OK");
    }
    print_stats(me, nn, my_ip);
    if (me == 0) *failflag = (double)bad;
    if (dsm_barrier(BAR_DONE, nn) != DSM_OK) return 2;
    bad = *failflag != 0.0;
    dsm_shutdown();
    printf("[n%u ip=%s] RESULTADO: %s\n", me, my_ip, bad ? "FALLO" : "PASS");
    return bad;
}
