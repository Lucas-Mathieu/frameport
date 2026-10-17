// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#define LOG(...) ((void)0)
static int fail_allocation;
static _Atomic unsigned allocations,releases;
static void *test_alloc(size_t size) {
    if(fail_allocation)return NULL;
    allocations++;return malloc(size);
}
static void test_free(void *p) {releases++;free(p);}
#define malloc test_alloc
#define free test_free
#include "audio_geometry_owned.h"
#undef malloc
#undef free
#include "batman_audio_geometry_target.h"

typedef struct {
    unsigned char header[16];
    const void *data;
    uint64_t length;
    int immediate;
    const void *source;
} Closure;
static void original_worker(void *context,void *method) {
    Closure *closure=context;
    assert(method==(void*)42 && closure->length==UINT64_C(0x123400000040));
    assert(closure->data!=closure->source);
    for(unsigned i=0;i<64;i++)assert(((const unsigned char*)closure->data)[i]==(unsigned char)i);
}
static void original_queue(void *object,const void *data,uint64_t length,void *method) {
    Closure *closure=object;
    assert(method==(void*)42);
    closure->data=data;closure->length=length;
    if(closure->immediate)mx_geometry_worker(closure,method);
}
static void *concurrent_load(void *unused) {
    (void)unused;
    for(unsigned n=0;n<100;n++) {
        unsigned char source[64];for(unsigned i=0;i<64;i++)source[i]=(unsigned char)i;
        Closure closure={.immediate=1,.source=source};
        mx_geometry_queue(&closure,source,UINT64_C(0x123400000040),(void*)42);
    }
    return NULL;
}
int main(void) {
    mx_geometry_queue_original=original_queue;mx_geometry_worker_original=original_worker;
    unsigned char source[64];for(unsigned i=0;i<64;i++)source[i]=(unsigned char)i;
    Closure closure={.source=source};
    mx_geometry_queue(&closure,source,UINT64_C(0x123400000040),(void*)42);
    assert(mx_geometry_owned_head && closure.data!=source && allocations==1 && releases==0);
    memset(source,0xff,sizeof(source)); // download handler's source goes away before the task runs
    mx_geometry_worker(&closure,(void*)42);
    assert(!mx_geometry_owned_head && allocations==releases);
    for(unsigned i=0;i<64;i++)source[i]=(unsigned char)i;
    closure.immediate=1;
    mx_geometry_queue(&closure,source,UINT64_C(0x123400000040),(void*)42);
    assert(!mx_geometry_owned_head && allocations==releases); // task finishes before queue() returns
    fail_allocation=1;closure.immediate=0;
    mx_geometry_queue(&closure,source,64,(void*)42);
    assert(closure.data==source && !mx_geometry_owned_head); // preserve original call on allocation failure
    fail_allocation=0;
    mx_geometry_queue(&closure,NULL,0,(void*)42);
    assert(!closure.data && !mx_geometry_owned_head);
    mx_geometry_queue(&closure,source,UINT32_MAX,(void*)42); // negative NativeArray length: unchanged
    assert(closure.data==source && !mx_geometry_owned_head);
    pthread_t threads[4];
    for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,concurrent_load,NULL));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(!mx_geometry_owned_head && mx_geometry_queued==mx_geometry_finished);
    assert(mx_geometry_matches_id(mx_geometry_il2cpp_id,20));
    unsigned char wrong_id[20];memcpy(wrong_id,mx_geometry_il2cpp_id,20);wrong_id[0]^=1;
    assert(!mx_geometry_matches_id(wrong_id,20) && !mx_geometry_matches_id(wrong_id,19));
    assert(mx_geometry_matches_entries(mx_geometry_queue_entry,mx_geometry_worker_entry));
    uint32_t wrong_entry[4];memcpy(wrong_entry,mx_geometry_queue_entry,16);wrong_entry[3]^=1;
    assert(!mx_geometry_matches_entries(wrong_entry,mx_geometry_worker_entry));
    assert(!mx_geometry_matches_entries(mx_geometry_queue_entry,wrong_entry));
    puts("PASS: deferred/inline/concurrent ownership, cleanup, ABI arguments, fallback and target guards");
}
