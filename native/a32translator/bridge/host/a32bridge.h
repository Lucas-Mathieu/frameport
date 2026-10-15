/* Public API of the host bridge (liba32bridge.so), used by the per-library stubs (stub.c) and the PC harness.
 *
 * A converted APK keeps every 32-bit library libX.so as libX.a32.so next to the 64-bit ones. libX.so itself
 * becomes a copy of the stub, which forwards JNI_OnLoad and ANativeActivity_onCreate to the bridge. The bridge
 * runs the guest runtime (liba32rt.so, from the same folder) in this process and loads the guest libraries into
 * it. */
#ifndef A32BRIDGE_H
#define A32BRIDGE_H

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#define A32B_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeActivity;

/* Starts the guest runtime once (later calls return at once). libdir holds liba32rt.so and the guest libraries.
 * Returns 0 on success. */
A32B_API int a32b_init(JavaVM *vm, const char *libdir);

/* Loads the guest library for the stub at stub_path (".../libX.so" -> guest libX.so). Returns a guest handle, 0 on
 * failure. */
A32B_API uint32_t a32b_load(const char *stub_path);

/* What a stub's JNI_OnLoad does: init, load, the guest JNI_OnLoad, then registers the library's exported Java_*
 * functions with their classes. Returns the JNI version for ART (JNI_ERR on failure). */
A32B_API jint a32b_jni_onload(JavaVM *vm, const char *stub_path);

/* What a stub's ANativeActivity_onCreate does (the guest library's own ANativeActivity_onCreate). */
A32B_API void a32b_native_activity_oncreate(struct ANativeActivity *activity, void *saved, size_t saved_size,
                                            const char *stub_path);

/* Address of a guest symbol (0 if none). */
A32B_API uint32_t a32b_sym(uint32_t handle, const char *name);

/* Calls a guest function (AAPCS words: 64-bit values as an even-aligned pair). Returns r0 | r1 << 32. */
A32B_API uint64_t a32b_call(uint32_t fn, const uint32_t *words, int n);

#ifdef __cplusplus
}
#endif

#endif
