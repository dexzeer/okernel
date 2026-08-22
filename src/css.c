#include "css.h"
#include <string.h>

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
        P_ML, P_MR, P_PT, P_PR, P_PB, P_PL, P_BW, P_BC, P_BS, P_W, P_H, P_N };

static int css_prop_index(const char* p) {
    if (css_ieq(p,"color")) return P_COLOR;
    if (css_ieq(p,"background") || css_ieq(p,"background-color")) return P_BG;
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
    if (css_ieq(p,"display")) return P_DISPLAY;
    return -1;
}

static int css_px(const char* v) {
    int n = 0;
    while (*v >= '0' && *v <= '9') n = n*10 + (*v++ - '0');
    return n; // px (em/% ignored for v1; treated as px count)
}

// Parse up to `max` whitespace/comma-separated integers from a shorthand value
// (e.g. "10px 20px" -> {10,20}). Returns the count parsed.
static int css_px_list(const char* v, int* out, int max) {
    int n = 0;
    while (*v && n < max) {
        while (*v == ' ' || *v == '\t' || *v == ',') v++;
        if (!(*v >= '0' && *v <= '9')) break;
        int x = 0;
        while (*v >= '0' && *v <= '9') x = x*10 + (*v++ - '0');
        out[n++] = x;
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

// Apply one declaration if its specificity wins. `win` tracks per-prop winner.
static void css_apply(struct css_style* out, const char* prop, const char* val,
                      int spec, int* win) {
    int idx = css_prop_index(prop);
    if (idx < 0) return;
    if (spec < win[idx]) return;   // a more specific/earlier-equal rule owns it
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
        uint32_t rgb;
        int c = css_parse_color_rgb(val, &rgb);
        if (c >= 0) { out->has_bg = 1; out->bg = (uint8_t)c;
                      out->has_bg_rgb = 1; out->bg_rgb = rgb; }
        break;
    }
    case P_SIZE:
        out->has_size = 1; out->font_size = css_px(val);
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
        int v[4]; int n = css_px_list(val, v, 4);
        if (!n) break;
        if (n == 1) { v[1] = v[2] = v[3] = v[0]; }
        else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
        else if (n == 3) { v[3] = v[1]; }
        out->has_mt = 1; out->margin_top    = v[0];
        out->has_mr = 1; out->margin_right  = v[1];
        out->has_mb = 1; out->margin_bottom = v[2];
        out->has_ml = 1; out->margin_left   = v[3];
        break;
    }
    case P_MB: out->has_mb = 1; out->margin_bottom = css_px(val); break;
    case P_ML: out->has_ml = 1; out->margin_left   = css_px(val); break;
    case P_MR: out->has_mr = 1; out->margin_right  = css_px(val); break;
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
        int w = css_px(val);
        if (w > 0) { out->has_bw = 1; out->border_width = w; }
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
    case P_W:  out->has_w  = 1; out->width  = css_px(val); break;
    case P_H:  out->has_h  = 1; out->height = css_px(val); break;
    case P_DISPLAY:
        out->has_display = 1;
        if (css_ieq(val,"none")) out->display = CSS_DISPLAY_NONE;
        else if (css_ieq(val,"inline")) out->display = CSS_DISPLAY_INLINE;
        else out->display = CSS_DISPLAY_BLOCK;
        break;
    }
}

void css_style_defaults(struct css_style* s) {
    memset(s, 0, sizeof(*s));
    s->display = CSS_DISPLAY_INLINE; // block decided by token type in renderer
}

// ---- parser -------------------------------------------------------------

// Parse one simple selector string into rule.sel / sel_class / sel_id.
static void css_parse_selector(const char* s, struct css_rule* r) {
    r->sel.kind = CSS_SEL_UNIVERSAL;
    r->sel.value[0] = 0;
    r->sel_class[0] = 0;
    r->sel_id[0] = 0;
    while (*s == ' ' || *s == '\t') s++;

    if (*s == 0) return;
    if (*s == '*') { r->sel.kind = CSS_SEL_UNIVERSAL; s++; return; }

    if (*s == '#') {
        r->sel.kind = CSS_SEL_ID; s++;
        int i = 0; while (*s && css_is_ident(*s) && i < 31) r->sel.value[i++] = *s++;
        r->sel.value[i] = 0; css_lower(r->sel.value, r->sel.value);
    } else if (*s == '.') {
        r->sel.kind = CSS_SEL_CLASS; s++;
        int i = 0; while (*s && css_is_ident(*s) && i < 31) r->sel.value[i++] = *s++;
        r->sel.value[i] = 0; css_lower(r->sel.value, r->sel.value);
    } else {
        r->sel.kind = CSS_SEL_TAG;
        int i = 0; while (*s && css_is_ident(*s) && i < 31) r->sel.value[i++] = *s++;
        r->sel.value[i] = 0; css_lower(r->sel.value, r->sel.value);
    }
    // optional trailing .class / #id
    while (*s == '.' || *s == '#') {
        char sep = *s++;
        char buf[32]; int i = 0;
        while (*s && css_is_ident(*s) && i < 31) buf[i++] = *s++;
        buf[i] = 0; css_lower(buf, buf);
        if (sep == '.') strncpy(r->sel_class, buf, 31);
        else strncpy(r->sel_id, buf, 31);
    }
}

static int css_compute_specificity(struct css_rule* r) {
    int id = (r->sel.kind == CSS_SEL_ID ? 1 : 0) + (r->sel_id[0] ? 1 : 0);
    int cl = (r->sel.kind == CSS_SEL_CLASS ? 1 : 0) + (r->sel_class[0] ? 1 : 0);
    int tg = (r->sel.kind == CSS_SEL_TAG ? 1 : 0);
    return id*100 + cl*10 + tg;
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
        // prop = [i, colon), value = [colon+1, e)
        char prop[24], val[40]; int pi = 0, vi = 0;
        int k = i; while (k < colon && pi < 23) { char c=css[k]; if(c!=' '&&c!='\t') prop[pi++]=c; k++; }
        prop[pi]=0;
        k = colon+1; while (k < e && vi < 39) { char c=css[k]; if(c!=' '&&c!='\t') val[vi++]=c; k++; }
        val[vi]=0;
        if (prop[0] && val[0]) {
            strncpy(r->decl[r->ndecl].prop, prop, 23);
            strncpy(r->decl[r->ndecl].value, val, 39);
            r->ndecl++;
        }
        if (semi < 0) break;
        i = semi + 1;
    }
}

int css_parse(const char* css, int len, struct css_rule* rules, int max_rules) {
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
        int depth = 0;
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
            r.specificity = css_compute_specificity(&r);
            rules[n++] = r;
            p = (q < sel_end) ? q + 1 : q + 1;
        }
    }
    return n;
}

// ---- matching -----------------------------------------------------------

static int css_rule_matches(const struct css_rule* r, const char* tag,
                            const char* cls, const char* id) {
    if (r->sel.kind == CSS_SEL_UNIVERSAL) {
        // still must satisfy any extra class/id requirements
    } else if (r->sel.kind == CSS_SEL_TAG) {
        if (!tag || !css_ieq(tag, r->sel.value)) return 0;
    } else if (r->sel.kind == CSS_SEL_CLASS) {
        if (!cls || !css_has_class(cls, r->sel.value)) return 0;
    } else if (r->sel.kind == CSS_SEL_ID) {
        if (!id || !css_ieq(id, r->sel.value)) return 0;
    }
    if (r->sel_class[0] && (!cls || !css_has_class(cls, r->sel_class))) return 0;
    if (r->sel_id[0] && (!id || !css_ieq(id, r->sel_id))) return 0;
    return 1;
}

void css_match(const struct css_rule* rules, int n,
               const char* tag, const char* cls, const char* id,
               struct css_style* out) {
    int win[P_N];
    for (int k = 0; k < P_N; k++) win[k] = -1;
    for (int i = 0; i < n; i++) {
        if (!css_rule_matches(&rules[i], tag, cls, id)) continue;
        for (int d = 0; d < rules[i].ndecl; d++)
            css_apply(out, rules[i].decl[d].prop, rules[i].decl[d].value,
                      rules[i].specificity, win);
    }
}

void css_parse_inline(const char* style, struct css_style* out) {
    if (!style || !style[0]) return;
    int win[P_N];
    for (int k = 0; k < P_N; k++) win[k] = -1;
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
        int k = i; while (k < colon && pi < 23) { char c=style[k]; if(c!=' '&&c!='\t') prop[pi++]=c; k++; }
        prop[pi]=0;
        k = colon+1; while (k < e && vi < 39) { char c=style[k]; if(c!=' '&&c!='\t') val[vi++]=c; k++; }
        val[vi]=0;
        if (prop[0] && val[0]) css_apply(out, prop, val, 0x7FFFFFFF, win);
        if (semi < 0) break;
        i = semi + 1;
    }
}

void css_compute(const struct css_rule* rules, int n,
                 const char* tag, const char* cls, const char* id,
                 const char* inline_style, struct css_style* out) {
    css_style_defaults(out);
    css_match(rules, n, tag, cls, id, out);
    if (inline_style && inline_style[0]) css_parse_inline(inline_style, out);
}

// Fill unset fields of `out` from `base`. Call after css_compute so inherited
// (e.g. body-level) styling applies where a token's own rules didn't set it.
void css_merge_base(struct css_style* out, const struct css_style* base) {
    if (!out->has_fg        && base->has_fg)        { out->has_fg = 1;        out->fg = base->fg; }
    if (!out->has_bg        && base->has_bg)        { out->has_bg = 1;        out->bg = base->bg; }
    if (!out->has_fg_rgb    && base->has_fg_rgb)    { out->has_fg_rgb = 1;    out->fg_rgb = base->fg_rgb; }
    if (!out->has_bg_rgb    && base->has_bg_rgb)    { out->has_bg_rgb = 1;    out->bg_rgb = base->bg_rgb; }
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
    if (!out->has_h         && base->has_h)         { out->has_h = 1;         out->height = base->height; }
    if (!out->has_display   && base->has_display)   { out->has_display = 1;   out->display = base->display; }
}
