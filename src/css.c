#include "css.h"
#include "string.h"

// ---- helpers ------------------------------------------------------------

static int css_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

// Copy src lowercased into dst (dst size assumed >= 32).
static void css_lower(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < 31) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        dst[i] = c;
        i++;
    }
    dst[i] = 0;
}

// Does space-separated class list contain `name`?
static int css_has_class(const char* cls_list, const char* name) {
    int i = 0, n = (int)strlen(name);
    while (cls_list[i]) {
        // skip separators
        while (cls_list[i] == ' ' || cls_list[i] == '\t') i++;
        if (!cls_list[i]) break;
        if (strncmp(cls_list + i, name, n) == 0 &&
            (cls_list[i + n] == ' ' || cls_list[i + n] == '\t' || cls_list[i + n] == 0))
            return 1;
        while (cls_list[i] && cls_list[i] != ' ' && cls_list[i] != '\t') i++;
    }
    return 0;
}

static int css_is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// ---- color quantization to the 16-color VGA text palette ----------------

static const uint8_t VGA[16][3] = {
    {0,0,0},     {0,0,170},   {0,170,0},   {0,170,170},
    {170,0,0},   {170,0,170}, {170,85,0},  {170,170,170},
    {85,85,85},  {85,85,255}, {85,255,85}, {85,255,255},
    {255,85,85}, {255,85,255},{255,255,85},{255,255,255},
};

static uint8_t css_quantize(uint8_t r, uint8_t g, uint8_t b) {
    int best = 0, best_d = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int dr = r - VGA[i][0], dg = g - VGA[i][1], db = b - VGA[i][2];
        int d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

// Named colors are handled by the NAMED table next to css_parse_color_rgb.

// Parse a color value; returns palette index 0-15 or -1, and (if rgb_out is
// non-NULL) the exact 0xRRGGBB color. Named colors use the web-standard RGB
// values, not the VGA approximations.
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

struct named_color { const char* name; uint8_t idx; uint32_t rgb; };
static const struct named_color NAMED[] = {
    {"black",        0,  0x000000}, {"blue",         1,  0x0000FF},
    {"green",        2,  0x008000}, {"cyan",         3,  0x00FFFF},
    {"aqua",         3,  0x00FFFF}, {"red",          4,  0xFF0000},
    {"magenta",      5,  0xFF00FF}, {"purple",       5,  0x800080},
    {"brown",        6,  0xA52A2A}, {"gray",         7,  0x808080},
    {"grey",         7,  0x808080}, {"lightgray",    7,  0xD3D3D3},
    {"lightgrey",    7,  0xD3D3D3}, {"darkgray",     8,  0xA9A9A9},
    {"darkgrey",     8,  0xA9A9A9}, {"lightblue",    9,  0xADD8E6},
    {"lightgreen",  10,  0x90EE90}, {"lightcyan",   11,  0xE0FFFF},
    {"lightred",    12,  0xFF5555}, {"crimson",     12,  0xDC143C},
    {"pink",        13,  0xFFC0CB}, {"lightmagenta",13,  0xFF77FF},
    {"yellow",      14,  0xFFFF00}, {"white",       15,  0xFFFFFF},
};

static int css_parse_color_rgb(const char* v, uint32_t* rgb_out) {
    // Functional notation: rgb(r,g,b) / rgba(r,g,b,a). Components are
    // 0-255 integers with optional % suffix (percent of 255); alpha is
    // 0-1 float (0 = fully transparent -> reject, anything else opaque —
    // we have no compositing, and a translucent color painted solid is
    // closer than unstyled). Values clamp to [0,255].
    if ((v[0] == 'r' || v[0] == 'R') && (v[1] == 'g' || v[1] == 'G') &&
        (v[2] == 'b' || v[2] == 'B')) {
        int is_rgba = (v[3] == 'a' || v[3] == 'A');
        int p = is_rgba ? 4 : 3;
        if (v[p] != '(') return -1;
        p++;
        int comp[4]; int nc = 0;
        int alpha_nz = 1; // alpha nonzero (opaque) unless proven literal 0
        int ok = 1;
        int want = is_rgba ? 4 : 3;
        while (ok && nc < want) {
            while (v[p] == ' ') p++; // (decl values pre-strip spaces; belt+suspenders)
            int num = 0, digits = 0, pct = 0, frac_nz = 0, frac_seen = 0;
            while (v[p] >= '0' && v[p] <= '9') { num = num * 10 + (v[p] - '0'); p++; digits++; }
            if (v[p] == '.') { // fractional part (alpha "0.5", sloppy "255.0")
                frac_seen = 1; p++;
                while (v[p] >= '0' && v[p] <= '9') { if (v[p] != '0') frac_nz = 1; p++; }
            }
            if (v[p] == '%') { pct = 1; p++; }
            if (!digits && !frac_seen) { ok = 0; break; }
            if (pct) num = num * 255 / 100;
            if (num < 0) num = 0;
            if (num > 255) num = 255;
            comp[nc] = num;
            if (nc == 3 && num == 0 && !frac_nz) alpha_nz = 0; // literal 0 / 0% / 0.0
            nc++;
            while (v[p] == ' ') p++;
            if (nc < (is_rgba ? 4 : 3)) {
                if (v[p] != ',') { ok = 0; break; }
                p++;
            }
        }
        if (!ok || nc != (is_rgba ? 4 : 3) || v[p] != ')' || v[p+1] != 0) return -1;
        if (is_rgba && !alpha_nz) return -1; // fully transparent: paint nothing
        {
            uint32_t rgb = ((uint32_t)comp[0] << 16) | ((uint32_t)comp[1] << 8) | (uint32_t)comp[2];
            if (rgb_out) *rgb_out = rgb;
            return css_quantize((uint8_t)comp[0], (uint8_t)comp[1], (uint8_t)comp[2]);
        }
    }
    if (v[0] == '#') {
        const char* h = v + 1;
        int n = 0;
        while (is_hex(h[n])) n++;
        if (n == 3) {
            int r = hexval(h[0]) * 17, g = hexval(h[1]) * 17, b = hexval(h[2]) * 17;
            if (rgb_out) *rgb_out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            return css_quantize((uint8_t)r,(uint8_t)g,(uint8_t)b);
        } else if (n == 6) {
            int r = hexval(h[0])*16 + hexval(h[1]);
            int g = hexval(h[2])*16 + hexval(h[3]);
            int b = hexval(h[4])*16 + hexval(h[5]);
            if (rgb_out) *rgb_out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            return css_quantize((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        return -1;
    }
    for (int k = 0; k < (int)(sizeof(NAMED)/sizeof(NAMED[0])); k++) {
        if (css_ieq(v, NAMED[k].name)) {
            if (rgb_out) *rgb_out = NAMED[k].rgb;
            return NAMED[k].idx;
        }
    }
    return -1;
}
static int css_parse_color(const char* v) {
    return css_parse_color_rgb(v, 0);
}

// ---- declaration application (with cascade specificity) ------------------

enum { P_COLOR=0, P_BG, P_SIZE, P_WEIGHT, P_ALIGN, P_MT, P_MB, P_DISPLAY,
        P_ML, P_MR, P_PT, P_PR, P_PB, P_PL, P_BW, P_BC, P_BS, P_W, P_H, P_TD,
        P_GCOLS, P_FDIR, P_MAXW, P_FLOAT, P_MINW, P_TT, P_VIS, P_LST, P_WS,
        P_POS, P_LEFT, P_RIGHT, P_TOP, P_BOTTOM, P_N };

static int css_prop_index(const char* p) {
    if (css_ieq(p,"color")) return P_COLOR;
    if (css_ieq(p,"background") || css_ieq(p,"background-color") ||
        css_ieq(p,"background-image")) return P_BG;
    if (css_ieq(p,"font-size")) return P_SIZE;
    if (css_ieq(p,"font-weight")) return P_WEIGHT;
    if (css_ieq(p,"text-align")) return P_ALIGN;
    if (css_ieq(p,"margin")) return P_MT;          // shorthand -> all four sides
    if (css_ieq(p,"margin-top")) return P_MT;
    if (css_ieq(p,"margin-bottom")) return P_MB;
    if (css_ieq(p,"margin-left")) return P_ML;
    if (css_ieq(p,"margin-right")) return P_MR;
    if (css_ieq(p,"padding")) return P_PT;         // shorthand -> all four sides
    if (css_ieq(p,"padding-top")) return P_PT;
    if (css_ieq(p,"padding-bottom")) return P_PB;
    if (css_ieq(p,"padding-left")) return P_PL;
    if (css_ieq(p,"padding-right")) return P_PR;
    if (css_ieq(p,"border")) return P_BW;          // shorthand -> width+style+color
    if (css_ieq(p,"border-width")) return P_BW;
    if (css_ieq(p,"border-color")) return P_BC;
    if (css_ieq(p,"border-style")) return P_BS;
    if (css_ieq(p,"width")) return P_W;
    if (css_ieq(p,"height")) return P_H;
    if (css_ieq(p,"text-decoration")) return P_TD;
    if (css_ieq(p,"display")) return P_DISPLAY;
    if (css_ieq(p,"grid-template-columns")) return P_GCOLS;
    if (css_ieq(p,"flex-direction")) return P_FDIR;
    if (css_ieq(p,"max-width")) return P_MAXW;
    if (css_ieq(p,"float")) return P_FLOAT;
    if (css_ieq(p,"min-width")) return P_MINW;
    if (css_ieq(p,"text-transform")) return P_TT;
    if (css_ieq(p,"visibility")) return P_VIS;
    if (css_ieq(p,"list-style-type") || css_ieq(p,"list-style")) return P_LST;
    if (css_ieq(p,"white-space")) return P_WS;
    if (css_ieq(p,"position")) return P_POS;
    if (css_ieq(p,"left")) return P_LEFT;
    if (css_ieq(p,"right")) return P_RIGHT;
    if (css_ieq(p,"top")) return P_TOP;
    if (css_ieq(p,"bottom")) return P_BOTTOM;
    return -1;
}

// Length parse with units. Returns px for px/bare/em/rem/%; sets *is_pct
// when the value was a percentage (of pct_base). em/rem resolve against
// 16px; % resolves against pct_base (caller picks an honest base — 16 for
// font-size, content width for widths). Fractional parts round to nearest.
// Unknown/non-numeric values yield 0.
static int css_len(const char* v, int pct_base, int* is_pct) {
    if (is_pct) *is_pct = 0;
    while (*v == ' ' || *v == '\t') v++;
    int neg = 0;
    if (*v == '-') { neg = 1; v++; } else if (*v == '+') v++;
    int whole = 0, digits = 0;
    while (*v >= '0' && *v <= '9') { whole = whole * 10 + (*v++ - '0'); digits++; }
    int frac = 0, fdiv = 1;
    if (*v == '.') {
        v++;
        while (*v >= '0' && *v <= '9' && fdiv < 1000) {
            frac = frac * 10 + (*v - '0'); fdiv *= 10; v++;
        }
    }
    if (!digits && fdiv == 1) return 0; // no number at all ("auto", "medium")
    // value in 16ths of a px-em: whole*16 + frac*16/fdiv
    int x16 = whole * 16 + (frac * 16 + fdiv / 2) / fdiv;
    int px;
    if (*v == 'e' || *v == 'E') { // em
        if (v[1] != 'm' && v[1] != 'M') return (neg ? -((x16 + 8) / 16) : (x16 + 8) / 16);
        px = x16; // em × 16px base: x16 IS px (16ths of 16px)
    } else if ((*v == 'r' || *v == 'R') && (v[1] == 'e' || v[1] == 'E') &&
               (v[2] == 'm' || v[2] == 'M')) {
        px = x16; // rem: same 16px root
    } else if (*v == '%') {
        if (is_pct) *is_pct = 1;
        px = x16 * pct_base / 1600; // (x16/16) * base / 100
    } else {
        px = (x16 + 8) / 16; // px or bare number
    }
    return neg ? -px : px;
}

static int css_px(const char* v) {
    return css_len(v, 16, 0);
}

// font-size: px/em/% plus absolute keywords (xx-small..xx-large) and
// smaller/larger against the 16px default. Returns px.
static int css_font_size(const char* v) {
    if (css_ieq(v,"xx-small")) return 9;
    if (css_ieq(v,"x-small")) return 10;
    if (css_ieq(v,"small")) return 12;
    if (css_ieq(v,"medium")) return 16;
    if (css_ieq(v,"large")) return 20;
    if (css_ieq(v,"x-large")) return 24;
    if (css_ieq(v,"xx-large")) return 32;
    if (css_ieq(v,"smaller")) return 12;
    if (css_ieq(v,"larger")) return 20;
    return css_len(v, 16, 0);
}

// Parse up to `max` whitespace/comma-separated lengths from a shorthand
// value (e.g. "10px 2em" -> {10,32}). Units resolve against base16 (see
// css_len); a leading '-' negates. Returns the count parsed.
static int css_px_list(const char* v, int* out, int max) {
    int n = 0;
    while (*v && n < max) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        if (!*v) break;
        if (!((*v >= '0' && *v <= '9') || *v == '-' || *v == '+' || *v == '.')) break;
        out[n++] = css_len(v, 16, 0);
        if (*v == '-' || *v == '+') v++;
        while ((*v >= '0' && *v <= '9') || *v == '.') v++;
        while ((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') || *v == '%') v++;
    }
    return n;
}

// Split a margin shorthand into side words (handles "0 auto"). Each word
// is classified: "auto" -> auto=1 (px value ignored), else css_len.
// Returns the word count (0 when empty).
static int css_margin_words(const char* v, int* px, int* auto_f, int max) {
    int n = 0;
    while (*v && n < max) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        if (!*v) break;
        // word span
        const char* w = v;
        while (*v && *v != ' ' && *v != '\t' && *v != ',') v++;
        int wl = (int)(v - w);
        char word[24]; int wi = 0;
        for (int k = 0; k < wl && wi < 23; k++) word[wi++] = w[k];
        word[wi] = 0;
        if ((word[0] == 'a' || word[0] == 'A') &&
            (word[1] == 'u' || word[1] == 'U') &&
            (word[2] == 't' || word[2] == 'T') &&
            (word[3] == 'o' || word[3] == 'O') && word[4] == 0) {
            px[n] = 0; auto_f[n] = 1; n++;
        } else if ((word[0] >= '0' && word[0] <= '9') || word[0] == '-' ||
                   word[0] == '+' || word[0] == '.') {
            px[n] = css_len(word, 16, 0); auto_f[n] = 0; n++;
        } else break;
    }
    return n;
}

// Local substring search (freestanding kernel has no strstr).
static const char* css_strstr(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return hay;
    int nl = 0; while (needle[nl]) nl++;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i+j] == needle[j]) j++;
        if (j == nl) return hay + i;
    }
    return 0;
}

// Strip a trailing "!important" (case-insensitive) from a parsed value.
// Returns 1 and truncates when present. Values keep single spaces, so the
// marker looks like "red !important" — the space before '!' is trimmed too
// ("red " would otherwise match no color/keyword).
static int css_take_important(char* val) {
    int L = 0; while (val[L]) L++;
    static const char imp[] = "!important";
    if (L < 10) return 0;
    for (int k = 0; k < 10; k++) {
        char c = val[L - 10 + k], w = imp[k];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != w) return 0;
    }
    L -= 10;
    while (L > 0 && (val[L-1] == ' ' || val[L-1] == '\t')) L--;
    val[L] = 0;
    return 1;
}

// Apply one declaration if it wins. `win` tracks per-prop winning
// specificity, `wimp` per-prop winning importance (!important beats any
// normal declaration; ties fall back to specificity/source order).
static void css_apply(struct css_style* out, const char* prop, const char* val,
                      int spec, int important, int* win, int* wimp) {
    int idx = css_prop_index(prop);
    if (idx < 0) return;
    if (important < wimp[idx]) return;
    if (important == wimp[idx] && spec < win[idx]) return;   // a more specific/earlier-equal rule owns it
    wimp[idx] = important;
    win[idx] = spec;

    switch (idx) {
    case P_COLOR: {
        uint32_t rgb;
        int c = css_parse_color_rgb(val, &rgb);
        if (c >= 0) { out->has_fg = 1; out->fg = (uint8_t)c;
                      out->has_fg_rgb = 1; out->fg_rgb = rgb; }
        break;
    }
    case P_BG: {
        // Canonical compact gradients from the splitter ("!g<dir><c0><c1>"):
        // decode directly, no scanning.
        if (val[0] == '!' && val[1] == 'g' && val[2] >= '0' && val[2] <= '3') {
            int hx = 0;
            uint32_t c[2] = {0, 0};
            int ok = 1;
            for (int cc = 0; cc < 2 && ok; cc++) {
                uint32_t v = 0;
                for (int k = 0; k < 6; k++) {
                    char ch = val[3 + cc * 6 + k];
                    int d = -1;
                    if (ch >= '0' && ch <= '9') d = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                    else { ok = 0; break; }
                    v = (v << 4) | (uint32_t)d;
                }
                c[cc] = v;
            }
            if (ok) {
                int dir = val[2] - '0';
                out->has_bg = 1; out->bg = (uint8_t)0; out->has_bg_rgb = 1;
                out->bg_rgb = c[0];
                out->bg_grad = (dir == 1) ? 3 : 2;
                out->bg_c1 = c[1];
                break;
            }
            // fall through to normal parsing on malformed canonical
        }
        // linear-gradient(...) backgrounds: parse direction + first/last
        // stops (middle stops dropped -- documented approximation). The band
        // painter interpolates per row/column; bg_rgb doubles as the solid
        // fallback (used when the gradient can't apply).
        // bg_grad: 0 = solid, 2 = vertical (top->bottom), 3 = horizontal.
        const char* lg = css_strstr(val, "linear-gradient(");
        if (lg) {
            const char* q = lg + 16;
            int dir = 0, flip = 0, have_dir = 0;
            while (*q == ' ' || *q == '\t') q++;
            if (q[0] == 't' && q[1] == 'o' && (q[2] == ' ' || q[2] == '\t')) {
                // "to <side>[ <side>]" (a second side is accepted and ignored
                // -- true diagonals degrade to the dominant axis).
                q += 3;
                for (int k = 0; k < 2; k++) {
                    while (*q == ' ' || *q == '\t') q++;
                    if ((q[0]=='r'||q[0]=='R') && (q[1]=='i'||q[1]=='I')) { dir = 1; q += 5; }
                    else if ((q[0]=='l'||q[0]=='L') && (q[1]=='e'||q[1]=='E')) { dir = 1; flip = 1; q += 4; }
                    else if ((q[0]=='t'||q[0]=='T') && (q[1]=='o'||q[1]=='O')) { dir = 0; flip = 1; q += 3; }
                    else if (q[0]=='b'||q[0]=='B') { dir = 0; q += 6; }
                    else break;
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q == ',') break;
                }
                have_dir = 1;
            } else if ((*q >= '0' && *q <= '9') || *q == '-' || *q == '+') {
                // <angle>deg quantized to the nearest cardinal.
                int neg = 0, deg = 0;
                if (*q == '-') { neg = 1; q++; } else if (*q == '+') q++;
                while (*q >= '0' && *q <= '9') { deg = deg * 10 + (*q - '0'); q++; }
                if (neg) deg = -deg;
                deg %= 360; if (deg < 0) deg += 360;
                dir = ((deg >= 45 && deg < 135) || (deg >= 225 && deg < 315)) ? 1 : 0;
                flip = (deg >= 135 && deg < 315) ? 1 : 0;
                have_dir = 1;
            }
            if (have_dir) {
                while (*q && *q != ',') q++;
                if (*q == ',') q++;
            }
            // first stop, then the last comma-separated component.
            char cbuf[40]; int ci = 0;
            uint32_t c0 = 0, c1 = 0;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != ',' && *q != ')' && ci < 39) cbuf[ci++] = *q++;
            cbuf[ci] = 0;
            for (int k = 0; cbuf[k]; k++)
                if (cbuf[k] == ' ') { cbuf[k] = 0; break; }
            if (css_parse_color_rgb(cbuf, &c0) < 0) break;
            {
                const char* last = 0;
                const char* t = q;
                while (*t && *t != ')') { if (*t == ',') last = t + 1; t++; }
                if (last) {
                    while (*last == ' ' || *last == '\t') last++;
                    ci = 0;
                    while (*last && *last != ',' && *last != ')' && ci < 39)
                        cbuf[ci++] = *last++;
                    cbuf[ci] = 0;
                    for (int k = 0; cbuf[k]; k++)
                        if (cbuf[k] == ' ') { cbuf[k] = 0; break; }
                    if (css_parse_color_rgb(cbuf, &c1) < 0) c1 = c0;
                } else c1 = c0;
            }
            out->has_bg = 1; out->bg = (uint8_t)0; out->has_bg_rgb = 1;
            out->bg_rgb = flip ? c1 : c0;
            out->bg_grad = dir ? 3 : 2;
            out->bg_c1 = flip ? c0 : c1;
            break;
        }
        {
            uint32_t rgb;
            int c = css_parse_color_rgb(val, &rgb);
            if (c >= 0) { out->has_bg = 1; out->bg = (uint8_t)c;
                          out->has_bg_rgb = 1; out->bg_rgb = rgb;
                          out->bg_grad = 0; }
        }
        break;
    }
    case P_SIZE:
        out->has_size = 1; out->font_size = css_font_size(val);
        break;
    case P_WEIGHT:
        out->has_bold = 1;
        out->bold = (css_ieq(val,"bold") || css_ieq(val,"700") || css_ieq(val,"600"));
        break;
    case P_ALIGN:
        out->has_align = 1;
        if (css_ieq(val,"center")) out->align = CSS_ALIGN_CENTER;
        else if (css_ieq(val,"right")) out->align = CSS_ALIGN_RIGHT;
        else out->align = CSS_ALIGN_LEFT;
        break;
    case P_MT: { // `margin` shorthand or `margin-top`
        int v[4], a[4]; int n = css_margin_words(val, v, a, 4);
        if (!n) break;
        if (n == 1) { v[1] = v[2] = v[3] = v[0]; a[1] = a[2] = a[3] = a[0]; }
        else if (n == 2) { v[2] = v[0]; v[3] = v[1]; a[2] = a[0]; a[3] = a[1]; }
        else if (n == 3) { v[3] = v[1]; a[3] = a[1]; }
        out->has_mt = 1; out->margin_top    = v[0];
        out->has_mr = 1; out->margin_right  = v[1]; out->mr_auto = a[1];
        out->has_mb = 1; out->margin_bottom = v[2];
        out->has_ml = 1; out->margin_left   = v[3]; out->ml_auto = a[3];
        break;
    }
    case P_MB: out->has_mb = 1; out->margin_bottom = css_px(val); break;
    case P_ML:
        if (css_ieq(val, "auto")) { out->ml_auto = 1; }
        else { out->has_ml = 1; out->margin_left = css_px(val); out->ml_auto = 0; }
        break;
    case P_MR:
        if (css_ieq(val, "auto")) { out->mr_auto = 1; }
        else { out->has_mr = 1; out->margin_right = css_px(val); out->mr_auto = 0; }
        break;
    case P_PT: { // `padding` shorthand or `padding-top`
        int v[4]; int n = css_px_list(val, v, 4);
        if (!n) break;
        if (n == 1) { v[1] = v[2] = v[3] = v[0]; }
        else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
        else if (n == 3) { v[3] = v[1]; }
        out->has_pt = 1; out->padding_top    = v[0];
        out->has_pr = 1; out->padding_right  = v[1];
        out->has_pb = 1; out->padding_bottom = v[2];
        out->has_pl = 1; out->padding_left   = v[3];
        break;
    }
    case P_PB: out->has_pb = 1; out->padding_bottom = css_px(val); break;
    case P_PL: out->has_pl = 1; out->padding_left   = css_px(val); break;
    case P_PR: out->has_pr = 1; out->padding_right  = css_px(val); break;
    case P_BW: { // `border` shorthand or `border-width`
        int w;
        if (css_ieq(val, "thin")) w = 1;
        else if (css_ieq(val, "medium")) w = 3;
        else if (css_ieq(val, "thick")) w = 5;
        else w = css_px(val);
        if (w > 0) { out->has_bw = 1; out->border_width = w; }
        else if (w == 0 && (val[0] == '0')) { out->has_bw = 1; out->border_width = 0; }
        if (css_strstr(val, "solid") || css_strstr(val, "dotted") || css_strstr(val, "dashed")) {
            out->has_bs = 1; out->border_style = 1;
        }
        const char* h = css_strstr(val, "#");
        if (h) { uint32_t bc; if (css_parse_color_rgb(h, &bc) >= 0) {
                     out->has_bc = 1; out->border_color = bc; } }
        break;
    }
    case P_BC: { uint32_t bc; if (css_parse_color_rgb(val, &bc) >= 0) {
                     out->has_bc = 1; out->border_color = bc; } break; }
    case P_BS: out->has_bs = 1; out->border_style = css_ieq(val, "none") ? 0 : 1; break;
    case P_W: {
        int is_pct = 0;
        int w = css_len(val, 100, &is_pct);
        if (is_pct) {
            // Percent of the containing block: the parser has no layout
            // info, so stash the percent; the renderer resolves it against
            // the live content width (clamped 0-100 here).
            if (w < 0) w = 0;
            if (w > 100) w = 100;
            out->has_wpct = 1; out->wpct = w;
            out->has_w = 0;
        } else if (w > 0) {
            out->has_w = 1; out->width = w;
            out->has_wpct = 0;
        }
        break;
    }
    case P_H: {
        // Height % needs a containing-block height we never have (single
        // pass, unbounded doc) -> ignore; px works as a hint.
        int is_pct = 0;
        int h = css_len(val, 16, &is_pct);
        if (!is_pct && h > 0) { out->has_h = 1; out->height = h; }
        break;
    }
    case P_TD:
        out->has_td = 1;
        out->td_ul = (css_strstr(val, "underline") != 0) ? 1 : 0;
        break;
    case P_DISPLAY:
        out->has_display = 1;
        if (css_ieq(val,"none")) out->display = CSS_DISPLAY_NONE;
        else if (css_ieq(val,"inline")) out->display = CSS_DISPLAY_INLINE;
        else if (css_ieq(val,"inline-block")) out->display = CSS_DISPLAY_INLINE_BLOCK;
        else if (css_ieq(val,"flex") || css_ieq(val,"inline-flex")) out->display = CSS_DISPLAY_FLEX;
        else if (css_ieq(val,"grid") || css_ieq(val,"inline-grid")) out->display = CSS_DISPLAY_GRID;
        else if (css_ieq(val,"table")) out->display = CSS_DISPLAY_TABLE;
        else if (css_ieq(val,"table-row")) out->display = CSS_DISPLAY_TABLE_ROW;
        else if (css_ieq(val,"table-cell")) out->display = CSS_DISPLAY_TABLE_CELL;
        else out->display = CSS_DISPLAY_BLOCK;
        break;
    case P_GCOLS:
        out->has_gcols = 1;
        { int i = 0; while (val[i] && i < 79) { out->grid_cols[i] = val[i]; i++; } out->grid_cols[i] = 0; }
        break;
    case P_FDIR:
        out->has_fdir = 1;
        out->flex_dir = css_strstr(val, "column") ? 1 : 0;
        break;
    case P_MAXW:
        out->has_maxw = 1;
        out->max_width = css_len(val, 0, 0);
        break;
    case P_FLOAT:
        out->has_float = 1;
        if (css_ieq(val, "left")) out->float_side = CSS_FLOAT_LEFT;
        else if (css_ieq(val, "right")) out->float_side = CSS_FLOAT_RIGHT;
        else out->float_side = CSS_FLOAT_NONE;
        break;
    case P_MINW:
        out->has_minw = 1;
        out->min_width = css_len(val, 0, 0);
        break;
    case P_TT:
        out->has_tt = 1;
        if (css_ieq(val, "uppercase")) out->tt_mode = 1;
        else if (css_ieq(val, "lowercase")) out->tt_mode = 2;
        else if (css_ieq(val, "capitalize")) out->tt_mode = 3;
        else out->tt_mode = 0;
        break;
    case P_VIS:
        out->has_vis = 1;
        out->vis_hide = (css_ieq(val, "hidden") || css_ieq(val, "collapse")) ? 1 : 0;
        break;
    case P_LST:
        out->has_lst = 1;
        if (css_ieq(val, "none")) out->lst_type = 1;
        else if (css_ieq(val, "disc")) out->lst_type = 2;
        else if (css_ieq(val, "circle")) out->lst_type = 3;
        else if (css_ieq(val, "square")) out->lst_type = 4;
        else if (css_ieq(val, "decimal")) out->lst_type = 5;
        else if (css_ieq(val, "lower-alpha") || css_ieq(val, "lower-latin")) out->lst_type = 6;
        else if (css_ieq(val, "upper-alpha") || css_ieq(val, "upper-latin")) out->lst_type = 7;
        else if (css_ieq(val, "lower-roman")) out->lst_type = 8;
        else if (css_ieq(val, "upper-roman")) out->lst_type = 9;
        else out->lst_type = 0;
        break;
    case P_WS:
        out->has_ws = 1;
        if (css_ieq(val, "pre") || css_ieq(val, "pre-wrap")) out->ws_mode = 2;
        else if (css_ieq(val, "nowrap")) out->ws_mode = 1;
        else out->ws_mode = 0;
        break;
    case P_POS:
        out->has_pos = 1;
        if (css_ieq(val, "relative")) out->pos_mode = 1;
        else if (css_ieq(val, "absolute")) out->pos_mode = 2;
        else if (css_ieq(val, "fixed")) out->pos_mode = 3;
        else if (css_ieq(val, "sticky")) out->pos_mode = 1; // sticky degrades to relative
        else out->pos_mode = 0;
        break;
    case P_LEFT:
        if (!css_ieq(val, "auto")) { out->has_left = 1; out->left = css_len(val, 0, 0); }
        break;
    case P_RIGHT:
        if (!css_ieq(val, "auto")) { out->has_right = 1; out->right = css_len(val, 0, 0); }
        break;
    case P_TOP:
        if (!css_ieq(val, "auto")) { out->has_top = 1; out->top = css_len(val, 0, 0); }
        break;
    case P_BOTTOM:
        if (!css_ieq(val, "auto")) { out->has_bottom = 1; out->bottom = css_len(val, 0, 0); }
        break;
    }
}

void css_style_defaults(struct css_style* s) {
    memset(s, 0, sizeof(*s));
    s->display = CSS_DISPLAY_INLINE; // block decided by token type in renderer
}

// Built-in UA rules; parsed once, matched at negative specificity so every
// author rule outranks them. `g_ua_n == 0` means "not built yet" (empty sheet
// would just re-parse, which is harmless).
static struct css_rule g_ua_rules[CSS_UA_MAX_RULES];
static int g_ua_n = 0;

// ---- parser -------------------------------------------------------------

// CSS whitespace (declared here; definition below).
static int css_isws(char c);

// Parse one compound selector into `c` (tag / #id / .class parts). Returns the
// number of chars consumed, or -1 if nothing matched. `comb` is prefilled.
static int css_parse_compound(const char* s, struct css_compound* c) {
    c->tag[0] = 0; c->id[0] = 0; c->ncls = 0;
    int consumed = 0;
    int any = 0;
    // optional tag or '*'
    if (*s == '*') {
        s++; consumed++; any = 1;
    } else if (css_is_ident(*s)) {
        int i = 0;
        while (css_is_ident(*s) && i < 23) c->tag[i++] = *s++;
        c->tag[i] = 0; css_lower(c->tag, c->tag);
        consumed += i; any = 1;
    }
    // id / classes in any order
    while (*s == '#' || *s == '.') {
        char sep = *s++;
        consumed++;
        char buf[24]; int i = 0;
        while (css_is_ident(*s) && i < 23) buf[i++] = *s++;
        buf[i] = 0; css_lower(buf, buf);
        consumed += i; any = 1;
        if (sep == '#') { strncpy(c->id, buf, 31); c->id[31] = 0; }
        else if (c->ncls < 4) strncpy(c->cls[c->ncls++], buf, 23);
    }
    return any ? consumed : -1;
}

// Parse a full complex selector ("div.a > ul li") into the rule's chain,
// stored right-to-left (chain[0] = subject). Returns 0 on failure.
static int css_parse_selector(const char* s, struct css_rule* r) {
    r->nchain = 0;
    // Split into compounds while tracking combinators. Build left-to-right
    // into a temp array, then reverse.
    struct css_compound tmp[CSS_MAX_COMPOUND];
    int n = 0;
    int comb = CSS_COMB_NONE;
    while (*s) {
        while (css_isws(*s)) s++;
        if (!*s) break;
        if (*s == '>') { comb = CSS_COMB_CHILD; s++; while (css_isws(*s)) s++; }
        struct css_compound c;
        c.comb = (n == 0) ? CSS_COMB_NONE : comb;
        int used = css_parse_compound(s, &c);
        if (used < 0) { // not a compound (e.g. pseudo ":hover"): skip token
            while (*s && !css_isws(*s) && *s != '>' && *s != ',') s++;
            continue;
        }
        if (n >= CSS_MAX_COMPOUND) break;
        tmp[n++] = c;
        s += used;
        // Skip pseudo-classes/elements (":hover", "::before") and attribute
        // selectors we do not support; they end this compound's identity.
        while (*s == ':' || *s == '[' || *s == '.') {
            if (*s == '.') break; // a real class belongs to the next parse
            while (*s && !css_isws(*s) && *s != '>' && *s != ',') {
                if (*s == '[') { int d = 1; s++; while (*s && d) { if (*s=='[') d++; else if (*s==']') d--; s++; } }
                else if (*s == ':') { s++; while (*s == ':') s++; while (css_is_ident(*s)) s++; }
                else break;
            }
        }
        comb = CSS_COMB_DESCEND;
    }
    if (n == 0) return 0;
    // Reverse: subject first.
    for (int i = 0; i < n; i++) {
        r->chain[i] = tmp[n - 1 - i];
        // The combinator stored in tmp relates a compound to its predecessor
        // on the LEFT. After reversing, chain[i].comb must describe how
        // chain[i-1] (the descendant) connects to chain[i] (the ancestor).
    }
    r->nchain = n;
    // Rebuild combinators: chain[0] = none; chain[k] uses the connector that
    // was between tmp[n-1-(k)] and the one to its left.
    for (int k = 0; k < n; k++) {
        int t = n - 1 - k;              // index in tmp of chain[k]
        r->chain[k].comb = (t == 0) ? CSS_COMB_NONE : tmp[t].comb;
    }
    return 1;
}

static int css_compute_specificity(struct css_rule* r) {
    int id = 0, cl = 0, tg = 0;
    for (int i = 0; i < r->nchain; i++) {
        if (r->chain[i].id[0]) id++;
        cl += r->chain[i].ncls;
        if (r->chain[i].tag[0]) tg++;
    }
    return id*100 + cl*10 + tg;
}

// CSS whitespace inside declarations (spaces, tabs, newlines around
// a multiline rule all separate tokens the same way).
static int css_isws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\v';
}

// Compact a linear-gradient(...) value to canonical form "!g<dir><c0><c1>"
// (dir digit + two 6-hex colors, 16 chars) so it survives the 40-char decl
// value cap. Full-length gradients would otherwise truncate mid-stops and
// parse to wrong colors. Only hex/rgb()/named stops are recognized;
// anything else leaves val alone. Bounds are explicit [v0,v1) — the source
// is NOT NUL-terminated. Returns 1 if compacted.
static int css_compact_gradient(const char* css, int v0, int v1, char* val) {
    static const char lg[] = "linear-gradient(";
    int at = -1;
    for (int i = v0; i + 15 < v1; i++) {
        int k = 0;
        while (k < 16 && i + k < v1 && css[i + k] == lg[k]) k++;
        if (k == 16) { at = i + 16; break; }
    }
    if (at < 0) return 0;
    const char* q = css + at;
    const char* hi = css + v1;
    int dir = 0, flip = 0, have_dir = 0;
    while (q < hi && (*q == ' ' || *q == '\t')) q++;
    if (hi - q >= 3 && q[0] == 't' && q[1] == 'o' && (q[2] == ' ' || q[2] == '\t')) {
        q += 3;
        for (int k = 0; k < 2; k++) {
            while (q < hi && (*q == ' ' || *q == '\t')) q++;
            if (hi - q >= 2 && (q[0]=='r'||q[0]=='R') && (q[1]=='i'||q[1]=='I')) { dir = 1; q += 5; }
            else if (hi - q >= 2 && (q[0]=='l'||q[0]=='L') && (q[1]=='e'||q[1]=='E')) { dir = 1; flip = 1; q += 4; }
            else if (hi - q >= 2 && (q[0]=='t'||q[0]=='T') && (q[1]=='o'||q[1]=='O')) { dir = 0; flip = 1; q += 3; }
            else if (q < hi && (q[0]=='b'||q[0]=='B')) { dir = 0; q += 6; }
            else break;
            while (q < hi && (*q == ' ' || *q == '\t')) q++;
            if (q < hi && *q == ',') break;
        }
        have_dir = 1;
    } else if (q < hi && ((*q >= '0' && *q <= '9') || *q == '-' || *q == '+')) {
        int neg = 0, deg = 0;
        if (*q == '-') { neg = 1; q++; } else if (*q == '+') q++;
        while (q < hi && *q >= '0' && *q <= '9') { deg = deg * 10 + (*q - '0'); q++; }
        if (neg) deg = -deg;
        deg %= 360; if (deg < 0) deg += 360;
        dir = ((deg >= 45 && deg < 135) || (deg >= 225 && deg < 315)) ? 1 : 0;
        flip = (deg >= 135 && deg < 315) ? 1 : 0;
        have_dir = 1;
    }
    if (have_dir) {
        while (q < hi && *q != ',') q++;
        if (q < hi && *q == ',') q++;
    }
    uint32_t cols[8]; int nc = 0;
    while (q < hi && nc < 8) {
        while (q < hi && (*q == ' ' || *q == '\t' || *q == ',')) q++;
        if (q >= hi || *q == ')') break;
        if (*q == '#') {
            int nd = 0;
            while (q + 1 + nd < hi && nd < 6) {
                char c = q[1 + nd];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) break;
                nd++;
            }
            if (nd == 3 || nd == 6) {
                char tmp[8]; int ti = 0;
                if (nd == 3) {
                    for (int k = 0; k < 3; k++) { tmp[ti++] = q[1+k]; tmp[ti++] = q[1+k]; }
                } else {
                    for (int k = 0; k < 6; k++) tmp[ti++] = q[1+k];
                }
                tmp[ti] = 0;
                uint32_t v = 0;
                for (int k = 0; k < 6; k++) {
                    char c = tmp[k];
                    int d = (c >= '0' && c <= '9') ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                    v = (v << 4) | (uint32_t)d;
                }
                cols[nc++] = v;
                q += 1 + nd;
                continue;
            }
            q++;
        } else if (*q == 'r' || *q == 'R') {
            const char* s = q;
            while (q < hi && *q != '(') q++;
            if (q < hi && *q == '(') {
                int depth = 0;
                const char* e2 = q;
                while (e2 < hi) {
                    if (*e2 == '(') depth++;
                    else if (*e2 == ')') { depth--; if (!depth) break; }
                    e2++;
                }
                char tmp[48]; int ti = 0;
                for (const char* pp = s; pp <= e2 && ti < 47; pp++) tmp[ti++] = *pp;
                tmp[ti] = 0;
                uint32_t v = 0;
                if (css_parse_color_rgb(tmp, &v) >= 0) cols[nc++] = v;
                q = e2 + 1;
                continue;
            }
            q = s + 1;
        } else if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) {
            char tmp[24]; int ti = 0;
            while (q < hi && ti < 23 &&
                   ((q[0] >= 'a' && q[0] <= 'z') || (q[0] >= 'A' && q[0] <= 'Z')))
                tmp[ti++] = *q++;
            tmp[ti] = 0;
            uint32_t v = 0;
            if (css_parse_color_rgb(tmp, &v) >= 0) cols[nc++] = v;
            continue;
        } else {
            q++;
        }
    }
    if (nc < 1) return 0;
    uint32_t c0 = cols[0], c1 = cols[nc - 1];
    if (flip) { uint32_t t = c0; c0 = c1; c1 = t; }
    static const char hx[] = "0123456789abcdef";
    val[0] = '!'; val[1] = 'g'; val[2] = (char)('0' + dir);
    for (int k = 0; k < 6; k++) val[3 + k] = hx[(c0 >> (20 - k * 4)) & 0xF];
    for (int k = 0; k < 6; k++) val[9 + k] = hx[(c1 >> (20 - k * 4)) & 0xF];
    val[15] = 0;
    return 1;
}

// Split the block [b0,b1) into declarations and fill rule.
static void css_parse_decls(const char* css, int b0, int b1, struct css_rule* r) {
    r->ndecl = 0;
    int i = b0;
    while (i < b1 && r->ndecl < CSS_MAX_DECL) {
        // find ':' for prop, ';' for end
        int colon = -1, semi = -1;
        int j = i;
        while (j < b1) {
            if (css[j] == ':' && colon < 0) { colon = j; }
            if (css[j] == ';') { semi = j; break; }
            j++;
        }
        if (colon < 0) break; // malformed, skip rest
        int e = (semi < 0) ? b1 : semi;
        // prop = [i, colon), value = [colon+1, e). Props never contain
        // whitespace; values keep SINGLE spaces ("0 auto", "2px solid #000"
        // need word structure — the old strip-all-spaces turned them into
        // "0auto"/"2pxsolid#000", silently breaking every multi-word value).
        char prop[24], val[40]; int pi = 0, vi = 0;
        int k = i; while (k < colon && pi < 23) { char c=css[k]; if(!css_isws(c)) prop[pi++]=c; k++; }
        prop[pi]=0;
        k = colon+1;
        while (k < e && css_isws(css[k])) k++; // leading trim
        int sp = 0;
        while (k < e && vi < 39) {
            char c = css[k];
            if (css_isws(c)) { sp = 1; k++; continue; }
            if (sp && vi > 0) { val[vi++] = ' '; if (vi >= 39) break; }
            sp = 0;
            val[vi++] = c; k++;
        }
        val[vi]=0;
        // Trailing CR/LF/formfeed (last decl before '}' written multiline:
        // "color: red\n}" left val="red\n", which matched nothing and the
        // declaration silently died). Trim all trailing whitespace.
        while (pi > 0 && (prop[pi-1] == '\r' || prop[pi-1] == '\n' ||
                          prop[pi-1] == '\f' || prop[pi-1] == '\v')) prop[--pi] = 0;
        while (vi > 0 && (val[vi-1] == '\r' || val[vi-1] == '\n' ||
                          val[vi-1] == '\f' || val[vi-1] == '\v')) val[--vi] = 0;
        // Long background gradients would truncate mid-stops at the
        // 40-char value cap and parse to wrong colors: compact to canonical
        // "!g<dir><c0><c1>" form first (bounded scan of the raw range).
        if ((css_ieq(prop, "background") || css_ieq(prop, "background-image")) &&
            e - (colon + 1) > 20) {
            char cval[16];
            if (css_compact_gradient(css, colon + 1, e, cval)) {
                int ci = 0;
                while (cval[ci] && ci < 39) { val[ci] = cval[ci]; ci++; }
                val[ci] = 0;
                vi = ci;
            }
        }
        if (prop[0] && val[0]) {
            int imp = css_take_important(val);
            if (!val[0]) { /* "!important" alone: nothing to apply */ }
            else {
                strncpy(r->decl[r->ndecl].prop, prop, 23);
                strncpy(r->decl[r->ndecl].value, val, 39);
                r->decl[r->ndecl].important = imp;
                r->ndecl++;
            }
        }
        if (semi < 0) break;
        i = semi + 1;
    }
}

// Extract a px number following `key` (e.g. "min-width:1120px") in a media
// condition, or -1. Handles "min-width" / "max-width" with optional spaces.
static int media_px(const char* cond, const char* key) {
    int kl = 0; while (key[kl]) kl++;
    for (int i = 0; cond[i]; i++) {
        int k = 0;
        while (k < kl && cond[i + k] == key[k]) k++;
        if (k != kl) continue;
        int j = i + kl;
        while (cond[j] == ' ' || cond[j] == '\t' || cond[j] == ':') j++;
        int v = 0, seen = 0;
        while (cond[j] >= '0' && cond[j] <= '9') { v = v * 10 + (cond[j++] - '0'); seen = 1; }
        if (seen) return v;
    }
    return -1;
}

int css_parse_rules_media(const char* css, int len, struct css_rule* rules,
                          int max_rules, int specificity_base,
                          int media_min, int media_max) {
    int n = 0, i = 0;
    while (i < len && n < max_rules) {
        // skip whitespace and comments
        if (css[i] == '/' && i+1 < len && css[i+1] == '*') {
            i += 2; while (i+1 < len && !(css[i]=='*' && css[i+1]=='/')) i++;
            i += 2; continue;
        }
        if (css[i] == ' ' || css[i] == '\t' || css[i] == '\n' || css[i] == '\r') { i++; continue; }

        // read selector list up to '{'
        int sel_start = i;
        while (i < len) {
            if (css[i] == '{') break;
            if (css[i] == '}') { i++; break; } // stray close
            i++;
        }
        if (i >= len || css[i] != '{') break;
        int sel_end = i;
        // find matching '}'
        int bclose = i, bdepth = 0;
        while (bclose < len) {
            if (css[bclose] == '{') bdepth++;
            else if (css[bclose] == '}') { bdepth--; if (bdepth == 0) break; }
            bclose++;
        }

        // At-rules: handle @media by recursing into the block with its bounds;
        // skip other at-rules (@font-face, @keyframes, @supports, ...).
        if (css[sel_start] == '@') {
            if (css[sel_start + 1] == 'm' && css[sel_start + 2] == 'e' &&
                css[sel_start + 3] == 'd' && css[sel_start + 4] == 'i' &&
                css[sel_start + 5] == 'a') {
                char cond[96]; int ci = 0;
                for (int k = sel_start + 6; k < sel_end && ci < 95; k++)
                    cond[ci++] = css[k];
                cond[ci] = 0;
                int mn = media_px(cond, "min-width");
                int mx = media_px(cond, "max-width");
                if (mn > media_min) media_min = mn;
                if (mx >= 0 && (media_max < 0 || mx < media_max)) media_max = mx;
                n += css_parse_rules_media(css + i + 1, bclose - (i + 1),
                                           rules + n, max_rules - n,
                                           specificity_base, media_min, media_max);
            }
            i = bclose + 1;
            continue;
        }
        int decl0 = i + 1, decl1 = bclose;
        i = bclose + 1;

        // split selector list by ','
        int p = sel_start;
        while (p <= sel_end && n < max_rules) {
            int q = p;
            while (q < sel_end && css[q] != ',') q++;
            // selector text = [p, q)
            char buf[64]; int bi = 0;
            for (int k = p; k < q && bi < 63; k++) buf[bi++] = css[k];
            buf[bi] = 0;
            struct css_rule r;
            memset(&r, 0, sizeof(r));
            css_parse_selector(buf, &r);
            css_parse_decls(css, decl0, decl1, &r);
            // Drop rules whose every declaration is an unsupported property:
            // real sheets (Wikipedia's skin is 127KB) are mostly properties we
            // cannot apply, and they otherwise fill the rule table before the
            // layout rules (display/grid/width) are reached.
            int useful = 0;
            for (int d = 0; d < r.ndecl; d++)
                if (css_prop_index(r.decl[d].prop) >= 0) { useful = 1; break; }
            if (!useful) { p = (q < sel_end) ? q + 1 : q + 1; continue; }
            r.specificity = css_compute_specificity(&r) + specificity_base;
            r.media_min = media_min;
            r.media_max = media_max;
            rules[n++] = r;
            p = (q < sel_end) ? q + 1 : q + 1;
        }
    }
    return n;
}

int css_parse_rules(const char* css, int len, struct css_rule* rules, int max_rules,
                    int specificity_base) {
    return css_parse_rules_media(css, len, rules, max_rules, specificity_base, -1, -1);
}

int css_parse(const char* css, int len, struct css_rule* rules, int max_rules) {
    return css_parse_rules_media(css, len, rules, max_rules, 0, -1, -1);
}

// ---- user-agent stylesheet ----------------------------------------------
//
// The default rendering rules every browser has in its UA sheet. They are
// parsed through the SAME parser as author CSS but carry a negative
// specificity base, so any author rule wins a tie. This is what makes
// unstyled pages legible (block separation, heading sizes, list markers,
// link underlines) without hard-coding any of it into the layout engine.
static const char UA_CSS[] =
    "html,body,div,p,ul,ol,li,dl,dt,dd,table,tr,td,th,blockquote,"
    "header,footer,nav,section,article,main,aside,figure,figcaption,"
    "form,fieldset,h1,h2,h3,h4,h5,h6,pre,hr{display:block}"
    "h1{font-size:2em;font-weight:bold;margin:.67em 0}"
    "h2{font-size:1.5em;font-weight:bold;margin:.83em 0}"
    "h3{font-size:1.17em;font-weight:bold;margin:1em 0}"
    "h4{font-size:1em;font-weight:bold;margin:1.33em 0}"
    "h5{font-size:.83em;font-weight:bold;margin:1.67em 0}"
    "h6{font-size:.67em;font-weight:bold;margin:2.33em 0}"
    "p{margin:1em 0}"
    "blockquote{margin:1em 40px}"
    "pre{white-space:pre;font-family:monospace;margin:1em 0}"
    "ul,ol{margin:1em 0;padding-left:40px}"
    "li{margin:0}"
    "dl{margin:1em 0}dt{margin:1em 0 0}dd{margin:0 0 0 40px}"
    "table{display:table;border-collapse:collapse}"
    "tr{display:table-row}td,th{display:table-cell;padding:2px;text-align:left}"
    "hr{display:block;margin:.5em 0}"
    "b,strong{font-weight:bold}i,em{font-style:italic}"
    "a{color:#0000EE;text-decoration:underline}"
    "a:link,a:visited{color:#0000EE}"
    "html,body{background:#FFFFFF;color:#000000}"
    "center{text-align:center;display:block}";

void css_ua_init(void) {
    if (g_ua_n > 0) return;
    g_ua_n = css_parse_rules(UA_CSS, (int)sizeof(UA_CSS) - 1,
                             g_ua_rules, CSS_UA_MAX_RULES, -10000);
}

int css_ua_rules(const struct css_rule** rules_out) {
    css_ua_init();
    if (rules_out) *rules_out = g_ua_rules;
    return g_ua_n;
}

// ---- matching -----------------------------------------------------------

// Does one compound match the identity (tag/cls/id) given?
static int css_compound_match(const struct css_compound* c, const char* tag,
                              const char* cls, const char* id) {
    if (c->tag[0] && (!tag || !css_ieq(tag, c->tag))) return 0;
    if (c->id[0] && (!id || !css_ieq(id, c->id))) return 0;
    for (int i = 0; i < c->ncls; i++)
        if (!cls || !css_has_class(cls, c->cls[i])) return 0;
    return 1;
}

// Match chain[k..nchain-1] against the ancestor list starting at `ai` (nearest
// first). chain[0] already matched the subject; a DESCEND combinator searches
// any ancestor, a CHILD combinator requires the immediately-next one.
static int css_match_chain(const struct css_rule* r, int k,
                           const struct css_ctx* ctx, int ai) {
    if (k >= r->nchain) return 1;
    const struct css_compound* c = &r->chain[k];
    if (c->comb == CSS_COMB_CHILD) {
        if (ai >= ctx->n_ancestors) return 0;
        if (!css_compound_match(c, ctx->ancestors_tag[ai],
                                ctx->ancestors_cls[ai], ctx->ancestors_id[ai]))
            return 0;
        return css_match_chain(r, k + 1, ctx, ai + 1);
    }
    // Descendant (or the subject handled by caller): try each ancestor.
    for (int j = ai; j < ctx->n_ancestors; j++) {
        if (!css_compound_match(c, ctx->ancestors_tag[j],
                                ctx->ancestors_cls[j], ctx->ancestors_id[j]))
            continue;
        if (css_match_chain(r, k + 1, ctx, j + 1)) return 1;
    }
    return 0;
}

static int css_rule_matches_ctx(const struct css_rule* r,
                                const struct css_ctx* ctx) {
    // @media bounds: only apply when the viewport width is in range. A ctx
    // with viewport_px == 0 (legacy callers/tests) ignores media conditions.
    if (ctx->viewport_px > 0) {
        if (r->media_min > 0 && ctx->viewport_px < r->media_min) return 0;
        if (r->media_max > 0 && ctx->viewport_px > r->media_max) return 0;
    }
    if (!css_compound_match(&r->chain[0], ctx->tag, ctx->cls, ctx->id))
        return 0;
    if (r->nchain == 1) return 1;
    return css_match_chain(r, 1, ctx, 0);
}

static void css_match_win_ctx(const struct css_rule* rules, int n,
               const struct css_ctx* ctx,
               struct css_style* out, int* win, int* wimp) {
    for (int i = 0; i < n; i++) {
        if (!css_rule_matches_ctx(&rules[i], ctx)) continue;
        for (int d = 0; d < rules[i].ndecl; d++)
            css_apply(out, rules[i].decl[d].prop, rules[i].decl[d].value,
                      rules[i].specificity, rules[i].decl[d].important,
                      win, wimp);
    }
}

void css_match_ctx(const struct css_rule* rules, int n,
                   const struct css_ctx* ctx, struct css_style* out) {
    int win[P_N], wimp[P_N];
    for (int k = 0; k < P_N; k++) { win[k] = -0x7FFFFFFF; wimp[k] = 0; }
    css_match_win_ctx(rules, n, ctx, out, win, wimp);
}

// Legacy flat match: build a one-element context (no ancestors).
void css_match(const struct css_rule* rules, int n,
               const char* tag, const char* cls, const char* id,
               struct css_style* out) {
    struct css_ctx ctx;
    ctx.tag = tag; ctx.cls = cls; ctx.id = id;
    ctx.ancestors_tag = 0; ctx.ancestors_cls = 0; ctx.ancestors_id = 0;
    ctx.n_ancestors = 0;
    int win[P_N], wimp[P_N];
    for (int k = 0; k < P_N; k++) { win[k] = -0x7FFFFFFF; wimp[k] = 0; }
    css_match_win_ctx(rules, n, &ctx, out, win, wimp);
}

static void css_parse_inline_win(const char* style, struct css_style* out,
                                 int* win, int* wimp) {
    int i = 0, len = (int)strlen(style);
    while (i < len) {
        int colon = -1, semi = -1, j = i;
        while (j < len) {
            if (style[j] == ':' && colon < 0) colon = j;
            if (style[j] == ';') { semi = j; break; }
            j++;
        }
        if (colon < 0) break;
        int e = (semi < 0) ? len : semi;
        char prop[24], val[40]; int pi = 0, vi = 0;
        int k = i; while (k < colon && pi < 23) { char c=style[k]; if(!css_isws(c)) prop[pi++]=c; k++; }
        prop[pi]=0;
        k = colon+1;
        while (k < e && css_isws(style[k])) k++; // leading trim
        int sp = 0;
        while (k < e && vi < 39) {
            char c = style[k];
            if (css_isws(c)) { sp = 1; k++; continue; }
            if (sp && vi > 0) { val[vi++] = ' '; if (vi >= 39) break; }
            sp = 0;
            val[vi++] = c; k++;
        }
        val[vi]=0;
        while (pi > 0 && (prop[pi-1] == '\r' || prop[pi-1] == '\n' ||
                          prop[pi-1] == '\f' || prop[pi-1] == '\v')) prop[--pi] = 0;
        while (vi > 0 && (val[vi-1] == '\r' || val[vi-1] == '\n' ||
                          val[vi-1] == '\f' || val[vi-1] == '\v')) val[--vi] = 0;
        // Long background gradients would truncate mid-stops at the
        // 40-char value cap and parse to wrong colors: compact to canonical
        // "!g<dir><c0><c1>" form first (bounded scan of the raw range).
        if ((css_ieq(prop, "background") || css_ieq(prop, "background-image")) &&
            e - (colon + 1) > 20) {
            char cval[16];
            if (css_compact_gradient(style, colon + 1, e, cval)) {
                int ci = 0;
                while (cval[ci] && ci < 39) { val[ci] = cval[ci]; ci++; }
                val[ci] = 0;
                vi = ci;
            }
        }
        if (prop[0] && val[0]) {
            int imp = css_take_important(val);
            // Inline styles outrank every normal rule (spec INF) but LOSE
            // to author !important per spec — hence important=imp, not 1.
            if (val[0]) css_apply(out, prop, val, 0x7FFFFFFF, imp, win, wimp);
        }
        if (semi < 0) break;
        i = semi + 1;
    }
}

void css_parse_inline(const char* style, struct css_style* out) {
    if (!style || !style[0]) return;
    int win[P_N], wimp[P_N];
    for (int k = 0; k < P_N; k++) { win[k] = -0x7FFFFFFF; wimp[k] = 0; }
    css_parse_inline_win(style, out, win, wimp);
}

void css_compute_ctx(const struct css_rule* rules, int n,
                     const struct css_ctx* ctx, const char* inline_style,
                     struct css_style* out) {
    css_style_defaults(out);
    // ONE winner table shared by rules + inline: an author !important must
    // beat a normal inline style (inline runs last but with the same state,
    // so its INF specificity only wins ties at equal importance).
    int win[P_N], wimp[P_N];
    for (int k = 0; k < P_N; k++) { win[k] = -0x7FFFFFFF; wimp[k] = 0; }
    css_match_win_ctx(rules, n, ctx, out, win, wimp);
    if (inline_style && inline_style[0]) css_parse_inline_win(inline_style, out, win, wimp);
}

void css_compute(const struct css_rule* rules, int n,
                 const char* tag, const char* cls, const char* id,
                 const char* inline_style, struct css_style* out) {
    struct css_ctx ctx;
    ctx.tag = tag; ctx.cls = cls; ctx.id = id;
    ctx.ancestors_tag = 0; ctx.ancestors_cls = 0; ctx.ancestors_id = 0;
    ctx.n_ancestors = 0;
    css_compute_ctx(rules, n, &ctx, inline_style, out);
}

// Fill unset fields of `out` from `base`. Call after css_compute so inherited
// (e.g. body-level) styling applies where a token's own rules didn't set it.
void css_merge_base(struct css_style* out, const struct css_style* base) {
    if (!out->has_fg        && base->has_fg)        { out->has_fg = 1;        out->fg = base->fg; }
    if (!out->has_bg        && base->has_bg)        { out->has_bg = 1;        out->bg = base->bg; }
    if (!out->has_fg_rgb    && base->has_fg_rgb)    { out->has_fg_rgb = 1;    out->fg_rgb = base->fg_rgb; }
    if (!out->has_bg_rgb    && base->has_bg_rgb)    { out->has_bg_rgb = 1;    out->bg_rgb = base->bg_rgb; out->bg_grad = base->bg_grad; out->bg_c1 = base->bg_c1; }
    if (!out->has_size      && base->has_size)      { out->has_size = 1;      out->font_size = base->font_size; }
    if (!out->has_bold      && base->has_bold)      { out->has_bold = 1;      out->bold = base->bold; }
    if (!out->has_align     && base->has_align)     { out->has_align = 1;     out->align = base->align; }
    if (!out->has_mt        && base->has_mt)        { out->has_mt = 1;        out->margin_top = base->margin_top; }
    if (!out->has_mb        && base->has_mb)        { out->has_mb = 1;        out->margin_bottom = base->margin_bottom; }
    if (!out->has_ml        && base->has_ml)        { out->has_ml = 1;        out->margin_left = base->margin_left; }
    if (!out->has_mr        && base->has_mr)        { out->has_mr = 1;        out->margin_right = base->margin_right; }
    if (!out->has_pt        && base->has_pt)        { out->has_pt = 1;        out->padding_top = base->padding_top; }
    if (!out->has_pb        && base->has_pb)        { out->has_pb = 1;        out->padding_bottom = base->padding_bottom; }
    if (!out->has_pl        && base->has_pl)        { out->has_pl = 1;        out->padding_left = base->padding_left; }
    if (!out->has_pr        && base->has_pr)        { out->has_pr = 1;        out->padding_right = base->padding_right; }
    if (!out->has_bw        && base->has_bw)        { out->has_bw = 1;        out->border_width = base->border_width; }
    if (!out->has_bc        && base->has_bc)        { out->has_bc = 1;        out->border_color = base->border_color; }
    if (!out->has_bs        && base->has_bs)        { out->has_bs = 1;        out->border_style = base->border_style; }
    if (!out->has_w         && base->has_w)         { out->has_w = 1;         out->width = base->width; }
    if (!out->has_wpct      && base->has_wpct)      { out->has_wpct = 1;      out->wpct = base->wpct; }
    if (!out->has_h         && base->has_h)         { out->has_h = 1;         out->height = base->height; }
    if (!out->has_vis       && base->has_vis)       { out->has_vis = 1;       out->vis_hide = base->vis_hide; }
    if (!out->has_ws        && base->has_ws)        { out->has_ws = 1;        out->ws_mode = base->ws_mode; }
    if (!out->has_lst       && base->has_lst)       { out->has_lst = 1;       out->lst_type = base->lst_type; }
    // display is NOT inherited (a flex container's children must not become
    // flex boxes); deliberately omitted from the base merge.
    if (!out->has_td        && base->has_td)        { out->has_td = 1;        out->td_ul = base->td_ul; }
    // NOTE: ml_auto/mr_auto deliberately NOT merged (margins don't inherit;
    // the base-merge already over-inherits layout, auto-centering every
    // descendant would be visibly wrong).
}
