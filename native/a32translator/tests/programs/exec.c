/* execve of a 32-bit ARM program (a32run re-executes itself), argv/envp passing, /proc/self/exe, pipes + wait. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {
        printf("child argc %d arg2 %s env %s\n", argc, argc > 2 ? argv[2] : "-", getenv("A32_TEST_VAR"));
        return 7;
    }
    if (argc > 1 && !strcmp(argv[1], "exec-self")) {
        char *args[] = {argv[0], "child", "via-proc-self-exe", NULL};
        char *env[] = {"A32_TEST_VAR=from-exec", NULL};
        execve("/proc/self/exe", args, env);
        perror("execve");
        return 1;
    }
    int p[2];
    pipe(p);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        char *args[] = {argv[0], "exec-self", NULL};
        execv(argv[0], args);
        _exit(99);
    }
    close(p[1]);
    char buf[256] = {0};
    ssize_t n = read(p[0], buf, sizeof buf - 1);
    int st;
    waitpid(pid, &st, 0);
    printf("read %d bytes: %s", (int)n, buf);
    printf("exit %d\n", WEXITSTATUS(st));
    return 0;
}
