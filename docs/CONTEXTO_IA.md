# CONTEXTO PARA UNA SESIÓN DE IA: DSM (IC-6600) — cómo entender, probar y depurar este proyecto

> Documento autosuficiente. **Público:** la persona que ejecutará las pruebas en el laboratorio **y la IA que la acompañe**, sin historial previo ni acceso a los autores. Si eres esa IA: léelo completo antes de tocar código. Lo referente al código se verificó **leyendo las fuentes versionadas** (se citan `archivo:línea`), pero la ejecución **no**: no asumas resultados, ejecuta y captura evidencia. Lo no verificado está marcado como **NO VERIFICADO**. En este árbol **no están los scripts ni la mayoría de las apps** de la batería que describen `README.md` y `docs/AVANCE3.md`; no hay `build/` y no hay `gcc`/`make` en el entorno de edición. Las secciones §12-§13 y el Apéndice A son el protocolo: **quien corra las pruebas debe llenar el Apéndice A con logs reales y corregir este documento si la evidencia lo contradice**. Los autores originales no tienen los logs del laboratorio.

## 0. Resumen en 10 líneas

1. Es un **DSM (Distributed Shared Memory)** en **C (gnu11), Linux x86-64**: varios procesos (nodos) aportan memoria y la ven como un solo espacio con punteros normales (`README.md:1-16`).
2. Mecanismo: `mmap` del espacio global en la misma dirección en todos los nodos; páginas ausentes en `PROT_NONE`; el acceso genera `SIGSEGV`; el handler encola la petición; un hilo la pide por TCP; se copia, se abre el permiso con `mprotect` y se reintenta (`pager.c:150-196`, `network.c:269-277`).
3. Coherencia **MSI** con directorio distribuido: el *home* de cada página = `splitmix64(page_id ^ hash_seed) % max_nodes` (`cluster.c:78`).
4. **Doble mapeo** del mismo `memfd`: vista de aplicación (permisos cambian) y vista del sistema (siempre RW) para que el hilo de red copie sin fallar (`pager.c:287-311`, `internal.h:16`).
5. Membresía estática (`max_nodes` = nº de líneas `node=`; `config.c:68-70`), `JOIN`/`NODE_LIST`, **heartbeat UDP**, fallo rápido a nodos caídos (`cluster.c:107-158`, `network.c:263-264`).
6. Extras del curso: **Extra 1 (ejecución distribuida de tareas) implementado** (`tasks.c`); **Extra 2 (tolerancia a fallos) NO implementado** por decisión (`AVANCE3.md:98`).
7. El único script presente es `scripts/run_local.sh`: lanza **2 nodos** de `test_basic` en `127.0.0.1:9101/9102` y verifica códigos de salida (`run_local.sh:1-14`). Para más nodos hay que lanzar procesos manualmente con `DSM_NODE_ID`.
8. **Ya no hay una batería ejecutable en el repo**: faltan `scripts/run_all.sh`, `run_n.sh`, `gen_conf.sh` y 6 de las 8 apps que menciona el `Makefile:9`. Las docs difieren: `AVANCE3.md:14` dice 11 pruebas PASS y `README.md:41` dice 14.
9. **NO VERIFICADO:** compilación/ejecución en este entorno; la ejecución en laboratorio la hará otra persona con una IA (§12), que debe dejar su evidencia en el Apéndice A. Tampoco verificados: multicore, computadoras físicas distintas, rendimiento, arquitecturas no x86-64, camino `shm_open`, nombres de host distintos de `localhost`, 16 nodos.
10. Proyecto universitario en parejas (TEC, Costa Rica): Ximena Molina y Susana Feng (`README.md:4`). Demo final 27 nov 2026 en laboratorio (`EXTRA1.md:1`, `AVANCE1.md:450`).

## 1. Realidad del repositorio (trampas conocidas)

| Hecho | Consecuencia |
|---|---|
| Hay 24 archivos versionados, la raíz es `D:\Desktop\SistemaDSM` (no `dsm-prototipo1/`) | Ignora las rutas `cd dsm-prototipo1` de `README.md:39`; trabaja desde la raíz real |
| `Makefile:9` declara 10 binarios, pero en `apps/` solo existen `test_basic.c`, `test_task.c` (singular) y `app_tasks.c` | `make` (target `all`) no puede generar `test_stress`, `test_hb`, `test_leave`, `test_tasks`, `app_matmul`, `app_counter`: falta `apps/%.c` para la regla `Makefile:22-23`. `test_task.c` sí tiene fuente pero **no está en `BINS`** |
| `Makefile:25-26` (`make test`) ejecuta `./scripts/run_all.sh`, que no existe | `make test` falla; usa `scripts/run_local.sh` o lanza nodos a mano |
| `Makefile:1` usa `.RECIPEPREFIX = >` | Las recetas empiezan con `>` en vez de tabulador |
| `FALLBACK` es una variable, no un target (`Makefile:5-6`) | `make FALLBACK=1` ejecuta `all` añadiendo `-DDSM_STATE_FALLBACK` |
| `-lm` solo está en la regla de apps (`Makefile:23`) | No está en `dsm_node` (`Makefile:17`) ni en `print_sizes` (`Makefile:20`) |
| No hay directorio `build/` ni compiladores en el entorno de edición | Los tamaños y salidas de esta guía son estáticos; para ejecutar hay que compilar en Linux |
| La máquina de desarrollo tiene **1 núcleo** (`README.md:165`, `AVANCE2.md:43`, `AVANCE3.md:105`) | No se ven carreras reales ni escalado; el reparto de tareas varía y los tiempos no son concluyentes |
| `DSM_NODE_ID` es obligatorio (`config.c:82-83`) | Sin él: `config: falta variable de entorno DSM_NODE_ID` |
| Procesos huérfanos tras una prueba ocupan puertos | `pkill -f build/` antes de repetir (el puerto TCP del nodo i es `base+i`; UDP es `+1000`) |

## 2. Estructura real

```
D:\Desktop\SistemaDSM\
├── Makefile                 make | make test (roto) | make FALLBACK=1 | make clean
├── dsm.conf                 2 nodos: 127.0.0.1 9101 / 9102 (config.c tiene los defaults)
├── README.md                guía general (contiene referencias desactualizadas, ver §1)
├── src/
│   ├── internal.h           CONTRATO entre módulos: tipos, mensajes, enums, API. Cambiar firmas AQUÍ primero
│   ├── config.c             parser de dsm.conf (+ DSM_NODE_ID) y TODOS los defaults
│   ├── cluster.c            membresía, JOIN/NODE_LIST, heartbeat UDP, hash de home, coordinadores, carga
│   ├── network.c            listener, 2 pools, framing, net_send, RPC, timeouts
│   ├── pager.c              mmap, doble mapeo, SIGSEGV, MSI local, PUNCH_HOLE, reintentos, prefetch
│   ├── directory.c          directorio en el home: transacciones por página, FETCH, invalidaciones, write-back
│   ├── dsm.c                API pública: init/shutdown, alloc global, barrier, locks, prefetch
│   ├── stats.c              estadísticas, tablas de páginas, asignaciones, HTML
│   ├── tasks.c              Extra 1: dsm_spawn / dsm_wait / dsm_register_task
│   └── main.c               nodo interactivo (build/dsm_node): stats|local|global|allocs|html|...
├── apps/                    test_basic.c, test_task.c, app_tasks.c  (solo estos 3)
├── scripts/                 run_local.sh  (solo este)
├── tools/print_sizes.c      tamaños de estructuras (usa copias privadas de page_local_t/dir_entry_t)
└── docs/                    AVANCE1.md, AVANCE2.md, AVANCE3.md, EXTRA1.md, TODO.md, CONTEXTO_IA.md
```

Archivos **mencionados en documentos pero inexistentes** (no buscarlos): `scripts/gen_conf.sh`, `scripts/run_n.sh`, `scripts/run_all.sh`; `apps/test_stress.c`, `test_hb.c`, `test_leave.c`, `test_tasks.c`, `app_matmul.c`, `app_counter.c`; `docs/CORRECCIONES.md`, `ENTREGABLE1.md`, `SIGUIENTES_PASOS.md`, `CAMBIOS_PROTOTIPO1.md`. El contenido de bugs/cambios está en `AVANCE3.md:58-118` y `AVANCE2.md:51-60`. `EXTRA1.md` y `TODO.md` son la misma lista de "siguientes pasos" (`EXTRA1.md:1` = `TODO.md:1`), no una especificación de tareas.

Documentos en español. Léelos si necesitas más detalle: `AVANCE3.md` (diseño del Prototipo 2, correcciones, decisiones y limitaciones), `AVANCE2.md` (Prototipo 1), `AVANCE1.md` (diseño original).

## 3. Cómo compilar y probar

```sh
# make (target all) intenta los 10 binarios de Makefile:9 y se detiene en el primero sin fuente;
# para generar solo lo que existe:
make build/dsm_node build/print_sizes build/test_basic build/test_task build/app_tasks
make FALLBACK=1 build/test_basic          # añade -DDSM_STATE_FALLBACK
./scripts/run_local.sh dsm.conf           # requiere build/test_basic
```

`run_local.sh`: sitúa la raíz del repo, usa `CONF=${1:-dsm.conf}`, lanza `DSM_NODE_ID=1` y `DSM_NODE_ID=0` con `./build/test_basic "$CONF"`, guarda `build/n1.log` / `build/n0.log`, imprime ambos logs y `codigos de salida: n0=$R0 n1=$R1`; exit 0 solo si ambos son 0 (`run_local.sh:3-14`). **No busca la cadena PASS** pese a su comentario `:2`. Los puertos vienen de `dsm.conf:12-13` (9101/9102).

Para otro binario o más nodos, lanza un proceso por nodo con `DSM_NODE_ID=i ./build/<bin> dsm.conf [args...]`, con `max_nodes` y líneas `node=` coherentes (`config.c:68-70`).

Variables de entorno reales: `DSM_NODE_ID` (obligatoria) y `DSM_VERBOSE` (activa `DSM_LOG` y los contadores al cerrar, `dsm.c:268`, `dsm.c:295-301`). **No existen** los atajos `TMO`, `EXTRA_CONF`, `USE_HOSTNAME` ni los scripts que los usaban; cambia `dsm.conf` directamente.

### Apps presentes y salidas reales

| App | Salida |
|---|---|
| `test_basic.c` | `[n%u] Test A (ping-pong x%d): OK/FALLO` (`:46`), `Test D (pagina del contribuyente)` (`:56`), `Test B (estres, %d hilos/nodo, misma pagina)` (`:66`), `[n%u] RESULTADO: PASS/FALLO` (`:70`) |
| `test_task.c` (archivo en singular) | `[n0] balanceo de 30 tareas: ...` (`:62`), `tarea con DSM_TASK_RETRY completada ...` (`:78`), `tarea sin reintento: nodo %u cayo => DSM_ERR_NODE_DOWN` (`:82`), `[n0] test_tasks (%u nodos): PASS/FALLO` (`:88`) |
| `app_tasks.c` | `[n0] app_tasks N=%d, %d tareas en %u nodos (...), %llu ms: PASS/FALLO` (`:44-46`), N por defecto 96 (`:20`) |

### Batería documentada, no reproducible en este árbol

`AVANCE3.md:14-23` describe **11 pruebas** (test_basic, test_stress 2/3/4/8 nodos, app_matmul, app_counter, test_hb, test_leave); `README.md:41` afirma **14**; la tabla del documento original sumaba 14 ejecuciones añadiendo tareas. Ninguna de esas fuentes está versionada, así que esas salidas (`test_stress (...): PASS`, `app_matmul ...: PASS`, `caida de nodo 2 detectada`, `test_leave: 16/16 valores correctos`) **no se pueden reproducir aquí**. No hay commit que documente el paso de 11 a 14.

Códigos de salida: las apps presentes devuelven `0` (PASS) / `1` (dato o `dsm_init`) / `2` (barrier); `pager.c:75` usa `_exit(70)` en timeout de página; `124` solo podría venir de un `timeout` externo (no hay wrapper en el repo).

## 4. Arquitectura en detalle

### 4.1 Memoria y direcciones
- `total = max_nodes × mem_per_node`; `npages = total / page_size` (`pager.c:300`). Base común `global_base_addr` (`0x700000000000`, `config.c:26`), mapeada con `MAP_FIXED_NOREPLACE` (`pager.c:311`).
- Cada nodo **contribuye** el rango `[base + i·mem_per_node, …)` (`config.c:80`). **Contribuyente / owner inicial** de la página p = `p·page_size / mem_per_node` (`cluster.c:79`). Su `memfd` guarda la **copia de referencia**.
- Dos vistas del mismo `memfd`: `app_base` (permisos cambian con `mprotect`) y `sys_base` (siempre RW), ambas con `MAP_NORESERVE` (`pager.c:310-311`). **El runtime nunca debe leer/escribir una página DSM por `app_base`** (un `SIGSEGV` en un hilo de red = deadlock); usar `pager_sys_addr(p)` (`pager.c:332`, `directory.c:131`).
- Respaldo `memfd_create` con `shm_open` como fallback (`pager.c:287-294`, camino NO ejercitado). `PUNCH_HOLE` libera RAM de páginas invalidadas **ajenas** (`pager.c:180-181`).

### 4.2 Estados MSI (por nodo y página, en `pager.c:33`, `page_local_t{state, granted_ns}`)
I (`PROT_NONE`), S (`PROT_READ`), M (`PROT_READ|PROT_WRITE`), y transitorios `PENDING_R/W` (petición en vuelo; los permisos no cambian hasta la respuesta). Enums en `internal.h:43-45`.

### 4.3 Flujo de un fallo (pager.c)
1. `sigsegv_handler`: si `si_addr` está fuera del espacio DSM → restaura `SIG_DFL` y retorna (el fallo se repite y mata el proceso: es un bug de la aplicación, no del DSM) (`pager.c:150-153`). Si no: lee/escritura vía `REG_ERR & 2` (x86-64) o, con `-DDSM_STATE_FALLBACK`, por estado (`S`⇒escritura) (`pager.c:139-143`).
2. `resolve_fault(p, acc)`: toma el spinlock de la franja de `p`; si el estado ya satisface el acceso → retorna; si no, busca/crea un `fault_ctx_t` (semáforo por fallo, `refs` = hilos esperando), pone `PENDING_R/W` y encola en un anillo (`ring_push`), todo con locks por instantes. **El handler no envía red**: `sender_main` saca del anillo y llama `net_send_page_request` (`pager.c:118-133`, `:163-172`).
3. `wait_fault`: `sem_timedwait(request_timeout_ms)` (`:104`). Si vence: reenvía `PAGE_REQUEST` hasta `request_retries` (defecto 2); si se agotan: `fatal_page("[dsm] DSM_ERR_TIMEOUT: la pagina no llego (nodo caido?)", ...)` compone `" | pagina=N home=nodo M"` (`:70-72`) y `_exit(70)` (`:75`).
4. `pager_on_page_data` (INLINE, hilo listener): **orden obligatorio**: copiar por `sys_base` → `mprotect` de la vista app → actualizar estado → `sem_post` por cada hilo esperando (`pager.c:190-196`).
5. Al terminar el handler el kernel reejecuta la instrucción.

Locks del pager: 64 spinlocks por franja (`page_id % 64`) + 1 spinlock de tabla de fallos/anillo (`pager.c:29`, `:44-45`). **Orden: franja → fallos.** Son `atomic_flag` + `sched_yield` (async-signal-safe; un mutex no lo es) (`:54-57`). Se toman un instante; **jamás durante `net_*` ni esperas de semáforo**.

### 4.4 Directorio (directory.c) — una transacción por página
Entrada por página en el home: `{owner_node, copyset (bitmap), owner_dirty, init}` (`directory.c:19`), inicializada perezosamente con `owner_node = contribuyente`, `owner_dirty = 0` (`:71-73`). Un mutex por franja de 256 serializa transacciones de la misma página (`:16`, `:79`). `dir_on_page_request` (clase **POOL_WAIT**, `:170`):
- **Fuente de datos:** `src = owner_dirty ? owner_node : contribuyente` (`:86`).
- **READ:** `FETCH(READ)` a `src` y espera `FETCH_REPLY`. Si estaba en M: el owner baja a S, `copyset |= owner`, y si `owner ≠ contribuyente` se envía `PAGE_WRITEBACK` al contribuyente; `owner_dirty=0`. `copyset |= solicitante`. Envía `PAGE_DATA(READ)` (`:88-99`).
- **WRITE:** `FETCH(WRITE)` a `src` (el owner pasa a I). Invalida `copyset \ {solicitante, src}` con `INVALIDATE`, espera ACK. `owner=solicitante, owner_dirty=1, copyset=0`. Envía `PAGE_DATA(WRITE)` (`:101-110`).
- `PAGE_DATA` **siempre lleva la página completa** y **pasa por el home** (`:67-69`).
- Cada transacción tiene una **generación** (`txn_t.gen`) que viaja en `req_id` de `FETCH`/`INVALIDATE`; respuestas de otra generación se descartan y el semáforo se drena al reutilizar `txn_t` (`:39-41`, `:117-127`).
- `PAGE_RELEASE` (al apagar): si el que sale es owner en M y no es el contribuyente, devuelve el dato a la copia de referencia; siempre lo quita del `copyset` (`:142-147`).

### 4.5 Red (network.c) y **reglas de clases de mensaje** (CRÍTICAS)
`net_register(tipo, handler, clase)` (`network.c:95-97`). Clases (`internal.h:170`):

| Clase | Hilo | Puede… | Mensajes (registros) |
|---|---|---|---|
| `MSG_INLINE` | listener | copiar/actualizar/`sem_post`; **no** enviar por red ni esperar respuestas (sí toma mutex muy breves: `network.c:156`, `directory.c:50`) | `PAGE_DATA` (`pager.c:318`), `FETCH_REPLY`/`INVALIDATE_ACK`/`PAGE_WRITEBACK` (`directory.c:172-174`), `NODE_LIST` (`cluster.c:108`), `ALLOC_REPLY`/`STATS_REPLY` (`network.c:198-199`), `BARRIER_RELEASE`/`LOCK_GRANT` (`dsm.c:279,282`), `TASK_STATUS`/`TASK_DONE` (`tasks.c:255-256`) |
| `MSG_POOL` (sin espera, ≥2 hilos) | worker | enviar mensajes; **nunca** esperar respuesta de red ni un lock de un handler POOL_WAIT | `FETCH`/`INVALIDATE` (`pager.c:319-320`), `JOIN` (`cluster.c:107`), `BARRIER_ARRIVE`/`LOCK_ACQUIRE`/`LOCK_RELEASE`/`ALLOC_REQUEST` (`dsm.c:278,280-283`), `STATS_REQUEST` (`network.c:197`), `SPAWN_TASK` (`tasks.c:254`) |
| `MSG_POOL_WAIT` (con espera, `worker_pool_size`) | worker | bloquearse esperando `FETCH_REPLY`/ACK | `PAGE_REQUEST`, `PAGE_RELEASE` (`directory.c:170-171`) |

**Por qué:** la respuesta a un handler POOL_WAIT la procesa el listener (INLINE) o el pool sin espera, **nunca otro POOL_WAIT**, así que agotar `worker_pool_size` no produce deadlock. El Prototipo 1 tenía un solo pool y hacía timeout con `worker_pool_size=1` (`AVANCE3.md:66`). **Si mueves un handler de clase, prueba con pool=1** (la prueba `test_stress` no está en el repo; usa `test_basic` con `worker_pool_size = 1` en `dsm.conf`).

Otros hechos de red:
- Framing: `msg_header_t` de 22 bytes (`internal.h:59-68`, con `_Static_assert`), big-endian en el cable (`network.c:62-69`); `payload_len` validado ≤ `max_payload` (`:118`, `:259`), que por defecto es `page_size + 4352` y es configurable (`config.c:52,76`). `read_exact` repite `recv` (`:43-52`).
- Una conexión TCP saliente perezosa por destino (`out_fd[dst]`, mutex por destino, `:35-36`), `connect()` no bloqueante con `connect_timeout_ms` (500 ms), `SO_SNDTIMEO = request_timeout_ms`, reconexión una vez (`:239-277`). Sockets aceptados con `SO_RCVTIMEO` de 3 s (`:141-142`).
- **TCP es FIFO por conexión**: `PAGE_DATA` llega antes que un `INVALIDATE`/`FETCH` posterior del mismo home.
- `net_send(dst == self)` despacha localmente: INLINE directo; POOL/POOL_WAIT se encolan (`:262`, `:89-93`).
- **Fallo rápido:** `net_send` a nodo caído devuelve `DSM_ERR_NODE_DOWN` sin conectar, salvo `JOIN`/`NODE_LIST`/`HEARTBEAT` (`:263-264`).
- RPC (`net_rpc_call`): `req_id` propio; `ALLOC_REPLY`/`STATS_REPLY` los empareja `rpc_on_reply` (`:157-199`).
- Campos sobrecargados: `page_id` = página o id de barrier/lock/tarea; `req_id` = RPC, generación de transacción, `n` en `BARRIER_ARRIVE` y tareas en cola/ejecución en el heartbeat; en el heartbeat `page_id` = carga CPU×100 (`cluster.c:138`, `:87`); `access` = READ/WRITE o estado de tarea.

### 4.6 Membresía (cluster.c)
- Config inmutable; estado mutable en atómicos: `alive[]`, `last_hb_ms[]`, carga reportada (`cluster.c:19-23`).
- `dsm_init` → `cluster_join()`: envía `JOIN` a nodos no vivos cada 200 ms hasta que todos estén vivos o venza `join_timeout_ms` (30 s) → `DSM_ERR_TIMEOUT`. `wait_all_nodes=0` solo hace un intento (`cluster.c:111-127`).
- Heartbeat UDP cada `heartbeat_interval_ms` a `puerto_del_nodo + heartbeat_port_offset` (1000); sin latido durante `heartbeat_timeout_ms` ⇒ nodo caído (`"nodo %u marcado CAIDO"` en verbose, `:72`). Un latido lo revive (`:158`).
- Home = `splitmix64(page ^ hash_seed) % max_nodes` (`:78`). **Decisión:** `max_nodes` = nodos de la corrida, porque un home inactivo rompería las asignaciones contiguas (`AVANCE3.md:95`).
- Coordinadores: alloc = `cluster_alloc_coord()` (hoy devuelve 0, `:83`); barrier = `splitmix64(id ^ seed) % max_nodes` (`:80`); lock = `splitmix64(id ^ seed ^ 0x10c4) % max_nodes` (`:81`). Si el coordinador está caído, esa operación falla/expira.

### 4.7 API y módulos de `dsm.c`
- `dsm_init` (`:266-290`): `config_load` → `C = config_get()` → `cluster_init` → `net_init` → `pager_init` → `dir_init` → `sem_init` de barrier/waiters → registros de barrier/lock/alloc → `cluster_register` → `stats_init` → `tasks_init` → `net_start` → `cluster_start_heartbeat` → `cluster_join()`.
- `dsm_shutdown` (`:292-309`): bloque verbose de contadores → `tasks_shutdown` → `pager_release_all` → `pager_shutdown` → `cluster_shutdown` → `net_shutdown` → `dir_shutdown`.
- **Alloc:** RPC a `cluster_alloc_coord()` (nodo 0), que lleva un cursor por slot (`:133`) y una tabla `{nodo, pid, orden, dirección, tamaño, nombre, viva}`; prefiere el slot del solicitante, luego otros vivos (`:138-141`); si no cabe, intenta un rango contiguo que cruza slots vecinos (`:143-154`). Nombre repetido ⇒ **mismo puntero** (`:170-173`). `dsm_free` marca la entrada pero **no reutiliza memoria** (`:190`, `:231`).
- **Barrier:** un solo `dsm_barrier` activo por nodo (mutex), semáforo; `n` viaja en `req_id`. `n == 0 || n > max_nodes` ⇒ `DSM_ERR_BAD_ARG` (`:42`); participantes caídos ⇒ `DSM_ERR_TIMEOUT` tras `barrier_timeout_ms` (`:47-49`). Locks usan el mismo timeout (`:108`), coordinador por hash y cola FIFO (`:75-96`).
- **Prefetch:** `dsm_prefetch_write/read(ptr,len)` piden permiso sin esperar un fallo, evitando los dos viajes lectura+escritura (`:249-258`).
- `stats.c` expone `dsm_print_stats`, `dsm_print_local_pages`, `dsm_print_global_pages`, `dsm_print_allocs`, `dsm_write_html` (`:70-176`); `main.c:10-32` es el nodo interactivo con comandos `stats|local|global|allocs|html|alloc|write|read|barrier|lock|unlock|quit`.

### 4.8 Extra 1: tareas (tasks.c)
`dsm_register_task(id, fn)` en todos los nodos **antes** de `dsm_init` (convención de los apps: `test_task.c:35-37`, `app_tasks.c:21-22`; el código no la enforcea); `dsm_spawn(id, args ≤ DSM_TASK_MAX_ARGS=256 B, nodo|DSM_ANY_NODE, flags, &t)` (`internal.h:223`, `tasks.c:175-177`); `dsm_wait(t,&res)` (`:206-231`, con `task_timeout_ms` o `barrier_timeout_ms` si es 0). Mensajes `SPAWN_TASK` (POOL: solo encola, `:70-84`) / `TASK_STATUS` (INLINE, `:157-163`) / `TASK_DONE` (INLINE, `:164-173`). Ejecutores propios (`task_pool_size`=4 por defecto, `config.c:32`), separados de los pools de red (`tasks.c:257-260`). Planificador: puntaje = tareas (PENDING/RUNNING)×1000 + CPU (limitada a 999); para remotos usa `max(reportado por latidos, enviado y pendiente)` (`:129-143`). `DSM_TASK_RETRY`: si cae el nodo que ejecuta, `dsm_wait` reenvía hasta `task_max_retries` (semántica al-menos-una-vez ⇒ tareas idempotentes); sin flag → `DSM_ERR_NODE_DOWN` (`:217-221`). Un `TASK_DONE` de un nodo que ya no es el destino se ignora (`:167`).

## 5. Configuración (`dsm.conf`)

Formato `clave = valor`, `#` comenta, tamaños con K/M/G (`config.c:11-14,38-41`). **Todos los nodos usan el mismo archivo.** El id propio viene de `DSM_NODE_ID` (si falta: `config: falta variable de entorno DSM_NODE_ID`, `config.c:83`).

Los **defaults están en `config.c:25-32,76`**, no en `dsm.conf`; el archivo versionado solo fija 8 claves (`page_size`, `mem_per_node`, `max_nodes`, `global_base_addr`, `worker_pool_size`, `request_timeout_ms`, `barrier_timeout_ms`, `hash_seed`) y 2 líneas `node` (`dsm.conf:3-13`).

`page_size`(4096) `mem_per_node`(16M) `max_nodes`(2; debe == nº de líneas `node=`, máx. 16) `global_base_addr`(`0x700000000000`) `worker_pool_size`(8) `nowait_pool_size`(4, mínimo 2) `request_timeout_ms`(5000) `request_retries`(2) `connect_timeout_ms`(500) `join_timeout_ms`(30000) `barrier_timeout_ms`(60000) `heartbeat_interval_ms`(1000) `heartbeat_timeout_ms`(5000) `heartbeat_port_offset`(1000) `min_hold_us`(200) `release_memory`(1) `wait_all_nodes`(1) `task_pool_size`(4) `task_timeout_ms`(0=barrier) `task_max_retries`(3) `hash_seed`(0, por `memset` en `config.c:24`) `max_payload`(por defecto `page_size+4352`) y `node = <id> <host|ip> <puerto>`.

Tamaños calculados a partir de las definiciones de `internal.h:59-108,121,132-135` y `pager.c:33` (x86-64): `msg_header_t` 22, `node_info_t` 112, `dsm_config_t` 1904, `page_local_t` 16, `dir_entry_t` 24, `alloc_entry_t` 64, `node_stats_t` 152. **Cuidado con `tools/print_sizes.c`**: imprime solo 6 líneas (incluye `sem_t`, no imprime `alloc_entry_t` ni `node_stats_t`) y su `page_local_t` privado (`:6`) es de 12 bytes, no el de 16 del pager. Los números de `AVANCE3.md:44-47` (104/1760/136) son anteriores a los campos de tareas y están desactualizados.

## 6. Invariantes: NO los rompas

1. El runtime solo toca páginas por `pager_sys_addr`. Nunca por la vista de aplicación.
2. Ningún handler `MSG_INLINE` envía por red ni espera **respuestas de red**; ningún handler `MSG_POOL` espera respuestas de red (los INLINE sí toman mutex breves: `network.c:156`, `directory.c:50`).
3. Solo `PAGE_REQUEST` y `PAGE_RELEASE` (POOL_WAIT) esperan `FETCH_REPLY`/ACK.
4. Nunca llamar a `net_*` con un spinlock del pager tomado. Orden de locks del pager: franja → fallos.
5. En `pager_on_page_data`: datos → `mprotect` → estado → `sem_post`. En `pager_on_fetch`: permiso primero, **luego** la copia (`pager.c:229-236`).
6. `PAGE_WRITEBACK` es INLINE (si fuera pool, un `FETCH` posterior podría leer la copia vieja).
7. `punch()` jamás sobre páginas cuyo contribuyente es este nodo (borraría la copia de referencia) y siempre **después** de copiar los datos cuando hay copia que hacer; en la rama de invalidación de una página en S no hay `memcpy` porque el dato ya está en la copia de referencia (`pager.c:205-209`, `:236-237`).
8. El handler de `SIGSEGV` solo usa funciones async-signal-safe (sin `malloc`, `printf`, mutex).
9. Cambios de firma/enum: primero en `internal.h`; mantener `_Static_assert(sizeof(msg_header_t) == 22)` (`internal.h:68`).
10. Toda **espera de red** tiene timeout; no introducir esperas infinitas de protocolo (los workers esperan en `pthread_cond_wait`/`sem_wait` hasta que haya trabajo o se apague el sistema: `network.c:104`, `pager.c:166`).
11. Los hilos de aplicación nunca deben retener un lock del runtime mientras tocan memoria DSM.

## 7. Guía de diagnóstico (síntoma → causa → qué hacer)

Primero: **reproducir** con `DSM_NODE_ID=i ./build/<bin> dsm.conf [args...]` (o `./scripts/run_local.sh`), abrir **todos** los logs (`build/n0.log`, `build/n1.log`, …) y repetir con `DSM_VERBOSE=1`. Para capturar evidencia reproducible de cada escenario, sigue el protocolo del **§12** y registra el resultado en el **Apéndice A**.

| Síntoma | Causas probables | Acciones |
|---|---|---|
| `[dsm] DSM_ERR_TIMEOUT: la pagina no llego (nodo caido?) \| pagina=N home=nodo M`, salida **70** | (a) el nodo **home M** o el owner/contribuyente ya salió o cayó (muy común sin barrier final); (b) transacción del directorio fallida (`FETCH ... fallo/timeout`, `ACKs ... incompletos` en verbose); (c) regresión de deadlock de pool; (d) nodo aún no arrancado | Buscar qué nodo terminó antes. Calcular `cluster_home_of(N)` y el contribuyente (`N*page_size/mem_per_node`). Verificar con `worker_pool_size = 1` en `dsm.conf`. Si salió antes: añadir barrier final |
| `dsm_init fallo` / join timeout | Nodos no arrancados; `max_nodes` ≠ líneas `node=`; puertos ocupados; falta `DSM_NODE_ID` | Leer el mensaje de `config:` / `net_start bind/listen` / `heartbeat bind`. `pkill -f build/`; cambiar puertos en `dsm.conf` |
| `heartbeat bind` o `net_start bind/listen` falla | Puerto TCP `base+i` o UDP `base+1000+i` ocupado | Cambiar puertos en todas las líneas `node=` |
| `mmap ... MAP_FIXED_NOREPLACE` falla | Dirección base ocupada o kernel < 4.17 | Cambiar `global_base_addr` en todos los nodos |
| Barrier/lock devuelve `DSM_ERR_TIMEOUT` (tras `barrier_timeout_ms`, 60 s por defecto) | Participante caído; `n` distinto; coordinador caído | Elegir un id de barrier cuyo coordinador sea un sobreviviente (`while (cluster_barrier_coord(id)==victim) id++`) |
| Valores incorrectos (`FALLO ...: got=X want=Y`) | Orden en `on_page_data`/`on_fetch`, write-back no INLINE, `punch` sobre contribuyente, respuesta tardía sin generación, página tocada por la vista de aplicación | Revisar §6. Reproducir con 2 nodos y `test_basic`; luego bajar `worker_pool_size` a 1 |
| Cuelgue (sin mensajes) | Handler cambiado de clase; mutex tomado esperando red; spinlock con `net_*` dentro | Revisar §4.5/§6. Bajar `request_timeout_ms` para convertir el cuelgue en timeout visible |
| `SIGSEGV` real (core/"Segmentation fault") | Acceso fuera del espacio DSM (bug de la app); desbordar `max_payload`; punteros locales | El handler restaura `SIG_DFL` para direcciones ajenas: es un bug normal, no del DSM |
| Home que sale/cae (documentado en `AVANCE3.md:107`, app `test_leave` no presente) | Su directorio se pierde por diseño; sobrevivientes que cierran antes de leer | La prueba documentada usa páginas con otro home y barrier final. No "arreglar" el diseño |
| Detección de caída lenta (documentado en `AVANCE3.md:22,71`, app `test_hb` no presente) | Latidos UDP bloqueados; `heartbeat_timeout_ms` grande | La detección tarda ≈ `heartbeat_timeout_ms` |
| Reparto de tareas desigual (`test_task.c:62`) | En 1 núcleo los remotos confirman tarde; el planificador carga al nodo que termina antes | La prueba exige solo ≥1 tarea por nodo; revisar `pick_node` (`tasks.c:129-146`) y heartbeats si falla de verdad |
| `Permission denied` al ejecutar | Partición sin permiso de ejecución | Copiar el proyecto a otra ruta (`README.md:44`) |
| `config: parametro desconocido 'x'` | Línea no soportada (se ignora, solo avisa, `config.c:60`) | Revisar ortografía o añadir en `config.c` e `internal.h` |
| `PAGE_DATA invalido` / `DSM_ERR_PROTO` en verbose | `payload_len ≠ page_size` o tipo inválido; `page_size` distinto entre nodos | Verificar que todos usan el mismo `dsm.conf` |
| Compilación falla | Faltan `apps/*.c` del `Makefile:9`, `-lm`, o `.RECIPEPREFIX` (make < 3.82) | Ver §1 y `Makefile` |

### Cómo aislar
1. 2 nodos, `test_basic` (`./scripts/run_local.sh`): confirma MSI + barrier.
2. `test_basic` con `worker_pool_size = 1` en `dsm.conf`: confirma ausencia de deadlock por pool.
3. `page_size = 8192` en `dsm.conf` (todos los nodos): sensibilidad a tamaño de página.
4. `make FALLBACK=1`: valida la detección lectura/escritura por estado (en x86-64).
5. Repetir 15–30 veces con puertos distintos para cazar intermitentes.
6. `nodo interactivo` (2 terminales): `DSM_NODE_ID=0|1 ./build/dsm_node dsm.conf` con `stats`, `local`, `global`, `allocs`, `html`.

## 8. Historial de bugs encontrados (para no reintroducirlos)

Fuente real: `AVANCE3.md:62-74` (NO existe `CORRECCIONES.md`, aunque `AVANCE3.md:10,56` y `README.md:29,138` lo citan).

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
| Spinlock global del pager | 64 spinlocks por franja + uno de fallos; orden franja → fallos |
| Posible livelock por false sharing | Retención mínima `min_hold_us` |
| Errores de pruebas, no del DSM (`AVANCE3.md:114-118`) | Guion con `$N_` inexistente; `test_leave` con páginas homed en el nodo que sale y sin barrier final; CLI sin barriers coordinados |

## 9. Limitaciones conocidas y decisiones tomadas (no son bugs)

- Extra 2 **no implementado**: si cae un nodo con páginas en M, ese dato se pierde; si cae un home, se pierde su directorio; un nodo que sale con `dsm_shutdown` devuelve sus páginas pero no su directorio (`AVANCE3.md:98,107`).
- `max_nodes` = nodos de la corrida; `mem_per_node` uniforme; coordinador de alloc fijo en el nodo 0 (`AVANCE3.md:95-99`).
- `dsm_free` no reutiliza memoria. `PAGE_DATA` siempre lleva la página completa (`page_size`, 4096 por defecto; no hay mejora S→M sin datos) (`AVANCE3.md:101,110`).
- Cola explícita por página en el directorio: no hay; se usa mutex por franja (`AVANCE3.md:100`).
- Tareas: argumentos ≤256 B, resultado `int64`, sin cancelación; reintento al-menos-una-vez; cuidado con `task_pool_size` y tareas anidadas.
- Barrier: un `dsm_barrier` activo por nodo. Lock: un `GRANT` tardío tras timeout puede asignarse a otro solicitante del mismo id (`AVANCE3.md:109`).
- **NO VERIFICADO:** compilación y ejecución en este entorno; multicore (posible livelock/contención de spinlock; mitigación `min_hold_us`); computadoras físicas distintas y nombres de host; rendimiento (los tiempos en 1 núcleo ≈0,4 s dominados por el arranque no son concluyentes, `AVANCE3.md:106`); `shm_open` como respaldo de `memfd_create`; arquitecturas no x86-64 (el respaldo por estado solo se probó compilando con `FALLBACK=1` en x86-64); 16 nodos; la batería de 11/14 pruebas documentada.

## 10. Contexto del curso (para priorizar)

- Curso IC-6600 Principios de Sistemas Operativos, TEC, II-2026. Proyecto en parejas: Ximena Molina y Susana Feng (`README.md:3-4`, `AVANCE3.md:3-5`).
- Entregas: Entregable 1 (6 nov, diseño, `AVANCE1.md:5`), Avance 2/Prototipo 1 (13 nov, según commits del 13 nov), Avance 3/Prototipo 2 (20 nov, `AVANCE3.md:5`), **Demo 27 nov 2026** en el laboratorio del TEC con computadoras físicas (`EXTRA1.md:1`, `AVANCE1.md:450`, `AVANCE2.md:68`).
- Hay commits de ambas integrantes en el historial (`e9841c5` de Susana; `88df7a1`, `e1d8e80`, etc. de Ximena). **NO VERIFICADO** que la nota sea individual, que dependa de la defensa de todo el sistema o que Git se valide para la nota: eso no aparece en los documentos del repo.
- Requisitos conocidos (reconstruidos de los AVANCE, no de un enunciado presente en el repo): MSI con páginas I/S/M, semáforos/mutex para tablas, archivo de configuración, heartbeat, estadísticas y consultas (texto o HTML por nodo), dos aplicaciones reales usando solo el API (no `malloc`), informe con rendimiento (pool 1/4/8/16, tamaños de página, nº de nodos) y manual de usuario (`AVANCE1.md:356,383-449`, `AVANCE3.md:52-54`, `README.md:15-27`). `EXTRA1.md:28` / `TODO.md:28` dejan pendiente **verificar los requisitos contra el enunciado**: no hay enunciado versionado.
- **Si arreglas algo:** mantén compatibilidad con lo que sí se puede ejecutar (`scripts/run_local.sh` y las 3 apps), documenta el cambio junto a los otros en `AVANCE3.md` §6 (no hay `CORRECCIONES.md`) y no cambies firmas sin tocar primero `internal.h`.

## 11. Comandos útiles

```sh
make build/dsm_node build/print_sizes build/test_basic build/test_task build/app_tasks   # en Linux
make FALLBACK=1 build/test_basic                # respaldo por estado
./scripts/run_local.sh dsm.conf                 # 2 nodos de test_basic, logs build/n0.log y build/n1.log
DSM_NODE_ID=0 ./build/test_basic dsm.conf       # nodo manual; usar ids 0..max_nodes-1
DSM_NODE_ID=1 ./build/test_basic dsm.conf
DSM_VERBOSE=1 DSM_NODE_ID=0 ./build/test_task dsm.conf   # logs internos (test_task.c; requiere >=3 nodos y dsm.conf con 3)
./build/print_sizes                             # 6 líneas; ojo: su page_local_t no es la del pager
DSM_NODE_ID=0 ./build/dsm_node dsm.conf         # nodo interactivo (2 terminales, ids 0 y 1)
pkill -f build/                                 # matar procesos huérfanos
```

## 12. Protocolo de ejecución en laboratorio (preparado; pendiente de correr)

Esta sección existe para que la ejecución produzca **evidencia reutilizable**, no solo un "PASS". Las causas del §7 y del §13 son inferidas del código hasta que el **Apéndice A** las confirme con logs reales.

**Reglas para la IA que acompañe al ejecutor:**
1. No inventes salidas ni escribas "PASS"/"confirmada" sin un log pegado por la persona que corrió el caso. Si falta información (máquina, permisos, puertos, versión del kernel), pregúntale.
2. Guía a la persona en el orden E01→E12: E01/E02 validan que el entorno y los tamaños son correctos antes de interpretar fallos. Tú puedes leer el código y redactar los comandos; quien teclea y observa la máquina es la persona.
3. Pide los logs completos (sin filtrar); para diagnosticar, contrasta el síntoma literal con el `archivo:línea` del §13 y solo entonces escribe la causa en el Apéndice A.
4. Si aparece un mensaje que no está en el §13, localízalo en el código (`grep` en `src/` y `apps/`), añádelo al catálogo con su origen y su estado, y registra el caso.
5. Si la evidencia contradice este documento, corrígelo tú mismo (§ correspondiente, README si aplica) y deja constancia en la fila del Apéndice A. No esperes a los autores.
6. Si un caso no se pudo ejecutar, déjalo como `no ejecutado` con el motivo (sin permisos, sin máquina, sin puerto, etc.). Nunca lo marques como fallo del DSM sin evidencia.

**Antes de empezar, en cada máquina:**

```sh
uname -m && uname -r && ldd --version | head -1 && nproc
make build/dsm_node build/print_sizes build/test_basic build/test_task build/app_tasks
```

Guarda esa salida: es la prueba del entorno (x86-64, kernel ≥ 4.17, glibc, núcleos). Usa siempre `DSM_VERBOSE=1` y conserva los logs **completos**; no filtres por PASS/FALLO. Entre casos: `pkill -f build/` y cambia puertos o usa copias de `dsm.conf` en `build/` para evitar colisiones (TCP `base+i`, UDP `base+1000+i`).

**Plantilla de captura por caso** (copiarla en el Apéndice A):

```
ID: E## | fecha: | ejecutor: | máquina/kernel/núcleos:
dsm.conf usado (diff contra el original):
env: DSM_NODE_ID=... DSM_VERBOSE=1 ... | comando exacto:
duración: | códigos de salida:
síntoma literal (primeras líneas de cada log):
causa raíz (contrastada con archivo:línea):
evidencia (ruta del log y líneas clave):
acción correctiva / cambio aplicado:
```

**Casos clave**

| ID | Cómo ejecutarlo | Qué debe observarse |
|---|---|---|
| E01 | `./scripts/run_local.sh dsm.conf` | `Test A/D/B: OK` y `RESULTADO: PASS` en ambos nodos; `n0=0 n1=0`; contadores al cerrar |
| E02 | `./build/print_sizes` | 6 líneas. Comparar con §5: si difieren, **corregir el documento** con los valores medidos |
| E03 | 2 terminales: `DSM_NODE_ID=0\|1 ./build/dsm_node dsm.conf`; comandos `stats`, `local`, `global`, `allocs`, `html` | Genera `dsm_nodo0.html` / `dsm_nodo1.html`; estados `vivo`/`caido` coherentes |
| E04 | `./build/test_basic dsm.conf` sin `DSM_NODE_ID` | `config: falta variable de entorno DSM_NODE_ID`, `dsm_init fallo`, exit 1 |
| E05 | Copias de `dsm.conf` con: (a) `max_nodes=3` y 2 líneas `node`; (b) `page_size=1000`; (c) host inexistente | `config: max_nodes ...`, `config: page_size ...`, `cluster: no se pudo resolver ...` |
| E06 | Ocupar 9101 (TCP) y 10101 (UDP) con otro proceso y arrancar un nodo | `net_start bind/listen: ...` y/o `heartbeat bind: ...` |
| E07 | Copia de `dsm.conf` con `join_timeout_ms=3000`; lanzar solo `DSM_NODE_ID=0` | Tras ~3 s, `dsm_init fallo` (join expira; buscar causa en líneas anteriores) |
| E08 | `dsm.conf` con 3 nodos (`node = 2 127.0.0.1 9103`) y 3 procesos `./build/test_task dsm.conf`; repetir con 4 nodos (añadir `node = 3 ... 9104`) | Reparto `balanceo de 30 tareas`, `tarea con DSM_TASK_RETRY completada ...`, `test_tasks (N nodos): PASS`; con 4 nodos también `tarea sin reintento: ... => DSM_ERR_NODE_DOWN` |
| E09 | `DSM_NODE_ID=0/1 ./build/app_tasks dsm.conf 96`; luego con `1200` | `app_tasks N=96 ... PASS`; con 1200 (3×11,5 MB > 32 MB totales) `alloc fallo`, exit 1 |
| E10 | Arrancar 2 nodos de `test_basic` a mano y `kill -9` al PID de `DSM_NODE_ID=1` durante Test A/D/B | `nodo 1 marcado CAIDO`, `PAGE_REQUEST pagina ... fallo (err 3)`, y `barrier fallo` (exit 2) o `fatal_page` (exit 70). Capturar ambos logs |
| E11 | Con 2 nodos, matar al nodo home/owner de la página que el otro está tocando (identificar con `local`/`global` y `cluster_home_of`) | `fatal_page`: `DSM_ERR_TIMEOUT: la pagina no llego ... pagina=P home=nodo H`, exit 70 tras ≈15 s |
| E12 | En la máquina de laboratorio (varios núcleos), repetir E01/E09 con `worker_pool_size` 1, 4 y 8 (5 veces cada uno) | Tiempos, `reintentos>0` en contadores y ausencia de cuelgues: permite retirar el **NO VERIFICADO: multicore** del §9 si sale bien |

Registro: los casos E04–E07 son fallos de configuración/entorno (rápidos); E08–E11 implican caídas y timeouts de hasta 60 s. Anota siempre el comando exacto y la configuración usada: sin eso un log no sirve para confirmar causa.

## 13. Catálogo de mensajes del runtime → causa → acción

**Estado:** todas las causas de esta tabla están **inferidas del código** (`archivo:línea`) hasta que se completen los casos del Apéndice A. Las filas marcadas *no reproducible* no pueden dispararse con los 3 apps y el script actuales; no cambies esa etiqueta sin nuevo código o inyección externa.

| Síntoma literal | Origen | Causa inferida / acción |
|---|---|---|
| `config: falta variable de entorno DSM_NODE_ID` | `config.c:83` | Invocación sin env → exportar `DSM_NODE_ID` |
| `config: max_nodes (...) debe ser 1..16 y coincidir con las lineas node= (...)` | `config.c:69` | `max_nodes` ≠ líneas `node=` → corregir `dsm.conf` (E05a) |
| `config: page_size debe ser potencia de 2 y multiplo de ...` | `config.c:66` | `page_size` inválido → usar 4096/8192/… (E05b) |
| `config: parametro desconocido 'x' (ignorado)` | `config.c:60` | Typo; se ignora. Corregir ortografía |
| `cluster: no se pudo resolver '...' (...)` | `cluster.c:42` | `getaddrinfo` falla → IP/nombre incorrecto (E05c) |
| `net_start bind/listen: ...` | `network.c:210` | Puerto TCP ocupado → cambiar `node =` (E06) |
| `heartbeat bind: ...` | `cluster.c:172` | UDP `puerto+1000` ocupado → cambiar puertos (E06) |
| `mmap (vista del sistema / aplicacion; kernel >= 4.17 ...): ...` | `pager.c:313` | Base ocupada o kernel < 4.17 → cambiar `global_base_addr` |
| `dsm_init fallo` | `main.c:9`, apps | Causa agregada; leer líneas anteriores (config/bind/mmap/join) |
| `[dsm nX] nodo N ahora VIVO` / `nodo N marcado CAIDO` | `cluster.c:68,72` | Transición de membresía; `CAIDO` explica en cascada `NODE_DOWN`, `FETCH` fallido y timeouts (E10) |
| `[dsm] DSM_ERR_TIMEOUT: la pagina no llego (nodo caido?) \| pagina=P home=nodo H`, exit 70 | `pager.c:67-75,109` | Se agotaron `request_retries` esperando `PAGE_DATA`: home/owner caído o transacción fallida; ver `FETCH`/`ACKs` y contadores (E11) |
| `[dsm] demasiados fallos simultaneos \| pagina=P ...`, exit 70 | `pager.c:126` | >256 fallos concurrentes → **no reproducible** con los apps actuales |
| `[dsm nX] PAGE_REQUEST pagina P fallo (err 3); el pager reintentara` | `pager.c:173` | `net_send` devolvió `NODE_DOWN`; precursor de `fatal_page` (E10) |
| `[dsm nX] sin espacio para transacciones` | `directory.c:85` | Agotadas las 24 transacciones del home → precursor de timeout → **no reproducible** con los apps actuales |
| `[dsm nX] FETCH pag P fallo/timeout (nodo N)` | `directory.c:89,101` | `FETCH` al owner falló o venció en 5000 ms → revisar owner y red (E10) |
| `[dsm nX] ACKs de pag P incompletos` | `directory.c:107` | Faltan ACKs de invalidación → **no reproducible** de forma fiable sin matar un nodo justo entre `INVALIDATE` y ACK |
| `[dsm nX] mensaje tipo T sin handler` / `DSM_ERR_PROTO: tipo=T payload_len=L (conexion cerrada)` | `network.c:88,119` | Tráfico malformado o nodos con versiones/page_size distintos → **no reproducible** sin inyección |
| `barrier fallo` / `barrier -> 2` / `return 2` | `test_basic.c:16`, `main.c:27` | `DSM_ERR_TIMEOUT`: participante/coordinador caído o `n` distinto (E10) |
| `FALLO <check>: got=X want=Y` / `FALLO C[i][j]` | `test_basic.c:19`, `app_tasks.c:42` | Coherencia/cómputo incorrecto: orden `on_page_data`/`on_fetch`, write-back, punch o generación |
| `FALLO 3 nodo N no recibio ninguna tarea` / `FALLO 7 tarea enviada al nodo caido` | `test_task.c:62,85` | Planificador/heartbeat desactualizado → revisar `pick_node` y carga reportada (E08) |
| `tarea con DSM_TASK_RETRY completada tras la caida del nodo N ...` | `test_task.c:78` | Comportamiento esperado tras caída: confirma que el reintento funcionó (E08) |
| `tarea sin reintento: nodo N cayo => DSM_ERR_NODE_DOWN` | `test_task.c:82` | Esperado sin `DSM_TASK_RETRY`; solo aparece con ≥4 nodos (E08) |
| `alloc fallo` / `alloc inesperado` / `spawn fallo` / `wait fallo` | `app_tasks.c:28,35,37`, `test_basic.c:35` | Sin memoria global, coordinador caído o tarea/node DOWN (E09) |
| `reintentos>0` en contadores de cierre | `dsm.c:297` | Hubo timeouts de página: buscar `PAGE_REQUEST ... fallo` y `FETCH ... timeout` |
| `(nodo caido)` / `(sin respuesta)` / `(no se pudo consultar al coordinador: err N)` | `stats.c:77,104,108,129` | Nodo caído, RPC expirado o coordinador de alloc (nodo 0) caído (E03) |

## Apéndice A. Casos ejecutados (lo llena quien ejecuta en el laboratorio, con su IA)

Este apéndice **no lo completan los autores**: es el registro de la persona que corre las pruebas en el laboratorio, asistida por una IA. Cada fila se rellena con el texto literal del log y se marca `confirmada` solo cuando código y evidencia coinciden.

| ID | Estado | Fecha / entorno | Síntoma observado | Causa raíz confirmada | Evidencia (log:líneas) | Acción |
|---|---|---|---|---|---|---|
| E01 | pendiente | | | | | |
| E02 | pendiente | | | | | |
| E03 | pendiente | | | | | |
| E04 | pendiente | | | | | |
| E05 | pendiente | | | | | |
| E06 | pendiente | | | | | |
| E07 | pendiente | | | | | |
| E08 | pendiente | | | | | |
| E09 | pendiente | | | | | |
| E10 | pendiente | | | | | |
| E11 | pendiente | | | | | |
| E12 | pendiente | | | | | |

Al llenarlo: (1) copiar el texto literal del síntoma, no una paráfrasis; (2) citar `archivo:línea` del código que lo emite; (3) marcar `confirmada` solo si el log y el código coinciden; (4) si un caso contradice este documento, corregirlo (y `README.md` si aplica); (5) si no se pudo ejecutar, escribir `no ejecutado` y el motivo en la columna Acción; (6) cuando E02 y E12 estén resueltos, actualizar §5 y §9 para que dejen de decir NO VERIFICADO. No hace falta enviar los logs a los autores: la evidencia queda aquí para la siguiente persona/IA que entre al proyecto.
