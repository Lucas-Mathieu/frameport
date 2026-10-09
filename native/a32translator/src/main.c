/* a32run: runs 32-bit ARM (AArch32) Linux programs on an AArch64 host.
 *
 *   a32run [options] [--] program [args...]
 *     --interp          interpreter only (no JIT)
 *     --strace          log guest syscalls to stderr
 *     --trace           log every executed instruction (interpreter) / block (JIT)
 *     --sysroot DIR     prefix for the program interpreter and absolute guest paths (like qemu -L)
 *     --argv0 NAME      argv[0] passed to the program
 *     -v                verbose diagnostics */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"
#include "signals.h"
#include "threads.h"

extern char **environ;
extern const char *g_sysroot;

static void usage(void)
{
    fprintf(stderr, "usage: a32run [--interp] [--strace] [--trace] [--sysroot DIR] [--argv0 NAME] [-v] [--] "
                    "program [args...]\n");
    exit(127);
}

int main(int argc, char **argv)
{
    const char *argv0 = NULL;
    int k = 1;
    for (; k < argc; k++) {
        const char *a = argv[k];
        if (!strcmp(a, "--")) {
            k++;
            break;
        }
        if (a[0] != '-') break;
        if (!strcmp(a, "--interp")) opt_interp = 1;
        else if (!strcmp(a, "--strace")) opt_strace = 1;
        else if (!strcmp(a, "--trace")) opt_trace = 1;
        else if (!strcmp(a, "-v")) opt_verbose = 1;
        else if ((!strcmp(a, "--sysroot") || !strcmp(a, "-L")) && k + 1 < argc) g_sysroot = argv[++k];
        else if (!strcmp(a, "--argv0") && k + 1 < argc) argv0 = argv[++k];
        else usage();
    }
    if (k >= argc) usage();
    if (getenv("A32_STRACE")) opt_strace = 1;
    if (getenv("A32_INTERP")) opt_interp = 1;

    a32_thread *t = thread_main();
    g_self = t;
    t->tid = getpid();
    t->cpu.thread = t;
    if (mem_init()) return 127;
    signals_init();

    const char *path = argv[k];
    char **gargv = &argv[k];
    if (argv0) gargv[0] = (char *)argv0;
    load_args la = {path, g_sysroot, argc - k, gargv, environ};
    a32_cpu *c = &t->cpu;
    if (load_program(&la, c)) return 127;
    c->thread = t;
    run_guest(c);
    return 0;
}
