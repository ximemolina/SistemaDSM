# Pasos realizados para validar el DSM (23-nov-2026)

Registro cronológico de la sesión de pruebas de Susana Feng con apoyo de una IA. Complementa a `CONTEXTO_IA.md` (§14 y Apéndice A). Cada paso indica qué se hizo, qué salió y qué se concluyó. Lo que **no** se ejecutó o no tiene salida pegada se marca como tal.

## Resumen

| Fase | Qué se probó | Resultado |
|---|---|---|
| 0 | Entorno y compilación (WSL2) | OK: x86_64, kernel 6.6, glibc 2.39, 12 núcleos; compila sin errores |
| 1 | E01: 2 nodos en una máquina | PASS, `n0=0 n1=0` |
| 2 | Revisión del estado real del repo | Documento desactualizado (puertos, `lib/`, offset UDP) |
| 3 | Preparación de red entre 2 equipos | TCP reportado OK; UDP **no verificado** |
| 4 | Tropiezos en el equipo del laboratorio | Dos errores resueltos (binario sin permiso, config ausente) |
| 5 | E13: `test_basic` en 2 computadoras | PASS en ambas, `exit=0` |
| 6 | Verificación de que eran equipos distintos | Confirmado por IPs y configuración |

---

## Fase 0. Entorno y compilación

En el portátil de Susana (WSL2):

```sh
uname -m && uname -r && ldd --version | head -1 && nproc
make build/dsm_node build/print_sizes build/test_basic build/test_task build/app_tasks
```

Salida relevante: `x86_64`, `6.6.87.2-microsoft-standard-WSL2`, `ldd (Ubuntu GLIBC 2.39-0ubuntu8.12) 2.39`, `12`.

Observaciones:
- Cumple los requisitos del §12 (x86-64, kernel ≥ 4.17).
- El `make` compiló también `lib/libdsm_math.c` y generó `build/libdsm_math.a`, que el documento original no mencionaba.
- Hay 12 núcleos, no 1 como decía `README.md:165`.

## Fase 1. E01: 2 nodos en una sola máquina

```sh
DSM_VERBOSE=1 ./scripts/run_local.sh dsm.conf
```

Resultado: `Test A (ping-pong x100): OK`, `Test D: OK`, `Test B: OK`, `RESULTADO: PASS` en los dos nodos, `codigos de salida: n0=0 n1=0`, `reintentos=0`.

| Nodo | Fallos R/W | Páginas recibidas | Invalidaciones | Fetch | Punch |
|---|---|---|---|---|---|
| n0 | 54 / 103 | 155 | 1 | 205 | 1 |
| n1 | 53 / 103 | 154 | 51 | 104 | 50 |

Conclusión: E01 confirmada. El asimetrismo de contadores entre nodos es esperable (los homes de las páginas se reparten por hash).

## Fase 2. Revisión del estado real del repo

Se pidió `ls apps scripts lib` y `git log --oneline | head`, y luego `cat dsm.conf` y `git show --stat` de tres commits.

Hallazgos:
- `apps/` y `scripts/` siguen como describe el documento (3 apps y `run_local.sh`).
- Existe `lib/libdsm_math.c/.h` (commit `1904632`, "Eliminar bibliotecas externas").
- El commit `abb097d` (contexto del proyecto) es **anterior** a tres cambios: `56b7409` (puertos y más nodos), `1904632` (librería) y `7ef3eae` (direcciones IP).
- `dsm.conf` ahora usa puertos **27800/27801** y `heartbeat_port_offset = 10`, por lo que el UDP es 27810/27811 (el documento decía 9101/9102 y `+1000`).
- `dsm.conf` tiene activas las líneas `127.0.0.1` y comentadas las del laboratorio (`172.19.12.81` y `172.19.12.48`).

Conclusión: se corrigió `CONTEXTO_IA.md` en puertos, offset UDP, `lib/`, `Makefile` y número de núcleos.

## Fase 3. Preparación de la red entre dos equipos

Se descartó una hipótesis inicial de la IA (que las IPs `172.19.x.x` fueran de WSL2 con NAT), porque Susana indicó que las sacó directamente de los dispositivos. Después `ip` mostró que son las IPs de `eno1` de los equipos del laboratorio.

Prueba de conectividad propuesta:

```sh
# Equipo nodo 1
python3 -m http.server 27801 --bind 0.0.0.0
# Equipo nodo 0
ping -c 3 172.19.12.48
curl -m 5 http://172.19.12.48:27801 | head -3
```

Resultado: Susana respondió "sí funciona". **No se pegaron las salidas**, así que queda como reportado, no capturado.

**Pendiente:** la prueba de UDP (`nc -u`) se propuso, pero no se registró su resultado. Eso importa porque el heartbeat usa UDP y `curl` solo prueba TCP.

## Fase 4. Tropiezos en el equipo del laboratorio

### 4.1 `Permiso denegado`

```text
curso@soporte:~/Descargas/SistemaDSM-main$ ls -l build/test_basic dsm_lab.conf
ls: no se puede acceder a 'dsm_lab.conf': No existe el archivo o el directorio
-rw-rw-r-- 1 curso curso 282016 nov 23 14:13 build/test_basic
...
bash: ./build/test_basic: Permiso denegado
exit=126
```

- Causa: el binario no tenía bit de ejecución (`-rw-rw-r--`). La carpeta `SistemaDSM-main` parece un zip de GitHub y probablemente traía un `build/` ajeno (hipótesis, no verificada).
- Arreglo indicado: `rm -rf build` y recompilar con `make build/dsm_node build/test_basic build/app_tasks`. No se pegó la salida de ese `make`, pero el binario ejecutó después.

### 4.2 `config_load: No such file or directory`

```text
DSM_VERBOSE=1 DSM_NODE_ID=1 ./build/test_basic dsm_lab.conf
config_load: No such file or directory
dsm_init fallo
exit=1
```

- Causa: `dsm_lab.conf` se había creado solo en la otra máquina.
- Arreglo: crear el archivo en cada equipo, o usar `dsm.conf` (que ya traía las IPs del laboratorio activas). Se usó `dsm.conf`.

### 4.3 Sobre el archivo `dsm_lab.conf`

Susana preguntó por qué no usar `dsm.conf` directamente. Respuesta: no hay razón técnica. El archivo aparte solo evitaba modificar un archivo versionado y mantenía el caso local (E01) intacto. Lo único que importa es que las dos máquinas carguen configuración equivalente (`grep -vE '^\s*(#|$)' dsm.conf | md5sum`).

## Fase 5. E13: `test_basic` en dos computadoras

Se lanzó un proceso por equipo, con pocos segundos de diferencia, usando `dsm.conf` con las IPs reales:

```sh
DSM_VERBOSE=1 DSM_NODE_ID=<id> ./build/test_basic dsm.conf 2>&1 | tee build/lab_n<id>.log; echo "exit=${PIPESTATUS[0]}"
```

**Nodo 1:**
```text
[dsm n1] nodo 0 ahora VIVO
[dsm n1] fallos R=66 W=103 | paginas recibidas=167 | invalidaciones=38 | fetch=104 (retenidos 59) | punch=63 | reintentos=0
[n1] Test A (ping-pong x100): OK
[n1] Test D (pagina del contribuyente): OK
[n1] Test B (estres, 2 hilos/nodo, misma pagina): OK
[n1] RESULTADO: PASS
exit=0
```

**Nodo 0:**
```text
[dsm n0] nodo 1 ahora VIVO
[dsm n0] fallos R=41 W=103 | paginas recibidas=142 | invalidaciones=1 | fetch=205 (retenidos 38) | punch=1 | reintentos=0
[n0] Test A (ping-pong x100): OK
[n0] Test D (pagina del contribuyente): OK
[n0] Test B (estres, 2 hilos/nodo, misma pagina): OK
[n0] RESULTADO: PASS
exit=0
```

## Fase 6. ¿Eran realmente dos computadoras?

Los dos logs tenían el mismo prompt (`curso@soporte`) y contadores casi iguales a los de E01 en local, lo que generó una duda razonable. Se verificó con:

```sh
hostname
ip -4 addr show | grep inet
grep -vE '^\s*(#|$)' dsm.conf
```

| | Equipo A | Equipo B |
|---|---|---|
| hostname | `soporte` | `soporte` |
| IP en `eno1` | `172.19.12.81/23` | `172.19.12.48/23` |
| Configuración activa | idéntica | idéntica |

Conclusión: son dos equipos distintos (clonados, de ahí el mismo hostname), cada uno con la IP de su línea `node=`, en la misma subred. Como `172.19.12.48` solo existe en un equipo, el `JOIN` del nodo 0 tuvo que cruzar la red.

Corrección de la sesión: en un momento se afirmó que "el tráfico TCP y UDP cruzó la red". Eso sobrepasaba la evidencia: `nodo X ahora VIVO` puede venir del `JOIN` por TCP, y el UDP no se probó de forma explícita.

## Qué quedó verificado y qué no

**Verificado:**
- Compilación y ejecución en WSL2 (12 núcleos) y en dos equipos Linux nativos.
- E01 (2 nodos, una máquina): PASS.
- E13 (2 computadoras, misma subred, `test_basic`, una corrida): PASS en ambas.

**No verificado:**
- UDP/heartbeat entre máquinas de forma explícita.
- 3 o más nodos (`test_task` necesita 3).
- Repetibilidad: una sola corrida buena no descarta fallos intermitentes (el §7 recomienda 15–30).
- `app_tasks` entre máquinas.
- Caída real de un nodo entre máquinas (E10/E11).
- Nombres de host (solo se usaron IPs), WSL2 entre máquinas, 16 nodos.
- Casos E02–E12: se propusieron comandos para E02, E04–E07, pero **no hay salidas registradas**, por lo que siguen pendientes.

## Siguientes pasos sugeridos (en orden)

1. Probar UDP: `nc -u -l 27811` en un equipo y `echo hola | nc -u -w1 172.19.12.48 27811` en el otro.
2. `app_tasks` entre los dos equipos:
   ```sh
   DSM_VERBOSE=1 DSM_NODE_ID=<id> ./build/app_tasks dsm.conf 96 2>&1 | tee build/lab_tasks_n<id>.log; echo "exit=${PIPESTATUS[0]}"
   ```
   Esperado: `app_tasks N=96 ... PASS` en el nodo 0 y `exit=0` en ambos.
3. Repetir `test_basic` 10 veces en cada equipo:
   ```sh
   for i in $(seq 1 10); do
     DSM_NODE_ID=<id> ./build/test_basic dsm.conf > build/rep_$i.log 2>&1
     echo "iter $i exit=$?"; sleep 3
   done
   ```
4. En el portátil (una sola máquina): E02, E04–E07 (rápidos) y E12 (12 núcleos con `worker_pool_size` 1, 4 y 8).
5. Si hay una tercera máquina: `test_task` (E08).
6. E10: `kill -9` del nodo 1 en pleno `test_basic` y capturar los dos logs.
7. Antes de la demo (27 de noviembre): si se usará WSL2, resolver la red (modo *mirrored* o reenvío de puertos); si se usará Linux nativo como en el laboratorio, no aplica.


``` ################################################################################################### ```
``` PRUEBA CON EL SCRIPT lab.sh ```

# Análisis del script `lab.sh`

Este script es una herramienta de pruebas y diagnóstico para el proyecto distribuido. Su función principal es leer la configuración desde `dsm.conf`, identificar qué nodo es la máquina actual y ejecutar distintos tests de conectividad y funcionamiento.

---

## Funcionamiento general

Al iniciar, el script:

1. Se mueve al directorio donde se encuentra el script.
2. Crea el directorio `build/` si no existe.
3. Busca el archivo `dsm.conf`.
4. Lee la configuración de los nodos (`node=`).
5. Obtiene las IPs de la máquina local.
6. Compara las IPs locales con las configuradas para determinar cuál nodo corresponde a esta máquina.

Por ejemplo:

```ini
node=1 192.168.1.10 5000
node=2 192.168.1.11 5000
```

Si la máquina tiene la IP `192.168.1.10`, el script determinará que corresponde al nodo 1.

---

# Modo `info`

```bash
bash lab.sh info
```

Muestra información de diagnóstico del entorno:

- Fecha actual.
- Nombre del host.
- Arquitectura del procesador.
- Versión del kernel.
- Número de CPUs.
- Último commit de Git.
- Hash MD5 del código fuente.
- Hash MD5 del archivo de configuración.
- Direcciones IP locales.
- Identificador del nodo detectado.
- Existencia de:
  - `build/test_basic`
  - `build/app_tasks`
- Estado del firewall (`ufw`) si está instalado.
- Prueba de conectividad mediante `ping` a los demás nodos.

**Uso recomendado:** verificar que el entorno esté correctamente configurado antes de realizar pruebas.

---

# Modo `udp-recv`

```bash
bash lab.sh udp-recv
```

Abre un socket UDP y escucha durante 30 segundos en el puerto:

```text
puerto_configurado + heartbeat_port_offset
```

Por cada mensaje recibido muestra:

```text
RECIBIDO de 192.168.1.20 : hola desde nodo 2 #1
```

Al finalizar indica cuántos mensajes recibió.

**Uso recomendado:** comprobar que otros nodos pueden enviar paquetes UDP correctamente.

---

# Modo `udp-send`

```bash
bash lab.sh udp-send
```

Envía mensajes UDP a todos los nodos definidos en `dsm.conf`, excepto al nodo local.

Envía cinco mensajes por cada nodo:

```text
hola desde nodo X #1
hola desde nodo X #2
hola desde nodo X #3
hola desde nodo X #4
hola desde nodo X #5
```

con una pausa de un segundo entre cada envío.

## Ejemplo de uso

En una máquina:

```bash
bash lab.sh udp-recv
```

En otra máquina:

```bash
bash lab.sh udp-send
```

Así puede verificarse la comunicación UDP entre ambos equipos.

---

# Modo `tasks`

```bash
bash lab.sh tasks
```

Comprueba que exista el ejecutable:

```text
build/app_tasks
```

y luego lo ejecuta de la forma:

```bash
./build/app_tasks "$CONF" 96
```

Además:

- Define variables de entorno:
  - `DSM_VERBOSE=1`
  - `DSM_NODE_ID=<id>`
- Limita la ejecución a 180 segundos.
- Guarda la salida en:

```text
build/lab_tasks_n<ID>.log
```

- Muestra:
  - Duración de la ejecución.
  - Código de salida del programa.

**Uso recomendado:** ejecutar y verificar el comportamiento principal de la aplicación distribuida.

---

# Modo `rep`

```bash
bash lab.sh rep
```

o

```bash
bash lab.sh rep 20
```

Ejecuta repetidamente el programa:

```text
build/test_basic
```

Por defecto realiza:

```text
10 iteraciones
```

En cada iteración:

1. Ejecuta `test_basic`.
2. Guarda el log generado.
3. Busca el resultado reportado.
4. Extrae la cantidad de reintentos.
5. Genera un resumen de la ejecución.

Los archivos generados quedan en:

```text
build/rep_n<ID>_1.log
build/rep_n<ID>_2.log
...
```

y el resumen final en:

```text
build/rep_resumen_n<ID>.txt
```

**Uso recomendado:** detectar errores intermitentes, condiciones de carrera o problemas de sincronización que no aparecen en una sola ejecución.

---

# Resumen rápido

| Comando | Función |
|----------|----------|
| `bash lab.sh info` | Verificar configuración y conectividad |
| `bash lab.sh udp-recv` | Escuchar paquetes UDP |
| `bash lab.sh udp-send` | Enviar paquetes UDP a otros nodos |
| `bash lab.sh tasks` | Ejecutar la aplicación `app_tasks` |
| `bash lab.sh rep 10` | Ejecutar `test_basic` varias veces y recopilar estadísticas |

---

# Conclusión

El script `lab.sh` sirve como herramienta de apoyo para validar el entorno del laboratorio, comprobar la comunicación entre nodos, ejecutar la aplicación principal y automatizar pruebas repetitivas. Es útil tanto para detectar errores de configuración como para verificar el correcto funcionamiento del sistema distribuido.