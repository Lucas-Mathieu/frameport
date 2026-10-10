/* grt_hc_raw(r7 value, a0, a1, a2, a3): a hypercall (SVC 0 with r7 = A32_HYPERCALL | n), result in r0. Out of line
 * because r7 is the Thumb frame pointer. */
    .syntax unified
    .arm
    .text
    .p2align 2
    .global grt_hc_raw
    .type grt_hc_raw, %function
grt_hc_raw:
    push {r7, lr}
    mov r7, r0
    mov r0, r1
    mov r1, r2
    mov r2, r3
    ldr r3, [sp, #8]
    svc 0
    pop {r7, pc}
    .size grt_hc_raw, . - grt_hc_raw
    .section .note.GNU-stack, "", %progbits
