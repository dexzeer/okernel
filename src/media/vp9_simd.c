#include "vp9_simd.h"
#include <emmintrin.h>
#ifdef KERNEL
#include "../fpu.h"
int vp9_simd_ok(void) { return fpu_has_sse2(); }
#else
int vp9_simd_ok(void) { return __builtin_cpu_supports("sse2"); }
#endif

// two taps as one 32-bit lane (lo | hi << 16), for pmaddwd
static inline __m128i tap_pair(int16_t a, int16_t b) { return _mm_set1_epi32((int)((uint16_t)a | ((uint32_t)(uint16_t)b << 16))); }

static inline __m128i finish8(__m128i lo, __m128i hi, __m128i rnd, __m128i zero, __m128i mx) {
    lo = _mm_srai_epi32(_mm_add_epi32(lo, rnd), 7);
    hi = _mm_srai_epi32(_mm_add_epi32(hi, rnd), 7);
    __m128i v = _mm_packs_epi32(lo, hi);                    // saturates to int16; the clamp below does the real work
    return _mm_min_epi16(_mm_max_epi16(v, zero), mx);
}

void vp9_simd_filter_h(const uint16_t* src, ptrdiff_t ss, uint16_t* dst, ptrdiff_t ds, int w, int h, const int16_t* k, int maxv) {
    const __m128i k01 = tap_pair(k[0], k[1]), k23 = tap_pair(k[2], k[3]), k45 = tap_pair(k[4], k[5]), k67 = tap_pair(k[6], k[7]);
    const __m128i rnd = _mm_set1_epi32(64), zero = _mm_setzero_si128(), mx = _mm_set1_epi16((short)maxv);
    for (int r = 0; r < h; r++) {
        const uint16_t* p = src + r * ss;
        uint16_t* o = dst + r * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m128i s0 = _mm_loadu_si128((const __m128i*)(p + x)), s1 = _mm_loadu_si128((const __m128i*)(p + x + 1));
            __m128i s2 = _mm_loadu_si128((const __m128i*)(p + x + 2)), s3 = _mm_loadu_si128((const __m128i*)(p + x + 3));
            __m128i s4 = _mm_loadu_si128((const __m128i*)(p + x + 4)), s5 = _mm_loadu_si128((const __m128i*)(p + x + 5));
            __m128i s6 = _mm_loadu_si128((const __m128i*)(p + x + 6)), s7 = _mm_loadu_si128((const __m128i*)(p + x + 7));
            __m128i lo = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s0, s1), k01), _mm_madd_epi16(_mm_unpacklo_epi16(s2, s3), k23)),
                                       _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s4, s5), k45), _mm_madd_epi16(_mm_unpacklo_epi16(s6, s7), k67)));
            __m128i hi = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(s0, s1), k01), _mm_madd_epi16(_mm_unpackhi_epi16(s2, s3), k23)),
                                       _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(s4, s5), k45), _mm_madd_epi16(_mm_unpackhi_epi16(s6, s7), k67)));
            _mm_storeu_si128((__m128i*)(o + x), finish8(lo, hi, rnd, zero, mx));
        }
        for (; x + 4 <= w; x += 4) {                         // 4 columns: exactly the samples needed (no read past the window)
            __m128i s0 = _mm_loadl_epi64((const __m128i*)(p + x)), s1 = _mm_loadl_epi64((const __m128i*)(p + x + 1));
            __m128i s2 = _mm_loadl_epi64((const __m128i*)(p + x + 2)), s3 = _mm_loadl_epi64((const __m128i*)(p + x + 3));
            __m128i s4 = _mm_loadl_epi64((const __m128i*)(p + x + 4)), s5 = _mm_loadl_epi64((const __m128i*)(p + x + 5));
            __m128i s6 = _mm_loadl_epi64((const __m128i*)(p + x + 6)), s7 = _mm_loadl_epi64((const __m128i*)(p + x + 7));
            __m128i a = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s0, s1), k01), _mm_madd_epi16(_mm_unpacklo_epi16(s2, s3), k23)),
                                      _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s4, s5), k45), _mm_madd_epi16(_mm_unpacklo_epi16(s6, s7), k67)));
            _mm_storel_epi64((__m128i*)(o + x), finish8(a, a, rnd, zero, mx));
        }
    }
}

void vp9_simd_filter_v(const uint16_t* src, ptrdiff_t ss, uint16_t* dst, ptrdiff_t ds, int w, int h, const int16_t* k, int maxv) {
    const __m128i k01 = tap_pair(k[0], k[1]), k23 = tap_pair(k[2], k[3]), k45 = tap_pair(k[4], k[5]), k67 = tap_pair(k[6], k[7]);
    const __m128i rnd = _mm_set1_epi32(64), zero = _mm_setzero_si128(), mx = _mm_set1_epi16((short)maxv);
    for (int r = 0; r < h; r++) {
        const uint16_t* p0 = src + r * ss;
        const uint16_t* p1 = p0 + ss; const uint16_t* p2 = p1 + ss; const uint16_t* p3 = p2 + ss;
        const uint16_t* p4 = p3 + ss; const uint16_t* p5 = p4 + ss; const uint16_t* p6 = p5 + ss; const uint16_t* p7 = p6 + ss;
        uint16_t* o = dst + r * ds;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m128i s0 = _mm_loadu_si128((const __m128i*)(p0 + x)), s1 = _mm_loadu_si128((const __m128i*)(p1 + x));
            __m128i s2 = _mm_loadu_si128((const __m128i*)(p2 + x)), s3 = _mm_loadu_si128((const __m128i*)(p3 + x));
            __m128i s4 = _mm_loadu_si128((const __m128i*)(p4 + x)), s5 = _mm_loadu_si128((const __m128i*)(p5 + x));
            __m128i s6 = _mm_loadu_si128((const __m128i*)(p6 + x)), s7 = _mm_loadu_si128((const __m128i*)(p7 + x));
            __m128i lo = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s0, s1), k01), _mm_madd_epi16(_mm_unpacklo_epi16(s2, s3), k23)),
                                       _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s4, s5), k45), _mm_madd_epi16(_mm_unpacklo_epi16(s6, s7), k67)));
            __m128i hi = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(s0, s1), k01), _mm_madd_epi16(_mm_unpackhi_epi16(s2, s3), k23)),
                                       _mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(s4, s5), k45), _mm_madd_epi16(_mm_unpackhi_epi16(s6, s7), k67)));
            _mm_storeu_si128((__m128i*)(o + x), finish8(lo, hi, rnd, zero, mx));
        }
        for (; x + 4 <= w; x += 4) {
            __m128i s0 = _mm_loadl_epi64((const __m128i*)(p0 + x)), s1 = _mm_loadl_epi64((const __m128i*)(p1 + x));
            __m128i s2 = _mm_loadl_epi64((const __m128i*)(p2 + x)), s3 = _mm_loadl_epi64((const __m128i*)(p3 + x));
            __m128i s4 = _mm_loadl_epi64((const __m128i*)(p4 + x)), s5 = _mm_loadl_epi64((const __m128i*)(p5 + x));
            __m128i s6 = _mm_loadl_epi64((const __m128i*)(p6 + x)), s7 = _mm_loadl_epi64((const __m128i*)(p7 + x));
            __m128i a = _mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s0, s1), k01), _mm_madd_epi16(_mm_unpacklo_epi16(s2, s3), k23)),
                                      _mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(s4, s5), k45), _mm_madd_epi16(_mm_unpacklo_epi16(s6, s7), k67)));
            _mm_storel_epi64((__m128i*)(o + x), finish8(a, a, rnd, zero, mx));
        }
    }
}
