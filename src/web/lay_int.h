#ifndef WEB_LAY_INT_H
#define WEB_LAY_INT_H

#include "layout.h"
#include "font.h"
#include "wcommon.h"

// ---- boxes --------------------------------------------------------------------
enum {
    LB_BLOCK,      // block container (also atomic inline-block / flex / grid / table parts)
    LB_INLINE,     // inline box (span, a, em...)
    LB_TEXT,       // text run (child of an inline formatting context)
    LB_REPLACED,   // img, svg, input, video... (inline-level or block-level)
    LB_BR,
    LB_WBR,
    LB_MARKER      // list marker text (outside markers are positioned specially)
};
// formatting-context type of a block-level / atomic box
enum { FC_FLOW, FC_FLEX, FC_GRID, FC_TABLE, FC_ROWGROUP, FC_ROW, FC_CELL, FC_CAPTION, FC_COLUMN };
// replaced element kinds
enum { RK_IMG, RK_SVG, RK_INPUT_TEXT, RK_INPUT_BUTTON, RK_CHECKBOX, RK_RADIO, RK_SELECT,
       RK_TEXTAREA, RK_IFRAME, RK_VIDEO, RK_CANVAS, RK_METER, RK_HR_UNUSED, RK_OBJECT };

#define BF_INLINE_LEVEL  0x0001   // participates in an inline formatting context
#define BF_IFC           0x0002   // block container whose in-flow children are inline-level
#define BF_ABS           0x0004   // absolutely/fixed positioned (out of flow)
#define BF_FLOAT         0x0008
#define BF_BFC           0x0010   // establishes a new block formatting context
#define BF_ANON          0x0020
#define BF_FIXED         0x0040   // position:fixed or inside a fixed subtree
#define BF_LAID          0x0080
#define BF_HEIGHT_DEF    0x0100   // used height came from a definite size (flex/grid stretch)
#define BF_WIDTH_FORCED  0x0200   // width imposed by the parent (flex/grid/table)
#define BF_COLLAPSE_TOP  0x0400   // first-child margin collapsed through the top
#define BF_EMPTY         0x0800   // margins collapse through (no in-flow content)
#define BF_MARKER        0x1000   // list item with an outside marker (text in t0/tl)
#define BF_RAWTEXT       0x2000   // LB_TEXT whose t0/tl index wlayout.text (generated content)
#define BF_POSITIONED    0x4000   // position != static (containing block for abs)

struct lbox {
    int32_t node;                 // DOM node, -1 anonymous
    const struct wstyle* st;
    uint8_t kind, fc, rk, pseudo; // pseudo: 1 before 2 after 3 marker
    uint16_t flags;
    uint16_t _pad;
    int32_t parent, first, last, next, prev;
    int32_t lparent;              // box whose coordinate space x/y are in (-1 = canvas)
    int32_t x, y, w, h;           // border box, relative to lparent's border box
    int32_t m[4], b[4], p[4];     // used margin/border/padding (T R B L)
    int32_t baseline;             // first baseline from border-box top (-1 none)
    int32_t last_baseline;
    int32_t fr0, nfr;             // IFC fragments
    uint32_t t0, tl;              // text slice (LB_TEXT / LB_MARKER)
    int32_t img;                  // replaced image id (-1)
    int32_t iw, ih;               // intrinsic size px (0 unknown)
    int32_t imin, imax;           // intrinsic widths cache (-1 unknown)
    int32_t gc0, gc1, gr0, gr1;   // grid/table placement (0-based, exclusive end)
    int32_t ax, ay;               // absolute position (after finalize)
    int32_t abs_next;             // linked list of abs boxes owned by a containing block
    int32_t abs_head;
    int32_t sx, sy;               // static position (abs boxes), provisional absolute
    int32_t pax, pay;             // provisional absolute border-box origin during layout
    int32_t list_ord;             // list item ordinal
    int32_t cm[4];                // collapsed margins: top pos/neg, bottom pos/neg
    int32_t cbh;                  // containing block content height for % heights (-1 indefinite)
};

// IFC fragment
enum { FR_TEXT, FR_ATOMIC, FR_IBOX, FR_MARKER };
struct lfrag {
    uint8_t kind;
    uint8_t first, last;          // FR_IBOX: box starts/ends on this line
    uint8_t _p;
    int32_t box;                  // text/inline/atomic box
    int32_t x, y, w, h;           // relative to the IFC root content box; text: y = baseline
    uint32_t t0, tl;              // text slice
    int32_t word_extra;           // justification extra per space (LU)
};

struct bfloat { int32_t x, y, w, h; uint8_t right; };

struct wlayout {
    struct wdom* d;
    struct wstyleset* ss;
    const struct wlay_images* imgs;
    int vw, vh;                   // viewport px
    struct lbox* b;
    int nb, capb;
    struct lfrag* fr;
    int nfr, capfr;
    struct wbuf text;             // processed text
    struct ditem* di;
    int ndi, capdi;
    struct dborder* bord;
    int nbord, capbord;
    struct dtext* dtx;
    int ndtx, capdtx;
    struct dshadow* dsh;
    int ndsh, capdsh;
    struct dbgimg* dbg;
    int ndbg, capdbg;
    int32_t (*rad)[4];
    int nrad, caprad;
    struct dhit* hits;
    int nhits, caphits;
    int32_t* node_box;            // DOM node -> first box (-1)
    int root;                     // root box index
    int32_t doc_h, doc_w;         // LU
    uint32_t canvas_bg;
    int nlines;
    // float context stack (one per BFC)
    struct bfloat* fl;
    int nfl, capfl;
    int fl_base;                  // first float of the current BFC
    struct wstyle* anon_chunk[512]; // anonymous-box styles (chunks of 128)
    int nanon_chunk, anon_used;
    uint8_t alpha;              // current group opacity during DL emission
    int fixed_ctx;                // emitting a fixed subtree
    int depth;
};

// ---- shared helpers (lay_main.c) ----------------------------------------------
int  lb_new(struct wlayout* L, int node, const struct wstyle* st, int kind);
void lb_append(struct wlayout* L, int parent, int child);
void lb_font(const struct wstyle* st, struct wfont* f);
void lb_font_metrics(const struct wstyle* st, struct wfmetrics* m);
int32_t lb_line_height(const struct wstyle* st);
// used values of margins/borders/padding for containing-block width cbw
void lb_resolve_box_sides(struct wlayout* L, int bi, int32_t cbw);
// Lay out box bi with available width avail (content-box width of the
// containing block), positioned at provisional absolute (pax, pay) for float
// queries. Sets w/h. forced_w >= 0 imposes a border-box width.
void lay_box(struct wlayout* L, int bi, int32_t avail, int32_t forced_w, int32_t forced_h);
// shrink-to-fit border-box width for avail
int32_t lay_shrink_width(struct wlayout* L, int bi, int32_t avail);
// intrinsic min/max-content (border-box widths)
void lay_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx);
int32_t lay_content_w(struct wlayout* L, int bi);   // content box width
int  lay_is_bfc(struct wlayout* L, int bi);
// float area queries (absolute coordinates)
void fl_avail(struct wlayout* L, int32_t y, int32_t h, int32_t left, int32_t right,
              int32_t* l_out, int32_t* r_out);
int32_t fl_clear_y(struct wlayout* L, int32_t y, int clear);
int32_t fl_bottom(struct wlayout* L);
void fl_place(struct wlayout* L, int bi, int32_t cb_left, int32_t cb_right, int32_t* y_io);
void lay_abs_register(struct wlayout* L, int bi);

// lay_tree.c
int  lay_build_tree(struct wlayout* L);
const struct wstyle* lay_anon_style(struct wlayout* L, const struct wstyle* parent, int display);
// raw text of a LB_TEXT box (DOM text or generated)
const char* lay_box_raw(struct wlayout* L, int bi, int* len);

// lay_inline.c
void lay_ifc(struct wlayout* L, int bi, int32_t content_w, int32_t* height);
void lay_ifc_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx);

// lay_flex.c / lay_grid.c / lay_table.c
void lay_flex(struct wlayout* L, int bi, int32_t content_w, int32_t* content_h);
void lay_flex_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx);
void lay_grid(struct wlayout* L, int bi, int32_t content_w, int32_t* content_h);
void lay_grid_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx);
void lay_table(struct wlayout* L, int bi, int32_t avail, int32_t* w, int32_t* h);
void lay_table_intrinsic(struct wlayout* L, int bi, int32_t* mn, int32_t* mx);

// lay_dl.c
void lay_emit(struct wlayout* L);

static inline struct lbox* LBX(struct wlayout* L, int i) { return &L->b[i]; }
static inline int lb_is_inflow(const struct lbox* b) {
    return !(b->flags & (BF_ABS | BF_FLOAT));
}

#endif
