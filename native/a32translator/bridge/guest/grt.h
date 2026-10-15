/* Guest runtime (liba32rt.so): a static 32-bit bionic program that loads the game's libraries and connects them
 * to the host bridge. */
#ifndef GRT_H
#define GRT_H

#include <stdint.h>
#include "../abi.h"

#define A32_HYPERCALL 0x0a320000u

uint32_t grt_hc_raw(uint32_t r7, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3); /* hc.S */

static inline uint32_t grt_hc(uint32_t n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    return grt_hc_raw(A32_HYPERCALL | n, a0, a1, a2, a3);
}

void grt_log(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
uint32_t grt_export(const char *name); /* exports.S + main.c: address of a "system" symbol, 0 if none */

void loader_set_dir(const char *dir);
void *grt_dlopen(const char *name, int flags);
void *grt_dlsym(void *handle, const char *name);
char *grt_dlerror(void);
uint32_t *grt_list_symbols(void *handle, const char *prefix);

#endif
