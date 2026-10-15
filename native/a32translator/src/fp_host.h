/* Host (A64) floating-point primitives. AArch32 VFP/NEON and A64 share the IEEE operations, NaN propagation,
 * flush-to-zero, default-NaN and exception flags, and FPCR/FPSR use the FPSCR bit positions, so each guest
 * operation is the matching A64 instruction run with the guest's control bits in FPCR. Everything is volatile
 * inline asm so the compiler can neither fold, reorder nor commute operands (NaN propagation is order dependent). */
#ifndef A32_FP_HOST_H
#define A32_FP_HOST_H

#include <stdint.h>
#include "cpu.h"

static inline uint64_t host_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void set_host_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" ::"r"(v)); }
static inline uint64_t host_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void set_host_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" ::"r"(v)); }

/* Runs guest FP operations with the given control value; flags are collected into the guest's FPSR. */
typedef struct { uint64_t fpcr, fpsr; } fp_saved;
static inline fp_saved fp_begin(uint32_t ctl)
{
    fp_saved s = {host_fpcr(), host_fpsr()};
    if (s.fpcr != ctl) set_host_fpcr(ctl);
    set_host_fpsr(0);
    return s;
}
static inline uint32_t fp_end(fp_saved s)
{
    uint32_t f = (uint32_t)host_fpsr() & FPSR_MASK;
    set_host_fpcr(s.fpcr);
    set_host_fpsr(s.fpsr);
    return f;
}

#define FP2(name, ins, T, reg)                                                                                     \
    static inline T name(T a, T b)                                                                                 \
    {                                                                                                              \
        T r;                                                                                                       \
        __asm__ volatile(ins " %" reg "0, %" reg "1, %" reg "2" : "=w"(r) : "w"(a), "w"(b));                       \
        return r;                                                                                                  \
    }
#define FP1(name, ins, T, reg)                                                                                     \
    static inline T name(T a)                                                                                      \
    {                                                                                                              \
        T r;                                                                                                       \
        __asm__ volatile(ins " %" reg "0, %" reg "1" : "=w"(r) : "w"(a));                                          \
        return r;                                                                                                  \
    }
#define FP3(name, ins, T, reg)                                                                                     \
    static inline T name(T n, T m, T a)                                                                            \
    {                                                                                                              \
        T r;                                                                                                       \
        __asm__ volatile(ins " %" reg "0, %" reg "1, %" reg "2, %" reg "3" : "=w"(r) : "w"(n), "w"(m), "w"(a));    \
        return r;                                                                                                  \
    }

typedef float f32;
typedef double f64;

FP2(fadd_s, "fadd", f32, "s") FP2(fadd_d, "fadd", f64, "d")
FP2(fsub_s, "fsub", f32, "s") FP2(fsub_d, "fsub", f64, "d")
FP2(fmul_s, "fmul", f32, "s") FP2(fmul_d, "fmul", f64, "d")
FP2(fdiv_s, "fdiv", f32, "s") FP2(fdiv_d, "fdiv", f64, "d")
FP2(fnmul_s, "fnmul", f32, "s") FP2(fnmul_d, "fnmul", f64, "d")
FP2(fmax_s, "fmax", f32, "s") FP2(fmax_d, "fmax", f64, "d")
FP2(fmin_s, "fmin", f32, "s") FP2(fmin_d, "fmin", f64, "d")
FP2(fmaxnm_s, "fmaxnm", f32, "s") FP2(fmaxnm_d, "fmaxnm", f64, "d")
FP2(fminnm_s, "fminnm", f32, "s") FP2(fminnm_d, "fminnm", f64, "d")
FP2(fabd_s, "fabd", f32, "s")
FP2(frecps_s, "frecps", f32, "s") FP2(frsqrts_s, "frsqrts", f32, "s")
FP1(fneg_s, "fneg", f32, "s") FP1(fneg_d, "fneg", f64, "d")
FP1(fabs_s, "fabs", f32, "s") FP1(fabs_d, "fabs", f64, "d")
FP1(fsqrt_s, "fsqrt", f32, "s") FP1(fsqrt_d, "fsqrt", f64, "d")
FP1(frintn_s, "frintn", f32, "s") FP1(frintn_d, "frintn", f64, "d")
FP1(frintp_s, "frintp", f32, "s") FP1(frintp_d, "frintp", f64, "d")
FP1(frintm_s, "frintm", f32, "s") FP1(frintm_d, "frintm", f64, "d")
FP1(frintz_s, "frintz", f32, "s") FP1(frintz_d, "frintz", f64, "d")
FP1(frinta_s, "frinta", f32, "s") FP1(frinta_d, "frinta", f64, "d")
FP1(frinti_s, "frinti", f32, "s") FP1(frinti_d, "frinti", f64, "d")
FP1(frintx_s, "frintx", f32, "s") FP1(frintx_d, "frintx", f64, "d")
FP1(frecpe_s, "frecpe", f32, "s") FP1(frsqrte_s, "frsqrte", f32, "s")
FP3(fmadd_s, "fmadd", f32, "s") FP3(fmadd_d, "fmadd", f64, "d")
FP3(fmsub_s, "fmsub", f32, "s") FP3(fmsub_d, "fmsub", f64, "d")
FP3(fnmadd_s, "fnmadd", f32, "s") FP3(fnmadd_d, "fnmadd", f64, "d")
FP3(fnmsub_s, "fnmsub", f32, "s") FP3(fnmsub_d, "fnmsub", f64, "d")

static inline f32 u2f(uint32_t v) { union { uint32_t u; f32 f; } x = {v}; return x.f; }
static inline uint32_t f2u(f32 v) { union { f32 f; uint32_t u; } x = {v}; return x.u; }
static inline f64 u2d(uint64_t v) { union { uint64_t u; f64 f; } x = {v}; return x.f; }
static inline uint64_t d2u(f64 v) { union { f64 f; uint64_t u; } x = {v}; return x.u; }

/* NZCV of an FP compare (A64 FCMP/FCMPE set the same flags as AArch32 VCMP). */
static inline uint32_t fcmp_s(f32 a, f32 b, int e)
{
    uint64_t f;
    if (e) __asm__ volatile("fcmpe %s1, %s2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
    else __asm__ volatile("fcmp %s1, %s2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
    return (uint32_t)f;
}
static inline uint32_t fcmp_d(f64 a, f64 b, int e)
{
    uint64_t f;
    if (e) __asm__ volatile("fcmpe %d1, %d2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
    else __asm__ volatile("fcmp %d1, %d2\n\tmrs %0, nzcv" : "=r"(f) : "w"(a), "w"(b) : "cc");
    return (uint32_t)f;
}
static inline uint32_t fcmp0_s(f32 a, int e)
{
    uint64_t f;
    if (e) __asm__ volatile("fcmpe %s1, #0.0\n\tmrs %0, nzcv" : "=r"(f) : "w"(a) : "cc");
    else __asm__ volatile("fcmp %s1, #0.0\n\tmrs %0, nzcv" : "=r"(f) : "w"(a) : "cc");
    return (uint32_t)f;
}
static inline uint32_t fcmp0_d(f64 a, int e)
{
    uint64_t f;
    if (e) __asm__ volatile("fcmpe %d1, #0.0\n\tmrs %0, nzcv" : "=r"(f) : "w"(a) : "cc");
    else __asm__ volatile("fcmp %d1, #0.0\n\tmrs %0, nzcv" : "=r"(f) : "w"(a) : "cc");
    return (uint32_t)f;
}

/* Conversions. rmode: 0 RN, 1 RP, 2 RM, 3 RZ, 6 RA (ties away). */
#define CVT_TO_INT(name, T, reg, wreg)                                                                             \
    static inline uint32_t name(T a, int rmode, int sgn)                                                           \
    {                                                                                                              \
        uint32_t r;                                                                                                \
        switch (rmode * 2 + sgn) {                                                                                 \
        case 0: __asm__ volatile("fcvtnu %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 1: __asm__ volatile("fcvtns %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 2: __asm__ volatile("fcvtpu %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 3: __asm__ volatile("fcvtps %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 4: __asm__ volatile("fcvtmu %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 5: __asm__ volatile("fcvtms %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 6: __asm__ volatile("fcvtzu %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 7: __asm__ volatile("fcvtzs %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                               \
        case 12: __asm__ volatile("fcvtau %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                              \
        default: __asm__ volatile("fcvtas %w0, %" reg "1" : "=r"(r) : "w"(a)); break;                              \
        }                                                                                                          \
        return r;                                                                                                  \
    }
CVT_TO_INT(cvt_s2i, f32, "s", "w")
CVT_TO_INT(cvt_d2i, f64, "d", "w")

static inline f32 cvt_i2s(uint32_t v, int sgn)
{
    f32 r;
    if (sgn) __asm__ volatile("scvtf %s0, %w1" : "=w"(r) : "r"(v));
    else __asm__ volatile("ucvtf %s0, %w1" : "=w"(r) : "r"(v));
    return r;
}
static inline f64 cvt_i2d(uint32_t v, int sgn)
{
    f64 r;
    if (sgn) __asm__ volatile("scvtf %d0, %w1" : "=w"(r) : "r"(v));
    else __asm__ volatile("ucvtf %d0, %w1" : "=w"(r) : "r"(v));
    return r;
}
static inline f64 cvt_s2d(f32 a) { f64 r; __asm__ volatile("fcvt %d0, %s1" : "=w"(r) : "w"(a)); return r; }
static inline f32 cvt_d2s(f64 a) { f32 r; __asm__ volatile("fcvt %s0, %d1" : "=w"(r) : "w"(a)); return r; }
static inline uint16_t cvt_s2h(f32 a)
{
    uint32_t r;
    __asm__ volatile("fcvt h16, %s1\n\tfmov %w0, s16" : "=r"(r) : "w"(a) : "v16");
    return (uint16_t)r;
}
static inline uint16_t cvt_d2h(f64 a)
{
    uint32_t r;
    __asm__ volatile("fcvt h16, %d1\n\tfmov %w0, s16" : "=r"(r) : "w"(a) : "v16");
    return (uint16_t)r;
}
static inline f32 cvt_h2s(uint16_t h)
{
    f32 r;
    __asm__ volatile("fmov s16, %w1\n\tfcvt %s0, h16" : "=w"(r) : "r"((uint32_t)h) : "v16");
    return r;
}
static inline f64 cvt_h2d(uint16_t h)
{
    f64 r;
    __asm__ volatile("fmov s16, %w1\n\tfcvt %d0, h16" : "=w"(r) : "r"((uint32_t)h) : "v16");
    return r;
}

/* Fixed point with 1..32 fraction bits (0 = plain integer conversion). Round towards zero (to fixed) and with
 * FPCR rounding (from fixed), like the guest. */
static inline uint32_t cvt_s2fix(f32 a, int fbits, int sgn)
{
    f32 scaled = a;
    if (fbits) {
        /* multiplying by 2^fbits is exact unless it overflows/underflows, which the integer conversion handles the
         * same way as FPToFixed (saturation / tiny values truncating to 0) except for flush-to-zero corner cases;
         * use the A64 fixed-point form instead to stay exact. */
        uint32_t r;
        switch (fbits) {
#define FB(n)                                                                                                      \
    case n:                                                                                                        \
        if (sgn) __asm__ volatile("fcvtzs %w0, %s1, #" #n : "=r"(r) : "w"(a));                                     \
        else __asm__ volatile("fcvtzu %w0, %s1, #" #n : "=r"(r) : "w"(a));                                         \
        return r;
            FB(1) FB(2) FB(3) FB(4) FB(5) FB(6) FB(7) FB(8) FB(9) FB(10) FB(11) FB(12) FB(13) FB(14) FB(15) FB(16)
            FB(17) FB(18) FB(19) FB(20) FB(21) FB(22) FB(23) FB(24) FB(25) FB(26) FB(27) FB(28) FB(29) FB(30)
            FB(31) FB(32)
#undef FB
        }
    }
    return cvt_s2i(scaled, 3, sgn);
}
static inline uint32_t cvt_d2fix(f64 a, int fbits, int sgn)
{
    if (fbits) {
        uint32_t r;
        switch (fbits) {
#define FB(n)                                                                                                      \
    case n:                                                                                                        \
        if (sgn) __asm__ volatile("fcvtzs %w0, %d1, #" #n : "=r"(r) : "w"(a));                                     \
        else __asm__ volatile("fcvtzu %w0, %d1, #" #n : "=r"(r) : "w"(a));                                         \
        return r;
            FB(1) FB(2) FB(3) FB(4) FB(5) FB(6) FB(7) FB(8) FB(9) FB(10) FB(11) FB(12) FB(13) FB(14) FB(15) FB(16)
            FB(17) FB(18) FB(19) FB(20) FB(21) FB(22) FB(23) FB(24) FB(25) FB(26) FB(27) FB(28) FB(29) FB(30)
            FB(31) FB(32)
#undef FB
        }
    }
    return cvt_d2i(a, 3, sgn);
}
static inline f32 cvt_fix2s(uint32_t v, int fbits, int sgn)
{
    if (fbits) {
        f32 r;
        switch (fbits) {
#define FB(n)                                                                                                      \
    case n:                                                                                                        \
        if (sgn) __asm__ volatile("scvtf %s0, %w1, #" #n : "=w"(r) : "r"(v));                                      \
        else __asm__ volatile("ucvtf %s0, %w1, #" #n : "=w"(r) : "r"(v));                                          \
        return r;
            FB(1) FB(2) FB(3) FB(4) FB(5) FB(6) FB(7) FB(8) FB(9) FB(10) FB(11) FB(12) FB(13) FB(14) FB(15) FB(16)
            FB(17) FB(18) FB(19) FB(20) FB(21) FB(22) FB(23) FB(24) FB(25) FB(26) FB(27) FB(28) FB(29) FB(30)
            FB(31) FB(32)
#undef FB
        }
    }
    return cvt_i2s(v, sgn);
}
static inline f64 cvt_fix2d(uint32_t v, int fbits, int sgn)
{
    if (fbits) {
        f64 r;
        switch (fbits) {
#define FB(n)                                                                                                      \
    case n:                                                                                                        \
        if (sgn) __asm__ volatile("scvtf %d0, %w1, #" #n : "=w"(r) : "r"(v));                                      \
        else __asm__ volatile("ucvtf %d0, %w1, #" #n : "=w"(r) : "r"(v));                                          \
        return r;
            FB(1) FB(2) FB(3) FB(4) FB(5) FB(6) FB(7) FB(8) FB(9) FB(10) FB(11) FB(12) FB(13) FB(14) FB(15) FB(16)
            FB(17) FB(18) FB(19) FB(20) FB(21) FB(22) FB(23) FB(24) FB(25) FB(26) FB(27) FB(28) FB(29) FB(30)
            FB(31) FB(32)
#undef FB
        }
    }
    return cvt_i2d(v, sgn);
}

#endif
