/* Guest test library (armv7, Thumb): what a game's JNI library does. JNI_OnLoad registers natives with
 * RegisterNatives; Java_* exports are found by name. The natives call back into "Java" with every argument style. */
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "btest"

static JavaVM *g_vm;
static jclass g_cls;
static jmethodID m_add3, m_mix, m_callback, m_ctor, m_greet;

static jint nativeAdd(JNIEnv *env, jclass c, jint a, jint b) { return a + b; }

/* 12 arguments: host-side some go on the stack (gpr and fpr overflow differ from ARM's) */
static jlong nativeMany(JNIEnv *env, jclass c, jlong a, jdouble b, jfloat f, jboolean z, jbyte by, jchar ch,
                        jshort s, jint i, jlong l2, jdouble d2, jfloat f2, jint i2)
{
    return a + (jlong)(b * 1000) + (jlong)(f * 100) + z + by + ch + s + i + l2 + (jlong)(d2 * 10) + (jlong)(f2 * 10) +
           i2;
}

static jfloat nativeFloat(JNIEnv *env, jclass c, jfloat a, jfloat b) { return a * b; }
static jdouble nativeDouble(JNIEnv *env, jclass c, jdouble a, jdouble b) { return a / b; }
static jbyte nativeByte(JNIEnv *env, jclass c, jbyte a) { return (jbyte)(a - 1); }

static jstring nativeConcat(JNIEnv *env, jclass c, jstring s)
{
    const char *u = (*env)->GetStringUTFChars(env, s, NULL);
    char buf[128];
    snprintf(buf, sizeof buf, "%s-%d", u, (int)(*env)->GetStringUTFLength(env, s));
    (*env)->ReleaseStringUTFChars(env, s, u);
    return (*env)->NewStringUTF(env, buf);
}

/* sums the array and writes element 0 back (shared or copied, both must work) */
static jint nativeSum(JNIEnv *env, jclass c, jintArray a)
{
    jint n = (*env)->GetArrayLength(env, a), sum = 0;
    jint *p = (*env)->GetIntArrayElements(env, a, NULL);
    for (jint k = 0; k < n; k++) sum += p[k];
    p[0] = 99;
    (*env)->ReleaseIntArrayElements(env, a, p, 0);
    return sum;
}

static jint call_v(JNIEnv *env, jclass c, jmethodID m, ...)
{
    va_list ap;
    va_start(ap, m);
    jint r = (*env)->CallStaticIntMethodV(env, c, m, ap);
    va_end(ap);
    return r;
}

/* calls Java in every style; returns a bit per passed check */
static jint nativeCallJava(JNIEnv *env, jclass c)
{
    jint ok = 0;
    if ((*env)->CallStaticIntMethod(env, c, m_add3, 1, 2, 3) == 6) ok |= 1;
    jdouble d = (*env)->CallStaticDoubleMethod(env, c, m_mix, 4, (jlong)5000000000LL, (jfloat)1.5f, 2.25,
                                               (*env)->NewStringUTF(env, "abc"));
    if (d == 4 + 5000000000.0 + 1.5 + 2.25 + 3) ok |= 2;
    jvalue v[3];
    v[0].i = 10, v[1].i = 20, v[2].i = 30;
    if ((*env)->CallStaticIntMethodA(env, c, m_add3, v) == 60) ok |= 4;
    if (call_v(env, c, m_add3, 7, 8, 9) == 24) ok |= 8;
    jobject o = (*env)->NewObject(env, c, m_ctor, 41);
    jfieldID f = (*env)->GetFieldID(env, c, "value", "I");
    if (o && (*env)->GetIntField(env, o, f) == 41) ok |= 16;
    jstring g = (*env)->CallObjectMethod(env, o, m_greet, (*env)->NewStringUTF(env, "abc"));
    const char *u = g ? (*env)->GetStringUTFChars(env, g, NULL) : NULL;
    if (u && !strcmp(u, "hello abc/41")) ok |= 32;
    if (u) (*env)->ReleaseStringUTFChars(env, g, u);
    jintArray arr = (*env)->NewIntArray(env, 4);
    jint in[4] = {1, 2, 3, 4}, out[4];
    (*env)->SetIntArrayRegion(env, arr, 0, 4, in);
    (*env)->GetIntArrayRegion(env, arr, 1, 3, out);
    if (out[0] == 2 && out[2] == 4) ok |= 64;
    return ok;
}

/* Java -> native -> Java -> native: nested calls on one thread */
static jint nativeNested(JNIEnv *env, jclass c, jint n) { return (*env)->CallStaticIntMethod(env, c, m_callback, n) + 1; }
static jint nativeTwice(JNIEnv *env, jclass c, jint n) { return 2 * n; }

static void *thread_fn(void *arg)
{
    JNIEnv *env = NULL;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return (void *)(intptr_t)-1;
    jint r = (*env)->CallStaticIntMethod(env, g_cls, m_add3, 100, 20, 3);
    (*g_vm)->DetachCurrentThread(g_vm);
    return (void *)(intptr_t)r;
}

/* a guest thread attaches itself and calls Java */
static jint nativeThread(JNIEnv *env, jclass c)
{
    pthread_t t;
    void *r = NULL;
    if (pthread_create(&t, NULL, thread_fn, NULL)) return -1;
    pthread_join(t, &r);
    return (jint)(intptr_t)r;
}

static jint nativeThrow(JNIEnv *env, jclass c)
{
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"), "boom");
    return 7;
}

JNIEXPORT jint JNICALL Java_com_example_Bridge_exported(JNIEnv *env, jclass c, jint x) { return x * 3; }
JNIEXPORT jlong JNICALL Java_com_example_Bridge_over__I(JNIEnv *env, jclass c, jint x) { return x + 1; }
JNIEXPORT jlong JNICALL Java_com_example_Bridge_over__J(JNIEnv *env, jclass c, jlong x) { return x + 2; }
JNIEXPORT jint JNICALL Java_com_example_Bridge_under_1score(JNIEnv *env, jclass c) { return 5; }

int btest_helper(int x) { return x ^ 0x55; }

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    g_vm = vm;
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass c = (*env)->FindClass(env, "com/example/Bridge");
    if (!c) return JNI_ERR;
    g_cls = (*env)->NewGlobalRef(env, c);
    m_add3 = (*env)->GetStaticMethodID(env, c, "add3", "(III)I");
    m_mix = (*env)->GetStaticMethodID(env, c, "mix", "(IJFDLjava/lang/String;)D");
    m_callback = (*env)->GetStaticMethodID(env, c, "callBack", "(I)I");
    m_ctor = (*env)->GetMethodID(env, c, "<init>", "(I)V");
    m_greet = (*env)->GetMethodID(env, c, "greet", "(Ljava/lang/String;)Ljava/lang/String;");
    static const JNINativeMethod natives[] = {
        {"nativeAdd", "(II)I", (void *)nativeAdd},
        {"nativeMany", "(JDFZBCSIJDFI)J", (void *)nativeMany},
        {"nativeFloat", "(FF)F", (void *)nativeFloat},
        {"nativeDouble", "(DD)D", (void *)nativeDouble},
        {"nativeByte", "(B)B", (void *)nativeByte},
        {"nativeConcat", "(Ljava/lang/String;)Ljava/lang/String;", (void *)nativeConcat},
        {"nativeSum", "([I)I", (void *)nativeSum},
        {"nativeCallJava", "()I", (void *)nativeCallJava},
        {"nativeNested", "(I)I", (void *)nativeNested},
        {"nativeTwice", "(I)I", (void *)nativeTwice},
        {"nativeThread", "()I", (void *)nativeThread},
        {"nativeThrow", "()I", (void *)nativeThrow},
    };
    if ((*env)->RegisterNatives(env, c, natives, sizeof natives / sizeof *natives)) return JNI_ERR;
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad done (%s)", m_add3 && m_mix ? "methods found" : "?");
    return JNI_VERSION_1_6;
}
