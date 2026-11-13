#define _GNU_SOURCE
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

static dsm_config_t g_cfg;
static int g_loaded;

static uint64_t parse_num(const char *s) {
    char *end;
    uint64_t v = strtoull(s, &end, 0);
    switch (toupper((unsigned char)*end)) {
        case 'K': v <<= 10; break;
        case 'M': v <<= 20; break;
        case 'G': v <<= 30; break;
        default: break;
    }
    return v;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

dsm_err_t config_load(const char *path, dsm_config_t *out) {
    dsm_config_t c;
    memset(&c, 0, sizeof c);
    c.page_size = DSM_DEFAULT_PAGESZ;
    c.mem_per_node = 16u << 20;
    c.max_nodes = 2;
    c.global_base_addr = 0x700000000000ULL;
    c.worker_pool_size = 8;
    c.request_timeout_ms = 5000;
    c.barrier_timeout_ms = 60000;
    c.heartbeat_interval_ms = 1000;
    c.heartbeat_timeout_ms = 5000;
    c.hash_seed = 0;

    FILE *f = fopen(path, "r");
    if (!f) { perror("config_load"); return DSM_ERR_BAD_ARG; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *s = trim(line);
        if (!*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if      (!strcmp(k, "page_size"))             c.page_size = parse_num(v);
        else if (!strcmp(k, "mem_per_node"))          c.mem_per_node = parse_num(v);
        else if (!strcmp(k, "max_nodes"))             c.max_nodes = (uint32_t)parse_num(v);
        else if (!strcmp(k, "global_base_addr"))      c.global_base_addr = parse_num(v);
        else if (!strcmp(k, "worker_pool_size"))      c.worker_pool_size = (uint32_t)parse_num(v);
        else if (!strcmp(k, "request_timeout_ms"))    c.request_timeout_ms = (uint32_t)parse_num(v);
        else if (!strcmp(k, "barrier_timeout_ms"))    c.barrier_timeout_ms = (uint32_t)parse_num(v);
        else if (!strcmp(k, "heartbeat_interval_ms")) c.heartbeat_interval_ms = (uint32_t)parse_num(v);
        else if (!strcmp(k, "heartbeat_timeout_ms"))  c.heartbeat_timeout_ms = (uint32_t)parse_num(v);
        else if (!strcmp(k, "max_payload"))           c.max_payload = (uint32_t)parse_num(v);
        else if (!strcmp(k, "hash_seed"))             c.hash_seed = parse_num(v);
        else if (!strcmp(k, "node")) {
            unsigned id, port; char ip[DSM_IP_LEN];
            if (sscanf(v, "%u %45s %u", &id, ip, &port) != 3 || id >= DSM_MAX_NODES) {
                fprintf(stderr, "config: linea node invalida: %s\n", v);
                fclose(f); return DSM_ERR_BAD_ARG;
            }
            c.slots[id].node_id = id;
            snprintf(c.slots[id].ip, DSM_IP_LEN, "%s", ip);
            c.slots[id].port = (uint16_t)port;
            c.slots[id].alive = 1;
            c.num_nodes++;
        } else fprintf(stderr, "config: parametro desconocido '%s' (ignorado)\n", k);
    }
    fclose(f);

    long sys_ps = sysconf(_SC_PAGESIZE);
    if (c.page_size == 0 || (c.page_size & (c.page_size - 1)) || c.page_size % (size_t)sys_ps) {
        fprintf(stderr, "config: page_size debe ser potencia de 2 y multiplo de %ld\n", sys_ps);
        return DSM_ERR_BAD_ARG;
    }
    if (c.max_nodes == 0 || c.max_nodes > DSM_MAX_NODES || c.num_nodes != c.max_nodes) {
        fprintf(stderr, "config: max_nodes (%u) debe ser 1..%d y coincidir con las lineas node= (%u)\n",
                c.max_nodes, DSM_MAX_NODES, c.num_nodes);
        return DSM_ERR_BAD_ARG;
    }
    if (c.mem_per_node % c.page_size) { fprintf(stderr, "config: mem_per_node no multiplo de page_size\n"); return DSM_ERR_BAD_ARG; }
    if (!c.max_payload) c.max_payload = (uint32_t)c.page_size + 256;
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
