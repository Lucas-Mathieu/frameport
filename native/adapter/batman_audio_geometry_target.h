// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static const unsigned char mx_geometry_il2cpp_id[20]={
    0x1d,0x48,0xa4,0x1d,0x7e,0x90,0x11,0xbe,0xf5,0xfa,0xe0,0xc2,0x3c,0x40,0x4c,0x62,0xa4,0xe7,0xa7,0x3c
};
static const uint32_t mx_geometry_queue_entry[4]={0xd10243ff,0xa9065ffe,0xa90757f6,0xa9084ff4};
static const uint32_t mx_geometry_worker_entry[4]={0xd10183ff,0xa90267fe,0xa9035ff8,0xa90457f6};
static int mx_geometry_matches_id(const void *id,size_t size) {
    return size==sizeof(mx_geometry_il2cpp_id) && !memcmp(id,mx_geometry_il2cpp_id,size);
}
static int mx_geometry_matches_entries(const void *queue,const void *worker) {
    return !memcmp(queue,mx_geometry_queue_entry,sizeof(mx_geometry_queue_entry)) &&
           !memcmp(worker,mx_geometry_worker_entry,sizeof(mx_geometry_worker_entry));
}
