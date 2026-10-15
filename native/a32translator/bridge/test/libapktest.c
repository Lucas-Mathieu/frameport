/* Guest NativeActivity for the on-device test APK (make_apk.py): checks the bridge against the real ART in a
 * Lepton container and logs "A32TEST ok|FAIL <check>" lines (logcat tag a32test), then "A32TEST done N failed". */
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "a32test", __VA_ARGS__)

static int failed;
static JavaVM *g_vm;

static void check(int ok, const char *what, const char *detail)
{
    if (!ok) failed++;
    LOG("A32TEST %s %s%s%s", ok ? "ok" : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

static jstring static_string(JNIEnv *env, const char *cls, const char *field)
{
    jclass c = (*env)->FindClass(env, cls);
    if (!c) return NULL;
    jfieldID f = (*env)->GetStaticFieldID(env, c, field, "Ljava/lang/String;");
    return f ? (jstring)(*env)->GetStaticObjectField(env, c, f) : NULL;
}

static void *thread_fn(void *arg)
{
    JNIEnv *env = NULL;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return (void *)0;
    jclass c = (*env)->FindClass(env, "android/os/SystemClock");
    jmethodID m = c ? (*env)->GetStaticMethodID(env, c, "uptimeMillis", "()J") : NULL;
    jlong t = m ? (*env)->CallStaticLongMethod(env, c, m) : 0;
    (*g_vm)->DetachCurrentThread(g_vm);
    return (void *)(intptr_t)(t > 0);
}

static void run_checks(ANativeActivity *a)
{
    JNIEnv *env = NULL;
    g_vm = a->vm;
    check((*a->vm)->GetEnv(a->vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && env == a->env, "GetEnv", NULL);

    jstring model = static_string(env, "android/os/Build", "MODEL");
    const char *u = model ? (*env)->GetStringUTFChars(env, model, NULL) : NULL;
    check(u && *u, "Build.MODEL", u);
    if (u) (*env)->ReleaseStringUTFChars(env, model, u);

    /* String.format("%d-%s", 42, "x") through a varargs Call with an Object[] built in JNI */
    jclass sc = (*env)->FindClass(env, "java/lang/String");
    jclass ic = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID valueof = (*env)->GetStaticMethodID(env, ic, "valueOf", "(I)Ljava/lang/Integer;");
    jmethodID format = (*env)->GetStaticMethodID(env, sc, "format", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;");
    jobjectArray args = (*env)->NewObjectArray(env, 2, (*env)->FindClass(env, "java/lang/Object"), NULL);
    (*env)->SetObjectArrayElement(env, args, 0, (*env)->CallStaticObjectMethod(env, ic, valueof, 42));
    (*env)->SetObjectArrayElement(env, args, 1, (*env)->NewStringUTF(env, "x"));
    jstring s = (*env)->CallStaticObjectMethod(env, sc, format, (*env)->NewStringUTF(env, "%d-%s"), args);
    u = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    check(u && !strcmp(u, "42-x"), "String.format via JNI", u);
    if (u) (*env)->ReleaseStringUTFChars(env, s, u);

    /* Math.hypot(3, 4): double arguments and result */
    jclass mc = (*env)->FindClass(env, "java/lang/Math");
    jmethodID hypot = (*env)->GetStaticMethodID(env, mc, "hypot", "(DD)D");
    check((*env)->CallStaticDoubleMethod(env, mc, hypot, 3.0, 4.0) == 5.0, "Math.hypot", NULL);

    /* int[] through Get/ReleaseIntArrayElements (ART's heap is below 4 GiB: shared, not copied) */
    jintArray arr = (*env)->NewIntArray(env, 1000);
    jboolean copy = 2;
    jint *p = (*env)->GetIntArrayElements(env, arr, &copy);
    for (int k = 0; k < 1000; k++) p[k] = k;
    (*env)->ReleaseIntArrayElements(env, arr, p, 0);
    jint back[2];
    (*env)->GetIntArrayRegion(env, arr, 998, 2, back);
    char d[64];
    snprintf(d, sizeof d, "isCopy %d, at %p", copy, (void *)p);
    check(back[0] == 998 && back[1] == 999, "int[] elements", d);

    /* an exception thrown in Java, seen and cleared by guest code */
    jclass ic2 = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID parse = (*env)->GetStaticMethodID(env, ic2, "parseInt", "(Ljava/lang/String;)I");
    (*env)->CallStaticIntMethod(env, ic2, parse, (*env)->NewStringUTF(env, "not a number"));
    int had = (*env)->ExceptionCheck(env);
    (*env)->ExceptionClear(env);
    check(had, "Java exception visible to guest code", NULL);

    pthread_t t;
    void *r = NULL;
    pthread_create(&t, NULL, thread_fn, NULL);
    pthread_join(t, &r);
    check(r != NULL, "guest thread attaches and calls Java", NULL);

    /* the C++ test library: dlopen from guest code, exceptions inside it */
    void *h = dlopen("libbtest3.so", RTLD_NOW);
    int (*cxx)(int) = h ? (int (*)(int))dlsym(h, "cxx_test") : NULL;
    int cr = cxx ? cxx(6) : -1;
    snprintf(d, sizeof d, "%d", cr);
    check(cr == 303, "C++ library via dlopen", d);

    check(a->internalDataPath && access(a->internalDataPath, F_OK) == 0, "internalDataPath exists",
          a->internalDataPath);
    LOG("A32TEST done %d failed", failed);
}

static void on_start(ANativeActivity *a) { LOG("onStart"); }
static void on_resume(ANativeActivity *a) { LOG("onResume"); }
static void on_pause(ANativeActivity *a) { LOG("onPause"); }
static void on_focus(ANativeActivity *a, int f) { LOG("onWindowFocusChanged %d", f); }
static void on_window(ANativeActivity *a, ANativeWindow *w)
{
    LOG("onNativeWindowCreated %dx%d format %d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w),
        ANativeWindow_getFormat(w));
}
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { LOG("onNativeWindowDestroyed"); }

void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t n)
{
    LOG("onCreate (32-bit guest code, sdk %d)", a->sdkVersion);
    a->callbacks->onStart = on_start;
    a->callbacks->onResume = on_resume;
    a->callbacks->onPause = on_pause;
    a->callbacks->onWindowFocusChanged = on_focus;
    a->callbacks->onNativeWindowCreated = on_window;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    run_checks(a);
}
