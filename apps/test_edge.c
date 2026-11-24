/* Casos limite del manejador de fallos.
 *  E1 un acceso fuera del espacio DSM (NULL) debe terminar en SIGSEGV normal, no colgarse ni ser "resuelto"
 *  E2 read() del kernel sobre un buffer DSM: con la pagina ya tocada debe funcionar; sin tocar es solo informativo (EFAULT es esperable)
 */
#include "test_util.h"
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    t_init(argc, argv);
    const size_t ps = config_get()->page_size;

    /* E1 */
    pid_t pid = fork();
    if (pid == 0) { alarm(5); *(volatile int *)0 = 1; _exit(0); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "E1 el hijo no murio con SIGSEGV (status=%d)", st);
    END_PHASE("E1 NULL => SIGSEGV normal");

    /* E2 */
    char *buf = dsm_alloc_named("ed_buf", 2 * ps);
    if (!buf) { fprintf(stderr, "dsm_alloc fallo\n"); return 1; }
    char msg[128]; memset(msg, 'x', sizeof msg);
    int fd[2];
    if (pipe(fd) != 0) { perror("pipe"); return 1; }
    fcntl(fd[0], F_SETFL, O_NONBLOCK); fcntl(fd[1], F_SETFL, O_NONBLOCK);   /* el drenaje de abajo nunca debe bloquearse */

    if (write(fd[1], msg, sizeof msg) != (ssize_t)sizeof msg) CHECK(0, "E2 write al pipe");
    ssize_t r = read(fd[0], buf + ps, sizeof msg);             /* pagina posiblemente no residente */
    printf("[n%u] E2 (informativo) read() sobre pagina sin tocar: %zd%s\n", g_me, r, r < 0 ? (errno == EFAULT ? " (EFAULT)" : " (otro error)") : "");
    if (r < 0) { /* limpiar lo que quedo en el pipe antes de seguir */
        char tmp[128]; (void)!read(fd[0], tmp, sizeof tmp); }

    buf[0] = 1;                                                /* tocar la pagina => residente en M */
    if (write(fd[1], msg, sizeof msg) != (ssize_t)sizeof msg) CHECK(0, "E2 write al pipe (2)");
    r = read(fd[0], buf, sizeof msg);
    CHECK(r == (ssize_t)sizeof msg, "E2 read() sobre pagina ya tocada devolvio %zd", r);
    CHECK(memcmp(buf, msg, sizeof msg) == 0, "E2 contenido leido distinto");
    close(fd[0]); close(fd[1]);
    END_PHASE("E2 syscalls sobre buffer DSM");
    return t_done();
}
