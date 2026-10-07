#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "ui_draw.h"
#include "cjk.h"
#include "memory.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>

extern int needs_redraw;
extern void desktop_paint_rect_pub(int x, int y, int w, int h);
int window_rgb_is_light(uint32_t c); // defined below; used early by the blit path

// VGA palette lookup for content rendering (non-static: okai reads it too so
// headings drawn as raw pixels match the body text's color exactly)
const uint32_t vga_to_rgb[16] = {
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
#define CONTENT_COLS_MAX ((SCREEN_W - 2 * WIN_BORDER) / CONTENT_GW)
#define CONTENT_ROWS_MAX ((SCREEN_H - WIN_TITLE_H - 2 * WIN_BORDER) / CONTENT_GH)
#define CONTENT_CELLS_MAX (CONTENT_COLS_MAX * CONTENT_ROWS_MAX)

// Per-window scrollback: lines pushed off the top of the content grid when it
// scrolls, replayable with the mouse wheel. The wheel shifts a VIEW offset —
// the live grid is never rewritten, so scrolling is non-destructive and new
// output simply snaps back to the bottom (auto-follow).
#define SB_ROWS 256
static uint16_t sb_ring[MAX_WINDOWS][SB_ROWS][CONTENT_COLS_MAX];
static int sb_count[MAX_WINDOWS]; // valid lines in the ring (<= SB_ROWS)
static int sb_next[MAX_WINDOWS];  // next write index (ring)

// Mouse state
static int mouse_x = 160, mouse_y = 100;
static int mouse_buttons = 0, mouse_cycle = 0;
static int8_t mouse_bytes[4];
static int mouse_has_wheel = 0;  // 1 once Intellimouse 4-byte mode is on
static int mouse_scroll = 0;     // accumulated wheel delta, consumed per frame
#define SMOOTH_SAMPLES 4
static int smooth_dx[SMOOTH_SAMPLES], smooth_dy[SMOOTH_SAMPLES], smooth_idx = 0;

static const uint16_t cursor_bitmap[16] = {
    0b110000000000, 0b111000000000, 0b111100000000, 0b111110000000,
    0b111111000000, 0b111111100000, 0b111111110000, 0b111111111000,
    0b111111000000, 0b111111100000, 0b110111110000, 0b110011111000,
    0b110001111000, 0b100000111100, 0b000000011100, 0b000000001100,
};

// Apply one movement packet (bytes 0..2) to the cursor position.
static uint32_t mse_pkt_count = 0;
static uint8_t mse_prev_buttons = 0;
static void mouse_process_packet(void) {
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
    mse_pkt_count++;
    // Ground truth for headless mouse tests: log button transitions with the
    // current position and cumulative packet count (desync shows up as the
    // count stalling or positions not following bursts).
    if (mouse_buttons != mse_prev_buttons) {
        serial_printf("[mse] btn=%d x=%d y=%d pkts=%u\n",
                      mouse_buttons, mouse_x, mouse_y, mse_pkt_count);
        mse_prev_buttons = mouse_buttons;
    }
}

static void mouse_irq_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;
    uint8_t data = inb(0x60);
    switch (mouse_cycle) {
        case 0: mouse_bytes[0] = data; if (data & 0x08) mouse_cycle++; break;
        case 1: mouse_bytes[1] = data; mouse_cycle++; break;
        case 2:
            mouse_bytes[2] = data;
            if (mouse_has_wheel) mouse_cycle++;          // expect 4th (wheel) byte
            else { mouse_cycle = 0; mouse_process_packet(); }
            break;
        case 3:
            mouse_bytes[3] = data; mouse_cycle = 0;
            // PS/2 Z byte: NEGATIVE = wheel up on the real input path (QEMU
            // GUI frontends; note HMP `mouse_move dz` has the opposite sign —
            // scripts inject dz=+1 for wheel-up). Consumers (okai scroll,
            // terminal scrollback) keep "positive = down/toward the tail".
            mouse_scroll += (int8_t)mouse_bytes[3];
            mouse_process_packet();
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

// Return the accumulated wheel delta since the last call, then reset it.
// Positive = wheel up (toward the user), negative = wheel down.
int mouse_get_scroll(void) {
    int s = mouse_scroll;
    mouse_scroll = 0;
    return s;
}

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

    // Try to enable Intellimouse wheel mode (4-byte packets). Send the standard
    // sample-rate magic sequence, then read the device ID; a wheel-capable
    // mouse answers 0x03 (or 0x04). If it doesn't, we stay in 3-byte mode and
    // simply never receive wheel bytes.
    {
        uint8_t id = 0;
        uint8_t rates[] = { 0xC8, 0x64, 0x50 }; // 200, 100, 80
        for (int i = 0; i < 3; i++) {
            while (inb(0x64) & 0x02); outb(0x64, 0xD4);
            while (inb(0x64) & 0x02); outb(0x60, 0xF3); // set sample rate
            while (inb(0x64) & 0x01) inb(0x60);          // ack
            while (inb(0x64) & 0x02); outb(0x64, 0xD4);
            while (inb(0x64) & 0x02); outb(0x60, rates[i]);
            while (inb(0x64) & 0x01) inb(0x60);          // ack
        }
        while (inb(0x64) & 0x02); outb(0x64, 0xD4);
        while (inb(0x64) & 0x02); outb(0x60, 0xF2);     // get device ID
        // The device answers ACK (0xFA) then the ID byte. WAIT for each byte —
        // draining the output buffer first would eat the ID and leave a garbage
        // read, misdetecting wheel mode while the device already switched to
        // 4-byte packets (permanent stream desync: movement dies).
        for (int spin = 0; spin < 100000 && !(inb(0x64) & 0x01); spin++);
        (void)inb(0x60);                                 // ack
        for (int spin = 0; spin < 100000 && !(inb(0x64) & 0x01); spin++);
        id = inb(0x60);
        if (id == 0x03 || id == 0x04) mouse_has_wheel = 1;
        serial_puts("[mse] wheel detect id=0x");
        serial_putchar("0123456789ABCDEF"[id >> 4]);
        serial_putchar("0123456789ABCDEF"[id & 0xF]);
        serial_puts(mouse_has_wheel ? " 4-byte mode ON\n" : " 3-byte mode\n");
    }

    // Re-enable data reporting (sample-rate commands may have paused it).
    while (inb(0x64) & 0x02); outb(0x64, 0xD4);
    while (inb(0x64) & 0x02); outb(0x60, 0xF4);
    while (inb(0x64) & 0x01) inb(0x60);

    irq_register_handler(12, mouse_irq_handler);
}

// ---- Window Manager ----

void window_init(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].visible = 0; windows[i].focused = 0; windows[i].content = 0;
        sb_count[i] = 0; sb_next[i] = 0; windows[i].scroll_off = 0;
    }
}

// Recompute the content grid from the window's size, font scale, and title-bar
// mode, and re-blank the (slack-sized) content buffer.
static void cell_blank(struct window* w, int idx) {
    uint8_t color = (uint8_t)((w->content_bg << 4) | w->text_fg);
    w->content[idx] = (uint16_t)((uint16_t)color << 8) | ' ';
    w->cell_fg[idx] = w->text_fg_rgb;
    w->cell_bg[idx] = w->text_bg_rgb;
    if (w->cell_attr) w->cell_attr[idx] = 0;
}

// Partial-dirty tracking: narrow repaints to the cells that actually changed.
// window_mark_all = full repaint (structural changes). window_mark_cell =
// union one cell into the pending range. Callers must still set w->dirty=1
// (all writers below do). Invariant: backbuffer == model everywhere outside
// overlay/cursor pixels, so skipping unchanged cells is pixel-identical.
static void window_mark_all(struct window* w) { w->pr_valid = 0; w->pxd_valid = 0; }
static void window_mark_cell(struct window* w, int row, int col) {
    if (!w || row < 0 || col < 0 || row >= w->content_h || col >= w->content_w)
        return;
    // A full repaint is already pending (dirty set, no range): a structural
    // change (clear/scroll/resize) blanked or shifted the whole grid, so a
    // later single-cell mark must NOT narrow it back to partial — the cells
    // outside the range still differ from the backbuffer. Writers therefore
    // mark BEFORE setting dirty (see window_put_char).
    if (w->dirty && !w->pr_valid) return;
    if (!w->pr_valid) {
        w->pr_c0 = w->pr_c1 = col;
        w->pr_r0 = w->pr_r1 = row;
        w->pr_valid = 1;
    } else {
        if (col < w->pr_c0) w->pr_c0 = col;
        if (col > w->pr_c1) w->pr_c1 = col;
        if (row < w->pr_r0) w->pr_r0 = row;
        if (row > w->pr_r1) w->pr_r1 = row;
    }
}

static void window_apply_metrics(struct window* w) {
    int title = w->no_titlebar ? 0 : WIN_TITLE_H;
    int scale = w->font_scale;
    int cw = (w->w - 2 * WIN_BORDER) / (CONTENT_GW * scale);
    int ch = (w->h - title - 2 * WIN_BORDER) / (CONTENT_GH * scale);
    if (cw > CONTENT_COLS_MAX) cw = CONTENT_COLS_MAX;
    if (ch > CONTENT_ROWS_MAX) ch = CONTENT_ROWS_MAX;
    w->content_w = cw;
    w->content_h = ch;
    if (w->content) {
        for (int k = 0; k < CONTENT_CELLS_MAX; k++) cell_blank(w, k);
    }
    w->cursor_x = 0; w->cursor_y = 0;
    w->scroll_off = 0; // resize reflows the grid — re-anchor at the live tail
    w->dirty = 1; w->pr_valid = 0; w->pxd_valid = 0; needs_redraw = 1; // full repaint
}

int window_create(const char* title, int x, int y, int w, int h) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].visible) {
            windows[i].x = x; windows[i].y = y; windows[i].w = w; windows[i].h = h;
            windows[i].visible = 1; windows[i].focused = 0; windows[i].font_scale = 1;
            windows[i].z = i;
            windows[i].no_titlebar = 0;
            windows[i].is_term = 0; // reset: slot reuse must not leak themes
            windows[i].has_restore = 0;
            windows[i].text_fg = 15; windows[i].text_bg = 0;
            windows[i].text_fg_rgb = vga_to_rgb[15]; windows[i].text_bg_rgb = vga_to_rgb[0];
            windows[i].content_bg = WIN_BG;
            windows[i].content_bg_rgb = WIN_BG_RGB;
            windows[i].dirty = 1; windows[i].pr_valid = 0; windows[i].pxd_valid = 0; windows[i].pix = 0; needs_redraw = 1; // full (slot reuse)
            sb_count[i] = 0; sb_next[i] = 0; windows[i].scroll_off = 0;
            int j = 0;
            while (title[j] && j < 31) { windows[i].title[j] = title[j]; j++; }
            windows[i].title[j] = 0;
            windows[i].content = (uint16_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint16_t));
            windows[i].cell_fg = (uint32_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint32_t));
            windows[i].cell_bg = (uint32_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint32_t));
            windows[i].cell_attr = (uint8_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint8_t));
            window_apply_metrics(&windows[i]); // sets content_w/h and blanks the buffer
            return i;
        }
    }
    return -1;
}

void window_set_font_scale(int id, int scale) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    w->font_scale = scale;
    window_apply_metrics(w);
}

void window_destroy(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    if (windows[id].content) { kfree(windows[id].content); windows[id].content = 0; }
    if (windows[id].cell_fg) { kfree(windows[id].cell_fg); windows[id].cell_fg = 0; }
    if (windows[id].cell_bg) { kfree(windows[id].cell_bg); windows[id].cell_bg = 0; }
    if (windows[id].cell_attr) { kfree(windows[id].cell_attr); windows[id].cell_attr = 0; }
    windows[id].visible = 0;
    windows[id].pix = 0; windows[id].pxd_valid = 0;
    // Backbuffer is persistent now: repair the freed region (wallpaper + icons +
    // any window behind) instead of relying on a full per-frame repaint.
    desktop_paint_rect_pub(windows[id].x, windows[id].y, windows[id].w, windows[id].h);
    sb_count[id] = 0; sb_next[id] = 0; windows[id].scroll_off = 0;
}

static int g_z_top = 0;   // monotonically increasing top of the z-stack

void window_raise(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    if (!windows[id].visible) return;
    windows[id].z = ++g_z_top;
}

void window_set_focus(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        int was = windows[i].focused;
        windows[i].focused = (i == id);
        // Title-bar color follows focus: repaint both sides of the change.
        // Without this the loser keeps its blue title until an unrelated
        // repair (e.g. a drag passing over it) repaints it grey.
        if (was != windows[i].focused) {
            windows[i].dirty = 1;
            window_mark_all(&windows[i]);
            needs_redraw = 1;
        }
    }
    window_raise(id);   // focused window is always topmost
}

void window_set_close_button(int id, int has_close) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].has_close_button = has_close;
}

void window_set_minimize_button(int id, int has_min) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].has_minimize_button = has_min;
}

void window_set_terminal(int id, int flag) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    windows[id].is_term = flag ? 1 : 0;
    windows[id].dirty = 1;
    window_mark_all(&windows[id]);
}

void window_set_no_titlebar(int id, int flag) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    if (w->no_titlebar == flag) return;
    w->no_titlebar = flag;
    window_apply_metrics(w);
}

void window_set_pixels(int id, const uint32_t* px, int x, int y, int w, int h, int stride) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* win = &windows[id];
    win->pix = px;
    win->pix_x = x; win->pix_y = y;
    win->pix_w = w; win->pix_h = h;
    win->pix_stride = stride;
    win->pxd_valid = 0;
    win->dirty = 1;
    window_mark_all(win);
}

void window_dirty_pixels(int id, int x, int y, int w, int h) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* win = &windows[id];
    if (!win->pix || w <= 0 || h <= 0) return;
    if (win->dirty && !win->pxd_valid) return; // full repaint already pending
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

void window_set_hide_cursor(int id, int flag) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    windows[id].hide_cursor = flag;
    windows[id].dirty = 1;
    window_mark_all(&windows[id]);
}

// Caption buttons of a no-titlebar window: three WIN_CTRL_W-wide cells at the
// top-right of the content area, CHROME_TAB_H tall (min | max | close).
int window_ctrl_rect(int id, int which, int r[4]) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    struct window* w = &windows[id];
    if (!w->visible || !w->no_titlebar) return 0;
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

// Title-bar buttons (windows WITH a title bar): close at the right edge,
// minimize left of it, both vertically centered. Hit tests and drawing use it.
static void title_btn_rect(const struct window* w, int minimize, int r[4]) {
    r[0] = w->x + w->w - WIN_BORDER - 8 - WIN_BTN_W - (minimize ? WIN_BTN_W + 6 : 0);
    r[1] = w->y + WIN_BORDER + (WIN_TITLE_H - WIN_BTN_H) / 2;
    r[2] = WIN_BTN_W; r[3] = WIN_BTN_H;
}

static int title_btn_hit(const struct window* w, int minimize, int mx, int my) {
    int r[4];
    title_btn_rect(w, minimize, r);
    return mx >= r[0] && mx < r[0] + r[2] && my >= r[1] && my < r[1] + r[3];
}

int window_check_close_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_close_button) return 0;
    if (w->no_titlebar) return ctrl_hit(id, WIN_CTRL_CLOSE, mx, my);
    return title_btn_hit(w, 0, mx, my);
}

int window_check_minimize_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_minimize_button) return 0;
    if (w->no_titlebar) return ctrl_hit(id, WIN_CTRL_MIN, mx, my);
    return title_btn_hit(w, 1, mx, my);
}

int window_check_maximize_click(int id, int mx, int my) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].visible) return 0;
    return ctrl_hit(id, WIN_CTRL_MAX, mx, my);
}

int window_is_maximized(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    struct window* w = &windows[id];
    return w->x == 0 && w->y == 0 && w->w == SCREEN_W && w->h == SCREEN_H - TASKBAR_H;
}

// Maximize to the work area, or restore the remembered rect (a window that
// opened maximized restores to a centered 3/4-size rect).
void window_toggle_maximize(int id) {
    if (id < 0 || id >= MAX_WINDOWS || !windows[id].visible) return;
    struct window* w = &windows[id];
    int wa_h = SCREEN_H - TASKBAR_H;
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    if (window_is_maximized(id)) {
        int nw = SCREEN_W * 3 / 4, nh = wa_h * 3 / 4;
        int nx = (SCREEN_W - nw) / 2, ny = (wa_h - nh) / 2;
        if (w->has_restore && w->restore_w < SCREEN_W - 16) {
            nx = w->restore_x; ny = w->restore_y; nw = w->restore_w; nh = w->restore_h;
        }
        w->x = nx; w->y = ny;
        window_resize(id, nw, nh);
    } else {
        w->has_restore = 1;
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
        w->x = 0; w->y = 0;
        window_resize(id, SCREEN_W, wa_h);
    }
    w->dirty = 1; w->pr_valid = 0; w->pxd_valid = 0; needs_redraw = 1;
    // the old footprint: wallpaper + whatever sits behind (z-order repair)
    desktop_paint_rect_pub(ox, oy, ow, oh);
}

void window_minimize(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].minimized = 1; windows[id].focused = 0;
        desktop_paint_rect_pub(windows[id].x, windows[id].y, windows[id].w, windows[id].h);
    }
}

void window_restore(int id) {
    if (id >= 0 && id < MAX_WINDOWS) { windows[id].minimized = 0; windows[id].dirty = 1; window_mark_all(&windows[id]); needs_redraw = 1; }
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

    int title = w->no_titlebar ? 0 : WIN_TITLE_H;
    int ncw = (new_w - 2 * WIN_BORDER) / (CONTENT_GW * w->font_scale);
    int nch = (new_h - title - 2 * WIN_BORDER) / (CONTENT_GH * w->font_scale);
    if (ncw > CONTENT_COLS_MAX) ncw = CONTENT_COLS_MAX;
    if (nch > CONTENT_ROWS_MAX) nch = CONTENT_ROWS_MAX;

    // The buffer is slack-sized with fixed stride, so a resize is just a
    // dimension change: blank the cells newly exposed by growth. The area
    // abandoned by shrinking is simply never read.
    uint8_t color = (w->content_bg << 4) | w->text_fg;
    uint16_t blank = (uint16_t)color << 8 | ' ';
    for (int r = 0; r < nch; r++) {
        int from = (r < w->content_h) ? w->content_w : 0;
        for (int c = from; c < ncw; c++) {
            int k = r * CONTENT_COLS_MAX + c;
            w->content[k] = blank;
            // Fresh cells must not inherit stale plane data (same intent as
            // the blank VGA byte above; previously only content was reset).
            w->cell_fg[k] = w->text_fg_rgb;
            w->cell_bg[k] = w->text_bg_rgb;
            if (w->cell_attr) w->cell_attr[k] = 0;
        }
    }

    w->content_w = ncw; w->content_h = nch;
    w->w = new_w; w->h = new_h;
    if (w->cursor_x >= ncw) w->cursor_x = ncw - 1;
    if (w->cursor_y >= nch) w->cursor_y = nch - 1;
    w->dirty = 1; w->pxd_valid = 0; w->pr_valid = 0; needs_redraw = 1;
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

// Topmost visible, non-minimized window containing screen point (x, y), or -1.
// "Topmost" = highest z (the window that would be drawn last / on top).
int window_from_point(int x, int y) {
    int best = -1, bestz = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* w = &windows[i];
        if (!w->visible || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (w->z > bestz) { bestz = w->z; best = i; }
        }
    }
    return best;
}

// Current text cursor position in the content buffer (row/col, not pixels).
void window_get_cursor(int id, int* out_x, int* out_y) {
    if (id < 0 || id >= MAX_WINDOWS) { if (out_x) *out_x = 0; if (out_y) *out_y = 0; return; }
    if (out_x) *out_x = windows[id].cursor_x;
    if (out_y) *out_y = windows[id].cursor_y;
}

static int window_blink_on(void) {
    extern uint32_t tick_count;
    return (tick_count / 100) % 2 == 0;
}

void window_paint_region(int id, int rx, int ry, int rw, int rh) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return;

    // ---- Border + (optional) title bar ----
    int title_off = w->no_titlebar ? 0 : WIN_TITLE_H;
    int title_bottom = w->y + WIN_BORDER + title_off;
    // Border frame — drawn whenever the clip rect overlaps the window at all.
    if (rx < w->x + w->w && rx + rw > w->x && ry < w->y + w->h && ry + rh > w->y) {
        uint32_t bc = w->no_titlebar ? (w->focused ? FX_FRAME_BORDER : FX_FRAME_BORDER_U) :
                      (w->focused ? HDR_BORDER : HDR_BORDER_U);
        rect_outline(w->x, w->y, w->w, w->h, bc, WIN_BORDER);
    }
    // Title bar (skipped for title-bar-less windows; the client draws its own
    // top): a dark GNOME-style header bar, centered Noto title, round buttons.
    if (!w->no_titlebar &&
        rx < w->x + w->w && rx + rw > w->x && ry < title_bottom && ry + rh > w->y) {
        int tx = w->x + WIN_BORDER, ty = w->y + WIN_BORDER;
        int tw = w->w - 2 * WIN_BORDER;
        uint32_t hbg = w->focused ? HDR_BG : HDR_BG_U;
        rect_fill(tx, ty, tw, WIN_TITLE_H, hbg);
        hline(tx, ty + WIN_TITLE_H - 1, tw, HDR_DIVIDER);
        struct wsurf s;
        ui_screen_surf(&s);
        int old[4], old2[4]; // (scratch surface: never popped)
        ws_push_clip(&s, rx, ry, rx + rw, ry + rh, old);
        ws_push_clip(&s, tx, ty, tx + tw, ty + WIN_TITLE_H - 1, old2);
        int btns_w = (w->has_close_button ? WIN_BTN_W + 14 : 0) +
                     (w->has_minimize_button ? WIN_BTN_W + 6 : 0);
        int room = tw - 2 * (btns_w + 8);
        if (room > 0) {
            int tl = ui_text_w(15, 1, w->title, -1);
            if (tl > room) tl = room;
            ui_text_draw(&s, tx + (tw - tl) / 2, ty + WIN_TITLE_H / 2, 15, 1,
                         w->focused ? HDR_TEXT : HDR_TEXT_U, w->title, -1, room, hbg);
        }
        for (int m = 0; m < 2; m++) {
            if (m == 0 ? !w->has_close_button : !w->has_minimize_button) continue;
            int r[4];
            title_btn_rect(w, m, r);
            int d = 24, cx = r[0] + (r[2] - d) / 2, cy = r[1] + (r[3] - d) / 2;
            ui_rrect(&s, cx, cy, d, d, d / 2, w->focused ? HDR_BTN : HDR_BTN_U);
            struct ui_pen p;
            ui_pen_at(&p, cx, cy, 150);
            ui_path_begin();
            if (m == 0) {
                ui_seg(&p, UI_U(8), UI_U(8), UI_U(16), UI_U(16), 1);
                ui_seg(&p, UI_U(16), UI_U(8), UI_U(8), UI_U(16), 1);
            } else {
                ui_seg(&p, UI_U(8), UI_U(15.5), UI_U(16), UI_U(15.5), 1);
            }
            ui_path_fill(&s, w->focused ? HDR_TEXT : HDR_TEXT_U);
        }
        ui_screen_done(ty, WIN_TITLE_H);
    }

    // ---- Content cells (pre-computed row/col range, no redundant bg fill) ----
    int cx = w->x + WIN_BORDER, cy = w->y + WIN_BORDER + title_off;
    int cw = w->w - 2 * WIN_BORDER, ch = w->h - title_off - 2 * WIN_BORDER;
    int x0 = cx > rx ? cx : rx, y0 = cy > ry ? cy : ry;
    int x1 = (cx + cw) < (rx + rw) ? (cx + cw) : (rx + rw);
    int y1 = (cy + ch) < (ry + rh) ? (cy + ch) : (ry + rh);

    if (x0 < x1 && y0 < y1 && w->pix) {
        // Client pixel surface (okai page): blit the overlap, fill the rest
        // of the content area (chrome band, slack) with the content bg.
        int sx0 = cx + w->pix_x, sy0 = cy + w->pix_y;
        int sx1 = sx0 + w->pix_w, sy1 = sy0 + w->pix_h;
        if (sx1 > cx + cw) sx1 = cx + cw;
        if (sy1 > cy + ch) sy1 = cy + ch;
        int ix0 = x0 > sx0 ? x0 : sx0, iy0 = y0 > sy0 ? y0 : sy0;
        int ix1 = x1 < sx1 ? x1 : sx1, iy1 = y1 < sy1 ? y1 : sy1;
        if (ix0 >= ix1 || iy0 >= iy1) {
            rect_fill(x0, y0, x1 - x0, y1 - y0, w->content_bg_rgb);
        } else {
            if (y0 < iy0) rect_fill(x0, y0, x1 - x0, iy0 - y0, w->content_bg_rgb);
            if (iy1 < y1) rect_fill(x0, iy1, x1 - x0, y1 - iy1, w->content_bg_rgb);
            if (x0 < ix0) rect_fill(x0, iy0, ix0 - x0, iy1 - iy0, w->content_bg_rgb);
            if (ix1 < x1) rect_fill(ix1, iy0, x1 - ix1, iy1 - iy0, w->content_bg_rgb);
            graphics_blit_pixels(ix0, iy0,
                                 w->pix + (iy0 - sy0) * w->pix_stride + (ix0 - sx0),
                                 ix1 - ix0, iy1 - iy0, w->pix_stride);
        }
    } else if (x0 < x1 && y0 < y1) {
        // Fill content background — essential: null chars (0x00) in the
        // buffer cause draw_char_scaled to bail early, leaving gaps.
        rect_fill(x0, y0, x1 - x0, y1 - y0, w->content_bg_rgb);
        if (w->content) {
            int wid = (int)(w - windows);
            int scale = w->font_scale;
            int char_w = CONTENT_GW * scale, char_h = CONTENT_GH * scale;
            // Pre-compute row/col range — skip cells outside clip rect
            int row_start = (y0 - cy) / char_h;
            int row_end   = (y1 - cy + char_h - 1) / char_h;
            if (row_start < 0) row_start = 0;
            if (row_end > w->content_h) row_end = w->content_h;
            int col_start = (x0 - cx) / char_w;
            int col_end   = (x1 - cx + char_w - 1) / char_w;
            if (col_start < 0) col_start = 0;
            if (col_end > w->content_w) col_end = w->content_w;
            for (int row = row_start; row < row_end; row++) {
                int py = cy + row * char_h;
                // Scrolled-back view: the top scroll_off rows come from the
                // history ring (VGA-indexed — terminals only); live rows read
                // the exact-color RGB planes.
                const uint16_t* src;
                const uint32_t *sfg = 0, *sbg = 0; // RGB planes (live rows)
                const uint8_t *sat = 0;           // attr plane (live rows)
                if (w->scroll_off > 0 && row < w->scroll_off) {
                    int h = sb_count[wid] - w->scroll_off + row; // history line
                    int idx = ((sb_next[wid] - sb_count[wid] + h) % SB_ROWS
                               + SB_ROWS) % SB_ROWS;
                    src = sb_ring[wid][idx];
                } else {
                    int lrow = row - w->scroll_off;
                    src = w->content + lrow * CONTENT_COLS_MAX;
                    sfg = w->cell_fg + lrow * CONTENT_COLS_MAX;
                    sbg = w->cell_bg + lrow * CONTENT_COLS_MAX;
                    if (w->cell_attr) sat = w->cell_attr + lrow * CONTENT_COLS_MAX;
                }
                for (int col = col_start; col < col_end; col++) {
                    int px = cx + col * char_w;
                    uint16_t entry = src[col];
                    char c = entry & 0xFF;
                    uint32_t fg, bg;
                    uint8_t attr = 0;
                    if (sfg) { fg = sfg[col]; bg = sbg[col]; if (sat) attr = sat[col]; }
                    else {
                        uint8_t color = (entry >> 8) & 0xFF;
                        fg = vga_to_rgb[color & 0x0F];
                        bg = vga_to_rgb[(color >> 4) & 0x0F];
                    }
                    if (sat && (attr & 0x80)) {
                        // CJK model cell (see window_write_cjk_cell): decode
                        // the packed codepoint and draw from the bitmap table.
                        // Glyph box is cell-wide, vertically centered.
                        uint32_t cjk = ((fg >> 24) << 13) |
                                       (((bg >> 24) & 0xFFu) << 5) |
                                       (((uint32_t)attr >> 2) & 0x1Fu);
                        const uint8_t* g = cjk_glyph_for(cjk);
                        int side = char_w, yoff = (char_h - char_w) / 2;
                        if (yoff < 0) { side = char_h; yoff = 0; }
                        if (g) draw_cjk_box(px, py + yoff, g,
                                            fg & 0xFFFFFFu, bg & 0xFFFFFFu,
                                            side, side);
                        else draw_char_cell(px, py, '?', fg & 0xFFFFFFu,
                                            bg & 0xFFFFFFu, char_w, char_h,
                                            attr & 3);
                        continue;
                    }
                    draw_char_cell(px, py, c, fg, bg, char_w, char_h, attr);
                }
            }
        }
    }

    // ---- Resize grip (skip if clip rect is far from bottom-right) ----
    if (rx + rw > w->x + w->w - WIN_GRIP_SIZE - WIN_BORDER &&
        ry + rh > w->y + w->h - WIN_GRIP_SIZE - WIN_BORDER) {
        for (int i = 0; i < WIN_GRIP_SIZE; i++)
            hline(w->x + w->w - WIN_BORDER - 1 - i,
                  w->y + w->h - WIN_BORDER - 1 - i,
                  i + 1, vga_to_rgb[8]);
    }

    // ---- Blinking cursor (skip if clip rect doesn't overlap; hidden while
    // scrolled back — it belongs to the live tail, not the history view) ----
    if (w->focused && w->content && !w->scroll_off && !w->hide_cursor) {
        int scale = w->font_scale;
        int char_w = CONTENT_GW * scale, char_h = CONTENT_GH * scale;
        int bx = cx + w->cursor_x * char_w, by = cy + w->cursor_y * char_h;
        if (bx < rx + rw && bx + char_w > rx && by < ry + rh && by + char_h > ry) {
            if (window_blink_on()) {
                rect_fill(bx, by, char_w, char_h, vga_to_rgb[10]);
            } else {
                int idx = w->cursor_y * CONTENT_COLS_MAX + w->cursor_x;
                if (idx >= 0 && idx < CONTENT_CELLS_MAX) {
                    uint8_t attr = (w->cell_attr) ? w->cell_attr[idx] : 0;
                    draw_char_cell(bx, by, w->content[idx] & 0xFF,
                        w->cell_fg[idx], w->cell_bg[idx], char_w, char_h, attr);
                }
            }
        }
    }
}

// Paint (px,py,pw,ph) of window id clipped to the parts NOT covered by
// higher-z windows. A dirty lower window must never draw over a clean higher
// one — the higher window won't repaint to cover it back, so background
// terminal output would bleed straight through a fullscreen browser.
// Falls back to an unclipped full paint if fragmentation overflows (a
// transient overpaint self-heals; an unpainted hole would persist).
#define OCCLUDE_MAX 24
static void window_paint_uncovered(int id, int px, int py, int pw, int ph) {
    struct window* w = &windows[id];
    int rects[OCCLUDE_MAX][4], nr = 1;
    rects[0][0] = px; rects[0][1] = py; rects[0][2] = pw; rects[0][3] = ph;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* hw = &windows[i];
        if (i == id || !hw->visible || hw->minimized || hw->z < w->z) continue;
        if (hw->z == w->z) continue; // unique in practice; paint order covers ties
        int hx0 = hw->x, hy0 = hw->y,
            hx1 = hw->x + hw->w, hy1 = hw->y + hw->h;
        int nr2 = 0, r2[OCCLUDE_MAX][4];
        for (int k = 0; k < nr; k++) {
            int rx = rects[k][0], ry = rects[k][1],
                rw = rects[k][2], rh = rects[k][3];
            int rx1 = rx + rw, ry1 = ry + rh;
            if (hx0 >= rx1 || hx1 <= rx || hy0 >= ry1 || hy1 <= ry) {
                if (nr2 < OCCLUDE_MAX) {
                    r2[nr2][0]=rx; r2[nr2][1]=ry; r2[nr2][2]=rw; r2[nr2][3]=rh; nr2++;
                } else goto fallback;
                continue;
            }
            if (rx < hx0) {
                if (nr2 >= OCCLUDE_MAX) goto fallback;
                r2[nr2][0]=rx; r2[nr2][1]=ry; r2[nr2][2]=hx0-rx; r2[nr2][3]=rh; nr2++;
            }
            if (rx1 > hx1) {
                if (nr2 >= OCCLUDE_MAX) goto fallback;
                r2[nr2][0]=hx1; r2[nr2][1]=ry; r2[nr2][2]=rx1-hx1; r2[nr2][3]=rh; nr2++;
            }
            {
                int lx = rx > hx0 ? rx : hx0, rxr = rx1 < hx1 ? rx1 : hx1;
                if (ry < hy0) {
                    if (nr2 >= OCCLUDE_MAX) goto fallback;
                    r2[nr2][0]=lx; r2[nr2][1]=ry; r2[nr2][2]=rxr-lx; r2[nr2][3]=hy0-ry; nr2++;
                }
                if (ry1 > hy1) {
                    if (nr2 >= OCCLUDE_MAX) goto fallback;
                    r2[nr2][0]=lx; r2[nr2][1]=hy1; r2[nr2][2]=rxr-lx; r2[nr2][3]=ry1-hy1; nr2++;
                }
            }
        }
        nr = nr2;
        for (int k = 0; k < nr; k++) {
            rects[k][0]=r2[k][0]; rects[k][1]=r2[k][1];
            rects[k][2]=r2[k][2]; rects[k][3]=r2[k][3];
        }
        if (nr == 0) { graphics_clip_reset(); return; }
    }
    for (int k = 0; k < nr; k++) {
        if (rects[k][2] <= 0 || rects[k][3] <= 0) continue;
        graphics_set_clip(rects[k][0], rects[k][1], rects[k][2], rects[k][3]);
        window_paint_region(id, rects[k][0], rects[k][1], rects[k][2], rects[k][3]);
    }
    graphics_clip_reset();
    return;
fallback:
    graphics_clip_reset();
    window_paint_region(id, px, py, pw, ph);
}

int window_draw(int id) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return 0;
    int blink = window_blink_on();
    int cursor_live = (w->focused && w->content && !w->hide_cursor);
    int blink_changed = (cursor_live && blink != w->last_cursor_visible);
    if (!w->dirty && !blink_changed) return 0;
    if (w->dirty && w->pix && w->pxd_valid) {
        // Partial pixel-surface repaint (a focused form field, a scrolled band).
        int title_off = w->no_titlebar ? 0 : WIN_TITLE_H;
        int px = w->x + WIN_BORDER + w->pix_x + w->pxd_x0;
        int py = w->y + WIN_BORDER + title_off + w->pix_y + w->pxd_y0;
        window_paint_uncovered(id, px, py, w->pxd_x1 - w->pxd_x0, w->pxd_y1 - w->pxd_y0);
        w->dirty = 0; w->pr_valid = 0; w->pxd_valid = 0; w->last_cursor_visible = blink;
        return 1;
    }
    if (w->dirty && w->pr_valid && !w->scroll_off && w->content) {
        // Partial repaint: only the marked cells + the cursor cell. Typing a
        // key repaints 1-3 cells instead of ~2000 — same pixels, ~1000x less.
        // scroll_off>0 falls through to full (marks are buffer coords, the
        // view shows history rows then).
        int scale = w->font_scale;
        int char_w = CONTENT_GW * scale, char_h = CONTENT_GH * scale;
        int title_off = w->no_titlebar ? 0 : WIN_TITLE_H;
        int cx = w->x + WIN_BORDER, cy = w->y + WIN_BORDER + title_off;
        int c0 = w->pr_c0, c1 = w->pr_c1, r0 = w->pr_r0, r1 = w->pr_r1;
        if (cursor_live) { // cursor cell must be in the clip (blink on/off)
            if (w->cursor_x < c0) c0 = w->cursor_x;
            if (w->cursor_x > c1) c1 = w->cursor_x;
            if (w->cursor_y < r0) r0 = w->cursor_y;
            if (w->cursor_y > r1) r1 = w->cursor_y;
        }
        if (c0 < 0) c0 = 0;
        if (r0 < 0) r0 = 0;
        if (c1 >= w->content_w) c1 = w->content_w - 1;
        if (r1 >= w->content_h) r1 = w->content_h - 1;
        if (c1 >= c0 && r1 >= r0) {
            int px = cx + c0 * char_w, py = cy + r0 * char_h;
            int pw = (c1 - c0 + 1) * char_w, ph = (r1 - r0 + 1) * char_h;
            // Occlusion-clipped: never draw over a clean higher window.
            window_paint_uncovered(id, px, py, pw, ph);
        }
        w->dirty = 0; w->pr_valid = 0; w->last_cursor_visible = blink;
        return 1;
    }
    if (!w->dirty && w->scroll_off) {
        // Blink toggled while scrolled back: the cursor belongs to the live
        // tail (not painted in history view), so a repaint would be pure
        // waste — just track the state. (Old code repainted the full window.)
        w->last_cursor_visible = blink;
        return 0;
    }
    if (!w->dirty) {
        // Blink-only change: repaint just the cursor cell (uncovered only).
        int scale = w->font_scale;
        int char_w = CONTENT_GW * scale, char_h = CONTENT_GH * scale;
        int title_off = w->no_titlebar ? 0 : WIN_TITLE_H;
        int bx = w->x + WIN_BORDER + w->cursor_x * char_w;
        int by = w->y + WIN_BORDER + title_off + w->cursor_y * char_h;
        window_paint_uncovered(id, bx, by, char_w, char_h);
        w->last_cursor_visible = blink;
        return 1;
    }
    window_paint_uncovered(id, w->x, w->y, w->w, w->h);
    w->dirty = 0; w->pr_valid = 0; w->pxd_valid = 0; w->last_cursor_visible = blink;
    return 1;
}

void window_draw_all(void) {
    int focused = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].visible) { if (windows[i].focused) focused = i; else window_draw(i); }
    }
    if (focused >= 0) window_draw(focused);
}

void window_set_dirty(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].dirty = 1;
        window_mark_all(&windows[id]); // legacy callers mean full repaint
    }
}

static void scroll_content(struct window* w) {
    if (w->cursor_y >= w->content_h) {
        // The top line leaves the grid — archive it into the scrollback ring.
        int id = (int)(w - windows);
        if (id >= 0 && id < MAX_WINDOWS) {
            for (int col = 0; col < w->content_w; col++)
                sb_ring[id][sb_next[id]][col] = w->content[col];
            sb_next[id] = (sb_next[id] + 1) % SB_ROWS;
            if (sb_count[id] < SB_ROWS) sb_count[id]++;
            w->scroll_off = 0; // new output → follow the tail again
        }
        for (int row = 0; row < w->content_h - 1; row++) {
            for (int col = 0; col < w->content_w; col++) {
                int dst = row * CONTENT_COLS_MAX + col, src = (row + 1) * CONTENT_COLS_MAX + col;
                w->content[dst] = w->content[src];
                w->cell_fg[dst] = w->cell_fg[src];
                w->cell_bg[dst] = w->cell_bg[src];
                if (w->cell_attr) w->cell_attr[dst] = w->cell_attr[src];
            }
        }
        int last = (w->content_h - 1) * CONTENT_COLS_MAX;
        for (int col = 0; col < w->content_w; col++) cell_blank(w, last + col);
        w->cursor_y = w->content_h - 1;
        window_mark_all(w); // grid shifted — every row changed
    }
}

// Mouse-wheel scroll: `notches` follows the okai convention (positive = wheel
// down = toward the live tail; negative = back through history). 3 lines per
// detent, clamped to [0, archived lines].
void window_scroll_view(int id, int notches) {
    if (id < 0 || id >= MAX_WINDOWS || notches == 0) return;
    struct window* w = &windows[id];
    if (!w->visible) return;
    int off = w->scroll_off - notches * 3;
    if (off > sb_count[id]) off = sb_count[id];
    if (off < 0) off = 0;
    if (off != w->scroll_off) {
        w->scroll_off = off;
        w->dirty = 1; window_mark_all(w); needs_redraw = 1;
        serial_printf("[scr] win=%d off=%d hist=%d\n", id, off, sb_count[id]);
    }
}

void window_put_char(int id, char c) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
    int ocx = w->cursor_x, ocy = w->cursor_y; // old cursor cell (blink erase)
    uint8_t color = (w->text_bg << 4) | w->text_fg;
    uint32_t idx = (uint32_t)w->cursor_y * CONTENT_COLS_MAX + w->cursor_x;
    if (c == '\n') { w->cursor_x = 0; w->cursor_y++; }
    else if (c == '\r') { w->cursor_x = 0; }
    else if (c == '\b') {
        if (w->cursor_x > 0) { w->cursor_x--; cell_blank(w, --idx); }
    } else {
        if (w->cursor_x < w->content_w) {
            w->content[idx] = (uint16_t)((uint16_t)color << 8) | (uint16_t)(uint8_t)c;
            w->cell_fg[idx] = w->text_fg_rgb;
            w->cell_bg[idx] = w->text_bg_rgb;
            if (w->cell_attr) w->cell_attr[idx] = 0; // terminal text is plain
            w->cursor_x++;
        }
    }
    if (w->cursor_x >= w->content_w) { w->cursor_x = 0; w->cursor_y++; }
    if (w->cursor_y >= w->content_h) {
        scroll_content(w); // marks all internally
    } else {
        scroll_content(w);
        // Typing touches at most the written cell + old/new cursor cells.
        window_mark_cell(w, ocy, ocx);
        window_mark_cell(w, w->cursor_y, w->cursor_x);
    }
    w->dirty = 1;
}

void window_puts(int id, const char* str) { while (*str) window_put_char(id, *str++); }

// Keep the legacy VGA attr byte roughly in sync when writing exact RGB
// directly: nearest palette entry by Euclidean distance (paint never reads it).
static uint8_t rgb_to_vga(uint32_t rgb) {
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    int best = 0, best_d = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        uint32_t c = vga_to_rgb[i];
        int dr = r - (int)((c >> 16) & 0xFF), dg = g - (int)((c >> 8) & 0xFF), db = b - (int)(c & 0xFF);
        int d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

void window_write_cell(int id, int row, int col, char c, uint8_t fg, uint8_t bg) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
    if (row < 0 || row >= w->content_h || col < 0 || col >= w->content_w) return;
    uint32_t idx = (uint32_t)row * CONTENT_COLS_MAX + col;
    uint16_t color = (uint16_t)((bg << 4) | fg);
    w->content[idx] = (uint16_t)((uint16_t)color << 8) | (uint8_t)c;
    w->cell_fg[idx] = vga_to_rgb[fg & 0x0F];
    w->cell_bg[idx] = vga_to_rgb[(bg >> 4) & 0x0F];
    if (w->cell_attr) w->cell_attr[idx] = 0;
    window_mark_cell(w, row, col);
    w->dirty = 1;
}

void window_write_cell_rgb(int id, int row, int col, char c, uint32_t fg, uint32_t bg) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
    if (row < 0 || row >= w->content_h || col < 0 || col >= w->content_w) return;
    uint32_t idx = (uint32_t)row * CONTENT_COLS_MAX + col;
    // Legacy VGA byte: live okai rows paint from the RGB planes (never from
    // this byte) and okai never enters the scrollback ring, so a full
    // rgb_to_vga Euclidean match (16 entries x mults) per cell is pure waste
    // on the hottest blit path. Luminance threshold keeps it plausible.
    uint8_t fgi = window_rgb_is_light(fg) ? 15 : 0;
    uint8_t bgi = window_rgb_is_light(bg) ? 15 : 0;
    w->content[idx] = (uint16_t)((uint16_t)(((bgi << 4) | fgi) << 8)) | (uint8_t)(uint8_t)c;
    w->cell_fg[idx] = fg;
    w->cell_bg[idx] = bg;
    if (w->cell_attr) w->cell_attr[idx] = 0; // styling follows via window_write_cell_attr
    window_mark_cell(w, row, col);
    w->dirty = 1;
}

void window_write_cell_attr(int id, int row, int col, uint8_t attr) {
    struct window* w = window_get(id);
    if (!w || !w->visible || !w->content || !w->cell_attr) return;
    if (row < 0 || row >= w->content_h || col < 0 || col >= w->content_w) return;
    w->cell_attr[(uint32_t)row * CONTENT_COLS_MAX + col] = attr;
    window_mark_cell(w, row, col);
    w->dirty = 1;
}

// CJK model cell. The single-byte model cannot hold a 21-bit codepoint, so
// it is bit-packed into the otherwise-unused high bytes: fg[31:24] holds
// cp[20:13], bg[31:24] holds cp[12:5], and attr carries 0x80 (CJK flag) +
// cp[4:0] in bits 6:2 with the styling bits (BOLD/UL) in 1:0. Repair
// repaints decode the exact glyph from the model (branch below) — drags and
// focus changes never degrade CJK to tofu. Block-slot cells (0x01/0x02
// bytes) never set bit 7, so the two never collide.
void window_write_cjk_cell(int id, int row, int col, uint32_t cp,
                           uint32_t fg, uint32_t bg, uint8_t style) {
    struct window* w = window_get(id);
    if (!w || !w->visible || !w->content) return;
    if (row < 0 || row >= w->content_h || col < 0 || col >= w->content_w) return;
    uint32_t idx = (uint32_t)row * CONTENT_COLS_MAX + col;
    uint8_t fgi = window_rgb_is_light(fg) ? 15 : 0;
    uint8_t bgi = window_rgb_is_light(bg) ? 15 : 0;
    w->content[idx] = (uint16_t)(((uint16_t)(((bgi << 4) | fgi) << 8)) | (uint8_t)' ');
    w->cell_fg[idx] = (fg & 0xFFFFFFu) | (((cp >> 13) & 0xFFu) << 24);
    w->cell_bg[idx] = (bg & 0xFFFFFFu) | (((cp >> 5) & 0xFFu) << 24);
    if (w->cell_attr)
        w->cell_attr[idx] = (uint8_t)(0x80 | (((cp & 0x1Fu) << 2) & 0xFFu) | (style & 3));
    window_mark_cell(w, row, col);
    w->dirty = 1;
}

void window_set_cursor(int id, int row, int col) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    if (row < 0) row = 0;
    if (row >= w->content_h) row = w->content_h - 1;
    if (col < 0) col = 0;
    if (col >= w->content_w) col = w->content_w - 1;
    w->cursor_x = col;
    w->cursor_y = row;
}

void window_clear(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    if (!w->content) return;
    for (int r = 0; r < w->content_h; r++)
        for (int c = 0; c < w->content_w; c++)
            cell_blank(w, r * CONTENT_COLS_MAX + c);
    w->cursor_x = 0; w->cursor_y = 0; w->dirty = 1;
    window_mark_all(w);
    sb_count[id] = 0; sb_next[id] = 0; w->scroll_off = 0; // wipe history too
}

void window_set_text_color(int id, uint8_t fg, uint8_t bg) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    w->text_fg = fg & 0x0F;
    w->text_bg = bg & 0x0F;
    w->text_fg_rgb = vga_to_rgb[fg & 0x0F];
    w->text_bg_rgb = vga_to_rgb[bg & 0x0F];
}

void window_set_text_color_rgb(int id, uint32_t fg, uint32_t bg) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    w->text_fg_rgb = fg;
    w->text_bg_rgb = bg;
    w->text_fg = rgb_to_vga(fg);
    w->text_bg = rgb_to_vga(bg);
}

void window_set_content_bg(int id, uint8_t bg) {
    if (id >= 0 && id < MAX_WINDOWS) {
        struct window* w = &windows[id];
        if (w->content_bg != bg || w->content_bg_rgb != vga_to_rgb[bg]) {
            w->content_bg = bg;
            w->content_bg_rgb = vga_to_rgb[bg];
            w->dirty = 1; window_mark_all(w); needs_redraw = 1;
        }
    }
}

void window_set_content_bg_rgb(int id, uint32_t bg) {
    if (id >= 0 && id < MAX_WINDOWS) {
        struct window* w = &windows[id];
        if (w->content_bg_rgb != bg) {
            w->content_bg_rgb = bg;
            w->content_bg = rgb_to_vga(bg);
            w->text_bg = rgb_to_vga(bg);
            w->text_bg_rgb = bg;
            w->dirty = 1; window_mark_all(w); needs_redraw = 1;
        }
    }
}

// True if an RGB color reads as light (high luminance), used to pick a
// readable default text color when a page sets a background.
int window_rgb_is_light(uint32_t c) {
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    int lum = (r * 77 + g * 150 + b * 29) / 256; // perceptual-ish 0..255
    return lum > 128;
}

int window_color_is_light(uint8_t idx) {
    if (idx > 15) return 0;
    return window_rgb_is_light(vga_to_rgb[idx]);
}

void window_set_title(int id, const char* title) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    int j = 0; while (title[j] && j < 31) { windows[id].title[j] = title[j]; j++; }
    windows[id].title[j] = 0; windows[id].dirty = 1;
    window_mark_all(&windows[id]);
}

