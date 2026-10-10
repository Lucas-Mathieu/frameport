/* Native method trampolines: ART calls a32b_tramp_table + 8 * n with the AAPCS64 JNI arguments; slot n says which
 * guest function and shorty (natives.c). Every slot is "adr x16, . ; b common": common turns x16 into the slot
 * number, saves the argument registers and calls a32b_native_call(n, gpr[8], fpr[8], stack args). The result comes
 * back in x0 and is copied to d0 as well (float and double results use s0/d0). */
#define NTRAMP 4096

    .text
    .p2align 3
    .global a32b_tramp_table
    .hidden a32b_tramp_table
    .type a32b_tramp_table, %function
a32b_tramp_table:
    .rept NTRAMP
    adr x16, .
    b a32b_tramp_common
    .endr
    .size a32b_tramp_table, . - a32b_tramp_table

    .type a32b_tramp_common, %function
a32b_tramp_common:
    stp x29, x30, [sp, #-160]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp d0, d1, [sp, #80]
    stp d2, d3, [sp, #96]
    stp d4, d5, [sp, #112]
    stp d6, d7, [sp, #128]
    adrp x17, a32b_tramp_table
    add x17, x17, :lo12:a32b_tramp_table
    sub x0, x16, x17
    lsr x0, x0, #3
    add x1, sp, #16
    add x2, sp, #80
    add x3, sp, #160
    bl a32b_native_call
    fmov d0, x0
    ldp x29, x30, [sp], #160
    ret
    .size a32b_tramp_common, . - a32b_tramp_common

    .section .note.GNU-stack, "", %progbits
