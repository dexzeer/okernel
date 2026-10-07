#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include "graphics.h"

// VGA index -> 0x00RRGGBB (defined in window.c; okai draws headings as raw
// pixels and needs this so heading colors match the body text exactly)
extern const uint32_t vga_to_rgb[16];

// Default content background (VGA index 0) as exact RGB.
#define WIN_BG_RGB 0x00000000

#define MAX_WINDOWS 8
// Layout constants derived from glyph size — never hardcode pixels;
// these track FONT_SCALE automatically
#define WIN_TITLE_H (CHAR_H + 8)
#define WIN_BORDER 2
#define TASKBAR_H (CHAR_H + 12)
#define WIN_BTN_W 30
#define WIN_BTN_H 30   // square chrome buttons (hit-test uses these too)
#define MIN_WIN_W 120
#define MIN_WIN_H 80
#define RESIZE_GRIP 16   // clickable corner zone
#define WIN_GRIP_SIZE 12 // visible triangle legs
#define WIN_CTRL_BTN 18  // size of the top-right close control for no-titlebar windows

#define CURSOR_W 12
#define CURSOR_H 16

// Window content colors (VGA palette indices; the title bar / border / button
// chrome is drawn with RGB theme colors from theme.h)
#define WIN_BG 0

struct window {
    int x, y, w, h;
    int visible;
    int focused;
    int minimized;
    int z;              // stacking order; higher = drawn on top. Raised on focus /
                       // interaction so the window you click is always topmost
                       // (and its pixels correctly occlude lower windows).
    int has_close_button;
    int has_minimize_button;
    int no_titlebar;   // when set, the window has no title bar; the client draws
                       // its own chrome at the top (used by the browser window)
    int dirty;
    int last_cursor_visible;
    char title[32];
    uint16_t* content;
    // Exact-color planes parallel to `content` (one 0xRRGGBB value per cell).
    // RGB is the paint-time source of truth; the VGA attr byte in `content`
    // is kept in sync for legacy callers. History rows in the scrollback ring
    // are VGA-indexed (terminals only — okai never scrolls the window grid).
    uint32_t* cell_fg;
    uint32_t* cell_bg;
    // Per-cell attribute bits (CELL_BOLD/CELL_UL from graphics.h), parallel
    // to content/cell_fg/cell_bg. Zero for plain cells. NOT carried into the
    // scrollback ring (history is VGA-indexed): scrolled-back bold/underline
    // repaints plain — terminals only, okai re-blits live rows on scroll.
    uint8_t* cell_attr;
    int cursor_x, cursor_y;
    int content_w, content_h;
    int font_scale;
    uint8_t text_fg;
    uint8_t text_bg;
    uint32_t text_fg_rgb, text_bg_rgb;   // exact colors behind text_fg/text_bg
    uint8_t content_bg;  // page background color index (set by okai from <body>)
    uint32_t content_bg_rgb;             // page background as 0xRRGGBB
    int scroll_off;      // scrollback view offset (0 = live tail; see window.c)
    int hide_cursor;     // no blinking text cursor (browser windows)
    // Partial-dirty cell range (inclusive). When dirty==1 and pr_valid==1,
    // only this cell range (+ cursor cell) is repainted instead of the whole
    // window — a keystroke repaints 1-2 cells, not ~2000. pr_valid==0 means
    // full repaint (create/resize/clear/focus-affecting changes). New fields
    // go at the TAIL (PCB-offset rule applies to struct process, same habit).
    int pr_valid;
    int pr_c0, pr_r0, pr_c1, pr_r1;
    int red_chrome;  // anarchy terminal chrome: flat red title bar + border,
                     // centered title, square black buttons. Set explicitly
                     // per window (reset in window_create against slot reuse).
    // Client pixel surface (okai page): when set, the content area paints
    // these pixels instead of the cell grid. Placed at (pix_x, pix_y)
    // relative to the content origin; the rest of the content area gets
    // content_bg_rgb. Owned by the client (never freed here).
    const uint32_t* pix;
    int pix_x, pix_y, pix_w, pix_h, pix_stride;
    // Partial pixel repaint (surface coords, inclusive-exclusive); valid only
    // while dirty is set and no full repaint is pending.
    int pxd_valid, pxd_x0, pxd_y0, pxd_x1, pxd_y1;
};

void window_init(void);
int window_create(const char* title, int x, int y, int w, int h);
void window_destroy(int id);
void window_set_focus(int id);
void window_raise(int id);   // bump a window to the top of the z-stack
int window_get_focused(void);
int window_draw(int id); // returns 1 if anything was painted (drives okai overlay throttle)
void window_draw_all(void);
void window_set_dirty(int id); // mark a window for re-render (used by animating chrome)
void window_paint_region(int id, int rx, int ry, int rw, int rh);
void window_put_char(int id, char c);
void window_puts(int id, const char* str);
// Write one content cell directly (no cursor movement) — used by the okai
// to blit a scrolled slice of its virtual document into the buffer.
void window_write_cell(int id, int row, int col, char c, uint8_t fg, uint8_t bg);
// Same, with exact 0xRRGGBB colors (true-color blit path).
void window_write_cell_rgb(int id, int row, int col, char c, uint32_t fg, uint32_t bg);
// Set a blitted cell's attribute bits (CELL_BOLD/CELL_UL) after
// window_write_cell_rgb (which zeroes them). Used by the okai blit.
void window_write_cell_attr(int id, int row, int col, uint8_t attr);
// CJK model cell for the okai blit (bit-packs the codepoint; see window.c).
// style carries CELL_BOLD/CELL_UL only (bits 1:0).
void window_write_cjk_cell(int id, int row, int col, uint32_t cp,
                           uint32_t fg, uint32_t bg, uint8_t style);
void window_clear(int id);
void window_set_font_scale(int id, int scale);
void window_set_close_button(int id, int has_close);
void window_set_minimize_button(int id, int has_min);
void window_set_red_chrome(int id, int flag); // flat-red anarchy terminal chrome
void window_set_no_titlebar(int id, int flag);
void window_set_hide_cursor(int id, int flag);
// Attach (px != NULL) or detach a client pixel surface; marks the window dirty.
void window_set_pixels(int id, const uint32_t* px, int x, int y, int w, int h, int stride);
// Repaint only a sub-rect of the pixel surface (surface coordinates).
void window_dirty_pixels(int id, int x, int y, int w, int h);
int window_check_close_click(int id, int mx, int my);
int window_check_minimize_click(int id, int mx, int my);
void window_minimize(int id);
void window_restore(int id);
void window_resize(int id, int w, int h);
int window_check_resize_grip(int id, int mx, int my);
void window_set_text_color(int id, uint8_t fg, uint8_t bg);
void window_set_text_color_rgb(int id, uint32_t fg, uint32_t bg);
void window_scroll_view(int id, int notches); // wheel scrollback (terminals; positive = down/tail)
void window_set_cursor(int id, int row, int col);
void window_set_content_bg(int id, uint8_t bg);
void window_set_content_bg_rgb(int id, uint32_t bg);
int window_color_is_light(uint8_t idx);
int window_rgb_is_light(uint32_t c);
void window_draw_taskbar(void);
struct window* window_get(int id);
void window_get_cursor(int id, int* out_x, int* out_y);
void window_set_title(int id, const char* title);

void mouse_init_fb(void);
void mouse_get_position(int* x, int* y);
void mouse_paint_cursor(int px, int py);
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_get_left_button(void);
int mouse_get_scroll(void);
int window_from_point(int x, int y);

#endif
