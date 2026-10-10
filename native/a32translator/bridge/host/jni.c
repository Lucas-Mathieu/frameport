/* JNI for guest code, the parts gen_stubs.py can't generate: the Call*Method / NewObject families (variadic,
 * va_list and jvalue[] arguments, read with the method's shorty), method IDs (which remember their shorty),
 * references, strings and primitive arrays (shared when the host's data lies in the guest's 4 GiB, as ART's heap
 * does, else copied), RegisterNatives (trampolines, natives.c) and the JavaVM functions. */
#include <stdlib.h>
#include "imports_internal.h"

/* ---------------------------------------------------------------- shorties */

/* "(I[Ljava/lang/String;J)V" -> "ILJ)V" */
void make_shorty(const char *sig, char *out, size_t n)
{
    size_t k = 0;
    const char *p = sig;
    if (*p == '(') p++;
    while (*p && *p != ')') {
        char t = *p;
        if (t == '[') {
            while (*p == '[') p++;
            if (*p == 'L') p = strchr(p, ';');
            if (!p) break;
            p++;
            t = 'L';
        } else if (t == 'L') {
            p = strchr(p, ';');
            if (!p) break;
            p++;
        } else {
            p++;
        }
        if (k + 3 < n) out[k++] = t;
    }
    char r = 'V';
    if (p && *p == ')') {
        p++;
        r = *p == '[' ? 'L' : *p ? *p : 'V';
    }
    out[k++] = ')';
    out[k++] = r;
    out[k] = 0;
}

/* JVM type letter of a class object: "int" -> 'I', anything not primitive -> 'L' */
static char class_letter(JNIEnv *env, jclass k)
{
    static jmethodID get_name;
    if (!get_name) get_name = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Class"), "getName",
                                                  "()Ljava/lang/String;");
    jstring s = (*env)->CallObjectMethod(env, k, get_name);
    const char *n = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    char r = 'L';
    if (n) {
        static const char *const prim[] = {"boolean", "byte", "char", "short", "int", "long", "float", "double",
                                           "void"};
        static const char letters[] = "ZBCSIJFDV";
        for (int i = 0; i < 9; i++)
            if (!strcmp(n, prim[i])) r = letters[i];
        (*env)->ReleaseStringUTFChars(env, s, n);
    }
    if (s) (*env)->DeleteLocalRef(env, s);
    return r;
}

/* the shorty of a java.lang.reflect.Method / Constructor */
static int reflected_shorty(JNIEnv *env, jobject m, char *out, size_t n)
{
    jclass mc = (*env)->GetObjectClass(env, m);
    jmethodID params = (*env)->GetMethodID(env, mc, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID ret = (*env)->GetMethodID(env, mc, "getReturnType", "()Ljava/lang/Class;");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* a Constructor has no getReturnType */
    if (!params) return -1;
    jobjectArray a = (*env)->CallObjectMethod(env, m, params);
    jsize np = a ? (*env)->GetArrayLength(env, a) : 0;
    size_t k = 0;
    for (jsize i = 0; i < np && k + 3 < n; i++) {
        jclass pk = (*env)->GetObjectArrayElement(env, a, i);
        out[k++] = class_letter(env, pk);
        (*env)->DeleteLocalRef(env, pk);
    }
    out[k++] = ')';
    char r = 'V';
    if (ret) {
        jclass rk = (*env)->CallObjectMethod(env, m, ret);
        r = class_letter(env, rk);
        (*env)->DeleteLocalRef(env, rk);
    }
    out[k++] = r;
    out[k] = 0;
    return 0;
}

/* ---------------------------------------------------------------- function classes */

enum {
    S_NONE, S_CALL, S_GET_ELEMS, S_REL_ELEMS, S_GET_METHOD, S_FROM_REFLECTED, S_NEW_GLOBAL, S_DELETE_GLOBAL,
    S_DELETE_LOCAL, S_NEW_WEAK, S_DELETE_WEAK, S_POP_FRAME, S_GET_STRING_CHARS, S_REL_STRING_CHARS,
    S_GET_STRING_UTF, S_REL_STRING_UTF, S_GET_CRITICAL, S_REL_CRITICAL, S_REGISTER, S_GET_VM, S_DIRECT_ADDR,
};
enum { K_VIRTUAL = 1, K_NONVIRTUAL, K_STATIC, K_NEW };

static struct {
    uint8_t special, kind, var; /* var: 0 "...", 1 va_list, 2 jvalue[] */
    char type;                  /* result (calls) or element type (arrays) */
} fn[JNI_TABLE_SIZE];
static pthread_once_t fn_once = PTHREAD_ONCE_INIT;

static char type_word(const char **p)
{
    static const struct { const char *w; char t; } words[] = {
        {"Object", 'L'}, {"Boolean", 'Z'}, {"Byte", 'B'}, {"Char", 'C'}, {"Short", 'S'},
        {"Int", 'I'},    {"Long", 'J'},    {"Float", 'F'}, {"Double", 'D'}, {"Void", 'V'},
    };
    for (size_t k = 0; k < sizeof words / sizeof *words; k++) {
        size_t n = strlen(words[k].w);
        if (!strncmp(*p, words[k].w, n)) {
            *p += n;
            return words[k].t;
        }
    }
    return 0;
}

static void classify(void)
{
    static const struct { const char *name; uint8_t s; } named[] = {
        {"GetMethodID", S_GET_METHOD}, {"GetStaticMethodID", S_GET_METHOD},
        {"FromReflectedMethod", S_FROM_REFLECTED}, {"NewGlobalRef", S_NEW_GLOBAL},
        {"DeleteGlobalRef", S_DELETE_GLOBAL}, {"DeleteLocalRef", S_DELETE_LOCAL},
        {"NewWeakGlobalRef", S_NEW_WEAK}, {"DeleteWeakGlobalRef", S_DELETE_WEAK},
        {"PopLocalFrame", S_POP_FRAME}, {"GetStringChars", S_GET_STRING_CHARS},
        {"ReleaseStringChars", S_REL_STRING_CHARS}, {"GetStringCritical", S_GET_STRING_CHARS},
        {"ReleaseStringCritical", S_REL_STRING_CHARS}, {"GetStringUTFChars", S_GET_STRING_UTF},
        {"ReleaseStringUTFChars", S_REL_STRING_UTF}, {"GetPrimitiveArrayCritical", S_GET_CRITICAL},
        {"ReleasePrimitiveArrayCritical", S_REL_CRITICAL}, {"RegisterNatives", S_REGISTER},
        {"GetJavaVM", S_GET_VM}, {"GetDirectBufferAddress", S_DIRECT_ADDR},
    };
    for (int i = 0; i < JNI_TABLE_SIZE; i++) {
        const char *n = jni_names[i];
        for (size_t k = 0; k < sizeof named / sizeof *named; k++)
            if (!strcmp(n, named[k].name)) fn[i].special = named[k].s;
        if (fn[i].special) continue;
        const char *p = n;
        uint8_t kind = 0;
        char t = 0;
        if (!strncmp(p, "NewObject", 9) && (!p[9] || !strcmp(p + 9, "V") || !strcmp(p + 9, "A"))) {
            kind = K_NEW, t = 'L', p += 9;
        } else if (!strncmp(p, "CallNonvirtual", 14)) {
            p += 14, kind = K_NONVIRTUAL, t = type_word(&p);
        } else if (!strncmp(p, "CallStatic", 10)) {
            p += 10, kind = K_STATIC, t = type_word(&p);
        } else if (!strncmp(p, "Call", 4)) {
            p += 4, kind = K_VIRTUAL, t = type_word(&p);
        }
        if (kind && t) {
            if (kind != K_NEW) {
                if (strncmp(p, "Method", 6)) continue;
                p += 6;
            }
            fn[i].special = S_CALL;
            fn[i].kind = kind;
            fn[i].type = t;
            fn[i].var = *p == 'V' ? 1 : *p == 'A' ? 2 : 0;
            continue;
        }
        int get = !strncmp(n, "Get", 3), rel = !strncmp(n, "Release", 7);
        if ((get || rel) && strstr(n, "ArrayElements")) {
            p = n + (get ? 3 : 7);
            t = type_word(&p);
            if (t && t != 'L' && !strcmp(p, "ArrayElements")) {
                fn[i].special = get ? S_GET_ELEMS : S_REL_ELEMS;
                fn[i].type = t;
            }
        }
    }
}

/* ---------------------------------------------------------------- method calls */

static jvalue arg_value(char t, argreader *A)
{
    jvalue v;
    v.j = 0;
    switch (t) {
    case 'Z': v.z = (jboolean)rd32(A); break;
    case 'B': v.b = (jbyte)rd32(A); break;
    case 'C': v.c = (jchar)rd32(A); break;
    case 'S': v.s = (jshort)rd32(A); break;
    case 'I': v.i = (jint)rd32(A); break;
    case 'J': v.j = (jlong)rd64(A); break;
    case 'F': v.f = (jfloat)rdd(A); break; /* promoted to double */
    case 'D': v.d = rdd(A); break;
    default: v.l = h2o(rd32(A)); break;
    }
    return v;
}

static jvalue array_value(char t, uint32_t a)
{
    jvalue v;
    v.j = 0;
    switch (t) {
    case 'Z': v.z = ld8(a); break;
    case 'B': v.b = (jbyte)ld8(a); break;
    case 'C': v.c = ld16(a); break;
    case 'S': v.s = (jshort)ld16(a); break;
    case 'I': v.i = (jint)ld32(a); break;
    case 'J': v.j = (jlong)ld64(a); break;
    case 'F': { uint32_t b = ld32(a); memcpy(&v.f, &b, 4); break; }
    case 'D': { uint64_t b = ld64(a); memcpy(&v.d, &b, 8); break; }
    default: v.l = h2o(ld32(a)); break;
    }
    return v;
}

#define CALL(T)                                                                                                 \
    (kind == K_VIRTUAL ? (*env)->Call##T##MethodA(env, obj, m, a)                                               \
     : kind == K_NONVIRTUAL ? (*env)->CallNonvirtual##T##MethodA(env, obj, cls, m, a)                           \
                            : (*env)->CallStatic##T##MethodA(env, cls, m, a))

static void call_method(a32_cpu *c, JNIEnv *env, int index, argreader *R)
{
    int kind = fn[index].kind;
    jobject obj = NULL;
    jclass cls = NULL;
    if (kind == K_VIRTUAL || kind == K_NONVIRTUAL) obj = h2o(rd32(R));
    if (kind != K_VIRTUAL) cls = (jclass)h2o(rd32(R));
    uint32_t mh = rd32(R);
    jmethodID m = h2mid(mh);
    const char *shorty = mid_shorty(mh);
    if (!m || !shorty) {
        bridge_log(ANDROID_LOG_ERROR, "%s: unknown method ID 0x%x", jni_names[index], mh);
        c->r[0] = c->r[1] = 0;
        return;
    }
    jvalue a[256];
    int n = 0;
    if (fn[index].var == 2) {
        uint32_t arr = rd32(R);
        for (const char *p = shorty; *p != ')' && n < 256; p++, n++) a[n] = array_value(*p, arr + 8u * (uint32_t)n);
    } else {
        argreader V, *A = R;
        if (fn[index].var == 1) {
            ar_init(&V, c, 4, rd32(R)); /* va_list: a pointer to the next argument in memory */
            A = &V;
        }
        for (const char *p = shorty; *p != ')' && n < 256; p++, n++) a[n] = arg_value(*p, A);
    }
    switch (fn[index].type) {
    case 'V': CALL(Void); break;
    case 'Z': ret32(c, CALL(Boolean)); break;
    case 'B': ret32(c, (uint32_t)(int32_t)CALL(Byte)); break;
    case 'C': ret32(c, CALL(Char)); break;
    case 'S': ret32(c, (uint32_t)(int32_t)CALL(Short)); break;
    case 'I': ret32(c, (uint32_t)CALL(Int)); break;
    case 'J': ret64(c, (uint64_t)CALL(Long)); break;
    case 'F': retf(c, CALL(Float)); break;
    case 'D': retd(c, CALL(Double)); break;
    default:
        if (kind == K_NEW) ret32(c, o2h_local((*env)->NewObjectA(env, cls, m, a)));
        else ret32(c, o2h_local(CALL(Object)));
        break;
    }
}

/* ---------------------------------------------------------------- copies of host data in guest memory */

/* Guest buffers that stand for host data outside the guest's 4 GiB (guest address -> size in bytes). */
static pthread_mutex_t copies_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t g, size; } *copies;
static int ncopies, capcopies;

static void copy_add(uint32_t g, uint32_t size)
{
    pthread_mutex_lock(&copies_lock);
    if (ncopies == capcopies) copies = realloc(copies, (size_t)(capcopies = capcopies ? capcopies * 2 : 64) * sizeof *copies);
    copies[ncopies].g = g;
    copies[ncopies++].size = size;
    pthread_mutex_unlock(&copies_lock);
}

/* -1 if g isn't a copy, else its size (forgotten unless keep) */
static int64_t copy_find(uint32_t g, int keep)
{
    int64_t r = -1;
    pthread_mutex_lock(&copies_lock);
    for (int k = ncopies - 1; k >= 0; k--)
        if (copies[k].g == g) {
            r = copies[k].size;
            if (!keep) copies[k] = copies[--ncopies];
            break;
        }
    pthread_mutex_unlock(&copies_lock);
    return r;
}


static int elem_size(char t)
{
    switch (t) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
    }
}

/* element size of a primitive array from its class name ("[I") */
static int array_elem_size(JNIEnv *env, jarray a)
{
    static jmethodID get_name;
    jclass k = (*env)->GetObjectClass(env, a);
    if (!get_name) get_name = (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Class"), "getName",
                                                  "()Ljava/lang/String;");
    jstring s = (*env)->CallObjectMethod(env, k, get_name);
    const char *n = (*env)->GetStringUTFChars(env, s, NULL);
    int r = n && n[0] == '[' ? elem_size(n[1]) : 1;
    (*env)->ReleaseStringUTFChars(env, s, n);
    (*env)->DeleteLocalRef(env, s);
    (*env)->DeleteLocalRef(env, k);
    return r;
}

static void set_is_copy(uint32_t is_copy, int v)
{
    if (is_copy) st8(is_copy, (uint8_t)v);
}

/* host data p (bytes) -> what the guest gets: p itself when the guest can reach it, else a copy */
static uint32_t share_or_copy(void *p, size_t bytes, int *copied)
{
    if (in_guest_reach(p, bytes)) {
        *copied = 0;
        return p2g(p);
    }
    uint32_t g = guest_malloc(bytes ? (uint32_t)bytes : 1);
    if (g) memcpy(g2p(g), p, bytes);
    *copied = 1;
    return g;
}

static uint32_t get_elems(JNIEnv *env, char t, jarray a, uint32_t is_copy)
{
    void *p = NULL;
    switch (t) {
    case 'Z': p = (*env)->GetBooleanArrayElements(env, a, NULL); break;
    case 'B': p = (*env)->GetByteArrayElements(env, a, NULL); break;
    case 'C': p = (*env)->GetCharArrayElements(env, a, NULL); break;
    case 'S': p = (*env)->GetShortArrayElements(env, a, NULL); break;
    case 'I': p = (*env)->GetIntArrayElements(env, a, NULL); break;
    case 'J': p = (*env)->GetLongArrayElements(env, a, NULL); break;
    case 'F': p = (*env)->GetFloatArrayElements(env, a, NULL); break;
    case 'D': p = (*env)->GetDoubleArrayElements(env, a, NULL); break;
    }
    if (!p) return 0;
    size_t bytes = (size_t)(*env)->GetArrayLength(env, a) * (size_t)elem_size(t);
    int copied;
    uint32_t g = share_or_copy(p, bytes, &copied);
    if (copied) {
        switch (t) {
        case 'Z': (*env)->ReleaseBooleanArrayElements(env, a, p, JNI_ABORT); break;
        case 'B': (*env)->ReleaseByteArrayElements(env, a, p, JNI_ABORT); break;
        case 'C': (*env)->ReleaseCharArrayElements(env, a, p, JNI_ABORT); break;
        case 'S': (*env)->ReleaseShortArrayElements(env, a, p, JNI_ABORT); break;
        case 'I': (*env)->ReleaseIntArrayElements(env, a, p, JNI_ABORT); break;
        case 'J': (*env)->ReleaseLongArrayElements(env, a, p, JNI_ABORT); break;
        case 'F': (*env)->ReleaseFloatArrayElements(env, a, p, JNI_ABORT); break;
        case 'D': (*env)->ReleaseDoubleArrayElements(env, a, p, JNI_ABORT); break;
        }
        copy_add(g, (uint32_t)bytes);
    }
    set_is_copy(is_copy, copied);
    return g;
}

/* a guest copy of array data goes back through the critical accessor (any element type) */
static void write_back(JNIEnv *env, jarray a, uint32_t g, uint32_t size)
{
    void *p = (*env)->GetPrimitiveArrayCritical(env, a, NULL);
    if (!p) return;
    memcpy(p, g2p(g), size);
    (*env)->ReleasePrimitiveArrayCritical(env, a, p, 0);
}

static void release_elems(JNIEnv *env, char t, jarray a, uint32_t g, jint mode)
{
    int64_t size = copy_find(g, mode == JNI_COMMIT);
    if (size >= 0) {
        if (mode != JNI_ABORT) write_back(env, a, g, (uint32_t)size);
        if (mode != JNI_COMMIT) guest_free(g);
        return;
    }
    void *p = g2p(g);
    switch (t) {
    case 'Z': (*env)->ReleaseBooleanArrayElements(env, a, p, mode); break;
    case 'B': (*env)->ReleaseByteArrayElements(env, a, p, mode); break;
    case 'C': (*env)->ReleaseCharArrayElements(env, a, p, mode); break;
    case 'S': (*env)->ReleaseShortArrayElements(env, a, p, mode); break;
    case 'I': (*env)->ReleaseIntArrayElements(env, a, p, mode); break;
    case 'J': (*env)->ReleaseLongArrayElements(env, a, p, mode); break;
    case 'F': (*env)->ReleaseFloatArrayElements(env, a, p, mode); break;
    case 'D': (*env)->ReleaseDoubleArrayElements(env, a, p, mode); break;
    }
}

/* ---------------------------------------------------------------- dispatch */

static void register_natives(a32_cpu *c, JNIEnv *env, argreader *R)
{
    jclass cls = (jclass)h2o(rd32(R));
    uint32_t methods = rd32(R);
    jint n = (jint)rd32(R);
    JNINativeMethod *hm = calloc(n > 0 ? (size_t)n : 1, sizeof *hm);
    for (jint k = 0; k < n; k++) {
        uint32_t e = methods + 12u * (uint32_t)k;
        hm[k].name = g2p(ld32(e));
        hm[k].signature = g2p(ld32(e + 4));
        hm[k].fnPtr = natives_trampoline_named(ld32(e + 8), hm[k].signature, hm[k].name);
        if (!hm[k].fnPtr) {
            free(hm);
            ret32(c, (uint32_t)JNI_ERR);
            return;
        }
    }
    jint r = (*env)->RegisterNatives(env, cls, hm, n);
    if (r) bridge_log(ANDROID_LOG_ERROR, "RegisterNatives of %d methods failed (%d)", n, r);
    ret32(c, (uint32_t)r);
    free(hm);
}

static pthread_once_t unattached_once = PTHREAD_ONCE_INIT;
static void unattached_warning(void) { bridge_log(ANDROID_LOG_ERROR, "JNI call on a thread not attached to the VM"); }

int jni_dispatch(a32_cpu *c, int index)
{
    pthread_once(&fn_once, classify);
    JNIEnv *env = host_env();
    if (!env) {
        pthread_once(&unattached_once, unattached_warning);
        c->r[0] = c->r[1] = 0;
        return 1;
    }
    argreader R;
    ar_init(&R, c, 1, c->r[13] + 8); /* r0 = the guest's JNIEnv; the stub pushed {r7, lr} */
    switch (fn[index].special) {
    case S_NONE:
        if (jni_dispatch_generated(c, env, index, &R)) return 1;
        bridge_log(ANDROID_LOG_ERROR, "JNI function %d (%s) is not supported", index, jni_names[index]);
        c->r[0] = c->r[1] = 0;
        return 1;
    case S_CALL:
        call_method(c, env, index, &R);
        return 1;
    case S_GET_METHOD: {
        jclass cls = (jclass)h2o(rd32(&R));
        const char *name = g2p(rd32(&R)), *sig = g2p(rd32(&R));
        jmethodID m = !strcmp(jni_names[index], "GetMethodID") ? (*env)->GetMethodID(env, cls, name, sig)
                                                               : (*env)->GetStaticMethodID(env, cls, name, sig);
        uint32_t h = mid2h(m);
        if (h) {
            char shorty[260];
            make_shorty(sig, shorty, sizeof shorty);
            mid_set_shorty(h, shorty);
        }
        ret32(c, h);
        return 1;
    }
    case S_FROM_REFLECTED: {
        jobject o = h2o(rd32(&R));
        uint32_t h = mid2h((*env)->FromReflectedMethod(env, o));
        char shorty[260];
        if (h && !mid_shorty(h) && !reflected_shorty(env, o, shorty, sizeof shorty)) mid_set_shorty(h, shorty);
        ret32(c, h);
        return 1;
    }
    case S_NEW_GLOBAL:
        ret32(c, o2h_global((*env)->NewGlobalRef(env, h2o(rd32(&R)))));
        return 1;
    case S_NEW_WEAK:
        ret32(c, o2h_weak((*env)->NewWeakGlobalRef(env, h2o(rd32(&R)))));
        return 1;
    case S_DELETE_GLOBAL:
    case S_DELETE_LOCAL:
    case S_DELETE_WEAK: {
        uint32_t h = rd32(&R);
        jobject o = h2o(h);
        if (o) {
            if (fn[index].special == S_DELETE_GLOBAL) (*env)->DeleteGlobalRef(env, o);
            else if (fn[index].special == S_DELETE_WEAK) (*env)->DeleteWeakGlobalRef(env, o);
            else (*env)->DeleteLocalRef(env, o);
        }
        h_free(h);
        return 1;
    }
    case S_POP_FRAME:
        ret32(c, o2h_local((*env)->PopLocalFrame(env, h2o(rd32(&R)))));
        return 1;
    case S_GET_STRING_CHARS: {
        jstring s = (jstring)h2o(rd32(&R));
        uint32_t is_copy = rd32(&R);
        jsize n = (*env)->GetStringLength(env, s);
        uint32_t g = guest_malloc(2u * (uint32_t)n + 2);
        if (g) {
            (*env)->GetStringRegion(env, s, 0, n, g2p(g));
            st16(g + 2u * (uint32_t)n, 0);
        }
        set_is_copy(is_copy, 1);
        ret32(c, g);
        return 1;
    }
    case S_GET_STRING_UTF: {
        jstring s = (jstring)h2o(rd32(&R));
        uint32_t is_copy = rd32(&R);
        const char *p = (*env)->GetStringUTFChars(env, s, NULL);
        uint32_t g = p ? guest_strdup(p) : 0;
        if (p) (*env)->ReleaseStringUTFChars(env, s, p);
        set_is_copy(is_copy, 1);
        ret32(c, g);
        return 1;
    }
    case S_REL_STRING_CHARS:
    case S_REL_STRING_UTF:
        rd32(&R);
        guest_free(rd32(&R));
        return 1;
    case S_GET_ELEMS: {
        jarray a = (jarray)h2o(rd32(&R));
        uint32_t is_copy = rd32(&R);
        ret32(c, a ? get_elems(env, fn[index].type, a, is_copy) : 0);
        return 1;
    }
    case S_REL_ELEMS: {
        jarray a = (jarray)h2o(rd32(&R));
        uint32_t g = rd32(&R);
        jint mode = (jint)rd32(&R);
        release_elems(env, fn[index].type, a, g, mode);
        return 1;
    }
    case S_GET_CRITICAL: {
        jarray a = (jarray)h2o(rd32(&R));
        uint32_t is_copy = rd32(&R);
        void *p = a ? (*env)->GetPrimitiveArrayCritical(env, a, NULL) : NULL;
        if (!p) {
            ret32(c, 0);
            return 1;
        }
        if (in_guest_reach(p, 1)) {
            set_is_copy(is_copy, 0);
            ret32(c, p2g(p));
            return 1;
        }
        /* outside the guest's reach: copy (the size needs the class, which can't be asked for in a critical
         * region) */
        (*env)->ReleasePrimitiveArrayCritical(env, a, p, JNI_ABORT);
        uint32_t size = (uint32_t)(*env)->GetArrayLength(env, a) * (uint32_t)array_elem_size(env, a);
        uint32_t g = guest_malloc(size ? size : 1);
        p = (*env)->GetPrimitiveArrayCritical(env, a, NULL);
        if (g && p) memcpy(g2p(g), p, size);
        if (p) (*env)->ReleasePrimitiveArrayCritical(env, a, p, JNI_ABORT);
        copy_add(g, size);
        set_is_copy(is_copy, 1);
        ret32(c, g);
        return 1;
    }
    case S_REL_CRITICAL: {
        jarray a = (jarray)h2o(rd32(&R));
        uint32_t g = rd32(&R);
        jint mode = (jint)rd32(&R);
        int64_t size = copy_find(g, mode == JNI_COMMIT);
        if (size >= 0) {
            if (mode != JNI_ABORT) write_back(env, a, g, (uint32_t)size);
            if (mode != JNI_COMMIT) guest_free(g);
        } else {
            (*env)->ReleasePrimitiveArrayCritical(env, a, g2p(g), mode);
        }
        return 1;
    }
    case S_REGISTER:
        register_natives(c, env, &R);
        return 1;
    case S_GET_VM:
        st32(rd32(&R), g_info.java_vm);
        ret32(c, 0);
        return 1;
    case S_DIRECT_ADDR: {
        void *p = (*env)->GetDirectBufferAddress(env, h2o(rd32(&R)));
        if (p && !in_guest_reach(p, 1)) {
            bridge_log(ANDROID_LOG_ERROR, "GetDirectBufferAddress: buffer at %p is outside the guest's reach", p);
            p = NULL;
        }
        ret32(c, p2g(p));
        return 1;
    }
    }
    return 0;
}

/* JNIInvokeInterface: reserved0-2, DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv,
 * AttachCurrentThreadAsDaemon */
int vm_dispatch(a32_cpu *c, int index)
{
    argreader R;
    ar_init(&R, c, 1, c->r[13] + 8);
    switch (index) {
    case 4:
    case 7: {
        uint32_t penv = rd32(&R), args = rd32(&R);
        JavaVMAttachArgs a = {JNI_VERSION_1_6, NULL, NULL}, *ap = NULL;
        if (args) {
            a.version = (jint)ld32(args);
            a.name = ld32(args + 4) ? g2p(ld32(args + 4)) : NULL;
            a.group = h2o(ld32(args + 8));
            ap = &a;
        }
        JNIEnv *env = NULL;
        jint r = index == 4 ? (*g_vm)->AttachCurrentThread(g_vm, &env, ap)
                            : (*g_vm)->AttachCurrentThreadAsDaemon(g_vm, &env, ap);
        if (penv) st32(penv, r == JNI_OK ? g_info.jni_env : 0);
        ret32(c, (uint32_t)r);
        return 1;
    }
    case 5:
        ret32(c, (uint32_t)(*g_vm)->DetachCurrentThread(g_vm));
        return 1;
    case 6: {
        uint32_t penv = rd32(&R);
        jint version = (jint)rd32(&R);
        JNIEnv *env = NULL;
        jint r = (*g_vm)->GetEnv(g_vm, (void **)&env, version);
        if (penv) st32(penv, r == JNI_OK ? g_info.jni_env : 0);
        ret32(c, (uint32_t)r);
        return 1;
    }
    default:
        ret32(c, (uint32_t)JNI_ERR);
        return 1;
    }
}
