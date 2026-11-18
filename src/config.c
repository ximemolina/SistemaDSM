#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

static dsm_config_t g_cfg;
static int g_loaded;

static uint64_t parse_num(const char *s) {
    char *end; uint64_t v = strtoull(s, &end, 0);
    switch (toupper((unsigned char)*end)) { case 'K': v <<= 10; break; case 'M': v <<= 20; break; case 'G': v <<= 30; break; default: break; }
    return v;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

dsm_err_t config_load(const char *path, dsm_config_t *out) {
    dsm_config_t c; memset(&c, 0, sizeof c);
    c.page_size = DSM_DEFAULT_PAGESZ; c.mem_per_node = 16u << 20; c.max_nodes = 2;
    c.global_base_addr = 0x700000000000ULL;
    c.worker_pool_size = 8; c.nowait_pool_size = 4;
    c.request_timeout_ms = 5000; c.request_retries = 2; c.connect_timeout_ms = 500;
    c.join_timeout_ms = 30000; c.barrier_timeout_ms = 60000;
    c.heartbeat_interval_ms = 1000; c.heartbeat_timeout_ms = 5000;
    c.min_hold_us = 200; c.heartbeat_port_offset = 1000; c.wait_all_nodes = 1; c.release_memory = 1;
    c.task_pool_size = 4; c.task_timeout_ms = 0; c.task_max_retries = 3;

    FILE *f = fopen(path, "r");
    if (!f) { perror("config_load"); return DSM_ERR_BAD_ARG; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *s = trim(line); if (!*s) continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0; char *k = trim(s), *v = trim(eq + 1);
#define U32(name) else if (!strcmp(k, #name)) c.name = (uint32_t)parse_num(v)
        if (!strcmp(k, "page_size")) c.page_size = parse_num(v);
        else if (!strcmp(k, "mem_per_node")) c.mem_per_node = parse_num(v);
        else if (!strcmp(k, "global_base_addr")) c.global_base_addr = parse_num(v);
        else if (!strcmp(k, "hash_seed")) c.hash_seed = parse_num(v);
        else if (!strcmp(k, "heartbeat_port_offset")) c.heartbeat_port_offset = (uint16_t)parse_num(v);
        else if (!strcmp(k, "wait_all_nodes")) c.wait_all_nodes = (uint8_t)parse_num(v);
        else if (!strcmp(k, "release_memory")) c.release_memory = (uint8_t)parse_num(v);
        U32(max_nodes); U32(worker_pool_size); U32(nowait_pool_size); U32(request_timeout_ms);
        U32(request_retries); U32(connect_timeout_ms); U32(join_timeout_ms); U32(barrier_timeout_ms);
        U32(task_pool_size); U32(task_timeout_ms); U32(task_max_retries); U32(heartbeat_interval_ms); U32(heartbeat_timeout_ms); U32(min_hold_us); U32(max_payload);
        else if (!strcmp(k, "node")) {
            unsigned id, port; char host[DSM_IP_LEN];
            if (sscanf(v, "%u %63s %u", &id, host, &port) != 3 || id >= DSM_MAX_NODES) {
                fprintf(stderr, "config: linea node invalida: %s\n", v); fclose(f); return DSM_ERR_BAD_ARG;
            }
            c.slots[id].node_id = id; snprintf(c.slots[id].ip, DSM_IP_LEN, "%s", host);
            c.slots[id].port = (uint16_t)port; c.num_nodes++;
        } else fprintf(stderr, "config: parametro desconocido '%s' (ignorado)\n", k);
    }
    fclose(f);

    long sys_ps = sysconf(_SC_PAGESIZE);
    if (c.page_size == 0 || (c.page_size & (c.page_size - 1)) || c.page_size % (size_t)sys_ps) {
        fprintf(stderr, "config: page_size debe ser potencia de 2 y multiplo de %ld\n", sys_ps); return DSM_ERR_BAD_ARG;
    }
    if (c.max_nodes == 0 || c.max_nodes > DSM_MAX_NODES || c.num_nodes != c.max_nodes) {
        fprintf(stderr, "config: max_nodes (%u) debe ser 1..%d y coincidir con las lineas node= (%u)\n", c.max_nodes, DSM_MAX_NODES, c.num_nodes);
        return DSM_ERR_BAD_ARG;
    }
    if (c.mem_per_node % c.page_size) { fprintf(stderr, "config: mem_per_node no multiplo de page_size\n"); return DSM_ERR_BAD_ARG; }
    if (!c.worker_pool_size) c.worker_pool_size = 1;
    if (!c.task_pool_size) c.task_pool_size = 1;
    if (c.nowait_pool_size < 2) c.nowait_pool_size = 2;     /* el pool sin espera nunca debe ser de 1 hilo */
    if (!c.max_payload) c.max_payload = (uint32_t)c.page_size + 4352;   /* cabe una pagina + tablas de stats */
    for (uint32_t i = 0; i < c.max_nodes; i++) {
        if (!c.slots[i].ip[0]) { fprintf(stderr, "config: falta node=%u\n", i); return DSM_ERR_BAD_ARG; }
        c.slots[i].mem_bytes = c.mem_per_node;
        c.slots[i].base_addr = c.global_base_addr + (uint64_t)i * c.mem_per_node;
    }
    const char *e = getenv("DSM_NODE_ID");
    if (!e) { fprintf(stderr, "config: falta variable de entorno DSM_NODE_ID\n"); return DSM_ERR_BAD_ARG; }
    c.self_id = (node_id_t)atoi(e);
    if (c.self_id >= c.max_nodes) { fprintf(stderr, "config: DSM_NODE_ID fuera de rango\n"); return DSM_ERR_BAD_ARG; }
    g_cfg = c; g_loaded = 1;
    if (out) *out = c;
    return DSM_OK;
}
const dsm_config_t *config_get(void) { return g_loaded ? &g_cfg : NULL; }