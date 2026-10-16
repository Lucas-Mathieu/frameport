/* The ARMv8 crypto instructions (SHA1C/P/M/H/SU0/SU1), which the JIT translates through the interpreter (one exit
 * stub each): dense() = 72 of them in a row, as dense as OpenSSL's sha1_block_data_order_armv8, whose block
 * overflowed the JIT's stub table (fatal "overflowed the code buffer"; GitHub #342, Pinball FX2 VR); then a fully
 * unrolled SHA-1 checked against known digests. Built with -march=armv8-a -mfpu=crypto-neon-fp-armv8. */
#include <arm_neon.h>
#include <stdio.h>
#include <string.h>

#define K0 0x5A827999u
#define K1 0x6ED9EBA1u
#define K2 0x8F1BBCDCu
#define K3 0xCA62C1D6u

/* 4 rounds: e1 = sha1h(a), abcd = sha1{c,p,m}(abcd, e0, w + k); then the message schedule */
#define R(op, k, e_in, e_out, w) do { \
        e_out = vsha1h_u32(vgetq_lane_u32(abcd, 0)); \
        abcd = op(abcd, e_in, vaddq_u32(w, vdupq_n_u32(k))); \
    } while (0)
#define SCHED(w0, w1, w2, w3) (w0 = vsha1su1q_u32(vsha1su0q_u32(w0, w1, w2), w3))

static void block(uint32_t st[5], const uint8_t *p)
{
    uint32x4_t abcd = vld1q_u32(st), abcd0 = abcd;
    uint32_t e0 = st[4], e1, e00 = e0;
    uint32x4_t w0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p)));
    uint32x4_t w1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16)));
    uint32x4_t w2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 32)));
    uint32x4_t w3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 48)));
    R(vsha1cq_u32, K0, e0, e1, w0); SCHED(w0, w1, w2, w3);
    R(vsha1cq_u32, K0, e1, e0, w1); SCHED(w1, w2, w3, w0);
    R(vsha1cq_u32, K0, e0, e1, w2); SCHED(w2, w3, w0, w1);
    R(vsha1cq_u32, K0, e1, e0, w3); SCHED(w3, w0, w1, w2);
    R(vsha1cq_u32, K0, e0, e1, w0); SCHED(w0, w1, w2, w3);
    R(vsha1pq_u32, K1, e1, e0, w1); SCHED(w1, w2, w3, w0);
    R(vsha1pq_u32, K1, e0, e1, w2); SCHED(w2, w3, w0, w1);
    R(vsha1pq_u32, K1, e1, e0, w3); SCHED(w3, w0, w1, w2);
    R(vsha1pq_u32, K1, e0, e1, w0); SCHED(w0, w1, w2, w3);
    R(vsha1pq_u32, K1, e1, e0, w1); SCHED(w1, w2, w3, w0);
    R(vsha1mq_u32, K2, e0, e1, w2); SCHED(w2, w3, w0, w1);
    R(vsha1mq_u32, K2, e1, e0, w3); SCHED(w3, w0, w1, w2);
    R(vsha1mq_u32, K2, e0, e1, w0); SCHED(w0, w1, w2, w3);
    R(vsha1mq_u32, K2, e1, e0, w1); SCHED(w1, w2, w3, w0);
    R(vsha1mq_u32, K2, e0, e1, w2); SCHED(w2, w3, w0, w1);
    R(vsha1pq_u32, K3, e1, e0, w3); SCHED(w3, w0, w1, w2);
    R(vsha1pq_u32, K3, e0, e1, w0);
    R(vsha1pq_u32, K3, e1, e0, w1);
    R(vsha1pq_u32, K3, e0, e1, w2);
    R(vsha1pq_u32, K3, e1, e0, w3);
    vst1q_u32(st, vaddq_u32(abcd, abcd0));
    st[4] = e0 + e00;
}

static void sha1(const char *msg, uint8_t out[20])
{
    uint32_t st[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t buf[128] = {0};
    size_t n = strlen(msg), blocks = (n + 9 + 63) / 64;
    memcpy(buf, msg, n);
    buf[n] = 0x80;
    unsigned long long bits = (unsigned long long)n * 8;
    for (int k = 0; k < 8; k++) buf[blocks * 64 - 1 - k] = (uint8_t)(bits >> (8 * k));
    for (size_t b = 0; b < blocks; b++) block(st, buf + 64 * b);
    for (int k = 0; k < 20; k++) out[k] = (uint8_t)(st[k / 4] >> (24 - 8 * (k % 4)));
}

/* 72 crypto instructions with nothing between them: denser than the compiler's code above, as dense as OpenSSL's
 * hand-written rounds */
__attribute__((noinline)) static void dense(uint32_t v[12])
{
    __asm__ volatile("vld1.32 {d0-d3}, [%0]!\n\t"
                     "vld1.32 {d4-d5}, [%0]\n\t"
                     ".rept 24\n\t"
                     "sha1c.32 q0, q1, q2\n\t"
                     "sha1h.32 q1, q0\n\t"
                     "sha1su0.32 q2, q0, q1\n\t"
                     ".endr\n\t"
                     "vst1.32 {d4-d5}, [%0]\n\t"
                     "sub %0, %0, #32\n\t"
                     "vst1.32 {d0-d3}, [%0]"
                     : "+r"(v)
                     :
                     : "memory", "d0", "d1", "d2", "d3", "d4", "d5");
}

int main(void)
{
    uint32_t v[12];
    for (int k = 0; k < 12; k++) v[k] = 0x9e3779b9u * (k + 1);
    for (int r = 0; r < 3; r++) dense(v);
    printf("dense:");
    for (int k = 0; k < 12; k++) printf(" %08x", v[k]);
    printf("\n");

    static const char *const msgs[] = {"", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
    static const char *const want[] = {"da39a3ee5e6b4b0d3255bfef95601890afd80709",
                                       "a9993e364706816aba3e25717850c26c9cd0d89d",
                                       "84983e441c3bd26ebaae4aa1f95129e5e54670f1"};
    int bad = 0;
    for (int m = 0; m < 3; m++) {
        uint8_t d[20];
        char hex[41];
        sha1(msgs[m], d);
        for (int k = 0; k < 20; k++) sprintf(hex + 2 * k, "%02x", d[k]);
        printf("sha1(\"%s\") = %s %s\n", msgs[m], hex, strcmp(hex, want[m]) ? "WRONG" : "ok");
        bad |= strcmp(hex, want[m]) != 0;
    }
    return bad;
}
