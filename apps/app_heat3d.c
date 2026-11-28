
#include "../src/internal.h"
#include "../lib/libdsm_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define NY 8

#define BAR_INIT 10
#define BAR_CALC 100
#define BAR_LOCK 200
#define BAR_VERIFY 300
#define BAR_DONE 500

/*
 * Temperatura base del volumen.
 * Se mantiene una distribucion lineal para los limites.
 */
static double analytic(int x, int y, uint64_t z) {
    return 20.0 + 0.01 * (double)x
                + 0.02 * (double)y
                + 0.03 * (double)z;
}

/*
 * Visualiza un corte XY del volumen 3D.
 *
 * Los colores representan cuanto ha aumentado
 * la temperatura respecto a la temperatura inicial.
 *
 * Azul      = Sin cambios significativos
 * Cyan      = Aumento pequeno
 * Verde     = Aumento moderado
 * Amarillo  = Aumento considerable
 * Rojo      = Zona muy caliente
 */
static void print_heatmap(double *cur,
                          uint64_t nx,
                          uint64_t nz,
                          uint64_t plane_elems,
                          uint32_t step,
                          uint32_t steps) {

    uint64_t z = nz / 2;

    printf("\n");
    printf("\033[1;36m");
    printf("========== HEAT3D | PASO %u/%u ==========\n",
           step, steps);
    printf("\033[0m");

    printf("Corte central XY | Z=%llu\n\n",
           (unsigned long long)z);

    printf("      ");

    for (uint64_t x = 0; x < nx; x++) {
        if (x % 8 == 0)
            printf("%-2llu ", (unsigned long long)x);
    }

    printf("\n");

    for (uint64_t y = 0; y < NY; y++) {

        printf("Y=%llu  ", (unsigned long long)y);

        for (uint64_t x = 0; x < nx; x++) {

            uint64_t i = z * plane_elems + y * nx + x;

            double temperature = cur[i];

            /*
             * Calcula cuanto cambio la temperatura
             * respecto a su valor inicial.
             */
            double increase =
                temperature - analytic((int)x, (int)y, z);

            const char *color;

            if (increase >= 40.0)
                color = "\033[1;31m";  // Rojo

            else if (increase >= 15.0)
                color = "\033[1;33m";  // Amarillo

            else if (increase >= 5.0)
                color = "\033[1;32m";  // Verde

            else if (increase >= 1.0)
                color = "\033[1;36m";  // Cyan

            else
                color = "\033[1;34m";  // Azul

            printf("%s##\033[0m", color);
        }

        printf("\n");
    }

    printf("\n");
    printf("Leyenda: ");

    printf("\033[1;34m Azul \033[0m ");
    printf("\033[1;36m Cyan \033[0m ");
    printf("\033[1;32m Verde \033[0m ");
    printf("\033[1;33m Amarillo \033[0m ");
    printf("\033[1;31m Rojo \033[0m ");

    printf("\n");

    printf("==========================================\n");

    fflush(stdout);
}

/*
 * Estadisticas del sistema DSM.
 */
static void print_stats(uint32_t me,
                        uint32_t nn,
                        const char *ip) {

    dsm_stats_t local;

    if (dsm_stats(&local) == DSM_OK)

        printf("[n%u ip=%s] stats_local: "
               "I=%llu S=%llu M=%llu "
               "faultR=%llu faultW=%llu "
               "rx=%llu inv=%llu fetch=%llu "
               "punch=%llu txMsg=%llu txBytes=%llu\n",

               me, ip,

               (unsigned long long)local.v[ST_PAGES_I],
               (unsigned long long)local.v[ST_PAGES_S],
               (unsigned long long)local.v[ST_PAGES_M],
               (unsigned long long)local.v[ST_FAULT_R],
               (unsigned long long)local.v[ST_FAULT_W],
               (unsigned long long)local.v[ST_PAGES_RX],
               (unsigned long long)local.v[ST_INVALIDATIONS],
               (unsigned long long)local.v[ST_FETCHES],
               (unsigned long long)local.v[ST_PUNCHED],
               (unsigned long long)local.v[ST_TX_MSGS],
               (unsigned long long)local.v[ST_TX_BYTES]);

    if (me != 0)
        return;

    for (uint32_t n = 0; n < nn; n++) {

        dsm_stats_t s;

        if (dsm_stats_node(n, &s) != DSM_OK) {

            printf("[n0] stats n%u no disponibles\n", n);
            continue;
        }

        printf("[n0] stats n%u: "
               "I=%llu S=%llu M=%llu "
               "faultR=%llu faultW=%llu "
               "rx=%llu inv=%llu fetch=%llu "
               "punch=%llu txMsg=%llu txBytes=%llu\n",

               n,

               (unsigned long long)s.v[ST_PAGES_I],
               (unsigned long long)s.v[ST_PAGES_S],
               (unsigned long long)s.v[ST_PAGES_M],
               (unsigned long long)s.v[ST_FAULT_R],
               (unsigned long long)s.v[ST_FAULT_W],
               (unsigned long long)s.v[ST_PAGES_RX],
               (unsigned long long)s.v[ST_INVALIDATIONS],
               (unsigned long long)s.v[ST_FETCHES],
               (unsigned long long)s.v[ST_PUNCHED],
               (unsigned long long)s.v[ST_TX_MSGS],
               (unsigned long long)s.v[ST_TX_BYTES]);
    }
}

int main(int argc, char **argv) {

    if (argc < 2) {

        fprintf(stderr,
                "uso: %s dsm.conf [NZ] [STEPS]\n",
                argv[0]);

        return 1;
    }

    uint64_t nz = argc > 2
        ? strtoull(argv[2], NULL, 10)
        : 2048;

    uint32_t steps = argc > 3
        ? (uint32_t)strtoul(argv[3], NULL, 10)
        : 8;

    /*
     * Inicializacion DSM.
     */
    if (dsm_init(argv[1]) != DSM_OK) {

        fprintf(stderr, "dsm_init fallo\n");
        return 1;
    }

    const dsm_config_t *c = config_get();

    uint32_t nn = c->max_nodes;
    uint32_t me = cluster_self_id();

    node_info_t self_info;

    cluster_get_node(me, &self_info);

    const char *my_ip = self_info.ip;

    /*
     * Dimensiones del volumen.
     *
     * Cada plano XY ocupa exactamente una pagina DSM.
     */
    uint64_t plane_elems =
        c->page_size / sizeof(double);

    uint64_t nx = plane_elems / NY;

    if (nx <= 2 || nz <= nn + 2 || steps == 0) {

        fprintf(stderr, "parametros invalidos\n");
        return 1;
    }

    if (nx * NY * sizeof(double) != c->page_size) {

        fprintf(stderr,
                "page_size no multiple de %zu\n",
                sizeof(double) * NY);

        return 1;
    }

    /*
     * Reserva de memoria compartida.
     */
    size_t buf_bytes =
        (size_t)(nz * c->page_size);

    double *cur = (double *)dsm_alloc_named(
        "heat_cur", buf_bytes);

    double *nxt = (double *)dsm_alloc_named(
        "heat_nxt", buf_bytes);

    double *ctrl = (double *)dsm_alloc_named(
        "heat_ctrl", (size_t)c->page_size * 2);

    if (!cur || !nxt || !ctrl) {

        fprintf(stderr, "dsm_alloc fallo\n");
        return 1;
    }

    /*
     * Variables de control compartidas.
     */
    volatile double *checksum = ctrl;
    volatile double *maxerr = ctrl + 1;
    volatile double *failflag = ctrl + 2;

    /*
     * Distribucion de capas Z entre los computadores.
     */
    uint64_t chunk =
        (nz - 2 + nn - 1) / nn;

    uint64_t z0 =
        1 + (uint64_t)me * chunk;

    uint64_t z1 = z0 + chunk;

    if (z1 > nz - 1)
        z1 = nz - 1;

    /*
     * Inicializacion del volumen.
     *
     * Cada computador inicializa sus paginas.
     */
    uint64_t hot_x = nx / 2;
    uint64_t hot_y = NY / 2;
    uint64_t hot_z = nz / 2;

    for (uint64_t z = me; z < nz; z += nn) {

        uint64_t base = z * plane_elems;

        for (uint64_t y = 0; y < NY; y++) {

            for (uint64_t x = 0; x < nx; x++) {

                double v = analytic(
                    (int)x, (int)y, z);

                /*
                 * Punto caliente inicial en el centro.
                 */
                if (x == hot_x &&
                    y == hot_y &&
                    z == hot_z) {

                    v += 100.0;
                }

                cur[base + y * nx + x] = v;
                nxt[base + y * nx + x] = v;
            }
        }
    }

    /*
     * Inicializacion de variables compartidas.
     */
    if (me == 0) {

        *checksum = 0.0;
        *maxerr = 0.0;
        *failflag = 0.0;
    }

    if (dsm_barrier(BAR_INIT, nn) != DSM_OK)
        return 2;

    /*
     * Informacion inicial.
     */
    printf("[n%u ip=%s] app_heat3d "
           "NX=%llu NY=%u NZ=%llu "
           "steps=%u nodos=%u memoria=%.2f MiB\n",

           me,
           my_ip,
           (unsigned long long)nx,
           NY,
           (unsigned long long)nz,
           steps,
           nn,

           (double)(2 * buf_bytes +
                    2 * c->page_size) / 1048576.0);

    /*
     * Mostrar el estado inicial.
     */
    if (me == 0) {

        print_heatmap(
            cur, nx, nz,
            plane_elems, 0, steps);
    }

    /*
     * SIMULACION DE DIFUSION DE CALOR.
     */
    for (uint32_t s = 1; s <= steps; s++) {

        if (me == 0)
            *checksum = 0.0;

        /*
         * Todos los nodos esperan antes de calcular.
         */
        if (dsm_barrier(BAR_CALC + s, nn) != DSM_OK)
            return 2;

        double local_sum = 0.0;

        /*
         * Cada computador procesa sus propias capas.
         */
        for (uint64_t z = z0; z < z1; z++) {

            uint64_t base = z * plane_elems;

            for (uint64_t y = 1; y < NY - 1; y++) {

                for (uint64_t x = 1; x < nx - 1; x++) {

                    uint64_t i =
                        base + y * nx + x;

                    /*
                     * Ecuacion de difusion 3D.
                     *
                     * Utiliza los seis vecinos:
                     * izquierda, derecha,
                     * arriba, abajo,
                     * plano anterior, plano siguiente.
                     */
                    double v = cur[i] + 0.125 * (

                        cur[i - 1] +
                        cur[i + 1] +

                        cur[i - nx] +
                        cur[i + nx] +

                        cur[i - plane_elems] +
                        cur[i + plane_elems] -

                        6.0 * cur[i]
                    );

                    nxt[i] = v;

                    local_sum += v;
                }
            }
        }

        /*
         * Acumulacion del checksum global.
         */
        if (dsm_lock(7) != DSM_OK)
            return 3;

        *checksum += local_sum;

        if (dsm_unlock(7) != DSM_OK)
            return 3;

        if (dsm_barrier(BAR_LOCK + s, nn) != DSM_OK)
            return 2;

        if (me == 0) {

            printf("\n[n0] step=%u suma=%.6f\n",
                   s, *checksum);
        }

        /*
         * Intercambio de matrices.
         *
         * nxt se convierte en cur.
         */
        double *tmp = cur;
        cur = nxt;
        nxt = tmp;

        /*
         * Visualizacion desde el nodo 0.
         *
         * Los resultados ya fueron calculados
         * y sincronizados por todos los nodos.
         */
        if (me == 0) {

            print_heatmap(
                cur, nx, nz,
                plane_elems, s, steps);
        }
    }

    /*
     * VERIFICACION DE RESULTADOS.
     */
    double local_max_excess = 0.0;
    int local_bad = 0;

    for (uint64_t z = z0; z < z1; z++) {

        uint64_t base = z * plane_elems;

        for (uint64_t y = 0; y < NY; y++) {

            for (uint64_t x = 0; x < nx; x++) {

                uint64_t i =
                    base + y * nx + x;

                double expected =
                    analytic((int)x, (int)y, z);

                double value = cur[i];

                /*
                 * Verificar que los valores sean validos.
                 */
                if (value != value ||
                    value > 1e300 ||
                    value < -1e300) {

                    local_bad = 1;
                    continue;
                }

                /*
                 * Verificar que los limites X e Y
                 * permanezcan sin modificaciones.
                 */
                if (x == 0 || x == nx - 1 ||
                    y == 0 || y == NY - 1) {

                    if (dsm_fabs(value - expected) > 1e-9)
                        local_bad = 1;
                }

                /*
                 * Medir el aumento de temperatura.
                 */
                double excess = value - expected;

                if (excess > local_max_excess)
                    local_max_excess = excess;
            }
        }
    }

    /*
     * Actualizar resultados globales.
     */
    if (dsm_lock(8) != DSM_OK)
        return 3;

    if (local_max_excess > *maxerr)
        *maxerr = local_max_excess;

    if (local_bad)
        *failflag = 1.0;

    if (dsm_unlock(8) != DSM_OK)
        return 3;

    if (dsm_barrier(BAR_VERIFY, nn) != DSM_OK)
        return 2;

    /*
     * Validacion final en nodo 0.
     */
    if (me == 0) {

        double excess = *maxerr;

        if (excess <= 1e-9)
            *failflag = 1.0;

        printf("\n");
        printf("========== VERIFICACION ==========\n");

        printf("Aumento maximo de temperatura: %.9f\n",
               excess);

        printf("Fuente de calor: (%llu, %llu, %llu)\n",
               (unsigned long long)hot_x,
               (unsigned long long)hot_y,
               (unsigned long long)hot_z);

        printf("Limites: %s\n",
               *failflag == 0.0 ? "OK" : "FALLO");

        printf("Simulacion: %s\n",
               *failflag == 0.0
                   ? "CORRECTA"
                   : "FALLO");

        printf("==================================\n");
    }

    /*
     * Estadisticas del sistema DSM.
     */
    print_stats(me, nn, my_ip);

    if (dsm_barrier(BAR_DONE, nn) != DSM_OK)
        return 2;

    int bad = *failflag != 0.0;

    dsm_shutdown();

    printf("[n%u ip=%s] RESULTADO: %s\n",
           me, my_ip,
           bad ? "FALLO" : "PASS");

    return bad;
}

