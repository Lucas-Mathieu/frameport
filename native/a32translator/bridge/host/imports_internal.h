/* Host side of the bridge: libandroid imports, NativeActivity glue and native method registration. */
#ifndef A32B_IMPORTS_INTERNAL_H
#define A32B_IMPORTS_INTERNAL_H

#include <android/native_activity.h>
#include <android/native_window.h>
#include "jni_internal.h"

/* bridge.c: logs the current guest call's caller (lib+offset) and the return addresses on its stack */
void guest_backtrace(a32_cpu *c, const char *what);

/* gen/imports.c (from imports.txt) */
int import_dispatch(a32_cpu *c, int index, argreader *R);
extern const char *const import_names[];

/* gl.c: GLES/EGL (HC_GL + index); 0 if the function is unknown */
int gl_dispatch(a32_cpu *c, int index, argreader *R);

/* native_activity.c */
ANativeActivity *guest_activity(uint32_t g); /* the host activity behind a guest ANativeActivity * */
void activity_create(ANativeActivity *a, void *saved, size_t saved_size, uint32_t oncreate);
extern volatile int g_window_w, g_window_h; /* the activity's window size (0 before it exists) */

/* input.c: synthetic input events for NativeActivity guests (Unreal: native_app_glue reads its AInputQueue) */
void input_queue_attached(void *queue, void *looper, int ident, uint32_t data, int has_callback);
void input_queue_detached(void *queue);
/* a synthetic event is waiting for the queue attached to this looper: its ident and guest poll data */
int input_queue_pending(void *looper, int *ident, uint32_t *data);
void *input_queue_pop(void *queue); /* the next synthetic AInputEvent of this queue, or NULL */

/* natives.c: registers the exported Java_* functions of a guest library with their classes */
void natives_register_java(JNIEnv *env, uint32_t handle, const char *libname);
/* a host function pointer that calls guest function fn with the JNI signature sig ("(IJ)V") */
void *natives_trampoline(uint32_t fn, const char *sig);
void *natives_trampoline_named(uint32_t fn, const char *sig, const char *name); /* name: for tracing */

#endif
