/* Java native methods implemented by guest code. RegisterNatives (jni.c) and the exported Java_* functions of a
 * guest library both end up as a trampoline slot (tramp.S) that ART calls with the host ABI; a32b_native_call
 * converts the arguments to AAPCS32 (objects become handles, valid until the native method returns) and calls the
 * guest function. */
#include <stdlib.h>
#include "imports_internal.h"

#define NTRAMP 4096 /* tramp.S */

extern const char a32b_tramp_table[];

typedef struct {
    uint32_t fn;
    char *shorty;
    char *name; /* for A32B_TRACE */
} native;

int a32b_trace = -1; /* A32B_TRACE=1: log native registrations and calls */

static int tracing(void)
{
    if (a32b_trace < 0) a32b_trace = getenv("A32B_TRACE") != NULL;
    return a32b_trace;
}

static native natives[NTRAMP];
static int nnatives;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void *natives_trampoline_named(uint32_t fn, const char *sig, const char *name)
{
    char shorty[260];
    make_shorty(sig, shorty, sizeof shorty);
    pthread_mutex_lock(&lock);
    int k;
    for (k = 0; k < nnatives; k++)
        if (natives[k].fn == fn && !strcmp(natives[k].shorty, shorty)) break;
    if (k == nnatives) {
        if (nnatives == NTRAMP) {
            pthread_mutex_unlock(&lock);
            bridge_log(ANDROID_LOG_ERROR, "out of native method trampolines (%d)", NTRAMP);
            return NULL;
        }
        natives[k].fn = fn;
        natives[k].shorty = strdup(shorty);
        natives[k].name = strdup(name ? name : "?");
        __atomic_store_n(&nnatives, k + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
    if (tracing()) bridge_log(ANDROID_LOG_INFO, "native %s%s -> guest 0x%08x (slot %d)", name ? name : "?", sig, fn, k);
    return (void *)(a32b_tramp_table + 8 * k);
}

void *natives_trampoline(uint32_t fn, const char *sig) { return natives_trampoline_named(fn, sig, NULL); }

__attribute__((visibility("hidden"))) uint64_t a32b_native_call(uint64_t index, const uint64_t *gpr,
                                                                 const uint64_t *fpr, const uint64_t *stack)
{
    const native *nt = &natives[index];
    if (tracing()) bridge_log(ANDROID_LOG_INFO, "call %s", nt->name);
    uint32_t mark = frame_mark();
    argbuilder B;
    B.n = 0;
    ab_32(&B, g_info.jni_env);
    ab_32(&B, o2h_local((jobject)gpr[1])); /* this or the class */
    int ng = 2, nf = 0, ns = 0;
    for (const char *p = nt->shorty; *p != ')'; p++) {
        uint64_t v = (*p == 'F' || *p == 'D') ? (nf < 8 ? fpr[nf++] : stack[ns++]) : (ng < 8 ? gpr[ng++] : stack[ns++]);
        switch (*p) {
        case 'Z': ab_32(&B, (uint8_t)v); break;
        case 'B': ab_32(&B, (uint32_t)(int32_t)(int8_t)v); break;
        case 'C': ab_32(&B, (uint16_t)v); break;
        case 'S': ab_32(&B, (uint32_t)(int32_t)(int16_t)v); break;
        case 'I':
        case 'F': ab_32(&B, (uint32_t)v); break; /* a float's bits are the low half of its register / slot */
        case 'J':
        case 'D': ab_64(&B, v); break;
        default: ab_32(&B, o2h_local((jobject)v)); break;
        }
    }
    uint64_t r = guest_call(nt->fn, B.w, B.n);
    const char *rt = strchr(nt->shorty, ')') + 1;
    switch (*rt) {
    case 'V': r = 0; break;
    case 'Z': r = (uint8_t)r; break;
    case 'B': r = (uint64_t)(int64_t)(int8_t)r; break;
    case 'C': r = (uint16_t)r; break;
    case 'S': r = (uint64_t)(int64_t)(int16_t)r; break;
    case 'I': r = (uint64_t)(int64_t)(int32_t)r; break;
    case 'F': r = (uint32_t)r; break;
    case 'J':
    case 'D': break;
    default: r = (uint64_t)(uintptr_t)h2o((uint32_t)r); break; /* still a valid reference: popped below is only the handle */
    }
    frame_pop(mark);
    return r;
}

/* ---------------------------------------------------------------- Java_* exports */

/* JNI short/long names: Java_<class>_<method>[__<argument types>] with '/' as '_' and the escapes _0xxxx (UTF-16
 * unit), _1 '_', _2 ';', _3 '['. Returns 0 and fills cls ("a/b/C"), method and args (the long name's argument
 * types, "" for a short name; *has_args says which). */
static int demangle(const char *s, char *cls, char *method, char *args, size_t n, int *has_args)
{
    char path[512];
    size_t k = 0;
    char *out = path;
    size_t cap = sizeof path;
    *has_args = 0;
    args[0] = 0;
    for (s += 5; *s; s++) {
        char ch = *s;
        if (ch == '_') {
            char e = s[1];
            if (e == '1') ch = '_', s++;
            else if (e == '2') ch = ';', s++;
            else if (e == '3') ch = '[', s++;
            else if (e == '0') {
                unsigned u = 0;
                for (int j = 0; j < 4; j++) {
                    char h = s[2 + j];
                    if (!h) return -1;
                    u = u * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                s += 5;
                if (u >= 0x80) return -1; /* non-ASCII class names: not worth it */
                ch = (char)u;
            } else if (e == '_' && out == path) {
                /* start of the long name's argument types */
                out[k] = 0;
                out = args;
                cap = n;
                k = 0;
                *has_args = 1;
                s++;
                continue;
            } else {
                ch = '/';
            }
        }
        if (k + 1 >= cap) return -1;
        out[k++] = ch;
    }
    out[k] = 0;
    char *slash = strrchr(path, '/');
    if (!slash) return -1;
    *slash = 0;
    snprintf(cls, n, "%s", path);
    snprintf(method, n, "%s", slash + 1);
    return 0;
}

/* descriptor of a class object: "I", "Ljava/lang/String;", "[I" */
static void class_descriptor(JNIEnv *env, jclass k, jmethodID get_name, char *out, size_t n)
{
    jstring s = (*env)->CallObjectMethod(env, k, get_name);
    const char *nm = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    out[0] = 0;
    if (nm) {
        static const char *const prim[] = {"boolean", "byte", "char", "short", "int", "long", "float", "double",
                                           "void"};
        static const char letters[] = "ZBCSIJFDV";
        int found = 0;
        for (int i = 0; i < 9; i++)
            if (!strcmp(nm, prim[i])) snprintf(out, n, "%c", letters[i]), found = 1;
        if (!found) snprintf(out, n, nm[0] == '[' ? "%s" : "L%s;", nm);
        for (char *p = out; *p; p++)
            if (*p == '.') *p = '/';
        (*env)->ReleaseStringUTFChars(env, s, nm);
    }
    if (s) (*env)->DeleteLocalRef(env, s);
}

/* The JNI signature of the native method `name` of cls (with argument types args when has_args). */
static int native_signature(JNIEnv *env, jclass cls, const char *name, const char *args, int has_args, char *sig,
                            size_t n)
{
    jclass kc = (*env)->FindClass(env, "java/lang/Class");
    jclass km = (*env)->FindClass(env, "java/lang/reflect/Method");
    jmethodID get_methods = (*env)->GetMethodID(env, kc, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID get_name = (*env)->GetMethodID(env, kc, "getName", "()Ljava/lang/String;");
    jmethodID m_name = (*env)->GetMethodID(env, km, "getName", "()Ljava/lang/String;");
    jmethodID m_mods = (*env)->GetMethodID(env, km, "getModifiers", "()I");
    jmethodID m_params = (*env)->GetMethodID(env, km, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID m_ret = (*env)->GetMethodID(env, km, "getReturnType", "()Ljava/lang/Class;");
    if (!get_methods || !get_name || !m_name || !m_mods || !m_params || !m_ret) return -1;
    jobjectArray ms = (*env)->CallObjectMethod(env, cls, get_methods);
    if (!ms || (*env)->ExceptionCheck(env)) return -1;
    int found = -1;
    jsize nm = (*env)->GetArrayLength(env, ms);
    for (jsize i = 0; i < nm && found; i++) {
        jobject m = (*env)->GetObjectArrayElement(env, ms, i);
        jstring mn = (*env)->CallObjectMethod(env, m, m_name);
        const char *mname = (*env)->GetStringUTFChars(env, mn, NULL);
        int match = mname && !strcmp(mname, name) && ((*env)->CallIntMethod(env, m, m_mods) & 0x100 /* native */);
        if (mname) (*env)->ReleaseStringUTFChars(env, mn, mname);
        (*env)->DeleteLocalRef(env, mn);
        if (match) {
            char buf[1024], d[256];
            size_t k = 0;
            buf[k++] = '(';
            jobjectArray ps = (*env)->CallObjectMethod(env, m, m_params);
            jsize np = ps ? (*env)->GetArrayLength(env, ps) : 0;
            for (jsize j = 0; j < np; j++) {
                jclass pk = (*env)->GetObjectArrayElement(env, ps, j);
                class_descriptor(env, pk, get_name, d, sizeof d);
                (*env)->DeleteLocalRef(env, pk);
                k += (size_t)snprintf(buf + k, sizeof buf - k, "%s", d);
                if (k >= sizeof buf - 1) break;
            }
            if (ps) (*env)->DeleteLocalRef(env, ps);
            buf[k] = 0;
            if (!has_args || !strcmp(buf + 1, args)) {
                jclass rk = (*env)->CallObjectMethod(env, m, m_ret);
                class_descriptor(env, rk, get_name, d, sizeof d);
                (*env)->DeleteLocalRef(env, rk);
                snprintf(sig, n, "%s)%s", buf, d);
                found = 0;
            }
        }
        (*env)->DeleteLocalRef(env, m);
    }
    (*env)->DeleteLocalRef(env, ms);
    return found;
}

void natives_register_java(JNIEnv *env, uint32_t handle, const char *libname)
{
    uint32_t prefix = guest_strdup("Java_");
    uint32_t w[2] = {handle, prefix};
    uint32_t list = (uint32_t)guest_call(g_info.fn_list_symbols, w, 2);
    guest_free(prefix);
    if (!list) return;
    int ok = 0, failed = 0;
    for (uint32_t e = list; ld32(e); e += 8) {
        const char *sym = g2p(ld32(e));
        uint32_t addr = ld32(e + 4);
        char cls[512], method[512], args[512], sig[1024];
        int has_args;
        if (demangle(sym, cls, method, args, sizeof cls, &has_args)) {
            bridge_log(ANDROID_LOG_WARN, "%s: can't decode %s", libname, sym);
            failed++;
            continue;
        }
        jclass k = (*env)->FindClass(env, cls);
        if (!k) {
            (*env)->ExceptionClear(env);
            bridge_log(ANDROID_LOG_WARN, "%s: %s: class %s not found", libname, sym, cls);
            failed++;
            continue;
        }
        if (native_signature(env, k, method, args, has_args, sig, sizeof sig)) {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            bridge_log(ANDROID_LOG_WARN, "%s: %s: no native method %s in %s", libname, sym, method, cls);
            (*env)->DeleteLocalRef(env, k);
            failed++;
            continue;
        }
        JNINativeMethod m = {method, sig, natives_trampoline_named(addr, sig, method)};
        if (!m.fnPtr || (*env)->RegisterNatives(env, k, &m, 1)) {
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            bridge_log(ANDROID_LOG_WARN, "%s: registering %s.%s%s failed", libname, cls, method, sig);
            failed++;
        } else {
            ok++;
        }
        (*env)->DeleteLocalRef(env, k);
    }
    guest_free(list);
    if (ok || failed) bridge_log(ANDROID_LOG_INFO, "%s: %d Java_ functions registered, %d not", libname, ok, failed);
}
