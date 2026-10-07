#ifndef UI_DRAW_H
#define UI_DRAW_H

// Anti-aliased UI drawing shared by the okai chrome, the taskbar and the
// window title bars: the web engine's path rasterizer + Noto Sans text on a
// struct wsurf. Implemented in okai_ui.c (where it grew up). Main-stack safe.
#include <stdint.h>
#include "web/surface.h"

// Stroke pen: coordinates are WR units (1/256 px) relative to the origin.
struct ui_pen { int32_t ox, oy, hw; };
#define UI_U(v) ((int32_t)((v) * 256))

void ui_pen_at(struct ui_pen* p, int x, int y, int hw256);
void ui_path_begin(void);
// Fill the current path; c = 0x00RRGGBB (opaque) or AARRGGBB with A < 0xFF.
void ui_path_fill(struct wsurf* s, uint32_t c);
void ui_seg(const struct ui_pen* p, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int caps);
void ui_polyline(const struct ui_pen* p, const int32_t* pts, int n);
void ui_arc(const struct ui_pen* p, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int a0, int sweep);
void ui_rrect(struct wsurf* s, int x, int y, int w, int h, int r, uint32_t c);
int  ui_text_w(int px, int bold, const char* str, int len);
// Text vertically centered on cy, clipped to maxw (fades into bg on overflow).
int  ui_text_draw(struct wsurf* s, int x, int cy, int px, int bold, uint32_t c,
                  const char* str, int len, int maxw, uint32_t bg);

// A wsurf over the desktop backbuffer, clipped to the current graphics clip.
// Call ui_screen_done(y0, h) afterwards to mark the touched rows dirty.
void ui_screen_surf(struct wsurf* s);
void ui_screen_done(int y0, int h);

#endif
