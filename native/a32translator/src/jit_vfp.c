/* VFP and Advanced SIMD translation. Guest q0-q15 are pinned in host v16-v31 (jit_enter/jit_exit load and store
 * them), so Advanced SIMD Q operations are single A64 instructions. A D register is one 64-bit lane of its Q
 * register and an S register one 32-bit lane: reads extract the lane into a scratch register when it is not lane 0,
 * writes insert it (scalar A64 writes would clear the rest of the register). Scratch: v0-v7.
 * Scalar VFP code goes through a small cache in v8-v15 instead: a lane insert merges into the old register value,
 * which in loops chains every iteration to the previous one; a cached S/D value is written with plain scalar
 * instructions and merged into its Q register once, at the next exit, helper call or control-flow split.
 * Host FPCR holds the guest's VFP control bits or the Advanced SIMD standard value (jx_fpcr_*).
 * Instructions without a native translation (and every UNPREDICTABLE form) return 0: the interpreter runs them. */
#include <stddef.h>
#include "interp.h"
#include "jit.h"

#define S0 0 /* scratch vector registers */
#define S1 1
#define S2 2
#define S3 3
#define S4 4
#define S5 5

static inline int qreg(unsigned q) { return 16 + (int)(q & 15); }

/* ---------------------------------------------------------------- lane access */

/* register whose lane 0 holds S n */
static int rd_s(cbuf *b, unsigned n, int scratch)
{
    int v = qreg(n >> 2), k = n & 3;
    if (!k) return v;
    emit(b, a64_dup_scalar(scratch, v, a64_imm5(2, k)));
    return scratch;
}

/* register whose low 64 bits hold D n */
static int rd_d(cbuf *b, unsigned n, int scratch)
{
    int v = qreg(n >> 1);
    if (!(n & 1)) return v;
    emit(b, a64_dup_scalar(scratch, v, a64_imm5(3, 1)));
    return scratch;
}

static void wr_s(cbuf *b, unsigned n, int src) { emit(b, a64_ins_elem(qreg(n >> 2), src, a64_imm5(2, n & 3), 0)); }
static void wr_d(cbuf *b, unsigned n, int src) { emit(b, a64_ins_elem(qreg(n >> 1), src, a64_imm5(3, n & 1), 0)); }

/* S n / D n <-> general register */
static void s_to_w(cbuf *b, int w, unsigned n) { emit(b, a64_umov(0, w, qreg(n >> 2), a64_imm5(2, n & 3))); }
static void w_to_s(cbuf *b, unsigned n, int w) { emit(b, a64_ins_gen(qreg(n >> 2), w, a64_imm5(2, n & 3))); }
static void d_to_x(cbuf *b, int x, unsigned n) { emit(b, a64_umov(1, x, qreg(n >> 1), a64_imm5(3, n & 1))); }
static void x_to_d(cbuf *b, unsigned n, int x) { emit(b, a64_ins_gen(qreg(n >> 1), x, a64_imm5(3, n & 1))); }

/* Advanced SIMD operand: Q -> its host register; D -> a register whose low half holds it */
static int src_n(cbuf *b, unsigned dreg, int q, int scratch) { return q ? qreg(dreg >> 1) : rd_d(b, dreg, scratch); }

/* accumulator copy for D destinations: the op works on scratch, then the result is inserted */
static int acc_d(cbuf *b, unsigned dreg, int scratch)
{
    int r = rd_d(b, dreg, scratch);
    if (r != scratch) emit(b, a64_mov_v(0, scratch, r));
    return scratch;
}

/* ---------------------------------------------------------------- scalar VFP cache (v8-v15) */

#define CB 8
#define V_S 1
#define V_D 2

static int vc_find(jit_ctx *x, int view, unsigned reg)
{
    for (int k = 0; k < 8; k++)
        if (x->vc[k].view == view && x->vc[k].reg == reg) return k;
    return -1;
}

static void vc_merge(jit_ctx *x, int k)
{
    if (!x->vc[k].view || !x->vc[k].dirty) return;
    if (x->vc[k].view == V_S) wr_s(&x->cb, x->vc[k].reg, CB + k);
    else wr_d(&x->cb, x->vc[k].reg, CB + k);
    x->vc[k].dirty = 0;
}

static void vc_drop(jit_ctx *x, int k) { x->vc[k].view = 0, x->vc[k].dirty = 0; }

void jit_vfp_flush(jit_ctx *x)
{
    for (int k = 0; k < 8; k++) {
        vc_merge(x, k);
        vc_drop(x, k);
    }
}

/* exit paths: merge dirty values without changing the translation-time state (code after a side exit goes on
 * with the cache as it is) */
void jit_vfp_emit_merges(jit_ctx *x)
{
    for (int k = 0; k < 8; k++) {
        if (!x->vc[k].view || !x->vc[k].dirty) continue;
        if (x->vc[k].view == V_S) wr_s(&x->cb, x->vc[k].reg, CB + k);
        else wr_d(&x->cb, x->vc[k].reg, CB + k);
    }
}

uint64_t jit_vfp_state(const jit_ctx *x)
{
    uint64_t v = 0;
    for (int k = 0; k < 8; k++)
        if (x->vc[k].view)
            v |= (uint64_t)(x->vc[k].dirty << 7 | x->vc[k].view << 5 | (x->vc[k].reg & 31)) << (8 * k);
    return v;
}

/* the pinned Q registers get the current value of S n / D n; drop: the caller overwrites the pinned storage */
static void sync_one(jit_ctx *x, int view, unsigned reg, int drop)
{
    int k = vc_find(x, view, reg);
    if (k < 0) return;
    vc_merge(x, k);
    if (drop) vc_drop(x, k);
}

static void sync_d(jit_ctx *x, unsigned d, int drop)
{
    sync_one(x, V_D, d, drop);
    if (d < 16) {
        sync_one(x, V_S, 2 * d, drop);
        sync_one(x, V_S, 2 * d + 1, drop);
    }
}

static void sync_s(jit_ctx *x, unsigned n, int drop)
{
    sync_one(x, V_S, n, drop);
    sync_one(x, V_D, n >> 1, drop);
}

static int vc_alloc(jit_ctx *x)
{
    int best = 0;
    for (int k = 0; k < 8; k++) {
        if (!x->vc[k].view) return k;
        if (x->vc[k].used < x->vc[best].used) best = k;
    }
    vc_merge(x, best);
    vc_drop(x, best);
    return best;
}

/* a register whose lane 0 holds S n */
static int get_s(jit_ctx *x, unsigned n, int scratch)
{
    int k = vc_find(x, V_S, n);
    if (k >= 0) {
        x->vc[k].used = ++x->vc_tick;
        return CB + k;
    }
    if ((k = vc_find(x, V_D, n >> 1)) >= 0) {
        if (!(n & 1)) return CB + k;
        emit(&x->cb, a64_dup_scalar(scratch, CB + k, a64_imm5(2, 1)));
        return scratch;
    }
    return rd_s(&x->cb, n, scratch);
}

/* a register whose low 64 bits hold D n */
static int get_d(jit_ctx *x, unsigned d, int scratch)
{
    int k = vc_find(x, V_D, d);
    if (k >= 0) {
        x->vc[k].used = ++x->vc_tick;
        return CB + k;
    }
    if (d < 16) { /* halves cached as S: make the Q register current */
        sync_one(x, V_S, 2 * d, 0);
        sync_one(x, V_S, 2 * d + 1, 0);
    }
    return rd_d(&x->cb, d, scratch);
}

/* the register to write S n / D n into (whole-register writes are fine); the value becomes dirty */
static int put_s(jit_ctx *x, unsigned n)
{
    sync_one(x, V_D, n >> 1, 1); /* a cached D value would go stale */
    int k = vc_find(x, V_S, n);
    if (k < 0) k = vc_alloc(x);
    x->vc[k].view = V_S, x->vc[k].reg = (uint8_t)n, x->vc[k].dirty = 1, x->vc[k].used = ++x->vc_tick;
    return CB + k;
}

static int put_d(jit_ctx *x, unsigned d)
{
    if (d < 16) { /* both halves are overwritten: cached S values are simply superseded */
        int k;
        if ((k = vc_find(x, V_S, 2 * d)) >= 0) vc_drop(x, k);
        if ((k = vc_find(x, V_S, 2 * d + 1)) >= 0) vc_drop(x, k);
    }
    int k = vc_find(x, V_D, d);
    if (k < 0) k = vc_alloc(x);
    x->vc[k].view = V_D, x->vc[k].reg = (uint8_t)d, x->vc[k].dirty = 1, x->vc[k].used = ++x->vc_tick;
    return CB + k;
}

/* Q register load/store at addr + pos (any byte offset) */
static void ldst_q(cbuf *b, int load, int v, int addr, unsigned pos)
{
    if (pos & 15) emit(b, a64_ldst_fp_unscaled(4, load, v, addr, (int)pos));
    else emit(b, a64_ldst_fp(4, load, v, addr, pos));
}

/* ---------------------------------------------------------------- VFP */

static int vfp_arith(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int dp = i->dp, ty = dp ? 1 : 0;
#define GET(r, scr) (dp ? get_d(x, (r), (scr)) : get_s(x, (r), (scr)))
#define PUT(r) (dp ? put_d(x, (r)) : put_s(x, (r)))
    int n = 0, m = 0, a = 0, d;
    jx_fpcr_guest(x);
    switch (i->sub) {
    case V_MLA: case V_MLS: case V_NMLA: case V_NMLS: case V_FMA: case V_FMS: case V_FNMA: case V_FNMS:
        a = GET(i->vd, S3);
        /* fall through */
    case V_MUL: case V_NMUL: case V_ADD: case V_SUB: case V_DIV: case V_MAXNM: case V_MINNM: case V_SEL:
        n = GET(i->vn, S1);
        m = i->vm == i->vn ? n : GET(i->vm, S2);
        break;
    case V_ABS: case V_NEG: case V_SQRT: case V_RINT: case V_MOV:
        m = GET(i->vm, S2);
        break;
    default:
        break;
    }
    /* the destination is written by the last instruction (it may reuse a source's register) */
    switch (i->sub) {
    case V_MLA:
        emit(b, a64_fp2(ty, 0, S4, n, m));
        emit(b, a64_fp2(ty, 2, PUT(i->vd), a, S4));
        return 1;
    case V_MLS:
        emit(b, a64_fp2(ty, 0, S4, n, m));
        emit(b, a64_fp1(ty, 2, S4, S4));
        emit(b, a64_fp2(ty, 2, PUT(i->vd), a, S4));
        return 1;
    case V_NMLA:
        emit(b, a64_fp2(ty, 0, S4, n, m));
        emit(b, a64_fp1(ty, 2, S4, S4));
        emit(b, a64_fp1(ty, 2, S5, a));
        emit(b, a64_fp2(ty, 2, PUT(i->vd), S5, S4));
        return 1;
    case V_NMLS:
        emit(b, a64_fp2(ty, 0, S4, n, m));
        emit(b, a64_fp1(ty, 2, S5, a));
        emit(b, a64_fp2(ty, 2, PUT(i->vd), S5, S4));
        return 1;
    case V_MUL: d = PUT(i->vd); emit(b, a64_fp2(ty, 0, d, n, m)); return 1;
    case V_NMUL: d = PUT(i->vd); emit(b, a64_fp2(ty, 8, d, n, m)); return 1;
    case V_ADD: d = PUT(i->vd); emit(b, a64_fp2(ty, 2, d, n, m)); return 1;
    case V_SUB: d = PUT(i->vd); emit(b, a64_fp2(ty, 3, d, n, m)); return 1;
    case V_DIV: d = PUT(i->vd); emit(b, a64_fp2(ty, 1, d, n, m)); return 1;
    case V_MAXNM: d = PUT(i->vd); emit(b, a64_fp2(ty, 6, d, n, m)); return 1;
    case V_MINNM: d = PUT(i->vd); emit(b, a64_fp2(ty, 7, d, n, m)); return 1;
    case V_FMA: d = PUT(i->vd); emit(b, a64_fp3(ty, 0, d, n, m, a)); return 1;
    case V_FMS: d = PUT(i->vd); emit(b, a64_fp3(ty, 1, d, n, m, a)); return 1;
    case V_FNMA: d = PUT(i->vd); emit(b, a64_fp3(ty, 2, d, n, m, a)); return 1;
    case V_FNMS: d = PUT(i->vd); emit(b, a64_fp3(ty, 3, d, n, m, a)); return 1;
    case V_SEL: d = PUT(i->vd); emit(b, a64_fcsel(ty, d, n, m, i->imm)); return 1;
    case V_ABS: case V_NEG: case V_SQRT: case V_MOV:
        d = PUT(i->vd);
        emit(b, a64_fp1(ty, i->sub == V_ABS ? 1 : i->sub == V_NEG ? 2 : i->sub == V_SQRT ? 3 : 0, d, m));
        return 1;
    case V_RINT: {
        static const int op[7] = {8, 9, 10, 11, 15, 14, 12}; /* N P M Z I(FPSCR) X A */
        d = PUT(i->vd);
        emit(b, a64_fp1(ty, op[i->imm], d, m));
        return 1;
    }
    case V_MOVI:
        if (dp) {
            jx_mov64(b, T0, i->imm64);
            emit(b, a64_fcvt_int(1, 1, 0x07, put_d(x, i->vd), T0)); /* fmov d, x16 */
        } else {
            jx_mov32(b, T0, (uint32_t)i->imm64);
            emit(b, a64_fcvt_int(0, 0, 0x07, put_s(x, i->vd), T0)); /* fmov s, w16 */
        }
        return 1;
    case V_CMP: {
        int e = (i->flags & VF_E) != 0, zero = (i->flags & VF_ZERO) != 0;
        int r = GET(i->vd, S1);
        if (!zero) m = i->vm == i->vd ? r : GET(i->vm, S2);
        /* VCMP; VMRS APSR_nzcv, FPSCR (the usual pair): the compare's flags simply stay in NZCV */
        const insn_t *nx = x->next;
        int fuse = nx && nx->op == OP_VFP && nx->sub == V_MRS && nx->rd == 15 && nx->cond == C_AL && !nx->unpred &&
                   i->cond == C_AL;
        if (!fuse) emit(b, a64_mrs(T1, SYS_NZCV));
        emit(b, a64_fcmp(ty, e, zero, r, m));
        emit(b, a64_mrs(T0, SYS_NZCV));
        emit(b, a64_ldst_imm(2, 0, T0, R_CPU, OFF_FPSCR_NZCV));
        if (fuse) x->skip_vmrs = 1;
        else emit(b, a64_msr(T1, SYS_NZCV));
        return 1;
    }
    case V_CVT_DS:
        if (dp) { /* double -> single */
            m = get_d(x, i->vm, S2);
            emit(b, a64_fp1(1, 4, put_s(x, i->vd), m));
        } else {
            m = get_s(x, i->vm, S2);
            emit(b, a64_fp1(0, 5, put_d(x, i->vd), m));
        }
        return 1;
    case V_CVT_I2F:
        m = get_s(x, i->vm, S2);
        emit(b, a64_fcvt_int(0, 0, 0x06, T0, m)); /* fmov w16, s */
        emit(b, a64_fcvt_int(0, ty, (i->flags & VF_SIGNED) ? 0x02 : 0x03, PUT(i->vd), T0));
        return 1;
    case V_CVT_F2I: {
        int sgn = (i->flags & VF_SIGNED) != 0, op;
        switch (i->imm) {
        case 0: op = 0x00; break; /* N */
        case 1: op = 0x08; break; /* P */
        case 2: op = 0x10; break; /* M */
        case 3: op = 0x18; break; /* Z */
        case 6: op = 0x04; break; /* A */
        default: return 0;        /* FPSCR rounding (VCVTR): interpreter */
        }
        m = GET(i->vm, S2);
        emit(b, a64_fcvt_int(0, ty, op | !sgn, T0, m));
        emit(b, a64_fcvt_int(0, 0, 0x07, put_s(x, i->vd), T0));
        return 1;
    }
    case V_CVT_FIX: {
        if (i->size != 32 || i->imm > 32) return 0;
        int sgn = (i->flags & VF_SIGNED) != 0, fb = i->imm;
        if (i->flags & VF_TOFIX) {
            int r = GET(i->vd, S1);
            if (fb) emit(b, a64_fcvt_fix(0, ty, sgn ? 0x18 : 0x19, T0, r, fb));
            else emit(b, a64_fcvt_int(0, ty, sgn ? 0x18 : 0x19, T0, r));
            if (dp) {
                if (sgn) emit(b, a64_bfm(1, 0, T0, T0, 0, 31)); /* sxtw */
                emit(b, a64_fcvt_int(1, 1, 0x07, put_d(x, i->vd), T0));
            } else {
                emit(b, a64_fcvt_int(0, 0, 0x07, put_s(x, i->vd), T0));
            }
        } else {
            int r = GET(i->vd, S1);
            emit(b, a64_fcvt_int(0, 0, 0x06, T0, r)); /* the fixed-point value is the low 32 bits */
            if (fb) emit(b, a64_fcvt_fix(0, ty, sgn ? 0x02 : 0x03, PUT(i->vd), T0, fb));
            else emit(b, a64_fcvt_int(0, ty, sgn ? 0x02 : 0x03, PUT(i->vd), T0));
        }
        return 1;
    }
    default:
        return 0;
    }
#undef GET
#undef PUT
}

/* address of a VLDR/VSTR/VLDM/VSTM into T0 (or the base register itself) */
static int vfp_addr(jit_ctx *x, const insn_t *i, uint32_t off, int down)
{
    cbuf *b = &x->cb;
    if (i->rn == 15) {
        uint32_t base = i->pcval & ~3u;
        jx_mov32(b, T0, down ? base - off : base + off);
        return T0;
    }
    if (off == 0) return i->rn;
    emit(b, a64_addsub_imm(0, down, 0, T0, i->rn, off, 0));
    return T0;
}

static int vfp_transfer(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int tocore = (i->flags & VF_TOCORE) != 0;
    switch (i->sub) {
    case V_MOV_CS:
        if (i->rd == 15) return 0;
        if (tocore) emit(b, a64_fcvt_int(0, 0, 0x06, i->rd, get_s(x, i->vn, S1)));
        else emit(b, a64_fcvt_int(0, 0, 0x07, put_s(x, i->vn), i->rd));
        return 1;
    case V_MOV_CSS:
        if (i->rd == 15 || i->ra == 15 || i->vm == 31 || (tocore && i->rd == i->ra)) return 0;
        if (tocore) {
            emit(b, a64_fcvt_int(0, 0, 0x06, T0, get_s(x, i->vm, S1)));
            emit(b, a64_fcvt_int(0, 0, 0x06, i->ra, get_s(x, i->vm + 1, S2)));
            emit(b, a64_logic_reg(0, 1, 0, i->rd, XZR, T0, 0, 0));
        } else {
            emit(b, a64_fcvt_int(0, 0, 0x07, put_s(x, i->vm), i->rd));
            emit(b, a64_fcvt_int(0, 0, 0x07, put_s(x, i->vm + 1), i->ra));
        }
        return 1;
    case V_MOV_CD:
        if (i->rd == 15 || i->ra == 15 || (tocore && i->rd == i->ra)) return 0;
        if (tocore) {
            emit(b, a64_fcvt_int(1, 1, 0x06, T0, get_d(x, i->vm, S1))); /* fmov x16, d */
            emit(b, a64_logic_reg(0, 1, 0, i->rd, XZR, T0, 0, 0));      /* mov wLo, w16 */
            emit(b, a64_bfm(1, 2, i->ra, T0, 32, 63));                  /* lsr xHi, x16, #32 */
        } else {
            emit(b, a64_logic_reg(1, 1, 0, T0, i->rd, i->ra, SHIFT_LSL, 32)); /* x16 = hi:lo */
            emit(b, a64_fcvt_int(1, 1, 0x07, put_d(x, i->vm), T0));
        }
        return 1;
    case V_MOV_SCALAR: {
        if (i->rd == 15) return 0;
        sync_d(x, i->vd, !tocore);
        int es = i->size == 1 ? 0 : i->size == 2 ? 1 : 2;
        int lane = (int)((i->vd & 1) * (8 / i->size) + i->imm), v = qreg(i->vd >> 1);
        if (tocore) {
            if ((i->flags & VF_SIGNED) && es < 2) emit(b, a64_smov(i->rd, v, a64_imm5(es, lane)));
            else emit(b, a64_umov(0, i->rd, v, a64_imm5(es, lane)));
        } else {
            emit(b, a64_ins_gen(v, i->rd, a64_imm5(es, lane)));
        }
        return 1;
    }
    case V_DUP: {
        if (i->rd == 15) return 0;
        sync_d(x, i->vd, 1);
        if (i->q) sync_d(x, i->vd + 1, 1);
        int imm5 = i->size == 1 ? 1 : i->size == 2 ? 2 : 4;
        if (i->q) {
            emit(b, a64_dup_gen(1, qreg(i->vd >> 1), i->rd, imm5));
        } else {
            emit(b, a64_dup_gen(0, S0, i->rd, imm5));
            wr_d(b, i->vd, S0);
        }
        return 1;
    }
    case V_MRS:
        if (i->rd != 15) return 0; /* full FPSCR read: interpreter (it merges the cumulative flags) */
        if (x->skip_vmrs) { /* fused with the VCMP before it */
            x->skip_vmrs = 0;
            return 1;
        }
        emit(b, a64_ldst_imm(2, 1, T0, R_CPU, OFF_FPSCR_NZCV));
        emit(b, a64_msr(T0, SYS_NZCV));
        return 1;
    case V_LDST: {
        int load = (i->flags & VF_LOAD) != 0, sz = i->dp ? 3 : 2;
        int addr = vfp_addr(x, i, i->imm, !i->u);
        int r = load ? (i->dp ? put_d(x, i->vd) : put_s(x, i->vd)) : (i->dp ? get_d(x, i->vd, S1) : get_s(x, i->vd, S1));
        emit(b, a64_ldst_fp(sz, load, r, addr, 0));
        return 1;
    }
    case V_LDSTM: {
        if (i->rn == 15 || i->unpred) return 0;
        int load = (i->flags & VF_LOAD) != 0;
        unsigned words = i->imm, n = i->nregs;
        if (!n || i->vd + n > 32) return 0;
        for (unsigned k = 0; k < n; k++) {
            if (i->dp) sync_d(x, i->vd + k, load);
            else sync_s(x, i->vd + k, load);
        }
        int addr = i->u ? i->rn : vfp_addr(x, i, 4 * words, 1);
        unsigned pos = 0; /* byte offset from addr */
        if (i->dp) {
            for (unsigned k = 0; k < n;) {
                unsigned r = i->vd + k;
                if (!(r & 1) && k + 1 < n) { /* a whole Q register */
                    ldst_q(b, load, qreg(r >> 1), addr, pos);
                    k += 2, pos += 16;
                } else {
                    if (load) {
                        emit(b, a64_ldst_imm(3, 1, T1, addr, pos));
                        x_to_d(b, r, T1);
                    } else {
                        d_to_x(b, T1, r);
                        emit(b, a64_ldst_imm(3, 0, T1, addr, pos));
                    }
                    k += 1, pos += 8;
                }
            }
        } else {
            for (unsigned k = 0; k < n; k++, pos += 4) {
                unsigned r = i->vd + k;
                if (load) {
                    emit(b, a64_ldst_imm(2, 1, T1, addr, pos));
                    w_to_s(b, r, T1);
                } else {
                    s_to_w(b, T1, r);
                    emit(b, a64_ldst_imm(2, 0, T1, addr, pos));
                }
            }
        }
        if (i->w) emit(b, a64_addsub_imm(0, !i->u, 0, i->rn, i->rn, 4 * words, 0));
        return 1;
    }
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- Advanced SIMD */

/* d = op(n, m) with Q or D operands (word: the A64 encoding with Q = 0 and zero registers); acc: d is an input */
static void simd_binop(cbuf *b, const insn_t *i, uint32_t word, int acc, int swap)
{
    int q = i->q;
    int n = src_n(b, i->vn, q, S1), m = src_n(b, i->vm, q, S2);
    int rn = swap ? m : n, rm = swap ? n : m;
    int d = q ? qreg(i->vd >> 1) : (acc ? acc_d(b, i->vd, S0) : S0);
    emit(b, word | (uint32_t)q << 30 | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)d);
    if (!q) wr_d(b, i->vd, S0);
}

static int three_same_int(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int u = !(i->flags & VF_SIGNED), opc, swap = 0, acc = 0, size = i->esize;
    switch (i->sub) {
    case N_VHADD: opc = 0x00; break;
    case N_VQADD: opc = 0x01; break;
    case N_VRHADD: opc = 0x02; break;
    case N_VHSUB: opc = 0x04; break;
    case N_VQSUB: opc = 0x05; break;
    case N_VCGT: opc = 0x06; break;
    case N_VCGE: opc = 0x07; break;
    case N_VSHL: opc = 0x08; swap = 1; break;
    case N_VQSHL: opc = 0x09; swap = 1; break;
    case N_VRSHL: opc = 0x0a; swap = 1; break;
    case N_VQRSHL: opc = 0x0b; swap = 1; break;
    case N_VMAX: opc = 0x0c; break;
    case N_VMIN: opc = 0x0d; break;
    case N_VABD: opc = 0x0e; break;
    case N_VABA: opc = 0x0f; acc = 1; break;
    case N_VADD: opc = 0x10; u = 0; break;
    case N_VSUB: opc = 0x10; u = 1; break;
    case N_VTST: opc = 0x11; u = 0; break;
    case N_VCEQ: opc = 0x11; u = 1; break;
    case N_VMLA: opc = 0x12; u = 0; acc = 1; break;
    case N_VMLS: opc = 0x12; u = 1; acc = 1; break;
    case N_VMUL: opc = 0x13; u = 0; break;
    case N_VMULP: opc = 0x13; u = 1; break;
    case N_VPMAX: opc = 0x14; break;
    case N_VPMIN: opc = 0x15; break;
    case N_VQDMULH: opc = 0x16; u = 0; break;
    case N_VQRDMULH: opc = 0x16; u = 1; break;
    case N_VPADD: opc = 0x17; u = 0; break;
    case N_VAND: case N_VBIC: case N_VORR: case N_VORN: case N_VEOR: case N_VBSL: case N_VBIT: case N_VBIF: {
        static const uint8_t uu[8] = {0, 0, 0, 0, 1, 1, 1, 1}, ss[8] = {0, 1, 2, 3, 0, 1, 2, 3};
        int k = i->sub - N_VAND;
        u = uu[k];
        size = ss[k];
        opc = 0x03;
        acc = k >= 5;
        break;
    }
    default:
        return 0;
    }
    if (size == 3 && opc != 0x03) {
        /* 64-bit lanes: only the forms with an A64 .2d / scalar equivalent */
        int ok = opc == 0x01 || opc == 0x05 || opc == 0x08 || opc == 0x09 || opc == 0x0a || opc == 0x0b || opc == 0x10 ||
                 opc == 0x11 || opc == 0x06 || opc == 0x07;
        if (!ok) return 0;
        if (!i->q) {
            /* D form: A64 scalar encoding (bit 28 set, Q = 1 in the vector layout) */
            int n = rd_d(b, i->vn, S1), m = rd_d(b, i->vm, S2);
            emit(b, a64_simd3(1, u, 3, opc, S0, swap ? m : n, swap ? n : m) | 0x10000000u);
            wr_d(b, i->vd, S0);
            return 1;
        }
    }
    simd_binop(b, i, a64_simd3(0, u, size, opc, 0, 0, 0), acc, swap);
    return 1;
}

static int three_same_float(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int u, a = 0, opc, acc = 0;
    switch (i->sub) {
    case N_FADD: u = 0, a = 0, opc = 0x1a; break;
    case N_FSUB: u = 0, a = 1, opc = 0x1a; break;
    case N_FPADD: u = 1, a = 0, opc = 0x1a; break;
    case N_FABD: u = 1, a = 1, opc = 0x1a; break;
    case N_FMUL: u = 1, a = 0, opc = 0x1b; break;
    case N_FCEQ: u = 0, a = 0, opc = 0x1c; break;
    case N_FCGE: u = 1, a = 0, opc = 0x1c; break;
    case N_FCGT: u = 1, a = 1, opc = 0x1c; break;
    case N_FACGE: u = 1, a = 0, opc = 0x1d; break;
    case N_FACGT: u = 1, a = 1, opc = 0x1d; break;
    case N_FMAX: u = 0, a = 0, opc = 0x1e; break;
    case N_FMIN: u = 0, a = 1, opc = 0x1e; break;
    case N_FPMAX: u = 1, a = 0, opc = 0x1e; break;
    case N_FPMIN: u = 1, a = 1, opc = 0x1e; break;
    case N_FMA: u = 0, a = 0, opc = 0x19, acc = 1; break;
    case N_FMS: u = 0, a = 1, opc = 0x19, acc = 1; break;
    case N_FMAXNM: u = 0, a = 0, opc = 0x18; break;
    case N_FMINNM: u = 0, a = 1, opc = 0x18; break;
    case N_FMLA: case N_FMLS: {
        /* not fused: the product is rounded first. NEON uses default NaNs, so FPAdd(d, -p) == FPSub(d, p). */
        int q = i->q;
        jx_fpcr_std(x);
        int n = src_n(b, i->vn, q, S1), m = src_n(b, i->vm, q, S2);
        emit(b, a64_simd3(q, 1, 0, 0x1b, S3, n, m));
        int d = q ? qreg(i->vd >> 1) : acc_d(b, i->vd, S0);
        emit(b, a64_simd3(q, 0, i->sub == N_FMLS ? 2 : 0, 0x1a, d, d, S3));
        if (!q) wr_d(b, i->vd, S0);
        return 1;
    }
    default:
        return 0; /* VRECPS/VRSQRTS: not fused in AArch32 */
    }
    jx_fpcr_std(x);
    simd_binop(b, i, a64_simd3(0, u, a << 1, opc, 0, 0, 0), acc, 0);
    return 1;
}

static int shift_imm(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int u = !(i->flags & VF_SIGNED), es = i->esize, bits = 8 << es, sh = (int)i->imm, opc, right, narrow = 0, wide = 0;
    switch (i->sub) {
    case N_VSHR: opc = 0x00; right = 1; break;
    case N_VSRA: opc = 0x02; right = 1; break;
    case N_VRSHR: opc = 0x04; right = 1; break;
    case N_VRSRA: opc = 0x06; right = 1; break;
    case N_VSRI: opc = 0x08; right = 1; u = 1; break;
    case N_VSHL_I: opc = 0x0a; right = 0; u = 0; break;
    case N_VSLI: opc = 0x0a; right = 0; u = 1; break;
    case N_VQSHLU_I: opc = 0x0c; right = 0; u = 1; break;
    case N_VQSHL_I: opc = 0x0e; right = 0; break;
    case N_VSHRN: opc = 0x10; right = 1; u = 0; narrow = 1; break;
    case N_VRSHRN: opc = 0x11; right = 1; u = 0; narrow = 1; break;
    case N_VQSHRUN: opc = 0x10; right = 1; u = 1; narrow = 1; break;
    case N_VQRSHRUN: opc = 0x11; right = 1; u = 1; narrow = 1; break;
    case N_VQSHRN: opc = 0x12; right = 1; narrow = 1; break;
    case N_VQRSHRN: opc = 0x13; right = 1; narrow = 1; break;
    case N_VSHLL: case N_VMOVL: opc = 0x14; right = 0; wide = 1; break;
    default: return 0;
    }
    int acc = opc == 0x02 || opc == 0x06 || opc == 0x08 || (opc == 0x0a && u);
    int q = i->q;
    if (narrow) { /* Q source of 2*esize elements -> D */
        emit(b, a64_simd_shift(0, u, 2 * bits - sh, opc, S0, qreg(i->vm >> 1)));
        wr_d(b, i->vd, S0);
        return 1;
    }
    if (wide) { /* D source -> Q */
        int m = rd_d(b, i->vm, S1);
        emit(b, a64_simd_shift(0, u, bits + sh, opc, qreg(i->vd >> 1), m));
        return 1;
    }
    int immhb = right ? 2 * bits - sh : bits + sh;
    int m = src_n(b, i->vm, q, S1);
    int d = q ? qreg(i->vd >> 1) : (acc ? acc_d(b, i->vd, S0) : S0);
    if (es == 3 && !q) emit(b, a64_simd_shift(1, u, immhb, opc, d, m) | 0x10000000u); /* scalar D */
    else emit(b, a64_simd_shift(q, u, immhb, opc, d, m));
    if (!q) wr_d(b, i->vd, S0);
    return 1;
}

static int misc2(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int q = i->q, size = i->esize, u, opc, fl = 0, acc = 0;
    switch (i->sub) {
    case N_VREV64: u = 0, opc = 0x00; break;
    case N_VREV32: u = 1, opc = 0x00; break;
    case N_VREV16: u = 0, opc = 0x01; break;
    case N_VPADDL: u = !(i->flags & VF_SIGNED), opc = 0x02; break;
    case N_VPADAL: u = !(i->flags & VF_SIGNED), opc = 0x06, acc = 1; break;
    case N_VCLS: u = 0, opc = 0x04; break;
    case N_VCLZ: u = 1, opc = 0x04; break;
    case N_VCNT: u = 0, opc = 0x05; break;
    case N_VMVN: u = 1, opc = 0x05, size = 0; break;
    case N_VQABS: u = 0, opc = 0x07; break;
    case N_VQNEG: u = 1, opc = 0x07; break;
    case N_VCGT0: u = 0, opc = 0x08; break;
    case N_VCGE0: u = 1, opc = 0x08; break;
    case N_VCEQ0: u = 0, opc = 0x09; break;
    case N_VCLE0: u = 1, opc = 0x09; break;
    case N_VCLT0: u = 0, opc = 0x0a; break;
    case N_VABS: u = 0, opc = 0x0b; break;
    case N_VNEG: u = 1, opc = 0x0b; break;
    case N_FCGT0: u = 0, opc = 0x0c, size = 2, fl = 1; break;
    case N_FCGE0: u = 1, opc = 0x0c, size = 2, fl = 1; break;
    case N_FCEQ0: u = 0, opc = 0x0d, size = 2, fl = 1; break;
    case N_FCLE0: u = 1, opc = 0x0d, size = 2, fl = 1; break;
    case N_FCLT0: u = 0, opc = 0x0e, size = 2, fl = 1; break;
    case N_FABS: u = 0, opc = 0x0f, size = 2, fl = 1; break;
    case N_FNEG: u = 1, opc = 0x0f, size = 2, fl = 1; break;
    case N_VRECPE: u = 0, opc = 0x1c, size = 2; break;
    case N_VRSQRTE: u = 1, opc = 0x1c, size = 2; break;
    case N_FRECPE: u = 0, opc = 0x1d, size = 2, fl = 1; break;
    case N_FRSQRTE: u = 1, opc = 0x1d, size = 2, fl = 1; break;
    case N_VCVT_FI:
        fl = 1;
        if (i->flags & VF_TOFIX) u = !(i->flags & VF_SIGNED), opc = 0x1b, size = 2; /* FCVTZS/U */
        else u = !(i->flags & VF_SIGNED), opc = 0x1d, size = 0;                    /* SCVTF/UCVTF */
        break;
    case N_VSWP:
        if (q) {
            emit(b, a64_mov_v(1, S0, qreg(i->vd >> 1)));
            emit(b, a64_mov_v(1, qreg(i->vd >> 1), qreg(i->vm >> 1)));
            emit(b, a64_mov_v(1, qreg(i->vm >> 1), S0));
        } else {
            int d = rd_d(b, i->vd, S0), m = rd_d(b, i->vm, S1);
            if (d != S0) emit(b, a64_mov_v(0, S0, d));
            if (m != S1) emit(b, a64_mov_v(0, S1, m));
            wr_d(b, i->vm, S0);
            wr_d(b, i->vd, S1);
        }
        return 1;
    case N_VTRN: case N_VUZP: case N_VZIP: {
        int o1 = i->sub == N_VTRN ? 2 : i->sub == N_VUZP ? 1 : 3;
        int d = src_n(b, i->vd, q, S1), m = src_n(b, i->vm, q, S2);
        emit(b, a64_perm(q, size, o1, S0, d, m));
        emit(b, a64_perm(q, size, o1 + 4, S3, d, m));
        if (q) {
            emit(b, a64_mov_v(1, qreg(i->vd >> 1), S0));
            emit(b, a64_mov_v(1, qreg(i->vm >> 1), S3));
        } else {
            wr_d(b, i->vd, S0);
            wr_d(b, i->vm, S3);
        }
        return 1;
    }
    case N_VMOVN: case N_VQMOVN: case N_VQMOVUN: {
        /* XTN 0x12 (U0), SQXTUN 0x12 (U1), SQXTN/UQXTN 0x14 */
        int un = i->sub == N_VQMOVUN ? 1 : (i->sub == N_VQMOVN ? !(i->flags & VF_SIGNED) : 0);
        int op = i->sub == N_VQMOVN ? 0x14 : 0x12;
        emit(b, a64_simd2(0, un, size, op, S0, qreg(i->vm >> 1)));
        wr_d(b, i->vd, S0);
        return 1;
    }
    case N_VSHLL_MAX: {
        int m = rd_d(b, i->vm, S1);
        emit(b, a64_simd2(0, 1, size, 0x13, qreg(i->vd >> 1), m)); /* SHLL */
        return 1;
    }
    default:
        return 0;
    }
    if (fl) jx_fpcr_std(x);
    int m = src_n(b, i->vm, q, S1);
    int d = q ? qreg(i->vd >> 1) : (acc ? acc_d(b, i->vd, S0) : S0);
    emit(b, a64_simd2(q, u, size, opc, d, m));
    if (!q) wr_d(b, i->vd, S0);
    return 1;
}

static int long_ops(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int u = !(i->flags & VF_SIGNED), size = i->esize, opc;
    switch (i->sub) {
    case N_VADDL: opc = 0x0; break;
    case N_VADDW: opc = 0x1; break;
    case N_VSUBL: opc = 0x2; break;
    case N_VSUBW: opc = 0x3; break;
    case N_VABAL: opc = 0x5; break;
    case N_VABDL: opc = 0x7; break;
    case N_VMLAL: opc = 0x8; break;
    case N_VMLSL: opc = 0xa; break;
    case N_VMULL: opc = 0xc; break;
    case N_VQDMULL: opc = 0xd; u = 0; break;
    case N_VQDMLAL: opc = 0x9; u = 0; break;
    case N_VQDMLSL: opc = 0xb; u = 0; break;
    case N_VMULLP: if (size != 0) return 0; opc = 0xe; u = 0; break;
    case N_VADDHN: case N_VRADDHN: case N_VSUBHN: case N_VRSUBHN: {
        int r = i->sub == N_VRADDHN || i->sub == N_VRSUBHN, sub = i->sub == N_VSUBHN || i->sub == N_VRSUBHN;
        emit(b, a64_simd3diff(0, r, size, sub ? 0x6 : 0x4, S0, qreg(i->vn >> 1), qreg(i->vm >> 1)));
        wr_d(b, i->vd, S0);
        return 1;
    }
    default:
        return 0;
    }
    int wide = opc == 0x1 || opc == 0x3;
    int n = wide ? qreg(i->vn >> 1) : rd_d(b, i->vn, S1);
    int m = rd_d(b, i->vm, S2);
    /* the accumulating forms read the destination, which is a whole Q register */
    emit(b, a64_simd3diff(0, u, size, opc, qreg(i->vd >> 1), n, m));
    return 1;
}

static int by_scalar(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int size = i->esize, q = i->q, u, opc, lng = 0, fl = 0, acc = 0;
    switch (i->sub) {
    case N_VMLA_S: u = 1, opc = 0x0, acc = 1; break;
    case N_VMLS_S: u = 1, opc = 0x4, acc = 1; break;
    case N_VMUL_S: u = 0, opc = 0x8; break;
    case N_VQDMULH_S: u = 0, opc = 0xc; break;
    case N_VQRDMULH_S: u = 0, opc = 0xd; break;
    case N_VMLAL_S: u = !(i->flags & VF_SIGNED), opc = 0x2, lng = 1; break;
    case N_VMLSL_S: u = !(i->flags & VF_SIGNED), opc = 0x6, lng = 1; break;
    case N_VMULL_S: u = !(i->flags & VF_SIGNED), opc = 0xa, lng = 1; break;
    case N_VQDMULL_S: u = 0, opc = 0xb, lng = 1; break;
    case N_VQDMLAL_S: u = 0, opc = 0x3, lng = 1; break;
    case N_VQDMLSL_S: u = 0, opc = 0x7, lng = 1; break;
    case N_FMUL_S: u = 0, opc = 0x9, fl = 1; break;
    default: return 0; /* VMLA/VMLS.F32 by scalar: not fused, interpreter */
    }
    if (fl) jx_fpcr_std(x);
    int idx = i->imm, h, l, m;
    if (size == 1) h = (idx >> 2) & 1, l = (idx >> 1) & 1, m = idx & 1;
    else h = (idx >> 1) & 1, l = idx & 1, m = 0;
    /* the scalar must be in v0-v15 for 16-bit elements */
    int sreg = rd_d(b, i->vm, S4);
    if (sreg != S4) emit(b, a64_mov_v(0, S4, sreg));
    if (lng) {
        int n = rd_d(b, i->vn, S1);
        emit(b, a64_simd_elem(0, u, size, opc, qreg(i->vd >> 1), n, S4, l, m, h));
        return 1;
    }
    int n = src_n(b, i->vn, q, S1);
    int d = q ? qreg(i->vd >> 1) : (acc ? acc_d(b, i->vd, S0) : S0);
    emit(b, a64_simd_elem(q, u, fl ? 2 : size, opc, d, n, S4, l, m, h));
    if (!q) wr_d(b, i->vd, S0);
    return 1;
}

static int neon_ldst(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    if (i->unpred || i->rn == 15 || i->sub != N_VLDST_MULTI) return 0;
    int load = (i->flags & VF_LOAD) != 0;
    unsigned n = i->imm, regs = i->nregs, inc = i->inc, total;
    int addr = i->rn;
    if (n == 1) {
        /* VLD1/VST1: bytes map straight onto consecutive D registers */
        if (i->vd + regs > 32) return 0;
        total = 8 * regs;
        unsigned pos = 0;
        for (unsigned k = 0; k < regs;) {
            unsigned r = i->vd + k;
            if (!(r & 1) && k + 1 < regs) {
                ldst_q(b, load, qreg(r >> 1), addr, pos);
                k += 2, pos += 16;
            } else {
                if (load) {
                    emit(b, a64_ldst_imm(3, 1, T1, addr, pos));
                    x_to_d(b, r, T1);
                } else {
                    d_to_x(b, T1, r);
                    emit(b, a64_ldst_imm(3, 0, T1, addr, pos));
                }
                k += 1, pos += 8;
            }
        }
    } else {
        /* VLDn/VSTn (n = 2..4): A64 LDn/STn on v0.. with the same element size; regs = 2 only for VLD2 */
        unsigned q = regs == 2;
        if (i->vd + (n - 1) * inc + regs - 1 > 31 || i->esize == 3) return 0;
        total = 8 * n * regs;
        if (!load) {
            for (unsigned s = 0; s < n; s++) {
                unsigned r = i->vd + s * inc;
                int v = rd_d(b, r, S0 + s);
                if (v != S0 + (int)s) emit(b, a64_mov_v(0, S0 + s, v));
                if (q) emit(b, a64_ins_elem(S0 + s, qreg((r + 1) >> 1), a64_imm5(3, 1), a64_imm4(3, (r + 1) & 1)));
            }
        }
        static const uint32_t opc[5] = {0, 0, 0x8, 0x4, 0x0}; /* LD2 1000, LD3 0100, LD4 0000 */
        emit(b, 0x0c000000u | q << 30 | (uint32_t)load << 22 | opc[n] << 12 | (uint32_t)i->esize << 10 |
                    (uint32_t)addr << 5 | S0);
        if (load) {
            for (unsigned s = 0; s < n; s++) {
                unsigned r = i->vd + s * inc;
                wr_d(b, r, S0 + s);
                if (q) emit(b, a64_ins_elem(qreg((r + 1) >> 1), S0 + s, a64_imm5(3, (r + 1) & 1), a64_imm4(3, 1)));
            }
        }
    }
    if (i->rm == 13) emit(b, a64_addsub_imm(0, 0, 0, i->rn, i->rn, total, 0));
    else if (i->rm != 15) emit(b, a64_addsub_reg(0, 0, 0, i->rn, i->rn, i->rm, 0, 0));
    return 1;
}

static int neon(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    unsigned op = i->sub;
    jit_vfp_flush(x);
    if (op == N_VLDST_MULTI || op == N_VLDST_LANE || op == N_VLD_ALL) return neon_ldst(x, i);
    if (op <= N_VPADD) return three_same_int(x, i);
    if (op <= N_FMINNM) return three_same_float(x, i);
    if (op <= N_VMULLP) return long_ops(x, i);
    if (op <= N_FMUL_S) return by_scalar(x, i);
    if (op <= N_VMOVL) return shift_imm(x, i);
    if (op == N_VMOV_I || op == N_VMVN_I || op == N_VORR_I || op == N_VBIC_I) {
        unsigned nd = i->q ? 2 : 1;
        jx_mov64(b, T1, i->imm64);
        for (unsigned k = 0; k < nd; k++) {
            unsigned r = i->vd + k;
            if (op == N_VMOV_I || op == N_VMVN_I) {
                x_to_d(b, r, T1);
            } else {
                d_to_x(b, T0, r);
                emit(b, a64_logic_reg(1, op == N_VORR_I ? 1 : 0, 0, T0, T0, T1, 0, 0));
                x_to_d(b, r, T0);
            }
        }
        return 1;
    }
    if (op <= N_SHA256SU0) return misc2(x, i);
    if (op == N_VEXT) {
        int q = i->q;
        int n = src_n(b, i->vn, q, S1), m = src_n(b, i->vm, q, S2);
        int d = q ? qreg(i->vd >> 1) : S0;
        emit(b, a64_ext(q, d, n, m, i->imm));
        if (!q) wr_d(b, i->vd, S0);
        return 1;
    }
    if (op == N_VDUP_S) {
        int m = rd_d(b, i->vm, S1);
        int imm5 = a64_imm5(i->esize, i->imm);
        if (i->q) {
            emit(b, a64_dup_elem(1, qreg(i->vd >> 1), m, imm5));
        } else {
            emit(b, a64_dup_elem(0, S0, m, imm5));
            wr_d(b, i->vd, S0);
        }
        return 1;
    }
    return 0;
}

int jit_vfp(jit_ctx *x, const insn_t *i)
{
    if (i->unpred) return 0;
    if (i->op == OP_VFP) {
        if (i->sub >= V_MOV_CS) return vfp_transfer(x, i);
        return vfp_arith(x, i);
    }
    return neon(x, i);
}
