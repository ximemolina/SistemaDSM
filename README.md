# DSM: Memoria Compartida Distribuida con paginación bajo demanda y coherencia MSI
 
Proyecto programado de IC-6600 Principios de Sistemas Operativos (TEC), II-2026.
Autoras: Ximena Molina y Susana Feng.

Lenguaje: C. Plataforma: Linux x86-64.
 
## Descripción
 
Varios procesos (nodos), en una o varias computadoras, aportan cada uno un bloque de memoria. El sistema los presenta como **una sola memoria compartida**: una aplicación lee y escribe con punteros normales sin saber en qué nodo están los datos.
 
1. Cada nodo reserva el espacio global con `mmap` en la misma dirección base y marca como `PROT_NONE` las páginas que no tiene.
2. Al tocar una de ellas, el kernel envía `SIGSEGV`. El handler calcula la página, detecta si fue lectura o escritura y pide la página por TCP.
3. Al llegar, la página se copia por una segunda vista de la memoria (`memfd_create`, doble mapeo), se abre el permiso con `mprotect` y la instrucción se reintenta.
4. El protocolo **MSI** (Modified, Shared, Invalid) garantiza que nadie lea datos viejos.
5. El **home** de cada página (por hash) lleva el directorio: quién tiene la versión modificada y quiénes tienen copia de solo lectura.

 
## Qué incluye (Prototipo 2)
 
- MSI completo con invalidaciones para hasta 16 nodos, varios hilos por nodo.
- Membresía (`JOIN`/`NODE_LIST`), heartbeat UDP con detección de nodos caídos y fallo rápido.
- Dos pools de hilos (con espera y sin espera) que evitan deadlocks por agotamiento.
- Asignación global con nombres (`dsm_alloc_named`), `dsm_barrier` y `dsm_lock`/`dsm_unlock`.
- Write-back al apagar, liberación de RAM de páginas invalidadas (`PUNCH_HOLE`).
- Estadísticas, tabla de páginas local y global, asignaciones por nodo y proceso, salida en texto y HTML.
- Dos aplicaciones de prueba: multiplicación de matrices y arreglo concurrente con locks.
- **Extra 1: ejecución distribuida de tareas** (`dsm_spawn`/`dsm_wait`, planificación por carga, reintento si cae el nodo). Ver `docs/EXTRA1.md`.
No incluye tolerancia a fallos (Extra 2). Limitaciones completas en `docs/CORRECCIONES.md` y `docs/EXTRA1.md`.
 
## Requisitos
 
- Linux x86-64, kernel 4.17 o superior (`MAP_FIXED_NOREPLACE`), glibc con `memfd_create` (2.27+; hay respaldo con `shm_open`)
- `gcc` y `make`
- Verificar en cada máquina: `uname -m && uname -r && ldd --version`
## Instalación y compilación
 
```sh
cd dsm-prototipo1
make            # genera build/test_basic, test_stress, test_hb, test_leave, test_tasks, app_matmul, app_tasks, app_counter, dsm_node
make test       # batería completa (14 pruebas, 2 a 8 nodos en la misma máquina)
```
 
Si la carpeta está en una partición montada sin permiso de ejecución, copie el proyecto a otra ruta (por ejemplo `~/dsm`).
 
`make FALLBACK=1` compila con la detección lectura/escritura por estado de página (para arquitecturas distintas de x86-64).
 
## Configuración (`dsm.conf`)
 
Formato `clave = valor`; `#` comenta; los tamaños aceptan K/M/G. **Todos los nodos deben usar el mismo archivo.**
 
| Parámetro | Descripción | Por defecto |
|---|---|---|
| `page_size` | Tamaño de página (potencia de 2, múltiplo del de sistema) | 4096 |
| `mem_per_node` | Memoria que aporta cada nodo (igual para todos) | 16M |
| `max_nodes` | Número de nodos de la corrida (1 a 16); debe coincidir con las líneas `node =` | 2 |
| `global_base_addr` | Dirección base común | `0x700000000000` |
| `worker_pool_size` | Hilos del pool que puede bloquearse (`PAGE_REQUEST`) | 8 |
| `nowait_pool_size` | Hilos del pool que nunca espera red (mínimo 2) | 4 |
| `request_timeout_ms` / `request_retries` | Espera de una página y reenvíos antes de abortar | 5000 / 2 |
| `connect_timeout_ms` | Timeout de conexión a un nodo | 500 |
| `join_timeout_ms` / `wait_all_nodes` | `dsm_init` espera a todos los nodos | 30000 / 1 |
| `barrier_timeout_ms` | Timeout de barrier y lock | 60000 |
| `heartbeat_interval_ms` / `heartbeat_timeout_ms` | Latidos y umbral de nodo caído | 1000 / 5000 |
| `heartbeat_port_offset` | Puerto UDP de latidos = puerto del nodo + offset | 1000 |
| `min_hold_us` | Retención mínima de una página recién recibida en M | 200 |
| `release_memory` | 1 = liberar RAM de páginas invalidadas ajenas | 1 |
| `task_pool_size` | Hilos ejecutores de tareas (Extra 1) | 4 |
| `task_timeout_ms` / `task_max_retries` | Espera de `dsm_wait` (0 = `barrier_timeout_ms`) y reenvíos de tareas `DSM_TASK_RETRY` | 0 / 3 |
| `hash_seed` | Semilla del hash de home | 0 |
| `node = <id> <host> <puerto>` | Un nodo (IP o nombre). Use el rango de puertos asignado a la pareja | — |
 
Ejemplo para 3 nodos en la misma máquina: `./scripts/gen_conf.sh 3 8 9100 > dsm.conf`.
 
## Ejecución
 
El nodo propio se indica con la variable `DSM_NODE_ID`.
 
```sh
# Una aplicación en 3 nodos, un proceso por nodo (en terminales distintas o computadoras distintas)
DSM_NODE_ID=0 ./build/app_matmul dsm.conf 128
DSM_NODE_ID=1 ./build/app_matmul dsm.conf 128
DSM_NODE_ID=2 ./build/app_matmul dsm.conf 128
 
# O todo en una máquina con el script
./scripts/run_n.sh app_matmul 3 8 9100 128     # binario, nodos, pool, puerto base, argumentos
```
 
En computadoras distintas: ponga las IP o nombres reales en `dsm.conf`, copie el mismo archivo a cada una y arranque un nodo por máquina. `DSM_VERBOSE=1` imprime contadores y eventos de membresía.
 
### Nodo interactivo y consultas
 
```sh
DSM_NODE_ID=0 ./build/dsm_node dsm.conf
```
 
Comandos: `stats`, `local [n]`, `global [n]`, `allocs`, `html [archivo]`, `alloc <bytes> [nombre]`, `write <dir> <valor>`, `read <dir>`, `barrier <id> <n>`, `lock <id>`, `unlock <id>`, `quit`. El HTML se genera por nodo (`dsm_nodo<ID>.html`), de modo que varios nodos en una misma máquina no se mezclan.
 
## API
 
```c
#include "internal.h"                       /* en este proyecto el API vive en este encabezado */
 
dsm_init("dsm.conf");                       /* espera a que estén todos los nodos */
double *a = dsm_alloc_named("A", n * 8);    /* mismo puntero en todos los nodos que usen el nombre */
void  *p = dsm_alloc(4096);                 /* asignación sin nombre */
dsm_prefetch_write(a, n * 8);               /* pide permiso de escritura de una vez (evita 2 viajes) */
a[0] = 1.0;                                 /* escritura normal: el DSM trae la página si hace falta */
dsm_barrier(1, num_nodos);                  /* sincroniza a n participantes */
dsm_lock(7); /* ... */ dsm_unlock(7);       /* exclusión mutua distribuida */
dsm_stats_t s; dsm_stats(&s);               /* estadísticas de este nodo */
dsm_register_task(1, mi_funcion);           /* Extra 1: antes de dsm_init, en todos los nodos */
dsm_task_t t; dsm_spawn(1, &args, sizeof args, DSM_ANY_NODE, DSM_TASK_RETRY, &t);
int64_t r; dsm_wait(t, &r);                 /* resultado de la tarea ejecutada en otro nodo */
dsm_shutdown();                             /* devuelve las páginas modificadas y cierra */
```
 
| Función | Notas |
|---|---|
| `dsm_init`, `dsm_shutdown` | Errores: `DSM_ERR_*` |
| `dsm_alloc`, `dsm_alloc_named`, `dsm_free` | Devuelven `NULL` en error; `dsm_last_error()` da el código. `dsm_free` no reutiliza la memoria |
| `dsm_barrier(id, n)`, `dsm_lock(id)`, `dsm_unlock(id)` | `DSM_ERR_TIMEOUT` si un participante cayó |
| `dsm_prefetch_write/read(ptr, len)` | Pistas de rendimiento |
| `dsm_register_task`, `dsm_spawn`, `dsm_wait`, `dsm_wait_all`, `dsm_task_status` | Ejecución distribuida (Extra 1), ver `docs/EXTRA1.md` |
| `dsm_stats`, `dsm_stats_node`, `dsm_print_stats`, `dsm_print_local_pages`, `dsm_print_global_pages`, `dsm_print_allocs`, `dsm_write_html` | Consultas |
 
Códigos de error: `DSM_ERR_NOMEM`, `DSM_ERR_TIMEOUT`, `DSM_ERR_NODE_DOWN`, `DSM_ERR_BAD_ARG`, `DSM_ERR_PROTO`, `DSM_ERR_IO`.
 
Reglas para las aplicaciones: usar `dsm_alloc*` (no `malloc`) para los datos compartidos, alinear a página lo que escriben nodos distintos (evita *false sharing*) y sincronizar con `dsm_barrier` o `dsm_lock`.
 
## Estructura del proyecto
 
```
src/        config, cluster, network, pager, directory, dsm, stats, main, internal.h
apps/       test_basic, test_stress, test_hb, test_leave, test_tasks, app_matmul, app_tasks, app_counter
tools/      print_sizes.c (tamaño de estructuras)
scripts/    gen_conf.sh, run_n.sh, run_all.sh, run_local.sh
docs/       AVANCE3.md, CORRECCIONES.md, EXTRA1.md, SIGUIENTES_PASOS.md
Makefile
```
 
| Módulo | Contenido |
|---|---|
| `pager.c` | `mmap`, doble mapeo, handler de `SIGSEGV`, `mprotect`, estados MSI, reintentos |
| `network.c` | Sockets, listener, dos pools, framing, RPC, timeouts |
| `cluster.c` | Membresía, heartbeat, hash de home, coordinadores |
| `directory.c` | Directorio en el home, `FETCH`, invalidaciones, write-back |
| `dsm.c` | API pública, alloc global, barrier, locks |
| `tasks.c` | Ejecución distribuida de tareas (Extra 1) |
| `stats.c`, `main.c` | Estadísticas, HTML, nodo interactivo |
 
## Problemas frecuentes
 
| Síntoma | Causa probable |
|---|---|
| `Permission denied` al ejecutar `build/...` | Partición sin permiso de ejecución: copie el proyecto a otra ruta |
| `mmap ... MAP_FIXED_NOREPLACE` falla | Dirección base ocupada o kernel anterior a 4.17: cambie `global_base_addr` en todos los nodos |
| `bind/listen` o `heartbeat bind` falla | Puerto en uso: cambie los puertos (el UDP usa puerto + `heartbeat_port_offset`) |
| `dsm_init` termina con `DSM_ERR_TIMEOUT` | No todos los nodos arrancaron en `join_timeout_ms`, o `max_nodes` no coincide con las líneas `node =` |
| `DSM_ERR_TIMEOUT ... pagina=N home=nodo M` y salida 70 | El home o el owner de esa página no responde: revise ese nodo |
| `falta variable de entorno DSM_NODE_ID` | Defina `DSM_NODE_ID` antes de ejecutar |
 
## Limitaciones
 
Sin tolerancia a fallos: si un nodo cae con páginas en estado M, esos datos se pierden, y el directorio de un home caído también. El nodo 0 coordina las asignaciones (`dsm_alloc`). Probado hasta con 8 nodos en una máquina de 1 núcleo; falta la prueba multicore y entre computadoras físicas.
