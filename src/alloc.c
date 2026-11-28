/*
 * alloc.c (AMBAS): asignacion global DESCENTRALIZADA (sin coordinador unico).
 *
 *  - Cada nodo es la autoridad de SU slot [id*mem, (id+1)*mem): lleva un mapa de paginas
 *    libres/ocupadas (used[]). Reservar o liberar un rango es atomico en el dueno del slot.
 *  - La tabla de asignaciones esta REPLICADA en todos los nodos. Cada alloc/free se anuncia
 *    por broadcast (MSG_ALLOC_ANNOUNCE), asi cualquier nodo sabe que espacio esta libre u ocupado.
 *  - Para asignar, el nodo calcula un hueco con su replica (prefiere su propio slot), reserva
 *    cada trozo en el dueno correspondiente (RPC) y, si alguien se le adelanto, reintenta.
 *    Un rango puede cruzar slots: se reserva trozo a trozo y se deshace si falla uno.
 *  - Los nombres (dsm_alloc_named) se serializan en un "home" por hash del nombre
 *    (como barrier/lock): no hay un nodo central, cada nombre tiene el suyo.
 *  - Un nodo que arranca tarde llama alloc_sync() y copia la tabla de sus pares.
 *
 * Contratos de hilos: on_alloc_request (MSG_POOL) solo hace operaciones locales, nunca espera red.
 * on_alloc_name (MSG_POOL_WAIT) puede esperar RPCs; sus respuestas las atienden POOL e INLINE.
 */
#define _GNU_SOURCE
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <endian.h>
#include <arpa/inet.h>

#define MAX_ALLOCS 2048
#define LIST_CHUNK 32
#define MAX_TRIES  8

typedef struct __attribute__((packed)) { uint8_t kind; uint32_t pid; uint64_t size; uint64_t addr; char name[DSM_NAME_LEN]; } alloc_req_t;
typedef struct __attribute__((packed)) { int32_t err; uint64_t addr; uint64_t size; } alloc_rep_t;
typedef struct __attribute__((packed)) { uint32_t node, pid, seq; uint8_t live; uint64_t addr, size; char name[DSM_NAME_LEN]; } alloc_wire_t;
/* AK_RESERVE / AK_RELEASE: addr = desplazamiento global (bytes desde global_base_addr), size = largo. */
enum { AK_RESERVE = 0, AK_RELEASE = 1, AK_LIST = 2 };

static const dsm_config_t *C;
static uint64_t mem, ps, slot_pages;
static uint8_t *used;                                   /* un byte por pagina de MI slot: la autoridad */
static pthread_mutex_t alloc_mu = PTHREAD_MUTEX_INITIALIZER;   /* protege used[] */
static alloc_entry_t atab[MAX_ALLOCS];                  /* replica de la tabla */
static unsigned acount;
static pthread_mutex_t tab_mu = PTHREAD_MUTEX_INITIALIZER;     /* protege atab[]; nunca se mantiene durante red */
static pthread_mutex_t name_mu = PTHREAD_MUTEX_INITIALIZER;    /* serializa altas con nombre en este nodo (solo POOL_WAIT) */

/* ---------------- mapa local de paginas (autoridad de mi slot) ---------------- */
static int local_reserve(uint64_t off, uint64_t len) {
    uint64_t base = (uint64_t)C->self_id * mem;
    if (!len || off < base || off + len > base + mem) return -1;
    uint64_t p0 = (off - base) / ps, n = len / ps; int rc = 0;
    pthread_mutex_lock(&alloc_mu);
    for (uint64_t i = 0; i < n; i++) if (used[p0 + i]) { rc = -1; break; }
    if (!rc) memset(used + p0, 1, n);
    pthread_mutex_unlock(&alloc_mu);
    return rc;
}
static void local_release(uint64_t off, uint64_t len) {
    uint64_t base = (uint64_t)C->self_id * mem;
    if (!len || off < base || off + len > base + mem) return;
    pthread_mutex_lock(&alloc_mu);
    memset(used + (off - base) / ps, 0, len / ps);
    pthread_mutex_unlock(&alloc_mu);
}
static void mark_overlap(const alloc_entry_t *e) {      /* reconstruye used[] a partir de la tabla sincronizada */
    uint64_t base = (uint64_t)C->self_id * mem, lo = base, hi = base + mem;
    uint64_t s = e->addr - C->global_base_addr, t = s + e->size;
    if (e->addr < C->global_base_addr) return;
    if (s < lo) s = lo;
    if (t > hi) t = hi;
    if (s >= t) return;
    pthread_mutex_lock(&alloc_mu);
    memset(used + (s - base) / ps, 1, (t - s) / ps);
    pthread_mutex_unlock(&alloc_mu);
}

/* ---------------- reservar / liberar en el dueno de cada slot ---------------- */
static int piece_rpc(node_id_t s, uint8_t kind, uint64_t off, uint64_t len) {
    alloc_req_t rq; memset(&rq, 0, sizeof rq);
    rq.kind = kind; rq.addr = htobe64(off); rq.size = htobe64(len);
    alloc_rep_t rep; uint32_t rl = sizeof rep;
    if (net_rpc_call(s, MSG_ALLOC_REQUEST, 0, 0, &rq, sizeof rq, &rep, &rl, C->request_timeout_ms) != DSM_OK || rl != sizeof rep) return -1;
    return ntohl((uint32_t)rep.err) ? -1 : 0;
}
static int reserve_piece(node_id_t s, uint64_t off, uint64_t len) {
    return s == C->self_id ? local_reserve(off, len) : piece_rpc(s, AK_RESERVE, off, len);
}
static void release_piece(node_id_t s, uint64_t off, uint64_t len) {
    if (s == C->self_id) local_release(off, len); else (void)piece_rpc(s, AK_RELEASE, off, len);
}
static void release_range(uint64_t off, uint64_t size) {
    uint64_t end = off + size, cur = off;
    while (cur < end) {
        node_id_t s = (node_id_t)(cur / mem);
        uint64_t pe = (uint64_t)(s + 1) * mem; if (pe > end) pe = end;
        release_piece(s, cur, pe - cur);
        cur = pe;
    }
}
static int reserve_range(uint64_t off, uint64_t size) {  /* todo o nada */
    uint64_t end = off + size, cur = off;
    while (cur < end) {
        node_id_t s = (node_id_t)(cur / mem);
        uint64_t pe = (uint64_t)(s + 1) * mem; if (pe > end) pe = end;
        if (reserve_piece(s, cur, pe - cur) != 0) { if (cur > off) release_range(off, cur - off); return -1; }
        cur = pe;
    }
    return 0;
}

/* ---------------- tabla replicada ---------------- */
static void table_apply(const alloc_entry_t *e) {
    pthread_mutex_lock(&tab_mu);
    if (!e->live) {
        for (unsigned i = 0; i < acount; i++) if (atab[i].live && atab[i].addr == e->addr) { atab[i].live = 0; break; }
    } else {
        int dup = 0;
        for (unsigned i = 0; i < acount; i++) if (atab[i].live && atab[i].addr == e->addr) { dup = 1; break; }
        if (!dup) {
            if (acount >= MAX_ALLOCS) {                  /* compacta: descarta el historial de las ya liberadas */
                unsigned k = 0;
                for (unsigned i = 0; i < acount; i++) if (atab[i].live) atab[k++] = atab[i];
                acount = k;
            }
            if (acount < MAX_ALLOCS) atab[acount++] = *e; else DSM_LOG("tabla de asignaciones llena");
        }
    }
    pthread_mutex_unlock(&tab_mu);
}
static void to_wire(const alloc_entry_t *e, alloc_wire_t *w) {
    memset(w, 0, sizeof *w);
    w->node = htonl(e->node); w->pid = htonl(e->pid); w->seq = htonl(e->seq); w->live = e->live;
    w->addr = htobe64(e->addr); w->size = htobe64(e->size); memcpy(w->name, e->name, DSM_NAME_LEN);
}
static void from_wire(const alloc_wire_t *w, alloc_entry_t *e) {
    memset(e, 0, sizeof *e);
    e->node = ntohl(w->node); e->pid = ntohl(w->pid); e->seq = ntohl(w->seq); e->live = w->live;
    e->addr = be64toh(w->addr); e->size = be64toh(w->size); memcpy(e->name, w->name, DSM_NAME_LEN);
    e->name[DSM_NAME_LEN - 1] = 0;
}
static void announce(const alloc_entry_t *e) {          /* aplica local y avisa a los demas nodos vivos */
    table_apply(e);
    alloc_wire_t w; to_wire(e, &w);
    for (node_id_t n = 0; n < C->max_nodes; n++) {
        if (n == C->self_id || !cluster_is_alive(n)) continue;
        msg_header_t h = { .type = MSG_ALLOC_ANNOUNCE, .payload_len = sizeof w };
        net_send(n, &h, &w);
    }
}
static void on_announce(const msg_header_t *h, const void *payload) {      /* INLINE: orden FIFO por emisor */
    if (h->payload_len != sizeof(alloc_wire_t)) return;
    alloc_wire_t w; memcpy(&w, payload, sizeof w);
    alloc_entry_t e; from_wire(&w, &e);
    table_apply(&e);
}

/* ---------------- busqueda de hueco con la replica ---------------- */
typedef struct { uint64_t s, e; } iv_t;
static int iv_cmp(const void *a, const void *b) {
    uint64_t x = ((const iv_t *)a)->s, y = ((const iv_t *)b)->s;
    return x < y ? -1 : x > y;
}
/* Devuelve el desplazamiento global de un hueco libre de 'size' bytes, o -1.
 * Ocupado = asignaciones vivas + slots de nodos caidos. Prefiere huecos desde el slot del solicitante. */
static int64_t find_gap(node_id_t req, uint64_t size) {
    iv_t iv[MAX_ALLOCS + DSM_MAX_NODES]; unsigned n = 0;
    pthread_mutex_lock(&tab_mu);
    for (unsigned i = 0; i < acount; i++)
        if (atab[i].live && atab[i].addr >= C->global_base_addr) {
            iv[n].s = atab[i].addr - C->global_base_addr; iv[n].e = iv[n].s + atab[i].size; n++;
        }
    pthread_mutex_unlock(&tab_mu);
    for (uint32_t s = 0; s < C->max_nodes; s++)
        if (!cluster_is_alive(s)) { iv[n].s = (uint64_t)s * mem; iv[n].e = (uint64_t)(s + 1) * mem; n++; }
    qsort(iv, n, sizeof iv[0], iv_cmp);
    uint64_t total = (uint64_t)C->max_nodes * mem, pref = (uint64_t)req * mem;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t cur = 0;
        for (unsigned i = 0; i <= n; i++) {
            uint64_t gend = i < n ? iv[i].s : total;
            if (gend > cur) {
                uint64_t cs = (pass == 0 && cur < pref) ? pref : cur;
                if (cs < gend && gend - cs >= size) return (int64_t)cs;
            }
            if (i < n && iv[i].e > cur) cur = iv[i].e;
        }
    }
    return -1;
}

static uint64_t round_up(uint64_t size) { return (size + ps - 1) / ps * ps; }

static dsm_err_t do_alloc(node_id_t req, uint64_t size, const char *name, uint32_t pid, uint64_t *addr_out, uint64_t *size_out) {
    size = round_up(size);
    if (!size) return DSM_ERR_NOMEM;
    for (int t = 0; t < MAX_TRIES; t++) {
        int64_t off = find_gap(req, size);
        if (off < 0) return DSM_ERR_NOMEM;
        if (reserve_range((uint64_t)off, size) == 0) {
            alloc_entry_t e; memset(&e, 0, sizeof e);
            e.node = req; e.pid = pid; e.live = 1; e.size = size; e.addr = C->global_base_addr + (uint64_t)off;
            snprintf(e.name, DSM_NAME_LEN, "%s", name ? name : "");
            pthread_mutex_lock(&tab_mu);
            for (unsigned i = 0; i < acount; i++) if (atab[i].node == req && atab[i].pid == pid) e.seq++;
            e.seq++;
            pthread_mutex_unlock(&tab_mu);
            announce(&e);
            *addr_out = e.addr; *size_out = size;
            return DSM_OK;
        }
        usleep(2000 * (t + 1));                          /* alguien se nos adelanto: su anuncio llegara; reintentar */
    }
    return DSM_ERR_NOMEM;
}

/* ---------------- handlers de red ---------------- */
static void reply(const msg_header_t *h, const void *p, uint32_t len) {
    msg_header_t r = { .type = MSG_ALLOC_REPLY, .req_id = h->req_id, .payload_len = len };
    net_send(h->src_node, &r, len ? p : NULL);
}

static void on_alloc_request(const msg_header_t *h, const void *payload) {   /* MSG_POOL: solo operaciones locales */
    if (h->payload_len != sizeof(alloc_req_t)) return;
    alloc_req_t rq; memcpy(&rq, payload, sizeof rq);
    uint64_t a = be64toh(rq.addr), s = be64toh(rq.size);
    if (rq.kind == AK_RESERVE || rq.kind == AK_RELEASE) {
        alloc_rep_t rep = { 0 };
        if (!s || a % ps || s % ps) rep.err = htonl(DSM_ERR_BAD_ARG);
        else if (rq.kind == AK_RESERVE) { if (local_reserve(a, s) != 0) rep.err = htonl(DSM_ERR_NOMEM); }
        else local_release(a, s);
        reply(h, &rep, sizeof rep);
    } else {                                              /* AK_LIST: trozo de mi replica (addr = indice inicial) */
        char out[12 + LIST_CHUNK * sizeof(alloc_wire_t)];
        uint32_t cnt = 0; uint64_t i = a;
        pthread_mutex_lock(&tab_mu);
        for (; i < acount && cnt < LIST_CHUNK; i++, cnt++) { alloc_wire_t w; to_wire(&atab[i], &w); memcpy(out + 12 + cnt * sizeof w, &w, sizeof w); }
        uint32_t c = htonl(cnt); uint64_t nx = htobe64(i < acount ? i : UINT64_MAX);
        pthread_mutex_unlock(&tab_mu);
        memcpy(out, &c, 4); memcpy(out + 4, &nx, 8);
        reply(h, out, (uint32_t)(12 + cnt * sizeof(alloc_wire_t)));
    }
}

static void on_alloc_name(const msg_header_t *h, const void *payload) {      /* MSG_POOL_WAIT: home del nombre */
    if (h->payload_len != sizeof(alloc_req_t)) return;
    alloc_req_t rq; memcpy(&rq, payload, sizeof rq);
    rq.name[DSM_NAME_LEN - 1] = 0;
    uint32_t pid = ntohl(rq.pid); uint64_t want = round_up(be64toh(rq.size)), addr = 0, sz = 0;
    alloc_rep_t rep = { 0 }; int found = 0;
    pthread_mutex_lock(&name_mu);
    pthread_mutex_lock(&tab_mu);
    for (unsigned i = 0; i < acount; i++)
        if (atab[i].live && !strcmp(atab[i].name, rq.name)) { addr = atab[i].addr; sz = atab[i].size; found = 1; break; }
    pthread_mutex_unlock(&tab_mu);
    if (found) { if (want > sz) rep.err = htonl(DSM_ERR_BAD_ARG); }   /* mismo nombre: mismo puntero */
    else { dsm_err_t rc = do_alloc(h->src_node, want, rq.name, pid, &addr, &sz); if (rc != DSM_OK) rep.err = htonl(rc); }
    pthread_mutex_unlock(&name_mu);
    if (!rep.err) { rep.addr = htobe64(addr); rep.size = htobe64(sz); }
    reply(h, &rep, sizeof rep);
}

/* ---------------- API publica ---------------- */
static node_id_t name_home(const char *name) {           /* determinista; si el home esta caido, el siguiente vivo */
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    uint32_t n = C->max_nodes, st = (uint32_t)(cluster_splitmix64(h ^ C->hash_seed) % n);
    for (uint32_t i = 0; i < n; i++) { node_id_t nd = (st + i) % n; if (cluster_is_alive(nd)) return nd; }
    return C->self_id;
}

void *dsm_alloc_named(const char *name, size_t size) {
    uint64_t addr = 0, sz = 0; dsm_err_t rc;
    if (!name || !*name) rc = do_alloc(C->self_id, size, "", (uint32_t)getpid(), &addr, &sz);   /* sin red de coordinacion */
    else {
        alloc_req_t rq; memset(&rq, 0, sizeof rq);
        rq.pid = htonl((uint32_t)getpid()); rq.size = htobe64(size); snprintf(rq.name, DSM_NAME_LEN, "%s", name);
        rc = DSM_ERR_NODE_DOWN;
        for (int t = 0; t < 2; t++) {                    /* 2o intento: recalcula el home por si cayo */
            alloc_rep_t rep; uint32_t rl = sizeof rep;
            dsm_err_t r = net_rpc_call(name_home(name), MSG_ALLOC_NAME, 0, 0, &rq, sizeof rq, &rep, &rl, C->request_timeout_ms * 4);
            if (r == DSM_OK) {
                if (rl != sizeof rep) r = DSM_ERR_PROTO;
                else if (ntohl((uint32_t)rep.err)) r = (dsm_err_t)ntohl((uint32_t)rep.err);
                else { addr = be64toh(rep.addr); sz = be64toh(rep.size); }
                rc = r; break;
            }
            rc = r;
        }
    }
    dsm_set_error(rc);
    return rc == DSM_OK ? (void *)(uintptr_t)addr : NULL;
}
void *dsm_alloc(size_t size) { return dsm_alloc_named(NULL, size); }

/* Libera: marca muerta la entrada (evita doble free), devuelve las paginas a su(s) dueno(s) y anuncia.
 * La memoria reutilizada NO se pone a cero. */
dsm_err_t dsm_free(void *ptr) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr; alloc_entry_t dead; int found = 0;
    pthread_mutex_lock(&tab_mu);
    for (unsigned i = 0; i < acount; i++)
        if (atab[i].live && atab[i].addr == addr) { atab[i].live = 0; dead = atab[i]; found = 1; break; }
    pthread_mutex_unlock(&tab_mu);
    if (!found) return DSM_ERR_BAD_ARG;
    release_range(addr - C->global_base_addr, dead.size);
    dead.live = 0;
    announce(&dead);
    return DSM_OK;
}

dsm_err_t dsm_alloc_list(uint64_t first, alloc_entry_t *out, uint32_t max, uint32_t *count, uint64_t *next) {
    uint32_t c = 0; uint64_t i = first;
    pthread_mutex_lock(&tab_mu);
    for (; i < acount && c < max; i++, c++) out[c] = atab[i];
    *count = c; *next = i < acount ? i : UINT64_MAX;
    pthread_mutex_unlock(&tab_mu);
    return DSM_OK;
}

/* ---------------- arranque ---------------- */
dsm_err_t alloc_init(const dsm_config_t *cfg) {
    C = cfg; mem = cfg->mem_per_node; ps = cfg->page_size; slot_pages = mem / ps;
    used = calloc(slot_pages, 1);
    if (!used) return DSM_ERR_NOMEM;
    net_register(MSG_ALLOC_REQUEST,  on_alloc_request, MSG_POOL);
    net_register(MSG_ALLOC_NAME,     on_alloc_name,    MSG_POOL_WAIT);
    net_register(MSG_ALLOC_ANNOUNCE, on_announce,      MSG_INLINE);
    return DSM_OK;
}

/* Copia la tabla de cada par vivo (idempotente) y reconstruye el mapa de mi slot.
 * Llamar despues de cluster_join(): un nodo que entra tarde o reinicia se pone al dia. */
dsm_err_t alloc_sync(void) {
    dsm_err_t last = DSM_OK;
    for (node_id_t p = 0; p < C->max_nodes; p++) {
        if (p == C->self_id || !cluster_is_alive(p)) continue;
        uint64_t first = 0;
        while (first != UINT64_MAX) {
            alloc_req_t rq; memset(&rq, 0, sizeof rq); rq.kind = AK_LIST; rq.addr = htobe64(first);
            char buf[12 + LIST_CHUNK * sizeof(alloc_wire_t)]; uint32_t rl = sizeof buf;
            dsm_err_t rc = net_rpc_call(p, MSG_ALLOC_REQUEST, 0, 0, &rq, sizeof rq, buf, &rl, C->request_timeout_ms);
            if (rc != DSM_OK || rl < 12) { last = rc != DSM_OK ? rc : DSM_ERR_PROTO; break; }
            uint32_t c; uint64_t nx; memcpy(&c, buf, 4); memcpy(&nx, buf + 4, 8); c = ntohl(c); nx = be64toh(nx);
            for (uint32_t i = 0; i < c && 12 + (i + 1) * sizeof(alloc_wire_t) <= rl; i++) {
                alloc_wire_t w; memcpy(&w, buf + 12 + i * sizeof w, sizeof w);
                alloc_entry_t e; from_wire(&w, &e);
                if (e.live) { table_apply(&e); mark_overlap(&e); }
            }
            first = nx;
        }
    }
    return last;
}
