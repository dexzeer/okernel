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

// Loop filter (spec 8.8.5) for one run of EIGHT sample lines along an edge: `q` points at q0 of the first line, `step` is the distance
// between samples across the edge (1 = a vertical edge), `along` the distance between the lines. fsize: 0 = 4-tap, 1 = up to 8, 2 = up to 16.
// The thresholds are already scaled to the bit depth. Bit-exact with the scalar filter in vp9_lf.c.
struct vp9_lf_p { int limit, blimit, thresh, one, lo, hi, off; };
void vp9_simd_lf_run(uint16_t* q, ptrdiff_t step, ptrdiff_t along, int fsize, const struct vp9_lf_p* P);


#endif
