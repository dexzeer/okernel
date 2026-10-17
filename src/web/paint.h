#ifndef WEB_PAINT_H
#define WEB_PAINT_H

#include "layout.h"
#include "surface.h"

// Decoded image: straight (non-premultiplied) ARGB pixels.
struct wimg { int w, h; const uint32_t* px; int opaque; };   // opaque: every pixel has alpha 0xFF

struct wpaint_env {
    const struct wstyleset* ss;          // gradients
    struct wdom* d;                      // inline SVG
    const struct wimg* (*image)(void* ctx, int id);
    void* ctx;
    uint32_t (*image_sig)(void* ctx, int id);   // identity of what image `id` would draw (0 = it changes by itself: always repaint)
};

// What the last paint left in the surface (see wpaint_inc in paint.c).
struct wpaint_cache {
    uint32_t* sig;
    uint8_t* always;
    void* rect;                                 // struct drect[] (paint.c)
    int n, cap, scroll, w, h;
    const uint32_t* px;
    uint32_t bg;
    int valid;
};
extern int wpaint_inc_enabled;
extern int wpaint_dbg;         // log the damage rectangles
void wpaint_cache_free(struct wpaint_cache* pc);
// Like wpaint, but repaints only what changed since the last call with the same cache when that gives the same picture. Returns the
// number of pixels painted. The surface must still hold what the last call left (video rectangles aside: those are ext images).
long wpaint_inc(const struct wlayout* L, struct wsurf* s, int scroll_y, const struct wpaint_env* env, struct wpaint_cache* pc);

// Paint the display list into s. The surface shows document rows
// [scroll_y, scroll_y + s->h) (px); fixed items ignore the scroll. The
// canvas background is filled first.
void wpaint(const struct wlayout* L, struct wsurf* s, int scroll_y, const struct wpaint_env* env);

// Paint an inline <svg> element (svg.c) into the rect (px, surface space).
void wsvg_paint(struct wsurf* s, struct wdom* d, const struct wstyleset* ss, int node,
                int x, int y, int w, int h, uint32_t current_color, uint8_t alpha);

// Scaled, clipped image blit (area-averaging when shrinking).
void wpaint_image(struct wsurf* s, const struct wimg* im, int dx, int dy, int dw, int dh, uint8_t alpha);

#endif
