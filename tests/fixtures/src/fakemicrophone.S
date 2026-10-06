// Test fixture for patches/frame/ovr_microphone.py: OVRPort's unchecked ovr_Microphone_GetOutputBufferMaxSize
// (same instructions as OVRPort 1.2.5's libovrplatformloader.so).
// Build: aarch64-linux-android29-clang -shared -fPIC -nostdlib fakemicrophone.S -o ../libfakemicrophone_arm64.so
    .text
    .globl AAudioStream_getFramesPerBurst
    .type AAudioStream_getFramesPerBurst, %function
AAudioStream_getFramesPerBurst:
    ldr w0, [x0]
    ret
    .size AAudioStream_getFramesPerBurst, .-AAudioStream_getFramesPerBurst

    .globl ovr_Microphone_GetOutputBufferMaxSize
    .type ovr_Microphone_GetOutputBufferMaxSize, %function
ovr_Microphone_GetOutputBufferMaxSize:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x0, [x0, #0x18]
    bl AAudioStream_getFramesPerBurst
    sxtw x0, w0
    ldp x29, x30, [sp], #16
    ret
    .size ovr_Microphone_GetOutputBufferMaxSize, .-ovr_Microphone_GetOutputBufferMaxSize
