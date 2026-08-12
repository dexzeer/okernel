#include "window.h"
#include "graphics.h"
#include "memory.h"
#include "idt.h"
#include "io.h"
#include <stdint.h>

static struct window windows[MAX_WINDOWS];

// Mouse state
static int mouse_x = 160;
static int mouse_y = 100;
static int mouse_buttons = 0;
static int mouse_cycle = 0;
static int8_t mouse_bytes[3];

// 8x8 mouse cursor bitmap (arrow)
// 1 = draw, 0 = transparent
static const uint8_t cursor_bitmap[8] = {
    0b10000000,
    0b11000000,
    0b11100000,
    0b11110000,
    0b11111000,
    0b11101100,
    0b10000110,
    0b00000110,
};

static const uint8_t cursor_mask[8] = {
    0b10000000,
    0b11000000,
    0b11100000,
    0b11110000,
    0b11111000,
    0b11111100,
    0b11101110,
    0b10000111,
};

// Save background under cursor for restoring
static uint8_t cursor_bg[8 * 8];
static int cursor_bg_x = 0;
static int cursor_bg_y = 0;
static int cursor_visible = 0;

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
            mouse_x += (int8_t)mouse_bytes[1];
            mouse_y -= (int8_t)mouse_bytes[2]; // Y inverted

            // Clamp to screen
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;
            break;
    }
}

// Save the background behind the cursor
static void mouse_save_bg(void) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int sx = mouse_x + col;
            int sy = mouse_y + row;
            if (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H) {
                cursor_bg[row * 8 + col] = graphics_get_buffer()[sy * SCREEN_W + sx];
            }
        }
    }
    cursor_bg_x = mouse_x;
    cursor_bg_y = mouse_y;
}

// Restore background under cursor
static void mouse_restore_bg(void) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int sx = cursor_bg_x + col;
            int sy = cursor_bg_y + row;
            if (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H) {
                putpixel(sx, sy, cursor_bg[row * 8 + col]);
            }
        }
    }
}

void mouse_draw_cursor(void) {
    mouse_restore_bg();
    mouse_save_bg();

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (cursor_bitmap[row] & (1 << col)) {
                putpixel(mouse_x + col, mouse_y + row, 15); // White cursor
            } else if (cursor_mask[row] & (1 << col)) {
                // Border pixel (black outline around white)
                putpixel(mouse_x + col, mouse_y + row, 0);
            }
        }
    }
    cursor_visible = 1;
}

void mouse_hide_cursor(void) {
    if (cursor_visible) {
        mouse_restore_bg();
        cursor_visible = 0;
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

            // Copy title
            int j = 0;
            while (title[j] && j < 31) {
                windows[i].title[j] = title[j];
                j++;
            }
            windows[i].title[j] = 0;

            // Calculate content area (inside borders + title)
            windows[i].content_w = (w - 2 * WIN_BORDER) / 8;
            windows[i].content_h = (h - WIN_TITLE_H - 2 * WIN_BORDER) / 8;

            // Allocate content buffer
            int buf_size = windows[i].content_w * windows[i].content_h;
            windows[i].content = (uint16_t*)kmalloc(buf_size * sizeof(uint16_t));
            if (windows[i].content) {
                for (int k = 0; k < buf_size; k++) {
                    windows[i].content[k] = 0x0F00; // Space, white on black
                }
            }

            windows[i].cursor_x = 0;
            windows[i].cursor_y = 0;
            return i;
        }
    }
    return -1;
}

void window_destroy(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    if (windows[id].content) {
        kfree(windows[id].content);
        windows[id].content = 0;
    }
    windows[id].visible = 0;
}

void window_set_focus(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].focused = (i == id);
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

void window_draw(int id) {
    struct window* w = &windows[id];
    if (!w->visible) return;

    uint8_t border_color = w->focused ? WIN_ACTIVE_BORDER : WIN_BORDER_BG;

    // Draw border
    rect_outline(w->x, w->y, w->w, w->h, border_color, WIN_BORDER);

    // Draw title bar
    rect_fill(w->x + WIN_BORDER, w->y + WIN_BORDER,
              w->w - 2 * WIN_BORDER, WIN_TITLE_H, WIN_TITLE_BG);

    // Draw title text
    draw_string(w->x + WIN_BORDER + 4, w->y + WIN_BORDER + 2,
                w->title, WIN_TITLE_FG, WIN_TITLE_BG);

    // Draw content background
    rect_fill(w->x + WIN_BORDER, w->y + WIN_BORDER + WIN_TITLE_H,
              w->w - 2 * WIN_BORDER, w->h - WIN_TITLE_H - 2 * WIN_BORDER, WIN_BG);

    // Draw content
    if (w->content) {
        int cx = w->x + WIN_BORDER;
        int cy = w->y + WIN_BORDER + WIN_TITLE_H;

        for (int row = 0; row < w->content_h; row++) {
            for (int col = 0; col < w->content_w; col++) {
                uint16_t entry = w->content[row * w->content_w + col];
                char c = entry & 0xFF;
                uint8_t color = (entry >> 8) & 0xFF;
                uint8_t fg = color & 0x0F;
                uint8_t bg = (color >> 4) & 0x0F;
                draw_char(cx + col * 8, cy + row * 8, c, fg, bg);
            }
        }
    }
}

void window_draw_all(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].visible) {
            window_draw(i);
        }
    }
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

    if (c == '\n') {
        w->cursor_x = 0;
        w->cursor_y++;
    } else if (c == '\r') {
        w->cursor_x = 0;
    } else if (c == '\b') {
        if (w->cursor_x > 0) {
            w->cursor_x--;
            w->content[w->cursor_y * w->content_w + w->cursor_x] = 0x0F00;
        }
    } else {
        if (w->cursor_x < w->content_w) {
            w->content[w->cursor_y * w->content_w + w->cursor_x] = 0x0F00 | (uint16_t)c;
            w->cursor_x++;
        }
    }

    if (w->cursor_x >= w->content_w) {
        w->cursor_x = 0;
        w->cursor_y++;
    }

    scroll_content(w);
}

void window_puts(int id, const char* str) {
    while (*str) window_put_char(id, *str++);
}

void window_clear(int id) {
    struct window* w = &windows[id];
    if (!w->content) return;
    int buf_size = w->content_w * w->content_h;
    for (int i = 0; i < buf_size; i++) {
        w->content[i] = 0x0F00;
    }
    w->cursor_x = 0;
    w->cursor_y = 0;
}
