// cjk.c — CJK bitmap lookup for the okai blit path (see cjk.h).
// Glyph data is generated (src/cjk_font.h); this file is hand-written.
#include "cjk.h"
#include "cjk_font.h"

const uint8_t* cjk_glyph_for(uint32_t cp) {
    int lo = 0, hi = (int)CJK_FONT_N - 1;
    while (lo <= hi) {
        int m = lo + (hi - lo) / 2;
        uint32_t c = cjk_font[m].cp;
        if (c == cp) return cjk_font[m].px;
        if (c < cp) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}
