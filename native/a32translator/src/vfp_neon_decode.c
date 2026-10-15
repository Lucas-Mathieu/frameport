/* VFP and Advanced SIMD decoder (A32 encodings; T32 ones are rewritten by the caller).
 *
 * Fields:
 *   VFP: dp = double precision; vd/vn/vm = S index (single) or D index (double), unless noted in insn.h.
 *   NEON: vd/vn/vm = D register numbers (Q n = D 2n), q = 128-bit, esize = log2 of the element size in bytes,
 *         VF_SIGNED = signed (U == 0), imm = shift amount / lane / rounding, imm64 = modified immediate. */
#include "insn.h"

static void undef(insn_t *i)
{
    i->op = OP_UNDEF;
}

#define VD_S(w) (BITS(w, 15, 12) << 1 | BIT(w, 22))
#define VN_S(w) (BITS(w, 19, 16) << 1 | BIT(w, 7))
#define VM_S(w) (BITS(w, 3, 0) << 1 | BIT(w, 5))
#define VD_D(w) (BIT(w, 22) << 4 | BITS(w, 15, 12))
#define VN_D(w) (BIT(w, 7) << 4 | BITS(w, 19, 16))
#define VM_D(w) (BIT(w, 5) << 4 | BITS(w, 3, 0))

static void vregs(insn_t *i, uint32_t w)
{
    if (i->dp) {
        i->vd = VD_D(w);
        i->vn = VN_D(w);
        i->vm = VM_D(w);
    } else {
        i->vd = VD_S(w);
        i->vn = VN_S(w);
        i->vm = VM_S(w);
    }
}

/* ---------------------------------------------------------------- VFP */

static void vfp_dp(uint32_t w, insn_t *i)
{
    unsigned opc1 = BITS(w, 23, 23) << 2 | BITS(w, 21, 20), op = BIT(w, 6), opc2 = BITS(w, 19, 16), opc3 = BITS(w, 7, 6);
    i->op = OP_VFP;
    i->dp = BIT(w, 8);
    vregs(i, w);
    switch (opc1) {
    case 0: i->sub = op ? V_MLS : V_MLA; return;
    case 1: i->sub = op ? V_NMLA : V_NMLS; return;
    case 2: i->sub = op ? V_NMUL : V_MUL; return;
    case 3: i->sub = op ? V_SUB : V_ADD; return;
    case 4: if (op) undef(i); else i->sub = V_DIV; return;
    case 5: i->sub = op ? V_FNMA : V_FNMS; return;
    case 6: i->sub = op ? V_FMS : V_FMA; return;
    default: break;
    }
    if (!(opc3 & 1)) { /* VMOV immediate */
        unsigned imm8 = BITS(w, 19, 16) << 4 | BITS(w, 3, 0);
        i->sub = V_MOVI;
        if (i->dp) {
            uint64_t sign = (uint64_t)BIT(imm8, 7) << 63, b6 = BIT(imm8, 6);
            uint64_t exp = ((b6 ^ 1) << 10) | (b6 ? 0x3fcu : 0) | BITS(imm8, 5, 4);
            i->imm64 = sign | exp << 52 | (uint64_t)BITS(imm8, 3, 0) << 48;
        } else {
            uint32_t sign = BIT(imm8, 7) << 31, b6 = BIT(imm8, 6);
            uint32_t exp = ((b6 ^ 1) << 7) | (b6 ? 0x7cu : 0) | BITS(imm8, 5, 4);
            i->imm64 = sign | exp << 23 | BITS(imm8, 3, 0) << 19;
        }
        if (BITS(w, 7, 4) & 0xa) i->unpred = 1; /* bits 7 and 5 should be zero */
        return;
    }
    switch (opc2) {
    case 0: i->sub = BIT(w, 7) ? V_ABS : V_MOV; return;
    case 1: i->sub = BIT(w, 7) ? V_SQRT : V_NEG; return;
    case 2: case 3: /* VCVTB/VCVTT half <-> single/double */
        i->sub = V_CVT_HALF;
        if (BIT(w, 7)) i->flags |= VF_TOP;
        if (opc2 & 1) i->flags |= VF_TOHALF;
        /* the half-precision side is always an S register */
        if (i->dp) {
            if (opc2 & 1) i->vm = VM_D(w), i->vd = VD_S(w);
            else i->vm = VM_S(w), i->vd = VD_D(w);
        }
        return;
    case 4: case 5:
        i->sub = V_CMP;
        if (BIT(w, 7)) i->flags |= VF_E;
        if (opc2 & 1) {
            i->flags |= VF_ZERO;
            if (BITS(w, 5, 0) & 0x2f) i->unpred = 1; /* Vm, M should be zero */
        }
        return;
    case 6:
        i->sub = V_RINT;
        i->imm = BIT(w, 7) ? 3 : 4; /* VRINTZ / VRINTR */
        return;
    case 7:
        if (BIT(w, 7)) { /* VCVT between double and single */
            i->sub = V_CVT_DS;
            if (i->dp) i->vd = VD_S(w), i->vm = VM_D(w);
            else i->vd = VD_D(w), i->vm = VM_S(w);
        } else {
            i->sub = V_RINT;
            i->imm = 5; /* VRINTX */
        }
        return;
    case 8: /* VCVT int -> float; the integer is in an S register */
        i->sub = V_CVT_I2F;
        if (BIT(w, 7)) i->flags |= VF_SIGNED;
        i->vm = VM_S(w);
        return;
    case 10: case 11: case 14: case 15: { /* VCVT between float and fixed-point, in place */
        unsigned sx = BIT(w, 7), size = sx ? 32 : 16, imm = BITS(w, 3, 0) << 1 | BIT(w, 5);
        i->sub = V_CVT_FIX;
        if (opc2 & 4) i->flags |= VF_TOFIX;
        if (!(opc2 & 1)) i->flags |= VF_SIGNED;
        i->size = size;
        if (imm > size) i->unpred = 1;
        i->imm = size - imm;
        i->vm = i->vd;
        return;
    }
    case 12: case 13: /* VCVT/VCVTR float -> int; result in an S register */
        i->sub = V_CVT_F2I;
        if (opc2 & 1) i->flags |= VF_SIGNED;
        i->imm = BIT(w, 7) ? 3 : 4;
        i->vd = VD_S(w);
        return;
    default:
        undef(i);
    }
}

static void vfp_uncond(uint32_t w, insn_t *i)
{
    /* ARMv8: VSEL, VMAXNM/VMINNM, VRINT{A,N,P,M}, VCVT{A,N,P,M} */
    i->op = OP_VFP;
    i->dp = BIT(w, 8);
    vregs(i, w);
    if (BITS(w, 23, 23) == 0) {
        if (BIT(w, 6)) { undef(i); return; }
        static const uint8_t cc[4] = {C_EQ, C_VS, C_GE, C_GT};
        i->sub = V_SEL;
        i->imm = cc[BITS(w, 21, 20)];
        return;
    }
    if (BITS(w, 21, 20) == 0) {
        i->sub = BIT(w, 6) ? V_MINNM : V_MAXNM;
        return;
    }
    if (BITS(w, 21, 18) == 0xe && BITS(w, 7, 6) == 1) {
        static const uint8_t rm[4] = {6, 0, 1, 2}; /* A N P M */
        i->sub = V_RINT;
        i->imm = rm[BITS(w, 17, 16)];
        return;
    }
    if (BITS(w, 21, 18) == 0xf && BIT(w, 6)) {
        static const uint8_t rm[4] = {6, 0, 1, 2};
        i->sub = V_CVT_F2I;
        i->imm = rm[BITS(w, 17, 16)];
        if (BIT(w, 7)) i->flags |= VF_SIGNED;
        i->vd = VD_S(w);
        return;
    }
    undef(i);
}

static void vfp_transfer(uint32_t w, insn_t *i)
{
    unsigned l = BIT(w, 20), c = BIT(w, 8), a = BITS(w, 23, 21), b = BITS(w, 6, 5);
    i->op = OP_VFP;
    i->rd = BITS(w, 15, 12);
    if (!c) {
        if (a == 0) {
            i->sub = V_MOV_CS;
            i->vn = VN_S(w);
            if (l) i->flags |= VF_TOCORE;
            if (i->rd == 15 || i->rd == 13) i->unpred = 1;
            return;
        }
        if (a == 7) {
            i->sub = l ? V_MRS : V_MSR;
            i->imm = BITS(w, 19, 16);
            if (l) {
                if (i->imm != 1) { undef(i); return; } /* FPSID/MVFRx/FPEXC: not accessible at PL0 */
            } else {
                if (i->imm != 1) { undef(i); return; } /* only FPSCR is writable in user mode */
                if (i->rd == 15) i->unpred = 1;
            }
            return;
        }
        undef(i);
        return;
    }
    unsigned opc1 = BITS(w, 22, 21), opc2 = b;
    if (!l && (a & 4)) { /* VDUP (core register) */
        unsigned be = BIT(w, 22) << 1 | BIT(w, 5);
        if (be == 3 || BIT(w, 6)) { undef(i); return; }
        if (BITS(w, 3, 0)) i->unpred = 1; /* should be zero */
        i->sub = V_DUP;
        i->q = BIT(w, 21);
        i->vd = BIT(w, 7) << 4 | BITS(w, 19, 16);
        i->size = be == 0 ? 4 : be == 1 ? 2 : 1;
        if (i->q && (i->vd & 1)) { undef(i); return; }
        if (i->rd == 15) i->unpred = 1;
        return;
    }
    i->sub = V_MOV_SCALAR;
    i->vd = BIT(w, 7) << 4 | BITS(w, 19, 16);
    if (opc1 & 2) {
        i->size = 1;
        i->imm = (opc1 & 1) << 2 | opc2;
    } else if (opc2 & 1) {
        i->size = 2;
        i->imm = (opc1 & 1) << 1 | (opc2 >> 1);
    } else if (opc2 == 0) {
        i->size = 4;
        i->imm = opc1 & 1;
    } else {
        undef(i);
        return;
    }
    if (l) {
        i->flags |= VF_TOCORE;
        if (!BIT(w, 23)) i->flags |= VF_SIGNED;
        if (BIT(w, 23) && i->size == 4) { undef(i); return; }
    }
    if (i->rd == 15) i->unpred = 1;
}

static void vfp_ldst(uint32_t w, insn_t *i)
{
    unsigned opc = BITS(w, 24, 20), p = BIT(w, 24), u = BIT(w, 23), wb = BIT(w, 21), l = BIT(w, 20);
    unsigned dp = BIT(w, 8), imm8 = w & 0xff;
    i->op = OP_VFP;
    i->dp = dp;
    i->rn = BITS(w, 19, 16);
    i->vd = dp ? VD_D(w) : VD_S(w);
    if (l) i->flags |= VF_LOAD;
    if ((opc & 0x1e) == 0x04) { /* 64-bit transfers */
        i->rd = BITS(w, 15, 12);
        i->ra = BITS(w, 19, 16);
        i->rn = 0;
        if (BITS(w, 7, 6) != 0 || BIT(w, 4) != 1) { undef(i); return; }
        if (dp) {
            i->sub = V_MOV_CD;
            i->vm = VM_D(w);
        } else {
            i->sub = V_MOV_CSS;
            i->vm = VM_S(w);
            if (i->vm == 31) i->unpred = 1;
        }
        if (l) i->flags |= VF_TOCORE;
        if (i->rd == 15 || i->ra == 15 || i->rd == 13 || i->ra == 13 || (l && i->rd == i->ra)) i->unpred = 1;
        return;
    }
    if (p && !wb) { /* VLDR / VSTR */
        i->sub = V_LDST;
        i->u = u;
        i->imm = imm8 << 2;
        return;
    }
    if (p == u) { undef(i); return; } /* P=U=0 (64-bit transfers handled above) or P=U=1 with W=1 */
    i->sub = V_LDSTM;
    i->p = p;
    i->u = u;
    i->w = wb;
    i->nregs = dp ? imm8 / 2 : imm8;
    i->imm = imm8; /* odd imm8 with dp = FLDMX/FSTMX: one extra word */
    if (i->nregs == 0 || i->vd + i->nregs > 32 || (dp && i->nregs > 16)) i->unpred = 1;
    if (i->rn == 15 && wb) i->unpred = 1;
}

/* ---------------------------------------------------------------- Advanced SIMD */

static int qcheck(insn_t *i, int d, int n, int m)
{
    if (!i->q) return 0;
    if ((d && (i->vd & 1)) || (n && (i->vn & 1)) || (m && (i->vm & 1))) {
        undef(i);
        return 1;
    }
    return 0;
}

static void neon_3same(uint32_t w, insn_t *i)
{
    unsigned a = BITS(w, 11, 8), b = BIT(w, 4), u = BIT(w, 24), c = BITS(w, 21, 20), size = c;
    i->esize = size;
    if (!u) i->flags |= VF_SIGNED;
    int fl = 0; /* float op: size field is sz:op */
    switch (a) {
    case 0: i->sub = b ? N_VQADD : N_VHADD; break;
    case 1:
        if (!b) { i->sub = N_VRHADD; break; }
        {
            static const uint8_t lo[4] = {N_VAND, N_VBIC, N_VORR, N_VORN}, hi[4] = {N_VEOR, N_VBSL, N_VBIT, N_VBIF};
            i->sub = u ? hi[c] : lo[c];
            i->esize = 0;
        }
        break;
    case 2: i->sub = b ? N_VQSUB : N_VHSUB; break;
    case 3: i->sub = b ? N_VCGE : N_VCGT; break;
    case 4: i->sub = b ? N_VQSHL : N_VSHL; break;
    case 5: i->sub = b ? N_VQRSHL : N_VRSHL; break;
    case 6: i->sub = b ? N_VMIN : N_VMAX; break;
    case 7: i->sub = b ? N_VABA : N_VABD; break;
    case 8:
        if (!b) i->sub = u ? N_VSUB : N_VADD;
        else i->sub = u ? N_VCEQ : N_VTST;
        break;
    case 9:
        if (!b) i->sub = u ? N_VMLS : N_VMLA;
        else {
            i->sub = u ? N_VMULP : N_VMUL;
            if (u && size != 0) { undef(i); return; }
        }
        if (size == 3) { undef(i); return; }
        break;
    case 10:
        i->sub = b ? N_VPMIN : N_VPMAX;
        if (size == 3 || i->q) { undef(i); return; }
        break;
    case 11:
        if (!b) {
            i->sub = u ? N_VQRDMULH : N_VQDMULH;
            i->flags |= VF_SIGNED;
            if (size == 0 || size == 3) { undef(i); return; }
        } else {
            if (u) { undef(i); return; } /* VQRDMLAH (v8.1) */
            i->sub = N_VPADD;
            if (size == 3 || i->q) { undef(i); return; }
        }
        break;
    case 12:
        if (!b) {
            /* SHA-1/SHA-256 three-register (ARMv8 Crypto) */
            static const uint8_t lo[4] = {N_SHA1C, N_SHA1P, N_SHA1M, N_SHA1SU0}, hi[4] = {N_SHA256H, N_SHA256H2,
                                                                                         N_SHA256SU1, 0};
            if (!i->q || (u && c == 3)) { undef(i); return; }
            i->sub = u ? hi[c] : lo[c];
            i->esize = 2;
            break;
        }
        if (u) { undef(i); return; }
        i->sub = (c & 2) ? N_FMS : N_FMA;
        fl = 1;
        break;
    case 13:
        if (!b) {
            if (!u) i->sub = (c & 2) ? N_FSUB : N_FADD;
            else i->sub = (c & 2) ? N_FABD : N_FPADD;
        } else {
            if (!u) i->sub = (c & 2) ? N_FMLS : N_FMLA;
            else if (!(c & 2)) i->sub = N_FMUL;
            else { undef(i); return; }
        }
        fl = 1;
        break;
    case 14:
        if (!b) {
            if (!u) {
                if (c & 2) { undef(i); return; }
                i->sub = N_FCEQ;
            } else {
                i->sub = (c & 2) ? N_FCGT : N_FCGE;
            }
        } else {
            if (!u) { undef(i); return; }
            i->sub = (c & 2) ? N_FACGT : N_FACGE;
        }
        fl = 1;
        break;
    case 15:
        if (!b) {
            if (!u) i->sub = (c & 2) ? N_FMIN : N_FMAX;
            else i->sub = (c & 2) ? N_FPMIN : N_FPMAX;
        } else {
            if (!u) i->sub = (c & 2) ? N_FRSQRTS : N_FRECPS;
            else i->sub = (c & 2) ? N_FMINNM : N_FMAXNM;
        }
        fl = 1;
        break;
    }
    if (fl) {
        if (c & 1) { undef(i); return; } /* half precision (v8.2) */
        i->esize = 2;
        if ((i->sub == N_FPADD || i->sub == N_FPMAX || i->sub == N_FPMIN) && i->q) { undef(i); return; }
    }
    qcheck(i, 1, 1, 1);
}

static void neon_3diff(uint32_t w, insn_t *i)
{
    unsigned a = BITS(w, 11, 8), u = BIT(w, 24), size = BITS(w, 21, 20);
    static const uint8_t ops[16] = {N_VADDL, N_VADDW, N_VSUBL, N_VSUBW, N_VADDHN, N_VABAL, N_VSUBHN, N_VABDL,
                                    N_VMLAL, N_VQDMLAL, N_VMLSL, N_VQDMLSL, N_VMULL, N_VQDMULL, N_VMULLP, 0};
    i->esize = size;
    if (!u) i->flags |= VF_SIGNED;
    if (a == 15) { undef(i); return; }
    i->sub = ops[a];
    if (a == 4 && u) i->sub = N_VRADDHN;
    if (a == 6 && u) i->sub = N_VRSUBHN;
    if ((a == 9 || a == 11 || a == 13) && (u || size == 0)) { undef(i); return; }
    if (a == 14) {
        if (u || (size != 0 && size != 2)) { undef(i); return; }
    }
    /* operand widths: long (L) = D, D -> Q; wide (W) = Q, D -> Q; narrow (HN) = Q, Q -> D */
    int qd = !(a == 4 || a == 6), qn = (a == 1 || a == 3 || a == 4 || a == 6), qm = (a == 4 || a == 6);
    if ((qd && (i->vd & 1)) || (qn && (i->vn & 1)) || (qm && (i->vm & 1))) undef(i);
}

static void neon_scalar(uint32_t w, insn_t *i)
{
    unsigned a = BITS(w, 11, 8), u = BIT(w, 24), size = BITS(w, 21, 20);
    i->esize = size;
    if (size == 0) { undef(i); return; }
    if (size == 1) {
        i->vm = BITS(w, 2, 0);
        i->imm = BIT(w, 5) << 1 | BIT(w, 3);
    } else {
        i->vm = BITS(w, 3, 0);
        i->imm = BIT(w, 5);
    }
    if (!u) i->flags |= VF_SIGNED;
    int lng = 0;
    switch (a) {
    case 0: i->sub = N_VMLA_S; i->q = u; break;
    case 1: i->sub = N_FMLA_S; i->q = u; break;
    case 4: i->sub = N_VMLS_S; i->q = u; break;
    case 5: i->sub = N_FMLS_S; i->q = u; break;
    case 2: i->sub = N_VMLAL_S; lng = 1; break;
    case 6: i->sub = N_VMLSL_S; lng = 1; break;
    case 3: i->sub = N_VQDMLAL_S; lng = 1; if (u) { undef(i); return; } break;
    case 7: i->sub = N_VQDMLSL_S; lng = 1; if (u) { undef(i); return; } break;
    case 8: i->sub = N_VMUL_S; i->q = u; break;
    case 9: i->sub = N_FMUL_S; i->q = u; break;
    case 10: i->sub = N_VMULL_S; lng = 1; break;
    case 11: i->sub = N_VQDMULL_S; lng = 1; if (u) { undef(i); return; } break;
    case 12: i->sub = N_VQDMULH_S; i->q = u; break;
    case 13: i->sub = N_VQRDMULH_S; i->q = u; break;
    default: undef(i); return;
    }
    if (i->sub == N_FMLA_S || i->sub == N_FMLS_S || i->sub == N_FMUL_S) {
        if (size != 2) { undef(i); return; }
        i->flags &= ~VF_SIGNED;
    }
    if (i->sub == N_VMLA_S || i->sub == N_VMLS_S || i->sub == N_VMUL_S || i->sub == N_FMLA_S || i->sub == N_FMLS_S ||
        i->sub == N_FMUL_S || i->sub == N_VQDMULH_S || i->sub == N_VQRDMULH_S)
        i->flags &= ~VF_SIGNED;
    if (lng) {
        if (i->vd & 1) undef(i);
    } else if (i->q && ((i->vd & 1) || (i->vn & 1))) {
        undef(i);
    }
}

static void neon_shift(uint32_t w, insn_t *i)
{
    unsigned a = BITS(w, 11, 8), u = BIT(w, 24), l = BIT(w, 7), b = BIT(w, 6), imm6 = BITS(w, 21, 16);
    unsigned esize; /* log2 bytes of the (source) elements */
    if (l) esize = 3;
    else if (imm6 & 0x20) esize = 2;
    else if (imm6 & 0x10) esize = 1;
    else esize = 0;
    unsigned bits = 8u << esize;
    unsigned lshift = (l ? 64 + imm6 : imm6) - bits;  /* for left shifts: imm6 - esize */
    unsigned rshift = 2 * bits - (l ? 64 + imm6 : imm6); /* for right shifts: 2*esize - imm6 */
    i->esize = esize;
    if (!u) i->flags |= VF_SIGNED;
    switch (a) {
    case 0: i->sub = N_VSHR; i->imm = rshift; break;
    case 1: i->sub = N_VSRA; i->imm = rshift; break;
    case 2: i->sub = N_VRSHR; i->imm = rshift; break;
    case 3: i->sub = N_VRSRA; i->imm = rshift; break;
    case 4: if (!u) { undef(i); return; } i->sub = N_VSRI; i->imm = rshift; break;
    case 5: i->sub = u ? N_VSLI : N_VSHL_I; i->imm = lshift; i->flags &= ~VF_SIGNED; break;
    case 6: if (!u) { undef(i); return; } i->sub = N_VQSHLU_I; i->imm = lshift; i->flags |= VF_SIGNED; break;
    case 7: i->sub = N_VQSHL_I; i->imm = lshift; break;
    case 8: case 9: {
        if (l) { undef(i); return; }
        /* narrowing: imm6 encodes the result element size (esize), the source elements are twice as wide */
        i->imm = rshift;
        if (a == 8) {
            if (!u) i->sub = b ? N_VRSHRN : N_VSHRN;
            else i->sub = b ? N_VQRSHRUN : N_VQSHRUN;
            if (u) i->flags |= VF_SIGNED; /* VQSHRUN takes signed input */
        } else {
            i->sub = b ? N_VQRSHRN : N_VQSHRN;
        }
        if (i->vm & 1) undef(i);
        i->q = 0;
        return;
    }
    case 10:
        if (l || b) { undef(i); return; }
        if (esize == 3) { undef(i); return; }
        i->sub = lshift ? N_VSHLL : N_VMOVL;
        i->imm = lshift;
        if (i->vd & 1) undef(i);
        i->q = 0;
        return;
    case 14: case 15:
        if (l || !(imm6 & 0x20)) { undef(i); return; }
        i->sub = N_VCVT_FIXF;
        i->esize = 2;
        i->imm = 64 - imm6;
        if (a & 1) i->flags |= VF_TOFIX;
        break;
    default:
        undef(i);
        return;
    }
    qcheck(i, 1, 0, 1);
}

static uint64_t adv_simd_expand_imm(unsigned op, unsigned cmode, unsigned imm8, int *bad)
{
    uint64_t b = imm8, r = 0;
    *bad = 0;
    switch (cmode >> 1) {
    case 0: r = b * 0x0000000100000001ull; break;
    case 1: r = (b << 8) * 0x0000000100000001ull; break;
    case 2: r = (b << 16) * 0x0000000100000001ull; break;
    case 3: r = (b << 24) * 0x0000000100000001ull; break;
    case 4: r = b * 0x0001000100010001ull; break;
    case 5: r = (b << 8) * 0x0001000100010001ull; break;
    case 6:
        if (!(cmode & 1)) r = ((b << 8) | 0xff) * 0x0000000100000001ull;
        else r = ((b << 16) | 0xffff) * 0x0000000100000001ull;
        break;
    case 7:
        if (!(cmode & 1) && !op) {
            r = b * 0x0101010101010101ull;
        } else if (!(cmode & 1) && op) {
            for (int k = 0; k < 8; k++)
                if (b & (1u << k)) r |= 0xffull << (8 * k);
        } else if ((cmode & 1) && !op) {
            uint32_t f = (uint32_t)BIT(imm8, 7) << 31 | (uint32_t)(BIT(imm8, 6) ^ 1) << 30 |
                         (BIT(imm8, 6) ? 0x3e000000u : 0) | (uint32_t)BITS(imm8, 5, 0) << 19;
            r = (uint64_t)f << 32 | f;
        } else {
            *bad = 1;
        }
        break;
    }
    return r;
}

static void neon_modimm(uint32_t w, insn_t *i)
{
    unsigned cmode = BITS(w, 11, 8), op = BIT(w, 5);
    unsigned imm8 = BIT(w, 24) << 7 | BITS(w, 18, 16) << 4 | BITS(w, 3, 0);
    int bad;
    i->imm64 = adv_simd_expand_imm(op, cmode, imm8, &bad);
    if (bad) { undef(i); return; }
    int orr_bic = (cmode < 12) && (cmode & 1);
    if (!op) i->sub = orr_bic ? N_VORR_I : N_VMOV_I;
    else if (cmode == 14) i->sub = N_VMOV_I;
    else i->sub = orr_bic ? N_VBIC_I : N_VMVN_I;
    if (i->sub == N_VMVN_I) i->imm64 = ~i->imm64;
    if (i->sub == N_VBIC_I) i->imm64 = ~i->imm64; /* stored as the AND mask */
    qcheck(i, 1, 0, 0);
}

static void neon_misc(uint32_t w, insn_t *i)
{
    unsigned a = BITS(w, 17, 16), b = BITS(w, 10, 6), size = BITS(w, 19, 18);
    i->esize = size;
    switch (a) {
    case 0:
        if (b <= 1) { i->sub = N_VREV64; if (size == 3) undef(i); break; }
        if (b <= 3) { i->sub = N_VREV32; if (size >= 2) undef(i); break; }
        if (b <= 5) { i->sub = N_VREV16; if (size >= 1) undef(i); break; }
        if (b >= 8 && b <= 11) { i->sub = N_VPADDL; if (!BIT(w, 7)) i->flags |= VF_SIGNED; if (size == 3) undef(i); break; }
        if (b >= 12 && b <= 15) {
            static const uint8_t c[4] = {N_AESE, N_AESD, N_AESMC, N_AESIMC};
            i->q = 1;
            if (size != 0) { undef(i); return; }
            i->sub = c[b - 12];
            break;
        }
        if (b == 16 || b == 17) { i->sub = N_VCLS; if (size == 3) undef(i); break; }
        if (b == 18 || b == 19) { i->sub = N_VCLZ; if (size == 3) undef(i); break; }
        if (b == 20 || b == 21) { i->sub = N_VCNT; if (size != 0) undef(i); break; }
        if (b == 22 || b == 23) { i->sub = N_VMVN; if (size != 0) undef(i); break; }
        if (b >= 24 && b <= 27) { i->sub = N_VPADAL; if (!BIT(w, 7)) i->flags |= VF_SIGNED; if (size == 3) undef(i); break; }
        if (b == 28 || b == 29) { i->sub = N_VQABS; if (size == 3) undef(i); break; }
        if (b == 30 || b == 31) { i->sub = N_VQNEG; if (size == 3) undef(i); break; }
        undef(i);
        return;
    case 1: {
        unsigned f, k = BITS(b, 4, 1);
        if (b == 0x0b) { /* SHA1H */
            if (size != 2) { undef(i); return; }
            i->sub = N_SHA1H;
            i->q = 1;
            break;
        }
        static const uint8_t ints[8] = {N_VCGT0, N_VCGE0, N_VCEQ0, N_VCLE0, N_VCLT0, 0xff, N_VABS, N_VNEG};
        static const uint8_t flts[8] = {N_FCGT0, N_FCGE0, N_FCEQ0, N_FCLE0, N_FCLT0, 0xff, N_FABS, N_FNEG};
        unsigned idx = k & 7;
        f = BIT(b, 4);
        if (ints[idx] == 0xff) { undef(i); return; }
        if (f) {
            if (size != 2) { undef(i); return; }
            i->sub = flts[idx];
        } else {
            if (size == 3) { undef(i); return; }
            i->sub = ints[idx];
        }
        break;
    }
    case 2:
        if (b <= 1) { i->sub = N_VSWP; if (size != 0) undef(i); break; }
        if (b <= 7 && i->vd == i->vm) i->unpred = 1; /* VTRN/VUZP/VZIP with d == m */
        if (b <= 3) { i->sub = N_VTRN; if (size == 3) undef(i); break; }
        if (b <= 5) { i->sub = N_VUZP; if (size == 3 || (!i->q && size == 2)) undef(i); break; }
        if (b <= 7) { i->sub = N_VZIP; if (size == 3 || (!i->q && size == 2)) undef(i); break; }
        if (b == 8) { i->sub = N_VMOVN; goto narrow; }
        if (b == 9) { i->sub = N_VQMOVUN; i->flags |= VF_SIGNED; goto narrow; }
        if (b == 10 || b == 11) {
            i->sub = N_VQMOVN;
            if (!(b & 1)) i->flags |= VF_SIGNED;
        narrow:
            if (size == 3 || (i->vm & 1)) undef(i);
            i->q = 0;
            return;
        }
        if (b == 12) {
            i->sub = N_VSHLL_MAX;
            i->imm = 8u << size;
            if (size == 3 || (i->vd & 1)) undef(i);
            i->q = 0;
            return;
        }
        if (b == 14 || b == 15) {
            if (size != 2) { undef(i); return; }
            i->sub = b == 14 ? N_SHA1SU1 : N_SHA256SU0;
            i->q = 1;
            break;
        }
        if (b == 0x18 || b == 0x1c) { /* VCVT.F16.F32 / VCVT.F32.F16 */
            if (size != 1) { undef(i); return; }
            i->sub = N_VCVT_HS;
            i->q = 0;
            if (b == 0x18) {
                i->flags |= VF_TOHALF;
                if (i->vm & 1) undef(i);
            } else if (i->vd & 1) {
                undef(i);
            }
            return;
        }
        if (b >= 0x10) { /* VRINT{N,X,A,Z,M,P} */
            static const signed char rm[8] = {0, 5, 6, 3, -1, 2, -1, 1};
            int r = rm[BITS(b, 3, 1)];
            if (r < 0 || size != 2) { undef(i); return; }
            i->sub = N_FRINT;
            i->imm = r;
            break;
        }
        undef(i);
        return;
    case 3:
        if ((b & 0x18) == 0x10) { /* VRECPE / VRSQRTE */
            if (size != 2) { undef(i); return; }
            int fl = BIT(b, 2);
            i->sub = BIT(b, 1) ? (fl ? N_FRSQRTE : N_VRSQRTE) : (fl ? N_FRECPE : N_VRECPE);
            break;
        }
        if ((b & 0x18) == 0x18) { /* VCVT float <-> int */
            if (size != 2) { undef(i); return; }
            i->sub = N_VCVT_FI;
            unsigned op = BITS(b, 2, 1);
            if (op & 2) i->flags |= VF_TOFIX;
            if (!(op & 1)) i->flags |= VF_SIGNED;
            break;
        }
        if ((b & 0x10) == 0x00) { /* VCVT{A,N,P,M} */
            static const uint8_t rm[4] = {6, 0, 1, 2};
            if (size != 2) { undef(i); return; }
            i->sub = N_VCVT_RND;
            i->imm = rm[BITS(b, 3, 2)];
            if (!BIT(b, 1)) i->flags |= VF_SIGNED;
            break;
        }
        undef(i);
        return;
    }
    if (i->op != OP_UNDEF) qcheck(i, 1, 0, 1);
}

static void neon_dp(uint32_t w, insn_t *i)
{
    unsigned u = BIT(w, 24), a = BITS(w, 23, 19), b = BITS(w, 11, 8), c = BITS(w, 7, 4);
    i->op = OP_NEON;
    i->vd = VD_D(w);
    i->vn = VN_D(w);
    i->vm = VM_D(w);
    i->q = BIT(w, 6);
    if (!(a & 0x10)) {
        neon_3same(w, i);
        return;
    }
    if ((c & 1)) {
        if ((a & 0x17) == 0x10 && !(c & 8)) {
            neon_modimm(w, i);
            return;
        }
        neon_shift(w, i);
        return;
    }
    if ((a & 0x16) != 0x16) {
        /* A = 1x0xx or 1x10x */
        if (!(c & 4)) {
            i->q = 0;
            neon_3diff(w, i);
        } else {
            neon_scalar(w, i);
        }
        return;
    }
    /* A = 1x11x */
    if (!u) { /* VEXT */
        i->sub = N_VEXT;
        i->imm = b;
        if (!i->q && (b & 8)) { undef(i); return; }
        qcheck(i, 1, 1, 1);
        return;
    }
    if (!(b & 8)) {
        neon_misc(w, i);
        return;
    }
    if ((b & 0xc) == 8) { /* VTBL / VTBX */
        i->sub = N_VTBL;
        i->nregs = BITS(w, 9, 8) + 1;
        if (BIT(w, 6)) i->flags |= VF_E;
        i->q = 0;
        if (i->vn + i->nregs > 32) i->unpred = 1;
        return;
    }
    if (b == 12 && !(c & 8)) { /* VDUP (scalar) */
        unsigned imm4 = BITS(w, 19, 16);
        i->sub = N_VDUP_S;
        if (imm4 & 1) i->esize = 0, i->imm = imm4 >> 1;
        else if (imm4 & 2) i->esize = 1, i->imm = imm4 >> 2;
        else if (imm4 & 4) i->esize = 2, i->imm = imm4 >> 3;
        else { undef(i); return; }
        qcheck(i, 1, 0, 0);
        return;
    }
    undef(i);
}

/* Element and structure load/store. N_VLDST_MULTI: imm = n (VLDn), nregs = "regs" of the ARM pseudocode (VLD1:
 * 1-4, VLD2: 1-2, VLD3/4: 1), inc = register spacing, imm2 = align field. N_VLDST_LANE: imm = n, imm2 = lane,
 * reglist = index_align (alignment), inc. N_VLD_ALL: imm = n, nregs = registers per element (VLD1: 1-2), inc,
 * imm2 = alignment bit a. */
static void neon_ldst(uint32_t w, insn_t *i)
{
    unsigned a = BIT(w, 23), l = BIT(w, 21), b = BITS(w, 11, 8), size = BITS(w, 7, 6), align = BITS(w, 5, 4);
    i->op = OP_NEON;
    i->rn = BITS(w, 19, 16);
    i->rm = BITS(w, 3, 0);
    i->vd = VD_D(w);
    if (l) i->flags |= VF_LOAD;
    if (i->rn == 15) i->unpred = 1;
    if (!a) {
        static const signed char type[16] = {4, 4, 1, 2, 3, 3, 1, 1, 2, 2, 1, -1, -1, -1, -1, -1};
        static const signed char inc[16] = {1, 2, 1, 2, 1, 2, 1, 1, 1, 2, 1, -1, -1, -1, -1, -1};
        static const signed char regs[16] = {1, 1, 4, 2, 1, 1, 3, 1, 1, 1, 2, -1, -1, -1, -1, -1};
        if (type[b] < 0) { undef(i); return; }
        i->sub = N_VLDST_MULTI;
        i->imm = type[b];
        i->inc = inc[b];
        i->nregs = regs[b];
        i->esize = size;
        i->imm2 = align;
        unsigned last;
        switch (type[b]) {
        case 1:
            if ((i->nregs == 1 && (align & 2)) || (i->nregs == 3 && (align & 2)) || (i->nregs == 2 && align == 3)) {
                undef(i);
                return;
            }
            last = i->vd + i->nregs - 1;
            break;
        case 2:
            if (size == 3 || (i->nregs == 1 && align == 3)) { undef(i); return; }
            last = i->vd + i->inc + i->nregs - 1;
            break;
        case 3:
            if (size == 3 || (align & 2)) { undef(i); return; }
            last = i->vd + 2 * i->inc;
            break;
        default:
            if (size == 3) { undef(i); return; }
            last = i->vd + 3 * i->inc;
            break;
        }
        if (last > 31) i->unpred = 1;
        return;
    }
    unsigned n = (b & 3) + 1;
    i->imm = n;
    if ((b & 0xc) == 0xc) { /* single element to all lanes */
        unsigned t = BIT(w, 5), ab = BIT(w, 4);
        if (!l) { undef(i); return; }
        i->sub = N_VLD_ALL;
        i->esize = size;
        i->imm2 = ab;
        i->nregs = 1;
        i->inc = t ? 2 : 1;
        switch (n) {
        case 1:
            if (size == 3 || (size == 0 && ab)) { undef(i); return; }
            i->nregs = t ? 2 : 1;
            i->inc = 1;
            break;
        case 2:
            if (size == 3) { undef(i); return; }
            break;
        case 3:
            if (size == 3 || ab) { undef(i); return; }
            break;
        default:
            if (size == 3 && !ab) { undef(i); return; }
            if (size == 3) i->esize = 2;
            break;
        }
        unsigned last = n == 1 ? i->vd + i->nregs - 1 : i->vd + (n - 1) * i->inc;
        if (last > 31) i->unpred = 1;
        return;
    }
    unsigned sz = BITS(w, 11, 10), ia = BITS(w, 7, 4);
    i->sub = N_VLDST_LANE;
    i->esize = sz;
    i->nregs = 1;
    i->inc = 1;
    i->reglist = ia;
    switch (sz) {
    case 0:
        i->imm2 = ia >> 1;
        if ((n == 1 || n == 3) && (ia & 1)) { undef(i); return; }
        break;
    case 1:
        i->imm2 = ia >> 2;
        if (n == 1 && (ia & 2)) { undef(i); return; }
        if (n == 3 && (ia & 1)) { undef(i); return; }
        if (n > 1 && (ia & 2)) i->inc = 2;
        break;
    case 2:
        i->imm2 = ia >> 3;
        if (n == 1 && ((ia & 4) || ((ia & 3) != 0 && (ia & 3) != 3))) { undef(i); return; }
        if (n == 2 && (ia & 2)) { undef(i); return; }
        if (n == 3 && (ia & 3)) { undef(i); return; }
        if (n == 4 && (ia & 3) == 3) { undef(i); return; }
        if (n > 1 && (ia & 4)) i->inc = 2;
        break;
    default:
        undef(i);
        return;
    }
    if (i->vd + (n - 1) * i->inc > 31) i->unpred = 1;
}

void vfp_neon_decode(uint32_t w, insn_t *i)
{
    i->raw = w;
    if ((w & 0xfe000000u) == 0xf2000000u) {
        neon_dp(w, i);
        return;
    }
    if ((w & 0xff100000u) == 0xf4000000u) {
        neon_ldst(w, i);
        return;
    }
    if (w >> 28 == 0xf) {
        vfp_uncond(w, i);
        return;
    }
    unsigned top = BITS(w, 27, 24);
    if (top == 0xe) {
        if (!BIT(w, 4)) vfp_dp(w, i);
        else vfp_transfer(w, i);
        return;
    }
    if ((top & 0xe) == 0xc) {
        vfp_ldst(w, i);
        return;
    }
    undef(i);
}
