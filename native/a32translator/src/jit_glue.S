/* Entry to and exit from translated code.
 *
 * In translated code: x0-x14 = guest r0-r14 (upper halves always zero), v16-v31 = guest q0-q15, x19 = a32_cpu *,
 * host NZCV = guest NZCV, host FPSR collects the guest's cumulative FP flags; FPCR holds the guest's FPSCR control
 * bits or the Advanced SIMD standard value (blocks track which).
 * Scratch: x15-x17, x20-x23 (x20-x23 survive helper calls), v0-v7. v8-v15 cache scalar VFP values within a block
 * (their host values are saved here). x18 and x24-x28 are never touched. */

#define OFF_NZCV 0x40
#define OFF_FPCR 0x58
#define OFF_FPSR 0x5c
#define OFF_EXIT_REASON 0x78
#define OFF_HOST_FPCR 0xa8
#define OFF_D 0xc0

    .text
    .p2align 4
    .global jit_enter
    .type jit_enter, %function
/* int jit_enter(a32_cpu *c, const void *code): runs translated code until it exits, returns c->exit_reason */
jit_enter:
    stp x29, x30, [sp, #-160]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x19, x0
    mrs x2, fpcr
    str x2, [x19, #OFF_HOST_FPCR]
    ldr w3, [x19, #OFF_FPCR]
    msr fpcr, x3
    msr fpsr, xzr
    mov x16, x1
    ldp w0, w1, [x19, #0]
    ldp w2, w3, [x19, #8]
    ldp w4, w5, [x19, #16]
    ldp w6, w7, [x19, #24]
    ldp w8, w9, [x19, #32]
    ldp w10, w11, [x19, #40]
    ldp w12, w13, [x19, #48]
    ldr w14, [x19, #56]
    ldp q16, q17, [x19, #OFF_D]
    ldp q18, q19, [x19, #OFF_D + 32]
    ldp q20, q21, [x19, #OFF_D + 64]
    ldp q22, q23, [x19, #OFF_D + 96]
    ldp q24, q25, [x19, #OFF_D + 128]
    ldp q26, q27, [x19, #OFF_D + 160]
    ldp q28, q29, [x19, #OFF_D + 192]
    ldp q30, q31, [x19, #OFF_D + 224]
    ldr w17, [x19, #OFF_NZCV]
    msr nzcv, x17
    br x16
    .size jit_enter, . - jit_enter

    .p2align 4
    .global jit_exit
    .type jit_exit, %function
/* reached by a branch from translated code; the guest pc etc. are already stored */
jit_exit:
    stp w0, w1, [x19, #0]
    stp w2, w3, [x19, #8]
    stp w4, w5, [x19, #16]
    stp w6, w7, [x19, #24]
    stp w8, w9, [x19, #32]
    stp w10, w11, [x19, #40]
    stp w12, w13, [x19, #48]
    str w14, [x19, #56]
    stp q16, q17, [x19, #OFF_D]
    stp q18, q19, [x19, #OFF_D + 32]
    stp q20, q21, [x19, #OFF_D + 64]
    stp q22, q23, [x19, #OFF_D + 96]
    stp q24, q25, [x19, #OFF_D + 128]
    stp q26, q27, [x19, #OFF_D + 160]
    stp q28, q29, [x19, #OFF_D + 192]
    stp q30, q31, [x19, #OFF_D + 224]
    mrs x17, nzcv
    str w17, [x19, #OFF_NZCV]
    mrs x17, fpsr
    mov w16, #0x9f
    movk w16, #0x0800, lsl #16
    and w17, w17, w16
    ldr w16, [x19, #OFF_FPSR]
    orr w16, w16, w17
    str w16, [x19, #OFF_FPSR]
    ldr x17, [x19, #OFF_HOST_FPCR]
    msr fpcr, x17
    ldr w0, [x19, #OFF_EXIT_REASON]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x29, x30, [sp], #160
    ret
    .size jit_exit, . - jit_exit

    .section .note.GNU-stack, "", %progbits
