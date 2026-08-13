#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#define MAX_WINDOWS 8
#define WIN_TITLE_H 12
#define WIN_BORDER 2

// Window colors
#define WIN_TITLE_BG 1    // Blue
#define WIN_TITLE_FG 15   // White
#define WIN_BG 0          // Black
#define WIN_BORDER_BG 7   // Light grey
#define WIN_ACTIVE_BORDER 1  // Blue border when focused

struct window {
    int x, y, w, h;
    int visible;
    int focused;
    int has_close_button;
    char title[32];
    uint16_t* content;
    int cursor_x, cursor_y;
    int content_w, content_h;
    int font_scale;
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
int window_check_close_click(int id, int mx, int my);
struct window* window_get(int id);

// Mouse
void mouse_init_fb(void);
void mouse_draw_cursor(void);
void mouse_hide_cursor(void);
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_get_left_button(void);

#endif
