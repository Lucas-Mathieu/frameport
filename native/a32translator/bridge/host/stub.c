/* The stub: one copy per 32-bit library, renamed to that library's name (libX.so; the guest code is libX.a32.so).
 * It only finds its own path and hands JNI_OnLoad / ANativeActivity_onCreate to the bridge. */
#include <dlfcn.h>
#include <stddef.h>
#include "a32bridge.h"

static const char *self_path(void)
{
    Dl_info info;
    return dladdr((void *)self_path, &info) && info.dli_fname ? info.dli_fname : "";
}

A32B_API jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    return a32b_jni_onload(vm, self_path());
}

A32B_API void ANativeActivity_onCreate(struct ANativeActivity *activity, void *saved, size_t saved_size)
{
    a32b_native_activity_oncreate(activity, saved, saved_size, self_path());
}
