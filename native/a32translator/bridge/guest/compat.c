/* LP32 bionic symbols that only libc.so has (old NDK binaries still link against them). */
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

static short lower_tab[257], upper_tab[257];
const short *_tolower_tab_ = lower_tab;
const short *_toupper_tab_ = upper_tab;
int __isthreaded = 1;

__attribute__((constructor)) static void init_tabs(void)
{
    lower_tab[0] = upper_tab[0] = -1; /* EOF */
    for (int c = 0; c < 256; c++) {
        lower_tab[c + 1] = (short)tolower(c);
        upper_tab[c + 1] = (short)toupper(c);
    }
}

sighandler_t bsd_signal(int sig, sighandler_t h) { return signal(sig, h); }

int vfdprintf(int fd, const char *fmt, va_list ap) { return vdprintf(fd, fmt, ap); }
int fdprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int putw(int w, FILE *f) { return fwrite(&w, sizeof w, 1, f) == 1 ? 0 : EOF; }
void get_malloc_leak_info(void **info, size_t *overall, size_t *isize, size_t *total, size_t *backtrace)
{
    *info = 0;
    *overall = *isize = *total = *backtrace = 0;
}
void free_malloc_leak_info(void *info) { (void)info; }

/* old LP32 libc.so also exported these (Unity 5 and Mono builds link against them) */
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

unsigned int __page_size = 4096;
int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
int tkill(int tid, int sig) { return (int)syscall(__NR_tkill, tid, sig); }

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA1_CTX;

#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

static void sha1_block(uint32_t st[5], const uint8_t b[64])
{
    uint32_t w[80], a = st[0], bb = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (bb & c) | (~bb & d), k = 0x5a827999;
        else if (i < 40) f = bb ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8f1bbcdc;
        else f = bb ^ c ^ d, k = 0xca62c1d6;
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d, d = c, c = ROL(bb, 30), bb = a, a = t;
    }
    st[0] += a, st[1] += bb, st[2] += c, st[3] += d, st[4] += e;
}

void SHA1Init(SHA1_CTX *ctx)
{
    static const uint32_t init[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memcpy(ctx->state, init, sizeof init);
    ctx->count[0] = ctx->count[1] = 0;
}

void SHA1Update(SHA1_CTX *ctx, const uint8_t *data, uint32_t len)
{
    uint32_t j = (ctx->count[0] >> 3) & 63;
    if ((ctx->count[0] += len << 3) < (len << 3)) ctx->count[1]++;
    ctx->count[1] += len >> 29;
    uint32_t i = 0;
    if (j + len > 63) {
        memcpy(&ctx->buffer[j], data, (i = 64 - j));
        sha1_block(ctx->state, ctx->buffer);
        for (; i + 63 < len; i += 64) sha1_block(ctx->state, &data[i]);
        j = 0;
    }
    memcpy(&ctx->buffer[j], &data[i], len - i);
}

void SHA1Final(uint8_t digest[20], SHA1_CTX *ctx)
{
    uint8_t final[8];
    for (int i = 0; i < 8; i++) final[i] = (uint8_t)(ctx->count[i >= 4 ? 0 : 1] >> ((3 - (i & 3)) * 8));
    uint8_t c = 0x80;
    SHA1Update(ctx, &c, 1);
    c = 0;
    while ((ctx->count[0] & 504) != 448) SHA1Update(ctx, &c, 1);
    SHA1Update(ctx, final, 8);
    for (int i = 0; i < 20; i++) digest[i] = (uint8_t)(ctx->state[i >> 2] >> ((3 - (i & 3)) * 8));
}

void __google_potentially_blocking_region_begin(void) {}
void __google_potentially_blocking_region_end(void) {}
