/* FramePort's startup marker and the layout contract used by retained xLua bindings. */
#include <stddef.h>
#include <android/log.h>
#include "lj_obj.h"

_Static_assert(LJ_GC64, "Retained xLua bindings require GC64");
_Static_assert(offsetof(lua_State, glref) == 16, "xLua's xlua_gl must keep its global-state reference");

__attribute__((constructor)) static void ready(void)
{
    __android_log_print(ANDROID_LOG_INFO, "FrameBridge", "lua: full-width lightuserdata runtime enabled");
}
