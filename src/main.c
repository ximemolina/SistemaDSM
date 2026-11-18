/* main.c: nodo DSM interactivo (consultas desde cualquier nodo). Uso: DSM_NODE_ID=n ./dsm_node dsm.conf [-q] */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: DSM_NODE_ID=<n> %s dsm.conf\n", argv[0]); return 1; }
    if (dsm_init(argv[1]) != DSM_OK) { fprintf(stderr, "dsm_init fallo\n"); return 1; }
    printf("nodo %u listo. comandos: stats | local [n] | global [n] | allocs | html [archivo] | alloc <bytes> [nombre] | write <dir> <valor> | read <dir> | barrier <id> <n> | lock <id> | unlock <id> | quit\n", cluster_self_id());
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32] = "", a1[64] = "", a2[64] = "";
        int n = sscanf(line, "%31s %63s %63s", cmd, a1, a2);
        if (n < 1) continue;
        if (!strcmp(cmd, "quit")) break;
        else if (!strcmp(cmd, "stats")) dsm_print_stats(stdout);
        else if (!strcmp(cmd, "local")) dsm_print_local_pages(stdout, n > 1 ? (unsigned)atoi(a1) : 50);
        else if (!strcmp(cmd, "global")) dsm_print_global_pages(stdout, n > 1 ? (unsigned)atoi(a1) : 50);
        else if (!strcmp(cmd, "allocs")) dsm_print_allocs(stdout);
        else if (!strcmp(cmd, "html")) { char p[128];
            if (n > 1) snprintf(p, sizeof p, "%s", a1); else snprintf(p, sizeof p, "dsm_nodo%u.html", cluster_self_id());
            printf("html: %s -> %s\n", p, dsm_write_html(p) == DSM_OK ? "ok" : "error"); }
        else if (!strcmp(cmd, "alloc")) { void *p = dsm_alloc_named(n > 2 ? a2 : NULL, (size_t)strtoull(a1, NULL, 0)); printf("alloc -> %p (err %d)\n", p, dsm_last_error()); }
        else if (!strcmp(cmd, "write")) { volatile uint64_t *p = (volatile uint64_t *)strtoull(a1, NULL, 0); *p = strtoull(a2, NULL, 0); printf("ok\n"); }
        else if (!strcmp(cmd, "read")) { volatile uint64_t *p = (volatile uint64_t *)strtoull(a1, NULL, 0); printf("%llu\n", (unsigned long long)*p); }
        else if (!strcmp(cmd, "barrier")) printf("barrier -> %d\n", dsm_barrier((uint32_t)atoi(a1), (uint32_t)atoi(a2)));
        else if (!strcmp(cmd, "lock")) printf("lock -> %d\n", dsm_lock((uint32_t)atoi(a1)));
        else if (!strcmp(cmd, "unlock")) printf("unlock -> %d\n", dsm_unlock((uint32_t)atoi(a1)));
        else printf("comando desconocido\n");
        fflush(stdout);
    }
    dsm_shutdown();
    return 0;
}