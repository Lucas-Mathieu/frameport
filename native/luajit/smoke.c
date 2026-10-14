#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include "lua.h"
#include "lauxlib.h"
static const char *(*tolstring)(lua_State *,int,size_t *);
static int panic(lua_State *L) {
 fprintf(stderr,"PANIC: %s\n",tolstring(L,-1,NULL)); fflush(stderr); return 0;
}
#define RESOLVE(name) __typeof__(&name) f_##name=(__typeof__(&name))dlsym(h,#name); if(!f_##name){fprintf(stderr,"missing %s: %s\n",#name,dlerror());return 4;}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 void*h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 3;}
 RESOLVE(luaL_newstate); RESOLVE(lua_pushlightuserdata); RESOLVE(lua_touserdata); RESOLVE(lua_atpanic); RESOLVE(lua_close);
 tolstring=(void*)dlsym(h,"lua_tolstring");
 void*(*tag)(void)=dlsym(h,"xlua_tag");if(!tag)return 4;
 lua_State*L=f_luaL_newstate();if(!L)return 5;f_lua_atpanic(L,panic);
 void*p=tag();
 if(argv[2][0]=='l') {p=mmap((void*)0x10000000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)return 6;}
 printf("native pointer=%p, above47=%u, LuaState=%p\n",p,(unsigned)((uintptr_t)p>>47!=0),L);fflush(stdout);
 f_lua_pushlightuserdata(L,p);
 void*back=f_lua_touserdata(L,-1);
 printf("roundtrip=%p exact=%u\n",back,back==p);fflush(stdout);
 if(argv[2][0]=='h' && argv[2][1]=='e') {
  RESOLVE(lua_getfield); RESOLVE(lua_type); RESOLVE(lua_settop);
  int(*open_perf)(lua_State*)=dlsym(h,"luaopen_perflib");
  void*(*global)(lua_State*)=dlsym(h,"xlua_gl");
  if(!open_perf || !global)return 8;
  if(global(L)!=*(void**)((char*)L+16))return 9;
  open_perf(L);f_lua_getfield(L,LUA_GLOBALSINDEX,"perf");
  if(f_lua_type(L,-1)!=LUA_TTABLE)return 10;
  printf("retained xLua global reference and perf helper: PASS\n");fflush(stdout);
  f_lua_settop(L,0);
 }
 f_lua_close(L);return back==p?0:7;
}
