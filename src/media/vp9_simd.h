#ifndef VP9_SIMD_H
#define VP9_SIMD_H

#include <stddef.h>
#include <stdint.h>

// SSE2 kernels for VP9's 8-tap sub-pixel interpolation (the largest single cost of decoding a P frame). Samples are 16-bit
// (0..maxv). Like src/simd.c this file is built with -msse2 and is the only media code that touches XMM registers: call it
// from thread context only. vp9_simd_ok() says whether the CPU can run it; the callers keep their scalar loops otherwise.
// w must be a multiple of 4. `k` points at the 8 taps. src points at the sample for the FIRST tap's position of output 0
// (the caller subtracts 3 samples / 3 rows), so these read src[-0 .. w+6] (h) or rows 0..h+6 (v).
int  vp9_simd_ok(void);
void vp9_simd_filter_h(const uint16_t* src, ptrdiff_t ss, uint16_t* dst, ptrdiff_t ds, int w, int h, const int16_t* k, int maxv);
void vp9_simd_filter_v(const uint16_t* src, ptrdiff_t ss, uint16_t* dst, ptrdiff_t ds, int w, int h, const int16_t* k, int maxv);

#endif
