#ifndef WEB_CSS_H
#define WEB_CSS_H

#include <stdint.h>
#include "wdom.h"

// CSS engine: tokenizer (CSS Syntax 3), stylesheet parser, selector
// matching (Selectors 3 + common Level 4), cascade (origins, !important,
// inline style, specificity, order), custom properties + var(), calc(),
// media/supports queries, nesting, and computed values for the layout.
//
// Numbers are fixed point. Lengths in computed styles are LU (1/64 px);
// percentages are stored in 1/100ths of a percent (100% = 10000) and
// resolved by the layout against the right basis.

// ---- computed length: px (LU) + pct (1/100 %) ------------------------------
#define WL_AUTO    0
#define WL_LEN     1   // px + pct
#define WL_NONE    2   // max-width:none etc.
#define WL_MIN     3   // min-content
#define WL_MAX     4   // max-content
#define WL_FIT     5   // fit-content
#define WL_CONTENT 6   // flex-basis: content

struct wlen {
    int32_t px;     // LU
    int32_t pct;    // 1/100 percent
    uint8_t t;      // WL_*
};

static inline struct wlen wl_px(int32_t lu) { struct wlen l = { lu, 0, WL_LEN }; return l; }
static inline struct wlen wl_auto(void) { struct wlen l = { 0, 0, WL_AUTO }; return l; }
static inline int wl_is_auto(struct wlen l) { return l.t == WL_AUTO; }
// Resolve against a basis (LU). Auto/none resolve to `dflt`.
static inline int32_t wl_resolve(struct wlen l, int32_t basis, int32_t dflt) {
    if (l.t != WL_LEN) return dflt;
    int64_t v = (int64_t)basis * l.pct;
    // basis * pct / 10000 without libgcc 64-bit division
    int neg = v < 0; if (neg) v = -v;
    uint64_t u = (uint64_t)v; uint32_t hi = (uint32_t)(u >> 32), lo = (uint32_t)u, q, r;
    if (hi >= 10000u) q = 0x7FFFFFFF;
    else __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(10000u));
    (void)r;
    int32_t p = neg ? -(int32_t)q : (int32_t)q;
    return l.px + (l.pct ? p : 0);
}

// ---- keyword enums -----------------------------------------------------------
enum { D_NONE, D_INLINE, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_FLEX, D_INLINE_FLEX,
       D_GRID, D_INLINE_GRID, D_TABLE, D_INLINE_TABLE, D_TABLE_ROW_GROUP,
       D_TABLE_HEADER_GROUP, D_TABLE_FOOTER_GROUP, D_TABLE_ROW, D_TABLE_CELL,
       D_TABLE_COLUMN, D_TABLE_COLUMN_GROUP, D_TABLE_CAPTION, D_CONTENTS, D_FLOW_ROOT };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FL_NONE, FL_LEFT, FL_RIGHT };
enum { CLR_NONE, CLR_LEFT, CLR_RIGHT, CLR_BOTH };
enum { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO, OV_CLIP };
enum { BS_NONE, BS_HIDDEN, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_GROOVE, BS_RIDGE,
       BS_INSET, BS_OUTSET };
enum { TA_START, TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY, TA_END,
       TA_WEBKIT_CENTER };   // -webkit-center: also centers block-level children
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE, WS_BREAK_SPACES };
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER, VA_TEXT_TOP,
       VA_TEXT_BOTTOM, VA_LEN };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_DECIMAL_LZ, LS_LOWER_ALPHA,
       LS_UPPER_ALPHA, LS_LOWER_ROMAN, LS_UPPER_ROMAN, LS_LOWER_GREEK, LS_NONE,
       LS_DISCLOSURE_OPEN, LS_DISCLOSURE_CLOSED, LS_STRING };
enum { FD_ROW, FD_ROW_REV, FD_COL, FD_COL_REV };
enum { FW_NOWRAP, FW_WRAP, FW_WRAP_REV };
// alignment (justify-content / align-*): shared value set
enum { AL_NORMAL, AL_START, AL_END, AL_CENTER, AL_STRETCH, AL_BASELINE, AL_SPACE_BETWEEN,
       AL_SPACE_AROUND, AL_SPACE_EVENLY, AL_AUTO, AL_LEFT, AL_RIGHT };
enum { BG_REPEAT, BG_REPEAT_X, BG_REPEAT_Y, BG_NO_REPEAT };
enum { BGS_AUTO, BGS_COVER, BGS_CONTAIN, BGS_LEN };
enum { OF_FILL, OF_CONTAIN, OF_COVER, OF_NONE, OF_SCALE_DOWN };
enum { BX_CONTENT, BX_BORDER };
enum { WB_NORMAL, WB_BREAK_ALL, WB_KEEP_ALL, WB_BREAK_WORD };
enum { FAM_SANS, FAM_SERIF, FAM_MONO };

#define DECO_UNDERLINE 1
#define DECO_OVERLINE  2
#define DECO_LINE_THROUGH 4

// background-image layer: an image URL or a linear gradient
#define CSS_MAX_STOPS 8
struct wgrad {
    int32_t angle;           // degrees * 1000 (CSS: 0 = to top, 180 = to bottom)
    uint8_t nstops, repeating, radial;
    uint32_t color[CSS_MAX_STOPS];  // ARGB
    int32_t pos[CSS_MAX_STOPS];     // 1/100 % along the line (-1 = auto)
};

struct wcvars;   // custom properties (opaque)

struct wstyle {
    uint8_t display, position, float_, clear, box_sizing, visibility, overflow_x, overflow_y;
    uint8_t opacity;           // 0..255
    uint8_t z_auto;
    int32_t z_index;
    struct wlen width, height, min_w, min_h, max_w, max_h;
    struct wlen margin[4];     // top right bottom left
    struct wlen padding[4];
    struct wlen inset[4];      // top right bottom left
    int32_t bw[4];             // border widths (LU), already 0 when style none
    uint8_t bs[4];             // border styles
    uint32_t bc[4];            // border colors ARGB
    struct wlen radius[4];     // tl tr br bl
    uint32_t color;            // ARGB
    uint32_t bg_color;         // ARGB
    int32_t bg_image;          // URL: string pool offset (+1), 0 none
    int32_t bg_image_len;
    int16_t bg_grad;           // index into the document gradient table, -1 none
    uint8_t bg_repeat, bg_size_kind;
    struct wlen bg_size[2];
    struct wlen bg_pos[2];
    // text
    uint8_t font_family;       // FAM_*
    uint8_t font_style;        // 0 normal 1 italic
    uint16_t font_weight;      // 100..900
    int32_t font_size;         // LU
    int32_t line_height;       // LU (resolved; 0 = normal)
    int32_t lh_factor;         // number * 1000 when line-height is a number (inherits as a number)
    uint8_t text_align, text_transform, white_space, word_break, text_overflow_ellipsis;
    uint8_t deco_line, deco_style, list_style_type, list_style_inside, vertical_align;
    uint8_t font_small_caps, direction_rtl;
    int32_t va_len;            // LU for VA_LEN
    uint32_t deco_color;       // 0 = currentColor
    struct wlen text_indent;
    int32_t letter_spacing, word_spacing; // LU
    int32_t list_marker;       // LS_STRING: string pool offset (+1)
    // flex / grid
    uint8_t flex_direction, flex_wrap, justify_content, align_items, align_self, align_content;
    uint8_t justify_items, justify_self, grid_auto_flow_col, grid_auto_flow_dense;
    int32_t flex_grow, flex_shrink;   // * 1000
    struct wlen flex_basis;
    int32_t order;
    struct wlen row_gap, column_gap;
    int32_t grid_cols, grid_cols_len;  // template strings (pool offset+1, length)
    int32_t grid_rows, grid_rows_len;
    int32_t grid_areas, grid_areas_len;
    int32_t grid_auto_cols, grid_auto_cols_len;
    int32_t grid_auto_rows, grid_auto_rows_len;
    int16_t gc_start, gc_end, gr_start, gr_end; // line numbers (0 auto); span: +1000+n
    int32_t grid_area_name, grid_area_name_len; // named area (pool offset+1)
    // table
    uint8_t border_collapse, table_layout_fixed, caption_bottom;
    int32_t border_spacing_h, border_spacing_v;
    // generated content (::before/::after): pool offset (+1) / length, 0 = none
    int32_t content, content_len;
    // effects
    int32_t shadow_x, shadow_y, shadow_blur, shadow_spread;
    uint32_t shadow_color; uint8_t shadow_inset, has_shadow;
    struct wlen translate_x, translate_y;
    int32_t aspect_ratio;      // w/h * 1000, 0 none
    uint8_t object_fit, pointer_events_none, cursor_pointer, outline_style;
    int32_t outline_width; uint32_t outline_color;
    // svg paint
    uint32_t fill, stroke; int32_t stroke_width; uint8_t fill_none, stroke_none;
    // text-decoration propagated from ancestors (applies to this box's text)
    uint8_t deco_inh;
    uint8_t fs_default;        // font-size is the inherited UA default ("medium")
    uint8_t overflow_wrap;     // 1 = break-word / anywhere
    uint8_t list_value_set;    // <li value> present (ordinal in list_value)
    uint8_t has_mask;          // mask-image set (unsupported: background not painted)
    int32_t list_value;
    uint32_t deco_inh_color;
    // custom properties (inherited)
    struct wcvars* vars;
};

// ---- stylesheets -----------------------------------------------------------------

struct wsheet;        // opaque parsed stylesheet
struct wstyleset;     // all sheets of a document + computed-style storage

#define CSS_ORIGIN_UA     0
#define CSS_ORIGIN_AUTHOR 1

struct wstyleset* css_set_new(struct wdom* d);
// Document URL: base for inline-style url() and presentational backgrounds.
void css_set_doc_base(struct wstyleset* ss, const char* url);
// Number of author sheets added so far.
int  css_set_sheet_count(const struct wstyleset* ss);
void css_set_free(struct wstyleset* ss);
// Append (or insert before `before_idx`, -1 = append) an author stylesheet.
// base_url resolves url()/@import. Returns the sheet index.
int  css_set_add_sheet(struct wstyleset* ss, const char* text, int len, const char* base_url,
                       int before_idx);
// Same, gated by a media query list (the media="" attribute); NULL = all.
int  css_set_add_sheet_ex(struct wstyleset* ss, const char* text, int len, const char* base_url,
                          int before_idx, const char* media, int media_len);
// @import URLs discovered so far that have not been supplied yet; returns
// count and fills urls (absolute) + the sheet index to insert each before.
int  css_set_pending_imports(struct wstyleset* ss, char urls[][256], int* before, int max);
void css_set_mark_import_done(struct wstyleset* ss, const char* url);
// Inline <style> and <link> collection is done by the caller (document
// order); this helper gathers <style> text from the DOM in order.
int  css_collect_inline(struct wdom* d, char* out, int cap);

// Compute styles for every element (and ::before/::after/::marker) for the
// given viewport (px). Results are attached via wnode.aux (style index).
void css_compute_all(struct wstyleset* ss, int viewport_w, int viewport_h);
const struct wstyle* css_style_of(const struct wstyleset* ss, int node);
// Pseudo-element styles: 1 = ::before, 2 = ::after, 3 = ::marker. NULL if none.
const struct wstyle* css_pseudo_of(const struct wstyleset* ss, int node, int which);
// String pool access (content, grid templates, urls).
const char* css_str(const struct wstyleset* ss, int32_t ref, int32_t len);
const struct wgrad* css_grad(const struct wstyleset* ss, int idx);
// Base URL resolution used for url() values (absolute URL into out).
void css_resolve_url(const char* base, const char* rel, int rlen, char* out, int cap);

// Inline style attribute + presentational hints are handled internally.
// Stats for diagnostics.
void css_stats(const struct wstyleset* ss, int* rules, int* styles, int* sheets);

// Anonymous-box style: initial values + parent's inherited properties.
void css_anon_style(const struct wstyle* parent, int display, struct wstyle* out);

// ---- colors --------------------------------------------------------------------
// Parse a CSS color string; returns 1 and ARGB on success.
int  css_parse_color(const char* s, int len, uint32_t* argb);

#endif
