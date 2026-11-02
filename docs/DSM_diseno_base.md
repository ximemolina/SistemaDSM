# Diseño base: Sistema DSM con paginación bajo demanda y coherencia MSI
 
> Documento de trabajo para IC-6600 (Principios de Sistemas Operativos, II-2026). Reúne las decisiones de diseño discutidas, el razonamiento detrás de cada una y las preguntas que siguen abiertas. Sirve de base para el **Entregable 1 (6 nov)**; no es el entregable final.
 
---
 
## 0. Estado de las decisiones
 
| # | Decisión | Estado | Resolución |
|---|---|---|---|
| D1 | Asignación del nodo *home* de cada página | **Decidida** | Por hash de `page_id` (ver §2.2) |
| D2 | Espacio de direcciones cuando un nodo se une | **Decidida** | Mapa fijo por *slots* en config, `max_nodes = 16` (ver §2.3) |
| D3 | Extra 1 (ejecución distribuida) | **Indecisa** | Dejar ganchos en protocolo y API (ver §14) |
| D4 | Extra 2 (tolerancia a fallas) | No se intentará por ahora | Documentar limitaciones (ver §10) |
| D5 | Doble mapeo con `memfd_create` | Recomendada | Ver §3.2 |
| D6 | Cómo atiende peticiones el hilo de red | Recomendada | Listener + pool de hilos (ver §8.4) |
| D7 | Lenguaje | **Decidida** | C |
| D8 | Tamaño de página | Por defecto 4 KB, configurable | Ver §12 |
| D9 | Ruta de `PAGE_DATA` | **Decidida** | Pasa por el home (ver §6.3) |
| D10 | Detección de lectura vs. escritura en el handler | **Decidida** | Código de error en `ucontext` (x86-64), con respaldo por estado de página (ver §3.3) |
| D11 | Semántica de `dsm_barrier` | **Decidida** | Solo sincronización, sin write-back (ver §9) |
 
---
 
## 1. Qué es el sistema, en simple
 
Varias computadoras (o terminales) aportan cada una un bloque de memoria. El sistema hace que todos esos bloques parezcan **una sola memoria compartida**. Un programa lee y escribe en ella con punteros normales, sin saber en qué nodo están realmente los datos.
 
El mecanismo:
 
1. Cada nodo reserva un bloque grande con `mmap`.
2. Las páginas que el nodo no tiene se marcan `PROT_NONE` con `mprotect`.
3. Si un hilo toca una de ellas, el kernel envía `SIGSEGV`.
4. El manejador (`sigaction` con `SA_SIGINFO`) lee la dirección en `si_addr`, calcula el número de página, pide la página por TCP y espera.
5. Al llegar, se copia la página, se abre el permiso con `mprotect` y la instrucción fallida se reintenta de forma transparente.
6. El protocolo **MSI** garantiza que nadie lea datos viejos.
---
 
## 2. Arquitectura general
 
### 2.1 Componentes por nodo
 
- **Hilos de trabajo:** ejecutan la aplicación; son los que reciben `SIGSEGV`.
- **Hilo listener de red:** acepta conexiones TCP y lee mensajes.
- **Pool de hilos handler:** procesa peticiones que pueden bloquearse (ver §8.4).
- **Hilo de heartbeat:** envía y vigila latidos.
- **Tabla de nodos:** IP, puerto, memoria aportada, base, estado, último latido.
- **Tabla de páginas local (`page_local_t`):** estado MSI de cada página en este nodo.
- **Directorio (`dir_entry_t`):** solo para las páginas cuyo home (por hash) es este nodo.
- **Tabla de procesos/asignaciones:** qué páginas pertenecen a qué proceso y en qué orden.
### 2.2 Asignación de home por hash (D1, decidida)
 
```
home(page_id) = slot_de( hash(page_id) mod max_nodes )
```
 
- Todos los nodos usan **la misma función hash y el mismo `max_nodes`** (vienen del archivo de configuración).
- Se recomienda una mezcla simple (por ejemplo, *splitmix64*) en lugar de `page_id % N` directo, para que páginas consecutivas (típicas de arreglos y matrices) no caigan todas en el mismo home y para repartir la carga.
**Aclaración importante (corrige la explicación inicial):** con hash, el **home es solo el gestor del directorio**, no necesariamente quien tiene los datos. Hay que distinguir dos conceptos:
 
| Concepto | Qué es | Cómo se determina |
|---|---|---|
| **Home (directorio)** | Nodo que sabe el estado, el owner y el copyset de la página | `hash(page_id)` |
| **Owner inicial (datos)** | Nodo que tiene los bytes al inicio | El nodo que aporta el rango donde cae la página |
| **Owner actual** | Nodo con la versión M, si existe | Se guarda en el directorio |
 
Costo del hash frente al "home = quien aporta la memoria": **un salto de red más** en algunos casos (pedir al home, que reenvía al owner). Se compensa con mejor reparto de carga de coordinación y se documenta como decisión de diseño.
 
### 2.3 Espacio de direcciones y unión de nodos (D2, recomendada)
 
**Problema:** si `home = hash(page_id) mod N` y `N` es la cantidad de nodos *vivos*, al unirse un nodo cambia `N` y **cambia el home de casi todas las páginas**, lo que obliga a migrar todo el directorio. Se debe evitar.
 
**Recomendación: slots fijos definidos en la configuración.**
 
- El archivo de configuración define `max_nodes` (16) y la lista de *slots*: cada slot tiene un `node_id`, IP, puerto y un **rango fijo del espacio global** (`slot_i` cubre `[base + i·mem_per_node, base + (i+1)·mem_per_node)`).
- El espacio global total **siempre existe** con ese tamaño (`max_nodes × mem_per_node`); se mapea con `mmap` en la misma dirección base en todos los nodos. Las páginas cuyo slot no ha entrado todavía simplemente están `PROT_NONE` y no se pueden asignar.
- `home(page_id)` usa `max_nodes` (constante), por lo que **no cambia** cuando entran o salen nodos.
- Cuando un nodo se une, **ocupa su slot**: anuncia su llegada, recibe la tabla de nodos, y sus páginas se vuelven asignables. El directorio de las páginas cuyo home es ese slot se activa en ese nodo.
- Mientras un slot no esté activo, las páginas cuyo home cae ahí no se asignan (evita tener un home inexistente). Se puede mostrar en las estadísticas como "memoria potencial".
Esto cumple el enunciado ("entre más nodos, más memoria disponible") sin migrar directorios. Mensajes: `JOIN` y `NODE_LIST` (§6).
 
**Alternativas descartadas (documentarlas):**
 
- *Hash sobre nodos vivos con migración de directorio:* más flexible, pero requiere un protocolo de migración (congelar páginas, mover entradas) con muchos casos de carrera.
- *Consistent / rendezvous hashing:* mueve solo ~1/N de los homes al unirse un nodo. Es la opción "bonita", pero añade complejidad que no se necesita para la demo.
**Decisión:** `max_nodes = 16`. Es el total de nodos del cluster (un nodo = un proceso DSM, identificado por IP + puerto), sin importar en cuántas computadoras corran. Si el profesor pide nodos "ilimitados", habría que pasar a consistent hashing. El `copyset` de 64 bits permite hasta 64 nodos.
 
---
 
## 3. Memoria virtual y paginación bajo demanda
 
### 3.1 Reserva
 
- Al iniciar, cada nodo mapea el **espacio global completo** en una dirección base común (`MAP_FIXED_NOREPLACE`), de modo que un puntero significa lo mismo en todos los nodos.
- `page_id = (addr - base) / page_size`.
- Linux asigna RAM real solo cuando se toca una página, así que las páginas `PROT_NONE` nunca tocadas **no consumen RAM física**.
### 3.2 Doble mapeo (D5): evita que el hilo de red haga `SIGSEGV`
 
El kernel no distingue hilos "de aplicación" de hilos "del sistema": los permisos de `mprotect` aplican a la memoria. Si el hilo de red hace `memcpy` sobre una página `PROT_NONE`, él mismo recibe `SIGSEGV` y se queda esperando una respuesta que debería procesar él mismo.
 
Solución: crear la memoria con `memfd_create` (o `shm_open`) y mapearla **dos veces**:
 
| Vista | Usuario | Permisos |
|---|---|---|
| **Vista de aplicación** | Hilos de trabajo | Cambian con `mprotect` (NONE / READ / RW) |
| **Vista del sistema** | Hilo de red y handlers | Siempre RW |
 
Ambas apuntan a los mismos bytes. El hilo de red copia por la vista del sistema y **después** abre el permiso en la vista de aplicación, así la app nunca ve una página a medio copiar (`mprotect` afecta a todo el proceso; no existe permiso "solo para este hilo").
 
Notas:
 
- Liberar memoria física de una página invalidada en `memfd`: `madvise(MADV_DONTNEED)` no basta; usar `fallocate` con `FALLOC_FL_PUNCH_HOLE`.
- Páginas invalidadas con contenido viejo ocupan espacio virtual pero, con lo anterior, no RAM física.
### 3.3 Manejador de `SIGSEGV`
 
- Registrar con `sigaction` y `SA_SIGINFO`.
- Obtener `si_addr` y calcular `page_id`.
- **Detección de lectura vs. escritura (decidida):** leer el código de error del fallo en el `ucontext_t` (tercer argumento del handler). En x86-64 Linux, `((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2` es 1 si el acceso fue una escritura (requiere definir `_GNU_SOURCE`). Verificar con `uname -m` que los equipos del laboratorio son x86-64.
- **Respaldo (por estado de la página):** si la página está en S (`PROT_READ`), un fallo solo puede ser escritura; si está en I, tratarlo como lectura y pedir M al fallar de nuevo (costo: dos viajes por red). Se usa solo si el código de error no está disponible.
- Si la dirección está fuera del espacio DSM, es un fallo real: restaurar el manejador por defecto y re-lanzar.
**Cuidado con funciones en handlers:** solo son seguras las *async-signal-safe*. `malloc` y `pthread_mutex_lock` **no** lo son. Reglas:
 
- Handler mínimo: calcular página, registrar el fallo, enviar la petición, esperar el semáforo.
- Estructuras de fallo **prealocadas** (arreglo fijo, por hilo), sin `malloc`.
- El código del runtime DSM nunca debe tocar memoria DSM mientras tiene un lock tomado.
---
 
## 4. Protocolo de coherencia MSI
 
### 4.1 Estados
 
| Estado | Significado | Permiso local (`mprotect`) |
|---|---|---|
| **I** (Invalid) | No hay copia válida | `PROT_NONE` |
| **S** (Shared) | Copia de solo lectura; puede haber varias | `PROT_READ` |
| **M** (Modified) | Única copia válida y escribible | `PROT_READ\|PROT_WRITE` |
| **PENDING_R / PENDING_W** (extra) | Hay una petición en vuelo para leer / escribir | Sin cambio hasta que llegue la respuesta |
 
El estado transitorio `PENDING` (la especificación permite estados extra si se justifican) existe para: (a) que dos hilos del mismo nodo que fallan en la misma página no envíen dos peticiones, y (b) detectar un `INVALIDATE` que llega mientras se espera la página.
 
### 4.2 Transiciones
 
| Evento | De | A | Acción |
|---|---|---|---|
| Lectura local | I | S | `PAGE_REQUEST(READ)`; si otro nodo está en M, baja a S |
| Escritura local | I ó S | M | `PAGE_REQUEST(WRITE)`; el home invalida el resto |
| Otro nodo lee | M | S | Envía datos (y write-back al home si aplica) |
| Otro nodo escribe | M ó S | I | `INVALIDATE` (+ datos si era M) |
| `INVALIDATE` recibido | S | I | `mprotect(PROT_NONE)` + ACK |
 
### 4.3 Comportamiento perezoso (lazy)
 
- B en M **no devuelve** la página por iniciativa propia. Se queda en M hasta que otro nodo la pida.
- Si otro nodo **lee**: B baja de M a S (el hilo de red de B hace `mprotect(PROT_READ)` y envía los datos); si B vuelve a escribir, falla, pide M otra vez y se invalidan las copias S.
- Si otro nodo **escribe**: los datos pasan de B al nuevo escritor; B queda en I.
- El `mprotect` de B surte efecto de inmediato aunque su hilo de trabajo esté en un ciclo cerrado, así que no hay *starvation* del que pide.
- Una instrucción interrumpida (`a[i]++`) no se corrompe: la escritura falla, se pide el permiso y se **reintenta**; los datos enviados corresponden a escrituras completas.
### 4.4 Cuándo se actualiza el espacio del home/contribuyente
 
Con home por hash y owner inicial por rango, hay que decidir **dónde queda la copia "de referencia"** cuando un dato en M viaja. Política recomendada (write-back perezoso):
 
| Evento | Se actualiza la copia de referencia |
|---|---|
| El owner escribe y sigue en M | No |
| Otro nodo lee (M→S) | **Sí**: el owner envía los datos y se guarda copia en el contribuyente (mensaje `PAGE_WRITEBACK`) |
| Otro nodo escribe (M→I) | No (el dato cambia de owner) |
| Apagado ordenado de un nodo | **Sí**: write-back de sus páginas en M |
| Caída repentina de un nodo en M | **No: se pierde el dato** (solo lo resolvería el Extra 2) |
 
Esta limitación debe quedar documentada en el informe.
 
### 4.5 Condición de carrera clásica
 
B está en `PENDING` esperando la página 7 y le llega un `INVALIDATE` de esa misma página (porque el home ya procesó otra petición). B no tiene copia válida: debe **responder ACK de inmediato** y recordar que el `PAGE_DATA` que llegue puede ser obsoleto (se resuelve con el estado `PENDING` y `req_id`/números de secuencia). Preparar un diagrama de secuencia de este caso; es una pregunta típica de defensa.
 
---
 
## 5. Estructuras de datos
 
La versión inicial mezclaba estado local y directorio en una sola estructura. Es más claro **separarlas**:
 
```c
typedef enum {
    PAGE_INVALID, PAGE_SHARED, PAGE_MODIFIED,
    PAGE_PENDING_R, PAGE_PENDING_W
} page_state_t;
 
/* Una por página, en TODOS los nodos: estado local */
typedef struct {
    page_state_t state;
    uint32_t     pid_owner;   /* proceso al que pertenece */
    uint32_t     seq_order;   /* orden de la página dentro del proceso */
    sem_t        lock;        /* protege solo este registro, se toma un instante */
} page_local_t;
 
/* Solo en el nodo home (por hash) de cada página: directorio */
typedef struct {
    uint32_t owner_node;      /* quién tiene la versión M (o el owner inicial) */
    uint64_t copyset;         /* bitmap de nodos con copia S (máx. 64 nodos) */
    uint8_t  busy;            /* transacción en curso para esta página */
    /* cola de peticiones pendientes de esta página */
} dir_entry_t;
 
typedef struct {
    uint32_t node_id;
    char     ip[46];
    uint16_t port;
    uint64_t mem_bytes;
    uint64_t base_addr;
    uint8_t  alive;
    time_t   last_heartbeat;
} node_info_t;
```
 
Qué responde cada campo del directorio:
 
| Campo | Pregunta |
|---|---|
| home | ¿Quién lleva el directorio? (fijo, por hash) |
| `owner_node` | ¿Quién tiene los datos más recientes? (cambia) |
| `copyset` | ¿Quiénes tienen copia de solo lectura? (cambia) |
| `busy` + cola | ¿Hay una transacción en curso? ¿Quién espera? |
 
Añadir: tabla de procesos/asignaciones, estructura de configuración y `fault_ctx_t` (ver §8.5). Documentar el tamaño exacto en bytes de cada estructura (lo pide el Entregable 1).
 
---
 
## 6. Protocolo de mensajes (TCP)
 
### 6.1 Encabezado fijo
 
```c
typedef struct __attribute__((packed)) {
    uint8_t  type;         /* tipo de mensaje */
    uint32_t src_node;
    uint64_t page_id;
    uint32_t req_id;       /* empareja respuestas con peticiones */
    uint8_t  access;       /* READ o WRITE */
    uint32_t payload_len;  /* bytes del cuerpo que siguen al encabezado */
} msg_header_t;
```
 
### 6.2 Framing (por qué existe `payload_len`)
 
TCP es un **flujo de bytes**, no de mensajes. El receptor:
 
1. Lee exactamente `sizeof(msg_header_t)` bytes.
2. Lee `payload_len`.
3. Lee exactamente esa cantidad de bytes más.
```c
msg_header_t hdr;
read_exact(sock, &hdr, sizeof(hdr));
void *payload = NULL;
if (hdr.payload_len > 0) {
    payload = malloc(hdr.payload_len);
    read_exact(sock, payload, hdr.payload_len);
}
```
 
`read_exact` es una función propia que repite `recv` hasta completar los bytes (un `recv` puede devolver menos).
 
Cuidados:
 
- **Validar** `payload_len` contra un máximo (page_size + margen); si no, rechazar.
- **Endianness:** convertir campos multibyte con `htonl`/`ntohl` (y equivalente de 64 bits).
- **Padding:** usar `packed` y documentar el tamaño exacto del encabezado.
### 6.3 Tipos de mensaje
 
| Mensaje | Dirección | `payload_len` | Descripción |
|---|---|---|---|
| `PAGE_REQUEST` | solicitante → home | 0 | Pide página con acceso READ o WRITE |
| `PAGE_DATA` | owner/home → solicitante | `page_size` | Contenido + permiso concedido |
| `FETCH` | home → owner | 0 | El home pide los datos frescos al owner |
| `INVALIDATE` | home → nodos del copyset | 0 | Invalidar la copia local |
| `INVALIDATE_ACK` | nodo → home | 0 | Confirma la invalidación |
| `PAGE_WRITEBACK` | owner → contribuyente/home | `page_size` | Actualiza la copia de referencia |
| `JOIN` | nodo nuevo → semilla / todos | `sizeof(node_info_t)` | Anuncia un nodo nuevo |
| `NODE_LIST` | semilla → nodo nuevo | `n·sizeof(node_info_t)` | Tabla de nodos |
| `HEARTBEAT` | nodo → todos | 0 ó pequeño | Latido |
| `ALLOC_REQUEST` / `ALLOC_REPLY` | nodo ↔ nodo | variable | Pedir/ceder memoria cuando la local no alcanza |
| `STATS_REQUEST` / `STATS_REPLY` | nodo ↔ nodo | variable | Consultas globales |
| `BARRIER_ARRIVE` | participante → coordinador | 0 ó pequeño | Un participante llegó al barrier `id` (el `id` puede viajar en `page_id`) |
| `BARRIER_RELEASE` | coordinador → participantes | 0 | Llegó el último; todos continúan |
 
Para cada mensaje, el informe debe documentar: quién lo envía, parámetros, respuesta esperada, errores posibles y significado.
 
**Decisión:** `PAGE_DATA` llega al solicitante **a través del home**, que así puede seguir toda la transacción. El envío directo del owner ahorra un salto de red pero complica ese seguimiento; queda como posible optimización futura.
 
---
 
## 7. Flujos principales
 
### 7.1 Lectura de una página en I
 
1. Fallo → handler → `PAGE_REQUEST(READ)` al home.
2. El home consulta su directorio:
   - Si `owner_node` está en M: `FETCH` al owner → el owner baja a S, envía datos (y write-back).
   - Si no, los datos ya están disponibles en algún nodo del copyset o en el contribuyente.
3. `PAGE_DATA` al solicitante. Se copia por la vista del sistema, `mprotect(PROT_READ)`, el home añade al nodo al `copyset`.
### 7.2 Escritura de una página en I o S
 
1. `PAGE_REQUEST(WRITE)` al home.
2. El home envía `INVALIDATE` a todo el `copyset` (menos el solicitante) y **espera todos los ACK** (de forma no bloqueante para su hilo de red, §8.4).
3. Entrega `PAGE_DATA` (o permiso, si el solicitante ya tenía los datos en S). `owner_node = solicitante`, copyset vacío, estado M, `mprotect(RW)`.
### 7.3 Dos peticiones simultáneas a la misma página
 
Cada nodo tiene **sus propios locks**; los demás nodos no pueden tomarlos. Entre nodos solo hay mensajes. El **home actúa de árbitro**:
 
- Atiende **una transacción por página a la vez** (`busy = 1`).
- Si llega otra petición para la misma página, la deja en la **cola de la página** y sigue atendiendo otros mensajes (el hilo de red no se bloquea).
- Al terminar la transacción, saca la siguiente de la cola.
El orden lo decide el home por llegada. El que llega segundo **espera**, pero no hay deadlock porque nadie retiene un lock que el otro necesite.
 
---
 
## 8. Concurrencia, hilos y deadlocks
 
Un deadlock requiere una **espera circular**. Las reglas siguientes evitan que se forme.
 
### 8.1 Regla 1: no retener el lock de página mientras se espera la red
 
El `page->lock` (semáforo POSIX) protege **solo** la nota de estado de esa página en ese nodo. Lo toman el hilo de trabajo y el hilo de red. Se toma un instante y se suelta **antes** de esperar a otro nodo.
 
**Caso malo:** el hilo de trabajo de B toma el lock de la pág. 7, envía `PAGE_REQUEST` y se queda esperando con el lock. Llega un `INVALIDATE`; el hilo de red de B necesita ese lock y se bloquea. El home espera el ACK de B; B espera el `PAGE_DATA` del home. Espera circular.
 
**Patrón correcto:**
 
```c
sem_wait(&page->lock);
page->state = PAGE_PENDING_R;   /* "ya hay una petición en vuelo" */
sem_post(&page->lock);          /* se suelta ANTES de esperar la red */
 
enviar_page_request(...);
esperar_respuesta(...);         /* sin ningún lock de página */
 
sem_wait(&page->lock);
page->state = PAGE_SHARED;
sem_post(&page->lock);
```
 
`PENDING` hace de aviso durante la espera sin bloquear a nadie. Si otro hilo de B falla en la misma página, ve `PENDING` y espera el resultado en lugar de enviar una segunda petición.
 
### 8.2 Regla 2: el hilo de red nunca toca páginas por la vista de la aplicación
 
Ver §3.2 (doble mapeo).
 
### 8.3 Regla 3: orden global de adquisición de locks
 
Jerarquía documentada, por ejemplo: `tabla_nodos` → `tabla_procesos` → `tabla_fallos` → `page_lock[id]` con `id` ascendente. Nunca tomar un lock de jerarquía superior teniendo uno inferior.
 
### 8.4 Regla 4: el hilo de red no espera respuestas de red
 
**Problema:** el home recibe un `PAGE_REQUEST`, manda `FETCH` y se queda esperando; la respuesta le llega al mismo hilo, que está dormido.
 
**Solución elegida (D6): listener + pool de hilos.**
 
- El **listener** lee mensajes. Las **respuestas** (`PAGE_DATA`, `INVALIDATE_ACK`, `HEARTBEAT`) las procesa él directamente, porque su trabajo es rápido y nunca espera (copiar, postear un semáforo).
- Las **peticiones nuevas** (`PAGE_REQUEST`, `INVALIDATE`, `FETCH`, etc.) se encolan para el **pool**, cuyos hilos sí pueden bloquearse.
- Así un hilo del pool bloqueado siempre es despertado por el listener, nunca por otro hilo del pool (evita deadlock por agotamiento del pool).
**Tamaño del pool:** `worker_pool_size = max(4, 2 × nodos)` como punto de partida, configurable en el archivo de configuración. Un pool pequeño agota hilos; uno grande gasta pila y cambios de contexto (error menos grave). En el informe, medir con 1, 4, 8 y 16 hilos y comparar tiempos.
 
**Alternativa:** máquina de estados en el listener (sin bloqueo). Más elegante pero más difícil de razonar.
 
### 8.5 Regla 5: timeouts en toda espera de red
 
Usar `sem_timedwait` / `pthread_cond_timedwait`. Al vencer: reintentar, marcar al otro nodo como caído (con ayuda del heartbeat) o devolver error al API. Los valores van en el archivo de configuración.
 
### 8.6 Coordinación handler ↔ hilo de red
 
Es el mecanismo para que el hilo que falló se entere de que la página llegó. Analogía: el **timbre** del restaurante.
 
```c
/* Handler (corre en el hilo de trabajo que falló) */
void sigsegv_handler(int sig, siginfo_t *si, void *ctx) {
    uint64_t page_id = ((char*)si->si_addr - base) / PAGE_SIZE;
 
    fault_ctx_t *f = tomar_fault_ctx_prealocado();
    sem_init(&f->done, 0, 0);     /* 0 = nadie ha avisado todavía */
    f->page_id = page_id;
    registrar_fallo(f);           /* para que el hilo de red lo encuentre */
 
    enviar_page_request(page_id, acceso);
    sem_timedwait(&f->done, ...); /* el hilo duerme aquí */
    /* al despertar la página ya tiene el permiso correcto */
}
 
/* Hilo de red, al recibir PAGE_DATA */
void on_page_data(msg_t *m) {
    memcpy(vista_sistema + off(m->page_id), m->payload, PAGE_SIZE); /* 1. datos */
    mprotect(vista_app + off(m->page_id), PAGE_SIZE, permiso);      /* 2. permiso */
    actualizar_estado(m->page_id, nuevo_estado);
    sem_post(&fault_de(m->page_id)->done);                          /* 3. timbre */
}
```
 
El **orden importa**: datos, permiso, aviso. Al terminar el handler, el kernel reejecuta la instrucción que falló.
 
Detalles:
 
- Cada fallo tiene **su propio semáforo**; la tabla de fallos (indexada por `page_id`) tiene su propio lock, tomado un instante.
- Si el `PAGE_DATA` llega antes que el `sem_wait`, no se pierde el aviso: el `sem_post` sube el contador a 1 y el `sem_wait` pasa directo.
- **Dos hilos fallan en la misma página:** solo uno envía `PAGE_REQUEST`; el otro se anota en la misma entrada y espera. Al llegar la página se hace un `sem_post` por cada hilo esperando.
- Usar `sem_timedwait` para no quedar dormidos si el otro nodo cae.
### 8.7 Resumen de riesgos
 
| Riesgo | Regla |
|---|---|
| Lock tomado durante una espera de red | Soltar antes de esperar; usar `PENDING` |
| El hilo de red hace `SIGSEGV` | Doble mapeo |
| Locks cruzados entre hilos | Orden global de adquisición |
| Listener esperando su propia respuesta | Pool + listener que no espera |
| Nodo caído deja todo colgado | Timeouts y reintentos |
| Handler que se bloquea solo | Handler mínimo, sin `malloc` ni mutex |
 
---
 
## 9. Administración distribuida
 
- **Unión (`JOIN`):** el nodo nuevo contacta a un nodo semilla (del archivo de configuración), recibe `NODE_LIST` y anuncia su llegada a los demás. Todos registran IP, memoria aportada, etc.
- **Heartbeat:** cada nodo envía periódicamente un latido a los demás; sin latidos durante `heartbeat_timeout` se marca como caído (parámetros en la configuración).
- **Asignación (`dsm_alloc`):** el proceso lo pide al nodo local. Si cabe en memoria local, se atiende ahí; si no, se piden páginas a otros nodos (`ALLOC_REQUEST`). Se registra qué páginas pertenecen a qué proceso y en qué **orden** (necesario para la consulta de uso del sistema).
- **Barrier (`dsm_barrier(id, n)`):** punto de encuentro; cada participante espera hasta que lleguen los `n`. El coordinador es `hash(id) mod max_nodes`, cuenta los `BARRIER_ARRIVE` y, al llegar el último, envía `BARRIER_RELEASE` a todos. **Es solo sincronización: no fuerza write-back**, porque la coherencia MSI ya garantiza que quien lea después reciba el dato correcto desde el owner. Con timeout, como el resto de las esperas.
- **Consultas:** desde cualquier nodo, estadísticas (uso, consumo, disponibilidad), tabla de páginas local y global, uso y asignación por nodo y proceso. Salida de texto con formato claro o HTML (si hay varios nodos en la misma máquina, el HTML debe poder mostrar cada nodo por separado).
---
 
## 10. Tolerancia a fallas (Extra 2: no previsto, pero conviene documentar)
 
Aunque no se implemente, el informe puede explicar la limitación actual:
 
- Si un nodo cae mientras es el único con la versión M de una página, **ese dato se pierde**.
- Si cae el nodo home de un conjunto de páginas, **se pierde su directorio** (con home por hash esto afecta a páginas repartidas por todo el espacio).
- Mitigaciones posibles: write-through al contribuyente, réplicas periódicas, o replicar el directorio en un nodo secundario (con costo de tráfico y complejidad).
---
 
## 11. API del DSM (borrador)
 
| Función | Propósito |
|---|---|
| `dsm_init(config_path)` | Lee configuración, reserva memoria, registra el manejador, une al nodo |
| `dsm_alloc(size)` | Reserva memoria del espacio compartido; devuelve puntero |
| `dsm_free(ptr)` | Libera |
| `dsm_barrier(id, n)` | Sincroniza a `n` participantes (solo sincronización, sin write-back) |
| `dsm_lock(id)` / `dsm_unlock(id)` | Exclusión mutua distribuida |
| `dsm_stats(dsm_stats_t*)` | Estadísticas del sistema |
| `dsm_shutdown()` | Cierre ordenado con write-back |
 
Documentar para cada función: tipos, parámetros, entradas, salidas y **códigos de error** (por ejemplo `DSM_ERR_NOMEM`, `DSM_ERR_TIMEOUT`, `DSM_ERR_NODE_DOWN`).
 
---
 
## 12. Archivo de configuración (`dsm.conf`)
 
Compartido y leído por todos los nodos.
 
| Parámetro | Descripción | Ejemplo |
|---|---|---|
| `page_size` | Tamaño de página | 4096 |
| `mem_per_node` | Memoria que aporta cada nodo | 1 GB |
| `max_nodes` | Slots del espacio global (usado en el hash); total de nodos del cluster | 16 |
| `global_base_addr` | Dirección base común | `0x700000000000` |
| `nodes[]` | Lista de slots: node_id, IP/nombre | ... |
| `port_main` | Puerto de datos | rango asignado a la pareja |
| `port_control`, `port_heartbeat` | Otros puertos | ... |
| `heartbeat_interval`, `heartbeat_timeout` | Latidos | 1 s / 5 s |
| `request_timeout` | Timeout de esperas de página | ... |
| `worker_pool_size` | Hilos del pool | 8 |
| `max_payload` | Límite de `payload_len` | `page_size + margen` |
| `hash_seed` | Semilla del hash | ... |
 
Usar el **rango de puertos asignado a la pareja**. Documentar todo parámetro nuevo que se agregue.
 
---
 
## 13. Módulos de código
 
`config.c`, `dsm.c` (API y alloc), `pager.c` (handler y MSI local), `directory.c` (directorio en el home), `network.c` (listener, pool, mensajes), `heartbeat.c`, `stats.c`, `main.c`, más `Makefile`. Repositorio Git con commits de **ambas personas** durante las semanas del proyecto.
 
---
 
## 14. Extra 1 (indeciso): ganchos para no cerrar la puerta
 
Para que decidirlo después no obligue a rehacer el protocolo:
 
- Reservar tipos de mensaje: `SPAWN_TASK`, `TASK_DONE`, `TASK_STATUS`.
- Mantener en la tabla de nodos un campo de **carga/CPU disponible**.
- En el API, reservar `dsm_spawn(func_id, args)` y `dsm_wait(task_id)`.
- Los datos de entrada y salida de las tareas viven en el espacio DSM; el mensaje solo transporta el id de función, punteros y parámetros.
No empezar el Extra 1 hasta tener el Prototipo 2 estable. Es la opción de menor riesgo de los dos extras.
 
---
 
## 15. Aplicaciones de prueba y rendimiento
 
Se requieren **2 aplicaciones reales** adaptadas al API (no `malloc` ni llamadas normales de memoria). Ideas: multiplicación de matrices distribuida, incremento concurrente de arreglos, almacenar un archivo grande (video) en memoria distribuida.
 
Cuidados específicos de DSM:
 
- **False sharing:** dos nodos escriben datos distintos que caen en la misma página de 4 KB y esta viaja de ida y vuelta (*ping-pong*). Mitigar alineando estructuras a página o repartiendo rangos por nodo.
- Con el hash de home, distribuir bien los accesos evita que un solo nodo sea cuello de botella.
- Documentar escenarios con mucha memoria, uso por nodo, estadísticas, rendimiento (con diferentes tamaños de pool, de página y número de nodos) y problemas encontrados.
- La demo es en el laboratorio del TEC, con varias computadoras físicas (no personales), ambas personas presentes y capaces de explicar todo.
---
 
## 16. Plan por entregas
 
| Fecha | Entregable | Meta |
|---|---|---|
| **6 nov** | Entregable 1 | Este documento convertido a diseño formal: arquitectura, API borrador, protocolo, configuración, estructuras y tamaños, diagramas |
| **13 nov** | Prototipo 1 | 2 nodos: `mmap` + `SIGSEGV` + `PAGE_REQUEST/DATA`, doble mapeo, lectura y escritura básicas |
| **20 nov** | Prototipo 2 | MSI completo con invalidaciones, N nodos, hash de home, heartbeat, estadísticas, primera app |
| **27 nov** | Demo | Segunda app, pruebas en laboratorio, pulido |
 
**Experimento de una tarde (antes de todo):** en un solo proceso, `mmap` un bloque, ponerlo `PROT_NONE`, registrar `sigaction`, y en el handler hacer `mprotect` a RW. Si el programa continúa, la parte más rara del proyecto queda desmitificada.
 
---
