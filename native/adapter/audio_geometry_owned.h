// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// The source NativeArray belongs to a UnityWebRequest. Copy while its caller
// still owns that request, before async-void queues the background parser.
typedef struct MxGeometryOwned {
    struct MxGeometryOwned *next;
    size_t size;
    unsigned char bytes[];
} MxGeometryOwned;
static pthread_mutex_t mx_geometry_owned_mutex=PTHREAD_MUTEX_INITIALIZER;
static MxGeometryOwned *mx_geometry_owned_head;
static unsigned mx_geometry_queued,mx_geometry_finished;
static void (*mx_geometry_queue_original)(void*,const void*,uint64_t,void*);
static void (*mx_geometry_worker_original)(void*,void*);
static void mx_geometry_queue(void *object,const void *data,uint64_t packed_length,void *method) {
    void (*original)(void*,const void*,uint64_t,void*);
    while(!(original=__atomic_load_n(&mx_geometry_queue_original,__ATOMIC_ACQUIRE)))sched_yield();
    int32_t length=(int32_t)(uint32_t)packed_length;
    size_t size=length>0?(size_t)length:0;
    if(!data || !size || size>SIZE_MAX-sizeof(MxGeometryOwned)){original(object,data,packed_length,method);return;}
    MxGeometryOwned *owned=malloc(sizeof(*owned)+size);
    if(!owned){original(object,data,packed_length,method);return;}
    owned->size=size;
    memcpy(owned->bytes,data,size);
    pthread_mutex_lock(&mx_geometry_owned_mutex);
    owned->next=mx_geometry_owned_head;mx_geometry_owned_head=owned;
    pthread_mutex_unlock(&mx_geometry_owned_mutex);
    unsigned queued=__atomic_fetch_add(&mx_geometry_queued,1,__ATOMIC_RELAXED);
    if(queued<4)LOG("FrameBridge audio geometry: owned %zu bytes before background task queued",size);
    original(object,owned->bytes,packed_length,method);
}
static void mx_geometry_worker(void *closure,void *method) {
    const void *data=NULL;
    if(closure)memcpy(&data,(const unsigned char*)closure+16,8);
    void (*original)(void*,void*);
    while(!(original=__atomic_load_n(&mx_geometry_worker_original,__ATOMIC_ACQUIRE)))sched_yield();
    original(closure,method);
    MxGeometryOwned *finished=NULL;
    pthread_mutex_lock(&mx_geometry_owned_mutex);
    for(MxGeometryOwned **p=&mx_geometry_owned_head;*p;p=&(*p)->next)if((*p)->bytes==data) {
        finished=*p;*p=finished->next;break;
    }
    pthread_mutex_unlock(&mx_geometry_owned_mutex);
    if(finished) {
        unsigned count=__atomic_fetch_add(&mx_geometry_finished,1,__ATOMIC_RELAXED);
        if(count<4)LOG("FrameBridge audio geometry: background parse finished; releasing %zu owned bytes",finished->size);
        free(finished);
    }
}
