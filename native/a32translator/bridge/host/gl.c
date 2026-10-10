/* GLES/EGL for guest code: function lookup, GLsync handles, driver strings, buffer mappings, and the functions
 * gen_gl.py leaves to hand-written code (pointer arrays, host pointers as results, callbacks). Everything else is
 * generated (gen/gl.c). */
#include <dlfcn.h>
#include <stdlib.h>
#define GL_GLEXT_PROTOTYPES 1
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include "gl_internal.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------- function lookup */

#define MISSING ((void *)1)
static void **fns;

static void *lookup(const char *name)
{
    static void *gles, *egl;
    static void *(*get_proc)(const char *);
    if (!egl) {
        egl = dlopen("libEGL.so", RTLD_NOW);
        gles = dlopen("libGLESv3.so", RTLD_NOW);
        if (!gles) gles = dlopen("libGLESv2.so", RTLD_NOW);
        get_proc = egl ? (void *(*)(const char *))dlsym(egl, "eglGetProcAddress") : NULL;
    }
    void *f = gles ? dlsym(gles, name) : NULL;
    if (!f && egl) f = dlsym(egl, name);
    if (!f && get_proc) f = get_proc(name);
    return f;
}

static void *fn_at(int index)
{
    void *f = __atomic_load_n(&fns[index], __ATOMIC_ACQUIRE);
    if (!f) {
        f = lookup(gl_names[index]);
        if (!f) f = MISSING;
        __atomic_store_n(&fns[index], f, __ATOMIC_RELEASE);
    }
    return f == MISSING ? NULL : f;
}

/* a host function by name, for the hand-written code */
static void *host_fn(const char *name)
{
    for (int k = 0; k < gl_count; k++)
        if (!strcmp(gl_names[k], name)) return fn_at(k);
    return lookup(name);
}

static void once_per_name(const char *what, const char *name)
{
    static const char *seen[512];
    static int n;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < n; k++)
        if (seen[k] == name) {
            pthread_mutex_unlock(&lock);
            return;
        }
    if (n < 512) seen[n++] = name;
    pthread_mutex_unlock(&lock);
    bridge_log(ANDROID_LOG_WARN, "%s %s (returned 0)", what, name);
}

int gl_dispatch(a32_cpu *c, int index, argreader *R)
{
    if (index < 0 || index >= gl_count) return 0;
    if (!fns) {
        pthread_mutex_lock(&lock);
        if (!fns) fns = calloc((size_t)gl_count, sizeof *fns);
        pthread_mutex_unlock(&lock);
    }
    void *f = fn_at(index);
    if (f && gl_dispatch_generated(c, index, R, f)) return 1;
    once_per_name(f ? "the bridge doesn't support" : "the system has no", gl_names[index]);
    c->r[0] = c->r[1] = 0;
    return 1;
}

/* ---------------------------------------------------------------- GLsync handles */

static void **syncs;
static uint32_t nsyncs, capsyncs, sync_free_list; /* index + 1 */

uint32_t sync2h(void *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&lock);
    uint32_t i;
    if (sync_free_list) {
        i = sync_free_list - 1;
        sync_free_list = (uint32_t)(uintptr_t)syncs[i];
    } else {
        if (nsyncs == capsyncs) syncs = realloc(syncs, (capsyncs = capsyncs ? capsyncs * 2 : 64) * sizeof *syncs);
        i = nsyncs++;
    }
    syncs[i] = s;
    pthread_mutex_unlock(&lock);
    return (i + 1) << 2 | 1; /* odd: never mistaken for another kind of handle in logs */
}

void *h2sync(uint32_t h)
{
    if (!h) return NULL;
    uint32_t i = (h >> 2) - 1;
    pthread_mutex_lock(&lock);
    void *s = i < nsyncs ? syncs[i] : NULL;
    pthread_mutex_unlock(&lock);
    return s;
}

static void sync_release(uint32_t h)
{
    uint32_t i = (h >> 2) - 1;
    pthread_mutex_lock(&lock);
    if (h && i < nsyncs) {
        syncs[i] = (void *)(uintptr_t)sync_free_list;
        sync_free_list = i + 1;
    }
    pthread_mutex_unlock(&lock);
}

void gl_glDeleteSync(a32_cpu *c, argreader *R, void *fn)
{
    uint32_t h = rd32(R);
    ((__typeof__(&glDeleteSync))fn)((GLsync)h2sync(h));
    sync_release(h);
}

/* ---------------------------------------------------------------- strings */

uint32_t gl_guest_string(const char *s) { return guest_cstr(s); }

void gl_glGetString(a32_cpu *c, argreader *R, void *fn)
{
    GLenum name = (GLenum)rd32(R);
    ret32(c, gl_guest_string((const char *)((__typeof__(&glGetString))fn)(name)));
}

void gl_glGetStringi(a32_cpu *c, argreader *R, void *fn)
{
    GLenum name = (GLenum)rd32(R);
    GLuint index = (GLuint)rd32(R);
    ret32(c, gl_guest_string((const char *)((__typeof__(&glGetStringi))fn)(name, index)));
}

void gl_eglQueryString(a32_cpu *c, argreader *R, void *fn)
{
    EGLDisplay dpy = h2ptr(rd32(R));
    EGLint name = (EGLint)rd32(R);
    ret32(c, gl_guest_string(((__typeof__(&eglQueryString))fn)(dpy, name)));
}

/* ---------------------------------------------------------------- pointer arrays */

static const GLchar **string_array(uint32_t g, GLsizei n)
{
    const GLchar **v = calloc(n > 0 ? (size_t)n : 1, sizeof *v);
    for (GLsizei k = 0; k < n; k++) v[k] = g2p(ld32(g + 4u * (uint32_t)k));
    return v;
}

void gl_glShaderSource(a32_cpu *c, argreader *R, void *fn)
{
    GLuint shader = rd32(R);
    GLsizei count = (GLsizei)rd32(R);
    uint32_t strings = rd32(R);
    const GLint *length = g2p(rd32(R));
    const GLchar **v = string_array(strings, count);
    ((__typeof__(&glShaderSource))fn)(shader, count, v, length);
    free(v);
}

void gl_glTransformFeedbackVaryings(a32_cpu *c, argreader *R, void *fn)
{
    GLuint program = rd32(R);
    GLsizei count = (GLsizei)rd32(R);
    const GLchar **v = string_array(rd32(R), count);
    GLenum mode = rd32(R);
    ((__typeof__(&glTransformFeedbackVaryings))fn)(program, count, v, mode);
    free(v);
}

void gl_glGetUniformIndices(a32_cpu *c, argreader *R, void *fn)
{
    GLuint program = rd32(R);
    GLsizei count = (GLsizei)rd32(R);
    const GLchar **v = string_array(rd32(R), count);
    GLuint *indices = g2p(rd32(R));
    ((__typeof__(&glGetUniformIndices))fn)(program, count, v, indices);
    free(v);
}

static void configs_out(uint32_t gconfigs, EGLConfig *cfg, EGLint n)
{
    for (EGLint k = 0; k < n; k++) st32(gconfigs + 4u * (uint32_t)k, ptr2h(cfg[k]));
}

void gl_eglChooseConfig(a32_cpu *c, argreader *R, void *fn)
{
    EGLDisplay dpy = h2ptr(rd32(R));
    const EGLint *attrib = g2p(rd32(R));
    uint32_t gconfigs = rd32(R);
    EGLint size = (EGLint)rd32(R);
    EGLint *num = g2p(rd32(R));
    EGLConfig *cfg = gconfigs && size > 0 ? calloc((size_t)size, sizeof *cfg) : NULL;
    EGLBoolean r = ((__typeof__(&eglChooseConfig))fn)(dpy, attrib, cfg, cfg ? size : 0, num);
    if (r && cfg) configs_out(gconfigs, cfg, *num < size ? *num : size);
    free(cfg);
    ret32(c, r);
}

void gl_eglGetConfigs(a32_cpu *c, argreader *R, void *fn)
{
    EGLDisplay dpy = h2ptr(rd32(R));
    uint32_t gconfigs = rd32(R);
    EGLint size = (EGLint)rd32(R);
    EGLint *num = g2p(rd32(R));
    EGLConfig *cfg = gconfigs && size > 0 ? calloc((size_t)size, sizeof *cfg) : NULL;
    EGLBoolean r = ((__typeof__(&eglGetConfigs))fn)(dpy, cfg, cfg ? size : 0, num);
    if (r && cfg) configs_out(gconfigs, cfg, *num < size ? *num : size);
    free(cfg);
    ret32(c, r);
}

/* ---------------------------------------------------------------- callbacks */

void gl_glDebugMessageCallback(a32_cpu *c, argreader *R, void *fn)
{
    (void)c, (void)R, (void)fn;
    once_per_name("ignored (guest callbacks aren't supported yet):", "glDebugMessageCallback");
}

void gl_glDebugMessageCallbackKHR(a32_cpu *c, argreader *R, void *fn) { gl_glDebugMessageCallback(c, R, fn); }

/* ---------------------------------------------------------------- buffer mappings */

/* A mapping the guest can't reach (driver memory above 4 GiB) gets a guest copy: filled when mapped for reading,
 * written back at flush/unmap when mapped for writing. One per (context, target). */
typedef struct {
    EGLContext ctx;
    GLenum target;
    uint8_t *host;
    uint32_t guest;
    uint32_t len;
    GLbitfield access;
} mapping;

static mapping maps[64];

static mapping *find_map(EGLContext ctx, GLenum target)
{
    for (int k = 0; k < 64; k++)
        if (maps[k].host && maps[k].ctx == ctx && maps[k].target == target) return &maps[k];
    return NULL;
}

static EGLContext current_context(void)
{
    static __typeof__(&eglGetCurrentContext) f;
    if (!f) f = (__typeof__(f))host_fn("eglGetCurrentContext");
    return f ? f() : NULL;
}

static uint32_t map_result(GLenum target, void *p, uint32_t len, GLbitfield access)
{
    if (!p) return 0;
    if (in_guest_reach(p, len)) return p2g(p);
    uint32_t g = guest_malloc(len ? len : 1);
    if (!g) return 0;
    if (access & GL_MAP_READ_BIT) memcpy(g2p(g), p, len);
    pthread_mutex_lock(&lock);
    EGLContext ctx = current_context();
    mapping *m = find_map(ctx, target);
    if (!m)
        for (int k = 0; k < 64 && !m; k++)
            if (!maps[k].host) m = &maps[k];
    if (m) *m = (mapping){ctx, target, p, g, len, access};
    pthread_mutex_unlock(&lock);
    return g;
}

void gl_glMapBufferRange(a32_cpu *c, argreader *R, void *fn)
{
    GLenum target = rd32(R);
    GLintptr offset = (int32_t)rd32(R);
    GLsizeiptr length = (int32_t)rd32(R);
    GLbitfield access = rd32(R);
    void *p = ((__typeof__(&glMapBufferRange))fn)(target, offset, length, access);
    ret32(c, map_result(target, p, (uint32_t)length, access));
}

void gl_glMapBufferRangeEXT(a32_cpu *c, argreader *R, void *fn) { gl_glMapBufferRange(c, R, fn); }

void gl_glMapBufferOES(a32_cpu *c, argreader *R, void *fn)
{
    GLenum target = rd32(R);
    GLenum access = rd32(R);
    GLint size = 0;
    __typeof__(&glGetBufferParameteriv) gp = (__typeof__(gp))host_fn("glGetBufferParameteriv");
    if (gp) gp(target, GL_BUFFER_SIZE, &size);
    void *p = ((__typeof__(&glMapBufferOES))fn)(target, access);
    ret32(c, map_result(target, p, (uint32_t)size, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT));
}

/* copies a mapping's guest data back (whole range, or [off, off+len) for an explicit flush) */
static void write_back(GLenum target, uint32_t off, uint32_t len, int unmap)
{
    pthread_mutex_lock(&lock);
    mapping *m = find_map(current_context(), target);
    mapping copy;
    if (m) {
        copy = *m;
        if (unmap) m->host = NULL;
    }
    pthread_mutex_unlock(&lock);
    if (!m) return;
    if (copy.access & GL_MAP_WRITE_BIT) {
        if (unmap) off = 0, len = copy.len;
        if (off < copy.len) memcpy(copy.host + off, g2p(copy.guest + off), len > copy.len - off ? copy.len - off : len);
    }
    if (unmap) guest_free(copy.guest);
}

void gl_glFlushMappedBufferRange(a32_cpu *c, argreader *R, void *fn)
{
    GLenum target = rd32(R);
    GLintptr offset = (int32_t)rd32(R);
    GLsizeiptr length = (int32_t)rd32(R);
    write_back(target, (uint32_t)offset, (uint32_t)length, 0);
    ((__typeof__(&glFlushMappedBufferRange))fn)(target, offset, length);
}

void gl_glFlushMappedBufferRangeEXT(a32_cpu *c, argreader *R, void *fn) { gl_glFlushMappedBufferRange(c, R, fn); }

void gl_glUnmapBuffer(a32_cpu *c, argreader *R, void *fn)
{
    GLenum target = rd32(R);
    write_back(target, 0, 0, 1);
    ret32(c, ((__typeof__(&glUnmapBuffer))fn)(target));
}

void gl_glUnmapBufferOES(a32_cpu *c, argreader *R, void *fn) { gl_glUnmapBuffer(c, R, fn); }
