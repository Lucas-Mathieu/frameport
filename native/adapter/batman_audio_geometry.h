// SPDX-License-Identifier: GPL-3.0-only
// Batman 1.4.1: keep the web request's native audio data alive across Task.Run.
#pragma once
#if defined(__aarch64__)
#include "audio_geometry_owned.h"
#include "batman_audio_geometry_target.h"
static int mx_geometry_checked;
static int mx_geometry_find(struct dl_phdr_info *info,size_t ignored,void *unused) {
    (void)ignored;(void)unused;
    if(!strstr(info->dlpi_name,"/libil2cpp.so"))return 0;
    int match=0;
    for(unsigned i=0;i<info->dlpi_phnum;i++)if(info->dlpi_phdr[i].p_type==PT_NOTE) {
        const unsigned char *p=(void*)(info->dlpi_addr+info->dlpi_phdr[i].p_vaddr);
        const unsigned char *end=p+info->dlpi_phdr[i].p_memsz;
        while(p+sizeof(Elf64_Nhdr)<=end) {
            const Elf64_Nhdr *note=(const void*)p;
            const unsigned char *name=p+sizeof(*note),*desc=name+((note->n_namesz+3)&~3u);
            const unsigned char *next=desc+((note->n_descsz+3)&~3u);
            if(next>end || next<=p)break;
            if(note->n_type==NT_GNU_BUILD_ID && note->n_namesz==4 && !memcmp(name,"GNU",4) &&
               mx_geometry_matches_id(desc,note->n_descsz))match=1;
            p=next;
        }
    }
    __atomic_store_n(&mx_geometry_checked,1,__ATOMIC_RELEASE);
    if(!match)return 1;
    unsigned char *base=(void*)info->dlpi_addr;
    // Live VAs, not the file offsets printed by the managed-code dump.
    unsigned char *queue=base+0x2e61bb4,*worker=base+0x2e63a28;
    if(!mx_geometry_matches_entries(queue,worker))return 1;
    // Hook the completion boundary first so every queued copy has a consumer.
    void *original=mx_audio_detour(worker,(void*)mx_geometry_worker,mx_geometry_worker_entry);
    __atomic_store_n(&mx_geometry_worker_original,original,__ATOMIC_RELEASE);
    if(!original)return 1;
    original=mx_audio_detour(queue,(void*)mx_geometry_queue,mx_geometry_queue_entry);
    __atomic_store_n(&mx_geometry_queue_original,original,__ATOMIC_RELEASE);
    LOG("FrameBridge audio geometry: Batman async input lifetime %s",original?"installed":"not installed");
    return 1;
}
static pthread_mutex_t mx_geometry_install_mutex=PTHREAD_MUTEX_INITIALIZER;
static void mx_batman_geometry_initialize(void) {
    // The metadata initializer already verified the Meta SDK build/prologues.
    // Keep this independent of its early-out: IL2CPP may be loaded afterwards.
    if(__atomic_load_n(&mx_geometry_checked,__ATOMIC_ACQUIRE) ||
       !__atomic_load_n(&mx_audio_base,__ATOMIC_ACQUIRE))return;
    if(pthread_mutex_trylock(&mx_geometry_install_mutex))return;
    if(!mx_geometry_checked) {
        static time_t last_scan;
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        if(now.tv_sec!=last_scan) {
            last_scan=now.tv_sec;
            char package[64]={0};FILE *cmd=fopen("/proc/self/cmdline","rb");
            if(cmd){(void)fread(package,1,sizeof(package)-1,cmd);fclose(cmd);}
            if(strcmp(package,"com.camouflaj.manta"))__atomic_store_n(&mx_geometry_checked,1,__ATOMIC_RELEASE);
            else dl_iterate_phdr(mx_geometry_find,NULL);
        }
    }
    pthread_mutex_unlock(&mx_geometry_install_mutex);
}
#else
static void mx_batman_geometry_initialize(void) {}
#endif
