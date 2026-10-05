// SPDX-License-Identifier: GPL-3.0-only
// FramePort depth-format shim for GL-to-GLES wrappers (QuestCraft's LTW): desktop OpenGL's sized depth format
// GL_DEPTH_COMPONENT32 doesn't exist in OpenGL ES. Qualcomm's Quest driver accepts it anyway; the Frame's Mesa (Zink)
// rejects it (GL_INVALID_OPERATION in glTexImage2D), so Minecraft's depth textures never exist, every framebuffer that
// uses them is incomplete and the picture stays black. LTW resolves every GLES function through eglGetProcAddress from
// a libEGL it dlopens itself; FramePort points that dlopen string at this library (patches/frame/ltw_depth.py), which
// hands back wrappers for the texture/renderbuffer allocation functions and the real functions for everything else:
//   GL_DEPTH_COMPONENT32 -> GL_DEPTH_COMPONENT32F (GL_DEPTH_COMPONENT24 when pixel data comes as GL_UNSIGNED_INT),
//   unsized GL_DEPTH_COMPONENT with GL_FLOAT data -> GL_DEPTH_COMPONENT32F.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "FrameBridge", __VA_ARGS__)
#define EXPORT __attribute__((visibility("default")))
#ifndef GL_DEPTH_COMPONENT32
#define GL_DEPTH_COMPONENT32 0x81A7
#endif

typedef __eglMustCastToProperFunctionPointerType FnPtr;
static FnPtr (*real_gpa)(const char *);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int logged;

static void init(void) {
    void *egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    real_gpa = egl ? (FnPtr (*)(const char *))dlsym(egl, "eglGetProcAddress") : NULL;
    LOG("depth format shim: libEGL.so %s", real_gpa ? "OK" : "MISSING");
}

// the ES internal format for a desktop depth request (`type` = the pixel data type, 0 when there is none)
static GLint fix(GLint internal, GLenum type, const char *fn) {
    GLint out = internal;
    if (internal == GL_DEPTH_COMPONENT32) out = type == GL_UNSIGNED_INT ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT32F;
    else if (internal == GL_DEPTH_COMPONENT && type == GL_FLOAT) out = GL_DEPTH_COMPONENT32F;
    if (out != internal && logged < 8) {
        logged++;
        LOG("depth format shim: %s 0x%x -> 0x%x", fn, internal, out);
    }
    return out;
}

static PFNGLTEXIMAGE2DPROC r_ti2;
static PFNGLTEXIMAGE3DPROC r_ti3;
static PFNGLTEXSTORAGE2DPROC r_ts2;
static PFNGLTEXSTORAGE3DPROC r_ts3;
static PFNGLTEXSTORAGE2DMULTISAMPLEPROC r_ts2ms;
static PFNGLTEXSTORAGE3DMULTISAMPLEPROC r_ts3ms;
static PFNGLRENDERBUFFERSTORAGEPROC r_rbs;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC r_rbsms;

static void GL_APIENTRY w_ti2(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d) {
    r_ti2(t, l, fix(i, ty, "glTexImage2D"), w, h, b, f, ty, d);
}
static void GL_APIENTRY w_ti3(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLsizei dp, GLint b, GLenum f, GLenum ty,
                              const void *d) {
    r_ti3(t, l, fix(i, ty, "glTexImage3D"), w, h, dp, b, f, ty, d);
}
static void GL_APIENTRY w_ts2(GLenum t, GLsizei l, GLenum i, GLsizei w, GLsizei h) {
    r_ts2(t, l, (GLenum)fix((GLint)i, 0, "glTexStorage2D"), w, h);
}
static void GL_APIENTRY w_ts3(GLenum t, GLsizei l, GLenum i, GLsizei w, GLsizei h, GLsizei d) {
    r_ts3(t, l, (GLenum)fix((GLint)i, 0, "glTexStorage3D"), w, h, d);
}
static void GL_APIENTRY w_ts2ms(GLenum t, GLsizei s, GLenum i, GLsizei w, GLsizei h, GLboolean fl) {
    r_ts2ms(t, s, (GLenum)fix((GLint)i, 0, "glTexStorage2DMultisample"), w, h, fl);
}
static void GL_APIENTRY w_ts3ms(GLenum t, GLsizei s, GLenum i, GLsizei w, GLsizei h, GLsizei d, GLboolean fl) {
    r_ts3ms(t, s, (GLenum)fix((GLint)i, 0, "glTexStorage3DMultisample"), w, h, d, fl);
}
static void GL_APIENTRY w_rbs(GLenum t, GLenum i, GLsizei w, GLsizei h) {
    r_rbs(t, (GLenum)fix((GLint)i, 0, "glRenderbufferStorage"), w, h);
}
static void GL_APIENTRY w_rbsms(GLenum t, GLsizei s, GLenum i, GLsizei w, GLsizei h) {
    r_rbsms(t, s, (GLenum)fix((GLint)i, 0, "glRenderbufferStorageMultisample"), w, h);
}

EXPORT FnPtr eglGetProcAddress(const char *name) {
    pthread_once(&once, init);
    FnPtr fn = real_gpa && name ? real_gpa(name) : NULL;
    if (!fn) return fn;
#define WRAP(sym, real, w) if (!strcmp(name, sym)) { real = (void *)fn; return (FnPtr)w; }
    WRAP("glTexImage2D", r_ti2, w_ti2)
    WRAP("glTexImage3D", r_ti3, w_ti3)
    WRAP("glTexStorage2D", r_ts2, w_ts2)
    WRAP("glTexStorage3D", r_ts3, w_ts3)
    WRAP("glTexStorage2DMultisample", r_ts2ms, w_ts2ms)
    WRAP("glTexStorage3DMultisample", r_ts3ms, w_ts3ms)
    WRAP("glRenderbufferStorage", r_rbs, w_rbs)
    WRAP("glRenderbufferStorageMultisample", r_rbsms, w_rbsms)
#undef WRAP
    return fn;
}
