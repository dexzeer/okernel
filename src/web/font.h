#ifndef WEB_FONT_H
#define WEB_FONT_H

#include <stdint.h>
#include "surface.h"

// TrueType text for the web engine. Faces are embedded subsets of Noto
// Sans/Serif/Mono (+ a DejaVu symbols fallback), rasterized by our own
// outline parser and the AA rasterizer in raster.c. Glyphs are cached per
// (face, pixel size, glyph, quarter-pixel x phase).
//
// All metrics are LU (1/64 px). No hinting, no kerning, no shaping:
// one glyph per codepoint from the cmap, advance from hmtx.

#define WF_FAMILY_SANS  0
#define WF_FAMILY_SERIF 1
#define WF_FAMILY_MONO  2

struct wfont {
    uint8_t family;    // WF_FAMILY_*
    uint8_t bold;      // weight >= 600
    uint8_t italic;
    uint16_t px;       // font size in whole pixels (1..512)
};

struct wfmetrics {
    int32_t ascent;    // above baseline (positive)
    int32_t descent;   // below baseline (positive)
    int32_t line_gap;
    int32_t xheight;
    int32_t capheight;
};

void    wfont_init(void);
void    wfont_metrics(const struct wfont* f, struct wfmetrics* m);
int32_t wfont_advance(const struct wfont* f, uint32_t cp);
// Width of a UTF-8 run (no letter/word spacing).
int32_t wfont_measure(const struct wfont* f, const char* s, int len);
// Draw a UTF-8 run with its baseline at (x, y), both LU in surface space.
// letter_sp / word_sp (LU) are added after every glyph / every U+0020.
// Returns the advance consumed.
int32_t wfont_draw(struct wsurf* s, const struct wfont* f, int32_t x, int32_t y,
                   uint32_t argb, const char* str, int len,
                   int32_t letter_sp, int32_t word_sp);
// Drop every cached glyph (memory pressure / tests).
void    wfont_cache_flush(void);
// Cache stats for diagnostics.
void    wfont_cache_stats(int* glyphs, int* bytes);

#endif
