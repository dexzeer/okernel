#include "vcvt.h"
#include <emmintrin.h>
#ifdef KERNEL
#include "../fpu.h"
static int cpu_sse2(void) { return fpu_has_sse2(); }
#else
static int cpu_sse2(void) { return __builtin_cpu_supports("sse2"); }
#endif

static int force_scalar;
void vcvt_force_scalar(int on) { force_scalar = on; }
int vcvt_simd_ok(void) { return !force_scalar && cpu_sse2(); }

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void vcvt_matrix(struct vcvt_cs* m, int hd, int full_range) {
    if (hd) {
        if (full_range) { m->ky = 256; m->rv = 403; m->gu = 48; m->gv = 120; m->bu = 475; m->yoff = 0; }
        else { m->ky = 298; m->rv = 459; m->gu = 55; m->gv = 136; m->bu = 541; m->yoff = 16; }
    } else {
        if (full_range) { m->ky = 256; m->rv = 359; m->gu = 88; m->gv = 183; m->bu = 454; m->yoff = 0; }
        else { m->ky = 298; m->rv = 409; m->gu = 100; m->gv = 208; m->bu = 516; m->yoff = 16; }
    }
}

// ---- YUV -> ARGB ---------------------------------------------------------------------------------------------------------

static void row_scalar(uint32_t* o, const uint16_t* yr, const uint16_t* ur, const uint16_t* vr, int x0, int x1, int sx, int sh,
                       const struct vcvt_cs* m) {
    for (int x = x0; x < x1; x++) {
        int yy = (yr[x] >> sh) - m->yoff, uu = (ur[x >> sx] >> sh) - 128, vv = (vr[x >> sx] >> sh) - 128;
        int base = m->ky * yy + 128;
        int r = (base + m->rv * vv) >> 8, g = (base - m->gu * uu - m->gv * vv) >> 8, b = (base + m->bu * uu) >> 8;
        o[x] = 0xFF000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
    }
}

static inline __m128i pair16(int a, int b) { return _mm_set1_epi32((int)((uint16_t)a | ((uint32_t)(uint16_t)b << 16))); }

struct kset { __m128i rnd, zero, ones, cR, cB, cG1, cG2; };

static inline void kset_init(struct kset* k, const struct vcvt_cs* m) {
    k->rnd = _mm_set1_epi32(128);
    k->zero = _mm_setzero_si128();
    k->ones = _mm_set1_epi8((char)0xFF);
    k->cR = pair16(m->ky, m->rv);
    k->cB = pair16(m->ky, m->bu);
    k->cG1 = pair16(m->ky, -m->gu);
    k->cG2 = pair16(-m->gv, 0);
}

// 8 pixels from offset-corrected y/u/v (int16 lanes): ARGB to o[0..7]. madd keeps every product exact in 32 bits.
static inline void emit8(uint32_t* o, __m128i y, __m128i u, __m128i v, const struct kset* k) {
    __m128i yv_l = _mm_unpacklo_epi16(y, v), yv_h = _mm_unpackhi_epi16(y, v);
    __m128i yu_l = _mm_unpacklo_epi16(y, u), yu_h = _mm_unpackhi_epi16(y, u);
    __m128i vz_l = _mm_unpacklo_epi16(v, k->zero), vz_h = _mm_unpackhi_epi16(v, k->zero);
    __m128i r_l = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yv_l, k->cR), k->rnd), 8);
    __m128i r_h = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yv_h, k->cR), k->rnd), 8);
    __m128i b_l = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yu_l, k->cB), k->rnd), 8);
    __m128i b_h = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yu_h, k->cB), k->rnd), 8);
    __m128i g_l = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(yu_l, k->cG1), _mm_madd_epi16(vz_l, k->cG2)), k->rnd), 8);
    __m128i g_h = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(yu_h, k->cG1), _mm_madd_epi16(vz_h, k->cG2)), k->rnd), 8);
    __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(r_l, r_h), k->zero);
    __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(g_l, g_h), k->zero);
    __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(b_l, b_h), k->zero);
    __m128i bg = _mm_unpacklo_epi8(b8, g8), ra = _mm_unpacklo_epi8(r8, k->ones);
    _mm_storeu_si128((__m128i*)o, _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128((__m128i*)(o + 4), _mm_unpackhi_epi16(bg, ra));
}

// 16-bit samples; 8 pixels per step; returns the first x not done
static int row_sse2(uint32_t* o, const uint16_t* yr, const uint16_t* ur, const uint16_t* vr, int w, int sx, int sh, const struct vcvt_cs* m) {
    struct kset k;
    kset_init(&k, m);
    const __m128i cnt = _mm_cvtsi32_si128(sh);
    const __m128i yoff = _mm_set1_epi16((short)m->yoff), c128 = _mm_set1_epi16(128);
    int x = 0;
    for (; x + 8 <= w; x += 8) {
        __m128i y = _mm_sub_epi16(_mm_srl_epi16(_mm_loadu_si128((const __m128i*)(yr + x)), cnt), yoff);
        __m128i u, v;
        if (sx) {
            __m128i u4 = _mm_loadl_epi64((const __m128i*)(ur + (x >> 1))), v4 = _mm_loadl_epi64((const __m128i*)(vr + (x >> 1)));
            u = _mm_unpacklo_epi16(u4, u4);
            v = _mm_unpacklo_epi16(v4, v4);
        } else {
            u = _mm_loadu_si128((const __m128i*)(ur + x));
            v = _mm_loadu_si128((const __m128i*)(vr + x));
        }
        u = _mm_sub_epi16(_mm_srl_epi16(u, cnt), c128);
        v = _mm_sub_epi16(_mm_srl_epi16(v, cnt), c128);
        emit8(o + x, y, u, v, &k);
    }
    return x;
}

// 8-bit samples (H.264)
static int row8_sse2(uint32_t* o, const uint8_t* yr, const uint8_t* ur, const uint8_t* vr, int w, int sx, const struct vcvt_cs* m) {
    struct kset k;
    kset_init(&k, m);
    const __m128i yoff = _mm_set1_epi16((short)m->yoff), c128 = _mm_set1_epi16(128), zero = _mm_setzero_si128();
    int x = 0;
    for (; x + 8 <= w; x += 8) {
        __m128i y = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(yr + x)), zero), yoff);
        __m128i u, v;
        if (sx) {
            int u4, v4;
            __builtin_memcpy(&u4, ur + (x >> 1), 4);
            __builtin_memcpy(&v4, vr + (x >> 1), 4);
            __m128i uw = _mm_unpacklo_epi8(_mm_cvtsi32_si128(u4), zero), vw = _mm_unpacklo_epi8(_mm_cvtsi32_si128(v4), zero);
            u = _mm_unpacklo_epi16(uw, uw);
            v = _mm_unpacklo_epi16(vw, vw);
        } else {
            u = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(ur + x)), zero);
            v = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(vr + x)), zero);
        }
        emit8(o + x, y, _mm_sub_epi16(u, c128), _mm_sub_epi16(v, c128), &k);
    }
    return x;
}

static void row8_scalar(uint32_t* o, const uint8_t* yr, const uint8_t* ur, const uint8_t* vr, int x0, int x1, int sx, const struct vcvt_cs* m) {
    for (int x = x0; x < x1; x++) {
        int yy = yr[x] - m->yoff, uu = ur[x >> sx] - 128, vv = vr[x >> sx] - 128;
        int base = m->ky * yy + 128;
        int r = (base + m->rv * vv) >> 8, g = (base - m->gu * uu - m->gv * vv) >> 8, b = (base + m->bu * uu) >> 8;
        o[x] = 0xFF000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
    }
}

void vcvt_yuv_rows(uint32_t* dst, size_t dst_stride, const uint16_t* const plane[3], const int stride[3], int w, int y0, int y1,
                   int sx, int sy, int sh, const struct vcvt_cs* m) {
    int simd = vcvt_simd_ok();
    for (int y = y0; y < y1; y++) {
        const uint16_t* yr = plane[0] + (size_t)y * (size_t)stride[0];
        const uint16_t* ur = plane[1] + (size_t)(y >> sy) * (size_t)stride[1];
        const uint16_t* vr = plane[2] + (size_t)(y >> sy) * (size_t)stride[2];
        uint32_t* o = dst + (size_t)y * dst_stride;
        int x = simd ? row_sse2(o, yr, ur, vr, w, sx, sh, m) : 0;
        if (x < w) row_scalar(o, yr, ur, vr, x, w, sx, sh, m);
    }
}

void vcvt_yuv8_rows(uint32_t* dst, size_t dst_stride, const uint8_t* const plane[3], const int stride[3], int w, int y0, int y1,
                    int sx, int sy, const struct vcvt_cs* m) {
    int simd = vcvt_simd_ok();
    for (int y = y0; y < y1; y++) {
        const uint8_t* yr = plane[0] + (size_t)y * (size_t)stride[0];
        const uint8_t* ur = plane[1] + (size_t)(y >> sy) * (size_t)stride[1];
        const uint8_t* vr = plane[2] + (size_t)(y >> sy) * (size_t)stride[2];
        uint32_t* o = dst + (size_t)y * dst_stride;
        int x = simd ? row8_sse2(o, yr, ur, vr, w, sx, m) : 0;
        if (x < w) row8_scalar(o, yr, ur, vr, x, w, sx, m);
    }
}

// ---- bilinear scale ------------------------------------------------------------------------------------------------------

size_t vcvt_scale_scratch(int sw, int dw) { return ((size_t)sw + (size_t)dw) * sizeof(uint32_t) + 16; }

// (a*(256-f) + b*f) >> 8 on all four channels of two packed pixels at once (the channels sit in 16-bit lanes, so no carries cross)
static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t f) {
    uint32_t g = 256 - f;
    uint32_t rb = (((a & 0x00FF00FFu) * g + (b & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * g + ((b >> 8) & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    return rb | (ag << 8);
}

static void blend_rows(uint32_t* out, const uint32_t* a, const uint32_t* b, int n, uint32_t fy) {
    int i = 0;
    if (vcvt_simd_ok()) {
        const __m128i zero = _mm_setzero_si128();
        const __m128i wa = _mm_set1_epi16((short)(256 - fy)), wb = _mm_set1_epi16((short)fy);
        for (; i + 4 <= n; i += 4) {
            __m128i pa = _mm_loadu_si128((const __m128i*)(a + i)), pb = _mm_loadu_si128((const __m128i*)(b + i));
            __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(pa, zero), wa), _mm_mullo_epi16(_mm_unpacklo_epi8(pb, zero), wb)), 8);
            __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(pa, zero), wa), _mm_mullo_epi16(_mm_unpackhi_epi8(pb, zero), wb)), 8);
            _mm_storeu_si128((__m128i*)(out + i), _mm_packus_epi16(lo, hi));
        }
    }
    for (; i < n; i++) out[i] = lerp_px(a[i], b[i], fy);
}

void vcvt_scale(uint32_t* dst, int dw, int dh, const uint32_t* src, int sw, int sh, void* scratch) {
    uint32_t* row = (uint32_t*)scratch;
    uint32_t* xm = row + sw;                                   // per output column: source x (24 bits) << 8 | weight
    int32_t xstep = (int32_t)(((uint32_t)sw << 16) / (uint32_t)dw), ystep = (int32_t)(((uint32_t)sh << 16) / (uint32_t)dh);
    int32_t x0 = xstep / 2 - 32768, y0 = ystep / 2 - 32768;
    for (int x = 0; x < dw; x++) {
        int32_t p = x0 + x * xstep;
        if (p < 0) p = 0;
        int xi = p >> 16;
        uint32_t fx = (uint32_t)(p >> 8) & 255u;
        if (xi >= sw - 1) { xi = sw - 1; fx = 0; }
        xm[x] = ((uint32_t)xi << 8) | fx;
    }
    for (int y = 0; y < dh; y++) {
        int32_t p = y0 + y * ystep;
        if (p < 0) p = 0;
        int yi = p >> 16;
        uint32_t fy = (uint32_t)(p >> 8) & 255u;
        if (yi >= sh - 1) { yi = sh - 1; fy = 0; }
        const uint32_t* r0 = src + (size_t)yi * (size_t)sw;
        const uint32_t* rp = r0;
        if (fy) {
            blend_rows(row, r0, r0 + sw, sw, fy);
            rp = row;
        }
        uint32_t* o = dst + (size_t)y * (size_t)dw;
        for (int x = 0; x < dw; x++) {
            uint32_t e = xm[x];
            int xi = (int)(e >> 8);
            uint32_t fx = e & 255u;
            o[x] = fx ? lerp_px(rp[xi], rp[xi + 1], fx) : rp[xi];
        }
    }
}
