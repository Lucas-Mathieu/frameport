// SPDX-License-Identifier: GPL-3.0-only
// Climb-only workaround for the verified renderer and Valve layer, never a global driver edit.
#include <sys/mman.h>
#include <unistd.h>
#include "sha256.h"
static void climb_disable_foveation(const char *reason) {
    unsetenv("ENABLE_VULKAN_FDM_INJECTION_LAYER");
    setenv("DISABLE_VULKAN_FDM_INJECTION_LAYER", "1", 1);
    unsetenv("FDM_DEBUG");
    LOG("CLIMB foveation disabled: %s", reason);
}
static void climb_stereo_fix(void) {
    const char *enabled=getenv("FRAMEPORT_CLIMB_STEREO_OFFSETS");
    if (!enabled || strcmp(enabled,"1")) return;
    char process[128] = {0};
    FILE *cmd = fopen("/proc/self/cmdline", "rb");
    if (!cmd) return;
    size_t n = fread(process, 1, sizeof(process)-1, cmd); fclose(cmd);
    if (!n || strcmp(process, "com.crytek.climb2")) return;
    void *h=dlopen("libVkLayer_VALVE_fdm_injection.so",RTLD_NOW|RTLD_LOCAL);
    void *sym=h?dlsym(h,"vkGetInstanceProcAddr"):NULL;
    Dl_info di;
    if (!sym || !dladdr(sym,&di)) { climb_disable_foveation("layer not found");return; }
    // Exact verified Valve binary. Never guess offsets after a SteamOS update.
    FILE *file = fopen(di.dli_fname, "rb");
    unsigned char digest[32];
    int verified = 0;
    if (file && !fseek(file, 0, SEEK_END)) {
        long size = ftell(file);
        if (size > 0 && size <= 16*1024*1024 && !fseek(file, 0, SEEK_SET)) {
            unsigned char *bytes = malloc((size_t)size);
            if (bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size) {
                sha256(bytes, (size_t)size, digest);
                static const unsigned char expected[32] = {0x55,0x76,0x28,0x8e,0x08,0x20,0x5d,0xea,0xfd,0xe2,0x20,0x3d,0xbe,0xe2,0x5e,0x77,0x16,0x9c,0x5e,0xe4,0x8f,0xb6,0xaf,0xdb,0xe1,0xb5,0x76,0x11,0xbb,0x2a,0x48,0xb4};
                verified = !memcmp(digest, expected, 32);
            }
            free(bytes);
        }
    }
    if (file) fclose(file);
    if (!verified) { climb_disable_foveation("unsupported Valve library"); return; }
    unsigned char *base=di.dli_fbase;
    uint32_t *count=(void*)(base+0x1abdc), *mask=(void*)(base+0x1b6e4);
    uint32_t *next1=(void*)(base+0x1ada0), *next2=(void*)(base+0x1ae48);
    if (*count!=0xb9403a89u || *mask!=0xb9403e75u || *next1!=0xb40000cau || *next2!=0xb40000cau) {climb_disable_foveation("instruction mismatch");return;}
    long page=sysconf(_SC_PAGESIZE);
    if (page <= 0) { climb_disable_foveation("invalid page size"); return; }
    uintptr_t lo=(uintptr_t)count&~((uintptr_t)page-1);
    uintptr_t hi=((uintptr_t)mask+4+page-1)&~((uintptr_t)page-1);
    if(mprotect((void*)lo,hi-lo,PROT_READ|PROT_WRITE|PROT_EXEC)) {climb_disable_foveation("mprotect failed");return;}
    *count=0x52800049u; // mov w9, #2: one offset for each FDM array layer
    *next1=0x14000006u; // skip invalid mask-table read; stereo end mask is explicit
    *next2=0x14000006u;
    *mask=0x52800075u;  // mov w21, #3: calculate offsets for both eyes
    __builtin___clear_cache((char*)lo,(char*)hi);
    mprotect((void*)lo,hi-lo,PROT_READ|PROT_EXEC);
    LOG("CLIMB stereo fix: two density-map offsets + safe subpass bookkeeping enabled in this process");
}
