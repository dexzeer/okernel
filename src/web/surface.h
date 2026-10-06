#ifndef WEB_SURFACE_H
#define WEB_SURFACE_H

#include <stdint.h>

// A 32bpp XRGB pixel target with a clip rectangle. The engine paints pages
// into one of these (the browser's per-window page surface in the kernel, a
// PNG buffer in host tests). Colors passed to drawing calls are ARGB with
// A = opacity (0xFF opaque); the stored pixels keep A = 0.

struct wsurf {
    uint32_t* px;
    int w, h, stride;          // stride in pixels
    int cx0, cy0, cx1, cy1;    // clip rect [cx0,cx1) x [cy0,cy1)
};

static inline void ws_reset_clip(struct wsurf* s) {
    s->cx0 = 0; s->cy0 = 0; s->cx1 = s->w; s->cy1 = s->h;
}

// Intersect the clip with a rect; returns the previous clip in old[4].
static inline void ws_push_clip(struct wsurf* s, int x0, int y0, int x1, int y1, int old[4]) {
    old[0] = s->cx0; old[1] = s->cy0; old[2] = s->cx1; old[3] = s->cy1;
    if (x0 > s->cx0) s->cx0 = x0;
    if (y0 > s->cy0) s->cy0 = y0;
    if (x1 < s->cx1) s->cx1 = x1;
    if (y1 < s->cy1) s->cy1 = y1;
}
static inline void ws_pop_clip(struct wsurf* s, const int old[4]) {
    s->cx0 = old[0]; s->cy0 = old[1]; s->cx1 = old[2]; s->cy1 = old[3];
}

// dst = dst*(255-a) + src*a, per channel, a in 0..255.
static inline uint32_t ws_blend(uint32_t dst, uint32_t src, unsigned a) {
    if (a >= 255) return src & 0xFFFFFF;
    if (a == 0) return dst;
    uint32_t rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (255 - a) + 0x800080);
    uint32_t g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (255 - a) + 0x008000);
    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    return rb | g;
}

void ws_fill_rect(struct wsurf* s, int x, int y, int w, int h, uint32_t argb);
// Blend an 8-bit coverage mask (w x h, row stride mstride) at (x, y).
void ws_mask(struct wsurf* s, int x, int y, int w, int h,
             const uint8_t* mask, int mstride, uint32_t argb);

#endif
