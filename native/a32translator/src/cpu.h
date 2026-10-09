/* Guest (AArch32, user mode) CPU state.
 *
 * Layout notes:
 *  - nzcv uses the CPSR/A64 NZCV bit positions (31..28), so the JIT can MSR it into the host flags unchanged.
 *  - fpcr/fpsr use the A64 FPCR/FPSR layouts, which match the AArch32 FPSCR bit positions (AHP 26, DN 25, FZ 24,
 *    RMode 23:22; QC 27, IDC 7, IXC 4, UFC 3, OFC 2, DZC 1, IOC 0). FPSCR = fpscr_nzcv | fpcr | fpsr.
 *  - d[] is the VFP/NEON register file. On a little-endian host S<n> is ((uint32_t *)d)[n] and Q<n> is d[2n]:d[2n+1],
 *    exactly the AArch32 aliasing.
 *  - The JIT addresses fields by offset (cpu_offsets.h checks them at compile time), so keep the order. */
#ifndef A32_CPU_H
#define A32_CPU_H

#include <stdint.h>
#include <stddef.h>

#define CPSR_N (1u << 31)
#define CPSR_Z (1u << 30)
#define CPSR_C (1u << 29)
#define CPSR_V (1u << 28)

#define FPCR_AHP (1u << 26)
#define FPCR_DN (1u << 25)
#define FPCR_FZ (1u << 24)
#define FPCR_RMODE_SHIFT 22
#define FPCR_MASK (FPCR_AHP | FPCR_DN | FPCR_FZ | (3u << FPCR_RMODE_SHIFT))
#define FPSR_QC (1u << 27)
#define FPSR_MASK (FPSR_QC | 0x9fu)
/* "Standard FPSCR value" used by every Advanced SIMD floating-point operation: DN=1, FZ=1, round to nearest. */
#define FPCR_STANDARD (FPCR_DN | FPCR_FZ)

enum exit_reason {
    EXIT_NONE = 0,   /* block ended normally, continue at r[15] */
    EXIT_SVC,        /* supervisor call, r[15] already points after it */
    EXIT_UNDEF,      /* undefined instruction at r[15] */
    EXIT_BKPT,       /* breakpoint at r[15] */
    EXIT_FLUSH,      /* code cache must be flushed (cacheflush, SMC), continue at r[15] */
    EXIT_SEGV,       /* guest memory fault (fault_addr) at r[15] */
};

typedef struct a32_cpu {
    uint32_t r[16];          /* 0x000 */
    uint32_t nzcv;           /* 0x040 */
    uint32_t q;              /* 0x044 sticky saturation flag (0/1) */
    uint32_t ge;             /* 0x048 GE[3:0] */
    uint32_t thumb;          /* 0x04c 0 = ARM, 1 = Thumb */
    uint32_t itstate;        /* 0x050 ITSTATE[7:0] */
    uint32_t fpscr_nzcv;     /* 0x054 FPSCR[31:28] */
    uint32_t fpcr;           /* 0x058 guest FPSCR control bits, A64 FPCR layout */
    uint32_t fpsr;           /* 0x05c guest cumulative flags, A64 FPSR layout */
    uint32_t tpidruro;       /* 0x060 TLS register (set_tls), read by MRC p15,0,Rt,c13,c0,3 */
    uint32_t tpidrurw;       /* 0x064 */
    uint32_t excl_addr;      /* 0x068 exclusive monitor (LDREX/STREX), emulated as compare-and-swap */
    uint32_t excl_valid;     /* 0x06c */
    uint64_t excl_val;       /* 0x070 */
    uint64_t exit_reason;    /* 0x078 */
    uint32_t svc_imm;        /* 0x080 */
    uint32_t fault_addr;     /* 0x084 */
    uint64_t host_sp;        /* 0x088 host stack pointer saved by jit_enter */
    uint64_t exit_stub;      /* 0x090 address of jit_exit (JIT code branches there through a register) */
    uint64_t helper_tab;     /* 0x098 address of the JIT helper table */
    uint64_t lookup_tab;     /* 0x0a0 block lookup hash table (direct-mapped) */
    uint64_t host_fpcr;      /* 0x0a8 host FPCR at jit_enter, restored at exit */
    uint32_t interrupt;      /* 0x0b0 set asynchronously (signal, code flush): JIT code leaves at the next block */
    uint32_t pad1;           /* 0x0b4 */
    uint64_t last_exit;      /* 0x0b8 chainable exit taken (struct jit_site *), 0 if none */
    uint64_t d[32] __attribute__((aligned(16))); /* 0x0c0 VFP/NEON registers */
    /* Not touched by JIT code directly. */
    struct a32_thread *thread;
    uint64_t insn_count;
} a32_cpu;

static inline uint32_t *cpu_s(a32_cpu *c) { return (uint32_t *)c->d; }

#define OFF_R(n) (offsetof(a32_cpu, r) + 4 * (n))
#define OFF_D(n) (offsetof(a32_cpu, d) + 8 * (n))

_Static_assert(offsetof(a32_cpu, nzcv) == 0x40, "layout");
_Static_assert(offsetof(a32_cpu, exit_reason) == 0x78, "layout");
_Static_assert(offsetof(a32_cpu, host_sp) == 0x88, "layout");
_Static_assert(offsetof(a32_cpu, exit_stub) == 0x90, "layout");
_Static_assert(offsetof(a32_cpu, helper_tab) == 0x98, "layout");
_Static_assert(offsetof(a32_cpu, lookup_tab) == 0xa0, "layout");
_Static_assert(offsetof(a32_cpu, host_fpcr) == 0xa8, "layout");
_Static_assert(offsetof(a32_cpu, interrupt) == 0xb0, "layout");
_Static_assert(offsetof(a32_cpu, last_exit) == 0xb8, "layout");
_Static_assert(offsetof(a32_cpu, d) == 0xc0, "layout");

static inline uint32_t cpu_fpscr(const a32_cpu *c) { return c->fpscr_nzcv | c->fpcr | c->fpsr; }
static inline void cpu_set_fpscr(a32_cpu *c, uint32_t v)
{
    c->fpscr_nzcv = v & 0xf0000000u;
    c->fpcr = v & FPCR_MASK;
    c->fpsr = v & FPSR_MASK;
}

/* APSR as read by MRS: NZCVQ + GE. */
static inline uint32_t cpu_apsr(const a32_cpu *c) { return c->nzcv | (c->q << 27) | (c->ge << 16); }

#endif
