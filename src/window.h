#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define MAX_WINDOWS 8
#define WIN_TITLE_H 12
#define WIN_BORDER 2

// Window colors — modern dark theme
#define WIN_TITLE_BG 1    // Dark blue
#define WIN_TITLE_FG 15   // White
#define WIN_BG 0          // Black
#define WIN_BORDER_BG 8   // Dark grey
#define WIN_ACTIVE_BORDER 9  // Light blue border when focused

struct window {
    int x, y, w, h;
    int visible;
    int focused;
    int minimized;
    int has_close_button;
    int has_minimize_button;
    int dirty; // Needs redraw
    int last_cursor_visible;
    char title[32];
    uint16_t* content;
    int cursor_x, cursor_y;
    int content_w, content_h;
    int font_scale;
    uint8_t text_fg;
    uint8_t text_bg;
};

void window_init(void);
int window_create(const char* title, int x, int y, int w, int h);
void window_destroy(int id);
void window_set_focus(int id);
int window_get_focused(void);
void window_draw(int id);
void window_draw_all(void);
void window_put_char(int id, char c);
void window_puts(int id, const char* str);
void window_clear(int id);
void window_set_font_scale(int id, int scale);
void window_set_close_button(int id, int has_close);
void window_set_minimize_button(int id, int has_min);
int window_check_close_click(int id, int mx, int my);
int window_check_minimize_click(int id, int mx, int my);
void window_minimize(int id);
void window_restore(int id);
void window_set_text_color(int id, uint8_t fg, uint8_t bg);
void window_draw_taskbar(void);
struct window* window_get(int id);

// Mouse
void mouse_init_fb(void);
void mouse_draw_cursor(void);
void mouse_hide_cursor(void);
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_get_left_button(void);

#endif
