# Siguientes pasos después del Avance 3 (Demo, 27 de nov)

## 1. Primero (en el laboratorio)

| Verificación | Comando |
|---|---|
| Arquitectura, kernel, glibc | `uname -m && uname -r && ldd --version` |
| Compilar | `make` |
| Batería completa | `make test` |
| Prueba entre máquinas | Ajustar IP en `dsm.conf`, copiar el mismo archivo y lanzar `DSM_NODE_ID=<n> ./build/test_stress dsm.conf` en cada una |
| Multicore | Repetir `test_stress` con más iteraciones y 3 o 4 nodos varios minutos |

## 2. Medir para el informe

| Variable | Valores |
|---|---|
| `worker_pool_size` | 1, 4, 8, 16 |
| `page_size` | 4096, 8192, 16384 |
| Nodos | 2, 4, 8, 16 |
| Métricas | Tiempo, fallos R/W, invalidaciones, reintentos (`DSM_VERBOSE=1` o `stats`) |

## 3. Si hay tiempo (por prioridad)

1. Si en multicore aparece livelock: subir `min_hold_us` o añadir retroceso exponencial.
2. Si el spinlock del pager aparece como cuello de botella: más franjas.
3. Mejora S→M sin reenviar datos.
4. Reutilizar memoria en `dsm_free`.
5. Extra 1 (ya implementado, ver `EXTRA1.md`): medir su overhead y aceleración en el laboratorio y verificar los requisitos contra el enunciado. Extra 2: sigue sin implementarse.

## 4. Entregables restantes

- Segunda aplicación afinada y documentada (ya existen `app_matmul` y `app_counter`).
- Informe: MSI, memoria, handler, concurrencia (A); protocolo, red, administración, manual de usuario (B); rendimiento y problemas encontrados (ambas).
- Ensayo de defensa: cada una explica el módulo de la otra. Preguntas clave: pools separados y por qué, generación de transacciones, `INVALIDATE` en PENDING, `PUNCH_HOLE` y la copia de referencia, write-back al apagar.