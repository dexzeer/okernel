#ifndef MEDIA_VCVT_H
#define MEDIA_VCVT_H

#include <stddef.h>
#include <stdint.h>

// Video colour conversion and scaling for the media engine: planar YUV (16-bit samples, any bit depth) -> ARGB, and a bilinear
// ARGB scaler. SSE2 where the CPU has it (exact: the same integer arithmetic as the scalar code, so both give identical pixels),
// plain C otherwise. Pure functions on caller-owned buffers, safe to run on an AP (no globals, no logging).

struct vcvt_cs {
    int ky, rv, gu, gv, bu;     // matrix in 8.8 fixed point: R = (ky*Y + rv*V + 128) >> 8 ...
    int yoff;                   // 16 for limited range, 0 for full
};

// BT.709 (hd) or BT.601, limited or full range
void vcvt_matrix(struct vcvt_cs* m, int hd, int full_range);

// Rows [y0, y1) of the image into dst (width w, row stride dst_stride pixels, row y0 stored at dst + (y0 - y0) ... i.e. dst points
// at row 0 of the frame). sx/sy = chroma subsampling shifts, sh = bit depth - 8.
void vcvt_yuv_rows(uint32_t* dst, size_t dst_stride, const uint16_t* const plane[3], const int stride[3], int w, int y0, int y1,
                   int sx, int sy, int sh, const struct vcvt_cs* m);

// The same for 8-bit planes (H.264).
void vcvt_yuv8_rows(uint32_t* dst, size_t dst_stride, const uint8_t* const plane[3], const int stride[3], int w, int y0, int y1,
                    int sx, int sy, const struct vcvt_cs* m);

// Bilinear scale (pixel centres aligned) of an sw x sh ARGB image to dw x dh. `scratch` must hold vcvt_scale_scratch() bytes.
size_t vcvt_scale_scratch(int sw, int dw);
void vcvt_scale(uint32_t* dst, int dw, int dh, const uint32_t* src, int sw, int sh, void* scratch);

int vcvt_simd_ok(void);                 // 1 when the SSE2 paths are in use
void vcvt_force_scalar(int on);         // tests: take the scalar paths

#endif
