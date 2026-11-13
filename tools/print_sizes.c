/* Imprime el tamano exacto de las estructuras (lo pide el Entregable 1). */
#include "../src/internal.h"
#include <stdio.h>
#include <semaphore.h>
/* copias de las estructuras privadas de pager.c / directory.c */
typedef struct { page_state_t state; uint32_t pid_owner; uint32_t seq_order; } page_local_t;
typedef struct { uint32_t owner_node; uint64_t copyset; uint8_t owner_dirty; uint8_t init; } dir_entry_t;
int main(void) {
    printf("msg_header_t : %zu bytes (packed)\n", sizeof(msg_header_t));
    printf("node_info_t  : %zu bytes\n", sizeof(node_info_t));
    printf("dsm_config_t : %zu bytes\n", sizeof(dsm_config_t));
    printf("page_local_t : %zu bytes\n", sizeof(page_local_t));
    printf("dir_entry_t  : %zu bytes\n", sizeof(dir_entry_t));
    printf("sem_t        : %zu bytes\n", sizeof(sem_t));
    return 0;
}
