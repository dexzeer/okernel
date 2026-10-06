#ifndef WEB_RASTER_H
#define WEB_RASTER_H

#include <stdint.h>

// Anti-aliased polygon rasterizer (integer only). Paths are built from
// lines and quadratic/cubic Beziers in 24.8 fixed-point pixels (1/256 px),
// flattened into an edge list, then swept in 64-row bands with exact-area
// cell accumulation (the classic FreeType/AGG "cover + area" scheme). The
// output is one coverage row (0-255) per scanline, handed to a callback.
//
// Shared by glyph rendering, rounded boxes/borders, and SVG paths.

#define WR_SHIFT 8
#define WR_ONE   256           // 1px in raster subpixel units
#define WR_FIX(px) ((int32_t)(px) << WR_SHIFT)

struct wr_edge { int32_t x0, y0, x1, y1; };

struct wraster {
    struct wr_edge* e;
    int ne, cap;
    int32_t cx, cy;            // current point
    int32_t sx, sy;            // subpath start
    int has_sub;
    int32_t minx, miny, maxx, maxy; // edge bounds (subpx)
    // scratch, grown on demand
    int32_t* cover;
    int32_t* area;
    uint8_t* row;
    int scratch_cells, scratch_w;
};

typedef void (*wr_span_fn)(void* ctx, int y, int x, int len, const uint8_t* cov);

void wr_init(struct wraster* r);
void wr_free(struct wraster* r);
void wr_reset(struct wraster* r);              // drop the path, keep scratch
void wr_move(struct wraster* r, int32_t x, int32_t y);
void wr_line(struct wraster* r, int32_t x, int32_t y);
void wr_quad(struct wraster* r, int32_t cx, int32_t cy, int32_t x, int32_t y);
void wr_cubic(struct wraster* r, int32_t c1x, int32_t c1y, int32_t c2x, int32_t c2y,
              int32_t x, int32_t y);
void wr_close(struct wraster* r);
// Convenience shapes (subpx coords): rectangle and rounded rectangle with
// per-corner radii (tl, tr, br, bl). Elliptical arcs approximated by cubics.
void wr_rect(struct wraster* r, int32_t x, int32_t y, int32_t w, int32_t h);
void wr_rrect(struct wraster* r, int32_t x, int32_t y, int32_t w, int32_t h,
              const int32_t rad[4]);
void wr_ellipse(struct wraster* r, int32_t cx, int32_t cy, int32_t rx, int32_t ry);
// Fill the path, clipped to the pixel box [x0,x1) x [y0,y1). Calls fn once per
// covered scanline with the coverage of pixels [x, x+len). evenodd selects
// the fill rule (else nonzero). Returns 0 if nothing was drawn.
int  wr_fill(struct wraster* r, int x0, int y0, int x1, int y1, int evenodd,
             wr_span_fn fn, void* ctx);
// Path bounds in whole pixels (after flattening); 0 when empty.
int  wr_bounds(const struct wraster* r, int* x0, int* y0, int* x1, int* y1);

#endif
