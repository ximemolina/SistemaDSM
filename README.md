# DSM: Memoria Compartida Distribuida con paginación bajo demanda y coherencia MSI

Proyecto programado de IC-6600 Principios de Sistemas Operativos (TEC), II-2026.
Autoras: Ximena Molina y Susana Feng.

## Descripción

Varios procesos (nodos), en una o varias computadoras, aportan cada uno un bloque de memoria. El sistema los presenta como **una sola memoria compartida**: una aplicación lee y escribe con punteros normales sin saber en qué nodo están los datos.

Cómo funciona:

1. Cada nodo reserva el espacio global con `mmap` en la misma dirección base y marca como `PROT_NONE` las páginas que no tiene.
2. Al tocar una de ellas, el kernel envía `SIGSEGV`. El handler calcula la página, detecta si fue lectura o escritura y pide la página por TCP.
3. Al llegar, la página se copia por una segunda vista de la memoria (`memfd_create`, doble mapeo), se abre el permiso con `mprotect` y la instrucción se reintenta.
4. El protocolo **MSI** (Modified, Shared, Invalid) garantiza que nadie lea datos viejos.
5. El **home** de cada página (calculado por hash) lleva el directorio: quién tiene la versión modificada y quiénes tienen copia de solo lectura.

Lenguaje: C. Plataforma: Linux x86-64.

## Estado

Prototipo 1: dos nodos con lectura y escritura coherentes, directorio mínimo y barrier. Detalle en `AVANCE2.md`. Los cambios respecto al diseño están en `CAMBIOS_PROTOTIPO1.md` y lo que sigue en `SIGUIENTES_PASOS.md`.

No implementado todavía: JOIN dinámico, heartbeat, estadísticas, alloc remoto, `dsm_lock`.

## Requisitos

- Linux x86-64 (usa `memfd_create` y `MAP_FIXED_NOREPLACE`, kernel 4.17 o superior)
- `gcc` y `make`
- Soporte de `pthread` y semáforos POSIX (incluidos en glibc)

## Instalación y compilación

```sh
cd dsm-prototipo1
make
```

Genera `build/test_basic` y `build/print_sizes`. Si el directorio está en una partición montada sin permiso de ejecución, copie la carpeta del proyecto a otra ruta (por ejemplo `~/dsm`).

## Configuración (`dsm.conf`)

```
page_size          = 4096
mem_per_node       = 16M
max_nodes          = 2
global_base_addr   = 0x700000000000
worker_pool_size   = 8
request_timeout_ms = 5000
barrier_timeout_ms = 60000
hash_seed          = 0
node = 0 127.0.0.1 9101
node = 1 127.0.0.1 9102
```

- `max_nodes` debe coincidir con el número de líneas `node =` (máximo 16).
- Cada línea `node = <id> <ip> <puerto>` define un nodo. Use el rango de puertos asignado a la pareja.
- Todos los nodos deben usar el **mismo archivo**.
- El nodo propio se indica con la variable de entorno `DSM_NODE_ID`.

## Ejecución

Prueba automática (dos nodos en la misma máquina, espera `PASS` en ambos):

```sh
make test
```

A mano, en dos terminales:

```sh
DSM_NODE_ID=0 ./build/test_basic dsm.conf
DSM_NODE_ID=1 ./build/test_basic dsm.conf
```

En computadoras distintas: ponga las IP reales en `dsm.conf`, copie el mismo archivo a cada una y arranque un nodo por máquina. Con `DSM_VERBOSE=1` se imprimen contadores de fallos, páginas e invalidaciones al cerrar.

## Uso de la API

```c
#include "internal.h"   /* en el prototipo, el API vive en este encabezado */

dsm_init("dsm.conf");                   /* usa DSM_NODE_ID */
uint64_t *x = dsm_alloc(4096);          /* memoria compartida, alineada a página */
x[0] = 42;                              /* escritura normal; el DSM trae la página si hace falta */
dsm_barrier(1, 2);                      /* sincroniza a 2 participantes */
dsm_shutdown();
```

Funciones: `dsm_init`, `dsm_alloc` (local), `dsm_free` (no hace nada por ahora), `dsm_barrier`, `dsm_shutdown`, `dsm_slot_base`. Códigos de error: `DSM_ERR_NOMEM`, `DSM_ERR_TIMEOUT`, `DSM_ERR_NODE_DOWN`, `DSM_ERR_BAD_ARG`, `DSM_ERR_PROTO`, `DSM_ERR_IO`.

## Estructura del proyecto

```
src/        módulos: config, cluster, network, pager, directory, dsm, internal.h
apps/       test_basic.c (pruebas del Prototipo 1)
tools/      print_sizes.c (tamaño de estructuras)
scripts/    run_local.sh (lanza 2 nodos locales)
dsm.conf    configuración de ejemplo
Makefile
```

| Módulo | Contenido |
|---|---|
| `pager.c` | `mmap`, doble mapeo, handler de `SIGSEGV`, `mprotect`, estados MSI locales |
| `network.c` | Sockets, listener, pool de hilos, framing, timeouts |
| `cluster.c` | Tabla de nodos, hash de home, propietario inicial |
| `directory.c` | Directorio en el home, `FETCH`, invalidaciones, write-back |
| `config.c` | Lectura de `dsm.conf` |
| `dsm.c` | API pública, alloc local, barrier |

## Problemas frecuentes

| Síntoma | Causa probable |
|---|---|
| `Permission denied` al ejecutar `build/...` | Partición sin permiso de ejecución: copie la carpeta del proyecto a otra ruta |
| `mmap ... MAP_FIXED_NOREPLACE` falla | La dirección base está ocupada: cambie `global_base_addr` en todos los nodos |
| `bind/listen` falla | Puerto en uso: cambie los puertos en `dsm.conf` |
| `DSM_ERR_TIMEOUT` y salida con código 70 | El otro nodo no responde: verifique que esté corriendo y que IP y puertos sean correctos |
| `falta variable de entorno DSM_NODE_ID` | Defina `DSM_NODE_ID` antes de ejecutar |

## Limitaciones

Sin tolerancia a fallas: si un nodo cae con páginas en estado M, esos datos se pierden. Probado solo con 2 nodos en `127.0.0.1`.