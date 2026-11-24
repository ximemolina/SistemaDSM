# DSM: Memoria Compartida Distribuida con paginación bajo demanda y coherencia MSI

Proyecto programado de IC-6600 Principios de Sistemas Operativos (TEC), II-2026.
Autoras: Ximena Molina y Susana Feng.

Lenguaje: C. Plataforma: Linux x86-64.

## Descripción

Varios procesos (nodos), en una o varias computadoras, aportan cada uno un bloque de memoria. El sistema los presenta como **una sola memoria compartida**: una aplicación lee y escribe con punteros normales sin saber en qué nodo están los datos.

1. Cada nodo reserva el espacio global con `mmap` en la misma dirección base y marca como `PROT_NONE` las páginas que no tiene.
2. Al tocar una de ellas, el kernel envía `SIGSEGV`. El handler calcula la página, detecta si fue lectura o escritura y encola la petición; un hilo la envía por TCP.
3. Al llegar, la página se copia por una segunda vista de la memoria (`memfd_create`, doble mapeo), se abre el permiso con `mprotect` y la instrucción se reintenta.
4. El protocolo **MSI** (Modified, Shared, Invalid) garantiza que nadie lea datos viejos.
5. El **home** de cada página (por hash) lleva el directorio: quién tiene la versión modificada y quiénes tienen copia de solo lectura.

El detalle verificable de cada módulo, con referencias `archivo:línea`, está en `docs/CONTEXTO_IA.md`.

## Qué incluye (Prototipo 2)

- MSI completo con invalidaciones para hasta 16 nodos, varios hilos por nodo.
- Membresía (`JOIN`/`NODE_LIST`), heartbeat UDP con detección de nodos caídos y fallo rápido.
- Dos pools de hilos (con espera y sin espera) que evitan deadlocks por agotamiento.
- Asignación global con nombres (`dsm_alloc_named`), `dsm_barrier` y `dsm_lock`/`dsm_unlock`.
- Write-back al apagar, liberación de RAM de páginas invalidadas (`PUNCH_HOLE`).
- Estadísticas, tabla de páginas local y global, asignaciones por nodo y proceso, salida en texto y HTML.
- **Extra 1: ejecución distribuida de tareas** (`dsm_spawn`/`dsm_wait`, planificación por carga, reintento si cae el nodo). Implementación en `src/tasks.c`; detalle en `docs/CONTEXTO_IA.md` §4.8. (`docs/EXTRA1.md` es una lista de pendientes, no una especificación de tareas.)
- No incluye tolerancia a fallos (Extra 2). Limitaciones y decisiones en `docs/AVANCE3.md` §4 y correcciones en §6. **No existe `docs/CORRECCIONES.md`** pese a las referencias antiguas en `docs/AVANCE3.md:10,56`.

**Estado del árbol:** las aplicaciones `app_matmul` y `app_counter` (documentadas en `docs/AVANCE3.md:20-21`) y los scripts `gen_conf.sh`, `run_n.sh`, `run_all.sh` **no están versionados**. Las fuentes de app presentes son `apps/test_basic.c`, `apps/test_task.c`, `apps/app_tasks.c`, `apps/app_pagerank.c` y `apps/app_heat3d.c`. Los scripts son `scripts/run_local.sh`, `run_apps.sh` y `run_3nodes.sh`. La demo de apps de gran memoria está en `docs/DEMO_APPS.md`.

## Requisitos

- Linux x86-64, kernel 4.17 o superior (`MAP_FIXED_NOREPLACE`), glibc con `memfd_create` (2.27+; hay respaldo con `shm_open`)
- `gcc` y `make`
- Verificar en cada máquina: `uname -m && uname -r && ldd --version`

## Instalación y compilación

```sh
make
./scripts/run_local.sh dsm.local.conf  # 2 nodos de test_basic, pero dsm.local.conf apunta a 127.0.0.1
```

`make` compila los 7 binarios declarados en `Makefile:11`. `make test` ejecuta `./scripts/run_local.sh dsm.conf`; en una sola máquina usa directamente `./scripts/run_local.sh dsm.local.conf`.

Si la carpeta está en una partición montada sin permiso de ejecución, copie el proyecto a otra ruta (por ejemplo `~/dsm`).

`make FALLBACK=1 build/test_basic` compila con la detección lectura/escritura por estado de página (para arquitecturas distintas de x86-64). `FALLBACK` es una variable, no un target.

## Configuración (`dsm.conf`)

Formato `clave = valor`; `#` comenta; los tamaños aceptan K/M/G. **Todos los nodos deben usar el mismo archivo.** El id propio se da con `DSM_NODE_ID`.

Los valores por defecto viven en `src/config.c:25-32,76`; el `dsm.conf` incluido solo fija 8 claves y 2 líneas `node` (no hay `scripts/gen_conf.sh`).

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
| `max_payload` | Tamaño máximo de mensaje (0 = `page_size + 4352`) | `page_size + 4352` |
| `node = <id> <host> <puerto>` | Un nodo (IP o nombre). Use el rango de puertos asignado a la pareja | — |

Ejemplo para 3 nodos en la misma máquina: edite `dsm.conf`, cambie `max_nodes = 3` y añada `node = 2 127.0.0.1 9103` (los latidos UDP usan 9101/9102/9103 + 1000).

## Ejecución

El nodo propio se indica con la variable `DSM_NODE_ID` (obligatoria).

```sh
# Una aplicación en 2 nodos, un proceso por nodo (terminales distintas o computadoras distintas)
DSM_NODE_ID=0 ./build/test_basic dsm.conf
DSM_NODE_ID=1 ./build/test_basic dsm.conf

# O en una sola máquina con el script disponible
./scripts/run_local.sh dsm.conf

# Extra 1: test_task.c requiere >= 3 nodos (max_nodes=3 y 3 líneas node=)
make build/test_task
DSM_NODE_ID=0 ./build/test_task dsm.conf
DSM_NODE_ID=1 ./build/test_task dsm.conf
DSM_NODE_ID=2 ./build/test_task dsm.conf

# Otra app disponible: app_tasks (compila desde apps/app_tasks.c)
make build/app_tasks
DSM_NODE_ID=0 ./build/app_tasks dsm.conf 96

# Apps de gran memoria en DOS MAQUINAS (una terminal por máquina)
./scripts/setup_lab.sh <ip_nodo0> <ip_nodo1>       # una sola vez; copie dsm.conf y dsm.lab2.conf a ambas
# Maquina A:
./scripts/run_lab_node.sh app_pagerank dsm.conf 0 131072 16 12
# Maquina B:
./scripts/run_lab_node.sh app_pagerank dsm.conf 1 131072 16 12
# Segunda app, en paralelo y con otros puertos:
./scripts/run_lab_node.sh app_heat3d dsm.lab2.conf 0 2048 8    # maquina A
./scripts/run_lab_node.sh app_heat3d dsm.lab2.conf 1 2048 8    # maquina B
```

Detalle completo de investigación, adaptación, escenarios, métricas y guion de demo:
`docs/DEMO_APPS.md`. Para smoke tests en una sola máquina: `./scripts/run_apps.sh` o
`./scripts/run_3nodes.sh`.

Los binarios `test_stress`, `test_hb`, `test_leave`, `test_tasks`, `app_matmul` y `app_counter` que aparecen en informes antiguos no se pueden lanzar porque no hay fuente para ellos en `apps/`.

En computadoras distintas: use `scripts/setup_lab.sh` para fijar las IP reales, copie el mismo `dsm.conf` (y `dsm.lab2.conf`) a cada una, recompile ahí mismo y arranque un nodo por máquina con `scripts/run_lab_node.sh`. `DSM_VERBOSE=1` imprime contadores y eventos de membresía.

### Nodo interactivo y consultas

```sh
DSM_NODE_ID=0 ./build/dsm_node dsm.conf
```

Comandos: `stats`, `local [n]`, `global [n]`, `allocs`, `html [archivo]`, `alloc <bytes> [nombre]`, `write <dir> <valor>`, `read <dir>`, `barrier <id> <n>`, `lock <id>`, `unlock <id>`, `quit`. El HTML se genera por nodo (`dsm_nodo<ID>.html` por defecto, `src/main.c:22`), de modo que varios nodos en una misma máquina no se mezclan.

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
| `dsm_barrier(id, n)`, `dsm_lock(id)`, `dsm_unlock(id)` | `n = 0` o `n > max_nodes` da `DSM_ERR_BAD_ARG`; participante caído da `DSM_ERR_TIMEOUT` |
| `dsm_prefetch_write/read(ptr, len)` | Pistas de rendimiento |
| `dsm_register_task`, `dsm_spawn`, `dsm_wait`, `dsm_waitin_all`, `dsm_task_status` | Ejecución distribuida (Extra 1), implementación en `src/tasks.c` |
| `dsm_stats`, `dsm_stats_node`, `dsm_print_stats`, `dsm_print_local_pages`, `dsm_print_global_pages`, `dsm_print_allocs`, `dsm_write_html` | Consultas |

Códigos de error: `DSM_ERR_NOMEM`, `DSM_ERR_TIMEOUT`, `DSM_ERR_NODE_DOWN`, `DSM_ERR_BAD_ARG`, `DSM_ERR_PROTO`, `DSM_ERR_IO`.

Reglas para las aplicaciones: usar `dsm_alloc*` (no `malloc`) para los datos compartidos, alinear a página lo que escriben nodos distintos (evita *false sharing*) y sincronizar con `dsm_barrier` o `dsm_lock`.

## Estructura del proyecto

```
src/        config.c cluster.c network.c pager.c directory.c dsm.c stats.c tasks.c main.c internal.h
apps/       test_basic.c, test_task.c, app_tasks.c, app_pagerank.c, app_heat3d.c
tools/      print_sizes.c (imprime 6 tamaños; su page_local_t no es la del pager)
scripts/    run_local.sh, run_apps.sh, run_3nodes.sh, run_lab_node.sh, setup_lab.sh
configs     dsm.conf, dsm.lab2.conf, dsm.local.conf, dsm.local2.conf, dsm.local3.conf, dsm.local.big.conf
docs/       AVANCE1.md, AVANCE2.md, AVANCE3.md, EXTRA1.md, DEMO_APPS.md
Makefile    README.md
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

Archivos citados en documentos antiguos pero **inexistentes**: `scripts/gen_conf.sh`, `run_n.sh`, `run_all.sh`; los 6 apps ausentes listados arriba; `docs/CORRECCIONES.md`, `ENTREGABLE1.md`, `SIGUIENTES_PASOS.md`, `CAMBIOS_PROTOTIPO1.md`.

## Problemas frecuentes

| Síntoma | Causa probable |
|---|---|
| `make` se detiene por falta de `apps/...c` | El `Makefile:9` declara binarios sin fuente: ver comandos de compilación arriba |
| `make test` falla | Ejecuta `scripts/run_all.sh`, que no está en el repo |
| `Permission denied` al ejecutar `build/...` | Partición sin permiso de ejecución: copie el proyecto a otra ruta |
| `mmap ... MAP_FIXED_NOREPLACE` falla | Dirección base ocupada o kernel anterior a 4.17: cambie `global_base_addr` en todos los nodos |
| `bind/listen` o `heartbeat bind` falla | Puerto en uso: cambie los puertos (el UDP usa puerto + `heartbeat_port_offset`) |
| `dsm_init` termina con `DSM_ERR_TIMEOUT` | No todos los nodos arrancaron en `join_timeout_ms`, o `max_nodes` no coincide con las líneas `node =` |
| `DSM_ERR_TIMEOUT ... pagina=N home=nodo M` y salida 70 | El home o el owner de esa página no responde: revise ese nodo |
| `falta variable de entorno DSM_NODE_ID` | Defina `DSM_NODE_ID` antes de ejecutar |

Más diagnóstico en `docs/CONTEXTO_IA.md` §7. Si vas a ejecutar en el laboratorio (con o sin IA), sigue el protocolo de `docs/CONTEXTO_IA.md` §12-13 y **llena el Apéndice A con tus logs reales**: ahí queda la evidencia para la siguiente persona, sin depender de los autores.

## Limitaciones

Sin tolerancia a fallos: si un nodo cae con páginas en estado M, esos datos se pierden, y el directorio de un home caído también. El nodo 0 coordina las asignaciones (`dsm_alloc`). Según `docs/AVANCE3.md:14` se probó hasta con 8 nodos en una máquina de 1 núcleo; las fuentes de esa batería no están en este árbol y no se pudieron ejecutar aquí. Falta la prueba multicore y entre computadoras físicas.
