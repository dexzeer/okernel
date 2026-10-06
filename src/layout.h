#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdint.h>
#include "dom.h"
#include "css.h"

// From-scratch block/inline layout for the browser's character-cell renderer.
//
// layout_run() walks the DOM in block context and produces a flat display
// list. A "line" item is one grid row plus the styled inline runs on it,
// already wrapped to its content width. A "band" item is a block's painted
// background/border box behind a row range. Coordinates are DOCUMENT rows
// (scroll offsets the blit) and grid columns. All arrays are capped; an
// overflowing page truncates instead of crashing.

#define LAYOUT_MAX_ITEMS 9000
#define LAYOUT_MAX_RUNS  48000
#define LAYOUT_MAX_TEXT  DOM_MAX_TEXT
#define LAYOUT_MAX_WORDS 2600

#define LAYOUT_FLAG_BOLD 1
#define LAYOUT_FLAG_UL   2
#define LAYOUT_FLAG_HIDE 4   // visibility:hidden — reserves space, paints bg only
#define LAYOUT_FLAG_WS   8   // white-space:nowrap word — never force a line break;
// masked out when copying word flags to run flags (paint never sees it)
#define LAYOUT_FLAG_CJK  16  // run holds raw UTF-8 (CJK ideographs/syllabary):
// text_len counts display COLUMNS (chars), blit decodes per char and draws
// from the CJK bitmap table. Survives the WS mask above (paint must see it).

struct layout_run {
    uint32_t text_off;   // into layout.text (decoded render slots)
    uint16_t text_len;
    uint16_t col;        // start column (absolute grid cols)
    uint16_t node;       // source DOM node (link hit-testing / fields)
    uint32_t fg, bg;     // 0xRRGGBB
    uint8_t  flags;      // LAYOUT_FLAG_*
    uint8_t  is_link;
    uint8_t  is_field;   // bracketed form control ([value] / [Submit])
};

// Item kinds
#define LOUT_BAND 0   // painted background/border box over [row, row+height)
#define LOUT_LINE 1   // one line of content at `row`

struct layout_item {
    uint16_t kind;
    uint16_t row;
    uint16_t height;     // 1 for lines; band span otherwise
    uint16_t box_left;   // band: box left column
    uint16_t box_width;  // band: width
    uint32_t box_bg;     // band: fill colour (gradient start)
    uint32_t box_c1;     // band: gradient end colour
    uint32_t box_border; // band: border colour (0 = none)
    uint8_t  draw_box;   // band: 0=none 1=solid fill/border 2=vertical gradient 3=horizontal
    uint8_t  heading;    // line: heading level 1..6, 0 = body text. The
                         // renderer draws levels 1/2/3 at 3x/2x/1x via the
                         // pixel overlay (the grid glyph is a placeholder).
    uint8_t  is_fixed;   // line/band: position:fixed viewport-pinned row —
                         // blit maps row without the scroll offset
    uint8_t  is_oof;     // line/band: out-of-flow (absolute/fixed) box —
                         // blit paints these AFTER normal flow (on top)
    uint16_t run_start, run_count;
};

struct layout {
    struct layout_item items[LAYOUT_MAX_ITEMS];
    struct layout_run  runs[LAYOUT_MAX_RUNS];
    char     text[LAYOUT_MAX_TEXT];
    int      n_items, n_runs, text_len;
    int      height;      // total document rows (>= 1)
    int      width, page_left;
    uint32_t page_bg, page_fg;
    int      truncated;
};

struct layout_opts {
    int      width_cols; // viewport content width for wrapping
    int      page_left;  // left margin (grid cols)
    uint32_t page_bg;
    uint32_t page_fg;
};

// Block test on a DOM node (wrapper so callers need not carry a tag buffer).
int  dom_is_block_node(const struct dom* d, int node);

void layout_run(const struct dom* d, const struct css_rule* rules, int n_rules,
                const struct layout_opts* opt, struct layout* out);

#endif