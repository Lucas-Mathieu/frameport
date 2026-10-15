/* Interface between the guest runtime (liba32rt.so, 32-bit ARM) and the host bridge (arm64).
 *
 * The guest calls the host with a hypercall: SVC 0 with r7 = A32_HYPERCALL | n, arguments in r0-r3 and on the
 * stack (AAPCS, softfp), result in r0 (r0:r1 for 64-bit values). The host calls guest functions on the calling
 * thread's guest context; they return through A32_RETURN_TRAMPOLINE. Keep both sides in sync. */
#ifndef A32_BRIDGE_ABI_H
#define A32_BRIDGE_ABI_H

#define A32B_VERSION 1

/* hypercall numbers (low 16 bits of r7) */
#define HC_RETURN 1       /* end of a host-to-guest call; also parks a thread that has nothing to do */
#define HC_READY 2        /* r0 = struct grt_info *: the runtime is initialised (main thread parks here) */
#define HC_WAIT_WORK 3    /* service thread: blocks until the host has work; returns a WORK_* code */
#define HC_LOG 4          /* r0 = priority, r1 = tag, r2 = message */
#define HC_UNRESOLVED 5   /* r0 = symbol name: a guest library called a symbol nothing defines */
#define HC_ABORT 6        /* r0 = message: the guest runtime gave up */
#define HC_JNI 0x100      /* + JNINativeInterface index (0..232) */
#define HC_VM 0x200       /* + JNIInvokeInterface index (0..7) */
#define HC_IMPORT 0x300   /* + index into the host import table (bridge/imports.txt) */
#define HC_GL 0x1000      /* + index into the GLES/EGL table (gen_gl.py) */

/* service thread work */
#define WORK_NONE 0
#define WORK_ATTACH 1     /* pthread_create a thread that the waiting host thread adopts (threads.h) */

#define JNI_TABLE_SIZE 233
#define VM_TABLE_SIZE 8

/* Guest addresses the host needs, passed with HC_READY. All fields are 32-bit guest addresses. */
struct grt_info {
    unsigned version;
    unsigned jni_env;       /* JNIEnv: one for every thread (the host finds the thread itself) */
    unsigned java_vm;       /* JavaVM */
    unsigned fn_malloc, fn_free;
    unsigned fn_dlopen, fn_dlsym, fn_dlerror;  /* the runtime's loader (bionic semantics, LP32 RTLD_* values) */
    unsigned fn_attached_entry;
    unsigned fn_list_symbols; /* (handle, prefix) -> malloc'd (name, address) pairs, 0-terminated */
};

#endif
