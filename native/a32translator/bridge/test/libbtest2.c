/* Guest test library (armv7, ARM): a NativeActivity. Needs libbtest.so (DT_NEEDED) and libandroid. */
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>

int btest_helper(int x); /* libbtest.so */

static int started, resumed, width, focus, saved_ok, sdk, env_ok, destroyed;

static void on_start(ANativeActivity *a) { started++; }
static void on_resume(ANativeActivity *a) { resumed++; }
static void on_focus(ANativeActivity *a, int f) { focus = f; }
static void on_window(ANativeActivity *a, ANativeWindow *w) { width = ANativeWindow_getWidth(w); }
static void on_destroy(ANativeActivity *a) { destroyed++; }

static void *on_save(ANativeActivity *a, size_t *len)
{
    char *p = malloc(6);
    memcpy(p, "state", 6);
    *len = 6;
    return p;
}

void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t n)
{
    a->callbacks->onStart = on_start;
    a->callbacks->onResume = on_resume;
    a->callbacks->onWindowFocusChanged = on_focus;
    a->callbacks->onNativeWindowCreated = on_window;
    a->callbacks->onSaveInstanceState = on_save;
    a->callbacks->onDestroy = on_destroy;
    saved_ok = n == 3 && !memcmp(saved, "abc", 3);
    sdk = a->sdkVersion;
    JNIEnv *env = NULL;
    env_ok = (*a->vm)->GetEnv(a->vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && env == a->env &&
             a->internalDataPath && !strcmp(a->internalDataPath, "/data/test/files");
    __android_log_print(ANDROID_LOG_INFO, "btest2", "onCreate: sdk %d, saved %d", sdk, saved_ok);
}

int btest2_result(int which)
{
    switch (which) {
    case 0: return started;
    case 1: return resumed;
    case 2: return width;
    case 3: return focus;
    case 4: return saved_ok;
    case 5: return sdk;
    case 6: return env_ok;
    case 7: return btest_helper(1);
    case 8: return destroyed;
    }
    return -1;
}
