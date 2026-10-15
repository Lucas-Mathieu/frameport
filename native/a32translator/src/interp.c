/* Integer instruction semantics (ARMv7-A ARM pseudocode). */
#include <stdint.h>
#include "interp.h"

static inline uint32_t rreg(const a32_cpu *c, const insn_t *i, unsigned n)
{
    return n == 15 ? i->pcval : c->r[n];
}

static inline unsigned carry(const a32_cpu *c) { return (c->nzcv >> 29) & 1; }

static void set_nz(a32_cpu *c, uint32_t r)
{
    c->nzcv = (c->nzcv & 0x30000000u) | (r & CPSR_N) | (r ? 0 : CPSR_Z);
}

static void set_nzc(a32_cpu *c, uint32_t r, unsigned cy)
{
    c->nzcv = (c->nzcv & CPSR_V) | (r & CPSR_N) | (r ? 0 : CPSR_Z) | (cy << 29);
}

static uint32_t add_with_carry(a32_cpu *c, uint32_t x, uint32_t y, unsigned cin, int setflags)
{
    uint64_t us = (uint64_t)x + y + cin;
    int64_t ss = (int64_t)(int32_t)x + (int32_t)y + cin;
    uint32_t r = (uint32_t)us;
    if (setflags) {
        c->nzcv = (r & CPSR_N) | (r ? 0 : CPSR_Z) | ((uint32_t)(us >> 32) << 29) |
                  ((int64_t)(int32_t)r != ss ? CPSR_V : 0);
    }
    return r;
}

static void branch_write(a32_cpu *c, uint32_t a)
{
    c->r[15] = c->thumb ? a & ~1u : a & ~3u;
}

static void bx_write(a32_cpu *c, uint32_t a)
{
    c->thumb = a & 1;
    c->r[15] = a & ~1u;
}

static void alu_write_pc(a32_cpu *c, uint32_t a)
{
    if (c->thumb) branch_write(c, a);
    else bx_write(c, a);
}

/* Shift with carry out; amount already decoded for immediate shifts (LSR/ASR 1..32, RRX). */
static uint32_t shift_c(uint32_t v, unsigned type, unsigned amt, unsigned cin, unsigned *cout)
{
    *cout = cin;
    if (type == SH_RRX) {
        *cout = v & 1;
        return (cin << 31) | (v >> 1);
    }
    if (amt == 0) return v;
    switch (type) {
    case SH_LSL:
        if (amt < 32) { *cout = (v >> (32 - amt)) & 1; return v << amt; }
        *cout = amt == 32 ? (v & 1) : 0;
        return 0;
    case SH_LSR:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return v >> amt; }
        *cout = amt == 32 ? v >> 31 : 0;
        return 0;
    case SH_ASR:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *cout = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default: { /* ROR */
        unsigned a = amt & 31;
        uint32_t r = a ? ror32(v, a) : v;
        *cout = r >> 31;
        return r;
    }
    }
}

/* Second operand of data processing with its shifter carry out. */
static uint32_t operand2(const a32_cpu *c, const insn_t *i, unsigned *cout)
{
    if (i->form == F_IMM) {
        *cout = i->immcarry ? i->imm >> 31 : carry(c);
        return i->imm;
    }
    uint32_t m = rreg(c, i, i->rm);
    if (i->form == F_REG) return shift_c(m, i->shtype, i->shimm, carry(c), cout);
    return shift_c(m, i->shtype, rreg(c, i, i->rs) & 0xff, carry(c), cout);
}

static int exec_dp(a32_cpu *c, const insn_t *i)
{
    unsigned cy;
    uint32_t op2 = operand2(c, i, &cy), n = rreg(c, i, i->rn), r;
    int s = i->s, logical = 0, write = 1;
    switch (i->sub) {
    case A_AND: r = n & op2; logical = 1; break;
    case A_EOR: r = n ^ op2; logical = 1; break;
    case A_SUB: r = add_with_carry(c, n, ~op2, 1, s); break;
    case A_RSB: r = add_with_carry(c, ~n, op2, 1, s); break;
    case A_ADD: r = add_with_carry(c, n, op2, 0, s); break;
    case A_ADC: r = add_with_carry(c, n, op2, carry(c), s); break;
    case A_SBC: r = add_with_carry(c, n, ~op2, carry(c), s); break;
    case A_RSC: r = add_with_carry(c, ~n, op2, carry(c), s); break;
    case A_TST: r = n & op2; logical = 1; write = 0; break;
    case A_TEQ: r = n ^ op2; logical = 1; write = 0; break;
    case A_CMP: r = add_with_carry(c, n, ~op2, 1, 1); write = 0; break;
    case A_CMN: r = add_with_carry(c, n, op2, 0, 1); write = 0; break;
    case A_ORR: r = n | op2; logical = 1; break;
    case A_MOV: r = op2; logical = 1; break;
    case A_BIC: r = n & ~op2; logical = 1; break;
    case A_MVN: r = ~op2; logical = 1; break;
    default: r = n | ~op2; logical = 1; break; /* ORN */
    }
    if (logical && s) set_nzc(c, r, cy);
    if (write) {
        if (i->rd == 15) {
            alu_write_pc(c, r);
            return 1;
        }
        c->r[i->rd] = r;
    }
    return 0;
}

static int32_t sat_s(int64_t v, unsigned bits, int *sat)
{
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (v > hi) { *sat = 1; return (int32_t)hi; }
    if (v < lo) { *sat = 1; return (int32_t)lo; }
    return (int32_t)v;
}

static uint32_t sat_u(int64_t v, unsigned bits, int *sat)
{
    int64_t hi = bits >= 32 ? 0xffffffffll : (((int64_t)1 << bits) - 1);
    if (v > hi) { *sat = 1; return (uint32_t)hi; }
    if (v < 0) { *sat = 1; return 0; }
    return (uint32_t)v;
}

static void exec_mul(a32_cpu *c, const insn_t *i)
{
    uint32_t n = c->r[i->rn], m = c->r[i->rm];
    switch (i->sub) {
    case M_MUL: case M_MLA: case M_MLS: {
        uint32_t r = n * m;
        if (i->sub == M_MLA) r += c->r[i->ra];
        if (i->sub == M_MLS) r = c->r[i->ra] - r;
        c->r[i->rd] = r;
        if (i->s) set_nz(c, r);
        return;
    }
    default: {
        uint64_t r;
        switch (i->sub) {
        case M_UMULL: r = (uint64_t)n * m; break;
        case M_SMULL: r = (uint64_t)((int64_t)(int32_t)n * (int32_t)m); break;
        case M_UMLAL: r = (uint64_t)n * m + ((uint64_t)c->r[i->rd] << 32 | c->r[i->ra]); break;
        case M_SMLAL: r = (uint64_t)((int64_t)(int32_t)n * (int32_t)m) + ((uint64_t)c->r[i->rd] << 32 | c->r[i->ra]); break;
        default: r = (uint64_t)n * m + c->r[i->rd] + c->r[i->ra]; break; /* UMAAL */
        }
        c->r[i->ra] = (uint32_t)r;
        c->r[i->rd] = (uint32_t)(r >> 32);
        if (i->s) {
            c->nzcv = (c->nzcv & 0x30000000u) | ((uint32_t)(r >> 32) & CPSR_N) | (r ? 0 : CPSR_Z);
        }
    }
    }
}

static inline int32_t half(uint32_t v, int hi) { return hi ? (int16_t)(v >> 16) : (int16_t)v; }

static void exec_mulxy(a32_cpu *c, const insn_t *i)
{
    uint32_t n = c->r[i->rn], m = c->r[i->rm];
    int32_t a16 = half(n, i->flags & MF_NHI), b16 = half(m, i->flags & MF_MHI);
    int sat = 0;
    switch (i->sub) {
    case MX_SMUL:
        c->r[i->rd] = (uint32_t)(a16 * b16);
        break;
    case MX_SMLA: {
        int64_t r = (int64_t)(a16 * b16) + (int32_t)c->r[i->ra];
        c->r[i->rd] = (uint32_t)r;
        if (r != (int32_t)r) c->q = 1;
        break;
    }
    case MX_SMULW:
        c->r[i->rd] = (uint32_t)(((int64_t)(int32_t)n * b16) >> 16);
        break;
    case MX_SMLAW: {
        int64_t r = (((int64_t)(int32_t)n * b16) >> 16) + (int32_t)c->r[i->ra];
        c->r[i->rd] = (uint32_t)r;
        if (r != (int32_t)r) c->q = 1;
        break;
    }
    default: { /* SMLAL<x><y>: rd = RdHi, ra = RdLo */
        uint64_t acc = (uint64_t)c->r[i->rd] << 32 | c->r[i->ra];
        acc += (uint64_t)(int64_t)(a16 * b16);
        c->r[i->ra] = (uint32_t)acc;
        c->r[i->rd] = (uint32_t)(acc >> 32);
        break;
    }
    }
    (void)sat;
}

static void exec_dualmul(a32_cpu *c, const insn_t *i)
{
    uint32_t n = c->r[i->rn], m = c->r[i->rm];
    if (i->flags & MF_SWAP) m = ror32(m, 16);
    int64_t p1 = (int64_t)(int16_t)n * (int16_t)m, p2 = (int64_t)(int16_t)(n >> 16) * (int16_t)(m >> 16);
    int sub = i->sub == DM_SMLSD || i->sub == DM_SMLSLD;
    int64_t r = sub ? p1 - p2 : p1 + p2;
    if (i->sub == DM_SMLAD || i->sub == DM_SMLSD) {
        if (i->ra != 15) r += (int32_t)c->r[i->ra];
        c->r[i->rd] = (uint32_t)r;
        if (r != (int32_t)r) c->q = 1;
    } else {
        uint64_t acc = ((uint64_t)c->r[i->rd] << 32 | c->r[i->ra]) + (uint64_t)r;
        c->r[i->ra] = (uint32_t)acc;
        c->r[i->rd] = (uint32_t)(acc >> 32);
    }
}

static void exec_smmul(a32_cpu *c, const insn_t *i)
{
    int64_t p = (int64_t)(int32_t)c->r[i->rn] * (int32_t)c->r[i->rm];
    int64_t r;
    uint64_t a = (uint64_t)c->r[i->ra] << 32;
    if (i->sub == 0) r = p;
    else if (i->sub == 1) r = (int64_t)(a + (uint64_t)p);
    else r = (int64_t)(a - (uint64_t)p);
    if (i->flags & MF_ROUND) r = (int64_t)((uint64_t)r + 0x80000000u);
    c->r[i->rd] = (uint32_t)((uint64_t)r >> 32);
}

static uint32_t extend(const insn_t *i, uint32_t m, uint32_t n, int add)
{
    uint32_t v = ror32(m, i->shimm);
    switch (i->sub) {
    case E_SXTB: return (add ? n : 0) + (uint32_t)(int8_t)v;
    case E_SXTH: return (add ? n : 0) + (uint32_t)(int16_t)v;
    case E_UXTB: return (add ? n : 0) + (v & 0xff);
    case E_UXTH: return (add ? n : 0) + (v & 0xffff);
    case E_SXTB16: {
        uint32_t lo = (uint32_t)(int8_t)v, hi = (uint32_t)(int8_t)(v >> 16);
        if (add) lo += n, hi += n >> 16;
        return (lo & 0xffff) | hi << 16;
    }
    default: {
        uint32_t lo = v & 0xff, hi = (v >> 16) & 0xff;
        if (add) lo += n, hi += n >> 16;
        return (lo & 0xffff) | hi << 16;
    }
    }
}

static uint32_t parallel(a32_cpu *c, unsigned sub, uint32_t n, uint32_t m)
{
    unsigned pre = sub >> 4, op = sub & 15;
    int bits = (op == PO_ADD8 || op == PO_SUB8) ? 8 : 16, lanes = 32 / bits;
    int sgn = pre == PP_S || pre == PP_Q || pre == PP_SH;
    uint32_t r = 0, ge = 0, mask = (1u << bits) - 1;
    for (int k = 0; k < lanes; k++) {
        int64_t a, b, v;
        /* ASX: lane 0 = n.lo - m.hi, lane 1 = n.hi + m.lo; SAX: lane 0 = n.lo + m.hi, lane 1 = n.hi - m.lo */
        int mk = k, add;
        switch (op) {
        case PO_ADD16: case PO_ADD8: add = 1; break;
        case PO_SUB16: case PO_SUB8: add = 0; break;
        case PO_ASX: mk = 1 - k; add = k == 1; break;
        default: mk = 1 - k; add = k == 0; break;
        }
        uint32_t ua = (n >> (k * bits)) & mask, ub = (m >> (mk * bits)) & mask;
        if (sgn) {
            a = bits == 8 ? (int8_t)ua : (int16_t)ua;
            b = bits == 8 ? (int8_t)ub : (int16_t)ub;
        } else {
            a = ua;
            b = ub;
        }
        v = add ? a + b : a - b;
        int satd = 0;
        int64_t out;
        switch (pre) {
        case PP_S: out = v; if (v >= 0) ge |= ((1u << (bits / 8)) - 1) << (k * bits / 8); break;
        case PP_U:
            out = v;
            if (add ? v >= (1ll << bits) : v >= 0) ge |= ((1u << (bits / 8)) - 1) << (k * bits / 8);
            break;
        case PP_Q: out = sat_s(v, bits, &satd); break;
        case PP_UQ: out = sat_u(v, bits, &satd); break;
        default: out = v >> 1; break; /* SH / UH: halving (arithmetic shift of the exact result) */
        }
        r |= ((uint32_t)out & mask) << (k * bits);
    }
    if (pre == PP_S || pre == PP_U) c->ge = ge;
    return r;
}

static int ldst(a32_cpu *c, const insn_t *i)
{
    uint32_t base = rreg(c, i, i->rn), off;
    if (i->form == F_IMM) {
        off = i->imm;
    } else {
        unsigned cy;
        off = shift_c(rreg(c, i, i->rm), i->shtype, i->shimm, carry(c), &cy);
    }
    uint32_t oaddr = i->u ? base + off : base - off;
    uint32_t addr = i->p ? oaddr : base;
    if (i->flags & MF_LOAD) {
        uint32_t v, v2 = 0;
        switch (i->size) {
        case 1: v = i->sign ? (uint32_t)(int8_t)ld8(addr) : ld8(addr); break;
        case 2: v = i->sign ? (uint32_t)(int16_t)ld16(addr) : ld16(addr); break;
        case 4: v = ld32(addr); break;
        default: v = ld32(addr); v2 = ld32(addr + 4); break;
        }
        if (i->w) c->r[i->rn] = oaddr;
        if (i->size == 8) {
            c->r[i->rd] = v;
            c->r[i->ra] = v2;
        } else if (i->rd == 15) {
            bx_write(c, v);
            return 1;
        } else {
            c->r[i->rd] = v;
        }
    } else {
        uint32_t v = rreg(c, i, i->rd);
        switch (i->size) {
        case 1: st8(addr, (uint8_t)v); break;
        case 2: st16(addr, (uint16_t)v); break;
        case 4: st32(addr, v); break;
        default: st32(addr, v); st32(addr + 4, rreg(c, i, i->ra)); break;
        }
        if (i->w) c->r[i->rn] = oaddr;
    }
    return 0;
}

static int ldm_stm(a32_cpu *c, const insn_t *i)
{
    uint32_t base = c->r[i->rn], n = __builtin_popcount(i->reglist);
    uint32_t start, wb;
    if (i->u) {
        start = i->p ? base + 4 : base;
        wb = base + 4 * n;
    } else {
        start = i->p ? base - 4 * n : base - 4 * n + 4;
        wb = base - 4 * n;
    }
    uint32_t a = start;
    if (i->op == OP_STM) {
        for (int r = 0; r < 16; r++) {
            if (!(i->reglist & (1u << r))) continue;
            st32(a, r == 15 ? i->pcval : c->r[r]);
            a += 4;
        }
        if (i->w) c->r[i->rn] = wb;
        return 0;
    }
    uint32_t vals[16];
    for (int r = 0; r < 16; r++) {
        if (!(i->reglist & (1u << r))) continue;
        vals[r] = ld32(a);
        a += 4;
    }
    if (i->w) c->r[i->rn] = wb;
    for (int r = 0; r < 15; r++)
        if (i->reglist & (1u << r)) c->r[r] = vals[r];
    if (i->reglist & 0x8000) {
        bx_write(c, vals[15]);
        return 1;
    }
    return 0;
}

static uint32_t load_sized(uint32_t a, unsigned size, int order)
{
    switch (size) {
    case 1: return __atomic_load_n((uint8_t *)g2h(a), order);
    case 2: return __atomic_load_n((uint16_t *)g2h(a), order);
    default: return __atomic_load_n((uint32_t *)g2h(a), order);
    }
}

static void store_sized(uint32_t a, unsigned size, uint32_t v, int order)
{
    switch (size) {
    case 1: __atomic_store_n((uint8_t *)g2h(a), (uint8_t)v, order); break;
    case 2: __atomic_store_n((uint16_t *)g2h(a), (uint16_t)v, order); break;
    default: __atomic_store_n((uint32_t *)g2h(a), v, order); break;
    }
}

static void exec_ldrex(a32_cpu *c, const insn_t *i)
{
    uint32_t a = c->r[i->rn] + i->imm;
    int order = (i->flags & MF_ACQREL) ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED;
    if (i->size == 8) {
        uint64_t v = __atomic_load_n((uint64_t *)g2h(a), order);
        c->r[i->rd] = (uint32_t)v;
        c->r[i->ra] = (uint32_t)(v >> 32);
        c->excl_val = v;
    } else {
        uint32_t v = load_sized(a, i->size, order);
        c->r[i->rd] = v;
        c->excl_val = v;
    }
    c->excl_addr = a;
    c->excl_valid = i->size;
}

static void exec_strex(a32_cpu *c, const insn_t *i)
{
    uint32_t a = c->r[i->rn] + i->imm;
    int ok = 0;
    int order = (i->flags & MF_ACQREL) ? __ATOMIC_SEQ_CST : __ATOMIC_SEQ_CST;
    if (c->excl_valid == i->size && c->excl_addr == a) {
        switch (i->size) {
        case 1: {
            uint8_t e = (uint8_t)c->excl_val;
            ok = __atomic_compare_exchange_n((uint8_t *)g2h(a), &e, (uint8_t)c->r[i->rm], 0, order, __ATOMIC_RELAXED);
            break;
        }
        case 2: {
            uint16_t e = (uint16_t)c->excl_val;
            ok = __atomic_compare_exchange_n((uint16_t *)g2h(a), &e, (uint16_t)c->r[i->rm], 0, order, __ATOMIC_RELAXED);
            break;
        }
        case 4: {
            uint32_t e = (uint32_t)c->excl_val;
            ok = __atomic_compare_exchange_n((uint32_t *)g2h(a), &e, c->r[i->rm], 0, order, __ATOMIC_RELAXED);
            break;
        }
        default: {
            uint64_t e = c->excl_val, v = (uint64_t)c->r[i->ra] << 32 | c->r[i->rm];
            ok = __atomic_compare_exchange_n((uint64_t *)g2h(a), &e, v, 0, order, __ATOMIC_RELAXED);
            break;
        }
        }
    }
    c->excl_valid = 0;
    c->r[i->rd] = !ok;
}

static uint32_t crc32_update(uint32_t crc, uint32_t v, unsigned bytes, int castagnoli)
{
    uint32_t poly = castagnoli ? 0x82f63b78u : 0xedb88320u;
    for (unsigned b = 0; b < bytes; b++) {
        crc ^= (v >> (8 * b)) & 0xff;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
    }
    return crc;
}

static inline uint64_t read_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static int exec_body(a32_cpu *c, const insn_t *i)
{
    uint32_t next = i->pc + i->len;
    switch (i->op) {
    case OP_NOP:
    case OP_IT:
        break;
    case OP_DP:
        if (exec_dp(c, i)) return EXIT_NONE;
        break;
    case OP_MOVW:
        c->r[i->rd] = i->imm;
        break;
    case OP_MOVT:
        c->r[i->rd] = (c->r[i->rd] & 0xffff) | i->imm << 16;
        break;
    case OP_MUL:
        exec_mul(c, i);
        break;
    case OP_MULXY:
        exec_mulxy(c, i);
        break;
    case OP_DUALMUL:
        exec_dualmul(c, i);
        break;
    case OP_SMMUL:
        exec_smmul(c, i);
        break;
    case OP_USAD8: {
        uint32_t n = c->r[i->rn], m = c->r[i->rm], s = 0;
        for (int k = 0; k < 32; k += 8) {
            int d = (int)((n >> k) & 0xff) - (int)((m >> k) & 0xff);
            s += d < 0 ? -d : d;
        }
        if (i->ra != 15) s += c->r[i->ra];
        c->r[i->rd] = s;
        break;
    }
    case OP_DIV: {
        uint32_t n = c->r[i->rn], m = c->r[i->rm], r;
        if (m == 0) r = 0;
        else if (i->sign) r = (n == 0x80000000u && m == 0xffffffffu) ? n : (uint32_t)((int32_t)n / (int32_t)m);
        else r = n / m;
        c->r[i->rd] = r;
        break;
    }
    case OP_CLZ: {
        uint32_t m = c->r[i->rm];
        c->r[i->rd] = m ? __builtin_clz(m) : 32;
        break;
    }
    case OP_REV:
        c->r[i->rd] = __builtin_bswap32(c->r[i->rm]);
        break;
    case OP_REV16: {
        uint32_t m = c->r[i->rm];
        c->r[i->rd] = ((m & 0x00ff00ffu) << 8) | ((m >> 8) & 0x00ff00ffu);
        break;
    }
    case OP_REVSH: {
        uint32_t m = c->r[i->rm];
        c->r[i->rd] = (uint32_t)(int16_t)(((m & 0xff) << 8) | ((m >> 8) & 0xff));
        break;
    }
    case OP_RBIT: {
        uint32_t m = c->r[i->rm], r = 0;
        for (int k = 0; k < 32; k++) r |= ((m >> k) & 1) << (31 - k);
        c->r[i->rd] = r;
        break;
    }
    case OP_EXTEND:
        c->r[i->rd] = extend(i, c->r[i->rm], i->rn == 15 ? 0 : c->r[i->rn], i->rn != 15);
        break;
    case OP_PKH: {
        uint32_t n = c->r[i->rn], m = c->r[i->rm];
        if (i->sub == 0) {
            c->r[i->rd] = (n & 0xffff) | ((m << i->shimm) & 0xffff0000u);
        } else {
            uint32_t s = (uint32_t)((int32_t)m >> (i->shimm >= 32 ? 31 : i->shimm));
            c->r[i->rd] = (n & 0xffff0000u) | (s & 0xffff);
        }
        break;
    }
    case OP_BFX: {
        uint32_t n = c->r[i->rn];
        unsigned lsb = i->imm, width = i->shimm;
        if (lsb + width > 32) width = 32 - lsb;
        uint32_t v = n >> lsb;
        if (width < 32) {
            v &= (1u << width) - 1;
            if (i->sign && (v >> (width - 1)) & 1) v |= ~0u << width;
        }
        c->r[i->rd] = v;
        break;
    }
    case OP_BFI: {
        unsigned lsb = i->imm, msb = i->shimm;
        if (msb >= lsb) {
            uint32_t mask = (msb - lsb == 31) ? ~0u : (((1u << (msb - lsb + 1)) - 1) << lsb);
            uint32_t src = i->rn == 15 ? 0 : c->r[i->rn] << lsb;
            c->r[i->rd] = (c->r[i->rd] & ~mask) | (src & mask);
        }
        break;
    }
    case OP_SAT: {
        unsigned cy;
        uint32_t v = shift_c(c->r[i->rn], i->shtype, i->shimm, 0, &cy);
        int sat = 0;
        if (i->sign) c->r[i->rd] = (uint32_t)sat_s((int32_t)v, i->imm, &sat);
        else c->r[i->rd] = sat_u((int32_t)v, i->imm, &sat);
        if (sat) c->q = 1;
        break;
    }
    case OP_SAT16: {
        uint32_t n = c->r[i->rn];
        int sat = 0;
        uint32_t lo, hi;
        if (i->sign) {
            lo = (uint32_t)sat_s((int16_t)n, i->imm, &sat);
            hi = (uint32_t)sat_s((int16_t)(n >> 16), i->imm, &sat);
        } else {
            lo = sat_u((int16_t)n, i->imm, &sat);
            hi = sat_u((int16_t)(n >> 16), i->imm, &sat);
        }
        c->r[i->rd] = (lo & 0xffff) | hi << 16;
        if (sat) c->q = 1;
        break;
    }
    case OP_QADDSUB: {
        int sat = 0;
        int64_t m = (int32_t)c->r[i->rm], n = (int32_t)c->r[i->rn];
        if (i->sub >= 2) n = sat_s(2 * n, 32, &sat);
        int64_t r = (i->sub & 1) ? m - n : m + n;
        c->r[i->rd] = (uint32_t)sat_s(r, 32, &sat);
        if (sat) c->q = 1;
        break;
    }
    case OP_PARALLEL:
        c->r[i->rd] = parallel(c, i->sub, c->r[i->rn], c->r[i->rm]);
        break;
    case OP_SEL: {
        uint32_t n = c->r[i->rn], m = c->r[i->rm], r = 0;
        for (int k = 0; k < 4; k++) r |= ((c->ge >> k) & 1 ? n : m) & (0xffu << (8 * k));
        c->r[i->rd] = r;
        break;
    }
    case OP_LDST:
        if (ldst(c, i)) return EXIT_NONE;
        break;
    case OP_LDM:
    case OP_STM:
        if (ldm_stm(c, i)) return EXIT_NONE;
        break;
    case OP_LDREX:
        exec_ldrex(c, i);
        break;
    case OP_STREX:
        exec_strex(c, i);
        break;
    case OP_LDA:
        c->r[i->rd] = load_sized(c->r[i->rn], i->size, __ATOMIC_ACQUIRE);
        break;
    case OP_STL:
        store_sized(c->r[i->rn], i->size, c->r[i->rm], __ATOMIC_RELEASE);
        break;
    case OP_SWP: {
        uint32_t a = c->r[i->rn], v = c->r[i->rm];
        if (i->size == 1) c->r[i->rd] = __atomic_exchange_n((uint8_t *)g2h(a), (uint8_t)v, __ATOMIC_SEQ_CST);
        else c->r[i->rd] = __atomic_exchange_n((uint32_t *)g2h(a), v, __ATOMIC_SEQ_CST);
        break;
    }
    case OP_B:
        c->r[15] = i->imm;
        return EXIT_NONE;
    case OP_BL:
        c->r[14] = c->thumb ? next | 1 : next;
        c->r[15] = i->imm;
        return EXIT_NONE;
    case OP_BLX_IMM:
        c->r[14] = c->thumb ? next | 1 : next;
        c->thumb ^= 1;
        c->r[15] = i->imm;
        return EXIT_NONE;
    case OP_BX:
        bx_write(c, rreg(c, i, i->rm));
        return EXIT_NONE;
    case OP_BLX_REG: {
        uint32_t t = rreg(c, i, i->rm);
        c->r[14] = c->thumb ? next | 1 : next;
        bx_write(c, t);
        return EXIT_NONE;
    }
    case OP_CBZ:
        if ((c->r[i->rn] != 0) == !!(i->flags & MF_NONZERO)) {
            c->r[15] = i->imm;
            return EXIT_NONE;
        }
        break;
    case OP_TB: {
        uint32_t base = rreg(c, i, i->rn), idx = c->r[i->rm];
        uint32_t off = i->size == 1 ? ld8(base + idx) : ld16(base + 2 * idx);
        c->r[15] = i->pcval + 2 * off;
        return EXIT_NONE;
    }
    case OP_SVC:
        c->svc_imm = i->imm;
        c->r[15] = next;
        return EXIT_SVC;
    case OP_BKPT:
        c->r[15] = i->pc;
        return EXIT_BKPT;
    case OP_MRS:
        c->r[i->rd] = cpu_apsr(c) | 0x10;
        break;
    case OP_MSR: {
        uint32_t v = i->form == F_IMM ? i->reglist : c->r[i->rn];
        if (i->imm & 8) {
            c->nzcv = v & 0xf0000000u;
            c->q = (v >> 27) & 1;
        }
        if (i->imm & 4) c->ge = (v >> 16) & 0xf;
        break;
    }
    case OP_MRC:
        switch (i->imm) {
        case CP15_TPIDRURO: c->r[i->rd] = c->tpidruro; break;
        case CP15_TPIDRURW: c->r[i->rd] = c->tpidrurw; break;
        default: c->r[i->rd] = (uint32_t)read_cntfrq(); break;
        }
        break;
    case OP_MCR:
        if (i->imm == CP15_TPIDRURW) c->tpidrurw = c->r[i->rd];
        break;
    case OP_MRRC: {
        uint64_t v = read_cntvct();
        c->r[i->rd] = (uint32_t)v;
        c->r[i->ra] = (uint32_t)(v >> 32);
        break;
    }
    case OP_BARRIER:
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        break;
    case OP_CLREX:
        c->excl_valid = 0;
        break;
    case OP_SETEND:
        if (i->imm) {
            c->r[15] = i->pc;
            return EXIT_UNDEF; /* big-endian data accesses are not supported */
        }
        break;
    case OP_CRC32: {
        uint32_t v = c->r[i->rm];
        c->r[i->rd] = crc32_update(c->r[i->rn], v, i->size, i->flags & MF_CRC32C);
        break;
    }
    case OP_VFP: {
        int r = a32_exec_vfp(c, i);
        if (r) {
            c->r[15] = i->pc;
            return r;
        }
        break;
    }
    case OP_NEON: {
        int r = a32_exec_neon(c, i);
        if (r) {
            c->r[15] = i->pc;
            return r;
        }
        break;
    }
    default:
        c->r[15] = i->pc;
        return EXIT_UNDEF;
    }
    c->r[15] = next;
    return EXIT_NONE;
}

int a32_exec(a32_cpu *c, const insn_t *i)
{
    uint32_t it = c->itstate;
    int r;
    if (i->op == OP_UNDEF && (i->cond == C_AL || cond_pass(c->nzcv, i->cond))) {
        /* an UNDEFINED instruction in an IT block that fails its condition is skipped (IMPLEMENTATION DEFINED;
         * qemu does the same) */
        c->r[15] = i->pc;
        return EXIT_UNDEF;
    }
    if (i->cond != C_AL && !cond_pass(c->nzcv, i->cond)) {
        c->r[15] = i->pc + i->len;
        r = EXIT_NONE;
    } else {
        r = exec_body(c, i);
    }
    if (i->thumb) {
        if (i->op == OP_IT && (i->cond == C_AL || r == EXIT_NONE)) c->itstate = i->imm;
        else if (it) c->itstate = (r == EXIT_UNDEF || r == EXIT_BKPT) ? it : itstate_advance(it);
    }
    return r;
}

int a32_decode_at(const a32_cpu *c, uint32_t pc, insn_t *i)
{
    if (!c->thumb) return a32_decode(ld32(pc), pc, i);
    uint16_t hw[2];
    hw[0] = ld16(pc);
    if (t32_insn_len(hw[0]) == 4) hw[1] = ld16(pc + 2);
    int in_it = (c->itstate & 0xf) != 0;
    int itcond = (c->itstate >> 4) | ((c->itstate & 0xf) == 8 ? 0x100 : 0);
    return t32_decode(hw, pc, in_it, itcond, i);
}
