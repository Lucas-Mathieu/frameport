/* liblog for guest libraries: formatted here, written by the host's liblog. */
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "grt.h"

int __android_log_write(int prio, const char *tag, const char *msg)
{
    grt_hc(HC_LOG, (uint32_t)prio, (uint32_t)(uintptr_t)(tag ? tag : ""), (uint32_t)(uintptr_t)(msg ? msg : ""), 0);
    return 1;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

int __android_log_buf_write(int buf, int prio, const char *tag, const char *msg)
{
    (void)buf;
    return __android_log_write(prio, tag, msg);
}

int __android_log_buf_print(int buf, int prio, const char *tag, const char *fmt, ...)
{
    (void)buf;
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char buf[1024];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
    } else {
        snprintf(buf, sizeof buf, "assertion failed: %s", cond ? cond : "");
    }
    __android_log_write(ANDROID_LOG_FATAL, tag, buf);
    abort();
}

int __android_log_is_loggable(int prio, const char *tag, int def)
{
    (void)tag;
    return prio >= def;
}
