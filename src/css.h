#ifndef CSS_H
#define CSS_H

#include <stdint.h>

// Rule cap. Real pages carry hundreds of rules before the useful body/a
// rules (google.com: ~200 obfuscated .gb_* widget rules first) — a 48-rule
// cap never reached `body{background:#fff}`, so pages rendered on the default
// black instead of the page's own background.
#define CSS_MAX_RULES   256
#define CSS_MAX_DECL    12

// Selector kinds (subject of a rule)
#define CSS_SEL_UNIVERSAL 0
#define CSS_SEL_TAG      1
#define CSS_SEL_CLASS    2
#define CSS_SEL_ID       3

struct css_decl {
    char prop[24];
    char value[40];
};

struct css_selector {
    int  kind;          // CSS_SEL_*
    char value[32];     // tag name, class name, or id (lowercased)
};

struct css_rule {
    struct css_selector sel;   // subject selector
    char sel_class[32];        // extra class requirement (for "tag.class")
    char sel_id[32];           // extra id requirement (for "tag#id")
    int  ndecl;
    struct css_decl decl[CSS_MAX_DECL];
    int  specificity;          // id*100 + class*10 + tag
};

// Renderer-facing computed style. Colors are 0-15 (VGA text palette indices).
#define CSS_DISPLAY_INLINE 0
#define CSS_DISPLAY_BLOCK  1
#define CSS_DISPLAY_NONE   2
#define CSS_ALIGN_LEFT   0
#define CSS_ALIGN_CENTER 1
#define CSS_ALIGN_RIGHT  2

struct css_style {
    int   has_fg;  uint8_t fg;
    // Exact resolved color (0xRRGGBB). Set whenever has_fg/has_bg is set;
    // the 8-bit field above is the nearest VGA palette index kept for
    // legacy callers and host-test expectations.
    int   has_fg_rgb; uint32_t fg_rgb;
    int   has_bg;  uint8_t bg;
    int   has_bg_rgb; uint32_t bg_rgb;
    int   has_size;    int font_size;   // px
    int   has_bold;    int bold;        // 0/1
    int   has_align;   int align;       // CSS_ALIGN_*
    int   has_mt;      int margin_top;  // px
    int   has_mb;      int margin_bottom;
    int   has_ml;      int margin_left;  // px
    int   has_mr;      int margin_right; // px
    int   has_pt;      int padding_top;  // px
    int   has_pr;      int padding_right;
    int   has_pb;      int padding_bottom;
    int   has_pl;      int padding_left;
    int   has_bw;      int border_width; // px (all sides, v1)
    int   has_bc;      uint32_t border_color; // 0xRRGGBB
    int   has_bs;      int border_style; // 0=none 1=solid (v1)
    int   has_w;       int width;        // px content-width hint
    int   has_h;       int height;       // px
    int   has_display; int display;     // CSS_DISPLAY_*
};

// Fill any unset field in `out` from `base` (used to inherit body-level
// styles into every element when the parser has no DOM hierarchy).
void css_merge_base(struct css_style* out, const struct css_style* base);

void css_style_defaults(struct css_style* s);

// Parse a stylesheet (<style> body) into rules. Returns rule count (<= max_rules).
int  css_parse(const char* css, int len, struct css_rule* rules, int max_rules);

// Match rules against an element (tag, space-separated class list, id) and
// fold them into `out` with correct cascade (specificity then source order).
void css_match(const struct css_rule* rules, int n,
               const char* tag, const char* cls, const char* id,
               struct css_style* out);

// One-shot: defaults + matched rules + highest-priority inline style.
void css_compute(const struct css_rule* rules, int n,
                 const char* tag, const char* cls, const char* id,
                 const char* inline_style, struct css_style* out);

// Parse an inline `style="..."` attribute and apply at highest priority.
void css_parse_inline(const char* style, struct css_style* out);

#endif
