/* CPU benchmark: integer, memory, float and double kernels. Checksums go to stdout, timings to stderr. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static uint32_t crc_table[256];
static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t st[8], const uint8_t *p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    st[0] += a, st[1] += b, st[2] += c, st[3] += d, st[4] += e, st[5] += f, st[6] += g, st[7] += h;
}

static int cmpu(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    double t0 = now(), t;
    /* 1. sieve */
    int n = 2000000 * scale, primes = 0;
    char *sieve = calloc(n + 1, 1);
    for (int i = 2; i <= n; i++)
        if (!sieve[i]) {
            primes++;
            for (long long j = (long long)i * i; j <= n; j += i) sieve[j] = 1;
        }
    printf("primes %d\n", primes);
    t = now(); fprintf(stderr, "sieve   %.3f s\n", t - t0); t0 = t;
    /* 2. crc32 + sha256 over 4 MiB */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    size_t len = (size_t)4 << 20;
    uint8_t *data = malloc(len);
    uint32_t r = 1;
    for (size_t i = 0; i < len; i++) r = r * 1664525u + 1013904223u, data[i] = (uint8_t)(r >> 24);
    uint32_t crc = 0;
    for (int k = 0; k < scale; k++) crc ^= crc32_buf(data, len);
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int k = 0; k < scale; k++)
        for (size_t off = 0; off + 64 <= len; off += 64) sha256_block(st, data + off);
    printf("crc %08x sha %08x%08x\n", crc, st[0], st[7]);
    t = now(); fprintf(stderr, "hash    %.3f s\n", t - t0); t0 = t;
    /* 3. sort 1M */
    uint32_t *arr = malloc(sizeof *arr * 1000000);
    for (int i = 0; i < 1000000; i++) r = r * 1664525u + 1013904223u, arr[i] = r;
    qsort(arr, 1000000, 4, cmpu);
    printf("sorted %u %u\n", arr[0], arr[999999]);
    t = now(); fprintf(stderr, "qsort   %.3f s\n", t - t0); t0 = t;
    /* 4. float matrix multiply 256x256 */
    int N = 256;
    float *A = malloc(4 * N * N), *B = malloc(4 * N * N), *C = calloc(N * N, 4);
    for (int i = 0; i < N * N; i++) A[i] = (float)(i % 17) * 0.25f, B[i] = (float)(i % 13) - 6.0f;
    for (int s = 0; s < scale; s++)
        for (int i = 0; i < N; i++)
            for (int k = 0; k < N; k++) {
                float a = A[i * N + k];
                for (int j = 0; j < N; j++) C[i * N + j] += a * B[k * N + j];
            }
    double cs = 0;
    for (int i = 0; i < N * N; i++) cs += C[i];
    printf("matmul %.1f\n", cs);
    t = now(); fprintf(stderr, "matmul  %.3f s\n", t - t0); t0 = t;
    /* 5. double: mandelbrot */
    int iters = 0;
    for (int y = 0; y < 300 * scale; y++)
        for (int x = 0; x < 400; x++) {
            double cr = -2.0 + x * 3.0 / 400, ci = -1.2 + y * 2.4 / (300 * scale), zr = 0, zi = 0;
            int k = 0;
            while (k < 200 && zr * zr + zi * zi < 4) {
                double tr = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = tr;
                k++;
            }
            iters += k;
        }
    printf("mandel %d\n", iters);
    t = now(); fprintf(stderr, "mandel  %.3f s\n", t - t0); t0 = t;
    /* 6. libm */
    double acc = 0;
    for (int i = 1; i < 300000 * scale; i++) acc += sin(i * 0.001) * exp(-i * 1e-6) + log(i) * sqrt(i);
    printf("libm %.6e\n", acc);
    t = now(); fprintf(stderr, "libm    %.3f s\n", t - t0);
    return 0;
}
