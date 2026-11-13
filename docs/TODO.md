# Siguientes pasos después del Avance 2

## 1. Decisiones pendientes (primero)

| Decisión | Opciones | Cuándo |
|---|---|---|
| Home inactivo con asignaciones contiguas | (a) arrancar todos los nodos y fijar `max_nodes` igual a los nodos de la corrida; (b) delegar homes inactivos a un slot activo; (c) consistent hashing | Antes de implementar JOIN |
| `memfd_create` o `shm_open` (Q-D) | `memfd_create` funciona bien; solo documentar | Esta semana |
| Directorio del home caído (Q-G) | Documentar como limitación o replicar | Después del Prototipo 2 |
| Extra 1 (Q-H) | Sí o no | Solo tras un Prototipo 2 estable |

## 2. Plan del Prototipo 2 (14 al 20 de nov)

| Prioridad | Tarea | Responsable |
|---|---|---|
| 1 | Cola de peticiones por página en el directorio (reemplaza el mutex por franja) | Ambas |
| 2 | Lock del pager por franjas en vez de spinlock global (medir antes y después) | A |
| 3 | `fallocate(PUNCH_HOLE)` al invalidar páginas | A |
| 4 | `JOIN`, `NODE_LIST` y heartbeat con detección de nodo caído | B |
| 5 | Write-back al apagar (`dsm_shutdown`) | Ambas |
| 6 | `ALLOC_REQUEST/REPLY` y registro de páginas por proceso y orden | B (remoto), A (local) |
| 7 | `stats.c`: estadísticas, tabla de páginas local y global, salida en texto o HTML | B (A da accesores) |
| 8 | Pruebas con 3 o 4 nodos en una máquina | Ambas |
| 9 | Primera aplicación real (multiplicación de matrices) en N nodos | A |

**Listo cuando:** MSI completo con N nodos, caso `INVALIDATE` en PENDING cubierto por una prueba, estadísticas consultables y la primera aplicación con salidas correctas.

## 3. Documentación a preparar

- Diagrama de secuencia del `INVALIDATE` en PENDING (cambio C11).
- Diagrama de secuencia de escritura con invalidaciones.
- Actualizar el protocolo de mensajes y los tamaños de estructuras.
- Entregable 3 (20 nov): cambios de arquitectura del Prototipo 2.

## 4. Demo (21 al 27 de nov)

| Tarea | Responsable |
|---|---|
| Aplicación de prueba 1 | A |
| Aplicación de prueba 2 | B |
| Pruebas en el laboratorio con computadoras físicas (no personales) | Ambas |
| Rendimiento con pool de 1, 4, 8 y 16 hilos; distintos tamaños de página y número de nodos | Ambas |
| Informe final y manual de usuario | A: MSI, memoria, handler. B: protocolo, red, administración |
| Ensayo de la defensa (cada una explica el módulo de la otra) | Ambas |

## 5. Verificaciones tempranas

- Correr `uname -m` en el laboratorio (el diseño asume x86-64).
- Probar el proyecto entre dos máquinas físicas lo antes posible, no solo en `127.0.0.1`.
- Confirmar el rango de puertos asignado a la pareja y ponerlo en `dsm.conf`.
- Mantener commits de ambas en Git cada semana.

## 6. Riesgos

| Riesgo | Mitigación |
|---|---|
| Carreras que no aparecen en un solo núcleo | Pruebas con 3 o 4 nodos, más núcleos y repeticiones largas |
| Spinlock global como cuello de botella | Medir y pasar a franjas |
| Contribuyente como punto caliente de lecturas | Pedir los datos a un miembro del copyset |
| Integración tardía | Fecha fija de integración el 18 de nov |