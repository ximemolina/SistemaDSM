# Aplicaciones de gran memoria sobre el DSM (demo en vivo)

Este documento cubre el entregable de aplicaciones: dos programas diferentes e independientes,
adaptados al API del DSM del proyecto, con escenarios, métricas y guía de demo.

## 1. Cumplimiento de la especificación

| Requisito | Cómo se cumple |
|---|---|
| Al menos dos tipos de aplicaciones diferentes e independientes | `apps/app_pagerank.c` (análisis de grafos / SpMV) y `apps/app_heat3d.c` (stencil 3D de ecuación del calor). Binarios, configs y puertos distintos; pueden correr simultáneamente. |
| Terminales distintas o computadoras en la red | Un proceso por nodo con `DSM_NODE_ID`. En una máquina: 4 terminales con `dsm.local.conf` y `dsm.local2.conf`. En laboratorio: `dsm.conf` con las IPs reales. |
| Investigar problemas que requieren gran memoria; localizar 2 implementaciones | Sección 2. Se eligieron PageRank-CSR y el stencil 3D heat-3d, ambos clásicos *memory-bound*. |
| Adaptar el código al API DSM, no a peticiones regulares del SO | Sección 3. Los datos viven en `dsm_alloc_named`; la sincronización usa `dsm_barrier`, `dsm_lock`/`dsm_unlock`; no hay `malloc`, `mmap`, OpenMP ni MPI en las apps. |
| Demostrar funcionalidad del DSM | Sección 7: contadores reales de fallos, páginas recibidas, invalidaciones, fetch, punch y bytes TX en 2 y 3 nodos. |
| Documentar escenarios, uso de memoria en múltiples nodos, datos, estadísticas, rendimiento y problemas | Secciones 6, 7 y 9. |
| Prueba en máquinas físicas, demo de 1-2 h por pareja | Sección 8. Incluye reparto de roles y guion de defensa. |

## 2. Investigación

Los dos kernels elegidos son representativos de cargas donde el cuello de botella es la memoria,
no la CPU:

* **SpMV / PageRank.** Williams et al. (*Auto-tuning Memory Intensive Kernels for Multicore*,
  PERI) miden SpMV con intención aritmética forzada muy baja: 2 FLOP por cada `double` e índice
  leídos de DRAM (<https://www.davidhbailey.com/dhbpapers/scidac08_peri-a.pdf>). La encuesta de
  Xiao et al. (ACM Computing Surveys, 2023) explica que los accesos irregulares y el tamaño de la
  matriz impiden que quepa en caché (<https://dl.acm.org/doi/10.1145/3604606>).
* **Stencils 3D.** El mismo informe PERI describe la ecuación del calor 3D como otro kernel
  *memory-bound*: 8 FLOP y dos arreglos completos por barrido, con intensidad forzada 0.33.

Implementaciones localizadas y adaptadas:

1. **PageRank CSR**: `step2.c`/`step3.c` de <https://github.com/purtroppo/PageRank> (versión CSR
   secuencial y versión OpenMP) y <https://github.com/louridas/pagerank> (diseño para millones de
   vértices con matriz dispersa). Se conservó la representación CSR transpuesta y el método de
   potencias; se sustituyó OpenMP por partición de filas entre nodos DSM.
2. **Heat-3D**: `heat-3d.c` de PolyBench/C
   (<https://github.com/Meinersbur/polybench/blob/master/polybench-code/stencils/heat-3d/heat-3d.c>)
   y `stencil/3dHeat/heat.c`
   (<https://github.com/SumedhArani/Parallel-Computing/blob/master/stencil/3dHeat/heat.c>).
   El intercambio de halo MPI se sustituyó por páginas compartidas con coherencia MSI, siguiendo
   la idea de descomposición por planos de
   <https://github.com/vinayakbhosle001/Parallel-3D-Stencil-Computation-using-MPI>.

## 3. Adaptación al API DSM

| Mecanismo de la implementación original | Reemplazo en este proyecto |
|---|---|
| `malloc` / `new[]` / `mmap` de matrices y vectores | `dsm_alloc_named("pr_rowptr", ...)`, `dsm_src`, `pr_rank`, `pr_next`, `heat_cur`, `heat_nxt`, `heat_ctrl`. Todos los nodos piden el mismo nombre y reciben el mismo puntero. |
| Inicialización secuencial de la matriz | Inicialización cooperativa: cada nodo llena sólo su rango de filas (PageRank) o sus planos `z % nodos == id` (heat). |
| `#pragma omp parallel for` | Reparto estático de filas/planes alineados a página; un hilo de aplicación por nodo. |
| `atomic` sobre `p_next[dst]` (push) | Modelo **pull** con CSR transpuesto: cada nodo escribe sólo las filas que posee, evitando contención. |
| Reducción `reduction(+:sum)` de OpenMP/MPI | Acumulador en memoria DSM protegido con `dsm_lock`/`dsm_unlock`. |
| `barrier` de OpenMP / `MPI_Barrier` | `dsm_barrier(id, n)`, con ids únicos por iteración. |
| `MPI_Isend`/`MPI_Irecv` de halo | No hay copia de halo: el nodo lee planos vecinos directamente; un `SIGSEGV` trae la página y MSI mantiene la coherencia. |
| `MPI_Allreduce` del residuo | `delta_acc` en DSM + lock + barrier; el nodo 0 decide convergencia y publica `conv`. |
| `*p_curr` ↔ `*p_next` swap local | Intercambio de punteros locales; los arreglos físicos siguen en DSM. |

Reglas aplicadas en el código: los rangos escritos por nodos distintos empiezan en límites de
página (PageRank usa `align = lcm(4096/8, 4096/4) * nodos = 1024 * nodos` entradas; heat usa
un plano = una página de 4096 B), y las variables de control se separan en páginas distintas para
no compartir línea con los acumuladores.

## 4. Aplicación 1: PageRank distribuido

* Grafo regular generado de forma determinista: para cada destino `d`, sus `DEG` enlaces entrantes
  son `(d + k*N/DEG) mod N`, `k = 0..DEG-1`. Cada vértice tiene exactamente `DEG` salidas, así que
  no hace falta arreglo de grados y la suma de `rank` es invariante.
* Iteración: `next[d] = (1-0.85)*pref(d) + 0.85 * Σ rank[src] / DEG`, con
  `pref(d) = ((d mod 97)+1) / (49 * Σ pref)` normalizado para que `Σ pref = 1`.
* Partición: `rows_per_node = N / nodos`, `d0 = id * rows_per_node`. `rowptr`, `src`, `rank` y
  `next` se escriben sólo en el rango propio.
* Sincronización por iteración: reset de acumuladores → barrier → cálculo local → lock/suma →
  barrier → nodo 0 evalúa L1 y publica `conv` → barrier → swap de buffers.
* Verificación: suma global de `rank` ≈ 1, estructura CSR correcta en una muestra y L1 decreciente.
* Memoria: `(N+1)*4 + N*DEG*4 + 2*N*8 + 2*page_size` bytes.
* Uso: `app_pagerank dsm.conf [N] [DEG] [ITER]` (defaults `131072 16 12`).

## 5. Aplicación 2: ecuación del calor 3D

* Dominio `NX × NY × NZ`, con `NY = 8` y `NX = page_size/(NY*8) = 64`, de modo que **un plano Z
  ocupa exactamente una página**.
* Stencil de 7 puntos de PolyBench: `T' = T + 0.125*(vecinos - 6*T)`, con condiciones de
  frontera de Dirichlet.
* Solución manufacturada: `T(x,y,z) = 20 + 0.01x + 0.02y + 0.03z`. El laplaciano discreto es
  cero, por lo que el esquema debe dejar el campo invariante; eso da una verificación exacta.
* Partición: planos `1..NZ-2`, en bloques contiguos por nodo; los planos `0` y `NZ-1` quedan fijos.
* Sincronización por paso: reset de suma → barrier → stencil sobre `cur` escribiendo `nxt` →
  lock/suma → barrier → impresión → swap.
* Verificación: error máximo `< 1e-9` entre el campo final y la solución analítica.
* Memoria: `2 * NZ * page_size + 2*page_size` bytes.
* Uso: `app_heat3d dsm.conf [NZ] [STEPS]` (defaults `2048 8`).

## 6. Escenarios y distribución de memoria

| Escenario | Config | PageRank | Heat-3D | Total usado / total cluster |
|---|---|---|---|---|
| A. Demo rápida | 2 nodos × 16 MiB | N=16 384, DEG=16 → 1.32 MiB | NZ=256 → 2.01 MiB | ≤ 2 MiB / 32 MiB |
| B. Demo estándar | 2 nodos × 16 MiB | N=131 072, DEG=16 → **10.51 MiB** (rowptr 0.50, src 8.00, vectores 2.00, ctrl 8 KiB) | NZ=2048 → **16.01 MiB** (dos buffers de 8.00 MiB + ctrl) | 10.5 y 16.0 MiB / 32 MiB |
| C. Tres nodos | 3 nodos × 16 MiB | N=98 304 → **7.88 MiB**, 6.00 MiB de CSR | NZ=2049 → **16.02 MiB** | 48 MiB en total, ~1/3 por nodo de cómputo |
| D. Memoria amplia | 2 nodos × 64 MiB | N=524 288 → **42.01 MiB**, 32 MiB de CSR, 8.4 M aristas | NZ=8192 → **64.01 MiB**, 64 MiB de grilla | 128 MiB en total (50 % del clúster) |

El asignador global (`dsm.c:136-155`) reparte las reservas desde el slot del nodo que pide
primero; en las corridas siguientes casi todas las páginas de datos quedaron en el slot 0 y el
otro nodo las accedió remotamente. Eso es justamente lo que ejercita el DSM: en el escenario D se
midieron 17 185 páginas recibidas en n0 y 5 650 invalidaciones en n1 para PageRank.

## 7. Datos de prueba, estadísticas y rendimiento

Las cifras 7.1-7.3 son un **smoke test de desarrollo en una sola máquina** (WSL2 Ubuntu, x86-64,
kernel `6.18.33.2-microsoft-standard-WSL2`, gcc 15.2, `-O2 -pthread`, página DSM de 4096 B; logs en
`build/pr*_n*.log`, `build/heat*_n*.log`, `build/pagerank_n*.log` y `build/heat3d_n*.log`). Sirven
para validar la lógica, **no para la nota**: la evidencia de la sección 7.4 debe venir de dos
computadoras físicas y es la que se mostrará en el demo.

### 7.1 Escenario B (2 nodos, 16 MiB por nodo)

| Métrica (nodo 0 / nodo 1) | PageRank (12 iteraciones) | Heat-3D (8 pasos) |
|---|---|---|
| Fallos de lectura | 1 549 / 1 556 | 527 / 528 |
| Fallos de escritura | 2 772 / 2 766 | 3 088 / 3 086 |
| Páginas recibidas | 4 321 / 4 322 | 3 615 / 3 614 |
| Invalidaciones | 0 / 1 426 | 8 / 518 |
| Fetch atendidos | 7 094 / 1 549 | 6 172 / 1 057 |
| Páginas liberadas (`PUNCH_HOLE`) | 0 / 1 426 | 8 / 1 030 |
| Mensajes TX | 9 442 / 10 258 | 7 594 / 7 844 |
| Bytes TX | 23.7 MB / 15.7 MB | 20.3 MB / 10.9 MB |
| Tiempo total (ambos nodos) | 2.32 s | 1.83 s |
| Resultado | `suma=1.000000000`, `PASS` | `error=0.000000000000`, `PASS` |

En PageRank el nodo 0 acumula todas las páginas en M y el nodo 1 sufre las invalidaciones: es el
efecto esperado del modelo pull, donde el dueño de la copia modificada cambia en cada ronda.

### 7.2 Escenario C (3 nodos)

| Métrica | PageRank n0 / n1 / n2 | Heat-3D n0 / n1 / n2 |
|---|---|---|
| Fallos R/W | 1 556/1 408, 1 561/1 389, 1 561/1 390 | 475/2 296, 479/2 298, 471/2 289 |
| Páginas recibidas | 2 964, 2 950, 2 951 | 2 771, 2 777, 2 760 |
| Invalidaciones | 0, 1 432, 1 431 | 3, 470, 470 |
| Fetch atendidos | 7 303, 781, 781 | 4 387, 2 995, 926 |
| Resultado | `L1 final=0.002858`, `PASS` en 3 nodos | `error=0`, `PASS` en 3 nodos |

### 7.3 Escenario D (memoria amplia)

| Métrica | PageRank (N=524 288) | Heat-3D (NZ=8192) |
|---|---|---|
| Fallos R/W en n0 | 6 157 / 11 028 | 2 064 / 12 304 |
| Páginas recibidas n0/n1 | 17 185 / 17 186 | 14 368 / 14 367 |
| Invalidaciones n0/n1 | 0 / 5 650 | 2 051 / 11 |
| Fetch atendidos n0/n1 | 28 214 / 6 157 | 12 329 / 16 406 |
| Bytes TX n0/n1 | 93.5 MB / 60.6 MB | 59.6 MB / 63.8 MB |
| Tiempo total | 8.14 s | 6.22 s |
| Resultado | `suma=1.000000000`, `PASS` | `error=0`, `PASS` |

El patrón de invalidaciones cambia entre ejecuciones porque depende de qué nodo gana la carrera de
`dsm_alloc_named` y de qué página concreta se escribe primero; el resultado y los contadores de
páginas recibidas son estables.

### 7.4 Evidencia a capturar en el laboratorio (dos máquinas)

Cada app imprime ahora la IP configurada (`ip=...`) y sus propios contadores (`stats_local`) en
cada nodo, y el flag de resultado se comparte por DSM para que ambos logs digan lo mismo. Llene
esta tabla con los valores de los logs `build/lab_app_pagerank_n0.log`, `...n1.log`,
`build/lab_app_heat3d_n0.log` y `...n1.log` de las máquinas A y B:

| Dato a copiar del log | PageRank máquina A (n0) | PageRank máquina B (n1) | Heat-3D máquina A (n0) | Heat-3D máquina B (n1) |
|---|---|---|---|---|
| `ip=` | | | | |
| `memoria=` (MiB) | | | | |
| `stats_local: faultR/faultW` | | | | |
| `stats_local: rx / inv / fetch` | | | | |
| `stats_local: txMsg / txBytes` | | | | |
| Verificación (`suma=` / `error maximo=`) | | | | |
| `RESULTADO` y código de salida | | | | |
| Tiempo medido con `time` | | | | |

Las IP distintas en la columna A y B son la prueba directa de que el espacio DSM cruzó la red:
si un nodo lee una página cuyo dueño es el otro, `rx` crece en el lector y `fetch` en el dueño.

## 8. Ejecución en máquinas físicas y guion de laboratorio

### 8.1 Preparación (en las dos computadoras)

1. Copiar el proyecto a cada máquina y verificar `uname -m && uname -r` (x86-64, kernel ≥ 4.17).
   **Recompilar en cada máquina** con `make build/app_pagerank build/app_heat3d`; los binarios
   generados en WSL requieren GLIBC 2.38 y podrían no ejecutarse en el laboratorio.
2. Elegir las IPs reales y ejecutar una sola vez `./scripts/setup_lab.sh <ip_nodo0> <ip_nodo1>`.
   Ese script fija `dsm.conf` en 27800/27801 y `dsm.lab2.conf` en 27810/27811. Copiar **los dos
   archivos resultantes** a la otra máquina: deben ser byte-idénticos.
3. Verificar que no haya firewall que bloquee esos puertos TCP ni el UDP de heartbeat
   (puerto del nodo + 10).

### 8.2 Prueba 1: PageRank en dos máquinas distintas

```sh
# Máquina A
./scripts/run_lab_node.sh app_pagerank dsm.conf 0 131072 16 12
# Máquina B (iniciar dentro de los 30 s siguientes)
./scripts/run_lab_node.sh app_pagerank dsm.conf 1 131072 16 12
```

Cada máquina guarda `build/lab_app_pagerank_n<id>.log`. El log debe mostrar `ip=<IP de esa
máquina>`, `stats_local` con contadores propios y `RESULTADO: PASS` en **ambos** equipos.

### 8.3 Prueba 2: Heat-3D en dos máquinas distintas

```sh
# Máquina A
./scripts/run_lab_node.sh app_heat3d dsm.lab2.conf 0 2048 8
# Máquina B
./scripts/run_lab_node.sh app_heat3d dsm.lab2.conf 1 2048 8
```

Como usa otros puertos, esta prueba puede correrse **al mismo tiempo** que la de PageRank, en las
mismas dos máquinas y en terminales distintas. Verificar `error maximo=0.000000000000` y `PASS` en
los dos logs. Para más evidencia del directorio: `build/dsm_node` con `stats`, `global` y `html`.

### 8.4 Escenario de memoria amplia

Editar `mem_per_node = 64M` y `request_timeout_ms = 10000` en **ambos** configs y repetir:
`app_pagerank ... 524288 16 12` y `app_heat3d ... 8192 8`. Cada máquina aporta 64 MiB y las apps
ocupan 42.01 y 64.01 MiB respectivamente.

### 8.5 Smoke test en una sola máquina (sólo desarrollo, no es la prueba del profe)

```sh
make
./scripts/run_apps.sh     # 2 clústeres DSM en 127.0.0.1, 4 terminales lógicas
./scripts/run_3nodes.sh   # 3 nodos con dsm.local3.conf
```

No sustituye a 8.2-8.4: aquí el tráfico pasa por el loopback, no por la red del laboratorio.

### 8.6 Reparto sugerido para la defensa (1-2 h)

* **Persona A**: explicar paginación bajo demanda, doble mapeo, estados MSI, `SIGSEGV`, `FETCH`,
  invalidación y `PUNCH_HOLE`; mostrar los contadores de `rx`, `inv` y `fetch` de ambos nodos.
* **Persona B**: explicar `dsm_alloc_named`, barriers, locks, directorio por hash, heartbeat y
  ejecución de tareas; explicar el modelo pull de PageRank y el halo implícito del stencil.
* **Ambas**: ejecutar el cambio de tamaño en vivo (`PR_N`, `HEAT_NZ`), interpretar la tabla de
  memoria, explicar la verificación y responder sobre fallos y límites.

## 9. Problemas presentados y soluciones

1. **Normalización incorrecta del vector de teletransporte.** La primera versión usaba
   `pref(d) = w(d)/N`; la suma de `rank` convergía a `Σw/N = 0.9995087` en vez de 1 y la prueba
   fallaba (`suma=0.999725600 esperada=0.999926010`). Se normalizó por `Σw` y la suma quedó exacta
   (`1.000000000`) en los tres escenarios.
2. **Falso uso compartido en los límites de partición.** `rowptr` tiene entradas de 4 B y `rank` de
  8 B; si el corte entre nodos cae en la misma página, cada escritura invalida la del vecino. Se
  alineó `N` a `1024 * nodos` entradas y en heat se hizo que un plano mida exactamente una página.
3. **Contención en el modelo push.** La fórmula original de PageRank suma contribuciones sobre el
  destino (`p_next[d] += ...`), lo que serializa 2 M aristas sobre unas pocas páginas. Se cambió a
  CSR transpuesto + pull; cada nodo escribe sólo sus filas.
4. **Tiempos de espera con archivos de configuración mezclados.** `dsm.conf` trae IPs del
  laboratorio; usarlo en una sola máquina provoca `DSM_ERR_TIMEOUT` en `dsm_init`. Por eso se
  crearon `dsm.local.conf`, `dsm.local2.conf`, `dsm.local3.conf` y `dsm.local.big.conf`.
5. **RPC de estadísticas al apagar.** Pedir `dsm_stats_node` después de que el otro nodo llamó
  `dsm_shutdown` produce `stats no disponibles`. Las apps consultan las estadísticas antes del
  barrier final y sólo luego hacen shutdown.
6. **Límite de asignación.** `dsm_free` no reutiliza memoria y el asignador es monotónico; con
  16 MiB por nodo no caben más de ~32 MiB de datos. Para el escenario D se aumentó
  `mem_per_node = 64M` en ambos nodos y `request_timeout_ms = 10000`.
7. **Ruido de páginas en WSL vs red real.** En WSL el `fetch` atendido por el dueño de la página
  predomina sobre la invalidación; en máquinas físicas se espera más latencia por página y mayor
  probabilidad de `PAGE_REQUEST` reintentados. Si aparece `pagina=N home=nodo M`, revisar el
  heartbeat y los puertos UDP antes de culpar a la app.
8. **Leer memoria DSM después de `dsm_shutdown`.** Al propagar el resultado con un `failflag`
   compartido, la primera versión lo leía después de apagar el pager: el nodo 1 moría con
   `DSM_ERR_TIMEOUT: la pagina no llego ... pagina=4433 home=nodo 0` y perdía su salida por
   `_exit(70)`. Se movió la lectura antes del shutdown; ahora ambos nodos imprimen `PASS` y el
   mismo código de salida.

## 10. Limitaciones conocidas

* La verificación de PageRank usa un grafo regular sintético; es suficiente para estresar el DSM,
  pero no reproduce la distribución de grados de una web real.
* El stencil usa una solución lineal; comprueba exactitud, no convergencia numérica de un caso
  físico complejo.
* No hay tolerancia a fallos (Extra 2): si muere un nodo con páginas en M, el resultado se pierde.
* Las apps lanzan un hilo de aplicación por nodo; el paralelismo dentro del nodo queda para trabajo
  futuro.
