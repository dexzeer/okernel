#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include "graphics.h"

// VGA index -> 0x00RRGGBB (defined in window.c; okai draws headings as raw
// pixels and needs this so heading colors match the body text exactly)
extern const uint32_t vga_to_rgb[16];

#define MAX_WINDOWS 8
// Layout constants derived from glyph size — never hardcode pixels;
// these track FONT_SCALE automatically
#define WIN_TITLE_H (CHAR_H + 8)
#define WIN_BORDER 2
#define TASKBAR_H (CHAR_H + 12)
#define WIN_BTN_W (CHAR_W + 8)
#define WIN_BTN_H (CHAR_H + 4)
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
    int has_close_button;
    int has_minimize_button;
    int no_titlebar;   // when set, the window has no title bar; the client draws
                       // its own chrome at the top (used by the browser window)
    int dirty;
    int last_cursor_visible;
    char title[32];
    uint16_t* content;
    int cursor_x, cursor_y;
    int content_w, content_h;
    int font_scale;
    uint8_t text_fg;
    uint8_t text_bg;
    uint8_t content_bg;  // page background color (set by okai from <body>)
    int scroll_off;      // scrollback view offset (0 = live tail; see window.c)
};

void window_init(void);
int window_create(const char* title, int x, int y, int w, int h);
void window_destroy(int id);
void window_set_focus(int id);
int window_get_focused(void);
void window_draw(int id);
void window_draw_all(void);
void window_paint_region(int id, int rx, int ry, int rw, int rh);
void window_put_char(int id, char c);
void window_puts(int id, const char* str);
// Write one content cell directly (no cursor movement) — used by the okai
// to blit a scrolled slice of its virtual document into the buffer.
void window_write_cell(int id, int row, int col, char c, uint8_t fg, uint8_t bg);
void window_clear(int id);
void window_set_font_scale(int id, int scale);
void window_set_close_button(int id, int has_close);
void window_set_minimize_button(int id, int has_min);
void window_set_no_titlebar(int id, int flag);
int window_check_close_click(int id, int mx, int my);
int window_check_minimize_click(int id, int mx, int my);
void window_minimize(int id);
void window_restore(int id);
void window_resize(int id, int w, int h);
int window_check_resize_grip(int id, int mx, int my);
void window_set_text_color(int id, uint8_t fg, uint8_t bg);
void window_scroll_view(int id, int notches); // wheel scrollback (terminals; positive = down/tail)
void window_set_cursor(int id, int row, int col);
void window_set_content_bg(int id, uint8_t bg);
int window_color_is_light(uint8_t idx);
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
