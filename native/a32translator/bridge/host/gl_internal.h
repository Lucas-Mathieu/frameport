/* GLES/EGL for guest code (gl.c, gen/gl.c). */
#ifndef A32B_GL_INTERNAL_H
#define A32B_GL_INTERNAL_H

#include "imports_internal.h"

/* gen/gl.c */
extern const int gl_count;
extern const char *const gl_names[];
int gl_dispatch_generated(a32_cpu *c, int index, argreader *R, void *fn);

/* gl.c */
uint32_t sync2h(void *s); /* GLsync handles: freed by glDeleteSync */
void *h2sync(uint32_t h);
uint32_t gl_guest_string(const char *s); /* a guest copy of a driver string (kept for good) */

#endif
