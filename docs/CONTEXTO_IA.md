# CONTEXTO PARA UNA SESIÓN DE IA: DSM (IC-6600) — cómo entender, probar y depurar este proyecto

> Documento autosuficiente. Si eres una IA que entra sin historial: léelo completo antes de tocar código. Todo lo que dice aquí fue **verificado ejecutando el código** salvo lo marcado como **NO VERIFICADO**.

## 0. Resumen en 10 líneas

1. Es un **DSM (Distributed Shared Memory)** en **C (gnu11), Linux x86-64**: varios procesos (nodos) aportan memoria y la ven como un solo espacio con punteros normales.
2. Mecanismo: `mmap` del espacio global en la misma dirección en todos los nodos; páginas ausentes en `PROT_NONE`; el acceso genera `SIGSEGV`; el handler pide la página por TCP; se copia, se abre el permiso con `mprotect` y la instrucción se reintenta.
3. Coherencia **MSI** con directorio distribuido: el *home* de cada página = `splitmix64(page_id ^ hash_seed) % max_nodes`.
4. **Doble mapeo** del mismo `memfd`: vista de aplicación (permisos cambian) y vista del sistema (siempre RW) para que el hilo de red copie sin fallar.
5. Membresía estática (`max_nodes` = nº de líneas `node=` de `dsm.conf`), `JOIN`/`NODE_LIST`, **heartbeat UDP**, fallo rápido a nodos caídos.
6. Extras del curso: **Extra 1 (ejecución distribuida de tareas) implementado**; **Extra 2 (tolerancia a fallos) NO implementado** (por decisión).
7. Todo corre en pruebas locales: N procesos en `127.0.0.1` con puertos distintos (`scripts/run_n.sh`).
8. Estado: batería de **14 pruebas, todas PASS**, 2–8 nodos, en una máquina de **1 núcleo**.
9. **NO VERIFICADO:** multicore, computadoras físicas distintas, rendimiento, arquitecturas no x86-64, camino `shm_open`.
10. Proyecto universitario en parejas (TEC, Costa Rica): Ximena Molina y Susana Feng. Demo final 27 nov 2026 en laboratorio.

## 1. Entorno de trabajo (trampas conocidas)

| Hecho | Consecuencia |
|---|---|
| El directorio de salida `/mnt/user-data/outputs` está montado **sin permiso de ejecución** | Copiar el proyecto a `/home/claude/<dir>` antes de `make` y ejecutar. Síntoma: `Permission denied` al correr `build/...` |
| `/bin/sh` es dash: **sin expansión de llaves** (`mkdir -p a/{b,c}` crea un directorio llamado `{b,c}`), **sin `time`** | Usar comandos separados; medir con `date +%s.%N` |
| La máquina tiene **1 núcleo** (`nproc`=1) | No se ven carreras reales ni escalado; los tiempos no son concluyentes. Los resultados de reparto de tareas varían |
| Python3, gcc 13, make, bc disponibles; no hay `gdb` garantizado | Para editar archivos de forma segura usar scripts Python con `replace` y **verificar que el reemplazo se aplicó** (`grep`) |
| `Makefile` usa `.RECIPEPREFIX = >` | Las recetas empiezan con `>` en vez de tabulador |
| Procesos huérfanos tras un timeout ocupan puertos | `pkill -f build/` antes de repetir |

## 2. Estructura

```
dsm-prototipo1/
├── Makefile                 make | make test | make FALLBACK=1 | make clean
├── dsm.conf (ejemplo se genera con scripts/gen_conf.sh)
├── src/
│   ├── internal.h           CONTRATO entre módulos: tipos, mensajes, enums, API. Cambiar firmas AQUÍ primero
│   ├── config.c             parser de dsm.conf (+ DSM_NODE_ID por entorno)
│   ├── cluster.c            membresía, JOIN/NODE_LIST, heartbeat UDP, hash de home, coordinadores, carga por nodo
│   ├── network.c            listener, 2 pools, framing, net_send, RPC, timeouts
│   ├── pager.c              mmap, doble mapeo, SIGSEGV, MSI local, PUNCH_HOLE, reintentos, prefetch, release_all
│   ├── directory.c          directorio en el home: transacciones por página, FETCH, invalidaciones, write-back
│   ├── dsm.c                API pública: init/shutdown, alloc global, barrier, locks, prefetch
│   ├── stats.c              estadísticas, tablas de páginas, asignaciones, HTML
│   ├── tasks.c              Extra 1: dsm_spawn / dsm_wait / dsm_register_task
│   └── main.c               nodo interactivo (build/dsm_node)
├── apps/                    test_basic, test_stress, test_hb, test_leave, test_tasks, app_matmul, app_tasks, app_counter
├── scripts/                 gen_conf.sh, run_n.sh, run_all.sh, run_local.sh
├── tools/print_sizes.c      tamaños de estructuras
└── docs/                    ENTREGABLE1/AVANCE2/AVANCE3/CORRECCIONES/EXTRA1/SIGUIENTES_PASOS/CAMBIOS_PROTOTIPO1/este archivo
```

Documentos en español. Léelos si necesitas más detalle: `CORRECCIONES.md` (bugs corregidos y por qué), `EXTRA1.md` (tareas), `ENTREGABLE1.md` (diseño original).

## 3. Cómo compilar y probar

```sh
cp -r /mnt/user-data/outputs/dsm-prototipo1 /home/claude/dsm && cd /home/claude/dsm
make 2>&1 | grep -E "error|warning"       # debe salir vacío
./scripts/run_all.sh                       # batería completa: 14 pruebas, termina con "pruebas: 14, fallidas: 0"
```

Una prueba suelta: `./scripts/run_n.sh <binario> <nodos> <worker_pool> <puerto_base> [args...]`

| Variable de entorno | Efecto |
|---|---|
| `TMO=90` | Timeout (s) por proceso (por defecto 120). Código 124 = se colgó |
| `EXTRA_CONF="page_size = 8192"` | Líneas extra para `dsm.conf` (la última gana) |
| `USE_HOSTNAME=1` | Sustituye `127.0.0.1` por `localhost` (prueba `getaddrinfo`) |
| `DSM_VERBOSE=1` | Logs internos `[dsm nX] ...` y contadores al cerrar |

`run_n.sh` genera `build/dsm_<N>_<POOL>.conf`, lanza los nodos (del N-1 al 0) con `DSM_NODE_ID=i`, guarda `build/<binario>_n<i>.log` y sale con código ≠ 0 si **algún** nodo falla. Solo imprime las líneas que contienen PASS|FALLO|Test|test_|app_|caida|RESULTADO|balanceo|DSM_TASK|reintento: **para depurar lee siempre los logs completos** `build/*_n*.log`.

`gen_conf.sh <N> <pool> <puerto_base>`: `page_size=4096, mem_per_node=4M, max_nodes=N, worker_pool_size=pool, nowait_pool_size=4, request_timeout_ms=5000, heartbeat_interval_ms=300, heartbeat_timeout_ms=2000`, nodos `127.0.0.1 base+i`. **El heartbeat usa UDP en `puerto+1000`**: separar los `puerto_base` de pruebas simultáneas al menos 10 y nunca usar bases que colisionen con `base+1000`.

### Batería (`scripts/run_all.sh`) y salida esperada

| Prueba | Nodos / pool | Debe imprimir |
|---|---|---|
| `test_basic` | 2 / 8 | `Test A/D/B ... OK` y `RESULTADO: PASS` en ambos nodos |
| `test_stress` | 2/1, 3/1, 4/2, 8/4 | `test_stress (...): PASS` en cada nodo |
| `app_matmul` | 3/4 (N=96), 4/8 (N=128) | `[n0] app_matmul ...: PASS` |
| `app_counter` | 4/2 | `app_counter ...: PASS` en los 4 |
| `test_hb` | 3/4 | `[n0|n1] caida de nodo 2 detectada en ~2200 ms; net_send fallo rapido en 0 ms: PASS` y `[n2] caida simulada` |
| `test_leave` | 3/4 | `test_leave: 16/16 valores correctos ...: PASS` en los sobrevivientes |
| `test_tasks` | 3/4 y 4/4 | `balanceo de 30 tareas: ...`, `tarea con DSM_TASK_RETRY completada ...`, (4 nodos) `tarea sin reintento ... DSM_ERR_NODE_DOWN`, `test_tasks (N nodos): PASS` |
| `app_tasks` | 4/4 (N=96) | `[n0] app_tasks ... PASS` |
| `test_basic` con `USE_HOSTNAME=1` | 2/8 | PASS |

Códigos de salida de los binarios de prueba: `0` ok; `1` fallo de datos/`dsm_init`; `2` barrier fallido; **`70` = abort del pager por timeout de página**; `124` = lo mató `timeout`.

## 4. Arquitectura en detalle

### 4.1 Memoria y direcciones
- `total = max_nodes × mem_per_node`; `npages = total / page_size`. Base común `global_base_addr` (`0x700000000000`), mapeada con `MAP_FIXED_NOREPLACE`.
- Cada nodo **contribuye** el rango `[base + i·mem_per_node, …)` (nodo i). **Contribuyente / owner inicial** de la página p = `p·page_size / mem_per_node`. Su `memfd` guarda la **copia de referencia** (los bytes "en reposo" de la página).
- Dos vistas del mismo `memfd`: `app_base` (permisos cambian con `mprotect`) y `sys_base` (siempre RW). **El runtime nunca debe leer/escribir una página DSM por `app_base`** (un `SIGSEGV` en un hilo de red = deadlock). Usar `pager_sys_addr(p)`.
- Memoria virtual se reserva con `MAP_NORESERVE`; `PUNCH_HOLE` libera RAM de páginas invalidadas **ajenas** (nunca del propio contribuyente).

### 4.2 Estados MSI (por nodo y página, en `pager.c`, `page_local_t{state, granted_ns}`)
I (`PROT_NONE`), S (`PROT_READ`), M (`PROT_READ|PROT_WRITE`), y transitorios `PENDING_R/W` (petición en vuelo; los permisos no cambian hasta la respuesta).

### 4.3 Flujo de un fallo (pager.c)
1. `sigsegv_handler`: si `si_addr` está fuera del espacio DSM → restaura `SIG_DFL` y retorna (el fallo se repite y mata el proceso: es un bug real de la aplicación, no del DSM). Si no: lee/escritura vía `REG_ERR & 2` (x86-64) o, con `-DDSM_STATE_FALLBACK` / no-x86, por estado (`S`⇒escritura).
2. `resolve_fault(p, acc)`: toma el spinlock de la franja de `p`; si el estado ya satisface el acceso → retorna; si no, busca/crea un `fault_ctx_t` (un semáforo por fallo, `refs` = hilos esperando), pone `PENDING_R/W` y encola la petición en un anillo (`ring_push`), todo con locks por instantes. **El handler no envía red**: un hilo emisor (`sender_main`) saca del anillo y llama `net_send_page_request`.
3. `wait_fault`: `sem_timedwait(request_timeout_ms)`. Si vence: reenvía el `PAGE_REQUEST` (hasta `request_retries`, defecto 2; un duplicado es inocuo porque el directorio lo serializa como una petición normal); si se agotan: `fatal_page(...)` → escribe `"[dsm] DSM_ERR_TIMEOUT: la pagina no llego (nodo caido?) | pagina=N home=nodo M"` y `_exit(70)`.
4. `pager_on_page_data` (INLINE, hilo listener): **orden obligatorio**: copiar por `sys_base` → `mprotect` de la vista app → actualizar estado → `sem_post` por cada hilo esperando.
5. Al terminar el handler el kernel reejecuta la instrucción.

Locks del pager: 64 spinlocks por franja (`page_id % 64`) + 1 spinlock de tabla de fallos/anillo. **Orden: franja → fallos.** Son `atomic_flag` + `sched_yield` (async-signal-safe; un mutex no lo es). Se toman un instante; **jamás durante `net_*` ni esperas de semáforo**.

### 4.4 Directorio (directory.c) — una transacción por página
Entrada por página en el home: `{owner_node, copyset (bitmap), owner_dirty, init}` (se inicializa perezosamente: `owner_node = contribuyente`, `owner_dirty = 0`). Un mutex por franja de 256 serializa transacciones de la misma página. `dir_on_page_request` (clase **POOL_WAIT**):
- **Fuente de datos:** `src = owner_dirty ? owner_node : contribuyente`.
- **READ:** `FETCH(READ)` a `src` y espera `FETCH_REPLY`. Si estaba en M: el owner baja a S, `copyset |= owner`, y si `owner ≠ contribuyente` se envía `PAGE_WRITEBACK` al contribuyente; `owner_dirty=0`. `copyset |= solicitante`. Envía `PAGE_DATA(READ)`.
- **WRITE:** `FETCH(WRITE)` a `src` (el owner pasa a I). Invalida `copyset \ {solicitante, src}` con `INVALIDATE`, espera todos los ACK. `owner=solicitante, owner_dirty=1, copyset=0`. Envía `PAGE_DATA(WRITE)`.
- `PAGE_DATA` **siempre lleva la página completa** y **pasa por el home**.
- Cada transacción tiene una **generación** (`txn_t.gen`) que viaja en `req_id` de `FETCH`/`INVALIDATE`; las respuestas con otra generación se descartan (respuestas tardías de una transacción vencida). El semáforo se drena al reutilizar el `txn_t`.
- `PAGE_RELEASE` (al apagar un nodo): si el que sale es el owner en M y no es el contribuyente, devuelve el dato a la copia de referencia; siempre lo quita del `copyset`.

### 4.5 Red (network.c) y **reglas de clases de mensaje** (CRÍTICAS)
`net_register(tipo, handler, clase)`. Clases:

| Clase | Hilo | Puede… | Mensajes |
|---|---|---|---|
| `MSG_INLINE` | listener | solo copiar/actualizar/`sem_post`; **no** enviar por red ni esperar | `PAGE_DATA`, `FETCH_REPLY`, `INVALIDATE_ACK`, `PAGE_WRITEBACK`, `NODE_LIST`, `ALLOC_REPLY`/`STATS_REPLY` (RPC), `BARRIER_RELEASE`, `LOCK_GRANT`, `TASK_STATUS`, `TASK_DONE` |
| `MSG_POOL` (pool sin espera, ≥2 hilos) | worker | enviar mensajes; **nunca** esperar respuesta de red ni un lock que tenga un handler POOL_WAIT | `FETCH`, `INVALIDATE`, `JOIN`, `BARRIER_ARRIVE`, `LOCK_ACQUIRE/RELEASE`, `ALLOC_REQUEST`, `STATS_REQUEST`, `SPAWN_TASK` |
| `MSG_POOL_WAIT` (pool con espera, `worker_pool_size`) | worker | bloquearse esperando `FETCH_REPLY`/ACK | `PAGE_REQUEST`, `PAGE_RELEASE` |

**Por qué:** la respuesta a un handler POOL_WAIT la procesa el listener (INLINE) o el pool sin espera, **nunca otro POOL_WAIT**, así que agotar `worker_pool_size` no puede producir deadlock. El Prototipo 1 tenía un solo pool y hacía timeout con `worker_pool_size=1`. **Si mueves un handler de clase, re-ejecuta `test_stress` con pool=1.**

Otros hechos de red:
- Framing: encabezado fijo `msg_header_t` (22 bytes, packed, big-endian en el cable) + `payload_len` (validado ≤ `max_payload` = `page_size + 4352`). `read_exact` repite `recv`.
- Una conexión TCP saliente perezosa por destino (`out_fd[dst]`, mutex por destino), `connect()` no bloqueante con `connect_timeout_ms` (500), `SO_SNDTIMEO = request_timeout_ms`, reconexión una vez si falla el envío. Los sockets aceptados tienen `SO_RCVTIMEO` de 3 s.
- **TCP es FIFO por conexión**: `PAGE_DATA` llega antes que un `INVALIDATE`/`FETCH` posterior del mismo home. Varias garantías de corrección dependen de esto.
- `net_send(dst == self)` despacha localmente: INLINE se llama directo; POOL/POOL_WAIT se encolan.
- **Fallo rápido:** `net_send` a un nodo marcado caído devuelve `DSM_ERR_NODE_DOWN` sin conectar, salvo `JOIN`/`NODE_LIST`/`HEARTBEAT` (exentos).
- RPC (`net_rpc_call`): usa `req_id` propio; las respuestas `ALLOC_REPLY` y `STATS_REPLY` las empareja `rpc_on_reply`.
- Usos sobrecargados de campos del encabezado: `page_id` = id de página **o** de barrier/lock/tarea; `req_id` = id de RPC, generación de transacción, `n` en `BARRIER_ARRIVE`, y en el heartbeat = tareas en cola/ejecución; en el heartbeat `page_id` = carga CPU×100; `access` = READ/WRITE o código de estado de tarea.

### 4.6 Membresía (cluster.c)
- La configuración (`config.c`) es inmutable; `cluster.c` guarda solo estado mutable en atómicos: `alive[]`, `last_hb_ms[]`, carga reportada.
- `dsm_init` → `cluster_join()`: envía `JOIN` a nodos no vivos cada 200 ms hasta que todos estén vivos o venza `join_timeout_ms` (defecto 30 s) → `DSM_ERR_TIMEOUT`. `wait_all_nodes=0` solo hace un intento.
- Heartbeat UDP cada `heartbeat_interval_ms` a `puerto_del_nodo + heartbeat_port_offset(1000)`; sin latido durante `heartbeat_timeout_ms` ⇒ nodo caído (`nodo N marcado CAIDO` en verbose). Un latido de un nodo caído lo revive.
- Home = `splitmix64(page ^ hash_seed) % max_nodes`. **Decisión de diseño:** `max_nodes` = nodos de la corrida (todos deben estar al inicio), porque un home inactivo rompería las asignaciones contiguas.
- Coordinadores: alloc = nodo 0 fijo; barrier = `splitmix64(id ^ seed) % max_nodes`; lock = `splitmix64(id ^ seed ^ 0x10c4) % max_nodes`. Si el coordinador está caído, esa operación falla/expira.

### 4.7 API y módulos de `dsm.c`
- `dsm_init` orden: config → cluster → net_init → pager_init → dir_init → handlers de barrier/lock/alloc → `cluster_register` → `stats_init` → `tasks_init` → `net_start` → `cluster_start_heartbeat` → `cluster_join` (espera a todos).
- `dsm_shutdown` orden: `tasks_shutdown` → `pager_release_all` (devuelve páginas M/S al home) → `pager_shutdown` → `cluster_shutdown` → `net_shutdown`.
- **Alloc:** `dsm_alloc[_named]` es un RPC al nodo 0, que lleva un cursor por slot y una tabla `{nodo, pid, orden, dirección, tamaño, nombre, viva}`; prefiere el slot del solicitante, luego otros vivos; si no cabe, intenta un rango contiguo que cruza slots vecinos libres. Nombre repetido ⇒ **mismo puntero** (así los nodos comparten arreglos). `dsm_free` marca la entrada pero **no reutiliza memoria**.
- **Barrier:** un solo `dsm_barrier` activo por nodo (mutex), un semáforo; `n` viaja en `req_id`. Los participantes que no existen/cayeron producen `DSM_ERR_TIMEOUT` tras `barrier_timeout_ms` (60 s por defecto).
- **Locks:** coordinador con cola FIFO; `GRANT` despierta por id.
- **Prefetch:** `dsm_prefetch_write/read(ptr,len)` piden permiso sin esperar un fallo (evita los dos viajes lectura+escritura).

### 4.8 Extra 1: tareas (tasks.c)
`dsm_register_task(id, fn)` en todos los nodos (mismo binario) **antes** de `dsm_init`; `dsm_spawn(id, args≤256B, nodo|DSM_ANY_NODE, flags, &t)`, `dsm_wait(t,&res)`. Mensajes `SPAWN_TASK` (POOL: solo encola) / `TASK_STATUS` / `TASK_DONE`. Ejecutores propios (`task_pool_size`=4), separados de los pools de red. Planificador: puntaje = tareas×1000 + CPU; para remotos usa `max(reportado por latidos, enviado y pendiente)`; nodo propio: carga local exacta. `DSM_TASK_RETRY`: si cae el nodo que ejecuta, `dsm_wait` reenvía a otro (hasta `task_max_retries`, semántica al-menos-una-vez ⇒ tareas idempotentes); sin flag → `DSM_ERR_NODE_DOWN`. Un `TASK_DONE` de un nodo que ya no es el destino se ignora. Detalle: `docs/EXTRA1.md`.

## 5. Configuración (`dsm.conf`)

Formato `clave = valor`, `#` comenta, tamaños con K/M/G. **Todos los nodos usan el mismo archivo.** El id propio viene de la variable de entorno **`DSM_NODE_ID`** (si falta: `config: falta variable de entorno DSM_NODE_ID`).

`page_size`(4096) `mem_per_node`(16M) `max_nodes`(2; debe == nº de líneas `node=`) `global_base_addr` `worker_pool_size`(8) `nowait_pool_size`(4, mínimo 2) `request_timeout_ms`(5000) `request_retries`(2) `connect_timeout_ms`(500) `join_timeout_ms`(30000) `barrier_timeout_ms`(60000) `heartbeat_interval_ms`(1000) `heartbeat_timeout_ms`(5000) `heartbeat_port_offset`(1000) `min_hold_us`(200) `release_memory`(1) `wait_all_nodes`(1) `task_pool_size`(4) `task_timeout_ms`(0=barrier) `task_max_retries`(3) `hash_seed`(0) `max_payload` y `node = <id> <host|ip> <puerto>`.

Tamaños (x86-64): `msg_header_t` 22, `node_info_t` 112, `dsm_config_t` 1904, `page_local_t` 16, `dir_entry_t` 24, `alloc_entry_t` 64, `node_stats_t` 152 (`./build/print_sizes`).

## 6. Invariantes: NO los rompas

1. El runtime solo toca páginas por `pager_sys_addr`. Nunca por la vista de aplicación.
2. Ningún handler `MSG_INLINE` envía por red ni espera. Ningún handler `MSG_POOL` espera respuestas de red.
3. Solo `PAGE_REQUEST` y `PAGE_RELEASE` (POOL_WAIT) esperan `FETCH_REPLY`/ACK.
4. Nunca llamar a `net_*` con un spinlock del pager tomado. Orden de locks del pager: franja → fallos.
5. En `pager_on_page_data`: datos → `mprotect` → estado → `sem_post`. En `pager_on_fetch`: permiso primero, **luego** la copia.
6. `PAGE_WRITEBACK` es INLINE (si fuera pool, un `FETCH` posterior podría leer la copia vieja).
7. `punch()` jamás sobre páginas cuyo contribuyente es este nodo (borraría la copia de referencia). Y siempre **después** de copiar los datos.
8. El handler de `SIGSEGV` solo usa funciones async-signal-safe (sin `malloc`, `printf`, mutex).
9. Cambios de firma/enum: primero en `internal.h`; mantener `_Static_assert(sizeof(msg_header_t)==22)`.
10. Todo envío/espera tiene timeout. No introducir esperas infinitas.
11. Los hilos de aplicación nunca deben retener un lock del runtime mientras tocan memoria DSM.

## 7. Guía de diagnóstico (síntoma → causa → qué hacer)

Primero: **reproducir** con `./scripts/run_n.sh <bin> <N> <pool> <puerto> ...`, abrir **todos** los `build/<bin>_n*.log`, y repetir con `DSM_VERBOSE=1`.

| Síntoma | Causas probables | Acciones |
|---|---|---|
| `[dsm] DSM_ERR_TIMEOUT: la pagina no llego (nodo caido?) \| pagina=N home=nodo M`, salida **70** | (a) el nodo **home M** o el owner/contribuyente **ya salió o cayó** (muy común en pruebas sin barrier final); (b) la transacción del directorio falló (`FETCH pag ... fallo/timeout`, `ACKs de pag ... incompletos` en verbose); (c) regresión de deadlock de pool; (d) nodo aún no arrancado | Buscar en los logs qué nodo terminó antes. Calcular `cluster_home_of(N)` y el contribuyente (`N*page_size/mem_per_node`). Verificar con `pool=1`. Si salió antes: añadir barrier final entre quienes siguen vivos |
| `dsm_init fallo` / `join` timeout | Nodos no arrancados a tiempo; `max_nodes` ≠ líneas `node=`; puertos ocupados; falta `DSM_NODE_ID` | Leer el mensaje de `config:`/`net_start bind/listen`/`heartbeat bind`. `pkill -f build/`; cambiar `puerto_base` |
| `heartbeat bind` o `net_start bind/listen` falla | Puerto TCP `base+i` o UDP `base+1000+i` ocupado/colisión entre pruebas | Cambiar base; dejar ≥10 entre bases |
| `mmap ... MAP_FIXED_NOREPLACE` falla | Dirección base ocupada o kernel < 4.17 | Cambiar `global_base_addr` en todos los nodos |
| Barrier/lock devuelve `DSM_ERR_TIMEOUT` (tras 60 s) | Un participante no llegó/cayó; `n` distinto entre nodos; el coordinador (`cluster_barrier_coord(id)`) está caído | Al diseñar pruebas con nodos que mueren, elegir un id de barrier cuyo coordinador sea un sobreviviente (`while (cluster_barrier_coord(id)==victim) id++`) |
| Valores incorrectos (`FALLO ...: got=X want=Y`) | Error de coherencia: orden en `on_page_data`/`on_fetch`, write-back no INLINE, `punch` sobre contribuyente, respuesta tardía sin chequeo de generación, página tocada por la vista de aplicación | Revisar invariantes §6. Reproducir con 2 nodos y `test_basic`; luego `test_stress` con pool=1 |
| Cuelgue (código 124, sin mensajes) | Deadlock: handler que cambió de clase; mutex tomado esperando red; spinlock con `net_*` dentro | Revisar §4.5/§6. `ps`/`/proc/<pid>/task/*/stack` o `gdb -p`/`pstack` si hay. Ejecutar con `request_timeout_ms` pequeño para convertir el cuelgue en timeout visible |
| `SIGSEGV` real (core/"Segmentation fault") | Acceso fuera del espacio DSM (bug de la app); desbordar `max_payload`; uso de `malloc`/punteros locales | El handler restaura `SIG_DFL` para direcciones ajenas: es un bug normal, no del DSM. Verificar con `gdb`/prints |
| `test_leave`: lecturas incorrectas o timeout en sobrevivientes | Páginas cuyo **home es el nodo que sale** (su directorio se pierde por diseño, limitación Q-G); sobrevivientes que cierran antes que otros terminen de leer | La prueba usa páginas con otro home y un barrier final entre sobrevivientes. No "arreglar" el diseño |
| `test_hb`: no detecta caída / falla `alive_count` | Latidos UDP bloqueados/puerto ocupado; `heartbeat_timeout_ms` grande; un nodo no vivo tras `dsm_init` | La detección tarda ≈ `heartbeat_timeout_ms` (2 s en pruebas) |
| `test_tasks`: `FALLO 3 nodo X no recibio ninguna tarea` o reparto muy desigual | En 1 núcleo, remotos confirman tarde ⇒ el planificador carga al nodo que termina antes (observado 6–28 en 3 nodos, parejo en 4). La prueba exige solo ≥1 tarea por nodo | Si falla de verdad: revisar `pick_node` (`tasks.c`), heartbeats (`req_id`/`page_id`) y `cluster_set_local_tasks` |
| `test_tasks`: `FALLO 6` o espera larga | Reintento depende del heartbeat (~1,9–2,1 s). Si el coordinador del barrier final es una víctima, falla | Mantener la lógica `endbar` |
| `Permission denied` al ejecutar | Directorio sin permiso de ejecución | Copiar a `/home/claude` |
| Muchas fallas simultáneas tras un cambio | Procesos huérfanos de una corrida anterior ocupando puertos | `pkill -f build/` y repetir |
| `config: parametro desconocido 'x'` | Línea no soportada en `dsm.conf` (se ignora, solo avisa) | Revisar ortografía o añadir en `config.c` y `internal.h` |
| `PAGE_DATA invalido` / `DSM_ERR_PROTO` en verbose | `payload_len ≠ page_size` o tipo inválido; `page_size` distinto entre nodos | Verificar que todos usan el mismo `dsm.conf` |
| Compilación falla en `make` | Falta `-lm` en apps con `fabs`; `.RECIPEPREFIX` no soportado (make < 3.82) | Ver `Makefile` |

### Cómo aislar
1. 2 nodos, `test_basic`: confirma lo básico (MSI + barrier).
2. `test_stress 3 1`: confirma ausencia de deadlock por pool.
3. `test_stress` con `EXTRA_CONF="page_size = 8192"`: sensibilidad a tamaño de página.
4. `make FALLBACK=1`: valida la detección lectura/escritura por estado (en x86-64).
5. Repetir 15–30 veces para cazar intermitentes (bucle `for i in $(seq 1 15)` con puertos distintos por iteración).
6. `dsm_node` interactivo (`stats`, `local`, `global`, `allocs`, `html`) para inspeccionar estados y directorio de una corrida a mano.

## 8. Historial de bugs encontrados (para no reintroducirlos)

| Bug | Corrección |
|---|---|
| Deadlock con `worker_pool_size` pequeño (un solo pool) | Dos pools (POOL_WAIT / POOL) |
| `FETCH_REPLY` tardío caía en una transacción reutilizada | Generación en `req_id` + drenado del semáforo |
| `PUNCH_HOLE` habría borrado la copia de referencia | Excluir páginas del propio contribuyente; punch después de copiar |
| Write-back procesado por el pool podía llegar después de un `FETCH` | `PAGE_WRITEBACK` INLINE |
| Handler de SIGSEGV hacía `connect/send` | Hilo emisor + anillo prealocado |
| Timeout de una página mataba el proceso a la primera | Reintentos (`request_retries`) antes de `fatal_page` |
| Nodo caído congelaba a quien enviaba | `connect` no bloqueante, `SO_SNDTIMEO`, fallo rápido por heartbeat |
| Dos copias de la configuración | Puntero único a config inmutable; estado mutable en atómicos |
| Planificador contaba dos veces tareas propias/remotas | Nodo propio: carga exacta; remotos: `max(reportado, enviado)` |
| Pruebas mal diseñadas (no son bugs del DSM) | `run_n.sh` con `$N_` (variable inexistente); `test_leave` con páginas homed en el nodo que sale y sin barrier final; guion de CLI sin coordinar barriers |

## 9. Limitaciones conocidas y decisiones tomadas (no son bugs)

- Extra 2 **no implementado**: si cae un nodo con páginas en M, ese dato se pierde; si cae un home, se pierde su directorio; un nodo que sale con `dsm_shutdown` devuelve sus páginas pero no su directorio.
- `max_nodes` = nodos de la corrida; `mem_per_node` uniforme; coordinador de alloc fijo en el nodo 0.
- `dsm_free` no reutiliza memoria. `PAGE_DATA` siempre lleva 4 KB (sin mejora S→M sin datos).
- Cola explícita por página en el directorio: no hay; se usa mutex por franja.
- Tareas: argumentos ≤256 B, resultado `int64`, sin cancelación; reintento al-menos-una-vez; cuidado con `task_pool_size` y tareas anidadas.
- Barrier: un `dsm_barrier` activo por nodo. Lock: un `GRANT` tardío tras timeout puede asignarse a otro solicitante del mismo id.
- **NO VERIFICADO:** ejecución multicore (posible livelock/contención de spinlock; mitigación `min_hold_us`), computadoras físicas distintas, rendimiento (los tiempos en 1 núcleo ≈0,4 s dominados por el arranque; no son concluyentes), `shm_open` como respaldo de `memfd_create`, arquitecturas no x86-64 (el respaldo por estado solo se probó en x86-64 con `make FALLBACK=1`), nombres de host distintos de `localhost`, 16 nodos.

## 10. Contexto del curso (para priorizar)

- Curso IC-6600 Principios de Sistemas Operativos, TEC, II-2026. Proyecto en parejas; la **nota es individual y depende de la defensa de todo el sistema**.
- Entregas: Entregable 1 (6 nov, diseño), Avance 2/Prototipo 1 (13 nov), Avance 3/Prototipo 2 (20 nov), **Demo 27 nov** en el laboratorio del TEC con computadoras físicas (no personales). Se valida el historial de Git (commits de ambas personas).
- Requisitos conocidos: MSI con páginas I/S/M (estados extra permitidos si se justifican), semáforos/mutex para tablas, archivo de configuración, heartbeat, estadísticas y consultas (texto o HTML por nodo), dos aplicaciones reales usando solo el API (no `malloc`), informe con rendimiento (pool 1/4/8/16, tamaños de página, nº de nodos) y manual de usuario.
- El enunciado original del curso **no estaba disponible** al implementar el Extra 1; sus requisitos exactos no están verificados.
- Si arreglas algo: mantén compatibilidad con `scripts/run_all.sh` (debe seguir en 14/14), documenta el cambio en `docs/CORRECCIONES.md` (qué, por qué, evidencia) y no cambies firmas sin tocar primero `internal.h`.

## 11. Comandos útiles

```sh
make clean && make 2>&1 | grep -E "error|warning"
./scripts/run_all.sh                                          # batería (≈1 min)
TMO=60 ./scripts/run_n.sh test_stress 3 1 12000 600 8         # 3 nodos, pool=1
DSM_VERBOSE=1 TMO=60 ./scripts/run_n.sh test_tasks 4 4 12100  # con logs internos
cat build/test_tasks_n0.log build/test_tasks_n1.log           # logs completos
EXTRA_CONF="page_size = 8192" ./scripts/run_n.sh test_stress 3 8 12200 600 8
make clean && make FALLBACK=1 && ./scripts/run_n.sh test_basic 2 8 12300   # respaldo por estado
./build/print_sizes                                           # tamaños de estructuras
pkill -f build/                                               # matar procesos huérfanos
# nodo interactivo (2 terminales): DSM_NODE_ID=0|1 ./build/dsm_node build/dsm_2_8.conf
```
