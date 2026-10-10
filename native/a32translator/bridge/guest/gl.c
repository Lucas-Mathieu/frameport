/* eglGetProcAddress for guest code: the runtime's own GLES/EGL stubs (gen/gl_stubs.S), found by name. */
#include "grt.h"

void *grt_eglGetProcAddress(const char *name) { return name ? (void *)(uintptr_t)grt_export(name) : 0; }
