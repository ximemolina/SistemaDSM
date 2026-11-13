#define _GNU_SOURCE
#include "internal.h"
#include <stdio.h>
#include <string.h>

static dsm_config_t C;

uint64_t cluster_splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

dsm_err_t cluster_init(const dsm_config_t *cfg) { C = *cfg; return DSM_OK; }
node_id_t cluster_self_id(void) { return C.self_id; }
bool cluster_is_alive(node_id_t id) { return id < C.max_nodes && C.slots[id].alive; }

bool cluster_get_node(node_id_t id, node_info_t *out) {
    if (id >= C.max_nodes) return false;
    *out = C.slots[id];
    return true;
}

node_id_t cluster_home_of(page_id_t page) {
    return (node_id_t)(cluster_splitmix64(page ^ C.hash_seed) % C.max_nodes);
}

node_id_t cluster_initial_owner(page_id_t page) {
    return (node_id_t)(((uint64_t)page * C.page_size) / C.mem_per_node);
}

node_id_t cluster_barrier_coord(uint64_t id) {
    return (node_id_t)(cluster_splitmix64(id ^ C.hash_seed) % C.max_nodes);
}
