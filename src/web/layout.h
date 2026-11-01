#ifndef WEB_LAYOUT_H
#define WEB_LAYOUT_H

#include <stdint.h>
#include "wdom.h"
#include "css.h"

// Pixel layout engine. Input: DOM + computed styles + viewport size.
// Builds a box tree (block/inline/replaced/flex/grid/table), runs the
// formatting contexts, and emits a display list in paint order plus
// hit-test regions (links, form controls). All coordinates are LU
// (1/64 px) in document space; items flagged `fixed` are viewport-relative.

// ---- display list ----------------------------------------------------------
enum {
    DI_RECT,      // filled (rounded) rect: x y w h color, radii via ref
    DI_BORDER,    // box borders: x y w h, ref -> dborder
    DI_TEXT,      // text run: x (pen) y (baseline), ref -> dtext
    DI_IMAGE,     // image: x y w h, a = image id
    DI_GRAD,      // gradient fill: x y w h, a = gradient index, ref radii (optional)
    DI_SHADOW,    // box-shadow: x y w h (box), ref -> dshadow
    DI_CLIP,      // push clip: x y w h (ref radii optional)
    DI_UNCLIP,
    DI_LINE,      // decoration / hr line: x y w h color
    DI_WIDGET,    // form-control glyph: a = kind (checkbox/radio/select arrow...), b = state
    DI_MARKS,     // text-emphasis marks over a text run: ref -> dtext (a = baseline), emph_cp in the dtext
    DI_SVG,       // inline <svg>: a = DOM node, x y w h viewport
    DI_BGIMG      // background image layer: x y w h (painting area), ref -> dbgimg
};

struct ditem {
    uint8_t kind;
    uint8_t fixed;       // viewport-pinned (position: fixed subtree)
    uint8_t alpha;       // group opacity multiplier (255 = opaque)
    uint8_t _pad;
    int32_t x, y, w, h;
    uint32_t color;      // ARGB
    int32_t a, b;        // kind-specific
    int32_t ref;         // index into a side table, -1 none
};

struct dborder {
    int32_t w[4];        // top right bottom left (LU)
    uint32_t c[4];
    uint8_t s[4];        // BS_*
    int32_t r[4];        // radii tl tr br bl (LU)
};

struct dtext {
    uint32_t t0, tl;     // UTF-8 slice of wlayout.text
    uint8_t family, bold, italic, deco;  // deco: DECO_* bits
    uint16_t px;
    int16_t webfont;     // 0 = embedded faces; else 1 + @font-face registry index
    uint8_t deco_style;
    int32_t letter_sp, word_sp;  // LU
    uint32_t deco_color;
    int32_t width;       // LU advance of the run (for decorations)
    uint16_t emph_cp;    // text-emphasis mark codepoint (DI_MARKS), 0 = none
    uint8_t vmode, vup;  // DI_MARKS: vertical run (band rect), bottom-to-top
};

struct dshadow { int32_t blur, spread, ox, oy; int32_t r[4]; };
struct dbgimg {
    int32_t img;         // image id
    int32_t ix, iy, iw, ih; // first tile origin + tile size (LU)
    int32_t stepx, stepy;   // distance between tiles (tile size + space gap)
    uint8_t rx, ry;         // 1 = tiles repeat along this axis
};

// ---- hit regions --------------------------------------------------------------
#define HIT_LINK  1
#define HIT_FIELD 2      // text input / textarea / select
#define HIT_BUTTON 3     // submit/button/checkbox/radio
struct dhit {
    int32_t x, y, w, h;  // LU
    int32_t node;        // the <a>/<area> or control element
    uint8_t kind, fixed;
};

// Image access supplied by the document: return image id (>= 0) and the
// intrinsic size in px when known (w/h may be 0 while loading), or -1.
struct wlay_images {
    void* ctx;
    int (*for_node)(void* ctx, int node, int* w, int* h);        // <img>, <input type=image>, video poster
    int (*for_url)(void* ctx, const char* url, int* w, int* h);  // CSS url()
    int (*info)(void* ctx, int id, int* w, int* h, int32_t* ratio);   // intrinsic size px (-1 none) and ratio w/h*65536 (0 none) of image id
};

struct wlayout;

struct wlayout* wlay_run(struct wdom* d, struct wstyleset* ss, int vw, int vh,
                         const struct wlay_images* imgs);
void wlay_free(struct wlayout* L);

int  wlay_doc_height(const struct wlayout* L);   // px
int  wlay_doc_width(const struct wlayout* L);    // px
uint32_t wlay_canvas_bg(const struct wlayout* L); // page background (ARGB)

int  wlay_items(const struct wlayout* L, const struct ditem** items);
const struct dborder* wlay_border(const struct wlayout* L, int ref);
const struct dtext* wlay_text_ref(const struct wlayout* L, int ref);
const struct dshadow* wlay_shadow(const struct wlayout* L, int ref);
const struct dbgimg* wlay_bgimg(const struct wlayout* L, int ref);
const int32_t* wlay_radii(const struct wlayout* L, int ref);
const char* wlay_text(const struct wlayout* L);
int  wlay_hits(const struct wlayout* L, const struct dhit** hits);
// Box of a DOM element (absolute border box, LU); 0 if not rendered.
int  wlay_scroll_info(const struct wlayout* L, int node, int32_t* sw, int32_t* sh, int32_t* cw, int32_t* ch);
int  wlay_node_rect(const struct wlayout* L, int node, int32_t* x, int32_t* y, int32_t* w, int32_t* h);
int  wlay_paint_rank(const struct wlayout* L, int node, int* fixed);   // paint order of the node's box (0 = unpainted); *fixed: inside a fixed box
int  wlay_knows_node(const struct wlayout* L, int node);     // 1 if the node existed when this layout was made
// Stats for diagnostics.
void wlay_stats(const struct wlayout* L, int* boxes, int* items, int* lines);

#endif
