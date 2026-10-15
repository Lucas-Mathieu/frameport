/* malloc, memcpy/memmove/memset at many sizes and alignments (bionic's NEON routines), strings, qsort. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t h = 2166136261u;
static void mixb(const void *p, size_t n) { const uint8_t *b = p; for (size_t k = 0; k < n; k++) h = (h ^ b[k]) * 16777619u; }
static int cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

int main(void)
{
    uint8_t *src = malloc(5000), *dst = malloc(5000);
    for (int i = 0; i < 5000; i++) src[i] = (uint8_t)(i * 7 + (i >> 5));
    for (int n = 0; n < 300; n += (n < 70 ? 1 : 13))
        for (int sa = 0; sa < 8; sa++)
            for (int da = 0; da < 8; da += 3) {
                memset(dst, 0xa5, 400);
                memcpy(dst + da, src + sa, n);
                mixb(dst, 400);
                memmove(dst + da + 3, dst + da, n);
                mixb(dst, 400);
                memset(dst + sa, n & 0xff, n / 2);
                mixb(dst, 400);
                h ^= (uint32_t)memcmp(dst, src, n) * 31u;
            }
    printf("mem %08x\n", h);
    char s[256];
    for (int i = 0; i < 200; i++) {
        int len = snprintf(s, sizeof s, "item-%d-%x-%s", i, i * 1234567, i & 1 ? "odd" : "even");
        h = h * 31 + (uint32_t)strlen(s) + (uint32_t)len;
        h ^= (uint32_t)(strchr(s, 'x') ? strchr(s, 'x') - s : -1);
        h ^= (uint32_t)(strstr(s, "dd") != NULL) << 3;
        h += (uint32_t)strcmp(s, "item-100");
        h += (uint32_t)strncmp(s, "item-1", 6);
        char *d = strdup(s);
        h ^= (uint32_t)strcspn(d, "-");
        free(d);
    }
    printf("str %08x\n", h);
    int n = 20000;
    int *a = malloc(n * sizeof *a);
    uint32_t r = 1;
    for (int i = 0; i < n; i++) r = r * 1103515245 + 12345, a[i] = (int)(r >> 1) - (1 << 29);
    qsort(a, n, sizeof *a, cmp);
    int ok = 1;
    for (int i = 1; i < n; i++) ok &= a[i - 1] <= a[i];
    mixb(a, n * sizeof *a);
    printf("sort %d %08x\n", ok, h);
    void *blocks[512];
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 512; i++) blocks[i] = malloc((size_t)(i * 37 + round) % 3000 + 1);
        for (int i = 0; i < 512; i += 2) free(blocks[i]);
        for (int i = 0; i < 512; i += 2) blocks[i] = realloc(NULL, 64 + i);
        for (int i = 0; i < 512; i++) free(blocks[i]);
    }
    void *big = malloc(64 << 20);
    memset(big, 1, 64 << 20);
    printf("big %d\n", ((char *)big)[12345678]);
    free(big);
    free(a);
    return 0;
}
