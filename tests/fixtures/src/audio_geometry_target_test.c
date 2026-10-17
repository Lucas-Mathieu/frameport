// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <link.h>
#include <string.h>
#define LOG(...) ((void)0)
static uintptr_t mx_audio_base;
static unsigned detours,scans;
static void *mx_audio_detour(unsigned char *address,void *replacement,const uint32_t expected[4]) {
    assert(replacement && !memcmp(address,expected,16));detours++;return (void*)1;
}
static int fake_iterate(int (*callback)(struct dl_phdr_info*,size_t,void*),void *data) {
    (void)callback;(void)data;scans++;return 0;
}
#define dl_iterate_phdr fake_iterate
// Exercise the AArch64-only installer as C without executing ARM instructions.
#ifndef __aarch64__
#define __aarch64__ 1
#endif
#include "batman_audio_geometry.h"
int main(void) {
    mx_batman_geometry_initialize();assert(!scans && !detours); // unverified Meta SDK
    mx_audio_base=1;
    mx_batman_geometry_initialize();assert(!scans && !detours && mx_geometry_checked); // another package
    unsigned char *base=calloc(1,0x2e63a28+32);assert(base);
    Elf64_Phdr phdr={.p_type=PT_NOTE,.p_vaddr=256,.p_memsz=36};
    struct dl_phdr_info info={.dlpi_addr=(uintptr_t)base,.dlpi_name="/lib/libil2cpp.so",
                             .dlpi_phdr=&phdr,.dlpi_phnum=1};
    Elf64_Nhdr note={.n_namesz=4,.n_descsz=20,.n_type=NT_GNU_BUILD_ID};
    memcpy(base+256,&note,sizeof(note));memcpy(base+268,"GNU",4);
    memcpy(base+272,mx_geometry_il2cpp_id,20);
    memcpy(base+0x2e61bb4,mx_geometry_queue_entry,16);
    memcpy(base+0x2e63a28,mx_geometry_worker_entry,16);
    base[272]^=1;mx_geometry_find(&info,sizeof(info),NULL);assert(!detours); // other build
    base[272]^=1;base[0x2e63a28]^=1;
    mx_geometry_find(&info,sizeof(info),NULL);assert(!detours); // changed worker
    base[0x2e63a28]^=1;base[0x2e61bb4]^=1;
    mx_geometry_find(&info,sizeof(info),NULL);assert(!detours); // changed queue
    base[0x2e61bb4]^=1;
    mx_geometry_find(&info,sizeof(info),NULL);assert(detours==2); // only the exact pair is hooked
    free(base);
    puts("PASS: other package/SDK/build/entries skipped; matching binary hooks both boundaries");
}
