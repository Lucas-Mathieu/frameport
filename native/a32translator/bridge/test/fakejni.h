/* A small fake JVM for testing the bridge without Android: classes and methods are C structs, Java methods are C
 * functions, native methods are registered function pointers that the harness calls the way ART does. */
#ifndef FAKEJNI_H
#define FAKEJNI_H

#include <jni.h>

typedef struct fclass fclass;
typedef struct fmethod fmethod;
typedef jvalue (*fimpl)(JNIEnv *env, jobject self, const jvalue *args);

struct fmethod {
    const char *name, *sig;
    int is_static, is_native;
    fimpl impl;      /* Java methods */
    void *native_fn; /* native methods: set by RegisterNatives */
    fclass *cls;
};

JavaVM *fake_vm(void);
JNIEnv *fake_env(void);
void fake_attach(void); /* marks the calling thread as attached (ART threads are) */
/* defines a class with methods (name, sig, static, native, impl); NULL name ends the list */
fclass *fake_class(const char *name, const fmethod *methods);
fmethod *fake_method(fclass *c, const char *name, const char *sig);
jstring fake_string(const char *utf);
const char *fake_utf(jstring s);
/* a primitive array whose data lies below 4 GiB (like ART's heap) or above it */
jarray fake_array(char type, int len, int low);
void *fake_array_data(jarray a);
/* the pending exception's message (NULL if none); clears it */
const char *fake_take_exception(void);
int fake_get_int_field(jobject o);

#endif
