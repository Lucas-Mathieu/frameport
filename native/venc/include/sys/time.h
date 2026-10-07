/* Minimal stand-in for libc's <sys/time.h>: linux/videodev2.h includes it for struct timeval (v4l2_buffer) and
 * struct timespec (v4l2_event). There is no libc at build time (fp_venc is freestanding), so the two structs are
 * declared here with the aarch64 Linux layout (two 64-bit longs each). */
#ifndef FP_VENC_SYS_TIME_H
#define FP_VENC_SYS_TIME_H

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

#endif
