# Prototipo 1 del DSM: consideraciones de integración y opciones de mitigación

Análisis de `cluster.c`, `config.c`, `directory.c`, `dsm.c`, `network.c`, `pager.c` e `internal.h`, frente a `AVANCE2.md`, `TODO.md` y la especificación del proyecto (IC-6600, II-2026).

**Veredicto:** la base es buena y no hay que reescribirla. La separación en capas, el doble mapeo, la clasificación INLINE/POOL y el razonamiento sobre el orden FIFO de TCP son correctos. Los problemas de abajo aparecen al integrar lo que falta (JOIN, heartbeat, tolerancia a fallos, alloc global, estadísticas) o al probar con más de un núcleo, más de dos nodos o un pool pequeño.

## Resumen y prioridades

| # | Tema | Gravedad | Cuándo atacarlo |
|---|---|---|---|
| 1 | Agotamiento del pool (deadlock entre nodos) | Alta | Antes de las pruebas con 3-4 nodos |
| 2 | Membresía estática en varios módulos | Media-alta | Antes de implementar JOIN/heartbeat |
| 3 | `PUNCH_HOLE` destruye la copia de referencia | Media-alta | Antes de implementar `fallocate` |
| 4 | Manejo de fallos sin salida limpia | Media-alta | Antes del heartbeat |
| 5 | Alloc global y tabla de páginas por proceso | Media | Antes de las aplicaciones de prueba |
| 6 | Riesgos de multicore y de entorno | Media | Pruebas tempranas en el laboratorio |
| 7 | Estadísticas y consultas | Baja-media | Prototipo 2 |

Orden sugerido de trabajo: 1 → 2 → 5 → 4 → 3 → 7, con las pruebas del punto 6 corriendo en paralelo desde ya.

---

## 1. Agotamiento del pool: deadlock entre nodos

### El problema

`dir_on_page_request` es `MSG_POOL` y **se bloquea** esperando el `FETCH_REPLY` y los ACK de invalidación. Pero el `MSG_FETCH` y el `MSG_INVALIDATE` que esa espera necesita también son `MSG_POOL` (`h_fetch` y `h_invalidate` en `pager.c`), así que compiten por los mismos workers.

- Si los 8 workers del nodo A están dentro de `dir_on_page_request` esperando a B, y los de B esperan a A, nadie atiende los `FETCH`. Resultado: timeout y luego `fatal()`.
- Con `worker_pool_size=1` falla de inmediato. Si el home es también la fuente de datos, el `FETCH` se despacha localmente a la cola del pool, que ya está ocupada por el handler que lo espera.
- `BARRIER_ARRIVE` también está en el pool y sufre lo mismo.
- El contrato 3 de `internal.h` solo es cierto si el request que genera esa respuesta tampoco depende del pool.

### Opciones

**Opción A (recomendada para el plazo): pools separados.**
Dos colas con sus propios workers:

- `MSG_POOL_WAIT`: handlers que pueden bloquearse esperando respuestas (`PAGE_REQUEST`).
- `MSG_POOL`: handlers que nunca esperan respuestas de red (`FETCH`, `INVALIDATE`, `BARRIER_ARRIVE`, futuros `STATS_REQUEST`).

Esbozo en `network.c`:

```c
typedef enum { MSG_INLINE, MSG_POOL, MSG_POOL_WAIT } msg_class_t;

typedef struct {
    job_t *head, *tail;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t *threads;
    unsigned n;
} pool_t;

static pool_t pool_nowait, pool_wait;   /* dos colas, dos grupos de hilos */

/* en dispatch(): */
else enqueue(reg[h->type].cls == MSG_POOL_WAIT ? &pool_wait : &pool_nowait, h, payload, fn);
```

Por qué funciona: los handlers del pool sin espera solo copian, hacen `mprotect` y envían, así que siempre terminan, y por tanto las esperas del pool con espera siempre se resuelven. Usa al menos 2 hilos en el pool sin espera. Dimensiona `ntxn` en `directory.c` con el tamaño del pool con espera.

**Opción B: directorio como máquina de estados sin bloqueo.**
`dir_on_page_request` envía el `FETCH`, guarda la continuación en una estructura por transacción (fase: `ESPERANDO_FETCH`, `ESPERANDO_ACKS`, ...) y retorna. Los handlers de `FETCH_REPLY` e `INVALIDATE_ACK` hacen avanzar la fase. Los timeouts requieren un hilo temporizador.
- Ventaja: ningún handler se bloquea, el pool puede ser de cualquier tamaño y la cola por página (prioridad 1 del TODO) sale naturalmente.
- Costo: es el cambio más grande; reescribe la lógica de `directory.c`.

**Opción C (parche rápido, no para entrega final): un hilo desechable por `PAGE_REQUEST`.**
Sencillo, pero sin límite de hilos bajo carga.

### Cómo verificarlo

- Correr con `worker_pool_size=1` y `2` y 3-4 nodos con escrituras cruzadas sobre las mismas páginas.
- Agregar una prueba donde todos los nodos pidan páginas cuyo home es el nodo vecino.
- Medir con el pool en 1, 4, 8 y 16 como ya está en el TODO.

---

## 2. Membresía estática repartida en varios módulos

### El problema

JOIN dinámico, heartbeat y memoria por nodo van a tocar:

- **Dos copias de la configuración.** `cluster.c` hace `static dsm_config_t C = *cfg`, mientras los demás módulos apuntan a la de `config.c`. Si JOIN o heartbeat cambian `alive` o los slots, las copias divergen.
- **Sin sincronización.** `cluster_get_node` copia la estructura sin lock; con un hilo de heartbeat escribiendo hay carrera.
- **Memoria uniforme.** `config.c` exige `num_nodes == max_nodes` y `cluster_initial_owner` asume `mem_per_node` igual para todos, aunque la especificación habla de "la cantidad de memoria que aporta" cada nodo.
- **Solo IPv4 numérico.** `connect_peer` usa `inet_pton`; el PDF pide "direcciones IP **o nombres**".
- **Puertos sin usar.** `port_control` y `port_heartbeat` existen en la estructura pero no se leen.
- **Home inactivo.** El hash usa `max_nodes`; un home sin nodo activo no puede atender.

### Opciones

**2.1 Una sola fuente de verdad.**
- Opción A: `cluster.c` guarda `const dsm_config_t *C = config_get()` en vez de una copia, y la tabla de nodos mutable vive en `cluster.c` con un `pthread_rwlock_t`.
- Opción B: mover la tabla mutable (`alive`, `last_heartbeat`) a un arreglo aparte en `cluster.c` y dejar la config inmutable. Es la más limpia: la config nunca cambia en ejecución.
- En ambos casos, `alive` como `atomic_uchar` y `cluster_get_node` bajo `rdlock`.

**2.2 Memoria por nodo.**
- Opción A (recomendada): mantener `mem_per_node` uniforme y documentarlo. El archivo de configuración es compartido por todos los nodos, así que es coherente y mantiene simples `initial_owner` y las direcciones base.
- Opción B: memoria variable por nodo con sumas prefijas de `mem_bytes` y búsqueda binaria en `cluster_initial_owner`. Es más fiel a la especificación, pero afecta `config.c`, el mapeo y el hash.

**2.3 Nombres de host.** Reemplazar `inet_pton` por `getaddrinfo(ni.ip, port, &hints, &res)` con `AF_INET` y `SOCK_STREAM`. Convenir en el campo `ip` que acepte nombre o IP.

**2.4 Puertos.** Opciones:
- Un solo puerto, con `HEARTBEAT` como un tipo de mensaje más. Es simple, pero un heartbeat puede quedar detrás de un `PAGE_DATA` y generar falsos positivos de "nodo caído".
- Puerto de heartbeat separado con su propio hilo y sockets (recomendado: el heartbeat es la señal de salud y no debe competir con el tráfico de páginas).

**2.4 bis. Home inactivo (decisión pendiente del TODO).**

| Opción | Ventaja | Problema |
|---|---|---|
| (a) Arrancar todos los nodos y fijar `max_nodes` = nodos de la corrida | Simple, sin migración | JOIN dinámico limitado a "todos presentes al inicio" |
| (b) Delegar homes inactivos a un slot activo | Permite arranque parcial | Al unirse el nodo real, cambian homes de páginas con entrada de directorio: hay que migrarlas |
| (c) Consistent hashing / rendezvous hashing | Pocas páginas cambian de home al unirse un nodo | Más código; aún hay que migrar entradas |
| (d) `home_of = initial_owner` | Elimina el problema; el home siempre está donde está el contribuyente | Se pierde el reparto de carga del hash |

Recomendación: **(a) para el demo**, usando una barrera de inicio que espere a todos los nodos. Si se quiere JOIN real sin migración, (d) es la alternativa más barata.

---

## 3. `PUNCH_HOLE` puede destruir la copia de referencia

### El problema

La copia de referencia de cada página vive en el mismo `memfd` del contribuyente, y `FETCH` la sirve cuando `owner_dirty == 0`. Si al invalidar se hace `fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE)` en el contribuyente, esos datos se pierden y se sirven ceros.

### Opciones

**Opción A (recomendada): liberar solo en nodos que no son el contribuyente.**

```c
if (cluster_initial_owner(p) != cluster_self_id())
    fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
              (off_t)(p * psz), (off_t)psz);
```

Hacerlo **después** de copiar los datos con `memcpy` en `pager_on_fetch` (cuando la página pasa a I) y después de aplicar la invalidación. Como `memfd` es compartido entre las dos vistas, el hueco aplica a ambas.

**Opción B: separar la copia de referencia** en otro `memfd` y así poder liberar la vista de trabajo siempre. Duplica la memoria potencial en el contribuyente.

**Opción C: no liberar RAM** y documentarlo como limitación. Es aceptable si el demo no excede la RAM del laboratorio.

Nota: `madvise(MADV_DONTNEED)` no sirve aquí, porque el mapeo es `MAP_SHARED` sobre un `memfd` y no libera las páginas de la caché.

---

## 4. Manejo de fallos sin salida limpia

### 4.1 `fatal()` en timeouts del pager

El timeout en `wait_fault` llama a `_exit(70)`. Desde un handler de señal no se puede "devolver un error" a la aplicación.

Opciones:
- **Reintentar antes de morir** (recomendado): ante el primer timeout, reenviar `PAGE_REQUEST` (el hilo emisor puede reinsertar la solicitud en el anillo) hasta N veces con espera creciente; solo entonces `fatal()` con un mensaje claro que nombre el nodo y la página. Requiere que el directorio sea idempotente ante peticiones repetidas (con el `req_id`).
- **Callback de error** (`dsm_set_error_handler`) que la aplicación pueda registrar, llamado desde el handler solo si es async-signal-safe, o bien desde el hilo emisor.
- **Con tolerancia a fallos (Extra 2):** marcar el nodo como caído vía heartbeat y que el reintento se dirija a una réplica.

### 4.2 Un nodo caído congela a los que le envían

`connect_peer` puede bloquear hasta `request_timeout_ms` con `out_mu[dst]` tomado, y `send_all` no tiene timeout.

Mitigaciones:
- `connect()` no bloqueante con `poll()` y un timeout corto (por ejemplo 500 ms) en vez de 5 s.
- `SO_SNDTIMEO` en los sockets de salida para que `send` no bloquee indefinidamente.
- Fallo rápido: si `cluster_is_alive(dst)` es falso, `net_send` devuelve `DSM_ERR_NODE_DOWN` sin intentar conectar. Cuando el heartbeat lo marque vivo otra vez, se reintenta.
- Reintentos de conexión con espera en un hilo de reconexión, no con el mutex tomado.

### 4.3 `FETCH_REPLY` tardío en un `txn_t` reutilizado

`txn_find` busca solo por página. Tras un timeout, la respuesta tardía puede caer en una transacción nueva de la misma página (o dejar un `sem_post` colgando).

Solución con un contador de generación:

```c
static atomic_uint txn_gen = 1;

/* en txn_alloc */
t->gen = atomic_fetch_add(&txn_gen, 1);
while (sem_trywait(&t->sem) == 0) {}          /* drenar posts atrasados */

/* en do_fetch e INVALIDATE: usar el gen como req_id (net_send respeta req_id != 0) */
msg_header_t h = { .type = MSG_FETCH, .page_id = p, .access = (uint8_t)acc, .req_id = t->gen };

/* en los handlers de respuesta */
if (!t || h->req_id != t->gen) return;        /* respuesta de una transacción vieja: descartar */
```

`h_fetch` y `h_invalidate` ya copian `req_id` en la respuesta, así que no requiere cambios en `pager.c`.

### 4.4 Tolerancia a fallos (Extra 2)

El diseño actual tiene la copia modificada en un solo nodo (write-back perezoso) y el directorio solo en el home. Si cae cualquiera de los dos, hay pérdida de datos.

Decidir pronto entre:
- **No hacerlo**: documentarlo como limitación (el Extra vale 10% y requiere documentar nivel de tolerancia y overhead).
- **Nivel mínimo (tolera 1 nodo)**: replicar cada página modificada en un nodo secundario (por ejemplo `(owner+1) % n`) al pasar de M a S/I o en cada barrier, y replicar las entradas del directorio en un home de respaldo. Medir el overhead (mensajes y bytes extra por escritura) para el informe.
- **Write-through a la copia de referencia** al concederse un `PAGE_DATA` en W: más costoso, pero simple de razonar.

---

## 5. Alloc global y tabla de páginas por proceso

### El problema

`dsm_alloc` es un bump pointer local sin registro. `pid_owner` y `seq_order` en `page_local_t` no se usan. La especificación pide:
- atender solicitudes mayores que la memoria local,
- saber qué páginas son de qué proceso y en qué orden,
- que varios nodos compartan arreglos de forma transparente.

Hoy dos nodos solo comparten un puntero si lo coordinan a mano con `dsm_slot_base`.

### Opciones

**Opción A (recomendada): coordinador de alloc con `ALLOC_REQUEST/REPLY`.**
- Un nodo coordinador (el 0, o `cluster_barrier_coord`-style por hash) mantiene un bump pointer global sobre el espacio completo y una tabla de asignaciones: `(pid, nodo, seq, página_inicio, n_páginas, nombre)`.
- `dsm_alloc` local intenta primero el slot propio (sin red) y registra la asignación en el coordinador de forma asíncrona; si no cabe, pide al coordinador un rango en otros slots.
- Como los slots son contiguos en el espacio virtual, un rango que cruza nodos sigue siendo un solo puntero contiguo. Esto no choca con el diseño actual.
- `pid_owner` y `seq_order` es mejor llevarlos en esta tabla del coordinador que en `page_local_t` (más fácil de consultar para las estadísticas).

**Opción B: asignaciones con nombre.**
`dsm_alloc_named("matA", size)` devuelve el mismo puntero en todos los nodos: el coordinador guarda `nombre → dirección` y el segundo nodo que pide el nombre recibe la dirección existente. Resuelve el problema de "cómo comparten el puntero" sin canales laterales.

**Opción C (mínima): región raíz.**
Reservar la página 0 como tabla de punteros compartida; el nodo 0 escribe ahí las direcciones de los arreglos y, tras un `dsm_barrier`, los demás los leen. Muy poco código, pero no cubre la parte de "qué proceso, en qué orden".

Recomendación: A + B combinadas, porque las aplicaciones de prueba (matrices, archivo grande) las necesitan desde el principio.

---

## 6. Riesgos que no aparecen en una sola máquina de un núcleo

### 6.1 Livelock en páginas disputadas

Un nodo recibe `PAGE_DATA` en modo W, y antes de que el hilo reintente la instrucción llega un `FETCH` que le quita la página. Con multicore y false sharing esto puede repetirse indefinidamente. Las 30 pasadas del avance se hicieron con 1 núcleo.

Mitigaciones:
- **Tiempo mínimo de retención**: en `pager_on_fetch`, si la página se recibió hace menos de unos cientos de microsegundos, esperar un instante antes de ceder la página. Como `h_fetch` corre en el pool sin espera, una pausa corta es aceptable (nunca con el lock tomado).
- **Contador de fallos consecutivos** sobre la misma página, expuesto en estadísticas y en el log verbose, para detectar el livelock en pruebas.
- **Prueba dedicada**: 4 hilos por nodo, 3-4 nodos, mismas páginas, varios minutos, en una máquina con varios núcleos.
- Mantener la mitigación de `dsm_alloc` alineado a página para que las aplicaciones eviten false sharing.

### 6.2 Dos fallos por cada `x++`

Una lectura seguida de escritura sobre una página en I genera dos viajes de red.

Opciones:
- **Medir primero**: ya hay contadores de fallos R y W; ver la proporción en la multiplicación de matrices.
- **API de pistas**: `dsm_prefetch_write(ptr, len)` que pida la página directamente en modo W cuando la aplicación sabe que va a escribir.
- **Solicitud de mejora S→M**: cuando el nodo ya tiene la página en S, pedir el permiso sin reenviar los datos (hoy `PAGE_DATA` siempre lleva la página completa, decisión C documentada; sería un cambio de protocolo, no un arreglo menor).

### 6.3 Spinlock global con `memcpy` y `mprotect` adentro

Es correcto para el handler de señal (un mutex no es async-signal-safe), pero es un cuello de botella potencial y la especificación pide semáforos y mutexes para las tablas.

Opciones:
- **Spinlock por franjas** (`lk(p % NSTRIPES)`), como ya está en el TODO. Medir antes y después.
- **Defensa**: documentar por qué el handler usa spinlock con `sched_yield()` (async-signal-safe) y dónde sí se usan mutex y semáforos (directorio, cola de red, barrier, fallos).
- Verificar que ningún hilo de aplicación mantenga `g_lk` mientras pueda recibir `SIGSEGV` (hoy solo lo toman el handler y los hilos de red, que no tocan la vista de la aplicación).

### 6.4 Dependencias del sistema

`memfd_create` (glibc 2.27+), `MAP_FIXED_NOREPLACE` (kernel 4.17+) y el código de error de `ucontext` (x86-64).

Mitigaciones:
- Correr `uname -m`, `uname -r` y `ldd --version` en el laboratorio **antes que cualquier otra cosa**.
- Alternativa a `memfd_create`: `shm_open` con nombre único seguido de `shm_unlink`.
- Alternativa a `MAP_FIXED_NOREPLACE`: `mmap` con `MAP_FIXED` tras comprobar que el rango está libre con una reserva previa `PROT_NONE`.
- En arquitecturas distintas de x86-64, el respaldo por estado es correcto (una escritura sobre una página en I pide primero lectura y luego escritura) pero más lento, y no está probado: probarlo forzándolo con una macro.

---

## 7. Estadísticas y consultas

### El problema

El estado MSI, el `copyset` y el dueño solo existen en el home de cada página, y son `static` en `directory.c`. Además, `max_payload` es `page_size + 256`, así que un `STATS_REPLY` con una tabla de páginas grande no cabe en un mensaje.

### Opciones

- **Accesores en `directory.c`**: `dir_snapshot(page_id_t first, uint32_t n, dir_entry_t *out)` para lectura de mejor esfuerzo (sin tomar el mutex por franja, o tomándolo brevemente).
- **Tabla de páginas global**: un `STATS_REQUEST` a todos los nodos (handler de la clase `MSG_POOL` sin espera) y cada uno responde con su porción.
- **Paginar las respuestas**: chunks de unas decenas de entradas por mensaje, o respuestas agregadas (conteos por estado MSI y por proceso) en vez de entradas individuales.
- **Salida**: modo texto con tablas formateadas (más simple), o un pequeño servidor HTTP por nodo en un puerto propio, con un hilo dedicado, para la vista en navegador. En una misma máquina, el puerto distingue los nodos.
- **Alloc registrado (punto 5)** da los datos de "qué proceso, qué orden" para estas consultas.

---

## Lista de verificación antes del Prototipo 2

- [ ] Pools separados o directorio asíncrono, probado con `worker_pool_size=1` y 3-4 nodos.
- [ ] Una sola copia de la configuración y tabla de nodos con lock.
- [ ] Decisión de home inactivo documentada.
- [ ] `getaddrinfo` y puertos de control y heartbeat.
- [ ] Generación de transacción (`req_id`) en el directorio y semáforo drenado en `txn_alloc`.
- [ ] `connect_peer` con timeout corto, `SO_SNDTIMEO` y fallo rápido si el nodo está caído.
- [ ] Reintentos en el pager antes de `fatal()`.
- [ ] `PUNCH_HOLE` solo fuera del contribuyente.
- [ ] Alloc global con coordinador y nombres.
- [ ] Pruebas multicore con varios hilos y varios minutos.
- [ ] `uname -m`, kernel y glibc verificados en el laboratorio.
- [ ] Decisión sobre el Extra 2 (hacerlo o documentarlo como limitación).
