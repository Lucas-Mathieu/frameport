/* Advanced SIMD (NEON) semantics. Sources are copied out before the destination is written, so overlapping
 * registers behave like the ARM pseudocode. Floating-point lanes use the "standard FPSCR value" (DN, FZ, round to
 * nearest; AHP from FPSCR) like every Advanced SIMD floating-point instruction. */
#include <string.h>
#include "interp.h"
#include "fp_host.h"

typedef __int128 i128;

/* ---------------------------------------------------------------- register buffers */

static void vload(const a32_cpu *c, unsigned reg, unsigned nd, uint8_t *buf)
{
    for (unsigned k = 0; k < nd; k++) memcpy(buf + 8 * k, &c->d[(reg + k) & 31], 8);
}

static void vstore(a32_cpu *c, unsigned reg, unsigned nd, const uint8_t *buf)
{
    for (unsigned k = 0; k < nd; k++) memcpy(&c->d[(reg + k) & 31], buf + 8 * k, 8);
}

static inline uint64_t getu(const uint8_t *b, unsigned idx, unsigned es)
{
    switch (es) {
    case 0: return b[idx];
    case 1: { uint16_t v; memcpy(&v, b + 2 * idx, 2); return v; }
    case 2: { uint32_t v; memcpy(&v, b + 4 * idx, 4); return v; }
    default: { uint64_t v; memcpy(&v, b + 8 * idx, 8); return v; }
    }
}

static inline int64_t gets(const uint8_t *b, unsigned idx, unsigned es)
{
    switch (es) {
    case 0: return (int8_t)b[idx];
    case 1: return (int16_t)getu(b, idx, 1);
    case 2: return (int32_t)getu(b, idx, 2);
    default: return (int64_t)getu(b, idx, 3);
    }
}

/* lane value, sign- or zero-extended (128 bits so unsigned 64-bit lanes stay positive) */
static inline i128 get(const uint8_t *b, unsigned idx, unsigned es, int sgn)
{
    return sgn ? (i128)gets(b, idx, es) : (i128)getu(b, idx, es);
}

static inline void put(uint8_t *b, unsigned idx, unsigned es, uint64_t v)
{
    switch (es) {
    case 0: b[idx] = (uint8_t)v; break;
    case 1: { uint16_t x = (uint16_t)v; memcpy(b + 2 * idx, &x, 2); break; }
    case 2: { uint32_t x = (uint32_t)v; memcpy(b + 4 * idx, &x, 4); break; }
    default: memcpy(b + 8 * idx, &v, 8); break;
    }
}

/* ---------------------------------------------------------------- saturation helpers */

static __thread int qc; /* cumulative saturation (FPSCR.QC) of the current instruction */

static inline i128 smax_of(unsigned es) { return ((i128)1 << (8 * (1u << es) - 1)) - 1; }
static inline i128 smin_of(unsigned es) { return -((i128)1 << (8 * (1u << es) - 1)); }
static inline i128 umax_of(unsigned es) { return ((i128)1 << (8 * (1u << es))) - 1; }

static inline uint64_t sat(i128 v, unsigned es, int sgn)
{
    i128 hi = sgn ? smax_of(es) : umax_of(es), lo = sgn ? smin_of(es) : 0;
    if (v > hi) { qc = 1; return (uint64_t)hi; }
    if (v < lo) { qc = 1; return (uint64_t)lo; }
    return (uint64_t)v;
}

static inline uint64_t emask(unsigned es) { return es == 3 ? ~0ull : ((1ull << (8u << es)) - 1); }

/* Shift by a signed amount (VSHL/VQSHL/VRSHL/VQRSHL register forms and immediate forms). */
static uint64_t shift_any(i128 val, int shift, unsigned es, int sgn, int round, int saturate)
{
    unsigned bits = 8u << es;
    i128 v = val, r;
    (void)sgn;
    if (shift >= 0) {
        if (v == 0) return 0;
        if (shift >= (int)bits + 1) {
            if (!saturate) return 0;
            r = v < 0 ? -((i128)1 << 100) : ((i128)1 << 100);
        } else {
            r = v << shift;
        }
        if (saturate) return sat(r, es, sgn);
        return (uint64_t)r & emask(es);
    }
    int n = -shift;
    if (n > 120) n = 120;
    if (round) r = (v + ((i128)1 << (n - 1))) >> n;
    else r = v >> n;
    if (saturate) return sat(r, es, sgn);
    return (uint64_t)r & emask(es);
}

static uint64_t polymul(uint64_t a, uint64_t b, unsigned bits)
{
    uint64_t r = 0;
    for (unsigned k = 0; k < bits; k++)
        if ((b >> k) & 1) r ^= a << k;
    return r;
}

static void polymul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
    uint64_t l = 0, h = 0;
    for (unsigned k = 0; k < 64; k++)
        if ((b >> k) & 1) {
            l ^= a << k;
            if (k) h ^= a >> (64 - k);
        }
    *lo = l;
    *hi = h;
}

/* ---------------------------------------------------------------- floating point lanes */

#define FCMPV(name, ins)                                                                                           \
    static inline uint32_t name(uint32_t a, uint32_t b)                                                            \
    {                                                                                                              \
        f32 r;                                                                                                     \
        __asm__ volatile(ins " %s0, %s1, %s2" : "=w"(r) : "w"(u2f(a)), "w"(u2f(b)));                               \
        return f2u(r);                                                                                             \
    }
FCMPV(fcmeq_v, "fcmeq") FCMPV(fcmge_v, "fcmge") FCMPV(fcmgt_v, "fcmgt") FCMPV(facge_v, "facge")
FCMPV(facgt_v, "facgt")
#define FCMP0V(name, ins)                                                                                          \
    static inline uint32_t name(uint32_t a)                                                                        \
    {                                                                                                              \
        f32 r;                                                                                                     \
        __asm__ volatile(ins " %s0, %s1, #0.0" : "=w"(r) : "w"(u2f(a)));                                           \
        return f2u(r);                                                                                             \
    }
FCMP0V(fcmgt0_v, "fcmgt") FCMP0V(fcmge0_v, "fcmge") FCMP0V(fcmeq0_v, "fcmeq") FCMP0V(fcmle0_v, "fcmle")
FCMP0V(fcmlt0_v, "fcmlt")

static inline int is_inf(uint32_t a) { return (a & 0x7fffffffu) == 0x7f800000u; }
static inline int is_zero_or_denorm(uint32_t a) { return (a & 0x7f800000u) == 0; }

static uint32_t recip_step(uint32_t a, uint32_t b, int rsqrt)
{
    if ((is_inf(a) && is_zero_or_denorm(b)) || (is_inf(b) && is_zero_or_denorm(a))) {
        if ((a & 0x7fffffffu) && (b & 0x7fffffffu)) set_host_fpsr(host_fpsr() | 0x80); /* input denormal */
        return rsqrt ? 0x3fc00000u : 0x40000000u;
    }
    f32 p = fmul_s(u2f(a), u2f(b));
    if (rsqrt) return f2u(fdiv_s(fsub_s(u2f(0x40400000u), p), u2f(0x40000000u)));
    return f2u(fsub_s(u2f(0x40000000u), p));
}

static uint32_t urecpe(uint32_t a)
{
    uint32_t r;
    __asm__ volatile("fmov s16, %w1\n\turecpe v16.2s, v16.2s\n\tfmov %w0, s16" : "=r"(r) : "r"(a) : "v16");
    return r;
}

static uint32_t ursqrte(uint32_t a)
{
    uint32_t r;
    __asm__ volatile("fmov s16, %w1\n\tursqrte v16.2s, v16.2s\n\tfmov %w0, s16" : "=r"(r) : "r"(a) : "v16");
    return r;
}

static uint32_t flt_rint(uint32_t a, unsigned mode)
{
    f32 x = u2f(a);
    switch (mode) {
    case 0: return f2u(frintn_s(x));
    case 1: return f2u(frintp_s(x));
    case 2: return f2u(frintm_s(x));
    case 3: return f2u(frintz_s(x));
    case 5: return f2u(frintx_s(x));
    default: return f2u(frinta_s(x));
    }
}

static uint32_t f3(unsigned op, uint32_t a, uint32_t b, uint32_t d)
{
    f32 x = u2f(a), y = u2f(b), z = u2f(d);
    switch (op) {
    case N_FADD: case N_FPADD: return f2u(fadd_s(x, y));
    case N_FSUB: return f2u(fsub_s(x, y));
    case N_FABD: return f2u(fabd_s(x, y));
    case N_FMUL: case N_FMUL_S: return f2u(fmul_s(x, y));
    case N_FMLA: case N_FMLA_S: return f2u(fadd_s(z, fmul_s(x, y)));
    case N_FMLS: case N_FMLS_S: return f2u(fadd_s(z, fneg_s(fmul_s(x, y))));
    case N_FCEQ: return fcmeq_v(a, b);
    case N_FCGE: return fcmge_v(a, b);
    case N_FCGT: return fcmgt_v(a, b);
    case N_FACGE: return facge_v(a, b);
    case N_FACGT: return facgt_v(a, b);
    case N_FMAX: case N_FPMAX: return f2u(fmax_s(x, y));
    case N_FMIN: case N_FPMIN: return f2u(fmin_s(x, y));
    case N_FRECPS: return recip_step(a, b, 0);
    case N_FRSQRTS: return recip_step(a, b, 1);
    case N_FMA: return f2u(fmadd_s(x, y, z));
    case N_FMS: return f2u(fmsub_s(x, y, z));
    case N_FMAXNM: return f2u(fmaxnm_s(x, y));
    default: return f2u(fminnm_s(x, y));
    }
}

/* ---------------------------------------------------------------- crypto (host instructions) */

static void crypto(a32_cpu *c, const insn_t *i)
{
    uint8_t d[16], n[16], m[16];
    vload(c, i->vd, 2, d);
    vload(c, i->vn, 2, n);
    vload(c, i->vm, 2, m);
    __asm__ volatile("ld1 {v16.16b}, [%0]\n\tld1 {v17.16b}, [%1]\n\tld1 {v18.16b}, [%2]" ::"r"(d), "r"(n), "r"(m)
                     : "v16", "v17", "v18", "memory");
    switch (i->sub) {
    case N_AESE: __asm__ volatile(".arch_extension crypto\n\taese v16.16b, v18.16b" ::: "v16"); break;
    case N_AESD: __asm__ volatile(".arch_extension crypto\n\taesd v16.16b, v18.16b" ::: "v16"); break;
    case N_AESMC: __asm__ volatile(".arch_extension crypto\n\taesmc v16.16b, v18.16b" ::: "v16"); break;
    case N_AESIMC: __asm__ volatile(".arch_extension crypto\n\taesimc v16.16b, v18.16b" ::: "v16"); break;
    case N_SHA1H: __asm__ volatile(".arch_extension crypto\n\tsha1h s16, s18" ::: "v16"); break;
    case N_SHA1SU1: __asm__ volatile(".arch_extension crypto\n\tsha1su1 v16.4s, v18.4s" ::: "v16"); break;
    case N_SHA256SU0: __asm__ volatile(".arch_extension crypto\n\tsha256su0 v16.4s, v18.4s" ::: "v16"); break;
    case N_SHA1C: __asm__ volatile(".arch_extension crypto\n\tsha1c q16, s17, v18.4s" ::: "v16"); break;
    case N_SHA1P: __asm__ volatile(".arch_extension crypto\n\tsha1p q16, s17, v18.4s" ::: "v16"); break;
    case N_SHA1M: __asm__ volatile(".arch_extension crypto\n\tsha1m q16, s17, v18.4s" ::: "v16"); break;
    case N_SHA1SU0: __asm__ volatile(".arch_extension crypto\n\tsha1su0 v16.4s, v17.4s, v18.4s" ::: "v16"); break;
    case N_SHA256H: __asm__ volatile(".arch_extension crypto\n\tsha256h q16, q17, v18.4s" ::: "v16"); break;
    case N_SHA256H2: __asm__ volatile(".arch_extension crypto\n\tsha256h2 q16, q17, v18.4s" ::: "v16"); break;
    default: __asm__ volatile(".arch_extension crypto\n\tsha256su1 v16.4s, v17.4s, v18.4s" ::: "v16"); break;
    }
    __asm__ volatile("st1 {v16.16b}, [%0]" ::"r"(d) : "memory");
    vstore(c, i->vd, 2, d);
}

/* ---------------------------------------------------------------- 3 registers, same length */

static void three_same(a32_cpu *c, const insn_t *i)
{
    unsigned nd = i->q ? 2 : 1, es = i->esize, lanes = (nd * 8) >> es, op = i->sub;
    int sgn = (i->flags & VF_SIGNED) != 0;
    uint8_t n[16], m[16], d[16], r[16];
    vload(c, i->vn, nd, n);
    vload(c, i->vm, nd, m);
    vload(c, i->vd, nd, d);
    if (op >= N_VAND && op <= N_VBIF) {
        for (unsigned k = 0; k < nd * 8; k++) {
            uint8_t a = n[k], b = m[k], x = d[k];
            switch (op) {
            case N_VAND: r[k] = a & b; break;
            case N_VBIC: r[k] = a & ~b; break;
            case N_VORR: r[k] = a | b; break;
            case N_VORN: r[k] = a | ~b; break;
            case N_VEOR: r[k] = a ^ b; break;
            case N_VBSL: r[k] = (x & a) | (~x & b); break;
            case N_VBIT: r[k] = (x & ~b) | (a & b); break;
            default: r[k] = (x & b) | (a & ~b); break;
            }
        }
        vstore(c, i->vd, nd, r);
        return;
    }
    if (op == N_VPMAX || op == N_VPMIN || op == N_VPADD || op == N_FPADD || op == N_FPMAX || op == N_FPMIN) {
        unsigned half = lanes / 2;
        for (unsigned k = 0; k < lanes; k++) {
            const uint8_t *src = k < half ? n : m;
            unsigned j = 2 * (k % half);
            uint64_t v;
            if (op >= N_FADD) {
                v = f3(op, (uint32_t)getu(src, j, 2), (uint32_t)getu(src, j + 1, 2), 0);
            } else {
                i128 a = get(src, j, es, sgn), b = get(src, j + 1, es, sgn);
                v = op == N_VPADD ? (uint64_t)(a + b) : (uint64_t)((op == N_VPMAX) == (a > b) ? a : b);
            }
            put(r, k, es, v);
        }
        vstore(c, i->vd, nd, r);
        return;
    }
    for (unsigned k = 0; k < lanes; k++) {
        i128 a = get(n, k, es, sgn), b = get(m, k, es, sgn), x = get(d, k, es, sgn);
        uint64_t v;
        switch (op) {
        case N_VHADD: v = (uint64_t)((a + b) >> 1); break;
        case N_VRHADD: v = (uint64_t)((a + b + 1) >> 1); break;
        case N_VHSUB: v = (uint64_t)((a - b) >> 1); break;
        case N_VQADD: v = sat(a + b, es, sgn); break;
        case N_VQSUB: v = sat(a - b, es, sgn); break;
        case N_VCGT: v = a > b ? ~0ull : 0; break;
        case N_VCGE: v = a >= b ? ~0ull : 0; break;
        case N_VSHL: v = shift_any(b, (int8_t)a, es, sgn, 0, 0); break;
        case N_VQSHL: v = shift_any(b, (int8_t)a, es, sgn, 0, 1); break;
        case N_VRSHL: v = shift_any(b, (int8_t)a, es, sgn, 1, 0); break;
        case N_VQRSHL: v = shift_any(b, (int8_t)a, es, sgn, 1, 1); break;
        case N_VMAX: v = (uint64_t)(a > b ? a : b); break;
        case N_VMIN: v = (uint64_t)(a < b ? a : b); break;
        case N_VABD: { i128 t = a - b; v = (uint64_t)(t < 0 ? -t : t); break; }
        case N_VABA: { i128 t = a - b; v = (uint64_t)x + (uint64_t)(t < 0 ? -t : t); break; }
        case N_VADD: v = (uint64_t)a + (uint64_t)b; break;
        case N_VSUB: v = (uint64_t)a - (uint64_t)b; break;
        case N_VTST: v = ((uint64_t)a & (uint64_t)b & emask(es)) ? ~0ull : 0; break;
        case N_VCEQ: v = ((uint64_t)(a ^ b) & emask(es)) == 0 ? ~0ull : 0; break;
        case N_VMLA: v = (uint64_t)x + (uint64_t)a * (uint64_t)b; break;
        case N_VMLS: v = (uint64_t)x - (uint64_t)a * (uint64_t)b; break;
        case N_VMUL: v = (uint64_t)a * (uint64_t)b; break;
        case N_VMULP: v = polymul((uint64_t)a & 0xff, (uint64_t)b & 0xff, 8); break;
        case N_VQDMULH: v = sat((2 * a * b) >> (8u << es), es, 1); break;
        case N_VQRDMULH: v = sat((2 * a * b + ((i128)1 << ((8u << es) - 1))) >> (8u << es), es, 1); break;
        default: v = f3(op, (uint32_t)a, (uint32_t)b, (uint32_t)x); break;
        }
        put(r, k, es, v);
    }
    vstore(c, i->vd, nd, r);
}

/* ---------------------------------------------------------------- 3 registers, different lengths */

static void three_diff(a32_cpu *c, const insn_t *i)
{
    unsigned es = i->esize, op = i->sub, lanes = 8 >> es;
    int sgn = (i->flags & VF_SIGNED) != 0;
    uint8_t n[16], m[16], d[16], r[16];
    int wide_n = op == N_VADDW || op == N_VSUBW || op == N_VADDHN || op == N_VRADDHN || op == N_VSUBHN ||
                 op == N_VRSUBHN;
    int narrow = op == N_VADDHN || op == N_VRADDHN || op == N_VSUBHN || op == N_VRSUBHN;
    vload(c, i->vn, wide_n ? 2 : 1, n);
    vload(c, i->vm, narrow ? 2 : 1, m);
    vload(c, i->vd, 2, d);
    if (op == N_VMULLP && es == 2) { /* VMULL.P64 */
        uint64_t lo, hi;
        polymul64(getu(n, 0, 3), getu(m, 0, 3), &lo, &hi);
        put(r, 0, 3, lo);
        put(r, 1, 3, hi);
        vstore(c, i->vd, 2, r);
        return;
    }
    for (unsigned k = 0; k < lanes; k++) {
        i128 a = wide_n ? get(n, k, es + 1, sgn) : get(n, k, es, sgn);
        i128 b = narrow ? get(m, k, es + 1, sgn) : get(m, k, es, sgn);
        i128 x = get(d, k, es + 1, sgn);
        uint64_t v;
        unsigned hb = 8u << es;
        switch (op) {
        case N_VADDL: case N_VADDW: v = (uint64_t)a + (uint64_t)b; break;
        case N_VSUBL: case N_VSUBW: v = (uint64_t)a - (uint64_t)b; break;
        case N_VADDHN: v = ((uint64_t)a + (uint64_t)b) >> hb; break;
        case N_VRADDHN: v = ((uint64_t)a + (uint64_t)b + (1ull << (hb - 1))) >> hb; break;
        case N_VSUBHN: v = ((uint64_t)a - (uint64_t)b) >> hb; break;
        case N_VRSUBHN: v = ((uint64_t)a - (uint64_t)b + (1ull << (hb - 1))) >> hb; break;
        case N_VABAL: { i128 t = a - b; v = (uint64_t)x + (uint64_t)(t < 0 ? -t : t); break; }
        case N_VABDL: { i128 t = a - b; v = (uint64_t)(t < 0 ? -t : t); break; }
        case N_VMLAL: v = (uint64_t)x + (uint64_t)a * (uint64_t)b; break;
        case N_VMLSL: v = (uint64_t)x - (uint64_t)a * (uint64_t)b; break;
        case N_VMULL: v = (uint64_t)a * (uint64_t)b; break;
        case N_VMULLP: v = polymul((uint64_t)a & 0xff, (uint64_t)b & 0xff, 8); break;
        case N_VQDMULL: v = sat(2 * a * b, es + 1, 1); break;
        case N_VQDMLAL: { i128 p = (int64_t)sat(2 * a * b, es + 1, 1); v = sat(x + p, es + 1, 1); break; }
        default: { i128 p = (int64_t)sat(2 * a * b, es + 1, 1); v = sat(x - p, es + 1, 1); break; }
        }
        if (narrow) put(r, k, es, v);
        else put(r, k, es + 1, v);
    }
    vstore(c, i->vd, narrow ? 1 : 2, r);
}

/* ---------------------------------------------------------------- 2 registers and a scalar */

static void by_scalar(a32_cpu *c, const insn_t *i)
{
    unsigned es = i->esize, op = i->sub;
    int sgn = (i->flags & VF_SIGNED) != 0;
    int lng = op == N_VMLAL_S || op == N_VMLSL_S || op == N_VQDMLAL_S || op == N_VQDMLSL_S || op == N_VMULL_S ||
              op == N_VQDMULL_S;
    unsigned nd = lng ? 1 : (i->q ? 2 : 1), lanes = (nd * 8) >> es;
    uint8_t n[16], sm[8], d[16], r[16];
    vload(c, i->vn, nd, n);
    vload(c, i->vm, 1, sm);
    vload(c, i->vd, lng ? 2 : nd, d);
    int qd = op == N_VQDMULH_S || op == N_VQRDMULH_S || op == N_VQDMLAL_S || op == N_VQDMLSL_S || op == N_VQDMULL_S;
    i128 s = get(sm, i->imm, es, sgn || qd);
    for (unsigned k = 0; k < lanes; k++) {
        i128 a = get(n, k, es, sgn || qd);
        uint64_t v;
        if (op == N_FMLA_S || op == N_FMLS_S || op == N_FMUL_S) {
            v = f3(op, (uint32_t)a, (uint32_t)s, (uint32_t)getu(d, k, 2));
            put(r, k, es, v);
            continue;
        }
        if (lng) {
            i128 x = get(d, k, es + 1, sgn || qd);
            switch (op) {
            case N_VMLAL_S: v = (uint64_t)x + (uint64_t)a * (uint64_t)s; break;
            case N_VMLSL_S: v = (uint64_t)x - (uint64_t)a * (uint64_t)s; break;
            case N_VMULL_S: v = (uint64_t)a * (uint64_t)s; break;
            case N_VQDMULL_S: v = sat(2 * a * s, es + 1, 1); break;
            case N_VQDMLAL_S: { i128 p = (int64_t)sat(2 * a * s, es + 1, 1); v = sat(x + p, es + 1, 1); break; }
            default: { i128 p = (int64_t)sat(2 * a * s, es + 1, 1); v = sat(x - p, es + 1, 1); break; }
            }
            put(r, k, es + 1, v);
            continue;
        }
        uint64_t x = getu(d, k, es);
        switch (op) {
        case N_VMLA_S: v = x + (uint64_t)a * (uint64_t)s; break;
        case N_VMLS_S: v = x - (uint64_t)a * (uint64_t)s; break;
        case N_VMUL_S: v = (uint64_t)a * (uint64_t)s; break;
        case N_VQDMULH_S: v = sat((2 * a * s) >> (8u << es), es, 1); break;
        default: v = sat((2 * a * s + ((i128)1 << ((8u << es) - 1))) >> (8u << es), es, 1); break;
        }
        put(r, k, es, v);
    }
    vstore(c, i->vd, lng ? 2 : nd, r);
}

/* ---------------------------------------------------------------- shifts by immediate */

static void shift_imm(a32_cpu *c, const insn_t *i)
{
    unsigned es = i->esize, op = i->sub, sh = i->imm;
    int sgn = (i->flags & VF_SIGNED) != 0;
    uint8_t m[16], d[16], r[16];
    if (op == N_VSHRN || op == N_VRSHRN || op == N_VQSHRUN || op == N_VQRSHRUN || op == N_VQSHRN || op == N_VQRSHRN) {
        unsigned lanes = 8 >> es;
        vload(c, i->vm, 2, m);
        int round = op == N_VRSHRN || op == N_VQRSHRUN || op == N_VQRSHRN;
        for (unsigned k = 0; k < lanes; k++) {
            i128 a = get(m, k, es + 1, sgn);
            i128 v = round ? ((a + ((i128)1 << (sh - 1))) >> sh) : (a >> sh);
            uint64_t x;
            if (op == N_VSHRN || op == N_VRSHRN) x = (uint64_t)v;
            else if (op == N_VQSHRUN || op == N_VQRSHRUN) x = sat(v, es, 0);
            else x = sat(v, es, sgn);
            put(r, k, es, x);
        }
        vstore(c, i->vd, 1, r);
        return;
    }
    if (op == N_VSHLL || op == N_VMOVL) {
        unsigned lanes = 8 >> es;
        vload(c, i->vm, 1, m);
        for (unsigned k = 0; k < lanes; k++) put(r, k, es + 1, (uint64_t)get(m, k, es, sgn) << sh);
        vstore(c, i->vd, 2, r);
        return;
    }
    unsigned nd = i->q ? 2 : 1, lanes = (nd * 8) >> es, bits = 8u << es;
    vload(c, i->vm, nd, m);
    vload(c, i->vd, nd, d);
    if (op == N_VCVT_FIXF) {
        fp_saved s = fp_begin(FPCR_STANDARD | (c->fpcr & FPCR_AHP));
        for (unsigned k = 0; k < lanes; k++) {
            uint32_t a = (uint32_t)getu(m, k, 2), v;
            if (i->flags & VF_TOFIX) v = cvt_s2fix(u2f(a), sh, sgn);
            else v = f2u(cvt_fix2s(a, sh, sgn));
            put(r, k, 2, v);
        }
        c->fpsr |= fp_end(s);
        vstore(c, i->vd, nd, r);
        return;
    }
    for (unsigned k = 0; k < lanes; k++) {
        i128 a = get(m, k, es, sgn);
        uint64_t x = getu(d, k, es), v, mask;
        switch (op) {
        case N_VSHR: v = shift_any(a, -(int)sh, es, sgn, 0, 0); break;
        case N_VSRA: v = x + shift_any(a, -(int)sh, es, sgn, 0, 0); break;
        case N_VRSHR: v = shift_any(a, -(int)sh, es, sgn, 1, 0); break;
        case N_VRSRA: v = x + shift_any(a, -(int)sh, es, sgn, 1, 0); break;
        case N_VSRI:
            mask = sh >= bits ? 0 : emask(es) >> sh;
            v = (x & ~mask) | ((sh >= bits ? 0 : (uint64_t)a >> sh) & mask);
            break;
        case N_VSHL_I: v = (uint64_t)a << sh; break;
        case N_VSLI:
            mask = (emask(es) << sh) & emask(es);
            v = (x & ~mask) | (((uint64_t)a << sh) & mask);
            break;
        case N_VQSHLU_I: v = sat(a << sh, es, 0); break;
        default: v = sat(a << sh, es, sgn); break; /* VQSHL imm */
        }
        put(r, k, es, v);
    }
    vstore(c, i->vd, nd, r);
}

/* ---------------------------------------------------------------- 2 registers, miscellaneous */

static void misc(a32_cpu *c, const insn_t *i)
{
    unsigned nd = i->q ? 2 : 1, es = i->esize, lanes = (nd * 8) >> es, op = i->sub;
    int sgn = (i->flags & VF_SIGNED) != 0;
    uint8_t m[16], d[16], r[16];
    vload(c, i->vm, nd, m);
    vload(c, i->vd, nd, d);
    switch (op) {
    case N_VREV64: case N_VREV32: case N_VREV16: {
        unsigned cont = op == N_VREV64 ? 8 : op == N_VREV32 ? 4 : 2, per = cont >> es;
        for (unsigned k = 0; k < lanes; k++) {
            unsigned base = k - k % per, j = base + (per - 1 - k % per);
            put(r, k, es, getu(m, j, es));
        }
        vstore(c, i->vd, nd, r);
        return;
    }
    case N_VPADDL: case N_VPADAL: {
        for (unsigned k = 0; k < lanes / 2; k++) {
            uint64_t v = (uint64_t)get(m, 2 * k, es, sgn) + (uint64_t)get(m, 2 * k + 1, es, sgn);
            if (op == N_VPADAL) v += getu(d, k, es + 1);
            put(r, k, es + 1, v);
        }
        vstore(c, i->vd, nd, r);
        return;
    }
    case N_VSWP: {
        vstore(c, i->vd, nd, m);
        vstore(c, i->vm, nd, d);
        return;
    }
    case N_VTRN: {
        uint8_t rd[16], rm[16];
        memcpy(rd, d, sizeof rd);
        memcpy(rm, m, sizeof rm);
        for (unsigned k = 0; k + 1 < lanes; k += 2) {
            put(rd, k + 1, es, getu(m, k, es));
            put(rm, k, es, getu(d, k + 1, es));
        }
        vstore(c, i->vd, nd, rd);
        vstore(c, i->vm, nd, rm);
        return;
    }
    case N_VUZP: case N_VZIP: {
        uint8_t cat[32], out[32];
        memcpy(cat, d, nd * 8);
        memcpy(cat + nd * 8, m, nd * 8);
        for (unsigned k = 0; k < lanes; k++) {
            if (op == N_VUZP) {
                put(out, k, es, getu(cat, 2 * k, es));
                put(out, lanes + k, es, getu(cat, 2 * k + 1, es));
            } else {
                put(out, 2 * k, es, getu(d, k, es));
                put(out, 2 * k + 1, es, getu(m, k, es));
            }
        }
        vstore(c, i->vd, nd, out);
        vstore(c, i->vm, nd, out + nd * 8);
        return;
    }
    case N_VMOVN: case N_VQMOVUN: case N_VQMOVN: {
        uint8_t w[16];
        vload(c, i->vm, 2, w);
        for (unsigned k = 0; k < (8u >> es); k++) {
            i128 a = get(w, k, es + 1, sgn);
            uint64_t v = op == N_VMOVN ? (uint64_t)a : op == N_VQMOVUN ? sat(a, es, 0) : sat(a, es, sgn);
            put(r, k, es, v);
        }
        vstore(c, i->vd, 1, r);
        return;
    }
    case N_VSHLL_MAX: {
        uint8_t s[8];
        vload(c, i->vm, 1, s);
        for (unsigned k = 0; k < (8u >> es); k++) put(r, k, es + 1, getu(s, k, es) << i->imm);
        vstore(c, i->vd, 2, r);
        return;
    }
    case N_VCVT_HS: {
        fp_saved sv = fp_begin(FPCR_STANDARD | (c->fpcr & FPCR_AHP));
        uint8_t w[16];
        if (i->flags & VF_TOHALF) {
            vload(c, i->vm, 2, w);
            for (unsigned k = 0; k < 4; k++) put(r, k, 1, cvt_s2h(u2f((uint32_t)getu(w, k, 2))));
            c->fpsr |= fp_end(sv);
            vstore(c, i->vd, 1, r);
        } else {
            vload(c, i->vm, 1, w);
            for (unsigned k = 0; k < 4; k++) put(r, k, 2, f2u(cvt_h2s((uint16_t)getu(w, k, 1))));
            c->fpsr |= fp_end(sv);
            vstore(c, i->vd, 2, r);
        }
        return;
    }
    case N_AESE: case N_AESD: case N_AESMC: case N_AESIMC: case N_SHA1H: case N_SHA1SU1: case N_SHA256SU0:
        crypto(c, i);
        return;
    default:
        break;
    }
    int fl = (op >= N_FCGT0 && op <= N_FCLT0) || op == N_FRECPE || op == N_FRSQRTE || op == N_VCVT_FI ||
             op == N_FRINT || op == N_VCVT_RND;
    fp_saved sv = {0, 0};
    if (fl) sv = fp_begin(FPCR_STANDARD | (c->fpcr & FPCR_AHP));
    for (unsigned k = 0; k < lanes; k++) {
        i128 a = get(m, k, es, op == N_VQABS || op == N_VQNEG || op == N_VABS || op == N_VNEG || op == N_VCLS ||
                               (op >= N_VCGT0 && op <= N_VCLT0));
        uint64_t ua = getu(m, k, es), v;
        unsigned bits = 8u << es;
        switch (op) {
        case N_VCLS: {
            uint64_t x = (uint64_t)(a < 0 ? ~a : a) & emask(es);
            unsigned n = 0;
            for (int b = bits - 2; b >= 0 && !((x >> b) & 1); b--) n++;
            v = n;
            break;
        }
        case N_VCLZ: {
            unsigned n = 0;
            for (int b = bits - 1; b >= 0 && !((ua >> b) & 1); b--) n++;
            v = n;
            break;
        }
        case N_VCNT: v = __builtin_popcountll(ua); break;
        case N_VMVN: v = ~ua; break;
        case N_VQABS: v = sat(a < 0 ? -a : a, es, 1); break;
        case N_VQNEG: v = sat(-a, es, 1); break;
        case N_VCGT0: v = a > 0 ? ~0ull : 0; break;
        case N_VCGE0: v = a >= 0 ? ~0ull : 0; break;
        case N_VCEQ0: v = a == 0 ? ~0ull : 0; break;
        case N_VCLE0: v = a <= 0 ? ~0ull : 0; break;
        case N_VCLT0: v = a < 0 ? ~0ull : 0; break;
        case N_VABS: v = (uint64_t)(a < 0 ? -a : a); break;
        case N_VNEG: v = (uint64_t)-a; break;
        case N_FCGT0: v = fcmgt0_v((uint32_t)ua); break;
        case N_FCGE0: v = fcmge0_v((uint32_t)ua); break;
        case N_FCEQ0: v = fcmeq0_v((uint32_t)ua); break;
        case N_FCLE0: v = fcmle0_v((uint32_t)ua); break;
        case N_FCLT0: v = fcmlt0_v((uint32_t)ua); break;
        case N_FABS: v = ua & 0x7fffffffu; break;
        case N_FNEG: v = ua ^ 0x80000000u; break;
        case N_VRECPE: v = urecpe((uint32_t)ua); break;
        case N_VRSQRTE: v = ursqrte((uint32_t)ua); break;
        case N_FRECPE: v = f2u(frecpe_s(u2f((uint32_t)ua))); break;
        case N_FRSQRTE: v = f2u(frsqrte_s(u2f((uint32_t)ua))); break;
        case N_VCVT_FI:
            if (i->flags & VF_TOFIX) v = cvt_s2i(u2f((uint32_t)ua), 3, sgn);
            else v = f2u(cvt_i2s((uint32_t)ua, sgn));
            break;
        case N_FRINT: v = flt_rint((uint32_t)ua, i->imm); break;
        case N_VCVT_RND: v = cvt_s2i(u2f((uint32_t)ua), i->imm, sgn); break;
        default: v = ua; break;
        }
        put(r, k, es, v);
    }
    if (fl) c->fpsr |= fp_end(sv);
    vstore(c, i->vd, nd, r);
}

/* ---------------------------------------------------------------- element and structure loads/stores */

static void ld_el(uint8_t *reg, unsigned idx, unsigned es, uint32_t a)
{
    switch (es) {
    case 0: put(reg, idx, 0, ld8(a)); break;
    case 1: put(reg, idx, 1, ld16(a)); break;
    case 2: put(reg, idx, 2, ld32(a)); break;
    default: put(reg, idx, 3, ld64(a)); break;
    }
}

static void st_el(const uint8_t *reg, unsigned idx, unsigned es, uint32_t a)
{
    switch (es) {
    case 0: st8(a, (uint8_t)getu(reg, idx, 0)); break;
    case 1: st16(a, (uint16_t)getu(reg, idx, 1)); break;
    case 2: st32(a, (uint32_t)getu(reg, idx, 2)); break;
    default: st64(a, getu(reg, idx, 3)); break;
    }
}

static void ldst_struct(a32_cpu *c, const insn_t *i)
{
    unsigned es = i->esize, eb = 1u << es, n = i->imm, load = (i->flags & VF_LOAD) != 0;
    uint32_t a = c->r[i->rn], start = a, total;
    uint8_t regs[32 * 8];
    memcpy(regs, c->d, sizeof regs);
#define DREG(r) (regs + 8 * ((r) & 31))
    if (i->sub == N_VLDST_MULTI) {
        unsigned elements = 8 / eb;
        if (n == 1) {
            for (unsigned r = 0; r < i->nregs; r++)
                for (unsigned e = 0; e < elements; e++, a += eb) {
                    if (load) ld_el(DREG(i->vd + r), e, es, a);
                    else st_el(DREG(i->vd + r), e, es, a);
                }
            total = 8 * i->nregs;
        } else {
            unsigned regs_per = i->nregs; /* VLD2 with 4 registers: 2 */
            for (unsigned r = 0; r < regs_per; r++)
                for (unsigned e = 0; e < elements; e++)
                    for (unsigned s = 0; s < n; s++, a += eb) {
                        unsigned reg = i->vd + r + s * i->inc;
                        if (load) ld_el(DREG(reg), e, es, a);
                        else st_el(DREG(reg), e, es, a);
                    }
            total = 8 * n * regs_per;
        }
    } else if (i->sub == N_VLDST_LANE) {
        for (unsigned s = 0; s < n; s++, a += eb) {
            unsigned reg = i->vd + s * i->inc;
            if (load) ld_el(DREG(reg), i->imm2, es, a);
            else st_el(DREG(reg), i->imm2, es, a);
        }
        total = n * eb;
    } else { /* to all lanes */
        unsigned elements = 8 / eb;
        if (n == 1) {
            uint8_t tmp[8];
            ld_el(tmp, 0, es, a);
            uint64_t v = getu(tmp, 0, es);
            for (unsigned r = 0; r < i->nregs; r++)
                for (unsigned e = 0; e < elements; e++) put(DREG(i->vd + r), e, es, v);
            total = eb;
        } else {
            for (unsigned s = 0; s < n; s++, a += eb) {
                uint8_t tmp[8];
                ld_el(tmp, 0, es, a);
                uint64_t v = getu(tmp, 0, es);
                for (unsigned e = 0; e < elements; e++) put(DREG(i->vd + s * i->inc), e, es, v);
            }
            total = n * eb;
        }
    }
#undef DREG
    if (load) memcpy(c->d, regs, sizeof regs);
    if (i->rm != 15) c->r[i->rn] = start + (i->rm == 13 ? total : c->r[i->rm]);
}

/* ---------------------------------------------------------------- dispatch */

int a32_exec_neon(a32_cpu *c, const insn_t *i)
{
    unsigned op = i->sub;
    qc = 0;
    if (op == N_VLDST_MULTI || op == N_VLDST_LANE || op == N_VLD_ALL) {
        ldst_struct(c, i);
        return EXIT_NONE;
    }
    if (op <= N_FMINNM) {
        if (op >= N_FADD) {
            fp_saved s = fp_begin(FPCR_STANDARD | (c->fpcr & FPCR_AHP));
            three_same(c, i);
            c->fpsr |= fp_end(s);
        } else {
            three_same(c, i);
        }
    } else if (op <= N_VMULLP) {
        three_diff(c, i);
    } else if (op <= N_FMUL_S) {
        if (op >= N_FMLA_S) {
            fp_saved s = fp_begin(FPCR_STANDARD | (c->fpcr & FPCR_AHP));
            by_scalar(c, i);
            c->fpsr |= fp_end(s);
        } else {
            by_scalar(c, i);
        }
    } else if (op <= N_VCVT_FIXF) {
        shift_imm(c, i);
    } else if (op <= N_VBIC_I) {
        unsigned nd = i->q ? 2 : 1;
        for (unsigned k = 0; k < nd; k++) {
            uint64_t *d = &c->d[(i->vd + k) & 31];
            switch (op) {
            case N_VMOV_I: case N_VMVN_I: *d = i->imm64; break;
            case N_VORR_I: *d |= i->imm64; break;
            default: *d &= i->imm64; break;
            }
        }
    } else if (op <= N_SHA256SU0) {
        misc(c, i);
    } else if (op == N_VEXT) {
        unsigned nb = i->q ? 16 : 8;
        uint8_t n[16], m[16], r[16];
        vload(c, i->vn, nb / 8, n);
        vload(c, i->vm, nb / 8, m);
        for (unsigned k = 0; k < nb; k++) {
            unsigned j = k + i->imm;
            r[k] = j < nb ? n[j] : m[j - nb];
        }
        vstore(c, i->vd, nb / 8, r);
    } else if (op == N_VTBL) {
        uint8_t t[32], m[8], d[8], r[8];
        vload(c, i->vn, i->nregs, t);
        vload(c, i->vm, 1, m);
        vload(c, i->vd, 1, d);
        for (unsigned k = 0; k < 8; k++) r[k] = m[k] < 8 * i->nregs ? t[m[k]] : ((i->flags & VF_E) ? d[k] : 0);
        vstore(c, i->vd, 1, r);
    } else if (op == N_VDUP_S) {
        uint8_t m[8], r[16];
        unsigned nd = i->q ? 2 : 1;
        vload(c, i->vm, 1, m);
        uint64_t v = getu(m, i->imm, i->esize);
        for (unsigned k = 0; k < (nd * 8 >> i->esize); k++) put(r, k, i->esize, v);
        vstore(c, i->vd, nd, r);
    } else if (op <= N_SHA256SU1) {
        crypto(c, i);
    } else {
        return EXIT_UNDEF;
    }
    if (qc) c->fpsr |= FPSR_QC;
    return EXIT_NONE;
}
