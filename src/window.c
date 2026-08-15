#include "window.h"
#include "graphics.h"
#include "memory.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>

// Redraw flag from desktop.c
extern int needs_redraw;

static struct window windows[MAX_WINDOWS];

// Mouse state
static int mouse_x = 160;
static int mouse_y = 100;
static int mouse_buttons = 0;
static int mouse_cycle = 0;
static int8_t mouse_bytes[3];

// Mouse smoothing (moving average)
#define SMOOTH_SAMPLES 4
static int smooth_dx[SMOOTH_SAMPLES];
static int smooth_dy[SMOOTH_SAMPLES];
static int smooth_idx = 0;

// 12x16 mouse cursor bitmap (clean arrow)
// (CURSOR_W/CURSOR_H live in window.h — the compositor sizes repair rects from them)
static const uint16_t cursor_bitmap[16] = {
    0b110000000000,
    0b111000000000,
    0b111100000000,
    0b111110000000,
    0b111111000000,
    0b111111100000,
    0b111111110000,
    0b111111111000,
    0b111111000000,
    0b111111100000,
    0b110111110000,
    0b110011111000,
    0b110001111000,
    0b100000111100,
    0b000000011100,
    0b000000001100,
};

static void mouse_irq_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;

    uint8_t data = inb(0x60);

    switch (mouse_cycle) {
        case 0:
            mouse_bytes[0] = data;
            if (data & 0x08) mouse_cycle++;
            break;
        case 1:
            mouse_bytes[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_bytes[2] = data;
            mouse_cycle = 0;

            mouse_buttons = mouse_bytes[0] & 0x07;

            // Smooth mouse movement
            smooth_dx[smooth_idx] = (int8_t)mouse_bytes[1];
            smooth_dy[smooth_idx] = -(int8_t)mouse_bytes[2];
            smooth_idx = (smooth_idx + 1) % SMOOTH_SAMPLES;

            int avg_dx = 0, avg_dy = 0;
            for (int i = 0; i < SMOOTH_SAMPLES; i++) {
                avg_dx += smooth_dx[i];
                avg_dy += smooth_dy[i];
            }
            mouse_x += avg_dx / SMOOTH_SAMPLES;
            mouse_y += avg_dy / SMOOTH_SAMPLES;

            // Clamp to screen
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;
            break;
    }
}

// Atomic snapshot of cursor position — the ISR mutates mouse_x/mouse_y on
// IRQ12, so a two-word read must not be torn by a mid-read packet.
void mouse_get_position(int* x, int* y) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags));
    *x = mouse_x;
    *y = mouse_y;
    if (flags & 0x0200) __asm__ volatile("sti"); // restore IF if it was set
}

// Draw the cursor sprite at explicit coordinates. Stateless by design:
// erasing is the compositor's job (it recomposites the scene over the old
// rect), so there is no saved background patch to go stale.
void mouse_paint_cursor(int px, int py) {
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            if (cursor_bitmap[row] & (1 << (CURSOR_W - 1 - col))) {
                int is_edge = 0;
                if (row == 0 || col == 0 ||
                    !(cursor_bitmap[row-1] & (1 << (CURSOR_W - 1 - col))) ||
                    !(cursor_bitmap[row] & (1 << (CURSOR_W - col)))) {
                    is_edge = 1;
                }
                putpixel(px + col, py + row, is_edge ? 0 : 15);
            }
        }
    }
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
int mouse_get_left_button(void) { return mouse_buttons & 0x01; }

void mouse_init_fb(void) {
    mouse_x = SCREEN_W / 2;
    mouse_y = SCREEN_H / 2;
    mouse_buttons = 0;
    mouse_cycle = 0;

    // Enable auxiliary device
    while (inb(0x64) & 0x02);
    outb(0x64, 0xA8);

    // Get config byte
    while (inb(0x64) & 0x02);
    outb(0x64, 0x20);
    while (inb(0x64) & 0x02);
    uint8_t status = inb(0x60);
    status |= 0x02;   // Enable IRQ12
    status &= ~0x20;

    while (inb(0x64) & 0x02);
    outb(0x64, 0x60);
    while (inb(0x64) & 0x02);
    outb(0x60, status);

    // Reset mouse
    while (inb(0x64) & 0x02);
    outb(0x64, 0xD4);
    while (inb(0x64) & 0x02);
    outb(0x60, 0xFF);
    while (inb(0x64) & 0x02);
    inb(0x60);

    // Enable data reporting
    while (inb(0x64) & 0x02);
    outb(0x64, 0xD4);
    while (inb(0x64) & 0x02);
    outb(0x60, 0xF4);

    while (inb(0x64) & 0x01) inb(0x60);

    irq_register_handler(12, mouse_irq_handler);
}

// ---- Window Manager ----

void window_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].visible = 0;
        windows[i].focused = 0;
        windows[i].content = 0;
    }
}

int window_create(const char* title, int x, int y, int w, int h) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].visible) {
            windows[i].x = x;
            windows[i].y = y;
            windows[i].w = w;
            windows[i].h = h;
            windows[i].visible = 1;
            windows[i].focused = 0;
            windows[i].font_scale = 1;
            windows[i].text_fg = 15; // White
            windows[i].text_bg = 0;  // Black
            windows[i].dirty = 1;    // Needs initial draw
            needs_redraw = 1;

            int j = 0;
            while (title[j] && j < 31) {
                windows[i].title[j] = title[j];
                j++;
            }
            windows[i].title[j] = 0;

            // Calculate content area
            windows[i].content_w = (w - 2 * WIN_BORDER) / 8;
            windows[i].content_h = (h - WIN_TITLE_H - 2 * WIN_BORDER) / 8;

            int buf_size = windows[i].content_w * windows[i].content_h;
            windows[i].content = (uint16_t*)kmalloc(buf_size * sizeof(uint16_t));
            if (windows[i].content) {
                for (int k = 0; k < buf_size; k++) {
                    windows[i].content[k] = 0x0F00;
                }
            }

            windows[i].cursor_x = 0;
            windows[i].cursor_y = 0;
            return i;
        }
    }
    return -1;
}

void window_set_font_scale(int id, int scale) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    w->font_scale = scale;

    // Recalculate content dimensions
    int char_w = 8 * scale;
    int char_h = 8 * scale;
    w->content_w = (w->w - 2 * WIN_BORDER) / char_w;
    w->content_h = (w->h - WIN_TITLE_H - 2 * WIN_BORDER) / char_h;

    // Reallocate content buffer
    if (w->content) kfree(w->content);
    int buf_size = w->content_w * w->content_h;
    w->content = (uint16_t*)kmalloc(buf_size * sizeof(uint16_t));
    if (w->content) {
        for (int k = 0; k < buf_size; k++) {
            w->content[k] = 0x0F00;
        }
    }
    w->cursor_x = 0;
    w->cursor_y = 0;
}

void window_destroy(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    if (windows[id].content) {
        kfree(windows[id].content);
        windows[id].content = 0;
    }
    windows[id].visible = 0;
    needs_redraw = 1;
}

void window_set_focus(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].focused = (i == id);
    }
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
    int bx = w->x + w->w - WIN_BORDER - 14;
    int by = w->y + WIN_BORDER;
    return (mx >= bx && mx < bx + 12 && my >= by && my < by + 12);
}

int window_check_minimize_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_minimize_button) return 0;
    // Minimize button is next to close button
    int bx = w->x + w->w - WIN_BORDER - 28;
    int by = w->y + WIN_BORDER;
    return (mx >= bx && mx < bx + 12 && my >= by && my < by + 12);
}

// External flag — set by desktop.c when wallpaper needs redraw

void window_minimize(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].minimized = 1;
        windows[id].focused = 0;
        needs_redraw = 1;
    }
}

void window_restore(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].minimized = 0;
        windows[id].dirty = 1;
        needs_redraw = 1;
    }
}

int window_get_focused(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].focused) return i;
    }
    return -1;
}

struct window* window_get(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    return &windows[id];
}

// Blink state is a pure function of the tick count — no hidden static state,
// so any repaint (dirty, blink toggle, or scene repair) renders identically.
static int window_blink_on(void) {
    extern uint32_t tick_count;
    return (tick_count / 18) % 2 == 0; // ~1 second blink
}

// Repaint a window from its model. Content cells are clipped to the
// screen-space repair rect; frame/title are cheap enough to redraw whole.
// Idempotent: painting twice with no model change produces identical pixels.
void window_paint_region(int id, int rx, int ry, int rw, int rh) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return;

    // Frame + title bar + buttons (full redraw — over-repair is harmless)
    uint8_t border_color = w->focused ? WIN_ACTIVE_BORDER : WIN_BORDER_BG;
    rect_outline(w->x, w->y, w->w, w->h, border_color, WIN_BORDER);
    rect_fill(w->x + WIN_BORDER, w->y + WIN_BORDER,
              w->w - 2 * WIN_BORDER, WIN_TITLE_H, WIN_TITLE_BG);
    draw_string(w->x + WIN_BORDER + 4, w->y + WIN_BORDER + 2,
                w->title, WIN_TITLE_FG, WIN_TITLE_BG);

    if (w->has_close_button) {
        int bx = w->x + w->w - WIN_BORDER - 14;
        int by = w->y + WIN_BORDER;
        rect_fill(bx, by, 12, 12, 4);
        draw_string(bx + 2, by + 2, "X", 15, 4);
    }
    if (w->has_minimize_button) {
        int bx = w->x + w->w - WIN_BORDER - 28;
        int by = w->y + WIN_BORDER;
        rect_fill(bx, by, 12, 12, 6);
        draw_string(bx + 3, by + 2, "_", 15, 6);
    }

    // Content area, clipped to the repair rect
    int cx = w->x + WIN_BORDER;
    int cy = w->y + WIN_BORDER + WIN_TITLE_H;
    int cw = w->w - 2 * WIN_BORDER;
    int ch = w->h - WIN_TITLE_H - 2 * WIN_BORDER;

    // Intersect content rect with repair rect
    int x0 = cx > rx ? cx : rx;
    int y0 = cy > ry ? cy : ry;
    int x1 = (cx + cw) < (rx + rw) ? (cx + cw) : (rx + rw);
    int y1 = (cy + ch) < (ry + rh) ? (cy + ch) : (ry + rh);

    if (x0 < x1 && y0 < y1) {
        rect_fill(x0, y0, x1 - x0, y1 - y0, WIN_BG);

        if (w->content) {
            int scale = w->font_scale;
            int char_w = 8 * scale;
            int char_h = 8 * scale;

            for (int row = 0; row < w->content_h; row++) {
                int py = cy + row * char_h;
                if (py + char_h <= y0 || py >= y1) continue;
                for (int col = 0; col < w->content_w; col++) {
                    int px = cx + col * char_w;
                    if (px + char_w <= x0 || px >= x1) continue;
                    uint16_t entry = w->content[row * w->content_w + col];
                    char c = entry & 0xFF;
                    uint8_t color = (entry >> 8) & 0xFF;
                    draw_char_scaled(px, py, c, color & 0x0F, (color >> 4) & 0x0F, scale);
                }
            }
        }
    }

    // Blinking text cursor — render current state if the cell intersects
    if (w->focused && w->content) {
        int scale = w->font_scale;
        int char_w = 8 * scale;
        int char_h = 8 * scale;
        int bx = cx + w->cursor_x * char_w;
        int by = cy + w->cursor_y * char_h;

        if (bx < rx + rw && bx + char_w > rx &&
            by < ry + rh && by + char_h > ry) {
            if (window_blink_on()) {
                rect_fill(bx, by, char_w, char_h, 10);
            } else {
                int idx = w->cursor_y * w->content_w + w->cursor_x;
                if (idx >= 0 && idx < w->content_w * w->content_h) {
                    uint16_t entry = w->content[idx];
                    char c = entry & 0xFF;
                    uint8_t color = (entry >> 8) & 0xFF;
                    draw_char_scaled(bx, by, c, color & 0x0F, (color >> 4) & 0x0F, scale);
                }
            }
        }
    }
}

void window_draw(int id) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return;

    // Repaint when dirty, or when the focused window's blink cell toggles
    int blink = window_blink_on();
    if (w->dirty || (w->focused && blink != w->last_cursor_visible)) {
        window_paint_region(id, 0, 0, SCREEN_W, SCREEN_H);
        w->dirty = 0;
        w->last_cursor_visible = blink;
    }
}

void window_draw_all(void) {
    int focused = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].visible) {
            if (windows[i].focused) focused = i;
            else window_draw(i);
        }
    }
    // Draw focused window last (on top)
    if (focused >= 0) window_draw(focused);
}

static void scroll_content(struct window* w) {
    if (w->cursor_y >= w->content_h) {
        // Scroll up
        for (int row = 0; row < w->content_h - 1; row++) {
            for (int col = 0; col < w->content_w; col++) {
                w->content[row * w->content_w + col] =
                    w->content[(row + 1) * w->content_w + col];
            }
        }
        // Clear last row
        for (int col = 0; col < w->content_w; col++) {
            w->content[(w->content_h - 1) * w->content_w + col] = 0x0F00;
        }
        w->cursor_y = w->content_h - 1;
    }
}

void window_put_char(int id, char c) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;

    uint8_t color = (w->text_bg << 4) | w->text_fg;

    if (c == '\n') {
        w->cursor_x = 0;
        w->cursor_y++;
    } else if (c == '\r') {
        w->cursor_x = 0;
    } else if (c == '\b') {
        if (w->cursor_x > 0) {
            w->cursor_x--;
            w->content[w->cursor_y * w->content_w + w->cursor_x] = (uint16_t)color << 8 | ' ';
        }
    } else {
        if (w->cursor_x < w->content_w) {
            w->content[w->cursor_y * w->content_w + w->cursor_x] = (uint16_t)color << 8 | (uint16_t)c;
            w->cursor_x++;
        }
    }

    if (w->cursor_x >= w->content_w) {
        w->cursor_x = 0;
        w->cursor_y++;
    }

    scroll_content(w);
    w->dirty = 1; // Mark window for redraw
}

void window_puts(int id, const char* str) {
    while (*str) window_put_char(id, *str++);
}

void window_clear(int id) {
    struct window* w = &windows[id];
    if (!w->content) return;
    uint8_t color = (w->text_bg << 4) | w->text_fg;
    int buf_size = w->content_w * w->content_h;
    for (int i = 0; i < buf_size; i++) {
        w->content[i] = (uint16_t)color << 8 | ' ';
    }
    w->cursor_x = 0;
    w->cursor_y = 0;
    w->dirty = 1;
}

void window_set_text_color(int id, uint8_t fg, uint8_t bg) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].text_fg = fg;
        windows[id].text_bg = bg;
    }
}

void window_set_title(int id, const char* title) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    int j = 0;
    while (title[j] && j < 31) {
        windows[id].title[j] = title[j];
        j++;
    }
    windows[id].title[j] = 0;
    windows[id].dirty = 1;
}

void window_draw_taskbar(void) {
    int y = SCREEN_H - TASKBAR_H;

    rect_fill(0, y, SCREEN_W, TASKBAR_H, 1);
    hline(0, y, SCREEN_W, 7);

    // Count visible windows
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].visible) count++;
    }
    if (count == 0) return;

    // Dynamic button width: fill taskbar evenly
    int total_pad = (count + 1) * 4; // 4px gap on each side
    int btn_w = (SCREEN_W - total_pad) / count;
    if (btn_w > 120) btn_w = 120; // Cap max width
    if (btn_w < 30) btn_w = 30;   // Min width

    int x = 4;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* w = &windows[i];
        if (!w->visible) continue;

        uint8_t btn_bg = w->focused ? 9 : 1;
        rect_fill(x, y + 2, btn_w, TASKBAR_H - 4, btn_bg);

        char label[16];
        int li = 0;
        int max_chars = (btn_w - 8) / 8; // Fit text in button
        if (max_chars > 15) max_chars = 15;
        while (w->title[li] && li < max_chars) { label[li] = w->title[li]; li++; }
        label[li] = 0;
        draw_string(x + 4, y + 5, label, 15, btn_bg);

        x += btn_w + 4;
    }
}
