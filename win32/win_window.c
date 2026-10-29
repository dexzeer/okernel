// okai on Windows -- the window manager, reduced to what the browser uses.
//
// The kernel's window.c owns a desktop, z-order, taskbar buttons and animation
// (src/winfx.c). The port shows exactly one window: it fills the client area,
// draws its own chrome (no title bar) and owns a pixel surface for the page.
// The paint path mirrors window_paint_region(): full repaints fill the content
// area with the page background and blit the surface; partial repaints
// (window_dirty_pixels, e.g. a video frame) blit only their sub-rect.

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "window.h"
#include "theme.h"
#include "win_plat.h"

const uint32_t vga_to_rgb[16] = {
    0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
    0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
    0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
    0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF,
};

static struct window wins[MAX_WINDOWS];
static int win_count;

static int valid(int id) { return id >= 0 && id < MAX_WINDOWS && wins[id].visible; }

struct window* window_get(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    return &wins[id];
}

static void set_win_title(const char* title) {
    if (!win_hwnd || !title) return;
    wchar_t w[128];
    int n = MultiByteToWideChar(CP_UTF8, 0, title, -1, w, 127);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, title, -1, w, 127);
    if (n > 0) { w[n - 1] = 0; SetWindowTextW(win_hwnd, w); }
}

int window_create(const char* title, int x, int y, int w, int h) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].visible) continue;
        struct window* win = &wins[i];
        memset(win, 0, sizeof *win);
        win->x = x; win->y = y; win->w = w; win->h = h;
        win->visible = 1;
        win->z = 1;
        win->font_scale = 1;
        win->content_bg_rgb = 0x00FFFFFF;
        win->text_fg_rgb = 0x00000000;
        win->text_bg_rgb = 0x00FFFFFF;
        win->dirty = 1;
        win->pxd_valid = 0;
        int j = 0;
        while (title && title[j] && j < 31) { win->title[j] = title[j]; j++; }
        win->title[j] = 0;
        win_count++;
        set_win_title(title ? title : "okai");
        return i;
    }
    return -1;
}

void window_destroy(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    wins[id].visible = 0;
    wins[id].pix = 0;
    wins[id].pxd_valid = 0;
    win_count--;
}

void window_set_focus(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    for (int i = 0; i < MAX_WINDOWS; i++) wins[i].focused = (i == id);
}

int window_get_focused(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i].visible && wins[i].focused) return i;
    return -1;
}

void window_raise(int id) { (void)id; }

int window_from_point(int x, int y) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* w = &wins[i];
        if (!w->visible || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return i;
    }
    return -1;
}

void window_set_dirty(int id) {
    if (!valid(id)) return;
    wins[id].dirty = 1;
    wins[id].pxd_valid = 0;      // full repaint
}

void window_set_pixels(int id, const uint32_t* px, int x, int y, int w, int h, int stride) {
    if (!valid(id)) return;
    struct window* win = &wins[id];
    win->pix = px;
    win->pix_x = x; win->pix_y = y;
    win->pix_w = w; win->pix_h = h;
    win->pix_stride = stride;
    win->pxd_valid = 0;
    win->dirty = 1;
}

void window_dirty_pixels(int id, int x, int y, int w, int h) {
    if (!valid(id)) return;
    struct window* win = &wins[id];
    if (!win->pix || w <= 0 || h <= 0) return;
    if (win->dirty && !win->pxd_valid) return;   // a full repaint is already pending
    if (win->pxd_valid) {
        if (x < win->pxd_x0) win->pxd_x0 = x;
        if (y < win->pxd_y0) win->pxd_y0 = y;
        if (x + w > win->pxd_x1) win->pxd_x1 = x + w;
        if (y + h > win->pxd_y1) win->pxd_y1 = y + h;
    } else {
        win->pxd_x0 = x; win->pxd_y0 = y;
        win->pxd_x1 = x + w; win->pxd_y1 = y + h;
        win->pxd_valid = 1;
    }
    win->dirty = 1;
}

// Blit the whole page surface at its content position.
static void blit_surface(struct window* w, int cx, int cy, int cw, int ch) {
    if (!w->pix) return;
    int sx = cx + w->pix_x, sy = cy + w->pix_y;
    int sw = w->pix_w, sh = w->pix_h;
    if (sx + sw > cx + cw) sw = cx + cw - sx;
    if (sy + sh > cy + ch) sh = cy + ch - sy;
    if (sx < cx) { sw -= cx - sx; sx = cx; }
    if (sy < cy) { sh -= cy - sy; sy = cy; }
    if (sw <= 0 || sh <= 0) return;
    graphics_blit_pixels(sx, sy, w->pix, sw, sh, w->pix_stride);
}

int window_draw(int id) {
    if (!valid(id)) return 0;
    struct window* w = &wins[id];
    if (w->minimized || !w->dirty) return 0;

    int title_off = w->no_titlebar ? 0 : WIN_TITLE_H;
    int cx = w->x + WIN_BORDER;
    int cy = w->y + WIN_BORDER + title_off;
    int cw = w->w - 2 * WIN_BORDER;
    int ch = w->h - title_off - 2 * WIN_BORDER;

    if (w->pix && w->pxd_valid) {
        // Partial: only the changed sub-rect of the surface.
        int x0 = w->pxd_x0, y0 = w->pxd_y0, x1 = w->pxd_x1, y1 = w->pxd_y1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w->pix_w) x1 = w->pix_w;
        if (y1 > w->pix_h) y1 = w->pix_h;
        if (x1 > x0 && y1 > y0) {
            int px = cx + w->pix_x + x0, py = cy + w->pix_y + y0;
            graphics_blit_pixels(px, py, w->pix + (size_t)y0 * w->pix_stride + x0,
                                 x1 - x0, y1 - y0, w->pix_stride);
        }
        w->dirty = 0;
        w->pxd_valid = 0;
        return 1;
    }

    rect_fill(w->x, w->y, w->w, w->h, w->content_bg_rgb);
    rect_outline(w->x, w->y, w->w, w->h,
                 w->focused ? FX_FRAME_BORDER : FX_FRAME_BORDER_U, WIN_BORDER);
    blit_surface(w, cx, cy, cw, ch);
    w->dirty = 0;
    w->pxd_valid = 0;
    return 1;
}

void window_draw_all(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) window_draw(i);
}

void window_clear(int id) { window_set_dirty(id); }

void window_set_title(int id, const char* title) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    int j = 0;
    while (title && title[j] && j < 31) { wins[id].title[j] = title[j]; j++; }
    wins[id].title[j] = 0;
    set_win_title(title ? title : "okai");
}

void window_set_close_button(int id, int has) { if (id >= 0 && id < MAX_WINDOWS) wins[id].has_close_button = has; }
void window_set_minimize_button(int id, int has) { if (id >= 0 && id < MAX_WINDOWS) wins[id].has_minimize_button = has; }
void window_set_no_titlebar(int id, int flag) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    wins[id].no_titlebar = flag;
    wins[id].dirty = 1; wins[id].pxd_valid = 0;
}
void window_set_hide_cursor(int id, int flag) { if (id >= 0 && id < MAX_WINDOWS) wins[id].hide_cursor = flag; }
void window_set_popup(int id) { if (id >= 0 && id < MAX_WINDOWS) wins[id].is_popup = 1; }
void window_set_hidden(int id) { if (id >= 0 && id < MAX_WINDOWS) wins[id].visible = 0; }
void window_set_terminal(int id, int flag) { if (id >= 0 && id < MAX_WINDOWS) wins[id].is_term = flag; }
void window_set_content_bg(int id, uint8_t bg) { (void)id; (void)bg; }

void window_set_content_bg_rgb(int id, uint32_t bg) {
    if (!valid(id)) return;
    if (wins[id].content_bg_rgb != bg) {
        wins[id].content_bg_rgb = bg;
        wins[id].dirty = 1;
        wins[id].pxd_valid = 0;
    }
}

void window_set_font_scale(int id, int scale) { if (id >= 0 && id < MAX_WINDOWS) wins[id].font_scale = scale; }
void window_set_text_color(int id, uint8_t fg, uint8_t bg) { (void)id; (void)fg; (void)bg; }

void window_set_text_color_rgb(int id, uint32_t fg, uint32_t bg) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    wins[id].text_fg_rgb = fg;
    wins[id].text_bg_rgb = bg;
}

int window_is_maximized(int id) { return valid(id) ? 1 : 0; }
void window_toggle_maximize(int id) { (void)id; }
void window_minimize(int id) { if (valid(id)) wins[id].minimized = 1; }
void window_restore(int id) { if (id >= 0 && id < MAX_WINDOWS) wins[id].minimized = 0; }

void window_resize(int id, int w, int h) {
    if (!valid(id)) return;
    wins[id].w = w;
    wins[id].h = h;
    window_set_dirty(id);
}

// The three caption cells okai draws itself in the tab strip (min | max | close).
int window_ctrl_rect(int id, int which, int r[4]) {
    if (!valid(id)) return 0;
    struct window* w = &wins[id];
    if (!w->no_titlebar) return 0;
    if (which == WIN_CTRL_CLOSE && !w->has_close_button) return 0;
    if (which != WIN_CTRL_CLOSE && !w->has_minimize_button) return 0;
    r[0] = w->x + w->w - WIN_BORDER - (3 - which) * WIN_CTRL_W;
    r[1] = w->y + WIN_BORDER;
    r[2] = WIN_CTRL_W;
    r[3] = CHROME_TAB_H;
    return 1;
}

static int ctrl_hit(int id, int which, int mx, int my) {
    int r[4];
    if (!window_ctrl_rect(id, which, r)) return 0;
    return mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
}

int window_check_close_click(int id, int mx, int my) { return ctrl_hit(id, WIN_CTRL_CLOSE, mx, my); }
int window_check_minimize_click(int id, int mx, int my) { return ctrl_hit(id, WIN_CTRL_MIN, mx, my); }
int window_check_maximize_click(int id, int mx, int my) { return ctrl_hit(id, WIN_CTRL_MAX, mx, my); }
int window_check_resize_grip(int id, int mx, int my) { (void)id; (void)mx; (void)my; return 0; }

// WM_SIZE: the client area changed. gfx_w/gfx_h follow it, the single window is
// re-fitted to the whole client area (no taskbar in the port) and okai re-lays
// out its page for the new viewport.
void win_window_fit_full(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].visible) continue;
        wins[i].x = 0;
        wins[i].y = 0;
        wins[i].w = gfx_w;
        wins[i].h = gfx_h;
        wins[i].dirty = 1;
        wins[i].pxd_valid = 0;
    }
}

void win_window_resize(int client_w, int client_h) {
    if (client_w < 64 || client_h < 64) return;
    win_gfx_resize(client_w, client_h);
    win_window_fit_full();
}
