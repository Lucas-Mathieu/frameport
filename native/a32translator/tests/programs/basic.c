/* Integer, 64-bit, float/double, libm and printf coverage. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t h = 2166136261u;
static void mix(uint64_t v) { for (int k = 0; k < 8; k++) h = (h ^ (uint8_t)(v >> (8 * k))) * 16777619u; }
static void mixd(double d) { uint64_t v; memcpy(&v, &d, 8); mix(v); }

__attribute__((noinline)) static int64_t div64(int64_t a, int64_t b) { return b ? a / b : 0; }
__attribute__((noinline)) static uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a % b : 0; }

int main(int argc, char **argv)
{
    printf("argc=%d argv0_tail=%s\n", argc, strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0]);
    uint32_t x = 0x12345678u;
    for (int i = 0; i < 1000; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        int32_t s = (int32_t)x;
        mix(x);
        mix((uint64_t)x * 0x9e3779b97f4a7c15ull);
        mix(div64((int64_t)s * 12345, (int32_t)(x | 1) >> 3));
        mix(udiv64((uint64_t)x << 20 | i, (x >> 7) + 1));
        mix(x / ((x >> 11) + 1));
        mix((uint32_t)(s >> (i & 31)));
        mix(__builtin_clz(x | 1) + __builtin_ctz(x | 0x80000000u) + __builtin_popcount(x));
        mix(__builtin_bswap32(x));
        double d = (double)s / 1000.0;
        float f = (float)d * 0.5f;
        mixd(d);
        mixd(f);
        mixd(sqrt(fabs(d)));
        mixd(sin(d) + cos(d));
        mixd(exp(d / 1e6));
        mixd(log(fabs(d) + 1));
        mixd(pow(fabs(d) / 1e6 + 1, 1.5));
        mixd(floor(d) + ceil(f) + round(d) + trunc(f));
        mixd(fmod(d, 7.25));
        mixd(atan2(d, f + 1));
        mix((int64_t)d);
        mix((uint32_t)(int32_t)f);
        mix((int64_t)(f * 1e3f));
        mix(lrint(d));
        mixd(sqrtf(fabsf(f)));
        mixd(fmaf(f, 1.25f, -f));
    }
    printf("hash %08x\n", h);
    printf("%d %u %lld %llx %.6f %.3e %g %s %c %5.2f%%\n", -42, 42u, -123456789012345LL, 0xdeadbeefcafeULL, M_PI,
           6.02214076e23, 1.0 / 3, "str", 'c', 99.5);
    char buf[64];
    snprintf(buf, sizeof buf, "%08.3f|%-6d|%+d", -3.14159, 7, 9);
    printf("%s\n", buf);
    printf("strtod %.10g strtol %ld atof %.4f\n", strtod("1.5e-3", NULL), strtol("-0x7f", NULL, 16), atof("2.718281828"));
    return 3;
}
