/* stats.c (B): estadisticas, tabla de paginas local/global, asignaciones, salida texto y HTML. */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <endian.h>

typedef struct __attribute__((packed)) { uint8_t kind; uint64_t first; } stats_req_t;
typedef struct __attribute__((packed)) { uint64_t page; uint32_t owner; uint64_t copyset; uint8_t dirty; } snap_wire_t;
#define SNAP_CHUNK 128
static const dsm_config_t *C;

static void fill_local(node_stats_t *o) {
    memset(o, 0, sizeof *o);
    o->node = cluster_self_id(); o->alive = 1;
    uint64_t st[4]; pager_count_states(st);
    pager_stats_t ps; pager_get_stats(&ps);
    uint64_t tx, rx, txb; net_get_counters(&tx, &rx, &txb);
    o->v[ST_PAGES_I] = st[0]; o->v[ST_PAGES_S] = st[1]; o->v[ST_PAGES_M] = st[2]; o->v[ST_PAGES_PENDING] = st[3];
    o->v[ST_FAULT_R] = ps.faults_read; o->v[ST_FAULT_W] = ps.faults_write; o->v[ST_PAGES_RX] = ps.pages_received;
    o->v[ST_INVALIDATIONS] = ps.invalidations; o->v[ST_FETCHES] = ps.fetches_served; o->v[ST_FETCH_DELAYED] = ps.fetches_delayed;
    o->v[ST_DIR_ENTRIES] = dir_count(); o->v[ST_TX_MSGS] = tx; o->v[ST_RX_MSGS] = rx; o->v[ST_TX_BYTES] = txb;
    o->v[ST_PUNCHED] = ps.punched; o->v[ST_RETRIES] = ps.retries;
    tasks_counters(&o->v[ST_TASKS_SPAWNED], &o->v[ST_TASKS_EXECUTED]);
}

static void on_stats_request(const msg_header_t *h, const void *payload) {      /* POOL sin espera */
    if (h->payload_len != sizeof(stats_req_t)) return;
    stats_req_t rq; memcpy(&rq, payload, sizeof rq);
    char out[12 + SNAP_CHUNK * sizeof(snap_wire_t)]; uint32_t len = 0;
    if (rq.kind == 0) {
        node_stats_t s; fill_local(&s);
        uint64_t w[STAT_N]; for (int i = 0; i < STAT_N; i++) w[i] = htobe64(s.v[i]);
        memcpy(out, w, sizeof w); len = sizeof w;
    } else {
        dir_snap_t sn[SNAP_CHUNK]; page_id_t next;
        uint32_t n = dir_snapshot(be64toh(rq.first), sn, SNAP_CHUNK, &next);
        uint32_t c = htonl(n); uint64_t nx = htobe64(next);
        memcpy(out, &c, 4); memcpy(out + 4, &nx, 8);
        for (uint32_t i = 0; i < n; i++) {
            snap_wire_t w = { htobe64(sn[i].page), htonl(sn[i].owner), htobe64(sn[i].copyset), sn[i].dirty };
            memcpy(out + 12 + i * sizeof w, &w, sizeof w);
        }
        len = 12 + n * (uint32_t)sizeof(snap_wire_t);
    }
    msg_header_t r = { .type = MSG_STATS_REPLY, .req_id = h->req_id, .payload_len = len };
    net_send(h->src_node, &r, out);
}

dsm_err_t stats_init(void) { C = config_get(); net_register(MSG_STATS_REQUEST, on_stats_request, MSG_POOL); return DSM_OK; }

dsm_err_t dsm_stats(dsm_stats_t *out) { fill_local(out); return DSM_OK; }

dsm_err_t dsm_stats_node(node_id_t n, dsm_stats_t *out) {
    if (n == cluster_self_id()) return dsm_stats(out);
    if (n >= C->max_nodes) return DSM_ERR_BAD_ARG;
    stats_req_t rq = { 0, 0 }; uint64_t w[STAT_N]; uint32_t rl = sizeof w;
    dsm_err_t rc = net_rpc_call(n, MSG_STATS_REQUEST, 0, 0, &rq, sizeof rq, w, &rl, C->request_timeout_ms);
    if (rc != DSM_OK) return rc;
    if (rl != sizeof w) return DSM_ERR_PROTO;
    memset(out, 0, sizeof *out); out->node = n; out->alive = 1;
    for (int i = 0; i < STAT_N; i++) out->v[i] = be64toh(w[i]);
    return DSM_OK;
}

static const char *names[STAT_N] = { "paginas I", "paginas S", "paginas M", "paginas PENDING", "fallos lectura", "fallos escritura",
    "paginas recibidas", "invalidaciones", "fetch atendidos", "fetch retenidos", "entradas de directorio",
    "msgs enviados", "msgs recibidos", "bytes enviados", "paginas liberadas (punch)", "reintentos de PAGE_REQUEST", "tareas lanzadas", "tareas ejecutadas" };

void dsm_print_stats(FILE *f) {
    node_stats_t s[DSM_MAX_NODES]; int ok[DSM_MAX_NODES];
    fprintf(f, "%-28s", "metrica");
    for (node_id_t n = 0; n < C->max_nodes; n++) { ok[n] = cluster_is_alive(n) && dsm_stats_node(n, &s[n]) == DSM_OK; fprintf(f, " %12s%u", "nodo ", n); }
    fputc('\n', f);
    for (int i = 0; i < STAT_N; i++) {
        fprintf(f, "%-28s", names[i]);
        for (node_id_t n = 0; n < C->max_nodes; n++) { if (ok[n]) fprintf(f, " %13llu", (unsigned long long)s[n].v[i]); else fprintf(f, " %13s", "caido"); }
        fputc('\n', f);
    }
    uint64_t per_node_pages = C->mem_per_node / C->page_size;
    fprintf(f, "memoria aportada por nodo: %llu paginas (%llu KB); espacio global: %llu paginas\n",
            (unsigned long long)per_node_pages, (unsigned long long)(C->mem_per_node >> 10),
            (unsigned long long)per_node_pages * C->max_nodes);
}

void dsm_print_local_pages(FILE *f, unsigned max) { pager_dump_local(f, max); }

static dsm_err_t fetch_dir(node_id_t n, uint64_t first, dir_snap_t *out, uint32_t *cnt, uint64_t *next) {
    if (n == cluster_self_id()) { page_id_t nx; *cnt = dir_snapshot(first, out, SNAP_CHUNK, &nx); *next = nx; return DSM_OK; }
    stats_req_t rq = { 1, htobe64(first) }; char buf[12 + SNAP_CHUNK * sizeof(snap_wire_t)]; uint32_t rl = sizeof buf;
    dsm_err_t rc = net_rpc_call(n, MSG_STATS_REQUEST, 0, 0, &rq, sizeof rq, buf, &rl, C->request_timeout_ms);
    if (rc != DSM_OK) return rc;
    uint32_t c; uint64_t nx; memcpy(&c, buf, 4); memcpy(&nx, buf + 4, 8); c = ntohl(c); *next = be64toh(nx); *cnt = 0;
    for (uint32_t i = 0; i < c && i < SNAP_CHUNK && 12 + (i + 1) * sizeof(snap_wire_t) <= rl; i++) {
        snap_wire_t w; memcpy(&w, buf + 12 + i * sizeof w, sizeof w);
        out[i] = (dir_snap_t){ be64toh(w.page), ntohl(w.owner), be64toh(w.copyset), w.dirty }; (*cnt)++;
    }
    return DSM_OK;
}

void dsm_print_global_pages(FILE *f, unsigned max_per_node) {
    fprintf(f, "tabla de paginas global (directorio de cada home)\n%-8s %-10s %-6s %-6s %-6s %-10s\n", "home", "pagina", "owner", "M?", "contr.", "copyset");
    for (node_id_t n = 0; n < C->max_nodes; n++) {
        if (!cluster_is_alive(n)) { fprintf(f, "%-8u (nodo caido)\n", n); continue; }
        uint64_t first = 0; unsigned shown = 0, total = 0;
        while (first != UINT64_MAX) {
            dir_snap_t sn[SNAP_CHUNK]; uint32_t c; uint64_t nx;
            if (fetch_dir(n, first, sn, &c, &nx) != DSM_OK) { fprintf(f, "%-8u (sin respuesta)\n", n); break; }
            for (uint32_t i = 0; i < c; i++) {
                total++;
                if (shown < max_per_node) {
                    fprintf(f, "%-8u %-10llu %-6u %-6s %-6u 0x%llx\n", n, (unsigned long long)sn[i].page, sn[i].owner,
                            sn[i].dirty ? "si" : "no", cluster_initial_owner(sn[i].page), (unsigned long long)sn[i].copyset);
                    shown++;
                }
            }
            first = nx;
        }
        if (total > shown) fprintf(f, "%-8u ... %u entradas mas\n", n, total - shown);
    }
}

void dsm_print_allocs(FILE *f) {
    fprintf(f, "asignaciones (tabla replicada en todos los nodos)\n%-4s %-6s %-8s %-4s %-14s %-12s %-5s %s\n", "id", "nodo", "proceso", "orden", "direccion", "bytes", "viva", "nombre");
    uint64_t first = 0; unsigned id = 0;
    while (first != UINT64_MAX) {
        alloc_entry_t e[32]; uint32_t c; uint64_t nx;
        dsm_err_t rc = dsm_alloc_list(first, e, 32, &c, &nx);
        if (rc != DSM_OK) { fprintf(f, "(no se pudo leer la tabla de asignaciones: err %d)\n", rc); return; }
        for (uint32_t i = 0; i < c; i++, id++)
            fprintf(f, "%-4u %-6u %-8u %-4u 0x%-12llx %-12llu %-5s %s\n", id, e[i].node, e[i].pid, e[i].seq,
                    (unsigned long long)e[i].addr, (unsigned long long)e[i].size, e[i].live ? "si" : "no", e[i].name);
        first = nx;
    }
}

dsm_err_t dsm_write_html(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return DSM_ERR_IO;
    fprintf(f, "<!doctype html><html><head><meta charset=\"utf-8\"><title>DSM nodo %u</title>"
               "<style>body{font-family:sans-serif;margin:1.5em}table{border-collapse:collapse;margin-bottom:1.5em}"
               "td,th{border:1px solid #999;padding:3px 8px;text-align:right}th{background:#eee}h2{margin-top:1.2em}</style></head><body>",
            cluster_self_id());
    fprintf(f, "<h1>DSM - vista desde el nodo %u</h1>", cluster_self_id());
    fprintf(f, "<h2>Nodos</h2><table><tr><th>nodo</th><th>host</th><th>puerto</th><th>estado</th><th>tareas</th><th>carga</th></tr>");
    for (node_id_t n = 0; n < C->max_nodes; n++) {
        node_info_t ni; cluster_get_node(n, &ni);
        fprintf(f, "<tr><td>%u</td><td>%s</td><td>%u</td><td>%s</td><td>%u</td><td>%u.%02u</td></tr>", n, ni.ip, ni.port, ni.alive ? "vivo" : "caido", ni.tasks_running, ni.load100 / 100, ni.load100 % 100);
    }
    fprintf(f, "</table><h2>Estadisticas por nodo</h2><table><tr><th>metrica</th>");
    node_stats_t s[DSM_MAX_NODES]; int ok[DSM_MAX_NODES];
    for (node_id_t n = 0; n < C->max_nodes; n++) { ok[n] = cluster_is_alive(n) && dsm_stats_node(n, &s[n]) == DSM_OK; fprintf(f, "<th>nodo %u</th>", n); }
    fprintf(f, "</tr>");
    for (int i = 0; i < STAT_N; i++) {
        fprintf(f, "<tr><td style=\"text-align:left\">%s</td>", names[i]);
        for (node_id_t n = 0; n < C->max_nodes; n++) { if (ok[n]) fprintf(f, "<td>%llu</td>", (unsigned long long)s[n].v[i]); else fprintf(f, "<td>caido</td>"); }
        fprintf(f, "</tr>");
    }
    fprintf(f, "</table><h2>Asignaciones</h2><table><tr><th>id</th><th>nodo</th><th>proceso</th><th>orden</th><th>direccion</th><th>bytes</th><th>viva</th><th>nombre</th></tr>");
    uint64_t first = 0; unsigned id = 0;
    while (first != UINT64_MAX) {
        alloc_entry_t e[32]; uint32_t c; uint64_t nx;
        if (dsm_alloc_list(first, e, 32, &c, &nx) != DSM_OK) break;
        for (uint32_t i = 0; i < c; i++, id++)
            fprintf(f, "<tr><td>%u</td><td>%u</td><td>%u</td><td>%u</td><td>0x%llx</td><td>%llu</td><td>%s</td><td>%s</td></tr>", id, e[i].node, e[i].pid, e[i].seq,
                    (unsigned long long)e[i].addr, (unsigned long long)e[i].size, e[i].live ? "si" : "no", e[i].name);
        first = nx;
    }
    fprintf(f, "</table><h2>Tabla de paginas local (nodo %u)</h2><pre>", cluster_self_id());
    pager_dump_local(f, 200);
    fprintf(f, "</pre><h2>Tabla de paginas global (primeras 100 entradas por home)</h2><pre>");
    dsm_print_global_pages(f, 100);
    fprintf(f, "</pre></body></html>\n");
    fclose(f);
    return DSM_OK;
}
