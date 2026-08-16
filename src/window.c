#include "window.h"
#include "graphics.h"
#include "memory.h"
#include "idt.h"
#include "io.h"
#include <stdint.h>

extern int needs_redraw;

// VGA palette lookup for content rendering
static const uint32_t vga_to_rgb[16] = {
    0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
    0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
    0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
    0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF,
};

static struct window windows[MAX_WINDOWS];

// Content buffers are allocated ONCE at max size (full-screen window at
// scale 1) with this fixed stride. Resizing then never reallocates —
// kfree is a no-op on the bump heap, and per-frame reallocs during a
// resize drag would burn it down fast. ALL content indexing must use
// CONTENT_COLS_MAX as the row stride, never content_w.
#define CONTENT_COLS_MAX ((SCREEN_W - 2 * WIN_BORDER) / CHAR_W)
#define CONTENT_ROWS_MAX ((SCREEN_H - WIN_TITLE_H - 2 * WIN_BORDER) / CHAR_H)
#define CONTENT_CELLS_MAX (CONTENT_COLS_MAX * CONTENT_ROWS_MAX)

// Mouse state
static int mouse_x = 160, mouse_y = 100;
static int mouse_buttons = 0, mouse_cycle = 0;
static int8_t mouse_bytes[3];
#define SMOOTH_SAMPLES 4
static int smooth_dx[SMOOTH_SAMPLES], smooth_dy[SMOOTH_SAMPLES], smooth_idx = 0;

static const uint16_t cursor_bitmap[16] = {
    0b110000000000, 0b111000000000, 0b111100000000, 0b111110000000,
    0b111111000000, 0b111111100000, 0b111111110000, 0b111111111000,
    0b111111000000, 0b111111100000, 0b110111110000, 0b110011111000,
    0b110001111000, 0b100000111100, 0b000000011100, 0b000000001100,
};

static void mouse_irq_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;
    uint8_t data = inb(0x60);
    switch (mouse_cycle) {
        case 0: mouse_bytes[0] = data; if (data & 0x08) mouse_cycle++; break;
        case 1: mouse_bytes[1] = data; mouse_cycle++; break;
        case 2:
            mouse_bytes[2] = data; mouse_cycle = 0;
            mouse_buttons = mouse_bytes[0] & 0x07;
            smooth_dx[smooth_idx] = (int8_t)mouse_bytes[1];
            smooth_dy[smooth_idx] = -(int8_t)mouse_bytes[2];
            smooth_idx = (smooth_idx + 1) % SMOOTH_SAMPLES;
            int avg_dx = 0, avg_dy = 0;
            for (int i = 0; i < SMOOTH_SAMPLES; i++) { avg_dx += smooth_dx[i]; avg_dy += smooth_dy[i]; }
            mouse_x += avg_dx / SMOOTH_SAMPLES;
            mouse_y += avg_dy / SMOOTH_SAMPLES;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;
            break;
    }
}

void mouse_get_position(int* x, int* y) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    *x = mouse_x; *y = mouse_y;
    if (flags & 0x0200) __asm__ volatile("sti");
}

void mouse_paint_cursor(int px, int py) {
    for (int row = 0; row < CURSOR_H; row++)
        for (int col = 0; col < CURSOR_W; col++)
            if (cursor_bitmap[row] & (1 << (CURSOR_W - 1 - col))) {
                int is_edge = (row == 0 || col == 0 ||
                    !(cursor_bitmap[row-1] & (1 << (CURSOR_W - 1 - col))) ||
                    !(cursor_bitmap[row] & (1 << (CURSOR_W - col))));
                putpixel(px + col, py + row, is_edge ? 0x00000000 : 0x00FFFFFF);
            }
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
int mouse_get_left_button(void) { return mouse_buttons & 0x01; }

void mouse_init_fb(void) {
    mouse_x = SCREEN_W / 2; mouse_y = SCREEN_H / 2;
    mouse_buttons = 0; mouse_cycle = 0;
    while (inb(0x64) & 0x02); outb(0x64, 0xA8);
    while (inb(0x64) & 0x02); outb(0x64, 0x20);
    while (inb(0x64) & 0x02); uint8_t s = inb(0x60); s |= 0x02; s &= ~0x20;
    while (inb(0x64) & 0x02); outb(0x64, 0x60);
    while (inb(0x64) & 0x02); outb(0x60, s);
    while (inb(0x64) & 0x02); outb(0x64, 0xD4);
    while (inb(0x64) & 0x02); outb(0x60, 0xFF);
    while (inb(0x64) & 0x02); inb(0x60);
    while (inb(0x64) & 0x02); outb(0x64, 0xD4);
    while (inb(0x64) & 0x02); outb(0x60, 0xF4);
    while (inb(0x64) & 0x01) inb(0x60);
    irq_register_handler(12, mouse_irq_handler);
}

// ---- Window Manager ----

void window_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) { windows[i].visible = 0; windows[i].focused = 0; windows[i].content = 0; }
}

int window_create(const char* title, int x, int y, int w, int h) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].visible) {
            windows[i].x = x; windows[i].y = y; windows[i].w = w; windows[i].h = h;
            windows[i].visible = 1; windows[i].focused = 0; windows[i].font_scale = 1;
            windows[i].text_fg = 15; windows[i].text_bg = 0;
            windows[i].dirty = 1; needs_redraw = 1;
            int j = 0;
            while (title[j] && j < 31) { windows[i].title[j] = title[j]; j++; }
            windows[i].title[j] = 0;
            int cw = (w - 2 * WIN_BORDER) / CHAR_W;
            int ch = (h - WIN_TITLE_H - 2 * WIN_BORDER) / CHAR_H;
            if (cw > CONTENT_COLS_MAX) cw = CONTENT_COLS_MAX;
            if (ch > CONTENT_ROWS_MAX) ch = CONTENT_ROWS_MAX;
            windows[i].content_w = cw;
            windows[i].content_h = ch;
            windows[i].content = (uint16_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint16_t));
            if (windows[i].content) for (int k = 0; k < CONTENT_CELLS_MAX; k++) windows[i].content[k] = 0x0F00;
            windows[i].cursor_x = 0; windows[i].cursor_y = 0;
            return i;
        }
    }
    return -1;
}

void window_set_font_scale(int id, int scale) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    w->font_scale = scale;
    int cw = CHAR_W * scale, ch = CHAR_H * scale;
    int ncw = (w->w - 2 * WIN_BORDER) / cw;
    int nch = (w->h - WIN_TITLE_H - 2 * WIN_BORDER) / ch;
    if (ncw > CONTENT_COLS_MAX) ncw = CONTENT_COLS_MAX;
    if (nch > CONTENT_ROWS_MAX) nch = CONTENT_ROWS_MAX;
    w->content_w = ncw;
    w->content_h = nch;
    if (w->content) for (int k = 0; k < CONTENT_CELLS_MAX; k++) w->content[k] = 0x0F00;
    w->cursor_x = 0; w->cursor_y = 0;
    w->dirty = 1; needs_redraw = 1;
}

void window_destroy(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    if (windows[id].content) { kfree(windows[id].content); windows[id].content = 0; }
    windows[id].visible = 0; needs_redraw = 1;
}

void window_set_focus(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].focused = (i == id);
}

void window_set_close_button(int id, int has_close) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].has_close_button = has_close;
}

void window_set_minimize_button(int id, int has_min) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].has_minimize_button = has_min;
}

int window_check_close_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_close_button) return 0;
    int bx = w->x + w->w - WIN_BORDER - 4 - WIN_BTN_W, by = w->y + WIN_BORDER;
    return (mx >= bx && mx < bx + WIN_BTN_W && my >= by && my < by + WIN_BTN_H);
}

int window_check_minimize_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_minimize_button) return 0;
    int bx = w->x + w->w - WIN_BORDER - 8 - 2 * WIN_BTN_W, by = w->y + WIN_BORDER;
    return (mx >= bx && mx < bx + WIN_BTN_W && my >= by && my < by + WIN_BTN_H);
}

void window_minimize(int id) {
    if (id >= 0 && id < MAX_WINDOWS) { windows[id].minimized = 1; windows[id].focused = 0; needs_redraw = 1; }
}

void window_restore(int id) {
    if (id >= 0 && id < MAX_WINDOWS) { windows[id].minimized = 0; windows[id].dirty = 1; needs_redraw = 1; }
}

void window_resize(int id, int new_w, int new_h) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
    if (new_w < MIN_WIN_W) new_w = MIN_WIN_W;
    if (new_h < MIN_WIN_H) new_h = MIN_WIN_H;
    if (new_w > SCREEN_W - w->x) new_w = SCREEN_W - w->x;
    if (new_h > SCREEN_H - w->y) new_h = SCREEN_H - w->y;
    if (new_w == w->w && new_h == w->h) return;

    int ncw = (new_w - 2 * WIN_BORDER) / CHAR_W;
    int nch = (new_h - WIN_TITLE_H - 2 * WIN_BORDER) / CHAR_H;
    if (ncw > CONTENT_COLS_MAX) ncw = CONTENT_COLS_MAX;
    if (nch > CONTENT_ROWS_MAX) nch = CONTENT_ROWS_MAX;

    // The buffer is slack-sized with fixed stride, so a resize is just a
    // dimension change: blank the cells newly exposed by growth. The area
    // abandoned by shrinking is simply never read.
    uint8_t color = (w->text_bg << 4) | w->text_fg;
    uint16_t blank = (uint16_t)color << 8 | ' ';
    for (int r = 0; r < nch; r++) {
        int from = (r < w->content_h) ? w->content_w : 0;
        for (int c = from; c < ncw; c++)
            w->content[r * CONTENT_COLS_MAX + c] = blank;
    }

    w->content_w = ncw; w->content_h = nch;
    w->w = new_w; w->h = new_h;
    if (w->cursor_x >= ncw) w->cursor_x = ncw - 1;
    if (w->cursor_y >= nch) w->cursor_y = nch - 1;
    w->dirty = 1; needs_redraw = 1;
}

int window_check_resize_grip(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return 0;
    return (mx >= w->x + w->w - RESIZE_GRIP && mx < w->x + w->w &&
            my >= w->y + w->h - RESIZE_GRIP && my < w->y + w->h);
}

int window_get_focused(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].focused) return i;
    return -1;
}

struct window* window_get(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    return &windows[id];
}

static int window_blink_on(void) {
    extern uint32_t tick_count;
    return (tick_count / 18) % 2 == 0;
}

void window_paint_region(int id, int rx, int ry, int rw, int rh) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return;

    uint32_t bc = w->focused ? vga_to_rgb[WIN_ACTIVE_BORDER] : vga_to_rgb[WIN_BORDER_BG];
    rect_outline(w->x, w->y, w->w, w->h, bc, WIN_BORDER);
    rect_fill(w->x + WIN_BORDER, w->y + WIN_BORDER, w->w - 2 * WIN_BORDER, WIN_TITLE_H, vga_to_rgb[WIN_TITLE_BG]);
    draw_string(w->x + WIN_BORDER + 4, w->y + WIN_BORDER + 4, w->title, vga_to_rgb[WIN_TITLE_FG], vga_to_rgb[WIN_TITLE_BG]);

    if (w->has_close_button) {
        int bx = w->x + w->w - WIN_BORDER - 4 - WIN_BTN_W, by = w->y + WIN_BORDER;
        rect_fill(bx, by, WIN_BTN_W, WIN_BTN_H, vga_to_rgb[4]);
        draw_string(bx + 4, by, "X", vga_to_rgb[15], vga_to_rgb[4]);
    }
    if (w->has_minimize_button) {
        int bx = w->x + w->w - WIN_BORDER - 8 - 2 * WIN_BTN_W, by = w->y + WIN_BORDER;
        rect_fill(bx, by, WIN_BTN_W, WIN_BTN_H, vga_to_rgb[6]);
        draw_string(bx + 4, by, "_", vga_to_rgb[15], vga_to_rgb[6]);
    }

    int cx = w->x + WIN_BORDER, cy = w->y + WIN_BORDER + WIN_TITLE_H;
    int cw = w->w - 2 * WIN_BORDER, ch = w->h - WIN_TITLE_H - 2 * WIN_BORDER;
    int x0 = cx > rx ? cx : rx, y0 = cy > ry ? cy : ry;
    int x1 = (cx + cw) < (rx + rw) ? (cx + cw) : (rx + rw);
    int y1 = (cy + ch) < (ry + rh) ? (cy + ch) : (ry + rh);

    if (x0 < x1 && y0 < y1) {
        rect_fill(x0, y0, x1 - x0, y1 - y0, vga_to_rgb[WIN_BG]);
        if (w->content) {
            int scale = w->font_scale;
            int char_w = CHAR_W * scale, char_h = CHAR_H * scale;
            for (int row = 0; row < w->content_h; row++) {
                int py = cy + row * char_h;
                if (py + char_h <= y0 || py >= y1) continue;
                for (int col = 0; col < w->content_w; col++) {
                    int px = cx + col * char_w;
                    if (px + char_w <= x0 || px >= x1) continue;
                    uint16_t entry = w->content[row * CONTENT_COLS_MAX + col];
                    char c = entry & 0xFF;
                    uint8_t color = (entry >> 8) & 0xFF;
                    draw_char_scaled(px, py, c, vga_to_rgb[color & 0x0F], vga_to_rgb[(color >> 4) & 0x0F], scale);
                }
            }
        }
    }

    // Resize grip — filled triangle in the bottom-right corner
    for (int i = 0; i < WIN_GRIP_SIZE; i++)
        hline(w->x + w->w - WIN_BORDER - 1 - i, w->y + w->h - WIN_BORDER - 1 - i, i + 1, vga_to_rgb[8]);

    if (w->focused && w->content) {
        int scale = w->font_scale;
        int char_w = CHAR_W * scale, char_h = CHAR_H * scale;
        int bx = cx + w->cursor_x * char_w, by = cy + w->cursor_y * char_h;
        if (bx < rx + rw && bx + char_w > rx && by < ry + rh && by + char_h > ry) {
            if (window_blink_on()) {
                rect_fill(bx, by, char_w, char_h, vga_to_rgb[10]);
            } else {
                int idx = w->cursor_y * CONTENT_COLS_MAX + w->cursor_x;
                if (idx >= 0 && idx < CONTENT_CELLS_MAX) {
                    uint16_t entry = w->content[idx];
                    uint8_t color = (entry >> 8) & 0xFF;
                    draw_char_scaled(bx, by, entry & 0xFF, vga_to_rgb[color & 0x0F], vga_to_rgb[(color >> 4) & 0x0F], scale);
                }
            }
        }
    }
}

void window_draw(int id) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return;
    int blink = window_blink_on();
    if (w->dirty || (w->focused && blink != w->last_cursor_visible)) {
        window_paint_region(id, 0, 0, SCREEN_W, SCREEN_H);
        w->dirty = 0; w->last_cursor_visible = blink;
    }
}

void window_draw_all(void) {
    int focused = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].visible) { if (windows[i].focused) focused = i; else window_draw(i); }
    }
    if (focused >= 0) window_draw(focused);
}

static void scroll_content(struct window* w) {
    if (w->cursor_y >= w->content_h) {
        for (int row = 0; row < w->content_h - 1; row++)
            for (int col = 0; col < w->content_w; col++)
                w->content[row * CONTENT_COLS_MAX + col] = w->content[(row + 1) * CONTENT_COLS_MAX + col];
        for (int col = 0; col < w->content_w; col++)
            w->content[(w->content_h - 1) * CONTENT_COLS_MAX + col] = 0x0F00;
        w->cursor_y = w->content_h - 1;
    }
}

void window_put_char(int id, char c) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
    uint8_t color = (w->text_bg << 4) | w->text_fg;
    if (c == '\n') { w->cursor_x = 0; w->cursor_y++; }
    else if (c == '\r') { w->cursor_x = 0; }
    else if (c == '\b') { if (w->cursor_x > 0) { w->cursor_x--; w->content[w->cursor_y * CONTENT_COLS_MAX + w->cursor_x] = (uint16_t)color << 8 | ' '; } }
    else { if (w->cursor_x < w->content_w) { w->content[w->cursor_y * CONTENT_COLS_MAX + w->cursor_x] = (uint16_t)color << 8 | (uint16_t)c; w->cursor_x++; } }
    if (w->cursor_x >= w->content_w) { w->cursor_x = 0; w->cursor_y++; }
    scroll_content(w);
    w->dirty = 1;
}

void window_puts(int id, const char* str) { while (*str) window_put_char(id, *str++); }

void window_clear(int id) {
    struct window* w = &windows[id];
    if (!w->content) return;
    uint8_t color = (w->text_bg << 4) | w->text_fg;
    for (int r = 0; r < w->content_h; r++)
        for (int c = 0; c < w->content_w; c++)
            w->content[r * CONTENT_COLS_MAX + c] = (uint16_t)color << 8 | ' ';
    w->cursor_x = 0; w->cursor_y = 0; w->dirty = 1;
}

void window_set_text_color(int id, uint8_t fg, uint8_t bg) {
    if (id >= 0 && id < MAX_WINDOWS) { windows[id].text_fg = fg; windows[id].text_bg = bg; }
}

void window_set_title(int id, const char* title) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    int j = 0; while (title[j] && j < 31) { windows[id].title[j] = title[j]; j++; }
    windows[id].title[j] = 0; windows[id].dirty = 1;
}

void window_draw_taskbar(void) {
    int y = SCREEN_H - TASKBAR_H;
    rect_fill(0, y, SCREEN_W, TASKBAR_H, vga_to_rgb[1]);
    hline(0, y, SCREEN_W, vga_to_rgb[7]);

    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].visible) count++;
    if (count == 0) return;

    int total_pad = (count + 1) * 6;
    int btn_w = (SCREEN_W - total_pad) / count;
    if (btn_w > 180) btn_w = 180;
    if (btn_w < 60) btn_w = 60;

    int x = 6;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* w = &windows[i];
        if (!w->visible) continue;
        uint32_t btn_bg = w->focused ? vga_to_rgb[9] : vga_to_rgb[1];
        rect_fill(x, y + 3, btn_w, TASKBAR_H - 6, btn_bg);
        char label[20]; int li = 0;
        int max_chars = (btn_w - 8) / CHAR_W;
        if (max_chars > 18) max_chars = 18;
        while (w->title[li] && li < max_chars) { label[li] = w->title[li]; li++; }
        label[li] = 0;
        draw_string(x + 4, y + 5, label, vga_to_rgb[15], btn_bg);
        x += btn_w + 6;
    }
}
