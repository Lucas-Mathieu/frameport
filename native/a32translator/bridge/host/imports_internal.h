/* Host side of the bridge: libandroid imports, NativeActivity glue and native method registration. */
#ifndef A32B_IMPORTS_INTERNAL_H
#define A32B_IMPORTS_INTERNAL_H

#include <android/native_activity.h>
#include <android/native_window.h>
#include "jni_internal.h"

/* gen/imports.c (from imports.txt) */
int import_dispatch(a32_cpu *c, int index, argreader *R);
extern const char *const import_names[];

/* gl.c: GLES/EGL (HC_GL + index); 0 if the function is unknown */
int gl_dispatch(a32_cpu *c, int index, argreader *R);

/* native_activity.c */
ANativeActivity *guest_activity(uint32_t g); /* the host activity behind a guest ANativeActivity * */
void activity_create(ANativeActivity *a, void *saved, size_t saved_size, uint32_t oncreate);

/* natives.c: registers the exported Java_* functions of a guest library with their classes */
void natives_register_java(JNIEnv *env, uint32_t handle, const char *libname);
/* a host function pointer that calls guest function fn with the JNI signature sig ("(IJ)V") */
void *natives_trampoline(uint32_t fn, const char *sig);
void *natives_trampoline_named(uint32_t fn, const char *sig, const char *name); /* name: for tracing */

#endif
