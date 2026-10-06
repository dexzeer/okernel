// Host test: render text samples through the web engine's font stack into a
// PPM (tests/web/out/font.ppm). Visual check + a few numeric sanity checks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web/font.h"
#include "web/raster.h"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

static void save_ppm(const char* path, struct wsurf* s) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int i = 0; i < s->w * s->h; i++) {
        uint32_t p = s->px[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(void) {
    struct wsurf s;
    s.w = 1000; s.h = 760; s.stride = s.w;
    s.px = (uint32_t*)malloc(sizeof(uint32_t) * s.w * s.h);
    for (int i = 0; i < s.w * s.h; i++) s.px[i] = 0xFFFFFF;
    ws_reset_clip(&s);
    wfont_init();

    struct wfont f = { WF_FAMILY_SANS, 0, 0, 16 };
    const char* pangram = "The quick brown fox jumps over the lazy dog. 0123456789";
    int32_t w16 = wfont_measure(&f, pangram, (int)strlen(pangram));
    printf("sans16 pangram width = %d px\n", w16 / 64);
    CHECK(w16 / 64 > 380 && w16 / 64 < 480, "16px sans pangram width plausible (Chrome ~430px)");
    struct wfmetrics m;
    wfont_metrics(&f, &m);
    printf("sans16 ascent=%d descent=%d gap=%d xh=%d\n", m.ascent, m.descent, m.line_gap, m.xheight);
    CHECK(m.ascent / 64 == 17 && m.descent / 64 == 4, "16px Noto Sans ascent/descent");

    int y = 30;
    int sizes[] = { 11, 12, 13, 14, 16, 18, 24, 32, 48 };
    for (int i = 0; i < 9; i++) {
        f.px = (uint16_t)sizes[i];
        wfont_draw(&s, &f, 10 * 64, y * 64, 0xFF202122, pangram, (int)strlen(pangram), 0, 0);
        y += sizes[i] * 14 / 10 + 4;
    }
    struct wfont v[] = {
        { WF_FAMILY_SANS, 1, 0, 20 }, { WF_FAMILY_SANS, 0, 1, 20 }, { WF_FAMILY_SANS, 1, 1, 20 },
        { WF_FAMILY_SERIF, 0, 0, 20 }, { WF_FAMILY_SERIF, 1, 0, 20 }, { WF_FAMILY_SERIF, 0, 1, 20 },
        { WF_FAMILY_SERIF, 1, 1, 20 }, { WF_FAMILY_MONO, 0, 0, 20 }, { WF_FAMILY_MONO, 1, 1, 20 },
    };
    const char* names[] = { "Sans Bold", "Sans Italic", "Sans Bold Italic", "Serif Regular",
                            "Serif Bold", "Serif Italic", "Serif Bold Italic (synth)",
                            "Mono Regular", "Mono Bold Italic (synth)" };
    for (int i = 0; i < 9; i++) {
        char line[160];
        snprintf(line, sizeof line, "%s — Ünïcödé “quotes” café Ελληνικά Русский ½ → ★ ✓", names[i]);
        wfont_draw(&s, &v[i], 10 * 64, y * 64, 0xFF0645AD, line, (int)strlen(line), 0, 0);
        y += 30;
    }
    f.px = 20; f.family = WF_FAMILY_SANS; f.bold = 0; f.italic = 0;
    const char* cjk = "CJK fallback: 日本語 한국어 中文  missing: \xF0\x9F\x98\x80";
    wfont_draw(&s, &f, 10 * 64, y * 64, 0xFF000000, cjk, (int)strlen(cjk), 0, 0);
    y += 34;
    // white on dark
    ws_fill_rect(&s, 0, y, 1000, 50, 0xFF1A2E4D);
    wfont_draw(&s, &f, 10 * 64, (y + 32) * 64, 0xFFFFFFFF, "White text on a dark band — okai", 33, 0, 0);

    // rounded rect + ellipse through the same rasterizer
    struct wraster r; wr_init(&r);
    int32_t rad[4] = { WR_FIX(12), WR_FIX(12), WR_FIX(12), WR_FIX(12) };
    wr_rrect(&r, WR_FIX(700), WR_FIX(20), WR_FIX(250), WR_FIX(90), rad);
    struct { struct wsurf* s; uint32_t c; } ctx = { &s, 0xFF3366CC };
    void span(void* c, int yy, int x, int len, const uint8_t* cov);
    wr_fill(&r, 0, 0, s.w, s.h, 0, span, &ctx);
    wr_reset(&r);
    wr_ellipse(&r, WR_FIX(825), WR_FIX(180), WR_FIX(60), WR_FIX(40));
    ctx.c = 0xC0E04040;
    wr_fill(&r, 0, 0, s.w, s.h, 0, span, &ctx);

    system("mkdir -p tests/web/out");
    save_ppm("tests/web/out/font.ppm", &s);
    int gl, by; wfont_cache_stats(&gl, &by);
    printf("glyph cache: %d glyphs, %d bytes\n", gl, by);
    printf(fails ? "FONT TESTS FAIL\n" : "FONT TESTS PASS\n");
    return fails != 0;
}

void span(void* c, int yy, int x, int len, const uint8_t* cov) {
    struct { struct wsurf* s; uint32_t c; }* k = c;
    for (int i = 0; i < len; i++) {
        uint8_t m = cov[i];
        ws_mask(k->s, x + i, yy, 1, 1, &m, 1, k->c);
    }
}
