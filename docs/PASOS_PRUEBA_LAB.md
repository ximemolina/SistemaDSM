# Guía para ejecutar todas las pruebas del DSM (24-nov-2026)

Cómo correr **todas** las pruebas del proyecto en las 2 computadoras del laboratorio y qué evidencia guardar para la revisión. Complementa a `PASOS_VALIDACION` (23-nov) y a `CONTEXTO_IA.md`.

> **Estado honesto:** los tests nuevos (`test_msi`, `test_locks`, `test_alloc`, `test_edge`) y `run_suite.sh` se escribieron contra `internal.h` pero **todavía no se han compilado ni ejecutado**. La columna "Estado" de abajo se llena con las corridas reales.

## Resumen

| Fase | Qué se prueba | Nodos | Esperado | Estado |
|---|---|---|---|---|
| 0 | Compilación limpia en ambas máquinas | – | sin errores | ☐ |
| 1 | Red: ping, TCP y **UDP** entre máquinas | – | UDP recibido | ☐ |
| 2 | `test_basic` (ping-pong, estrés) | 2 | `RESULTADO: PASS`, exit 0 | ☐ |
| 3 | Batería principal en 4 nodos | 4 | `SUITE: TODO PASS` | ☐ |
| 4 | Repetición (fallos intermitentes) | 4 | 30/30 PASS | ☐ |
| 5 | Escala con 16 nodos | 16 | PASS | ☐ |
| 6 | Evidencia de uso de la API | 4 | stats ≠ 0, HTML | ☐ |

## Fase 0. Preparación

Máquinas: **A = 172.19.12.81** y **B = 172.19.12.48** (misma subred).

1. Copiar `test_util.h`, `test_msi.c`, `test_locks.c`, `test_alloc.c`, `test_edge.c` a `apps/` (junto a los demás tests), y `run_suite.sh` a la raíz del repo. Los `#include "../src/internal.h"` de los archivos nuevos ya funcionan desde `apps/`.
2. En el `Makefile`, **no hace falta una regla nueva**: la regla genérica `build/%: apps/%.c` ya compila cualquier archivo de `apps/`. Solo hay que añadir los nombres a `BINS` y, para que un cambio en `test_util.h` recompile los tests, añadirlo como dependencia:

```make
BINS := test_basic test_task app_tasks app_pagerank app_heat3d test_msi test_locks test_alloc test_edge dsm_node print_sizes
...
build/%: apps/%.c apps/test_util.h $(LIB) $(MATHLIB) src/internal.h | build
> $(CC) $(CFLAGS) -o $@ $< $(LIB) $(MATHLIB) $(LDLIBS)
```

   (Nombre del binario de tareas: `test_task`, sin "s".)
3. En **ambas** máquinas:

```sh
chmod +x run_suite.sh scripts/*.sh lab.sh
rm -rf build && make
ls -l build/test_basic build/test_task build/app_tasks build/app_pagerank build/app_heat3d \
      build/test_msi build/test_locks build/test_alloc build/test_edge
```

Todos deben aparecer con `-rwx`. Si salta `Permiso denegado` (exit 126), es el `build/` ajeno del zip: `rm -rf build` y recompilar.

4. Comprobar que las dos máquinas tienen la **misma** configuración:

```sh
md5sum dsm2.conf dsm4.conf dsm16.conf
```

## Archivos de configuración

Todas comparten: `page_size = 4096`, `mem_per_node = 16M`, `global_base_addr = 0x700000000000`, `worker_pool_size = 8`, `request_timeout_ms = 5000`, `barrier_timeout_ms = 60000`, `heartbeat_port_offset = 10`, `hash_seed = 0`. Si `wait_all_nodes` no vale 1 por defecto, añadir `wait_all_nodes = 1`.

**`dsm2.conf`** (2 nodos, una máquina cada uno; es el `dsm.conf` actual):

```ini
max_nodes = 2
node = 0 172.19.12.81 27800
node = 1 172.19.12.48 27801
```

**`dsm4.conf`** (4 nodos, 2 por máquina; los mensajes entre máquinas siguen cruzando la red):

```ini
max_nodes = 4
node = 0 172.19.12.81 27800
node = 1 172.19.12.81 27801
node = 2 172.19.12.48 27800
node = 3 172.19.12.48 27801
```

**`dsm16.conf`** (16 nodos, 8 por máquina): `max_nodes = 16`; nodos 0–7 en 172.19.12.81 con puertos 27800–27807, nodos 8–15 en 172.19.12.48 con puertos 27800–27807. El UDP usa puerto + 10.

Regla: `max_nodes` debe coincidir con el número de líneas `node=`, y el nodo propio se elige con la variable `DSM_NODE_ID` (no por IP).

## Fase 1. Red (incluye el pendiente de UDP)

```sh
bash lab.sh info                  # ambas máquinas: IPs, nodo detectado, ping al otro
# Máquina B:
bash lab.sh udp-recv
# Máquina A (mientras B escucha):
bash lab.sh udp-send
```

Esperado: en B aparecen líneas `RECIBIDO de 172.19.12.81 : hola desde nodo 0 #1` … `#5`. Repetir en sentido contrario. Esto cierra el pendiente "UDP no verificado" (el heartbeat usa UDP; `curl` solo prueba TCP).

Nota: `lab.sh` detecta el nodo por IP, por eso solo se usa aquí y para `test_basic`; con 2 nodos por máquina se usa `run_suite.sh`.

## Fase 2. `test_basic` (exactamente 2 nodos)

```sh
# Máquina A:
./run_suite.sh dsm2.conf "0" test_basic
# Máquina B:
./run_suite.sh dsm2.conf "1" test_basic
```

Esperado en cada una: `n0: PASS` / `n1: PASS`. El log queda en `build/suite/test_basic_n<id>.log` y debe contener `Test A … OK`, `Test D … OK`, `Test B … OK`, `RESULTADO: PASS`.

## Fase 3. Batería principal (4 nodos)

Lanzar **el mismo comando casi a la vez** en cada máquina:

```sh
# Máquina A (nodos 0 y 1):
./run_suite.sh dsm4.conf "0 1" test_msi test_locks test_alloc test_edge test_task app_tasks app_pagerank app_heat3d
# Máquina B (nodos 2 y 3):
./run_suite.sh dsm4.conf "2 3" test_msi test_locks test_alloc test_edge test_task app_tasks app_pagerank app_heat3d
```

Variables opcionales: `TMO=180` (timeout por test, en segundos), `GAP=4` (pausa entre tests), `DSM_VERBOSE=1` (log detallado).

| Test | Qué demuestra | Línea esperada |
|---|---|---|
| `test_msi` | MSI: I→S, S→I, S→M, M→S, lecturas viejas (anillo de 100 pasos), false sharing, cruce de página, stats | `F1…F8 OK`, `RESULTADO: PASS` |
| `test_locks` | Exclusión mutua, invariante `a==b` en sección crítica, locks independientes | `L1…L3 OK`, `RESULTADO: PASS` |
| `test_alloc` | Misma dirección en todos los nodos, regiones disjuntas, `dsm_alloc` concurrente, agotamiento → NULL | `A1…A4 OK`, `RESULTADO: PASS` |
| `test_edge` | NULL → SIGSEGV normal; `read()` sobre buffer DSM | `E1, E2 OK`, `RESULTADO: PASS` |
| `test_task` | Tareas: nodo explícito, balanceo, datos en DSM, anidadas, caída con/sin reintento | `[n0] test_tasks (4 nodos): PASS` |
| `app_tasks` | Multiplicación de matrices por tareas (verificada contra el resultado secuencial) | `app_tasks N=96 … PASS` |
| `app_pagerank` | PageRank distribuido con locks y barriers | `RESULTADO: PASS` (`suma≈1.0`) |
| `app_heat3d` | Stencil 3D distribuido | `RESULTADO: PASS` |

Notas:
- `test_task` tarda **más de 60 s**: los nodos víctima duermen 60 s antes de salir. Es normal. Con 4 nodos también se ejecuta el caso 6b (muerte sin reintento).
- Salida final de la corrida: `SUITE: TODO PASS` (código de salida 0). Si falla, indica el log: `build/suite/<test>_n<id>.log`. Un `rc=124` es un **timeout** (cuelgue).
- `app_heat3d` por sí solo **no detecta lecturas viejas** (el campo analítico es un punto fijo del stencil); la prueba que sí lo hace es la fase F5 de `test_msi`. No presentar heat3d como prueba de coherencia.

## Fase 4. Repetición (fallos intermitentes)

Un PASS aislado no descarta errores de coherencia. Repetir 30 veces:

```sh
# Máquina A:
for i in $(seq 30); do ./run_suite.sh dsm4.conf "0 1" test_msi test_locks || break; done
# Máquina B:
for i in $(seq 30); do ./run_suite.sh dsm4.conf "2 3" test_msi test_locks || break; done
```

Para `test_basic` (2 nodos) se puede usar el script existente: `bash lab.sh rep 30`, que deja `build/rep_resumen_n<ID>.txt`.

Criterio: 30 de 30 PASS y `reintentos` bajos o en 0 en los logs.

## Fase 5. Escala (16 nodos)

```sh
# Máquina A:
./run_suite.sh dsm16.conf "0 1 2 3 4 5 6 7" test_msi app_pagerank
# Máquina B:
./run_suite.sh dsm16.conf "8 9 10 11 12 13 14 15" test_msi app_pagerank
```

Esperado: PASS. Si hay timeouts de barrier, subir `barrier_timeout_ms` y revisar `worker_pool_size`.

## Fase 6. Evidencia de que se usó la API

Para mostrar en la revisión (guardar capturas o logs):

1. **Contadores del protocolo.** `test_msi` (fase F8) imprime en el nodo 0: `stats cluster: faultR=… faultW=… invalidaciones=… fetch=…`. Valores distintos de cero prueban que MSI corrió de verdad. También las líneas `stats n<id>` que imprimen `app_pagerank` y `app_heat3d`.
2. **Funciones de la API que usa cada binario:**

```sh
nm -u build/test_msi    | grep dsm_
nm -u build/test_locks  | grep dsm_
nm -u build/app_tasks   | grep dsm_
```

3. **Estado de páginas y reporte HTML** (desde un test o app propia): `dsm_print_stats(stdout)`, `dsm_print_global_pages(stdout, 16)`, `dsm_print_allocs(stdout)` y `dsm_write_html("reporte.html")` para abrirlo en el navegador durante la revisión.
4. **Registro de corridas:** conservar `build/suite/*.log` y, de cada corrida, `hostname`, `ip -4 addr`, `md5sum dsm4.conf` (demuestra que son dos equipos distintos con la misma configuración).

## Problemas conocidos

| Síntoma | Causa | Solución |
|---|---|---|
| `Permiso denegado`, exit 126 | binario sin bit de ejecución (`build/` del zip) | `rm -rf build && make` |
| `config_load: No such file or directory` | el `.conf` no existe en esa máquina | copiarlo a ambas |
| Barrier o `dsm_init` colgado | `max_nodes` ≠ nodos lanzados, o falta una máquina | revisar el `.conf` y lanzar todos los ids |
| `rc=124` | timeout (cuelgue real) | revisar el log del nodo; repetir con `DSM_VERBOSE=1` |
| Puerto en uso entre tests | TIME_WAIT | subir `GAP` a 8–10 |

## Qué NO está cubierto

- **Caída abrupta de un nodo** entre máquinas (`kill -9` en pleno test) y la recuperación de sus páginas en M: falta definir el comportamiento esperado y escribir el test.
- **Salida ordenada de un nodo** (`dsm_shutdown` escalonado, `MSG_PAGE_RELEASE`): test pendiente de escribir.
- **Un test que recorra cada función pública** (`dsm_free`, `dsm_prefetch_read/write`, `dsm_last_error`…) con tabla `OK/FALLO`: pendiente.
- **Nombres de host** (solo se han usado IPs) y WSL2 entre máquinas.
- Supuestos de los tests nuevos sin confirmar en ejecución: contadores de stats acumulativos, ids de lock 20–22 y de barrier desde 1000 sin choques, `dsm_alloc` imposible → NULL.

## Orden recomendado antes de la demo (27-nov)

1. Fase 0 y Fase 1 (UDP).
2. Fase 2 y Fase 3 completas; arreglar lo que falle.
3. Fase 4 (30 repeticiones) de `test_msi` y `test_locks`.
4. Fase 5 (16 nodos) una vez.
5. Fase 6: guardar la evidencia y ensayar la demo en las mismas máquinas del laboratorio.