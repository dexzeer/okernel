#ifndef CSS_H
#define CSS_H

#include <stdint.h>

// Rule cap. Real pages carry hundreds of rules before the useful body/a
// rules (google.com: ~200 obfuscated .gb_* widget rules first) — a 48-rule
// cap never reached `body{background:#fff}`, so pages rendered on the default
// black instead of the page's own background.
#define CSS_MAX_RULES   1536
#define CSS_MAX_DECL    12

// Selector kinds (subject of a rule)
#define CSS_SEL_UNIVERSAL 0
#define CSS_SEL_TAG      1
#define CSS_SEL_CLASS    2
#define CSS_SEL_ID       3

// Combinator before a compound in a complex selector.
#define CSS_COMB_NONE      0  // first compound (subject), no combinator
#define CSS_COMB_DESCEND   1  // "a b"
#define CSS_COMB_CHILD     2  // "a > b"

#define CSS_MAX_COMPOUND 6   // compounds per complex selector ("div.a ul li a")

// One compound selector: optional tag, id and a set of classes. All present
// parts must match the same element ("div.a#b").
struct css_compound {
    char tag[24];        // "" = any
    char id[32];         // "" = any
    char cls[4][24];      // AND-ed class list (compound ".a.b")
    int  ncls;
    int  comb;           // CSS_COMB_* connector to the PREVIOUS compound
};

struct css_decl {
    char prop[24];
    char value[40];
    int  important;  // trailing "!important" (beats normal decls at any specificity)
};

struct css_rule {
    // Complex selector, stored right-to-left: chain[0] is the SUBJECT (the
    // element the rule styles), chain[1..n-1] are its ancestors/children
    // requirements. comb[i] tells how chain[i] relates to chain[i+1].
    struct css_compound chain[CSS_MAX_COMPOUND];
    int  nchain;
    int  ndecl;
    struct css_decl decl[CSS_MAX_DECL];
    int  specificity;          // id*100 + class*10 + tag
    int  media_min;            // @media (min-width: Npx); -1 = unbounded
    int  media_max;            // @media (max-width: Npx); -1 = unbounded
};

// A match context: the element's own identity plus its ancestor chain (parents
// only, nearest first), for descendant/child combinator matching.
struct css_ctx {
    const char* tag;
    const char* cls;
    const char* id;
    const char* const* ancestors_tag;  // nearest-first, NULL-terminated
    const char* const* ancestors_cls;
    const char* const* ancestors_id;
    int         n_ancestors;
    int         viewport_px;   // viewport width for @media evaluation (0 = any)
};

// Renderer-facing computed style. Colors are 0-15 (VGA text palette indices).
#define CSS_DISPLAY_INLINE 0
#define CSS_DISPLAY_BLOCK  1
#define CSS_DISPLAY_NONE   2
#define CSS_DISPLAY_FLEX   3
#define CSS_DISPLAY_GRID   4
#define CSS_DISPLAY_INLINE_BLOCK 5
#define CSS_DISPLAY_TABLE  6
#define CSS_DISPLAY_TABLE_ROW 7
#define CSS_DISPLAY_TABLE_CELL 8
#define CSS_FLOAT_NONE  0
#define CSS_FLOAT_LEFT  1
#define CSS_FLOAT_RIGHT 2
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
    int   bg_grad;    // 0=solid 2=vertical gradient 3=horizontal gradient
    uint32_t bg_c1;   // gradient end color (bg_rgb is the start)
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
    int   has_wpct;    int wpct;         // width: N% (0-100, renderer resolves)
    int   has_h;       int height;       // px
    int   has_display; int display;     // CSS_DISPLAY_*
    int   has_td;      int td_ul;       // text-decoration: underline present
    int   ml_auto;     int mr_auto;     // margin-left/right: auto (centering)
    // Layout containers (modern web): flex row/column, grid track list, float.
    int   has_gcols;   char grid_cols[80];  // grid-template-columns raw value
    int   has_fdir;    int flex_dir;        // 0 = row, 1 = column
    int   has_maxw;    int max_width;       // px
    int   has_float;   int float_side;      // CSS_FLOAT_*
    int   has_minw;    int min_width;       // px (content min clamp)
    int   has_tt;      int tt_mode;         // text-transform: 0=none 1=upper 2=lower 3=capitalize
    int   has_vis;     int vis_hide;        // visibility:hidden/collapse
    int   has_lst;     int lst_type;        // list-style-type: 0=default 1=none 2=disc 3=circle
                                            //   4=square 5=decimal 6=lower-alpha 7=upper-alpha
                                            //   8=lower-roman 9=upper-roman
    int   has_ws;      int ws_mode;         // white-space: 0=normal 1=nowrap 2=pre
    int   has_pos;     int pos_mode;        // position: 0=static 1=relative 2=absolute 3=fixed
    int   has_left;    int left;            // px offsets (percent ignored)
    int   has_right;   int right;
    int   has_top;     int top;
    int   has_bottom;  int bottom;
};

// Fill any unset field in `out` from `base` (used to inherit body-level
// styles into every element when the parser has no DOM hierarchy).
void css_merge_base(struct css_style* out, const struct css_style* base);

void css_style_defaults(struct css_style* s);

// Parse a stylesheet (<style> body) into rules. Returns rule count (<= max_rules).
int  css_parse(const char* css, int len, struct css_rule* rules, int max_rules);

// Built-in UA stylesheet. Parsed once into a static table and matched at a
// large negative specificity so every author rule beats it. Layout/render
// codes match these in addition to the page's own rules.
#define CSS_UA_MAX_RULES 80
void css_ua_init(void);
int  css_ua_rules(const struct css_rule** rules_out);

// Match rules against an element (tag, space-separated class list, id) with a
// flat ancestor context (legacy callers: no ancestors) and fold them into
// `out` with correct cascade (specificity then source order).
void css_match(const struct css_rule* rules, int n,
               const char* tag, const char* cls, const char* id,
               struct css_style* out);

// Full-context match (descendant/child combinators).
void css_match_ctx(const struct css_rule* rules, int n,
                   const struct css_ctx* ctx, struct css_style* out);

// One-shot: defaults + matched rules + highest-priority inline style.
void css_compute(const struct css_rule* rules, int n,
                 const char* tag, const char* cls, const char* id,
                 const char* inline_style, struct css_style* out);

// Full-context one-shot.
void css_compute_ctx(const struct css_rule* rules, int n,
                     const struct css_ctx* ctx, const char* inline_style,
                     struct css_style* out);

// Parse an inline `style="..."` attribute and apply at highest priority.
void css_parse_inline(const char* style, struct css_style* out);

#endif
