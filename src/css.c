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

// Named colors -> palette index (subset of common names).
static int css_named(const char* v) {
    if (css_ieq(v,"black"))   return 0;
    if (css_ieq(v,"blue"))    return 1;
    if (css_ieq(v,"green"))   return 2;
    if (css_ieq(v,"cyan")||css_ieq(v,"aqua")) return 3;
    if (css_ieq(v,"red"))     return 4;
    if (css_ieq(v,"magenta")||css_ieq(v,"purple")) return 5;
    if (css_ieq(v,"brown"))   return 6;
    if (css_ieq(v,"gray")||css_ieq(v,"grey")||css_ieq(v,"lightgray")||css_ieq(v,"lightgrey")) return 7;
    if (css_ieq(v,"darkgray")||css_ieq(v,"darkgrey")) return 8;
    if (css_ieq(v,"lightblue")) return 9;
    if (css_ieq(v,"lightgreen")) return 10;
    if (css_ieq(v,"lightcyan")) return 11;
    if (css_ieq(v,"lightred")||css_ieq(v,"crimson")) return 12;
    if (css_ieq(v,"pink")||css_ieq(v,"lightmagenta")) return 13;
    if (css_ieq(v,"yellow"))  return 14;
    if (css_ieq(v,"white"))   return 15;
    return -1;
}

// Parse a color value; returns palette index 0-15 or -1.
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int css_parse_color(const char* v) {
    if (v[0] == '#') {
        const char* h = v + 1;
        int n = 0;
        while (is_hex(h[n])) n++;
        if (n == 3) {
            int r = hexval(h[0]), g = hexval(h[1]), b = hexval(h[2]);
            return css_quantize((uint8_t)(r*17),(uint8_t)(g*17),(uint8_t)(b*17));
        } else if (n == 6) {
            int r = hexval(h[0])*16 + hexval(h[1]);
            int g = hexval(h[2])*16 + hexval(h[3]);
            int b = hexval(h[4])*16 + hexval(h[5]);
            return css_quantize((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        return -1;
    }
    return css_named(v);
}

// ---- declaration application (with cascade specificity) ------------------

enum { P_COLOR=0, P_BG, P_SIZE, P_WEIGHT, P_ALIGN, P_MT, P_MB, P_DISPLAY, P_N };

static int css_prop_index(const char* p) {
    if (css_ieq(p,"color")) return P_COLOR;
    if (css_ieq(p,"background") || css_ieq(p,"background-color")) return P_BG;
    if (css_ieq(p,"font-size")) return P_SIZE;
    if (css_ieq(p,"font-weight")) return P_WEIGHT;
    if (css_ieq(p,"text-align")) return P_ALIGN;
    if (css_ieq(p,"margin")) return P_MT;          // shorthand -> top & bottom
    if (css_ieq(p,"margin-top")) return P_MT;
    if (css_ieq(p,"margin-bottom")) return P_MB;
    if (css_ieq(p,"display")) return P_DISPLAY;
    return -1;
}

static int css_px(const char* v) {
    int n = 0;
    while (*v >= '0' && *v <= '9') n = n*10 + (*v++ - '0');
    return n; // px (em/% ignored for v1; treated as px count)
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
        int c = css_parse_color(val);
        if (c >= 0) { out->has_fg = 1; out->fg = (uint8_t)c; }
        break;
    }
    case P_BG: {
        int c = css_parse_color(val);
        if (c >= 0) { out->has_bg = 1; out->bg = (uint8_t)c; }
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
    case P_MT:
        out->has_mt = 1; out->margin_top = css_px(val);
        if (idx == P_MT && !out->has_mb) { out->has_mb = 1; out->margin_bottom = css_px(val); }
        break;
    case P_MB:
        out->has_mb = 1; out->margin_bottom = css_px(val);
        break;
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
    if (!out->has_size      && base->has_size)      { out->has_size = 1;      out->font_size = base->font_size; }
    if (!out->has_bold      && base->has_bold)      { out->has_bold = 1;      out->bold = base->bold; }
    if (!out->has_align     && base->has_align)     { out->has_align = 1;     out->align = base->align; }
    if (!out->has_mt        && base->has_mt)        { out->has_mt = 1;        out->margin_top = base->margin_top; }
    if (!out->has_mb        && base->has_mb)        { out->has_mb = 1;        out->margin_bottom = base->margin_bottom; }
    if (!out->has_display   && base->has_display)   { out->has_display = 1;   out->display = base->display; }
}
