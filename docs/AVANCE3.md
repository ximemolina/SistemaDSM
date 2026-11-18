# Avance 3: Prototipo 2 del DSM

**Curso:** IC-6600 Principios de Sistemas Operativos, II-2026
**Autoras:** Ximena Molina y Susana Feng
**Fecha:** 20 de noviembre de 2026
**Antecedentes:** Entregable 1 (diseño), Avance 2 (Prototipo 1)

## 1. Objetivo

Cumplir la meta del Prototipo 2: MSI completo con invalidaciones para N nodos, hash de home, heartbeat, estadísticas y primera aplicación, más la corrección de los problemas de integración detectados en el Prototipo 1 (ver `CORRECCIONES.md`).

## 2. Resultado

Una batería de **11 pruebas, todas PASS**, con 2 a 8 nodos (procesos en `127.0.0.1` y con nombre `localhost`), `worker_pool_size` de 1 a 8, y la repetición de 15 corridas de 3 nodos con pool = 1 sin fallos.

| Prueba | Qué verifica |
|---|---|
| `test_basic` (2 nodos) | Ping-pong ×100, página del contribuyente, estrés con 2 hilos por nodo en la misma página |
| `test_stress` (2, 3, 4 y 8 nodos; pool 1, 2 y 4) | Escrituras cruzadas sobre las mismas páginas (false sharing); sin deadlock con pool pequeño |
| `app_matmul` (3 y 4 nodos) | Aplicación 1: multiplicación de matrices distribuida, verificada contra el cálculo serial |
| `app_counter` (4 nodos) | Aplicación 2: arreglo concurrente + contador con `dsm_lock` |
| `test_hb` (3 nodos) | Detección de un nodo caído (≈2,2 s con timeout de 2 s) y fallo rápido de `net_send` (0 ms) |
| `test_leave` (3 y 4 nodos) | Write-back al apagar: los demás leen los valores de las páginas que el nodo saliente modificó |

## 3. Módulos

| Módulo | Estado |
|---|---|
| `config.c` | Completo (IP o nombre, parámetros nuevos) |
| `cluster.c` | JOIN/NODE_LIST, heartbeat UDP, hash de home, coordinadores |
| `network.c` | Dos pools, framing, RPC, timeouts, fallo rápido |
| `pager.c` | MSI, doble mapeo, `PUNCH_HOLE`, reintentos, retención, prefetch, write-back al apagar |
| `directory.c` | Directorio con generaciones de transacción, `PAGE_RELEASE`, instantáneas para estadísticas |
| `dsm.c` | Alloc global con coordinador y nombres, barrier, locks |
| `stats.c`, `main.c` | Estadísticas, tablas de páginas, asignaciones, HTML y nodo interactivo |

## 4. Tamaños de estructuras (x86-64)

| Estructura | Bytes |
|---|---|
| `msg_header_t` (packed) | 22 |
| `page_local_t` | 16 |
| `dir_entry_t` | 24 |
| `node_info_t` | 104 |
| `dsm_config_t` | 1760 |
| `alloc_entry_t` | 64 |
| `node_stats_t` | 136 |

## 5. Pendiente para la demo

- Correr la batería en el laboratorio con varias computadoras físicas y varios núcleos (aquí solo hubo 1 núcleo).
- Medir rendimiento con pool de 1, 4, 8 y 16, y tamaños de página 4/8/16 KB (aquí no fue concluyente).
- Verificar `uname -m`, kernel y glibc de las máquinas.
- Informe final, manual de usuario y ensayo de la defensa.

La lista completa de limitaciones está en `CORRECCIONES.md` §4.

## 6. Correcciones y cambios del Prototipo 2 (y por qué)

Punto de partida: el Prototipo 1 (`AVANCE2.md`) y el análisis de integración (`Prototipo 1 del DSM: consideraciones de integración y opciones de mitigación`). Cada fila indica el problema, qué se hizo y la evidencia.

### 1. Errores reales del Prototipo 1 que se corrigieron

| # | Problema | Corrección | Por qué / evidencia |
|---|---|---|---|
| 1 | **Deadlock por agotamiento del pool.** `PAGE_REQUEST` se bloquea esperando `FETCH_REPLY`/ACK, pero el `FETCH` e `INVALIDATE` que esa espera necesita corrían en el mismo pool | Dos pools: **POOL_WAIT** (`PAGE_REQUEST`, `PAGE_RELEASE`; pueden bloquearse) y **POOL** (`FETCH`, `INVALIDATE`, `BARRIER_ARRIVE`, locks, alloc, stats; nunca esperan red). El pool sin espera tiene mínimo 2 hilos | Reproducido: el Prototipo 1 con `worker_pool_size=1` termina en `DSM_ERR_TIMEOUT`. El nuevo pasa con pool = 1 y 2, 3 y 4 nodos (15 de 15 corridas repetidas) |
| 2 | **Dos copias de la configuración** (`cluster.c` copiaba `dsm_config_t`) y `alive` sin sincronización | `cluster.c` usa el puntero a la configuración inmutable; el estado mutable (`alive`, último latido) es atómico | Con heartbeat y JOIN las copias habrían divergido y habría carreras de datos |
| 3 | **`PUNCH_HOLE` habría destruido la copia de referencia** del contribuyente | `punch()` nunca actúa sobre páginas cuyo contribuyente es el propio nodo, y se hace después de copiar los datos | Sin esto, un `FETCH` posterior sirve ceros. Contadores: 31 páginas liberadas en una corrida de 3 nodos sin pérdida de datos |
| 4 | **`FETCH_REPLY` tardío caía en una transacción nueva** de la misma página | Cada transacción del directorio tiene una *generación* que viaja en `req_id`; las respuestas con otra generación se descartan y el semáforo se drena en `txn_alloc` | Tras un timeout, una respuesta vieja podía confirmar datos obsoletos |
| 5 | **`fatal()` inmediato ante un timeout** del pager | Reenvía el `PAGE_REQUEST` hasta `request_retries` veces; el aborto nombra la página y su home | Un duplicado es inocuo: el directorio lo serializa como una petición normal |
| 6 | **Un nodo caído congelaba a quien le enviaba** (connect de hasta 5 s con el mutex tomado, `send` sin timeout) | `connect()` no bloqueante con timeout de 500 ms, `SO_SNDTIMEO`, y **fallo rápido** (`DSM_ERR_NODE_DOWN`) si el heartbeat lo marcó caído. Lectura con `SO_RCVTIMEO` de 3 s | `test_hb`: el fallo rápido responde en 0 ms |
| 7 | **El listener podía quedarse esperando para siempre** a un par que envió medio mensaje | `SO_RCVTIMEO` en sockets aceptados | Era una limitación listada en el Avance 2 |
| 8 | **Spinlock global único** del pager | Spinlocks por franja (64) para estado de página + uno aparte para fallos y cola; orden franja → fallos | Reduce contención entre hilos que fallan en páginas distintas. Sigue siendo async-signal-safe |
| 9 | **Posible livelock** con false sharing (la página recién recibida se cede antes de usarse) | Retención mínima `min_hold_us` (200 µs) antes de ceder una página en M; contador de fetch retenidos | Mitigación; en esta máquina (1 núcleo) no se puede probar a fondo (ver §4) |

### 2. Funciones que faltaban y se implementaron

| Función | Detalle |
|---|---|
| **JOIN / NODE_LIST** | `dsm_init` envía JOIN a los nodos no vivos y espera a todos (`wait_all_nodes = 1`, tope `join_timeout_ms`). `NODE_LIST` lleva el mapa de nodos vivos |
| **Heartbeat** | UDP en puerto propio (puerto del nodo + `heartbeat_port_offset`), así no compite con el tráfico de páginas. Marca caído tras `heartbeat_timeout_ms` y revive al volver a recibir latidos |
| **Alloc global** | Coordinador en el nodo 0 con cursor por slot, asignación contigua que puede cruzar slots, asignaciones con nombre (`dsm_alloc_named`: mismo puntero en todos los nodos), tabla (nodo, proceso, orden) y `dsm_free` |
| **`dsm_lock/unlock`** | Exclusión mutua distribuida con coordinador por hash del id y cola FIFO |
| **Write-back al apagar** | `dsm_shutdown` envía `PAGE_RELEASE`: las páginas M devuelven sus datos a la copia de referencia; las S se quitan del copyset |
| **Estadísticas y consultas** | Por nodo, tabla de páginas local y global, asignaciones, salida en texto y en HTML por nodo, y un nodo interactivo (`dsm_node`) |
| **`dsm_prefetch_write/read`** | Pide M directamente en lugar de leer y luego escribir (dos viajes) |
| **Nombres de host** | `getaddrinfo` al arrancar; `dsm.conf` acepta IP o nombre |
| **Dependencias** | Respaldo de `memfd_create` (llamada al sistema, luego `shm_open`); `MAP_FIXED_NOREPLACE` definido si falta; `make FALLBACK=1` fuerza la detección lectura/escritura por estado de página |
| **Dos aplicaciones reales** | `app_matmul` (multiplicación de matrices) y `app_counter` (arreglo concurrente + contador con lock) |

### 3. Decisiones tomadas (y por qué)

| Tema | Decisión | Motivo |
|---|---|---|
| Home inactivo vs asignaciones contiguas | **Opción (a):** `max_nodes` = nodos de la corrida y `dsm_init` espera a todos | Es la más simple y no exige migrar directorios. El JOIN dinámico queda limitado a "todos presentes al inicio" |
| Memoria por nodo | **Uniforme** (`mem_per_node`) | El archivo de configuración es compartido; mantiene simples `initial_owner` y las direcciones base |
| Heartbeat | **UDP en puerto separado** | Un latido detrás de un `PAGE_DATA` en el mismo socket TCP generaría falsos "nodo caído" |
| Tolerancia a fallos (Extra 2) | **No se implementa**; se documenta | Cuesta replicar páginas M y directorios. Sí se tiene detección, fallo rápido, reintentos y salida ordenada |
| Coordinador de alloc | **Nodo 0 fijo** | Simple. Es un punto único de falla para `dsm_alloc` (no para leer/escribir memoria ya asignada) |
| Cola por página en el directorio | Se mantiene el **mutex por franja** | Serializa por página y no hay deadlock con pools separados. Una cola explícita solo mejoraría la concurrencia entre páginas de la misma franja |
| Mejora S→M sin reenviar datos | **No se hizo** | Es un cambio de protocolo; la mitigación de los dos viajes es el prefetch |

### 4. Lo que NO se pudo verificar o sigue siendo una limitación

- **Multicore:** la máquina de desarrollo tiene **1 núcleo**. El livelock y la contención del spinlock no se pueden descartar. La batería está lista (`make test`, y `test_stress` con más hilos); hay que correrla en el laboratorio.
- **Medición de rendimiento:** con 1 núcleo los tiempos por tamaño de pool, de página y número de nodos son casi idénticos (≈0,4 s, dominado por el arranque) y **no son concluyentes**. Hay que repetir en el laboratorio.
- **Home que sale o cae:** su directorio se pierde. Al salir con `dsm_shutdown` solo se devuelven las páginas, no el directorio. Los datos de un nodo que cae en M también se pierden.
- **Barrier/lock con un participante caído:** terminan por timeout (`DSM_ERR_TIMEOUT`).
- **Lock tras timeout:** un `GRANT` tardío puede asignarse a otro solicitante del mismo id (caso límite documentado).
- **`dsm_free`:** libera el registro, pero la memoria no se reutiliza (cursor monotónico).
- **Respaldo `shm_open` y arquitecturas no x86-64:** el respaldo por estado de página se probó compilando con `FALLBACK=1` en x86-64, no en otra arquitectura. El camino de `shm_open` no se ejercitó.
- **Rango y verificación de laboratorio:** falta `uname -m`, `uname -r` y `ldd --version` en las máquinas del TEC, y una prueba entre computadoras físicas.

### 5. Errores míos durante las pruebas (ya corregidos)

- `run_n.sh` usaba `$N_$POOL` (variable inexistente) para el nombre de la configuración.
- `test_leave` escogía páginas cuyo home era justo el nodo que sale (directorio perdido por diseño) y los sobrevivientes cerraban antes de que otros terminaran de leer. Ahora usa páginas con otro home y una barrera final entre sobrevivientes.
- Una primera prueba del CLI no coordinaba los barriers y parecía fallar; era el guion, no el código.