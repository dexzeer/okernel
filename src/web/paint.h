#ifndef WEB_PAINT_H
#define WEB_PAINT_H

#include "layout.h"
#include "surface.h"

// Decoded image: straight (non-premultiplied) ARGB pixels.
struct wimg { int w, h; const uint32_t* px; };

struct wpaint_env {
    const struct wstyleset* ss;          // gradients
    struct wdom* d;                      // inline SVG
    const struct wimg* (*image)(void* ctx, int id);
    void* ctx;
};

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
