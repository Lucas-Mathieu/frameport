/* RGB24 -> NV12 picture converter for fp_venc (pure computation, no syscalls). See native/venc/SPEC.md "Converter".
 * Built for aarch64 (NEON + scalar) and for x86_64 (scalar only; the host unit test loads it with ctypes). */
#ifndef FP_VENC_CONVERT_H
#define FP_VENC_CONVERT_H

#include <stdint.h>
#include <stddef.h>

/* Output size: height = min(max_h, src_h) (max_h <= 0 = src_h), width keeps the aspect; both rounded down to even,
 * both >= 2. */
void fp_out_size(int src_w, int src_h, int max_h, int *out_w, int *out_h);

/* Bytes of scratch memory fp_convert/fp_convert_scalar need for these sizes. */
size_t fp_scratch_size(int src_w, int src_h, int out_w, int out_h);

/* RGB24 (R,G,B bytes) -> NV12 (BT.709, limited range): box-average downscale to out_w x out_h (out_w <= src_w,
 * out_h <= src_h, both even), then out_w x out_h luma bytes and out_w x out_h/2 interleaved Cb,Cr bytes. */
void fp_convert_scalar(const uint8_t *src, int src_w, int src_h, int src_stride,
                       uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                       int out_w, int out_h, void *scratch);

/* Same result, byte for byte; NEON on aarch64, fp_convert_scalar elsewhere. */
void fp_convert(const uint8_t *src, int src_w, int src_h, int src_stride,
                uint8_t *y, int y_stride, uint8_t *uv, int uv_stride,
                int out_w, int out_h, void *scratch);

#endif
