#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>

extern int needs_redraw;
extern void desktop_paint_rect_pub(int x, int y, int w, int h);

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
    w->dirty = 1; needs_redraw = 1;
}

int window_create(const char* title, int x, int y, int w, int h) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].visible) {
            windows[i].x = x; windows[i].y = y; windows[i].w = w; windows[i].h = h;
            windows[i].visible = 1; windows[i].focused = 0; windows[i].font_scale = 1;
            windows[i].z = i;
            windows[i].no_titlebar = 0;
            windows[i].text_fg = 15; windows[i].text_bg = 0;
            windows[i].text_fg_rgb = vga_to_rgb[15]; windows[i].text_bg_rgb = vga_to_rgb[0];
            windows[i].content_bg = WIN_BG;
            windows[i].content_bg_rgb = WIN_BG_RGB;
            windows[i].dirty = 1; needs_redraw = 1;
            sb_count[i] = 0; sb_next[i] = 0; windows[i].scroll_off = 0;
            int j = 0;
            while (title[j] && j < 31) { windows[i].title[j] = title[j]; j++; }
            windows[i].title[j] = 0;
            windows[i].content = (uint16_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint16_t));
            windows[i].cell_fg = (uint32_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint32_t));
            windows[i].cell_bg = (uint32_t*)kmalloc(CONTENT_CELLS_MAX * sizeof(uint32_t));
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
    windows[id].visible = 0;
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
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].focused = (i == id);
    window_raise(id);   // focused window is always topmost
}

void window_set_close_button(int id, int has_close) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].has_close_button = has_close;
}

void window_set_minimize_button(int id, int has_min) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].has_minimize_button = has_min;
}

void window_set_no_titlebar(int id, int flag) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    struct window* w = &windows[id];
    if (w->no_titlebar == flag) return;
    w->no_titlebar = flag;
    window_apply_metrics(w);
}

void window_set_hide_cursor(int id, int flag) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    windows[id].hide_cursor = flag;
    windows[id].dirty = 1;
}

int window_check_close_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_close_button) return 0;
    if (w->no_titlebar) {
        int s = WIN_CTRL_BTN;
        int bx = w->x + w->w - WIN_BORDER - 4 - s, by = w->y + WIN_BORDER;
        return (mx >= bx && mx < bx + s && my >= by && my < by + s);
    }
    int bx = w->x + w->w - WIN_BORDER - 4 - WIN_BTN_W, by = w->y + WIN_BORDER;
    return (mx >= bx && mx < bx + WIN_BTN_W && my >= by && my < by + WIN_BTN_H);
}

int window_check_minimize_click(int id, int mx, int my) {
    struct window* w = &windows[id];
    if (!w->visible || !w->has_minimize_button) return 0;
    if (w->no_titlebar) return 0; // no minimize control in title-bar-less mode
    int bx = w->x + w->w - WIN_BORDER - 8 - 2 * WIN_BTN_W, by = w->y + WIN_BORDER;
    return (mx >= bx && mx < bx + WIN_BTN_W && my >= by && my < by + WIN_BTN_H);
}

void window_minimize(int id) {
    if (id >= 0 && id < MAX_WINDOWS) {
        windows[id].minimized = 1; windows[id].focused = 0;
        desktop_paint_rect_pub(windows[id].x, windows[id].y, windows[id].w, windows[id].h);
    }
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
        uint32_t bc = w->focused ? BORDER_ACTIVE : BORDER_INACTIVE;
        rect_outline(w->x, w->y, w->w, w->h, bc, WIN_BORDER);
    }
    // Title bar (skipped for title-bar-less windows; the client draws its own top).
    // Same blue gradient family as the browser toolbar (theme.h) so the OS
    // chrome and the browser chrome read as one system.
    if (!w->no_titlebar &&
        rx < w->x + w->w && rx + rw > w->x && ry < title_bottom && ry + rh > w->y) {
        int tx = w->x + WIN_BORDER, ty = w->y + WIN_BORDER;
        int tw = w->w - 2 * WIN_BORDER;
        if (w->focused)
            gradient_fill(tx, ty, tw, WIN_TITLE_H, CHROME_TOOL_TOP, CHROME_TOOL_BOT, 1);
        else
            gradient_fill(tx, ty, tw, WIN_TITLE_H, TITLE_GRAD_U_TOP, TITLE_GRAD_U_BOT, 1);
        hline(tx, ty + WIN_TITLE_H - 1, tw, TITLE_DIVIDER);
        draw_string_fg(tx + 4, ty + 4, w->title,
                       w->focused ? TITLE_TEXT_F : TITLE_TEXT_U);
        if (w->has_close_button) {
            int bx = w->x + w->w - WIN_BORDER - 4 - WIN_BTN_W, by = w->y + WIN_BORDER;
            round_rect_fill(bx, by, WIN_BTN_W, WIN_BTN_H, TBTN_CLOSE_BG, 4);
            int cx = bx + WIN_BTN_W / 2, cy = by + WIN_BTN_H / 2, d = 5;
            line(cx - d, cy - d, cx + d, cy + d, TBTN_FG);
            line(cx - d + 1, cy - d, cx + d + 1, cy + d, TBTN_FG);
            line(cx + d, cy - d, cx - d, cy + d, TBTN_FG);
            line(cx + d + 1, cy - d, cx - d + 1, cy + d, TBTN_FG);
        }
        if (w->has_minimize_button) {
            int bx = w->x + w->w - WIN_BORDER - 8 - 2 * WIN_BTN_W, by = w->y + WIN_BORDER;
            round_rect_fill(bx, by, WIN_BTN_W, WIN_BTN_H, TBTN_NEUTRAL_BG, 4);
            int cy = by + WIN_BTN_H / 2 + 4, d = 5;
            hline(bx + WIN_BTN_W / 2 - d, cy, 2 * d + 1, TBTN_FG);
            hline(bx + WIN_BTN_W / 2 - d, cy + 1, 2 * d + 1, TBTN_FG);
        }
    }

    // ---- Content cells (pre-computed row/col range, no redundant bg fill) ----
    int cx = w->x + WIN_BORDER, cy = w->y + WIN_BORDER + title_off;
    int cw = w->w - 2 * WIN_BORDER, ch = w->h - title_off - 2 * WIN_BORDER;
    int x0 = cx > rx ? cx : rx, y0 = cy > ry ? cy : ry;
    int x1 = (cx + cw) < (rx + rw) ? (cx + cw) : (rx + rw);
    int y1 = (cy + ch) < (ry + rh) ? (cy + ch) : (ry + rh);

    if (x0 < x1 && y0 < y1) {
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
                }
                for (int col = col_start; col < col_end; col++) {
                    int px = cx + col * char_w;
                    uint16_t entry = src[col];
                    char c = entry & 0xFF;
                    uint32_t fg, bg;
                    if (sfg) { fg = sfg[col]; bg = sbg[col]; }
                    else {
                        uint8_t color = (entry >> 8) & 0xFF;
                        fg = vga_to_rgb[color & 0x0F];
                        bg = vga_to_rgb[(color >> 4) & 0x0F];
                    }
                    draw_char_sized(px, py, c, fg, bg, char_w, char_h);
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
                    draw_char_sized(bx, by, w->content[idx] & 0xFF,
                        w->cell_fg[idx], w->cell_bg[idx], char_w, char_h);
                }
            }
        }
    }
}

void window_draw(int id) {
    struct window* w = &windows[id];
    if (!w->visible || w->minimized) return;
    int blink = window_blink_on();
    if (w->dirty || (w->focused && !w->hide_cursor && blink != w->last_cursor_visible)) {
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

void window_set_dirty(int id) {
    if (id >= 0 && id < MAX_WINDOWS) windows[id].dirty = 1;
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
            }
        }
        int last = (w->content_h - 1) * CONTENT_COLS_MAX;
        for (int col = 0; col < w->content_w; col++) cell_blank(w, last + col);
        w->cursor_y = w->content_h - 1;
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
        w->dirty = 1; needs_redraw = 1;
        serial_printf("[scr] win=%d off=%d hist=%d\n", id, off, sb_count[id]);
    }
}

void window_put_char(int id, char c) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
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
            w->cursor_x++;
        }
    }
    if (w->cursor_x >= w->content_w) { w->cursor_x = 0; w->cursor_y++; }
    scroll_content(w);
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
    w->dirty = 1;
}

void window_write_cell_rgb(int id, int row, int col, char c, uint32_t fg, uint32_t bg) {
    struct window* w = &windows[id];
    if (!w->visible || !w->content) return;
    if (row < 0 || row >= w->content_h || col < 0 || col >= w->content_w) return;
    uint32_t idx = (uint32_t)row * CONTENT_COLS_MAX + col;
    uint8_t fgi = rgb_to_vga(fg), bgi = rgb_to_vga(bg);
    w->content[idx] = (uint16_t)((uint16_t)(((bgi << 4) | fgi) << 8)) | (uint8_t)(uint8_t)c;
    w->cell_fg[idx] = fg;
    w->cell_bg[idx] = bg;
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
            w->dirty = 1; needs_redraw = 1;
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
            w->dirty = 1; needs_redraw = 1;
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
}

void window_draw_taskbar(void) {
    int y = SCREEN_H - TASKBAR_H;
    gradient_fill(0, y, SCREEN_W, TASKBAR_H, TASKBAR_TOP, TASKBAR_BOT, 1);
    hline(0, y, SCREEN_W, TITLE_DIVIDER);

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
        int focused = w->focused && !w->minimized;
        uint32_t btn_bg = focused ? CHROME_TAB_ACTIVE : TASK_BTN_INACT;
        round_rect_fill(x, y + 3, btn_w, TASKBAR_H - 6, btn_bg, 4);
        char label[20]; int li = 0;
        int max_chars = (btn_w - 8) / CHAR_W;
        if (max_chars > 18) max_chars = 18;
        while (w->title[li] && li < max_chars) { label[li] = w->title[li]; li++; }
        label[li] = 0;
        draw_string_fg(x + 4, y + 5, label, focused ? CHROME_TAB_TEXT : TITLE_TEXT_F);
        x += btn_w + 6;
    }
}
