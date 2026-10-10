/* Fake JVM (see fakejni.h). Objects are never freed; references are the object pointers. Only what the bridge and
 * the test libraries use is implemented: everything else aborts with the function's index. */
#define _GNU_SOURCE
#include "fakejni.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { O_CLASS = 1, O_STRING, O_ARRAY, O_OBJECT, O_METHOD };

struct fclass {
    char name[256]; /* dotted, as Class.getName() returns it */
    fmethod *methods;
    int nmethods;
};

typedef struct fobj {
    int kind;
    fclass *cls;
    union {
        fclass *klass;  /* O_CLASS */
        char *utf;      /* O_STRING */
        fmethod *m;     /* O_METHOD */
        int field;      /* O_OBJECT */
        struct { char type; int len; void *data; } arr;
    };
} fobj;

static fclass *classes[256];
static int nclasses;
static __thread int attached;
static __thread char exc[256];
static __thread int exc_pending;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define O(x) ((fobj *)(x))

static void dotted(const char *in, char *out, size_t n)
{
    snprintf(out, n, "%s", in);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '.';
}

static fclass *find_class(const char *dname)
{
    for (int k = 0; k < nclasses; k++)
        if (!strcmp(classes[k]->name, dname)) return classes[k];
    return NULL;
}

static fclass *get_class(const char *dname)
{
    pthread_mutex_lock(&lock);
    fclass *c = find_class(dname);
    if (!c) {
        c = calloc(1, sizeof *c);
        snprintf(c->name, sizeof c->name, "%s", dname);
        classes[nclasses++] = c;
    }
    pthread_mutex_unlock(&lock);
    return c;
}

static fobj *class_obj(fclass *k)
{
    fobj *o = calloc(1, sizeof *o);
    o->kind = O_CLASS;
    o->cls = get_class("java.lang.Class");
    o->klass = k;
    return o;
}

fclass *fake_class(const char *name, const fmethod *methods)
{
    char d[256];
    dotted(name, d, sizeof d);
    fclass *c = get_class(d);
    int n = 0;
    while (methods && methods[n].name) n++;
    c->methods = calloc((size_t)n + 1, sizeof *c->methods);
    memcpy(c->methods, methods, (size_t)n * sizeof *methods);
    c->nmethods = n;
    for (int k = 0; k < n; k++) c->methods[k].cls = c;
    return c;
}

fmethod *fake_method(fclass *c, const char *name, const char *sig)
{
    for (int k = 0; k < c->nmethods; k++)
        if (!strcmp(c->methods[k].name, name) && (!sig || !strcmp(c->methods[k].sig, sig))) return &c->methods[k];
    return NULL;
}

jstring fake_string(const char *utf)
{
    fobj *o = calloc(1, sizeof *o);
    o->kind = O_STRING;
    o->cls = get_class("java.lang.String");
    o->utf = strdup(utf);
    return (jstring)o;
}

const char *fake_utf(jstring s) { return s ? O(s)->utf : NULL; }

static int esize(char t) { return t == 'Z' || t == 'B' ? 1 : t == 'C' || t == 'S' ? 2 : t == 'J' || t == 'D' ? 8 : 4; }

/* "heap" memory below 4 GiB, as ART's */
static void *low_alloc(size_t n)
{
    static uint8_t *arena;
    static size_t used;
    pthread_mutex_lock(&lock);
    if (!arena) {
        arena = mmap((void *)0x30000000, 16 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1, 0);
        if (arena == MAP_FAILED) abort();
    }
    void *p = arena + used;
    used += (n + 15) & ~(size_t)15;
    pthread_mutex_unlock(&lock);
    return p;
}

static void *high_alloc(size_t n)
{
    void *p = mmap((void *)0x7100000000, (n + 4095) & ~(size_t)4095, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p < 0x100000000ull) {
        fprintf(stderr, "fake JNI: no memory above 4 GiB (%p)\n", p);
        abort();
    }
    return p;
}

jarray fake_array(char type, int len, int low)
{
    fobj *o = calloc(1, sizeof *o);
    o->kind = O_ARRAY;
    char cn[256];
    if (type == 'L') snprintf(cn, sizeof cn, "[Ljava.lang.Object;");
    else snprintf(cn, sizeof cn, "[%c", type);
    o->cls = get_class(cn);
    o->arr.type = type;
    o->arr.len = len;
    size_t n = (size_t)len * (type == 'L' ? sizeof(void *) : (size_t)esize(type));
    o->arr.data = low ? low_alloc(n ? n : 1) : high_alloc(n ? n : 1);
    memset(o->arr.data, 0, n);
    return (jarray)o;
}

void *fake_array_data(jarray a) { return O(a)->arr.data; }
int fake_get_int_field(jobject o) { return O(o)->field; }

const char *fake_take_exception(void)
{
    if (!exc_pending) return NULL;
    exc_pending = 0;
    return exc;
}

static void throw(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(exc, sizeof exc, fmt, ap);
    va_end(ap);
    exc_pending = 1;
}

/* ---------------------------------------------------------------- reflection (java.lang.Class / Method) */

static jvalue class_get_name(JNIEnv *env, jobject self, const jvalue *a)
{
    (void)env, (void)a;
    jvalue r;
    r.l = fake_string(O(self)->klass->name);
    return r;
}

/* the class object of one descriptor at *p (advances it) */
static fobj *descriptor_class(const char **p)
{
    static const char *const prim[] = {"boolean", "byte", "char", "short", "int", "long", "float", "double", "void"};
    static const char letters[] = "ZBCSIJFDV";
    char name[256];
    const char *s = *p;
    if (*s == '[') {
        const char *e = s;
        while (*e == '[') e++;
        e = *e == 'L' ? strchr(e, ';') + 1 : e + 1;
        snprintf(name, sizeof name, "%.*s", (int)(e - s), s);
        for (char *q = name; *q; q++)
            if (*q == '/') *q = '.';
        *p = e;
    } else if (*s == 'L') {
        const char *e = strchr(s, ';');
        snprintf(name, sizeof name, "%.*s", (int)(e - s - 1), s + 1);
        for (char *q = name; *q; q++)
            if (*q == '/') *q = '.';
        *p = e + 1;
    } else {
        snprintf(name, sizeof name, "%s", prim[strchr(letters, *s) - letters]);
        *p = s + 1;
    }
    return class_obj(get_class(name));
}

static jvalue class_get_declared_methods(JNIEnv *env, jobject self, const jvalue *a)
{
    (void)env, (void)a;
    fclass *k = O(self)->klass;
    jarray arr = fake_array('L', k->nmethods, 0);
    O(arr)->cls = get_class("[Ljava.lang.reflect.Method;");
    for (int i = 0; i < k->nmethods; i++) {
        fobj *m = calloc(1, sizeof *m);
        m->kind = O_METHOD;
        m->cls = get_class("java.lang.reflect.Method");
        m->m = &k->methods[i];
        ((fobj **)O(arr)->arr.data)[i] = m;
    }
    jvalue r;
    r.l = arr;
    return r;
}

static jvalue method_get_name(JNIEnv *env, jobject self, const jvalue *a)
{
    (void)env, (void)a;
    jvalue r;
    r.l = fake_string(O(self)->m->name);
    return r;
}

static jvalue method_get_modifiers(JNIEnv *env, jobject self, const jvalue *a)
{
    (void)env, (void)a;
    jvalue r;
    r.i = (O(self)->m->is_native ? 0x100 : 0) | (O(self)->m->is_static ? 0x8 : 0) | 1;
    return r;
}

static jvalue method_get_parameter_types(JNIEnv *env, jobject self, const jvalue *a)
{
    (void)env, (void)a;
    const char *p = O(self)->m->sig + 1;
    fobj *tmp[64];
    int n = 0;
    while (*p != ')') tmp[n++] = descriptor_class(&p);
    jarray arr = fake_array('L', n, 0);
    O(arr)->cls = get_class("[Ljava.lang.Class;");
    memcpy(O(arr)->arr.data, tmp, (size_t)n * sizeof *tmp);
    jvalue r;
    r.l = arr;
    return r;
}

static jvalue method_get_return_type(JNIEnv *env, jobject self, const jvalue *a)
{
    (void)env, (void)a;
    const char *p = strchr(O(self)->m->sig, ')') + 1;
    jvalue r;
    r.l = descriptor_class(&p);
    return r;
}

/* ---------------------------------------------------------------- JNIEnv */

static jint GetVersion(JNIEnv *env) { (void)env; return JNI_VERSION_1_6; }

static jclass FindClass(JNIEnv *env, const char *name)
{
    (void)env;
    char d[256];
    dotted(name, d, sizeof d);
    pthread_mutex_lock(&lock);
    fclass *c = find_class(d);
    pthread_mutex_unlock(&lock);
    if (!c) {
        throw("java.lang.NoClassDefFoundError: %s", name);
        return NULL;
    }
    return (jclass)class_obj(c);
}

static jclass GetObjectClass(JNIEnv *env, jobject o) { (void)env; return (jclass)class_obj(O(o)->cls); }
static jboolean IsInstanceOf(JNIEnv *env, jobject o, jclass k) { (void)env; return o && O(o)->cls == O(k)->klass; }
static jboolean IsSameObject(JNIEnv *env, jobject a, jobject b) { (void)env; return a == b; }
static jobject NewRef(JNIEnv *env, jobject o) { (void)env; return o; }
static void DeleteRef(JNIEnv *env, jobject o) { (void)env, (void)o; }
static jint PushLocalFrame(JNIEnv *env, jint n) { (void)env, (void)n; return 0; }
static jobject PopLocalFrame(JNIEnv *env, jobject o) { (void)env; return o; }
static jint EnsureLocalCapacity(JNIEnv *env, jint n) { (void)env, (void)n; return 0; }

static jboolean ExceptionCheck(JNIEnv *env) { (void)env; return (jboolean)exc_pending; }
static void ExceptionClear(JNIEnv *env) { (void)env; exc_pending = 0; }
static jthrowable ExceptionOccurred(JNIEnv *env) { (void)env; return exc_pending ? (jthrowable)fake_string(exc) : NULL; }
static void ExceptionDescribe(JNIEnv *env) { (void)env; if (exc_pending) fprintf(stderr, "fake JNI exception: %s\n", exc); }
static jint ThrowNew(JNIEnv *env, jclass k, const char *msg)
{
    (void)env;
    throw("%s: %s", O(k)->klass->name, msg);
    return 0;
}

static jmethodID GetMethodID(JNIEnv *env, jclass k, const char *name, const char *sig)
{
    (void)env;
    fmethod *m = fake_method(O(k)->klass, name, sig);
    if (!m || m->is_static) {
        throw("java.lang.NoSuchMethodError: %s%s", name, sig);
        return NULL;
    }
    return (jmethodID)m;
}

static jmethodID GetStaticMethodID(JNIEnv *env, jclass k, const char *name, const char *sig)
{
    (void)env;
    fmethod *m = fake_method(O(k)->klass, name, sig);
    if (!m || !m->is_static) {
        throw("java.lang.NoSuchMethodError: %s%s", name, sig);
        return NULL;
    }
    return (jmethodID)m;
}

static jfieldID GetFieldID(JNIEnv *env, jclass k, const char *name, const char *sig)
{
    (void)env, (void)k, (void)sig;
    return strcmp(name, "value") ? NULL : (jfieldID)1;
}
static jint GetIntField(JNIEnv *env, jobject o, jfieldID f) { (void)env, (void)f; return O(o)->field; }
static void SetIntField(JNIEnv *env, jobject o, jfieldID f, jint v) { (void)env, (void)f; O(o)->field = v; }

/* calls a Java method (impl) or a native one (through its registered pointer, ART style) */
jvalue fake_invoke(JNIEnv *env, fmethod *m, jobject self, const jvalue *a);

static jvalue call(JNIEnv *env, jobject self, jmethodID mid, const jvalue *a)
{
    fmethod *m = (fmethod *)mid;
    if (!m) {
        fprintf(stderr, "fake JNI: call with a NULL method ID\n");
        abort();
    }
    return fake_invoke(env, m, self, a);
}

/* "..." forms: used by the bridge only for methods without arguments */
static jobject CallObjectMethod(JNIEnv *env, jobject o, jmethodID m, ...)
{
    if (((fmethod *)m)->sig[1] != ')') abort();
    return call(env, o, m, NULL).l;
}
static jint CallIntMethod(JNIEnv *env, jobject o, jmethodID m, ...)
{
    if (((fmethod *)m)->sig[1] != ')') abort();
    return call(env, o, m, NULL).i;
}

static jobject CallObjectMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).l; }
static jboolean CallBooleanMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).z; }
static jbyte CallByteMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).b; }
static jchar CallCharMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).c; }
static jshort CallShortMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).s; }
static jint CallIntMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).i; }
static jlong CallLongMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).j; }
static jfloat CallFloatMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).f; }
static jdouble CallDoubleMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { return call(env, o, m, a).d; }
static void CallVoidMethodA(JNIEnv *env, jobject o, jmethodID m, const jvalue *a) { call(env, o, m, a); }
static jobject CallStaticObjectMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; return call(env, NULL, m, a).l; }
static jint CallStaticIntMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; return call(env, NULL, m, a).i; }
static jlong CallStaticLongMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; return call(env, NULL, m, a).j; }
static jfloat CallStaticFloatMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; return call(env, NULL, m, a).f; }
static jdouble CallStaticDoubleMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; return call(env, NULL, m, a).d; }
static void CallStaticVoidMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; call(env, NULL, m, a); }
static jboolean CallStaticBooleanMethodA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a) { (void)k; return call(env, NULL, m, a).z; }

static jobject NewObjectA(JNIEnv *env, jclass k, jmethodID m, const jvalue *a)
{
    fobj *o = calloc(1, sizeof *o);
    o->kind = O_OBJECT;
    o->cls = O(k)->klass;
    call(env, (jobject)o, m, a);
    return (jobject)o;
}

static jstring NewStringUTF(JNIEnv *env, const char *s) { (void)env; return fake_string(s); }
static jsize GetStringUTFLength(JNIEnv *env, jstring s) { (void)env; return (jsize)strlen(O(s)->utf); }
static jsize GetStringLength(JNIEnv *env, jstring s) { (void)env; return (jsize)strlen(O(s)->utf); } /* ASCII only */
static void GetStringRegion(JNIEnv *env, jstring s, jsize start, jsize len, jchar *buf)
{
    (void)env;
    for (jsize k = 0; k < len; k++) buf[k] = (jchar)O(s)->utf[start + k];
}
static const char *GetStringUTFChars(JNIEnv *env, jstring s, jboolean *is_copy)
{
    (void)env;
    if (is_copy) *is_copy = JNI_TRUE;
    char *p = high_alloc(strlen(O(s)->utf) + 1); /* malloc'd copies live high on Android */
    strcpy(p, O(s)->utf);
    return p;
}
static void ReleaseStringUTFChars(JNIEnv *env, jstring s, const char *p) { (void)env, (void)s, (void)p; }

static jsize GetArrayLength(JNIEnv *env, jarray a) { (void)env; return O(a)->arr.len; }
static jobject GetObjectArrayElement(JNIEnv *env, jobjectArray a, jsize i) { (void)env; return ((jobject *)O(a)->arr.data)[i]; }
static void SetObjectArrayElement(JNIEnv *env, jobjectArray a, jsize i, jobject v) { (void)env; ((jobject *)O(a)->arr.data)[i] = v; }
static jintArray NewIntArray(JNIEnv *env, jsize n) { (void)env; return (jintArray)fake_array('I', n, 1); }
static void *GetElements(JNIEnv *env, jarray a, jboolean *is_copy)
{
    (void)env;
    if (is_copy) *is_copy = JNI_FALSE;
    return O(a)->arr.data;
}
static void ReleaseElements(JNIEnv *env, jarray a, void *p, jint mode) { (void)env, (void)a, (void)p, (void)mode; }
static void GetIntArrayRegion(JNIEnv *env, jintArray a, jsize s, jsize n, jint *buf) { (void)env; memcpy(buf, (jint *)O(a)->arr.data + s, (size_t)n * 4); }
static void SetIntArrayRegion(JNIEnv *env, jintArray a, jsize s, jsize n, const jint *buf) { (void)env; memcpy((jint *)O(a)->arr.data + s, buf, (size_t)n * 4); }

static jint RegisterNatives(JNIEnv *env, jclass k, const JNINativeMethod *ms, jint n)
{
    (void)env;
    for (jint i = 0; i < n; i++) {
        fmethod *m = fake_method(O(k)->klass, ms[i].name, ms[i].signature);
        if (!m || !m->is_native) {
            throw("java.lang.NoSuchMethodError: no native method %s%s", ms[i].name, ms[i].signature);
            return JNI_ERR;
        }
        m->native_fn = ms[i].fnPtr;
    }
    return JNI_OK;
}

static jint GetJavaVM(JNIEnv *env, JavaVM **vm) { (void)env; *vm = fake_vm(); return 0; }
static void *GetDirectBufferAddress(JNIEnv *env, jobject b) { (void)env; return O(b)->arr.data; }
static jlong GetDirectBufferCapacity(JNIEnv *env, jobject b) { (void)env; return O(b)->arr.len; }

static struct JNINativeInterface env_table;
static JNIEnv env_obj = &env_table;

/* every function not implemented here: "mov w0, #index; b fake_unimplemented" */
__asm__(".text\n.p2align 3\nfake_unimpl_table:\n.set i, 0\n.rept 233\nmov w0, #i\nb fake_unimplemented\n"
        ".set i, i + 1\n.endr\n");
extern const char fake_unimpl_table[];

__attribute__((used)) void fake_unimplemented(int index)
{
    fprintf(stderr, "fake JNI: JNIEnv function %d is not implemented\n", index);
    abort();
}

static void init_env(void)
{
    struct JNINativeInterface *t = &env_table;
    void **slots = (void **)t;
    for (int k = 0; k < 233; k++) slots[k] = (void *)(fake_unimpl_table + 8 * k);
    t->GetVersion = GetVersion;
    t->FindClass = FindClass;
    t->GetObjectClass = GetObjectClass;
    t->IsInstanceOf = IsInstanceOf;
    t->IsSameObject = IsSameObject;
    t->NewGlobalRef = NewRef;
    t->NewLocalRef = NewRef;
    t->NewWeakGlobalRef = NewRef;
    t->DeleteGlobalRef = DeleteRef;
    t->DeleteLocalRef = DeleteRef;
    t->DeleteWeakGlobalRef = DeleteRef;
    t->PushLocalFrame = PushLocalFrame;
    t->PopLocalFrame = PopLocalFrame;
    t->EnsureLocalCapacity = EnsureLocalCapacity;
    t->ExceptionCheck = ExceptionCheck;
    t->ExceptionClear = ExceptionClear;
    t->ExceptionOccurred = ExceptionOccurred;
    t->ExceptionDescribe = ExceptionDescribe;
    t->ThrowNew = ThrowNew;
    t->GetMethodID = GetMethodID;
    t->GetStaticMethodID = GetStaticMethodID;
    t->GetFieldID = GetFieldID;
    t->GetIntField = GetIntField;
    t->SetIntField = SetIntField;
    t->CallObjectMethod = CallObjectMethod;
    t->CallIntMethod = CallIntMethod;
    t->CallObjectMethodA = CallObjectMethodA;
    t->CallBooleanMethodA = CallBooleanMethodA;
    t->CallByteMethodA = CallByteMethodA;
    t->CallCharMethodA = CallCharMethodA;
    t->CallShortMethodA = CallShortMethodA;
    t->CallIntMethodA = CallIntMethodA;
    t->CallLongMethodA = CallLongMethodA;
    t->CallFloatMethodA = CallFloatMethodA;
    t->CallDoubleMethodA = CallDoubleMethodA;
    t->CallVoidMethodA = CallVoidMethodA;
    t->CallStaticObjectMethodA = CallStaticObjectMethodA;
    t->CallStaticBooleanMethodA = CallStaticBooleanMethodA;
    t->CallStaticIntMethodA = CallStaticIntMethodA;
    t->CallStaticLongMethodA = CallStaticLongMethodA;
    t->CallStaticFloatMethodA = CallStaticFloatMethodA;
    t->CallStaticDoubleMethodA = CallStaticDoubleMethodA;
    t->CallStaticVoidMethodA = CallStaticVoidMethodA;
    t->NewObjectA = NewObjectA;
    t->NewStringUTF = NewStringUTF;
    t->GetStringUTFLength = GetStringUTFLength;
    t->GetStringLength = GetStringLength;
    t->GetStringRegion = GetStringRegion;
    t->GetStringUTFChars = GetStringUTFChars;
    t->ReleaseStringUTFChars = ReleaseStringUTFChars;
    t->GetArrayLength = GetArrayLength;
    t->GetObjectArrayElement = GetObjectArrayElement;
    t->SetObjectArrayElement = SetObjectArrayElement;
    t->NewIntArray = NewIntArray;
    t->GetIntArrayElements = (void *)GetElements;
    t->ReleaseIntArrayElements = (void *)ReleaseElements;
    t->GetFloatArrayElements = (void *)GetElements;
    t->ReleaseFloatArrayElements = (void *)ReleaseElements;
    t->GetByteArrayElements = (void *)GetElements;
    t->ReleaseByteArrayElements = (void *)ReleaseElements;
    t->GetPrimitiveArrayCritical = (void *)GetElements;
    t->ReleasePrimitiveArrayCritical = (void *)ReleaseElements;
    t->GetIntArrayRegion = GetIntArrayRegion;
    t->SetIntArrayRegion = SetIntArrayRegion;
    t->RegisterNatives = RegisterNatives;
    t->GetJavaVM = GetJavaVM;
    t->GetDirectBufferAddress = GetDirectBufferAddress;
    t->GetDirectBufferCapacity = GetDirectBufferCapacity;

    static const fmethod class_methods[] = {
        {"getName", "()Ljava/lang/String;", 0, 0, class_get_name, NULL, NULL},
        {"getDeclaredMethods", "()[Ljava/lang/reflect/Method;", 0, 0, class_get_declared_methods, NULL, NULL},
        {NULL, NULL, 0, 0, NULL, NULL, NULL},
    };
    static const fmethod method_methods[] = {
        {"getName", "()Ljava/lang/String;", 0, 0, method_get_name, NULL, NULL},
        {"getModifiers", "()I", 0, 0, method_get_modifiers, NULL, NULL},
        {"getParameterTypes", "()[Ljava/lang/Class;", 0, 0, method_get_parameter_types, NULL, NULL},
        {"getReturnType", "()Ljava/lang/Class;", 0, 0, method_get_return_type, NULL, NULL},
        {NULL, NULL, 0, 0, NULL, NULL, NULL},
    };
    fake_class("java/lang/Class", class_methods);
    fake_class("java/lang/reflect/Method", method_methods);
    fake_class("java/lang/String", NULL);
    fake_class("java/lang/IllegalStateException", NULL);
}

JNIEnv *fake_env(void) { return &env_obj; }

/* ---------------------------------------------------------------- JavaVM */

static jint vm_get_env(JavaVM *vm, void **env, jint version)
{
    (void)vm, (void)version;
    if (!attached) {
        *env = NULL;
        return JNI_EDETACHED;
    }
    *env = &env_obj;
    return JNI_OK;
}

static jint vm_attach(JavaVM *vm, JNIEnv **env, void *args)
{
    (void)vm, (void)args;
    attached = 1;
    *env = &env_obj;
    return JNI_OK;
}

static jint vm_detach(JavaVM *vm)
{
    (void)vm;
    attached = 0;
    return JNI_OK;
}

static struct JNIInvokeInterface vm_table = {
    .GetEnv = vm_get_env,
    .AttachCurrentThread = vm_attach,
    .AttachCurrentThreadAsDaemon = vm_attach,
    .DetachCurrentThread = vm_detach,
};
static JavaVM vm_obj = &vm_table;

JavaVM *fake_vm(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_env);
    return &vm_obj;
}

void fake_attach(void) { attached = 1; }
