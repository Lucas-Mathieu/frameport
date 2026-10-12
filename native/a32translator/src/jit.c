/* AArch32 -> AArch64 dynamic translator: code cache, blocks, chaining, run loop, and the translation of integer
 * instructions. Instructions without a native translation call the interpreter (jx_call_helper), so every
 * instruction the interpreter knows works here too.
 *
 * A block is a run of guest instructions translated together; it ends at the first instruction that may write the
 * PC, an SVC/UDF, or after MAX_INSNS. Blocks are keyed by (pc, Thumb, ITSTATE). Direct exits are patchable "b"
 * instructions that get chained to their target block; indirect exits look the target up inline in a direct-mapped
 * table of blocks (ITSTATE 0 only) before falling back to the run loop. Every block starts with a check of
 * cpu->interrupt so that signals and code flushes can stop chained loops. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <asm/sigcontext.h>
#include <ucontext.h>
#include <unistd.h>
#include "interp.h"
#include "jit.h"
#include "runtime.h"
#include "signals.h"
#include "threads.h"

extern int jit_enter(a32_cpu *c, const void *code);
extern void jit_exit(void);

#define CACHE_BYTES (128u << 20)
#define CACHE_MARGIN (64u << 10)   /* free space needed to start a block (a block uses far less) */
#define HASH_BITS 18
#define LTAB_BITS 18
#define MAX_INSNS 96

static uint32_t *cache, *cache_cur, *cache_end;
static jit_block *htab[1u << HASH_BITS];
static jit_block *ltab[1u << LTAB_BITS];
static jit_block invalid_block = {.key = ~0ull};
static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;
static jit_block **blocks;          /* in code address order (the cache is append-only between flushes) */
static size_t nblocks, cap_blocks;
static uint8_t codepages[(1u << 20) / 8];
/* the blocks on each guest page (jit_invalidate), two levels: 2^10 tables of 2^10 list heads, allocated on use */
static jit_block **pagelists[1u << 10];
static void *helpers[HELP_COUNT];
static volatile int flush_pending;   /* a flush waits for every thread to leave (a32_thread.in_jit_section) */
int jit_stats;
static FILE *dump_file;
static FILE *perf_map; /* A32_PERFMAP: perf's map of translated code (body, exit stubs, FPCR writes) */
static unsigned long helper_hist[OP_COUNT];
static unsigned long st_blocks, st_helpers_emitted, st_flushes, st_chains;

/* insn_t copies referenced by helper calls in translated code (stable addresses) */
typedef struct arena { struct arena *next; size_t n; insn_t v[1024]; } arena_t;
static arena_t *arenas;

static const insn_t *keep_insn(const insn_t *i)
{
    if (!arenas || arenas->n == 1024) {
        arena_t *a = calloc(1, sizeof *a);
        a->next = arenas;
        arenas = a;
    }
    insn_t *p = &arenas->v[arenas->n++];
    *p = *i;
    return p;
}

/* ---------------------------------------------------------------- emit helpers */

void jx_mov32(cbuf *b, int rd, uint32_t v)
{
    if ((v & 0xffff0000u) == 0) {
        emit(b, a64_movw(0, 2, rd, v, 0));
    } else if ((v & 0xffffu) == 0) {
        emit(b, a64_movw(0, 2, rd, v >> 16, 1));
    } else if ((~v & 0xffff0000u) == 0) {
        emit(b, a64_movw(0, 0, rd, ~v & 0xffff, 0));
    } else if ((~v & 0xffffu) == 0) {
        emit(b, a64_movw(0, 0, rd, (~v >> 16) & 0xffff, 1));
    } else {
        int bm = a64_bitmask(v, 0);
        if (bm >= 0) {
            emit(b, a64_logic_imm(0, 1, rd, XZR, (uint32_t)bm));
        } else {
            emit(b, a64_movw(0, 2, rd, v & 0xffff, 0));
            emit(b, a64_movw(0, 3, rd, v >> 16, 1));
        }
    }
}

void jx_mov64(cbuf *b, int rd, uint64_t v)
{
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        uint32_t part = (v >> (16 * hw)) & 0xffff;
        if (!part && !(first && hw == 3)) continue;
        emit(b, a64_movw(1, first ? 2 : 3, rd, part, hw));
        first = 0;
    }
    if (first) emit(b, a64_movw(1, 2, rd, 0, 0));
}

int jx_src(jit_ctx *x, unsigned r, int tmp)
{
    if (r != 15) return (int)r;
    jx_mov32(&x->cb, tmp, x->i->pcval);
    return tmp;
}

uint32_t *jx_placeholder(cbuf *b)
{
    uint32_t *p = b->p;
    emit(b, A64_NOP);
    return p;
}

void jx_patch_bcond(uint32_t *site, int cond, uint32_t *target) { *site = a64_bcond(cond, (int32_t)(target - site)); }
void jx_patch_b(uint32_t *site, uint32_t *target) { *site = a64_b((int32_t)(target - site)); }

/* guest q0-q15 (v16-v31) <-> cpu->d: C helpers may clobber v16-v31 */
static void spill_v(cbuf *b)
{
    for (int q = 0; q < 16; q += 2) emit(b, a64_ldstp_q(0, 16 + q, 17 + q, R_CPU, (int)OFF_D(2 * q)));
}

static void reload_v(cbuf *b)
{
    for (int q = 0; q < 16; q += 2) emit(b, a64_ldstp_q(1, 16 + q, 17 + q, R_CPU, (int)OFF_D(2 * q)));
}

void jx_spill(cbuf *b)
{
    for (int r = 0; r < 14; r += 2) emit(b, a64_ldstp(0, 0, r, r + 1, R_CPU, 4 * r));
    emit(b, a64_ldst_imm(2, 0, 14, R_CPU, OFF_R(14)));
    emit(b, a64_mrs(T0, SYS_NZCV));
    emit(b, a64_ldst_imm(2, 0, T0, R_CPU, offsetof(a32_cpu, nzcv)));
}

void jx_reload(cbuf *b)
{
    for (int r = 0; r < 14; r += 2) emit(b, a64_ldstp(0, 1, r, r + 1, R_CPU, 4 * r));
    emit(b, a64_ldst_imm(2, 1, 14, R_CPU, OFF_R(14)));
    emit(b, a64_ldst_imm(2, 1, T0, R_CPU, offsetof(a32_cpu, nzcv)));
    emit(b, a64_msr(T0, SYS_NZCV));
}

static void store_const32(cbuf *b, uint32_t off, uint32_t v)
{
    if (v == 0) {
        emit(b, a64_ldst_imm(2, 0, XZR, R_CPU, off));
    } else {
        jx_mov32(b, T0, v);
        emit(b, a64_ldst_imm(2, 0, T0, R_CPU, off));
    }
}

/* FPCR switching. Convention: between blocks the host FPCR holds the guest's VFP control bits (jit_enter sets
 * them), so a block starts in FPCR_GUEST; a block that switched to the Advanced SIMD standard value switches back
 * before every exit. FPCR_UNKNOWN only arises where two code paths merge. Uses x15/x17. */
static void fpcr_switch(jit_ctx *x, int want)
{
    cbuf *b = &x->cb;
    if (x->fpcr_std == want) return;
    emit(b, a64_ldst_imm(2, 1, T2, R_CPU, OFF_FPCR));
    if (want == FPCR_STD) {
        emit(b, a64_logic_imm(0, 0, T2, T2, (uint32_t)a64_bitmask(FPCR_AHP, 0)));
        emit(b, a64_logic_imm(0, 1, T2, T2, (uint32_t)a64_bitmask(FPCR_DN | FPCR_FZ, 0)));
    }
    if (x->fpcr_std == FPCR_UNKNOWN) {
        emit(b, a64_mrs(T1, SYS_FPCR));
        emit(b, a64_logic_reg(1, 2, 0, T1, T1, T2, 0, 0)); /* eor x17, x17, x15 */
        emit(b, a64_cbz(1, 0, T1, 2));                    /* already set: skip the msr */
    }
    emit(b, a64_msr(T2, SYS_FPCR));
    x->fpcr_std = want;
}

void jx_fpcr_guest(jit_ctx *x) { fpcr_switch(x, FPCR_GUEST); }

/* exits: guest FPCR and merged VFP values on this path only; the state for the code that follows is unchanged */
static void exit_prologue(jit_ctx *x)
{
    int saved = x->fpcr_std;
    jit_vfp_emit_merges(x);
    fpcr_switch(x, FPCR_GUEST);
    x->fpcr_std = saved;
}
void jx_fpcr_std(jit_ctx *x) { fpcr_switch(x, FPCR_STD); }

int jit_exact_addr; /* compute guest addresses modulo 2^32 (A32_EXACT_ADDR=1) instead of host addressing modes */

static jit_stub *add_stub(jit_ctx *x, int kind, uint32_t *site)
{
    if (x->nstubs == MAX_STUBS) {
        x->cb.overflow = 1;
        return &x->stubs[0];
    }
    jit_stub *s = &x->stubs[x->nstubs++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->site = site;
    return s;
}

void jx_exit_direct(jit_ctx *x, uint32_t pc, int thumb, uint32_t it)
{
    exit_prologue(x);
    jit_stub *s = add_stub(x, STUB_DIRECT, jx_placeholder(&x->cb));
    s->pc = pc;
    s->thumb = thumb;
    s->it = it;
}

void jx_exit_reason(jit_ctx *x, uint32_t pc, int thumb, uint32_t it, uint32_t reason, uint32_t svc)
{
    exit_prologue(x);
    jit_stub *s = add_stub(x, STUB_REASON, jx_placeholder(&x->cb));
    s->pc = pc;
    s->thumb = thumb;
    s->it = it;
    s->reason = reason;
    s->svc = svc;
}

/* target in w16, bit 0 = Thumb. Inline lookup of the block (ITSTATE 0), else back to the run loop. */
void jx_exit_indirect_t0(jit_ctx *x)
{
    cbuf *b = &x->cb;
    exit_prologue(x);
    emit(b, a64_ldst_imm(3, 1, T1, R_CPU, OFF_LOOKUP));
    emit(b, a64_bfm(1, 2, T2, T0, 1, LTAB_BITS));                 /* ubfx x15, x16, #1, #LTAB_BITS */
    emit(b, a64_ldst_reg(3, 1, T1, T1, T2, 3, 1));                /* ldr x17, [x17, x15, lsl #3] */
    emit(b, a64_ldstp(1, 1, T2, T1, T1, 0));                      /* ldp x15, x17, [x17] (key, code) */
    emit(b, a64_logic_reg(1, 2, 0, T2, T2, T0, 0, 0));            /* eor x15, x15, x16 */
    jit_stub *s = add_stub(x, STUB_INDIRECT, jx_placeholder(b));  /* cbnz x15, miss */
    (void)s;
    emit(b, a64_br(T1));
}

static void call_helper_raw(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    jit_vfp_flush(x);  /* the interpreter reads cpu->d */
    jx_fpcr_guest(x); /* the helper may end in an exit */
    jx_spill(b);
    if (x->thumb) {
        store_const32(b, OFF_ITSTATE, x->it);
        store_const32(b, OFF_THUMB, 1);
    } else {
        store_const32(b, OFF_THUMB, 0);
    }
    emit(b, a64_addsub_imm(1, 0, 0, 0, R_CPU, 0, 0)); /* mov x0, x19 */
    jx_mov64(b, 1, (uint64_t)(uintptr_t)keep_insn(i));
    emit(b, a64_ldst_imm(3, 1, T0, R_CPU, OFF_HELPERS));
    emit(b, a64_ldst_imm(3, 1, T0, T0, 8 * HELP_EXEC));
    spill_v(b);
    emit(b, a64_blr(T0));
    emit(b, a64_logic_reg(0, 1, 0, T3, XZR, 0, 0, 0)); /* mov w20, w0 */
    jx_reload(b);
    reload_v(b);
    st_helpers_emitted++;
}

void jx_call_helper(jit_ctx *x, const insn_t *i)
{
    call_helper_raw(x, i);
    /* nonzero: exception (UNDEF, ...) with the pc already set by the interpreter */
    add_stub(x, STUB_ASIS, jx_placeholder(&x->cb))->reason = 1;
}

/* ---------------------------------------------------------------- flags */

/* N and Z from w(r) (or x(r) if sf), C from carry_reg (0/1 in a W register, -1 = unchanged, -2/-3 = const 0/1),
 * V unchanged. Uses x16/x17. */
static void set_nzc(cbuf *b, int sf, int r, int carry)
{
    emit(b, a64_mrs(T1, SYS_NZCV));
    emit(b, a64_logic_reg(sf, 3, 0, XZR, r, r, 0, 0)); /* tst r, r */
    emit(b, a64_mrs(T0, SYS_NZCV));
    uint32_t keep = carry == -1 ? 0x30000000u : 0x10000000u;
    emit(b, a64_logic_imm(0, 0, T1, T1, (uint32_t)a64_bitmask(keep, 0)));
    emit(b, a64_logic_reg(0, 1, 0, T0, T0, T1, 0, 0));
    if (carry == -3) emit(b, a64_logic_imm(0, 1, T0, T0, (uint32_t)a64_bitmask(0x20000000u, 0)));
    else if (carry >= 0) emit(b, a64_bfm(0, 1, T0, carry, 3, 0)); /* bfi w16, wC, #29, #1 */
    emit(b, a64_msr(T0, SYS_NZCV));
}

/* ---------------------------------------------------------------- data processing */

static int is_logical(unsigned a)
{
    return a == A_AND || a == A_EOR || a == A_TST || a == A_TEQ || a == A_ORR || a == A_MOV || a == A_BIC ||
           a == A_MVN || a == A_ORN;
}

static uint32_t const_shift(uint32_t v, unsigned type, unsigned amt, int *carry)
{
    /* carry: -1 unchanged, 0/1 */
    *carry = -1;
    switch (type) {
    case SH_LSL:
        if (!amt) return v;
        *carry = (v >> (32 - amt)) & 1;
        return v << amt;
    case SH_LSR:
        *carry = (v >> (amt - 1)) & 1;
        return amt == 32 ? 0 : v >> amt;
    case SH_ASR:
        *carry = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> (amt == 32 ? 31 : amt));
    case SH_ROR:
        *carry = (ror32(v, amt) >> 31) & 1;
        return ror32(v, amt);
    default:
        return v; /* RRX: handled by the caller */
    }
}

typedef struct {
    int is_imm;
    uint32_t imm;
    int reg, shift, amt;
} op2_t;

/* Clamped register-specified shift (no flags): result in dst. */
static void rsr_shift(cbuf *b, int dst, int m, int s, unsigned type)
{
    emit(b, a64_logic_imm(0, 0, T2, s, (uint32_t)a64_bitmask(0xff, 0)));   /* and w15, ws, #0xff */
    if (type == SH_ROR) {
        emit(b, a64_dp2(0, 11, dst, m, T2));
        return;
    }
    /* mask = amount < 32 ? ~0 : 0 */
    emit(b, a64_bfm(0, 2, T0, T2, 5, 31));    /* lsr w16, w15, #5 */
    emit(b, a64_dp1(0, 4, T0, T0));           /* clz w16, w16 */
    emit(b, a64_bfm(0, 2, T0, T0, 5, 31));    /* lsr w16, w16, #5 */
    emit(b, a64_addsub_reg(0, 1, 0, T0, XZR, T0, 0, 0)); /* neg w16, w16 */
    if (type == SH_ASR) {
        emit(b, a64_logic_reg(0, 0, 0, T2, T2, T0, 0, 0)); /* and w15, w15, w16 */
        emit(b, a64_logic_reg(0, 1, 1, T2, T2, T0, 0, 0)); /* orn w15, w15, w16 -> 31 (mod 32) if >= 32 */
        emit(b, a64_dp2(0, 10, dst, m, T2));
        return;
    }
    emit(b, a64_dp2(0, type == SH_LSL ? 8 : 9, dst, m, T2));
    emit(b, a64_logic_reg(0, 0, 0, dst, dst, T0, 0, 0));
}

/* Second operand. allow_ror: the consumer is a logical instruction (supports ROR shifts). Computed values go to T1. */
static void get_op2(jit_ctx *x, const insn_t *i, op2_t *o, int allow_ror)
{
    cbuf *b = &x->cb;
    memset(o, 0, sizeof *o);
    if (i->form == F_IMM) {
        o->is_imm = 1;
        o->imm = i->imm;
        return;
    }
    if (i->form == F_RSR) {
        rsr_shift(b, T1, i->rm, i->rs, i->shtype);
        o->reg = T1;
        return;
    }
    if (i->rm == 15 && i->shtype != SH_RRX) {
        int cy;
        o->is_imm = 1;
        o->imm = const_shift(i->pcval, i->shtype, i->shimm, &cy);
        return;
    }
    int m = jx_src(x, i->rm, T1);
    switch (i->shtype) {
    case SH_LSL:
        o->reg = m, o->shift = SHIFT_LSL, o->amt = i->shimm;
        return;
    case SH_LSR:
        if (i->shimm == 32) {
            o->is_imm = 1;
            o->imm = 0;
        } else {
            o->reg = m, o->shift = SHIFT_LSR, o->amt = i->shimm;
        }
        return;
    case SH_ASR:
        o->reg = m, o->shift = SHIFT_ASR, o->amt = i->shimm == 32 ? 31 : i->shimm;
        return;
    case SH_ROR:
        if (allow_ror) {
            o->reg = m, o->shift = SHIFT_ROR, o->amt = i->shimm;
        } else {
            emit(b, a64_extr(0, T1, m, m, i->shimm));
            o->reg = T1;
        }
        return;
    default: /* RRX */
        emit(b, a64_csel(0, 1, T1, XZR, XZR, A64_CC)); /* cset w17, cs */
        emit(b, a64_extr(0, T1, T1, m, 1));
        o->reg = T1;
        return;
    }
}

/* Shifter carry for flag-setting logical instructions: returns -1 (unchanged), -2/-3 (const 0/1) or a register
 * holding 0/1 (computed into T2 before the result can overwrite the source). */
static int op2_carry(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    if (i->form == F_IMM) return i->immcarry ? (i->imm >> 31 ? -3 : -2) : -1;
    if (i->rm == 15 && i->shtype != SH_RRX) {
        int cy;
        const_shift(i->pcval, i->shtype, i->shimm, &cy);
        return cy < 0 ? -1 : (cy ? -3 : -2);
    }
    int m = jx_src(x, i->rm, T2);
    unsigned amt = i->shimm;
    int bit;
    switch (i->shtype) {
    case SH_LSL:
        if (!amt) return -1;
        bit = 32 - amt;
        break;
    case SH_RRX:
        bit = 0;
        break;
    default:
        bit = amt - 1;
        break;
    }
    emit(b, a64_bfm(0, 2, T2, m, bit, bit)); /* ubfx w15, wm, #bit, #1 */
    return T2;
}

static int addsub_imm_ok(uint32_t v, uint32_t *imm12, int *sh)
{
    if (v < 4096) {
        *imm12 = v, *sh = 0;
        return 1;
    }
    if ((v & 0xfff) == 0 && v < 0x1000000) {
        *imm12 = v >> 12, *sh = 1;
        return 1;
    }
    return 0;
}

/* d = n OP o for add/sub-type ops; sub: subtract; s: set flags. Uses T1 for unencodable immediates. */
static void emit_addsub(cbuf *b, int sub, int s, int d, int n, const op2_t *o)
{
    if (o->is_imm) {
        uint32_t imm12;
        int sh;
        if (addsub_imm_ok(o->imm, &imm12, &sh)) {
            emit(b, a64_addsub_imm(0, sub, s, d, n, imm12, sh));
            return;
        }
        if (!s && o->imm && addsub_imm_ok(-o->imm, &imm12, &sh)) {
            emit(b, a64_addsub_imm(0, !sub, 0, d, n, imm12, sh));
            return;
        }
        jx_mov32(b, T1, o->imm);
        emit(b, a64_addsub_reg(0, sub, s, d, n, T1, 0, 0));
        return;
    }
    emit(b, a64_addsub_reg(0, sub, s, d, n, o->reg, o->shift, o->amt));
}

static int op2_to_reg(cbuf *b, const op2_t *o)
{
    if (o->is_imm) {
        jx_mov32(b, T1, o->imm);
        return T1;
    }
    if (o->amt == 0 && o->shift == SHIFT_LSL) return o->reg;
    /* shifted: materialize */
    switch (o->shift) {
    case SHIFT_LSL: emit(b, a64_bfm(0, 2, T1, o->reg, (32 - o->amt) & 31, 31 - o->amt)); break;
    case SHIFT_LSR: emit(b, a64_bfm(0, 2, T1, o->reg, o->amt, 31)); break;
    case SHIFT_ASR: emit(b, a64_bfm(0, 0, T1, o->reg, o->amt, 31)); break;
    default: emit(b, a64_extr(0, T1, o->reg, o->reg, o->amt)); break;
    }
    return T1;
}

/* logical op with A64 opc (0 AND, 1 ORR, 2 EOR), inv = invert operand */
static void emit_logic(cbuf *b, int opc, int inv, int d, int n, const op2_t *o)
{
    if (o->is_imm) {
        uint32_t v = inv ? ~o->imm : o->imm;
        int bm = a64_bitmask(v, 0);
        if (bm >= 0) {
            emit(b, a64_logic_imm(0, opc, d, n, (uint32_t)bm));
            return;
        }
        if (v == 0) { /* AND -> 0, ORR/EOR -> n */
            if (opc == 0) emit(b, a64_movw(0, 2, d, 0, 0));
            else emit(b, a64_logic_reg(0, 1, 0, d, XZR, n, 0, 0));
            return;
        }
        if (v == 0xffffffffu) {
            if (opc == 0) emit(b, a64_logic_reg(0, 1, 0, d, XZR, n, 0, 0));
            else if (opc == 1) jx_mov32(b, d, 0xffffffffu);
            else emit(b, a64_logic_reg(0, 1, 1, d, XZR, n, 0, 0)); /* mvn */
            return;
        }
        jx_mov32(b, T1, v);
        emit(b, a64_logic_reg(0, opc, 0, d, n, T1, 0, 0));
        return;
    }
    emit(b, a64_logic_reg(0, opc, inv, d, n, o->reg, o->shift, o->amt));
}

static void alu_write_pc_exit(jit_ctx *x, int r)
{
    cbuf *b = &x->cb;
    if (r != T0) emit(b, a64_logic_reg(0, 1, 0, T0, XZR, r, 0, 0));
    if (x->thumb) emit(b, a64_logic_imm(0, 1, T0, T0, (uint32_t)a64_bitmask(1, 0))); /* stay in Thumb */
    jx_exit_indirect_t0(x);
    x->ended = 1;
}

/* MOVS Rd, Rm, <shift> Rs (Thumb's 16-bit LSLS/LSRS/ASRS/RORS Rdn, Rm): amounts 0-255 with the AArch32 carry-out.
 * The old flags are saved first, so the sequence can use cmp/csel and rebuilds NZCV at the end. */
static int emit_movs_rsr(jit_ctx *x, const insn_t *i, int d)
{
    cbuf *b = &x->cb;
    int m = i->rm;
    emit(b, a64_mrs(T3, SYS_NZCV));                                              /* old flags */
    emit(b, a64_logic_imm(0, 0, T2, i->rs, (uint32_t)a64_bitmask(0xff, 0)));      /* w15 = amount */
    emit(b, a64_addsub_imm(0, 1, 1, XZR, T2, 63, 0));                             /* cmp w15, #63 */
    emit(b, a64_movw(0, 2, T1, 63, 0));
    emit(b, a64_csel(0, 0, T1, T2, T1, A64_LS));                                  /* w17 = min(amount, 63) */
    switch (i->shtype) {
    case SH_LSL:
        emit(b, a64_dp2(1, 8, T0, m, T1));                                        /* lslv x16, xm, x17 */
        emit(b, a64_bfm(1, 2, T4, T0, 32, 32));                                   /* carry = bit 32 */
        break;
    case SH_LSR:
        emit(b, a64_dp2(1, 9, T0, m, T1));
        emit(b, a64_bfm(1, 2, T4, m, 63, 62));                                    /* lsl x21, xm, #1 */
        emit(b, a64_dp2(1, 9, T4, T4, T1));
        emit(b, a64_logic_imm(0, 0, T4, T4, (uint32_t)a64_bitmask(1, 0)));
        break;
    case SH_ASR:
        emit(b, a64_bfm(1, 0, T4, m, 0, 31));                                     /* sxtw x21, wm */
        emit(b, a64_dp2(1, 10, T0, T4, T1));
        emit(b, a64_bfm(1, 2, T4, T4, 63, 62));                                   /* lsl x21, x21, #1 */
        emit(b, a64_dp2(1, 10, T4, T4, T1));
        emit(b, a64_logic_imm(0, 0, T4, T4, (uint32_t)a64_bitmask(1, 0)));
        break;
    default: /* ROR: amount mod 32; carry = result bit 31 */
        emit(b, a64_dp2(0, 11, T0, m, T2));
        emit(b, a64_bfm(0, 2, T4, T0, 31, 31));
        break;
    }
    emit(b, a64_bfm(0, 2, T1, T3, 29, 29));                                       /* old C */
    emit(b, a64_addsub_imm(0, 1, 1, XZR, T2, 0, 0));                              /* cmp w15, #0 */
    emit(b, a64_csel(0, 0, T4, T1, T4, A64_EQ));                                  /* amount 0: C unchanged */
    emit(b, a64_logic_reg(0, 1, 0, d, XZR, T0, 0, 0));                            /* result */
    emit(b, a64_logic_reg(0, 3, 0, XZR, d, d, 0, 0));                             /* tst: N, Z */
    emit(b, a64_mrs(T0, SYS_NZCV));
    emit(b, a64_logic_imm(0, 0, T3, T3, (uint32_t)a64_bitmask(CPSR_V, 0)));
    emit(b, a64_logic_reg(0, 1, 0, T0, T0, T3, 0, 0));
    emit(b, a64_bfm(0, 1, T0, T4, 3, 0));                                         /* bfi w16, w21, #29, #1 */
    emit(b, a64_msr(T0, SYS_NZCV));
    return 1;
}

/* force_dst >= 0: write the result there instead of Rd (conditional selects) */
static int emit_dp_to(jit_ctx *x, const insn_t *i, int force_dst)
{
    cbuf *b = &x->cb;
    unsigned a = i->sub;
    int logical = is_logical(a), test = a == A_TST || a == A_TEQ || a == A_CMP || a == A_CMN;
    if (i->form == F_RSR && i->s && a == A_MOV && i->rd != 15 && i->rm != 15 && i->rs != 15)
        return emit_movs_rsr(x, i, force_dst >= 0 ? force_dst : i->rd);
    if (i->form == F_RSR && (i->rm == 15 || i->rs == 15 || i->rn == 15 || (i->s && logical))) return 0;
    if (i->rd == 15 && i->s && !test) return 0;
    int d = (test || i->rd == 15) ? (test && !logical ? XZR : T0) : i->rd;
    if (force_dst >= 0) d = force_dst;
    int carry = -1;
    if (logical && i->s) carry = op2_carry(x, i); /* before the result may overwrite rm */
    int n = 0;
    if (a != A_MOV && a != A_MVN) n = jx_src(x, i->rn, T3);
    op2_t o;
    get_op2(x, i, &o, logical);
    switch (a) {
    case A_AND: case A_TST: emit_logic(b, 0, 0, d, n, &o); break;
    case A_EOR: case A_TEQ: emit_logic(b, 2, 0, d, n, &o); break;
    case A_ORR: emit_logic(b, 1, 0, d, n, &o); break;
    case A_BIC: emit_logic(b, 0, 1, d, n, &o); break;
    case A_ORN: emit_logic(b, 1, 1, d, n, &o); break;
    case A_MOV:
        if (o.is_imm) jx_mov32(b, d, o.imm);
        else emit(b, a64_logic_reg(0, 1, 0, d, XZR, o.reg, o.shift, o.amt));
        break;
    case A_MVN:
        if (o.is_imm) jx_mov32(b, d, ~o.imm);
        else emit(b, a64_logic_reg(0, 1, 1, d, XZR, o.reg, o.shift, o.amt));
        break;
    case A_ADD: case A_CMN: emit_addsub(b, 0, i->s, d, n, &o); break;
    case A_SUB: case A_CMP: emit_addsub(b, 1, i->s, d, n, &o); break;
    case A_RSB: emit(b, a64_addsub_reg(0, 1, i->s, d, op2_to_reg(b, &o), n, 0, 0)); break;
    case A_ADC: emit(b, a64_adc(0, 0, i->s, d, n, op2_to_reg(b, &o))); break;
    case A_SBC: emit(b, a64_adc(0, 1, i->s, d, n, op2_to_reg(b, &o))); break;
    case A_RSC: emit(b, a64_adc(0, 1, i->s, d, op2_to_reg(b, &o), n)); break;
    default: return 0;
    }
    if (logical && i->s) set_nzc(b, 0, d, carry);
    if (i->rd == 15 && !test && force_dst < 0) alu_write_pc_exit(x, d);
    return 1;
}

static int emit_dp(jit_ctx *x, const insn_t *i) { return emit_dp_to(x, i, -1); }

/* ---------------------------------------------------------------- multiplies and misc integer */

static int emit_mul(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int n = i->rn, m = i->rm;
    if (n == 15 || m == 15 || i->rd == 15) return 0;
    switch (i->sub) {
    case M_MUL:
        emit(b, a64_madd(0, 0, i->rd, n, m, XZR));
        if (i->s) set_nzc(b, 0, i->rd, -1);
        return 1;
    case M_MLA:
        if (i->ra == 15) return 0;
        emit(b, a64_madd(0, 0, i->rd, n, m, i->ra));
        if (i->s) set_nzc(b, 0, i->rd, -1);
        return 1;
    case M_MLS:
        if (i->ra == 15) return 0;
        emit(b, a64_madd(0, 1, i->rd, n, m, i->ra));
        return 1;
    default: {
        /* rd = RdHi, ra = RdLo */
        if (i->ra == 15) return 0;
        int u = i->sub == M_UMULL || i->sub == M_UMLAL || i->sub == M_UMAAL;
        if (i->sub == M_UMLAL || i->sub == M_SMLAL) {
            emit(b, a64_logic_reg(1, 1, 0, T1, i->ra, i->rd, SHIFT_LSL, 32)); /* x17 = hi:lo */
            emit(b, a64_maddl(u, 0, T0, n, m, T1));
        } else if (i->sub == M_UMAAL) {
            emit(b, a64_maddl(1, 0, T0, n, m, i->ra));
            emit(b, a64_addsub_reg(1, 0, 0, T0, T0, i->rd, 0, 0));
        } else {
            emit(b, a64_maddl(u, 0, T0, n, m, XZR));
        }
        emit(b, a64_logic_reg(0, 1, 0, i->ra, XZR, T0, 0, 0));      /* lo */
        emit(b, a64_bfm(1, 2, i->rd, T0, 32, 63));                  /* hi = x16 >> 32 */
        if (i->s) set_nzc(b, 1, T0, -1);
        return 1;
    }
    }
}

static int emit_misc_int(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int d = i->rd, m = i->rm, n = i->rn;
    if (d == 15) return 0;
    switch (i->op) {
    case OP_MOVW:
        jx_mov32(b, d, i->imm);
        return 1;
    case OP_MOVT:
        emit(b, a64_movw(0, 3, d, i->imm, 1));
        return 1;
    case OP_CLZ:
        if (m == 15) return 0;
        emit(b, a64_dp1(0, 4, d, m));
        return 1;
    case OP_REV:
        if (m == 15) return 0;
        emit(b, a64_dp1(0, 2, d, m));
        return 1;
    case OP_REV16:
        if (m == 15) return 0;
        emit(b, a64_dp1(0, 1, d, m));
        return 1;
    case OP_REVSH:
        if (m == 15) return 0;
        emit(b, a64_dp1(0, 1, T0, m));
        emit(b, a64_bfm(0, 0, d, T0, 0, 15)); /* sxth */
        return 1;
    case OP_RBIT:
        if (m == 15) return 0;
        emit(b, a64_dp1(0, 0, d, m));
        return 1;
    case OP_DIV:
        if (n == 15 || m == 15) return 0;
        emit(b, a64_dp2(0, i->sign ? 3 : 2, d, n, m));
        return 1;
    case OP_EXTEND: {
        if (m == 15) return 0;
        static const int opt[6] = {-1, 4, 5, -1, 0, 1}; /* SXTB16, SXTB, SXTH, UXTB16, UXTB, UXTH -> extend option */
        int o = opt[i->sub];
        if (o < 0) return 0;
        int src = m;
        if (i->shimm) {
            emit(b, a64_extr(0, T1, m, m, i->shimm));
            src = T1;
        }
        if (i->rn == 15) {
            /* SXTB: sbfm d, src, #0, #7; SXTH: #15; UXTB/UXTH: ubfm */
            int bits = (o & 3) == 0 ? 7 : 15;
            emit(b, a64_bfm(0, o >= 4 ? 0 : 2, d, src, 0, bits));
        } else {
            emit(b, a64_addsub_ext(0, 0, 0, d, n, src, o, 0));
        }
        return 1;
    }
    case OP_BFX:
        if (n == 15 || i->imm + i->shimm > 32) return 0;
        emit(b, a64_bfm(0, i->sign ? 0 : 2, d, n, i->imm, i->imm + i->shimm - 1));
        return 1;
    case OP_BFI: {
        unsigned lsb = i->imm, msb = i->shimm;
        if (msb < lsb) return 1; /* UNPREDICTABLE: leave Rd unchanged like the interpreter */
        int src = i->rn == 15 ? XZR : n;
        emit(b, a64_bfm(0, 1, d, src, (32 - lsb) & 31, msb - lsb)); /* bfi */
        return 1;
    }
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- loads and stores */

/* access at addr + off (host addressing: off is in range, see emit_ldst) */
static void mem_access(cbuf *b, int load, unsigned size, int sign, int rt, int addr, int off)
{
    int sz = size == 1 ? 0 : size == 2 ? 1 : 2;
    int opc = load ? (sign ? 3 : 1) : 0;
    if (off >= 0 && !(off & (size - 1))) emit(b, a64_ldst_imm(sz, opc, rt, addr, (uint32_t)off));
    else emit(b, a64_ldst_unscaled(sz, opc, rt, addr, off));
}

/* can a byte offset be folded into the access (no separate address computation)? */
static int fold_ok(int off, unsigned size)
{
    if (jit_exact_addr) return off == 0;
    if (off >= 0 && !(off & (size - 1)) && (unsigned)off < 4096 * size) return 1;
    return off >= -256 && off < 256;
}

static int emit_ldst(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    int load = (i->flags & MF_LOAD) != 0;
    if (i->size == 8 && (i->rd == 15 || i->ra == 15 || (load && i->rd == i->ra))) return 0;
    if (load && i->rd == 15 && i->size != 4) return 0;
    if (i->rn == 15 && i->w) return 0;
    if (i->form == F_REG && (i->rm == 15 || i->shtype == SH_RRX)) return 0;
    int addr, off = 0;
    /* effective address */
    if (i->p && i->form == F_IMM && i->rn != 15 && i->size != 8 &&
        fold_ok(i->u ? (int)i->imm : -(int)i->imm, i->size) && i->imm < 4096u * i->size) {
        addr = i->rn;
        off = i->u ? (int)i->imm : -(int)i->imm;
    } else if (i->rn == 15) {
        if (i->form == F_IMM) {
            jx_mov32(b, T0, i->u ? i->pcval + i->imm : i->pcval - i->imm);
        } else { /* PC-relative with a register offset (position-independent code) */
            if (i->shtype == SH_ROR || (i->shtype == SH_LSR && i->shimm == 32)) return 0;
            jx_mov32(b, T0, i->pcval);
            emit(b, a64_addsub_reg(0, !i->u, 0, T0, T0, i->rm, i->shtype, i->shimm == 32 ? 31 : i->shimm));
        }
        addr = T0;
    } else if (!i->p) {
        addr = i->rn;
    } else if (i->form == F_IMM) {
        if (i->imm == 0) {
            addr = i->rn;
        } else {
            uint32_t imm12;
            int sh;
            if (addsub_imm_ok(i->imm, &imm12, &sh)) {
                emit(b, a64_addsub_imm(0, !i->u, 0, T0, i->rn, imm12, sh));
            } else {
                jx_mov32(b, T1, i->imm);
                emit(b, a64_addsub_reg(0, !i->u, 0, T0, i->rn, T1, 0, 0));
            }
            addr = T0;
        }
    } else {
        if (i->shtype == SH_ROR) {
            emit(b, a64_extr(0, T1, i->rm, i->rm, i->shimm));
            emit(b, a64_addsub_reg(0, !i->u, 0, T0, i->rn, T1, 0, 0));
        } else {
            unsigned amt = i->shimm == 32 ? 31 : i->shimm;
            if (i->shtype == SH_LSR && i->shimm == 32) {
                emit(b, a64_logic_reg(0, 1, 0, T0, XZR, i->rn, 0, 0)); /* offset 0 */
            } else {
                emit(b, a64_addsub_reg(0, !i->u, 0, T0, i->rn, i->rm, i->shtype, amt));
            }
        }
        addr = T0;
    }
    /* post-indexed with a register offset: compute the new base before the access (Rt may be Rm) */
    int wb_ready = 0;
    if (i->w && !i->p && i->form == F_REG) {
        if (i->shtype == SH_ROR) {
            emit(b, a64_extr(0, T2, i->rm, i->rm, i->shimm));
            emit(b, a64_addsub_reg(0, !i->u, 0, T2, i->rn, T2, 0, 0));
        } else if (i->shtype == SH_LSR && i->shimm == 32) {
            emit(b, a64_logic_reg(0, 1, 0, T2, XZR, i->rn, 0, 0));
        } else {
            emit(b, a64_addsub_reg(0, !i->u, 0, T2, i->rn, i->rm, i->shtype, i->shimm == 32 ? 31 : i->shimm));
        }
        wb_ready = 1;
    }
    /* access */
    if (load) {
        if (i->size == 8) {
            emit(b, a64_ldstp(0, 1, i->rd, i->ra, addr, 0));
        } else {
            int rt = i->rd == 15 ? T1 : i->rd;
            mem_access(b, 1, i->size, i->sign, rt, addr, off);
        }
    } else {
        if (i->size == 8) {
            emit(b, a64_ldstp(0, 0, i->rd, i->ra, addr, 0));
        } else {
            int rt = jx_src(x, i->rd, T1);
            mem_access(b, 0, i->size, 0, rt, addr, off);
        }
    }
    /* writeback */
    if (i->w) {
        if (wb_ready) {
            emit(b, a64_logic_reg(0, 1, 0, i->rn, XZR, T2, 0, 0));
        } else if (i->p && addr == T0) {
            emit(b, a64_logic_reg(0, 1, 0, i->rn, XZR, T0, 0, 0)); /* rn = address */
        } else if (i->form == F_IMM) {
            uint32_t imm12;
            int sh;
            if (addsub_imm_ok(i->imm, &imm12, &sh)) {
                emit(b, a64_addsub_imm(0, !i->u, 0, i->rn, i->rn, imm12, sh));
            } else {
                jx_mov32(b, T2, i->imm);
                emit(b, a64_addsub_reg(0, !i->u, 0, i->rn, i->rn, T2, 0, 0));
            }
        }
    }
    if (load && i->rd == 15) {
        emit(b, a64_logic_reg(0, 1, 0, T0, XZR, T1, 0, 0));
        jx_exit_indirect_t0(x);
        x->ended = 1;
    }
    return 1;
}

static int emit_ldm(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    uint32_t list = i->reglist;
    int n = __builtin_popcount(list), load = i->op == OP_LDM;
    if (i->rn == 15 || !n) return 0;
    int start = i->u ? (i->p ? 4 : 0) : (i->p ? -4 * n : -4 * n + 4);
    int base_in_list = (list >> i->rn) & 1;
    int addr = i->rn;
    if (start != 0 || (load && base_in_list)) {
        if (start >= 0) emit(b, a64_addsub_imm(0, 0, 0, T0, i->rn, (uint32_t)start, 0));
        else emit(b, a64_addsub_imm(0, 1, 0, T0, i->rn, (uint32_t)-start, 0));
        addr = T0;
    }
    int regs[16], k = 0;
    for (int r = 0; r < 16; r++)
        if (list & (1u << r)) regs[k++] = r;
    if (!load && (list & 0x8000)) jx_mov32(b, T1, i->pcval);
    for (int j = 0; j < n;) {
        int ra = regs[j] == 15 ? T1 : regs[j];
        if (j + 1 < n) {
            int rb = regs[j + 1] == 15 ? T1 : regs[j + 1];
            emit(b, a64_ldstp(0, load, ra, rb, addr, 4 * j));
            j += 2;
        } else {
            emit(b, a64_ldst_imm(2, load, ra, addr, 4 * j));
            j++;
        }
    }
    if (i->w && !(load && base_in_list)) {
        if (i->u) emit(b, a64_addsub_imm(0, 0, 0, i->rn, i->rn, 4 * n, 0));
        else emit(b, a64_addsub_imm(0, 1, 0, i->rn, i->rn, 4 * n, 0));
    }
    if (load && (list & 0x8000)) {
        emit(b, a64_logic_reg(0, 1, 0, T0, XZR, T1, 0, 0));
        jx_exit_indirect_t0(x);
        x->ended = 1;
    }
    return 1;
}

/* ---------------------------------------------------------------- branches */

static int emit_branch(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    uint32_t next = i->pc + i->len;
    uint32_t link = x->thumb ? next | 1 : next;
    switch (i->op) {
    case OP_B:
        if (i->imm == x->b_pc && !x->b_it && x->fpcr_std == FPCR_GUEST) {
            /* loop back to this block's start (its entry checks for interrupts) */
            jit_vfp_flush(x);
            jx_patch_b(jx_placeholder(b), x->cb.start);
        } else {
            jx_exit_direct(x, i->imm, x->thumb, 0);
        }
        x->ended = 1;
        return 1;
    case OP_BL:
        jx_mov32(b, 14, link);
        jx_exit_direct(x, i->imm, x->thumb, 0);
        x->ended = 1;
        return 1;
    case OP_BLX_IMM:
        jx_mov32(b, 14, link);
        jx_exit_direct(x, i->imm, !x->thumb, 0);
        x->ended = 1;
        return 1;
    case OP_BX:
    case OP_BLX_REG: {
        int m = jx_src(x, i->rm, T0);
        if (m != T0) emit(b, a64_logic_reg(0, 1, 0, T0, XZR, m, 0, 0));
        if (i->op == OP_BLX_REG) jx_mov32(b, 14, link);
        jx_exit_indirect_t0(x);
        x->ended = 1;
        return 1;
    }
    case OP_CBZ: {
        /* side exit when taken; translation goes on with the next instruction */
        uint32_t *site = jx_placeholder(b);
        jx_exit_direct(x, i->imm, x->thumb, 0);
        *site = a64_cbz(0, !(i->flags & MF_NONZERO), i->rn, (int32_t)(b->p - site));
        x->side_exit = 1;
        return 1;
    }
    case OP_TB: {
        int nreg = jx_src(x, i->rn, T0);
        if (i->size == 1) {
            emit(b, a64_addsub_reg(1, 0, 0, T0, nreg, i->rm, 0, 0));
            emit(b, a64_ldst_imm(0, 1, T0, T0, 0));
        } else {
            emit(b, a64_addsub_reg(1, 0, 0, T0, nreg, i->rm, 0, 1));
            emit(b, a64_ldst_imm(1, 1, T0, T0, 0));
        }
        jx_mov32(b, T1, i->pcval | 1);
        emit(b, a64_addsub_reg(0, 0, 0, T0, T1, T0, 0, 1));
        jx_exit_indirect_t0(x);
        x->ended = 1;
        return 1;
    }
    default:
        return 0;
    }
}

/* LDREX/STREX (not the doubleword forms), same emulation as the interpreter: LDREX records the address, size and
 * value; STREX succeeds if they still match, by compare-and-swap against the recorded value. */
static int emit_exclusive(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    if (i->rn == 15 || i->rd == 15 || (i->op == OP_STREX && i->rm == 15)) return 0;
    if (i->size == 8 && (i->ra == 15 || (i->op == OP_LDREX && i->rd == i->ra))) return 0;
    int sz = i->size == 1 ? 0 : i->size == 2 ? 1 : i->size == 4 ? 2 : 3, acq = (i->flags & MF_ACQREL) != 0;
    int addr = i->rn;
    if (i->imm) {
        emit(b, a64_addsub_imm(0, 0, 0, T0, i->rn, i->imm, 0));
        addr = T0;
    }
    if (i->op == OP_LDREX) {
        if (acq) emit(b, a64_ldar(sz, T1, addr));
        else emit(b, a64_ldst_imm(sz, 1, T1, addr, 0));
        emit(b, a64_ldst_imm(2, 0, addr, R_CPU, offsetof(a32_cpu, excl_addr)));
        emit(b, a64_ldst_imm(3, 0, T1, R_CPU, offsetof(a32_cpu, excl_val)));
        jx_mov32(b, T2, i->size);
        emit(b, a64_ldst_imm(2, 0, T2, R_CPU, OFF_EXCL_VALID));
        if (sz == 3) {
            emit(b, a64_logic_reg(0, 1, 0, i->rd, XZR, T1, 0, 0)); /* low word (the base is no longer needed) */
            emit(b, a64_bfm(1, 2, i->ra, T1, 32, 63));             /* high word */
        } else {
            emit(b, a64_logic_reg(0, 1, 0, i->rd, XZR, T1, 0, 0)); /* rd last: it may be the base */
        }
        return 1;
    }
    /* STREX: status in rd */
    if (addr != T0) {
        emit(b, a64_logic_reg(1, 1, 0, T0, XZR, addr, 0, 0)); /* x16 = address (rd may alias nothing we need) */
        addr = T0;
    }
    emit(b, a64_ldst_imm(2, 1, T1, R_CPU, OFF_EXCL_VALID));
    emit(b, a64_addsub_imm(0, 1, 0, T1, T1, i->size, 0));                 /* valid - size */
    uint32_t *f1 = jx_placeholder(b);                                     /* cbnz w17, fail */
    emit(b, a64_ldst_imm(2, 1, T1, R_CPU, offsetof(a32_cpu, excl_addr)));
    emit(b, a64_logic_reg(0, 2, 0, T1, T1, T0, 0, 0));                   /* eor w17, w17, w16 */
    uint32_t *f2 = jx_placeholder(b);
    emit(b, a64_ldst_imm(3, 1, T2, R_CPU, offsetof(a32_cpu, excl_val)));
    int val = i->rm;
    if (sz == 3) { /* Rt:Rt2 as one doubleword */
        emit(b, a64_logic_reg(1, 1, 0, T4, i->rm, i->ra, SHIFT_LSL, 32));
        val = T4;
    }
    uint32_t *retry = b->p;
    emit(b, a64_ldxr(sz, 0, T1, T0));
    emit(b, a64_logic_reg(1, 2, 0, T1, T1, T2, 0, 0));                   /* eor x17, x17, x15 */
    uint32_t *f3 = jx_placeholder(b);                                     /* cbnz x17, fail_clrex */
    emit(b, a64_stxr(sz, acq, T3, val, T0));
    emit(b, a64_cbz(0, 1, T3, (int32_t)(retry - b->p)));                  /* cbnz w20, retry */
    emit(b, a64_movw(0, 2, i->rd, 0, 0));                                 /* status 0 */
    uint32_t *done = jx_placeholder(b);
    uint32_t *fail_clrex = b->p;
    emit(b, A64_CLREX);
    uint32_t *fail = b->p;
    emit(b, a64_movw(0, 2, i->rd, 1, 0));                                 /* status 1 */
    uint32_t *end = b->p;
    emit(b, a64_ldst_imm(2, 0, XZR, R_CPU, OFF_EXCL_VALID));
    *f1 = a64_cbz(0, 1, T1, (int32_t)(fail - f1));
    *f2 = a64_cbz(0, 1, T1, (int32_t)(fail - f2));
    *f3 = a64_cbz(1, 1, T1, (int32_t)(fail_clrex - f3));
    *done = a64_b((int32_t)(end - done));
    return 1;
}

static int emit_system(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    switch (i->op) {
    case OP_NOP:
        return 1;
    case OP_BARRIER:
        emit(b, A64_DMB_ISH);
        return 1;
    case OP_CLREX:
        emit(b, a64_ldst_imm(2, 0, XZR, R_CPU, OFF_EXCL_VALID));
        return 1;
    case OP_MRC:
        if (i->rd == 15) return 0;
        if (i->imm == CP15_TPIDRURO) emit(b, a64_ldst_imm(2, 1, i->rd, R_CPU, OFF_TPIDRURO));
        else if (i->imm == CP15_TPIDRURW) emit(b, a64_ldst_imm(2, 1, i->rd, R_CPU, OFF_TPIDRURW));
        else return 0;
        return 1;
    case OP_MCR:
        if (i->imm != CP15_TPIDRURW || i->rd == 15) return 0;
        emit(b, a64_ldst_imm(2, 0, i->rd, R_CPU, OFF_TPIDRURW));
        return 1;
    case OP_SVC:
        jx_exit_reason(x, i->pc + i->len, x->thumb, x->thumb ? itstate_advance(x->it) : 0, EXIT_SVC, i->imm);
        x->ended = 1;
        return 1;
    case OP_BKPT:
        jx_exit_reason(x, i->pc, x->thumb, x->it, EXIT_BKPT, 0);
        x->ended = 1;
        return 1;
    default:
        return 0;
    }
}

static int emit_native(jit_ctx *x, const insn_t *i)
{
    if (i->unpred) return 0; /* the interpreter defines what happens */
    switch (i->op) {
    case OP_DP: return emit_dp(x, i);
    case OP_MUL: return emit_mul(x, i);
    case OP_MOVW: case OP_MOVT: case OP_CLZ: case OP_REV: case OP_REV16: case OP_REVSH: case OP_RBIT: case OP_DIV:
    case OP_EXTEND: case OP_BFX: case OP_BFI:
        return emit_misc_int(x, i);
    case OP_LDST: return emit_ldst(x, i);
    case OP_LDM: case OP_STM: return emit_ldm(x, i);
    case OP_B: case OP_BL: case OP_BLX_IMM: case OP_BX: case OP_BLX_REG: case OP_CBZ: case OP_TB:
        return emit_branch(x, i);
    case OP_NOP: case OP_BARRIER: case OP_CLREX: case OP_MRC: case OP_MCR: case OP_SVC: case OP_BKPT:
        return emit_system(x, i);
    case OP_LDREX: case OP_STREX:
        return emit_exclusive(x, i);
    case OP_VFP: case OP_NEON:
        return jit_vfp(x, i);
    default:
        return 0;
    }
}

/* instructions after which the block must end even if they do not branch */
static int ends_block(const insn_t *i)
{
    if (i->writes_pc) return 1;
    if (i->op == OP_SETEND) return 1;
    if (i->op == OP_VFP && i->sub == V_MSR) return 1; /* FPCR changes: re-enter with the new value */
    return 0;
}

static void translate_insn(jit_ctx *x, const insn_t *i)
{
    cbuf *b = &x->cb;
    x->i = i;
    if (i->op == OP_IT) return;
    uint32_t next_it = x->thumb && x->it ? itstate_advance(x->it) : 0;
    if (i->op == OP_B && i->cond < C_AL && i->imm == x->b_pc && !x->b_it && !x->it && x->fpcr_std == FPCR_GUEST &&
        !i->unpred) {
        /* conditional loop back to the block start */
        jit_vfp_flush(x);
        emit(b, a64_bcond(i->cond, (int32_t)(b->start - b->p)));
        jx_exit_direct(x, i->pc + i->len, x->thumb, 0);
        x->ended = 1;
        return;
    }
    if (i->op == OP_B && i->cond < C_AL && !i->unpred) {
        /* conditional branch: side exit, translation goes on with the fall-through path */
        uint32_t *skip = jx_placeholder(b);
        jx_exit_direct(x, i->imm, x->thumb, 0);
        jx_patch_bcond(skip, i->cond ^ 1, b->p);
        return;
    }
    if (i->op == OP_UNDEF) {
        uint32_t *skip = (i->cond != C_AL && i->cond != C_NV) ? jx_placeholder(b) : NULL;
        jx_exit_reason(x, i->pc, x->thumb, x->it, EXIT_UNDEF, 0);
        if (skip) {
            jx_patch_bcond(skip, i->cond ^ 1, b->p);
            jx_exit_direct(x, i->pc + i->len, x->thumb, next_it);
        }
        x->ended = 1;
        return;
    }
    int cond = i->cond != C_AL && i->cond != C_NV && i->op != OP_CBZ;
    if (cond && i->op == OP_DP && !i->s && i->rd != 15 && !i->unpred && i->sub != A_ADC && i->sub != A_SBC &&
        i->sub != A_RSC && !(i->form == F_REG && i->shtype == SH_RRX)) {
        /* conditional ALU op without flag effects: compute into x16, then select (no branch) */
        uint32_t *save = b->p;
        insn_t t = *i;
        t.rd = 0;
        if (emit_dp_to(x, &t, T0)) {
            emit(b, a64_csel(0, 0, i->rd, T0, i->rd, i->cond));
            return;
        }
        b->p = save;
    }
    if (cond) jit_vfp_flush(x); /* both paths must agree on the cache */
    uint32_t *save_p = b->p;
    int save_stubs = x->nstubs, save_std = x->fpcr_std;
    __typeof__(x->vc) save_vc;
    memcpy(save_vc, x->vc, sizeof save_vc);
    uint32_t *skip = cond ? jx_placeholder(b) : NULL;
    x->side_exit = 0;
    if (emit_native(x, i)) {
        if (x->side_exit) return;
        if (skip) {
            jit_vfp_flush(x);
            if (x->fpcr_std != save_std) x->fpcr_std = FPCR_UNKNOWN; /* the skipping path did not switch */
            jx_patch_bcond(skip, i->cond ^ 1, b->p);
            if (x->ended || ends_block(i)) {
                jx_exit_direct(x, i->pc + i->len, x->thumb, next_it);
                x->ended = 1;
            }
        } else if (!x->ended && ends_block(i)) {
            jx_exit_direct(x, i->pc + i->len, x->thumb, next_it);
            x->ended = 1;
        }
        return;
    }
    /* interpreter fallback (it evaluates the condition itself) */
    if (jit_stats) {
        helper_hist[i->op < OP_COUNT ? i->op : 0]++;
        if (jit_stats > 1) fprintf(stderr, "a32: helper %08x %0*x op %d unpred %d\n", i->pc, i->len == 2 ? 4 : 8, i->raw,
                                   i->op, i->unpred);
    }
    b->p = save_p;
    x->nstubs = save_stubs;
    x->fpcr_std = save_std;
    memcpy(x->vc, save_vc, sizeof save_vc);
    x->ended = 0;
    jx_call_helper(x, i);
    if (ends_block(i)) {
        add_stub(x, STUB_ASIS, jx_placeholder(b))->reason = 0;
        x->ended = 1;
    }
}

/* ---------------------------------------------------------------- blocks */

static inline uint64_t make_key(uint32_t pc, int thumb, uint32_t it) { return pc | (uint64_t)thumb | (uint64_t)it << 32; }
static inline unsigned hash_key(uint64_t k) { return (unsigned)((k ^ (k >> 15) ^ (k >> 32) * 0x9e37u) & ((1u << HASH_BITS) - 1)); }

static inline unsigned ltab_index(uint64_t key) { return (unsigned)(key >> 1) & ((1u << LTAB_BITS) - 1); }

/* lock-free: blocks are published with a release store and never leave a hash chain before a flush */
static jit_block *lookup(uint64_t key)
{
    for (jit_block *b = __atomic_load_n(&htab[hash_key(key)], __ATOMIC_ACQUIRE); b; b = b->hnext)
        if (b->key == key && b->valid) return b;
    return NULL;
}

/* the run loop puts a block it dispatched back into the inline lookup table: a later block in the same slot must
 * not shut a hot target out of it. Without the lock, so an invalidation may run at the same time: see
 * invalidate_block. */
static void ltab_refill(jit_block *blk)
{
    jit_block **slot = &ltab[ltab_index(blk->key)];
    if (__atomic_load_n(slot, __ATOMIC_RELAXED) == blk) return;
    __atomic_store_n(slot, blk, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&blk->valid, __ATOMIC_SEQ_CST)) {
        jit_block *expect = blk;
        __atomic_compare_exchange_n(slot, &expect, &invalid_block, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

static jit_block **page_list(uint32_t page, int create)
{
    jit_block **t = pagelists[page >> 10];
    if (!t) {
        if (!create) return NULL;
        t = pagelists[page >> 10] = calloc(1u << 10, sizeof *t);
    }
    return &t[page & ((1u << 10) - 1)];
}

static void emit_stubs(jit_ctx *x)
{
    cbuf *b = &x->cb;
    jit_block *blk = x->b;
    int ndirect = 0;
    for (int k = 0; k < x->nstubs; k++) ndirect += x->stubs[k].kind == STUB_DIRECT;
    blk->sites = ndirect ? calloc(ndirect, sizeof(jit_site)) : NULL;
    blk->nsites = 0;
    for (int k = 0; k < x->nstubs; k++) {
        jit_stub *s = &x->stubs[k];
        uint32_t *stub = b->p;
        switch (s->kind) {
        case STUB_DIRECT: {
            jit_site *site = &blk->sites[blk->nsites++];
            site->insn = s->site;
            site->stub = stub;
            site->owner = blk;
            jx_mov32(b, T0, s->pc);
            emit(b, a64_ldst_imm(2, 0, T0, R_CPU, OFF_PC));
            store_const32(b, OFF_THUMB, s->thumb);
            store_const32(b, OFF_ITSTATE, s->it);
            jx_mov64(b, T0, (uint64_t)(uintptr_t)site);
            emit(b, a64_ldst_imm(3, 0, T0, R_CPU, OFF_LAST_EXIT));
            jx_patch_b(s->site, stub);
            break;
        }
        case STUB_INDIRECT:
            emit(b, a64_logic_imm(0, 0, T1, T0, (uint32_t)a64_bitmask(1, 0)));          /* and w17, w16, #1 */
            emit(b, a64_ldst_imm(2, 0, T1, R_CPU, OFF_THUMB));
            emit(b, a64_logic_imm(0, 0, T0, T0, (uint32_t)a64_bitmask(0xfffffffeu, 0))); /* and w16, w16, #~1 */
            emit(b, a64_ldst_imm(2, 0, T0, R_CPU, OFF_PC));
            emit(b, a64_ldst_imm(2, 0, XZR, R_CPU, OFF_ITSTATE));
            emit(b, a64_ldst_imm(3, 0, XZR, R_CPU, OFF_LAST_EXIT));
            *s->site = a64_cbz(1, 1, T2, (int32_t)(stub - s->site));
            break;
        case STUB_REASON:
            jx_mov32(b, T0, s->pc);
            emit(b, a64_ldst_imm(2, 0, T0, R_CPU, OFF_PC));
            store_const32(b, OFF_THUMB, s->thumb);
            store_const32(b, OFF_ITSTATE, s->it);
            store_const32(b, OFF_EXIT, s->reason);
            if (s->reason == EXIT_SVC) store_const32(b, OFF_SVC, s->svc);
            emit(b, a64_ldst_imm(3, 0, XZR, R_CPU, OFF_LAST_EXIT));
            jx_patch_b(s->site, stub);
            break;
        case STUB_ASIS:
            /* the interpreter already set pc/thumb/itstate; w20 = its result */
            emit(b, a64_ldst_imm(2, 0, T3, R_CPU, OFF_EXIT));
            emit(b, a64_ldst_imm(3, 0, XZR, R_CPU, OFF_LAST_EXIT));
            if (s->reason) *s->site = a64_cbz(0, 1, T3, (int32_t)(stub - s->site));
            else jx_patch_b(s->site, stub);
            break;
        }
        emit(b, a64_ldst_imm(3, 1, T0, R_CPU, OFF_EXIT_STUB));
        emit(b, a64_br(T0));
    }
}

static int decode_guest(uint32_t pc, int thumb, uint32_t it, insn_t *i)
{
    if (!thumb) {
        if (!mem_readable(pc, 4)) return 0;
        a32_decode(ld32(pc), pc, i);
        return 4;
    }
    if (!mem_readable(pc, 2)) return 0;
    uint16_t hw[2] = {ld16(pc), 0};
    if (t32_insn_len(hw[0]) == 4) {
        if (!mem_readable(pc + 2, 2)) return 0;
        hw[1] = ld16(pc + 2);
    }
    int in_it = (it & 0xf) != 0;
    int itcond = (it >> 4) | ((it & 0xf) == 8 ? 0x100 : 0);
    return t32_decode(hw, pc, in_it, itcond, i);
}

static void mark_code(uint32_t lo, uint32_t hi)
{
    for (uint32_t p = lo / GUEST_PAGE; p <= (hi - 1) / GUEST_PAGE; p++) codepages[p >> 3] |= 1u << (p & 7);
}

/* "START SIZE name" lines (perf's /tmp/perf-<pid>.map format): the block's body as "a32:<pc>[T]", each FPCR write
 * with the instruction after it (where its samples land) as "a32:fpcr", the exit stubs as "a32:exits" */
static void write_perf_map(const jit_block *blk, const uint32_t *stubs)
{
    char name[32];
    snprintf(name, sizeof name, "a32:%08x%s", (uint32_t)blk->key, blk->thumb ? "T" : "");
#define RANGE(lo, hi, n) fprintf(perf_map, "%lx %lx %s\n", (unsigned long)(uintptr_t)(lo), \
                                 (unsigned long)(((hi) - (lo)) * 4), n)
    const uint32_t *from = blk->code;
    for (const uint32_t *w = blk->code; w + 1 < stubs; w++) {
        if (*w != a64_msr(T2, SYS_FPCR)) continue;
        if (w > from) RANGE(from, w, name);
        RANGE(w, w + 2, "a32:fpcr");
        from = w + 2;
    }
    if (stubs > from) RANGE(from, stubs, name);
    if (blk->end > stubs) RANGE(stubs, blk->end, "a32:exits");
#undef RANGE
    fflush(perf_map);
}

/* returns NULL if the code is not readable (*fault set) or the cache is full (*full set) */
static jit_block *translate(uint32_t pc, int thumb, uint32_t it, int *fault, int *full)
{
    *fault = *full = 0;
    if ((size_t)(cache_end - cache_cur) * 4 < CACHE_MARGIN) {
        *full = 1;
        return NULL;
    }
    insn_t first;
    if (!decode_guest(pc, thumb, it, &first)) {
        *fault = 1;
        return NULL;
    }
    jit_block *blk = calloc(1, sizeof *blk);
    blk->key = make_key(pc, thumb, it);
    blk->thumb = thumb;
    jit_ctx *x = calloc(1, sizeof *x);
    x->cb.start = x->cb.p = cache_cur;
    x->cb.end = cache_cur + (CACHE_MARGIN / 4) - 64;
    x->b = blk;
    x->b_pc = pc;
    x->b_it = it;
    x->thumb = thumb;
    x->it = it;
    x->fpcr_std = FPCR_GUEST;
    uint32_t map_host[MAX_INSNS], map_pc[MAX_INSNS];
    uint8_t map_it[MAX_INSNS];
    uint64_t map_vc[MAX_INSNS];
    int nmap = 0;
    cbuf *b = &x->cb;
    /* entry: leave if interrupted */
    emit(b, a64_ldst_imm(2, 1, T0, R_CPU, OFF_INTERRUPT));
    {
        jit_stub *s = add_stub(x, STUB_REASON, jx_placeholder(b));
        s->pc = pc, s->thumb = thumb, s->it = it, s->reason = EXIT_NONE;
        uint32_t *cbz_site = s->site;
        /* the stub patches its site to "b"; turn it into "cbnz w16" after emission (see below) */
        (void)cbz_site;
    }
    uint32_t cur = pc, lo = pc, hi = pc;
    insn_t in = first;
    int len = first.len;
    insn_t nx;
    int nlen = 0;
    for (int n = 0;; n++) {
        map_host[nmap] = (uint32_t)(b->p - b->start);
        map_pc[nmap] = cur;
        map_it[nmap] = (uint8_t)x->it;
        map_vc[nmap] = jit_vfp_state(x);
        nmap++;
        x->pc = cur;
        /* one instruction of lookahead for peepholes (only if it will be translated here) */
        uint32_t it_after = thumb ? (in.op == OP_IT ? in.imm : (x->it ? itstate_advance(x->it) : 0)) : 0;
        nlen = (n + 1 < MAX_INSNS && !in.writes_pc) ? decode_guest(cur + len, thumb, it_after, &nx) : 0;
        x->next = nlen ? &nx : NULL;
        translate_insn(x, &in);
        cur += len;
        if (cur > hi) hi = cur;
        x->it = it_after;
        if (x->ended || b->overflow) break;
        /* a fused VCMP needs its VMRS in the same block (x->next is set, so it is within MAX_INSNS) */
        if ((!x->skip_vmrs && (n + 1 >= MAX_INSNS || (size_t)(b->p - b->start) > (CACHE_MARGIN / 4) / 2)) ||
            !(len = nlen ? nlen : decode_guest(cur, thumb, x->it, &in))) {
            jx_exit_direct(x, cur, thumb, x->it);
            break;
        }
        if (nlen) in = nx;
    }
    uint32_t *stubs_start = b->p;
    emit_stubs(x);
    if (b->overflow) {
        fatal("block at 0x%08x overflowed the code buffer", pc);
    }
    /* the entry check is a conditional exit: cbnz w16, stub */
    {
        jit_stub *s = &x->stubs[0];
        uint32_t *stub = blk->code; (void)stub;
        uint32_t target_word = *s->site; /* "b stub" */
        int32_t off = (int32_t)(target_word << 6) >> 6;
        *s->site = a64_cbz(0, 1, T0, off);
    }
    blk->code = b->start;
    blk->end = b->p;
    blk->guest_lo = lo;
    blk->guest_hi = hi;
    blk->nmap = nmap;
    blk->map_host = malloc(nmap * sizeof(uint32_t));
    blk->map_pc = malloc(nmap * sizeof(uint32_t));
    blk->map_it = malloc(nmap);
    blk->map_vc = malloc(nmap * sizeof(uint64_t));
    memcpy(blk->map_vc, map_vc, nmap * sizeof(uint64_t));
    memcpy(blk->map_host, map_host, nmap * sizeof(uint32_t));
    memcpy(blk->map_pc, map_pc, nmap * sizeof(uint32_t));
    memcpy(blk->map_it, map_it, nmap);
    blk->valid = 1;
    cache_cur = (uint32_t *)(((uintptr_t)b->p + 15) & ~(uintptr_t)15);
    __builtin___clear_cache((char *)blk->code, (char *)blk->end);
    uint32_t p0 = lo / GUEST_PAGE, p1 = (hi - 1) / GUEST_PAGE;
    if (p1 > p0 + 1) fatal("block at 0x%08x spans more than two pages", pc);
    jit_block **l = page_list(p0, 1);
    blk->pnext[0] = *l, *l = blk;
    if (p1 != p0) l = page_list(p1, 1), blk->pnext[1] = *l, *l = blk;
    unsigned h = hash_key(blk->key);
    blk->hnext = htab[h];
    __atomic_store_n(&htab[h], blk, __ATOMIC_RELEASE);
    if (it == 0) __atomic_store_n(&ltab[ltab_index(blk->key)], blk, __ATOMIC_RELEASE);
    if (nblocks == cap_blocks) {
        cap_blocks = cap_blocks ? cap_blocks * 2 : 4096;
        blocks = realloc(blocks, cap_blocks * sizeof *blocks);
    }
    blocks[nblocks++] = blk;
    mark_code(lo, hi);
    if (perf_map) write_perf_map(blk, stubs_start);
    st_blocks++;
    if (opt_trace) fprintf(stderr, "a32: block %08x%s it=%02x: %d insns, %zu host words\n", pc, thumb ? "T" : "A", it,
                           nmap, (size_t)(blk->end - blk->code));
    if (dump_file) {
        /* tools/jitdump.py reads this: "block <pc> <thumb> <words>", host words in hex, "@ <pc>" before each insn */
        fprintf(dump_file, "block %08x %d %zu\n", pc, thumb, (size_t)(blk->end - blk->code));
        int k = 0;
        for (uint32_t *w = blk->code; w < blk->end; w++) {
            while (k < nmap && blk->map_host[k] == (uint32_t)(w - blk->code))
                fprintf(dump_file, "@ %08x\n", blk->map_pc[k++]);
            fprintf(dump_file, "%08x\n", *w);
        }
        fflush(dump_file);
    }
    free(x);
    return blk;
}

static void chain(jit_site *s, jit_block *target)
{
    if (!s->owner->valid || !target->valid || s->target) return;
    s->target = target;
    s->next_in = target->in;
    target->in = s;
    __atomic_store_n(s->insn, a64_b((int32_t)(target->code - s->insn)), __ATOMIC_RELEASE);
    __builtin___clear_cache((char *)s->insn, (char *)(s->insn + 1));
    st_chains++;
}

static void invalidate_block(jit_block *blk)
{
    if (!blk->valid) return;
    /* fenced against ltab_refill: either it sees valid == 0 or we see its entry */
    __atomic_store_n(&blk->valid, 0, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned li = ltab_index(blk->key);
    if (__atomic_load_n(&ltab[li], __ATOMIC_SEQ_CST) == blk)
        __atomic_store_n(&ltab[li], &invalid_block, __ATOMIC_RELEASE);
    for (jit_site *s = blk->in; s; s = s->next_in) {
        __atomic_store_n(s->insn, a64_b((int32_t)(s->stub - s->insn)), __ATOMIC_RELEASE);
        __builtin___clear_cache((char *)s->insn, (char *)(s->insn + 1));
        s->target = NULL;
    }
    blk->in = NULL;
    /* sites of this block chained into others: unlink so a later invalidation of the target does not touch us */
    for (int k = 0; k < blk->nsites; k++) {
        jit_site *s = &blk->sites[k];
        if (!s->target) continue;
        jit_site **pp = &s->target->in;
        while (*pp && *pp != s) pp = &(*pp)->next_in;
        if (*pp) *pp = s->next_in;
        s->target = NULL;
    }
}

void jit_invalidate(uint32_t addr, uint32_t len)
{
    if (!cache || !len) return;
    uint32_t end = addr + len < addr ? 0xffffffffu : addr + len;
    int any = 0;
    for (uint32_t p = addr / GUEST_PAGE; p <= (end - 1) / GUEST_PAGE; p++)
        if ((codepages[p >> 3] >> (p & 7)) & 1) {
            any = 1;
            break;
        }
    if (!any) return;
    /* only the blocks on the affected pages (Mono's JIT flushes new code next to translated code all the time);
     * invalid blocks leave the lists on the way */
    pthread_mutex_lock(&tlock);
    for (uint32_t p = addr / GUEST_PAGE; p <= (end - 1) / GUEST_PAGE; p++) {
        if (!((codepages[p >> 3] >> (p & 7)) & 1)) continue;
        jit_block **pp = page_list(p, 0);
        while (pp && *pp) {
            jit_block *blk = *pp;
            int k = blk->guest_lo / GUEST_PAGE != p;
            if (blk->valid && blk->guest_lo < end && blk->guest_hi > addr) invalidate_block(blk);
            if (blk->valid) pp = &blk->pnext[k];
            else *pp = blk->pnext[k];
        }
    }
    pthread_mutex_unlock(&tlock);
}

static void free_all_blocks(void)
{
    for (size_t k = 0; k < nblocks; k++) {
        jit_block *blk = blocks[k];
        free(blk->sites);
        free(blk->map_host);
        free(blk->map_pc);
        free(blk->map_it);
        free(blk->map_vc);
        free(blk);
    }
    nblocks = 0;
    memset(htab, 0, sizeof htab);
    for (size_t k = 0; k < (1u << LTAB_BITS); k++) ltab[k] = &invalid_block;
    memset(codepages, 0, sizeof codepages);
    for (size_t k = 0; k < sizeof pagelists / sizeof *pagelists; k++) {
        free(pagelists[k]);
        pagelists[k] = NULL;
    }
    while (arenas) {
        arena_t *a = arenas->next;
        free(arenas);
        arenas = a;
    }
    cache_cur = cache;
}

/* ---------------------------------------------------------------- thread coordination for flushes */

/* every dispatch passes here: a per-thread flag and a fence instead of a shared counter, whose cache line all
 * threads fought over (flush_cache sets flush_pending, fences, then waits for the flags: Dekker) */
static void enter_jit(void)
{
    for (;;) {
        __atomic_store_n(&g_self->in_jit_section, 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&flush_pending, __ATOMIC_RELAXED)) break;
        __atomic_store_n(&g_self->in_jit_section, 0, __ATOMIC_RELEASE);
        while (__atomic_load_n(&flush_pending, __ATOMIC_ACQUIRE)) sched_yield();
    }
}

static void leave_jit(void) { __atomic_store_n(&g_self->in_jit_section, 0, __ATOMIC_RELEASE); }

/* run_guest after a fault longjmp'ed out of translated code */
void jit_fault_cleanup(a32_thread *t)
{
    if (t->in_jit_section) leave_jit();
    t->cpu.last_exit = 0;
}

extern void threads_interrupt_all(void);

static void flush_cache(void)
{
    /* the caller is not inside translated code (left before calling) */
    __atomic_store_n(&flush_pending, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    threads_interrupt_all();
    while (threads_any_in_jit()) sched_yield();
    pthread_mutex_lock(&tlock);
    free_all_blocks();
    st_flushes++;
    pthread_mutex_unlock(&tlock);
    __atomic_store_n(&flush_pending, 0, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------- run loop */

static int jit_helper_exec(a32_cpu *c, const insn_t *i)
{
    a32_thread *t = c->thread;
    t->fault_pc = i->pc;
    t->in_helper = 1;
    int r = a32_exec(c, i);
    t->in_helper = 0;
    return r;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;

void jit_report(void)
{
    if (!jit_stats) return;
    fprintf(stderr, "a32: %lu blocks, %lu helper call sites, %lu chains, %lu flushes, %zu KiB code\n", st_blocks,
            st_helpers_emitted, st_chains, st_flushes, (size_t)(cache_cur - cache) * 4 / 1024);
    fprintf(stderr, "a32: helper sites by op:");
    for (int k = 0; k < OP_COUNT; k++)
        if (helper_hist[k]) fprintf(stderr, " %d:%lu", k, helper_hist[k]);
    fprintf(stderr, "\n");
}

static void jit_init(void)
{
    size_t bytes = CACHE_BYTES;
    const char *e = getenv("A32_CACHE_KB"); /* smaller caches exercise the flush path */
    if (e && atoi(e) >= 128 && (size_t)atoi(e) << 10 < CACHE_BYTES) bytes = (size_t)atoi(e) << 10;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fatal("cannot allocate the code cache: %s", strerror(errno));
    cache = cache_cur = p;
    cache_end = cache + bytes / 4;
    for (size_t k = 0; k < (1u << LTAB_BITS); k++) ltab[k] = &invalid_block;
    helpers[HELP_EXEC] = (void *)jit_helper_exec;
    if (getenv("A32_STATS")) jit_stats = atoi(getenv("A32_STATS")) > 1 ? 2 : 1;
    if (getenv("A32_DUMP")) dump_file = fopen(getenv("A32_DUMP"), "w");
    if (getenv("A32_PERFMAP")) { /* a path, or "1" for /tmp/perf-<pid>.map */
        char path[64];
        const char *e = getenv("A32_PERFMAP");
        if (!strcmp(e, "1")) snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)getpid()), e = path;
        perf_map = fopen(e, "w");
    }
    if (getenv("A32_EXACT_ADDR")) jit_exact_addr = 1;
}

int jit_run(a32_cpu *c)
{
    pthread_once(&once, jit_init);
    a32_thread *t = c->thread;
    if (!c->exit_stub) {
        c->exit_stub = (uint64_t)(uintptr_t)jit_exit;
        c->helper_tab = (uint64_t)(uintptr_t)helpers;
        c->lookup_tab = (uint64_t)(uintptr_t)ltab;
    }
    for (;;) {
        c->interrupt = 0;
        if (signal_pending(t)) return EXIT_NONE;
        uint32_t pc = c->r[15];
        if (pc >= 0xffff0f60u && kuser_helper(c)) continue;
        uint64_t key = make_key(pc, c->thumb, c->itstate);
        if (a32_break_pc[0]) /* A32_BREAK (debugging aid) */
            for (int b = 0; b < 4 && a32_break_pc[b]; b++)
                if (pc == a32_break_pc[b] && a32_break_hits[b] < 3) {
                    a32_break_hits[b]++;
                    a32_break_hit(c);
                }
        enter_jit();
        /* the lock only to translate or to chain the exit we came from (once per exit) */
        jit_block *blk = NULL;
        if (!c->itstate) {
            jit_block *f = __atomic_load_n(&ltab[ltab_index(key)], __ATOMIC_ACQUIRE);
            if (f->key == key && f->valid) blk = f;
        }
        if (!blk) blk = lookup(key);
        if (!blk || c->last_exit) {
            pthread_mutex_lock(&tlock);
            if (!blk || !blk->valid) blk = lookup(key);
            if (!blk) {
                int fault, full;
                blk = translate(pc, c->thumb, c->itstate, &fault, &full);
                if (!blk) {
                    pthread_mutex_unlock(&tlock);
                    leave_jit();
                    if (full) {
                        flush_cache();
                        c->last_exit = 0;
                        continue;
                    }
                    c->fault_addr = pc;
                    return EXIT_SEGV;
                }
            }
            if (c->last_exit) chain((jit_site *)(uintptr_t)c->last_exit, blk);
            pthread_mutex_unlock(&tlock);
        }
        if (!c->itstate) ltab_refill(blk);
        c->last_exit = 0;
        c->exit_reason = EXIT_NONE;
        t->in_guest = 1;
        int r = jit_enter(c, blk->code);
        t->in_guest = 0;
        leave_jit();
        if (opt_trace) fprintf(stderr, "a32: exit at %08x%s reason %d\n", c->r[15], c->thumb ? "T" : "A", r);
        if (r != EXIT_NONE) {
            c->last_exit = 0;
            return r;
        }
    }
}

/* In the host signal handler: a fault in guest memory while running translated code. Rebuilds the guest state
 * from the host registers; returns 1 if c holds a consistent guest state. */
int jit_owns_pc(uint64_t pc) { return cache && pc >= (uint64_t)(uintptr_t)cache && pc < (uint64_t)(uintptr_t)cache_end; }

int jit_fault_state(a32_thread *t, void *ucv)
{
    ucontext_t *uc = ucv;
    a32_cpu *c = &t->cpu;
    uint64_t pc = uc->uc_mcontext.pc;
    if (cache && pc >= (uint64_t)(uintptr_t)cache && pc < (uint64_t)(uintptr_t)cache_end) {
        size_t lo = 0, hi = nblocks;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if ((uint64_t)(uintptr_t)blocks[mid]->code <= pc) lo = mid;
            else hi = mid;
        }
        if (!nblocks) return 0;
        jit_block *blk = blocks[lo];
        uint32_t off = (uint32_t)((pc - (uint64_t)(uintptr_t)blk->code) / 4);
        int k = 0;
        while (k + 1 < blk->nmap && blk->map_host[k + 1] <= off) k++;
        for (int r = 0; r < 15; r++) c->r[r] = (uint32_t)uc->uc_mcontext.regs[r];
        c->nzcv = (uint32_t)uc->uc_mcontext.pstate & 0xf0000000u;
        /* guest q0-q15 live in v16-v31: take them (and the cumulative FP flags) from the FPSIMD record */
        for (struct _aarch64_ctx *h = (struct _aarch64_ctx *)uc->uc_mcontext.__reserved;
             h->magic && (char *)h < (char *)uc->uc_mcontext.__reserved + sizeof uc->uc_mcontext.__reserved;
             h = (struct _aarch64_ctx *)((char *)h + h->size)) {
            if (h->magic == FPSIMD_MAGIC) {
                struct fpsimd_context *f = (struct fpsimd_context *)h;
                memcpy(c->d, &f->vregs[16], 256);
                c->fpsr |= f->fpsr & FPSR_MASK;
                /* dirty scalar VFP values cached in v8-v15 */
                uint64_t vc = blk->map_vc[k];
                for (int s = 0; s < 8; s++) {
                    uint8_t e = (uint8_t)(vc >> (8 * s));
                    if (!(e & 0x80)) continue;
                    unsigned view = (e >> 5) & 3, reg = e & 31;
                    if (view == 1) memcpy((uint32_t *)c->d + reg, &f->vregs[8 + s], 4);
                    else if (view == 2) memcpy(&c->d[reg], &f->vregs[8 + s], 8);
                }
                break;
            }
            if (!h->size) break;
        }
        c->r[15] = blk->map_pc[k];
        c->thumb = blk->thumb;
        c->itstate = blk->map_it[k];
        c->excl_valid = 0;
        return 1;
    }
    if (t->in_helper) {
        t->in_helper = 0;
        c->r[15] = t->fault_pc;
        return 1;
    }
    return 0;
}
