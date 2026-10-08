// Mouse: PS/2 driver + a framebuffer-composited ("software hardware") cursor.
//
// The cursor is NEVER drawn into the backbuffer. It lives only on the
// framebuffer: the sprite is alpha-blended over the backbuffer's scene pixels
// and written straight to video memory. Consequences:
//   * moving it is two tiny rect copies (restore the old rect from the
//     backbuffer, blend the new one) done right in the mouse IRQ — the cursor
//     tracks the hand even while the main loop is busy rendering a page;
//   * the desktop never recomposites anything because the mouse moved (the
//     old path erased the sprite by re-painting wallpaper + every window +
//     okai chrome + taskbar under it, every loop iteration);
//   * no hover/repair race can leave sprite residue in the scene: the scene
//     never contains the sprite.
// graphics_flush() copies dirty backbuffer rows to the framebuffer in short
// interrupt-free chunks and calls cursor_fb_repair() after each, so a flush
// can never overwrite the sprite or interleave with an IRQ-time move.
//
// The driver applies packets 1:1 (proper 9-bit sign + overflow handling, no
// smoothing: the old 4-packet moving average made the cursor trail the hand
// and swallowed slow moves — |d| < 4 averaged to 0).
#include "cursor.h"
#include "graphics.h"
#include "ui_draw.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>

// ---- driver state -------------------------------------------------------------

static volatile int mouse_x, mouse_y;
static volatile int mouse_buttons;
static volatile int mouse_scroll;       // accumulated wheel delta, consumed per frame
static int mouse_cycle;
static uint8_t mouse_bytes[4];
static int mouse_has_wheel;
static uint32_t mse_pkt_count;
static uint8_t mse_prev_buttons;

// ---- sprite -----------------------------------------------------------------

#define SPR_W 24
#define SPR_H 30
#define HOT_X 3              // the arrow tip inside the sprite
#define HOT_Y 2
static uint32_t spr_premul[SPR_W * SPR_H];   // premultiplied 0x00RRGGBB
static uint8_t spr_alpha[SPR_W * SPR_H];
static int spr_x0, spr_x1, spr_y0, spr_y1;   // tight bounds of alpha > 0

static volatile int cur_on;                  // sprite live on the framebuffer
static int cur_x = -1000, cur_y = -1000;     // sprite top-left currently drawn

// Arrow outline (px, tip at the origin), macOS-like proportions.
static const int32_t ARROW[] = {
    0, 0,   0, 4224,   1024, 3264,   1728, 4864,   2368, 4576,   1728, 3008,   3008, 3008,
};
#define ARROW_N 7

static void arrow_path(int ox256, int oy256) {
    int32_t xy[2 * ARROW_N];
    for (int i = 0; i < ARROW_N; i++) {
        xy[2 * i] = ARROW[2 * i] + ox256;
        xy[2 * i + 1] = ARROW[2 * i + 1] + oy256;
    }
    ui_poly(xy, ARROW_N);
}

static void render_sprite(struct wsurf* s) {
    int ox = HOT_X * 256, oy = HOT_Y * 256;
    // soft drop shadow: a few offset translucent copies
    static const int sh[3][3] = { { 128, 320, 0x22 }, { 256, 576, 0x1C }, { 448, 832, 0x14 } };
    for (int k = 0; k < 3; k++) {
        ui_path_begin();
        arrow_path(ox + sh[k][0], oy + sh[k][1]);
        ui_path_fill(s, ((uint32_t)sh[k][2] << 24) | 0x000000);
    }
    // black body
    ui_path_begin();
    arrow_path(ox, oy);
    ui_path_fill(s, 0x00101014);
    // white outline (straddles the edge: ~1px out, ~1px in)
    struct ui_pen p;
    ui_pen_at(&p, 0, 0, 230);
    ui_path_begin();
    for (int i = 0; i < ARROW_N; i++) {
        int j = (i + 1) % ARROW_N;
        ui_seg(&p, ARROW[2 * i] + ox, ARROW[2 * i + 1] + oy,
               ARROW[2 * j] + ox, ARROW[2 * j + 1] + oy, 1);
    }
    ui_path_fill(s, 0x00FFFFFF);
}

// Render over black and over white; alpha = 255 - (white - black), and the
// over-black render IS the premultiplied color.
static void build_sprite(void) {
    static uint32_t onb[SPR_W * SPR_H], onw[SPR_W * SPR_H];
    struct wsurf s;
    s.w = SPR_W; s.h = SPR_H; s.stride = SPR_W;
    for (int i = 0; i < SPR_W * SPR_H; i++) { onb[i] = 0; onw[i] = 0xFFFFFF; }
    s.px = onb; ws_reset_clip(&s); render_sprite(&s);
    s.px = onw; ws_reset_clip(&s); render_sprite(&s);
    spr_x0 = SPR_W; spr_y0 = SPR_H; spr_x1 = 0; spr_y1 = 0;
    for (int y = 0; y < SPR_H; y++)
        for (int x = 0; x < SPR_W; x++) {
            int i = y * SPR_W + x;
            int gb = (onb[i] >> 8) & 0xFF, gw = (onw[i] >> 8) & 0xFF;
            int a = 255 - (gw - gb);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            if (a < 3) a = 0;
            spr_alpha[i] = (uint8_t)a;
            spr_premul[i] = a ? (onb[i] & 0xFFFFFF) : 0;
            if (a) {
                if (x < spr_x0) spr_x0 = x;
                if (x + 1 > spr_x1) spr_x1 = x + 1;
                if (y < spr_y0) spr_y0 = y;
                if (y + 1 > spr_y1) spr_y1 = y + 1;
            }
        }
}

// ---- framebuffer compositing (IF=0 callers only) -------------------------------

static inline uint32_t blend_premul(uint32_t src, unsigned a, uint32_t dst) {
    unsigned ia = 255 - a;
    uint32_t rb = (dst & 0xFF00FF) * ia + 0x800080;
    uint32_t g = (dst & 0x00FF00) * ia + 0x008000;
    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    return src + (rb | g);
}

// Copy backbuffer -> framebuffer for a rect (clipped to the screen).
static void fb_restore(int x, int y, int w, int h) {
    uint8_t* fb = graphics_fb_base();
    const uint32_t* bb = graphics_get_buffer();
    if (!fb || !bb) return;
    int pitch = graphics_fb_pitch();
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    int y1 = y + h > SCREEN_H ? SCREEN_H : y + h;
    for (int r = y0; r < y1; r++) {
        uint32_t* d = (uint32_t*)(fb + r * pitch);
        const uint32_t* s = bb + r * SCREEN_W;
        for (int c = x0; c < x1; c++) d[c] = s[c];
    }
}

// Blend the sprite at top-left (sx, sy) over the backbuffer scene into the
// framebuffer, rows [ry0, ry1) only.
static void fb_draw(int sx, int sy, int ry0, int ry1) {
    uint8_t* fb = graphics_fb_base();
    const uint32_t* bb = graphics_get_buffer();
    if (!fb || !bb) return;
    int pitch = graphics_fb_pitch();
    int y0 = sy + spr_y0, y1 = sy + spr_y1;
    if (y0 < ry0) y0 = ry0;
    if (y1 > ry1) y1 = ry1;
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    int x0 = sx + spr_x0, x1 = sx + spr_x1;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    for (int r = y0; r < y1; r++) {
        uint32_t* d = (uint32_t*)(fb + r * pitch);
        const uint32_t* s = bb + r * SCREEN_W;
        const uint8_t* al = spr_alpha + (r - sy) * SPR_W - sx;
        const uint32_t* pm = spr_premul + (r - sy) * SPR_W - sx;
        for (int c = x0; c < x1; c++) {
            unsigned a = al[c];
            if (a) d[c] = blend_premul(pm[c], a, s[c]);
        }
    }
}

static void move_sprite(int hx, int hy) {
    int nx = hx - HOT_X, ny = hy - HOT_Y;
    if (nx == cur_x && ny == cur_y) return;
    fb_restore(cur_x + spr_x0, cur_y + spr_y0, spr_x1 - spr_x0, spr_y1 - spr_y0);
    cur_x = nx; cur_y = ny;
    fb_draw(cur_x, cur_y, 0, SCREEN_H);
}

void cursor_fb_repair(int y0, int y1) {
    if (!cur_on) return;
    if (cur_y + spr_y1 <= y0 || cur_y + spr_y0 >= y1) return;
    fb_draw(cur_x, cur_y, y0, y1);
}

static uint32_t irq_save(void) {
    uint32_t f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}
static void irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

void cursor_enable(void) {
    uint32_t f = irq_save();
    if (!cur_on) {
        cur_on = 1;
        cur_x = mouse_x - HOT_X; cur_y = mouse_y - HOT_Y;
        fb_draw(cur_x, cur_y, 0, SCREEN_H);
    }
    irq_restore(f);
}

// ---- PS/2 driver ----------------------------------------------------------------

static void mouse_process_packet(void) {
    uint8_t b0 = mouse_bytes[0];
    mouse_buttons = b0 & 0x07;
    if (!(b0 & 0xC0)) {                       // overflow packets carry garbage deltas
        int dx = (int)mouse_bytes[1] - ((b0 & 0x10) ? 256 : 0);
        int dy = (int)mouse_bytes[2] - ((b0 & 0x20) ? 256 : 0);
        int x = mouse_x + dx, y = mouse_y - dy;
        if (x < 0) x = 0;
        if (x >= SCREEN_W) x = SCREEN_W - 1;
        if (y < 0) y = 0;
        if (y >= SCREEN_H) y = SCREEN_H - 1;
        mouse_x = x; mouse_y = y;
    }
    mse_pkt_count++;
    // Ground truth for headless mouse tests: button transitions with the
    // position and cumulative packet count.
    if (mouse_buttons != mse_prev_buttons) {
        serial_printf("[mse] btn=%d x=%d y=%d pkts=%u\n",
                      mouse_buttons, mouse_x, mouse_y, mse_pkt_count);
        mse_prev_buttons = (uint8_t)mouse_buttons;
    }
    if (cur_on) move_sprite(mouse_x, mouse_y);  // IRQ context: IF=0
}

static void mouse_irq_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x01)) return;
    if (!(status & 0x20)) return;             // keyboard byte: not ours to eat
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
    uint32_t f = irq_save();
    *x = mouse_x; *y = mouse_y;
    irq_restore(f);
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
int mouse_get_left_button(void) { return mouse_buttons & 0x01; }

int mouse_get_scroll(void) {
    uint32_t f = irq_save();
    int s = mouse_scroll;
    mouse_scroll = 0;
    irq_restore(f);
    return s;
}

static void ps2_wait_in(void) {   // controller input buffer empty
    while (inb(0x64) & 0x02) {}
}

// Wait for the controller output buffer, then read the byte (bounded).
static int ps2_read_wait(void) {
    for (int spin = 0; spin < 100000; spin++)
        if (inb(0x64) & 0x01) return inb(0x60);
    return -1;
}

static void ps2_mouse_cmd(uint8_t b) {
    ps2_wait_in(); outb(0x64, 0xD4);
    ps2_wait_in(); outb(0x60, b);
}

void mouse_init_fb(void) {
    mouse_x = SCREEN_W / 2; mouse_y = SCREEN_H / 2;
    mouse_buttons = 0; mouse_cycle = 0;
    ps2_wait_in(); outb(0x64, 0xA8);
    ps2_wait_in(); outb(0x64, 0x20);
    ps2_wait_in(); uint8_t s = inb(0x60); s |= 0x02; s &= ~0x20;
    ps2_wait_in(); outb(0x64, 0x60);
    ps2_wait_in(); outb(0x60, s);
    ps2_wait_in(); outb(0x64, 0xD4);
    ps2_wait_in(); outb(0x60, 0xFF);
    ps2_wait_in(); inb(0x60);
    ps2_wait_in(); outb(0x64, 0xD4);
    ps2_wait_in(); outb(0x60, 0xF4);
    while (inb(0x64) & 0x01) inb(0x60);

    // Intellimouse wheel mode (4-byte packets): sample-rate magic 200/100/80,
    // then GET ID. The device answers ACK (0xFA) then the ID byte — WAIT for
    // each, in order: draining first would eat the ID, misdetect wheel mode
    // while the device already switched to 4-byte packets, and permanently
    // desync the stream (HANDOFF bug #25).
    {
        uint8_t rates[] = { 0xC8, 0x64, 0x50 }; // 200, 100, 80
        for (int i = 0; i < 3; i++) {
            ps2_mouse_cmd(0xF3);
            while (inb(0x64) & 0x01) inb(0x60);          // ack
            ps2_mouse_cmd(rates[i]);
            while (inb(0x64) & 0x01) inb(0x60);          // ack
        }
        ps2_mouse_cmd(0xF2);                              // get device ID
        (void)ps2_read_wait();                            // ack
        int id = ps2_read_wait();
        if (id == 0x03 || id == 0x04) mouse_has_wheel = 1;
        serial_puts("[mse] wheel detect id=0x");
        serial_putchar("0123456789ABCDEF"[(id >> 4) & 0xF]);
        serial_putchar("0123456789ABCDEF"[id & 0xF]);
        serial_puts(mouse_has_wheel ? " 4-byte mode ON\n" : " 3-byte mode\n");
    }
    // The magic sequence left the rate at 80 Hz: back to 200 Hz reports
    // (smoother tracking). ACKs read in order, same rule as above.
    ps2_mouse_cmd(0xF3); (void)ps2_read_wait();
    ps2_mouse_cmd(0xC8); (void)ps2_read_wait();

    // Re-enable data reporting (sample-rate commands may have paused it).
    ps2_mouse_cmd(0xF4);
    while (inb(0x64) & 0x01) inb(0x60);

    build_sprite();
    irq_register_handler(12, mouse_irq_handler);
}
