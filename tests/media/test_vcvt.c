// vcvt: the SSE2 colour conversion and the scaler must give exactly the scalar results (and the scalar results exactly the
// formula the engine always used). Buffers are allocated to the exact size so ASan catches any read past a row end.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/media/vcvt.h"

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); if (fails > 20) exit(1); } } while (0)

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static uint32_t ref_px(int Y, int U, int V, const struct vcvt_cs* m) {
    int yy = Y - m->yoff, uu = U - 128, vv = V - 128;
    int base = m->ky * yy + 128;
    int r = (base + m->rv * vv) >> 8, g = (base - m->gu * uu - m->gv * vv) >> 8, b = (base + m->bu * uu) >> 8;
    return 0xFF000000u | (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
}

static void test_yuv16(void) {
    for (int iter = 0; iter < 400; iter++) {
        int w = 1 + (int)(rnd() % 70), h = 1 + (int)(rnd() % 9);
        int sx = (int)(rnd() & 1), sy = (int)(rnd() & 1);
        int sh = (int)(rnd() % 3) * 2;                              // 8, 10, 12 bit
        int cw = (w + sx) >> sx, ch = (h + sy) >> sy;
        uint16_t* py = malloc((size_t)w * h * 2); uint16_t* pu = malloc((size_t)cw * ch * 2); uint16_t* pv = malloc((size_t)cw * ch * 2);
        int maxv = (256 << sh) - 1;
        for (int i = 0; i < w * h; i++) py[i] = (uint16_t)(rnd() % (maxv + 1));
        for (int i = 0; i < cw * ch; i++) { pu[i] = (uint16_t)(rnd() % (maxv + 1)); pv[i] = (uint16_t)(rnd() % (maxv + 1)); }
        const uint16_t* pl[3] = { py, pu, pv };
        int st[3] = { w, cw, cw };
        struct vcvt_cs m;
        vcvt_matrix(&m, (int)(rnd() & 1), (int)(rnd() & 1));
        uint32_t* a = malloc((size_t)w * h * 4); uint32_t* b = malloc((size_t)w * h * 4);
        vcvt_force_scalar(0);
        vcvt_yuv_rows(a, (size_t)w, pl, st, w, 0, h, sx, sy, sh, &m);
        vcvt_force_scalar(1);
        vcvt_yuv_rows(b, (size_t)w, pl, st, w, 0, h, sx, sy, sh, &m);
        vcvt_force_scalar(0);
        CHECK(!memcmp(a, b, (size_t)w * h * 4), "simd != scalar (w=%d h=%d sx=%d sy=%d sh=%d)", w, h, sx, sy, sh);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t e = ref_px(py[y * w + x] >> sh, pu[(y >> sy) * cw + (x >> sx)] >> sh, pv[(y >> sy) * cw + (x >> sx)] >> sh, &m);
                if (b[y * w + x] != e) { CHECK(0, "scalar != formula at %d,%d", x, y); y = h; break; }
            }
        free(py); free(pu); free(pv); free(a); free(b);
    }
}

static void test_yuv8(void) {
    for (int iter = 0; iter < 300; iter++) {
        int w = 2 * (1 + (int)(rnd() % 40)), h = 2 * (1 + (int)(rnd() % 5));
        int cw = w / 2, ch = h / 2;
        uint8_t* py = malloc((size_t)w * h); uint8_t* pu = malloc((size_t)cw * ch); uint8_t* pv = malloc((size_t)cw * ch);
        for (int i = 0; i < w * h; i++) py[i] = (uint8_t)rnd();
        for (int i = 0; i < cw * ch; i++) { pu[i] = (uint8_t)rnd(); pv[i] = (uint8_t)rnd(); }
        const uint8_t* pl[3] = { py, pu, pv };
        int st[3] = { w, cw, cw };
        struct vcvt_cs m;
        vcvt_matrix(&m, (int)(rnd() & 1), (int)(rnd() & 1));
        uint32_t* a = malloc((size_t)w * h * 4); uint32_t* b = malloc((size_t)w * h * 4);
        vcvt_force_scalar(0);
        vcvt_yuv8_rows(a, (size_t)w, pl, st, w, 0, h, 1, 1, &m);
        vcvt_force_scalar(1);
        vcvt_yuv8_rows(b, (size_t)w, pl, st, w, 0, h, 1, 1, &m);
        vcvt_force_scalar(0);
        CHECK(!memcmp(a, b, (size_t)w * h * 4), "8-bit simd != scalar (w=%d h=%d)", w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (b[y * w + x] != ref_px(py[y * w + x], pu[(y >> 1) * cw + (x >> 1)], pv[(y >> 1) * cw + (x >> 1)], &m)) { CHECK(0, "8-bit scalar != formula"); y = h; break; }
        free(py); free(pu); free(pv); free(a); free(b);
    }
}

static void test_scale(void) {
    for (int iter = 0; iter < 400; iter++) {
        int sw = 1 + (int)(rnd() % 50), sh = 1 + (int)(rnd() % 30), dw = 1 + (int)(rnd() % 80), dh = 1 + (int)(rnd() % 60);
        uint32_t* src = malloc((size_t)sw * sh * 4);
        for (int i = 0; i < sw * sh; i++) src[i] = rnd() * 2654435761u;
        uint32_t* a = malloc((size_t)dw * dh * 4); uint32_t* b = malloc((size_t)dw * dh * 4);
        void* sc = malloc(vcvt_scale_scratch(sw, dw));
        vcvt_force_scalar(0);
        vcvt_scale(a, dw, dh, src, sw, sh, sc);
        vcvt_force_scalar(1);
        vcvt_scale(b, dw, dh, src, sw, sh, sc);
        vcvt_force_scalar(0);
        CHECK(!memcmp(a, b, (size_t)dw * dh * 4), "scale simd != scalar (%dx%d -> %dx%d)", sw, sh, dw, dh);
        free(src); free(a); free(b); free(sc);
    }
    // identity is an exact copy
    {
        int w = 37, h = 11;
        uint32_t* src = malloc((size_t)w * h * 4); uint32_t* o = malloc((size_t)w * h * 4); void* sc = malloc(vcvt_scale_scratch(w, w));
        for (int i = 0; i < w * h; i++) src[i] = rnd() * 2654435761u;
        vcvt_scale(o, w, h, src, w, h, sc);
        CHECK(!memcmp(src, o, (size_t)w * h * 4), "identity scale is not a copy");
        free(src); free(o); free(sc);
    }
    // a flat colour stays flat, up and down
    for (int k = 0; k < 2; k++) {
        int sw = k ? 90 : 17, sh = k ? 40 : 9, dw = k ? 31 : 70, dh = k ? 13 : 33;
        uint32_t* src = malloc((size_t)sw * sh * 4); uint32_t* o = malloc((size_t)dw * dh * 4); void* sc = malloc(vcvt_scale_scratch(sw, dw));
        for (int i = 0; i < sw * sh; i++) src[i] = 0xFF3A7BC1u;
        vcvt_scale(o, dw, dh, src, sw, sh, sc);
        int ok = 1;
        for (int i = 0; i < dw * dh; i++) if (o[i] != 0xFF3A7BC1u) ok = 0;
        CHECK(ok, "a flat image changed colour when scaled (%dx%d -> %dx%d)", sw, sh, dw, dh);
        free(src); free(o); free(sc);
    }
}

int main(void) {
    printf("simd %s\n", vcvt_simd_ok() ? "on" : "off");
    test_yuv16();
    test_yuv8();
    test_scale();
    printf(fails ? "VCVT TESTS FAIL (%d)\n" : "VCVT TESTS PASS\n", fails);
    return fails != 0;
}
