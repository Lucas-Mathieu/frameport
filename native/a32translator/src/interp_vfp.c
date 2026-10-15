/* VFP (scalar floating-point) semantics and VFP register transfers. */
#include "interp.h"
#include "fp_host.h"

#define S(n) (cpu_s(c)[n])
#define D(n) (c->d[n])

static inline int rmode_of(const a32_cpu *c) { return (c->fpcr >> FPCR_RMODE_SHIFT) & 3; }

static void arith(a32_cpu *c, const insn_t *i)
{
    unsigned d = i->vd, n = i->vn, m = i->vm;
    if (!i->dp) {
        f32 a = u2f(S(n)), b = u2f(S(m)), x = u2f(S(d)), r;
        switch (i->sub) {
        case V_MLA: r = fadd_s(x, fmul_s(a, b)); break;
        case V_MLS: r = fadd_s(x, fneg_s(fmul_s(a, b))); break;
        case V_NMLA: r = fadd_s(fneg_s(x), fneg_s(fmul_s(a, b))); break;
        case V_NMLS: r = fadd_s(fneg_s(x), fmul_s(a, b)); break;
        case V_MUL: r = fmul_s(a, b); break;
        case V_NMUL: r = fneg_s(fmul_s(a, b)); break;
        case V_ADD: r = fadd_s(a, b); break;
        case V_SUB: r = fsub_s(a, b); break;
        case V_DIV: r = fdiv_s(a, b); break;
        case V_FMA: r = fmadd_s(a, b, x); break;
        case V_FMS: r = fmsub_s(a, b, x); break;
        case V_FNMA: r = fnmadd_s(a, b, x); break;
        case V_FNMS: r = fnmsub_s(a, b, x); break;
        case V_MAXNM: r = fmaxnm_s(a, b); break;
        default: r = fminnm_s(a, b); break;
        }
        S(d) = f2u(r);
    } else {
        f64 a = u2d(D(n)), b = u2d(D(m)), x = u2d(D(d)), r;
        switch (i->sub) {
        case V_MLA: r = fadd_d(x, fmul_d(a, b)); break;
        case V_MLS: r = fadd_d(x, fneg_d(fmul_d(a, b))); break;
        case V_NMLA: r = fadd_d(fneg_d(x), fneg_d(fmul_d(a, b))); break;
        case V_NMLS: r = fadd_d(fneg_d(x), fmul_d(a, b)); break;
        case V_MUL: r = fmul_d(a, b); break;
        case V_NMUL: r = fneg_d(fmul_d(a, b)); break;
        case V_ADD: r = fadd_d(a, b); break;
        case V_SUB: r = fsub_d(a, b); break;
        case V_DIV: r = fdiv_d(a, b); break;
        case V_FMA: r = fmadd_d(a, b, x); break;
        case V_FMS: r = fmsub_d(a, b, x); break;
        case V_FNMA: r = fnmadd_d(a, b, x); break;
        case V_FNMS: r = fnmsub_d(a, b, x); break;
        case V_MAXNM: r = fmaxnm_d(a, b); break;
        default: r = fminnm_d(a, b); break;
        }
        D(d) = d2u(r);
    }
}

static f32 rint_s(f32 a, unsigned mode)
{
    switch (mode) {
    case 0: return frintn_s(a);
    case 1: return frintp_s(a);
    case 2: return frintm_s(a);
    case 3: return frintz_s(a);
    case 4: return frinti_s(a);
    case 5: return frintx_s(a);
    default: return frinta_s(a);
    }
}

static f64 rint_d(f64 a, unsigned mode)
{
    switch (mode) {
    case 0: return frintn_d(a);
    case 1: return frintp_d(a);
    case 2: return frintm_d(a);
    case 3: return frintz_d(a);
    case 4: return frinti_d(a);
    case 5: return frintx_d(a);
    default: return frinta_d(a);
    }
}

/* Saturates a 32-bit fixed-point conversion result to 16 bits; reports whether it saturated. */
static uint32_t sat16(uint32_t v, int sgn, int *sat)
{
    if (sgn) {
        int32_t x = (int32_t)v;
        if (x > 32767) { *sat = 1; return 32767; }
        if (x < -32768) { *sat = 1; return (uint32_t)-32768; }
        return v;
    }
    if (v > 65535) { *sat = 1; return 65535; }
    return v;
}

static int fp_ops(a32_cpu *c, const insn_t *i)
{
    unsigned d = i->vd, m = i->vm;
    switch (i->sub) {
    case V_MOVI:
        if (i->dp) D(d) = i->imm64;
        else S(d) = (uint32_t)i->imm64;
        break;
    case V_MOV:
        if (i->dp) D(d) = D(m);
        else S(d) = S(m);
        break;
    case V_ABS:
        if (i->dp) D(d) = D(m) & ~(1ull << 63);
        else S(d) = S(m) & ~(1u << 31);
        break;
    case V_NEG:
        if (i->dp) D(d) = D(m) ^ (1ull << 63);
        else S(d) = S(m) ^ (1u << 31);
        break;
    case V_SQRT:
        if (i->dp) D(d) = d2u(fsqrt_d(u2d(D(m))));
        else S(d) = f2u(fsqrt_s(u2f(S(m))));
        break;
    case V_CVT_HALF: {
        int top = (i->flags & VF_TOP) != 0;
        if (i->flags & VF_TOHALF) {
            uint16_t h = i->dp ? cvt_d2h(u2d(D(m))) : cvt_s2h(u2f(S(m)));
            S(d) = top ? (S(d) & 0xffff) | (uint32_t)h << 16 : (S(d) & 0xffff0000u) | h;
        } else {
            uint16_t h = top ? S(m) >> 16 : S(m) & 0xffff;
            if (i->dp) D(d) = d2u(cvt_h2d(h));
            else S(d) = f2u(cvt_h2s(h));
        }
        break;
    }
    case V_CMP: {
        int e = (i->flags & VF_E) != 0;
        uint32_t f;
        if (i->flags & VF_ZERO) f = i->dp ? fcmp0_d(u2d(D(d)), e) : fcmp0_s(u2f(S(d)), e);
        else f = i->dp ? fcmp_d(u2d(D(d)), u2d(D(m)), e) : fcmp_s(u2f(S(d)), u2f(S(m)), e);
        c->fpscr_nzcv = f & 0xf0000000u;
        break;
    }
    case V_RINT:
        if (i->dp) D(d) = d2u(rint_d(u2d(D(m)), i->imm));
        else S(d) = f2u(rint_s(u2f(S(m)), i->imm));
        break;
    case V_CVT_DS:
        if (i->dp) S(d) = f2u(cvt_d2s(u2d(D(m))));
        else D(d) = d2u(cvt_s2d(u2f(S(m))));
        break;
    case V_CVT_I2F: {
        int sgn = (i->flags & VF_SIGNED) != 0;
        if (i->dp) D(d) = d2u(cvt_i2d(S(m), sgn));
        else S(d) = f2u(cvt_i2s(S(m), sgn));
        break;
    }
    case V_CVT_F2I: {
        int sgn = (i->flags & VF_SIGNED) != 0, mode = i->imm == 4 ? rmode_of(c) : i->imm;
        S(d) = i->dp ? cvt_d2i(u2d(D(m)), mode, sgn) : cvt_s2i(u2f(S(m)), mode, sgn);
        break;
    }
    case V_CVT_FIX: {
        int sgn = (i->flags & VF_SIGNED) != 0, fb = i->imm;
        if (i->flags & VF_TOFIX) {
            uint32_t r = i->dp ? cvt_d2fix(u2d(D(d)), fb, sgn) : cvt_s2fix(u2f(S(d)), fb, sgn);
            if (i->size == 16) {
                int sat = 0;
                r = sat16(r, sgn, &sat);
                if (sat) set_host_fpsr((host_fpsr() & ~0x10ull) | 1); /* InvalidOp instead of Inexact */
                r = sgn ? (uint32_t)(int16_t)r : (r & 0xffff);
            }
            if (i->dp) D(d) = sgn ? (uint64_t)(int64_t)(int32_t)r : r;
            else S(d) = r;
        } else {
            uint32_t v = i->dp ? (uint32_t)D(d) : S(d);
            if (i->size == 16) v = sgn ? (uint32_t)(int16_t)v : (v & 0xffff);
            if (i->dp) D(d) = d2u(cvt_fix2d(v, fb, sgn));
            else S(d) = f2u(cvt_fix2s(v, fb, sgn));
        }
        break;
    }
    case V_SEL:
        if (i->dp) D(d) = cond_pass(c->nzcv, i->imm) ? D(i->vn) : D(m);
        else S(d) = cond_pass(c->nzcv, i->imm) ? S(i->vn) : S(m);
        break;
    default:
        arith(c, i);
        break;
    }
    return EXIT_NONE;
}

static int transfers(a32_cpu *c, const insn_t *i)
{
    switch (i->sub) {
    case V_MOV_CS:
        if (i->flags & VF_TOCORE) c->r[i->rd] = S(i->vn);
        else S(i->vn) = c->r[i->rd];
        return EXIT_NONE;
    case V_MOV_CSS:
        if (i->flags & VF_TOCORE) {
            c->r[i->rd] = S(i->vm);
            c->r[i->ra] = S((i->vm + 1) & 31);
        } else {
            S(i->vm) = c->r[i->rd];
            S((i->vm + 1) & 31) = c->r[i->ra];
        }
        return EXIT_NONE;
    case V_MOV_CD:
        if (i->flags & VF_TOCORE) {
            c->r[i->rd] = (uint32_t)D(i->vm);
            c->r[i->ra] = (uint32_t)(D(i->vm) >> 32);
        } else {
            D(i->vm) = (uint64_t)c->r[i->ra] << 32 | c->r[i->rd];
        }
        return EXIT_NONE;
    case V_MOV_SCALAR: {
        unsigned bits = i->size * 8, sh = i->imm * bits;
        uint64_t mask = (bits == 32 ? 0xffffffffull : ((1ull << bits) - 1)) << sh;
        if (i->flags & VF_TOCORE) {
            uint32_t v = (uint32_t)((D(i->vd) & mask) >> sh);
            if (i->flags & VF_SIGNED) v = bits == 8 ? (uint32_t)(int8_t)v : bits == 16 ? (uint32_t)(int16_t)v : v;
            c->r[i->rd] = v;
        } else {
            D(i->vd) = (D(i->vd) & ~mask) | (((uint64_t)c->r[i->rd] << sh) & mask);
        }
        return EXIT_NONE;
    }
    case V_DUP: {
        uint32_t v = c->r[i->rd];
        uint64_t r = i->size == 1 ? (v & 0xff) * 0x0101010101010101ull
                   : i->size == 2 ? (v & 0xffff) * 0x0001000100010001ull
                                  : (uint64_t)v << 32 | v;
        D(i->vd) = r;
        if (i->q) D(i->vd + 1) = r;
        return EXIT_NONE;
    }
    case V_MSR:
        cpu_set_fpscr(c, c->r[i->rd]);
        return EXIT_NONE;
    case V_MRS:
        if (i->rd == 15) c->nzcv = c->fpscr_nzcv;
        else c->r[i->rd] = cpu_fpscr(c);
        return EXIT_NONE;
    case V_LDST: {
        uint32_t base = i->rn == 15 ? (i->pcval & ~3u) : c->r[i->rn];
        uint32_t a = i->u ? base + i->imm : base - i->imm;
        if (i->flags & VF_LOAD) {
            if (i->dp) D(i->vd) = (uint64_t)ld32(a + 4) << 32 | ld32(a);
            else S(i->vd) = ld32(a);
        } else {
            if (i->dp) {
                st32(a, (uint32_t)D(i->vd));
                st32(a + 4, (uint32_t)(D(i->vd) >> 32));
            } else {
                st32(a, S(i->vd));
            }
        }
        return EXIT_NONE;
    }
    case V_LDSTM: {
        uint32_t base = i->rn == 15 ? (i->pcval & ~3u) : c->r[i->rn], words = i->imm;
        uint32_t a = i->u ? base : base - 4 * words;
        if (i->w) c->r[i->rn] = i->u ? base + 4 * words : base - 4 * words;
        for (unsigned k = 0; k < i->nregs; k++) {
            unsigned r = i->vd + k;
            if (i->dp) {
                if (r >= 32) break;
                if (i->flags & VF_LOAD) D(r) = (uint64_t)ld32(a + 4) << 32 | ld32(a);
                else st32(a, (uint32_t)D(r)), st32(a + 4, (uint32_t)(D(r) >> 32));
                a += 8;
            } else {
                if (r >= 32) break;
                if (i->flags & VF_LOAD) S(r) = ld32(a);
                else st32(a, S(r));
                a += 4;
            }
        }
        return EXIT_NONE;
    }
    }
    return EXIT_UNDEF;
}

int a32_exec_vfp(a32_cpu *c, const insn_t *i)
{
    if (i->sub >= V_MOV_CS) return transfers(c, i);
    fp_saved s = fp_begin(c->fpcr);
    int r = fp_ops(c, i);
    c->fpsr |= fp_end(s);
    return r;
}
