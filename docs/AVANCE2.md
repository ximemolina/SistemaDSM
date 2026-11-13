# Avance 2 (Entregable 2): Prototipo 1 del DSM

**Curso:** IC-6600 Principios de Sistemas Operativos, II-2026 

**Proyecto:** DSM con paginación bajo demanda y coherencia MSI

**Autoras:** Ximena Molina y Susana Feng


## 1. Objetivo del avance

Cumplir la meta del Prototipo 1: dos nodos que comparten memoria mediante `mmap`, `SIGSEGV` y los mensajes `PAGE_REQUEST`/`PAGE_DATA`, con doble mapeo, lectura y escritura básicas y un directorio mínimo.

## 2. Resumen de lo logrado

- Los dos nodos comparten memoria y mantienen coherencia MSI (estados I, S, M y los transitorios PENDING_R y PENDING_W).
- El handler de `SIGSEGV` detecta si el fallo fue de lectura o escritura con el código de error del `ucontext` (x86-64).
- El doble mapeo con `memfd_create` evita que el hilo de red reciba `SIGSEGV` al copiar páginas.
- El directorio atiende lecturas y escrituras con `FETCH`, invalidaciones con ACK y write-back perezoso.
- `dsm_barrier` funciona completo, con coordinador calculado por hash.
- La configuración se lee de `dsm.conf`.

## 3. Módulos entregados

| Módulo | Estado | Responsable |
|---|---|---|
| `config.c` | Completo | B |
| `cluster.c` | Parcial (tabla estática, hash de home, sin JOIN ni heartbeat) | B |
| `network.c` | Completo | B |
| `pager.c` | Completo | A |
| `directory.c` | Versión mínima (sin cola explícita) | Ambas |
| `dsm.c` | Parcial (alloc local, barrier completo) | Ambas |
| `apps/test_basic.c` | Completo | Ambas |

## 4. Pruebas realizadas

| Prueba | Qué verifica | Resultado |
|---|---|---|
| A: ping-pong ×100 | Un nodo escribe, el otro lee, en ambos sentidos, con barrier entre pasos | OK |
| D: página del contribuyente | Escritura de un nodo sobre una página cuyo contribuyente es el otro, y lectura cruzada | OK |
| B: estrés | 2 hilos por nodo escribiendo palabras distintas de la misma página (false sharing) | OK |

Se repitió la prueba completa **30 veces**: 30 pasadas, 0 fallidas, sin deadlocks ni timeouts. Entorno: x86-64, 1 núcleo, dos procesos en `127.0.0.1`.

Contadores de una corrida (con `DSM_VERBOSE=1`): el nodo 0 tuvo 63 fallos de lectura y 103 de escritura; el nodo 1 tuvo 45 y 105, con 58 invalidaciones.

## 5. Cambios respecto al diseño del Entregable 1

Se hicieron 15 ajustes, documentados uno por uno en `CAMBIOS_PROTOTIPO1.md`. Los más importantes:

1. El `PAGE_REQUEST` lo envía un hilo emisor, no el handler de `SIGSEGV`, para que el handler use solo funciones async-signal-safe.
2. Un spinlock global reemplaza el semáforo por página.
3. `PAGE_WRITEBACK` pasó a ser INLINE para que se aplique antes de cualquier `FETCH` posterior.
4. `PAGE_DATA` siempre lleva la página completa.
5. `INVALIDATE` en estado PENDING se resuelve sin bandera de dato obsoleto, porque el home serializa por página y TCP es FIFO.

## 6. Problemas encontrados

- **Hash con `max_nodes` fijo frente a asignaciones contiguas.** Si el home de una página cae en un slot inactivo, no se puede asignar. Por ahora `max_nodes` es igual al número de nodos declarados. Hay que decidir la solución antes de implementar `JOIN` dinámico.
- **Orden entre write-back y FETCH.** Con el write-back en el pool, un `FETCH` podía leer la copia vieja. Se resolvió con la clase INLINE.

## 7. Limitaciones conocidas

- No hay JOIN, NODE_LIST, heartbeat, estadísticas, consultas, `dsm_lock` ni alloc remoto.
- No se libera RAM de páginas invalidadas (falta `fallocate` con `PUNCH_HOLE`).
- No hay write-back al apagar un nodo.
- El respaldo de detección lectura/escritura para arquitecturas distintas de x86-64 está escrito pero sin probar.
- Solo se probó en una máquina; falta la prueba en el laboratorio con computadoras físicas.
- La prueba de estrés en un solo núcleo genera pocos fallos reales.

## 8. Evidencia

Código fuente, `Makefile`, `dsm.conf` de ejemplo y scripts en el repositorio del proyecto (carpeta `dsm-prototipo1`). Los tamaños exactos de las estructuras están en `CAMBIOS_PROTOTIPO1.md` (`msg_header_t` = 22 bytes, `dir_entry_t` = 24, `page_local_t` = 12).