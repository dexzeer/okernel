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

// ---- loop filter: eight sample lines at once -------------------------------------------------------------------------------
// The scalar code (vp9_lf.c, spec 8.8.5) decides per sample line: filterMask, hev, flat, flat2, then applies the narrow filter, the
// 8-sample or the 16-sample wide filter. Here those decisions become lane masks and all three results are computed for the lanes
// that need them and merged, with exactly the same integer arithmetic (every intermediate fits 16 bits: the wide sums stay below
// 16 * 4095 and are handled as unsigned lanes).

static inline __m128i lf_absd(__m128i a, __m128i b) { return _mm_or_si128(_mm_subs_epu16(a, b), _mm_subs_epu16(b, a)); }
static inline __m128i lf_sel(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }
static inline __m128i lf_clip(__m128i v, __m128i lo, __m128i hi) { return _mm_min_epi16(_mm_max_epi16(v, lo), hi); }

static inline void lf_transpose8(__m128i* r) {
    __m128i a0 = _mm_unpacklo_epi16(r[0], r[1]), a1 = _mm_unpackhi_epi16(r[0], r[1]);
    __m128i a2 = _mm_unpacklo_epi16(r[2], r[3]), a3 = _mm_unpackhi_epi16(r[2], r[3]);
    __m128i a4 = _mm_unpacklo_epi16(r[4], r[5]), a5 = _mm_unpackhi_epi16(r[4], r[5]);
    __m128i a6 = _mm_unpacklo_epi16(r[6], r[7]), a7 = _mm_unpackhi_epi16(r[6], r[7]);
    __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
    __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
    __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
    __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
    r[0] = _mm_unpacklo_epi64(b0, b4); r[1] = _mm_unpackhi_epi64(b0, b4);
    r[2] = _mm_unpacklo_epi64(b1, b5); r[3] = _mm_unpackhi_epi64(b1, b5);
    r[4] = _mm_unpacklo_epi64(b2, b6); r[5] = _mm_unpackhi_epi64(b2, b6);
    r[6] = _mm_unpacklo_epi64(b3, b7); r[7] = _mm_unpackhi_epi64(b3, b7);
}

// T[0..15] = p7..p0, q0..q7 (one lane per sample line); only the taps the filter size needs are valid. Updates T in place.
static void lf_core(__m128i* T, int fsize, const struct vp9_lf_p* P) {
    const __m128i lim = _mm_set1_epi16((short)P->limit), blim = _mm_set1_epi16((short)P->blimit), thr = _mm_set1_epi16((short)P->thresh);
    const __m128i one = _mm_set1_epi16((short)P->one);
    __m128i p3 = T[4], p2 = T[5], p1 = T[6], p0 = T[7], q0 = T[8], q1 = T[9], q2 = T[10], q3 = T[11];
    __m128i ap1p0 = lf_absd(p1, p0), aq1q0 = lf_absd(q1, q0);
    __m128i bad = _mm_or_si128(_mm_cmpgt_epi16(lf_absd(p3, p2), lim), _mm_cmpgt_epi16(lf_absd(p2, p1), lim));
    bad = _mm_or_si128(bad, _mm_or_si128(_mm_cmpgt_epi16(ap1p0, lim), _mm_cmpgt_epi16(aq1q0, lim)));
    bad = _mm_or_si128(bad, _mm_or_si128(_mm_cmpgt_epi16(lf_absd(q2, q1), lim), _mm_cmpgt_epi16(lf_absd(q3, q2), lim)));
    __m128i edge = _mm_add_epi16(_mm_slli_epi16(lf_absd(p0, q0), 1), _mm_srli_epi16(lf_absd(p1, q1), 1));
    bad = _mm_or_si128(bad, _mm_cmpgt_epi16(edge, blim));
    __m128i mask = _mm_xor_si128(bad, _mm_set1_epi16(-1));                 // filterMask
    if (_mm_movemask_epi8(mask) == 0) return;
    __m128i hev = _mm_or_si128(_mm_cmpgt_epi16(ap1p0, thr), _mm_cmpgt_epi16(aq1q0, thr));
    __m128i flat = _mm_setzero_si128(), flat2 = _mm_setzero_si128();
    if (fsize >= 1) {
        __m128i nf = _mm_or_si128(_mm_cmpgt_epi16(ap1p0, one), _mm_cmpgt_epi16(aq1q0, one));
        nf = _mm_or_si128(nf, _mm_or_si128(_mm_cmpgt_epi16(lf_absd(p2, p0), one), _mm_cmpgt_epi16(lf_absd(q2, q0), one)));
        nf = _mm_or_si128(nf, _mm_or_si128(_mm_cmpgt_epi16(lf_absd(p3, p0), one), _mm_cmpgt_epi16(lf_absd(q3, q0), one)));
        flat = _mm_andnot_si128(nf, mask);
        if (fsize >= 2 && _mm_movemask_epi8(flat)) {
            __m128i nf2 = _mm_or_si128(_mm_cmpgt_epi16(lf_absd(T[0], p0), one), _mm_cmpgt_epi16(lf_absd(T[15], q0), one));
            nf2 = _mm_or_si128(nf2, _mm_or_si128(_mm_cmpgt_epi16(lf_absd(T[1], p0), one), _mm_cmpgt_epi16(lf_absd(T[14], q0), one)));
            nf2 = _mm_or_si128(nf2, _mm_or_si128(_mm_cmpgt_epi16(lf_absd(T[2], p0), one), _mm_cmpgt_epi16(lf_absd(T[13], q0), one)));
            nf2 = _mm_or_si128(nf2, _mm_or_si128(_mm_cmpgt_epi16(lf_absd(T[3], p0), one), _mm_cmpgt_epi16(lf_absd(T[12], q0), one)));
            flat2 = _mm_andnot_si128(nf2, flat);
        }
    }
    __m128i out[16];
    for (int i = 0; i < 16; i++) out[i] = T[i];

    // narrow filter (8.8.5.2) where the mask holds and the samples are not flat
    __m128i m_narrow = _mm_andnot_si128(flat, mask);
    if (_mm_movemask_epi8(m_narrow)) {
        const __m128i lo = _mm_set1_epi16((short)P->lo), hi = _mm_set1_epi16((short)P->hi), off = _mm_set1_epi16((short)P->off);
        __m128i ps1 = _mm_sub_epi16(p1, off), ps0 = _mm_sub_epi16(p0, off), qs0 = _mm_sub_epi16(q0, off), qs1 = _mm_sub_epi16(q1, off);
        __m128i f = _mm_and_si128(hev, lf_clip(_mm_sub_epi16(ps1, qs1), lo, hi));
        __m128i d = _mm_sub_epi16(qs0, ps0);
        f = lf_clip(_mm_add_epi16(f, _mm_add_epi16(_mm_add_epi16(d, d), d)), lo, hi);
        __m128i f1 = _mm_srai_epi16(lf_clip(_mm_add_epi16(f, _mm_set1_epi16(4)), lo, hi), 3);
        __m128i f2 = _mm_srai_epi16(lf_clip(_mm_add_epi16(f, _mm_set1_epi16(3)), lo, hi), 3);
        __m128i nq0 = _mm_add_epi16(lf_clip(_mm_sub_epi16(qs0, f1), lo, hi), off);
        __m128i np0 = _mm_add_epi16(lf_clip(_mm_add_epi16(ps0, f2), lo, hi), off);
        __m128i f3 = _mm_srai_epi16(_mm_add_epi16(f1, _mm_set1_epi16(1)), 1);
        __m128i nq1 = _mm_add_epi16(lf_clip(_mm_sub_epi16(qs1, f3), lo, hi), off);
        __m128i np1 = _mm_add_epi16(lf_clip(_mm_add_epi16(ps1, f3), lo, hi), off);
        __m128i m_outer = _mm_andnot_si128(hev, m_narrow);                 // p1 / q1 only change where there is no high edge variance
        out[8] = lf_sel(m_narrow, nq0, out[8]);
        out[7] = lf_sel(m_narrow, np0, out[7]);
        out[9] = lf_sel(m_outer, nq1, out[9]);
        out[6] = lf_sel(m_outer, np1, out[6]);
    }

    // wide filters (8.8.5.3): F[i] = round2(v[i] + sum_{j=-n..n} v[clip(i + j)], log2)
    __m128i m_w8 = _mm_andnot_si128(flat2, flat);                          // flat, but not flat2 (or a size-8 filter)
    if (_mm_movemask_epi8(m_w8)) {
        const int n = 3, lo_i = -4;
        __m128i sum = _mm_setzero_si128();
        for (int j = -n; j <= n; j++) { int p = -n + j; if (p < lo_i) p = lo_i; sum = _mm_add_epi16(sum, T[p + 8]); }
        const __m128i rnd = _mm_set1_epi16(4);
        for (int i = -n; i < n; i++) {
            __m128i r = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(sum, T[i + 8]), rnd), 3);
            out[i + 8] = lf_sel(m_w8, r, out[i + 8]);
            int add = i + n + 1; if (add > n) add = n;
            int sub = i - n; if (sub < lo_i) sub = lo_i;
            sum = _mm_add_epi16(sum, _mm_sub_epi16(T[add + 8], T[sub + 8]));
        }
    }
    if (fsize >= 2 && _mm_movemask_epi8(flat2)) {
        const int n = 7, lo_i = -8;
        __m128i sum = _mm_setzero_si128();
        for (int j = -n; j <= n; j++) { int p = -n + j; if (p < lo_i) p = lo_i; sum = _mm_add_epi16(sum, T[p + 8]); }
        const __m128i rnd = _mm_set1_epi16(8);
        for (int i = -n; i < n; i++) {
            __m128i r = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(sum, T[i + 8]), rnd), 4);
            out[i + 8] = lf_sel(flat2, r, out[i + 8]);
            int add = i + n + 1; if (add > n) add = n;
            int sub = i - n; if (sub < lo_i) sub = lo_i;
            sum = _mm_add_epi16(sum, _mm_sub_epi16(T[add + 8], T[sub + 8]));
        }
    }
    for (int i = 0; i < 16; i++) T[i] = out[i];
}

void vp9_simd_lf_run(uint16_t* q, ptrdiff_t step, ptrdiff_t along, int fsize, const struct vp9_lf_p* P) {
    __m128i T[16];
    if (step != 1) {                                                        // horizontal edge: the taps are rows, the eight lines lie along a row
        int a = fsize >= 2 ? 0 : 4, b = fsize >= 2 ? 15 : 11;
        for (int i = a; i <= b; i++) T[i] = _mm_loadu_si128((const __m128i*)(q + (ptrdiff_t)(i - 8) * step));
        lf_core(T, fsize, P);
        int sa = fsize >= 2 ? 1 : fsize == 1 ? 5 : 6, sb = fsize >= 2 ? 14 : fsize == 1 ? 10 : 9;
        for (int i = sa; i <= sb; i++) _mm_storeu_si128((__m128i*)(q + (ptrdiff_t)(i - 8) * step), T[i]);
        return;
    }
    // vertical edge: each of the eight lines is a row; the taps run along it, so the block is transposed in and out
    __m128i A[8], B[8];
    if (fsize >= 2) {
        for (int r = 0; r < 8; r++) { A[r] = _mm_loadu_si128((const __m128i*)(q + r * along - 8)); B[r] = _mm_loadu_si128((const __m128i*)(q + r * along)); }
        lf_transpose8(A); lf_transpose8(B);
        for (int c = 0; c < 8; c++) { T[c] = A[c]; T[8 + c] = B[c]; }
        lf_core(T, fsize, P);
        for (int c = 0; c < 8; c++) { A[c] = T[c]; B[c] = T[8 + c]; }
        lf_transpose8(A); lf_transpose8(B);
        for (int r = 0; r < 8; r++) { _mm_storeu_si128((__m128i*)(q + r * along - 8), A[r]); _mm_storeu_si128((__m128i*)(q + r * along), B[r]); }
    } else {
        for (int r = 0; r < 8; r++) A[r] = _mm_loadu_si128((const __m128i*)(q + r * along - 4));
        lf_transpose8(A);
        for (int c = 0; c < 8; c++) T[4 + c] = A[c];
        lf_core(T, fsize, P);
        for (int c = 0; c < 8; c++) A[c] = T[4 + c];
        lf_transpose8(A);
        for (int r = 0; r < 8; r++) _mm_storeu_si128((__m128i*)(q + r * along - 4), A[r]);
    }
}
