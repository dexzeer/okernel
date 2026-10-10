// Text screen on a linear framebuffer, for the text-mode kernel when it is booted by UEFI (there is no VGA text memory
// there: GRUB hands over a GOP framebuffer). The terminal keeps drawing into an 80x25 array of VGA cells (fbtext_mem);
// fbtext_present() turns the cells that changed into pixels. The glyphs are the desktop's Terminus 16x32 bitmaps, box-
// filtered down to whatever cell size fits the screen (16x30 on 1366x768). Without a usable framebuffer (a real VGA
// text mode, or one above 4GB, which this unpaged kernel cannot reach) the cells are copied to 0xB8000 instead.
#include <stdint.h>
#include "vga.h"
#include "io.h"
#include "serial.h"

extern const uint8_t font8x16[95][64];   // font_data.c: ASCII 32..126, 16x32, 2 bytes per row

#define COLS 80
#define ROWS 25

uint16_t fbtext_mem[COLS * ROWS];

static uint32_t shadow[COLS * ROWS];     // what is on screen: cell | (cursor << 16) | 0x80000000 once drawn
static int mode;                         // 0 = copy to the VGA text buffer, 1 = 24/32-bit framebuffer
static volatile uint8_t* fb;
static uint32_t fb_pitch, fb_w, fb_h, ox, oy;
static int cw, ch, bytes_pp;
static uint8_t rpos, gpos, bpos, rsz, gsz, bsz;
static int cursor_pos = 0;
static int in_present, again;

static const uint32_t pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static uint32_t pack(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - rsz)) << rpos) | ((g >> (8 - gsz)) << gpos) | ((b >> (8 - bsz)) << bpos);
}

// is the glyph pixel (x, y) of 16x32 set
static inline int gl(const uint8_t* g, int x, int y) {
    return (g[y * 2 + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static inline void put(volatile uint8_t* p, uint32_t v) {
    if (bytes_pp == 4) *(volatile uint32_t*)p = v;
    else { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }
}

static void draw_cell(int idx, uint16_t cell, int cur) {
    int c = cell & 0xFF;
    const uint8_t* g = (c >= 32 && c <= 126) ? font8x16[c - 32] : (c >= 0x80 ? font8x16['?' - 32] : font8x16[0]);
    uint32_t fg = pack(pal[(cell >> 8) & 15]), bg = pack(pal[(cell >> 12) & 7]);
    uint32_t x0 = ox + (uint32_t)(idx % COLS) * cw, y0 = oy + (uint32_t)(idx / COLS) * ch;
    for (int dy = 0; dy < ch; dy++) {
        int sy0 = dy * 32 / ch, sy1 = ((dy + 1) * 32 + ch - 1) / ch;
        volatile uint8_t* row = fb + (y0 + dy) * fb_pitch + x0 * bytes_pp;
        for (int dx = 0; dx < cw; dx++) {
            int sx0 = dx * 16 / cw, sx1 = ((dx + 1) * 16 + cw - 1) / cw;
            int on = 0, tot = 0;
            for (int sy = sy0; sy < sy1 && sy < 32; sy++)
                for (int sx = sx0; sx < sx1 && sx < 16; sx++) { tot++; on += gl(g, sx, sy); }
            int lit = tot && on * 2 >= tot;
            if (cur && dy >= ch - (ch >= 16 ? 3 : 2)) lit = 1;          // underline cursor
            put(row + dx * bytes_pp, lit ? fg : bg);
        }
    }
}

void fbtext_init(uint32_t mboot) {
    for (int i = 0; i < COLS * ROWS; i++) { fbtext_mem[i] = 0x0F20; shadow[i] = 0; }
    mode = 0;
    uint32_t flags = *(volatile uint32_t*)(mboot + 0);
    if (flags & (1u << 12)) {
        uint32_t lo = *(volatile uint32_t*)(mboot + 88), hi = *(volatile uint32_t*)(mboot + 92);
        uint32_t pitch = *(volatile uint32_t*)(mboot + 96), w = *(volatile uint32_t*)(mboot + 100);
        uint32_t h = *(volatile uint32_t*)(mboot + 104);
        uint8_t bpp = *(volatile uint8_t*)(mboot + 108), type = *(volatile uint8_t*)(mboot + 109);
        serial_printf("[fbtext] fb=%x:%x %dx%d pitch=%d bpp=%d type=%d\n", hi, lo, (int)w, (int)h, (int)pitch, bpp, type);
        if (type == 1 && (bpp == 32 || bpp == 24) && hi == 0 && w >= 640 && h >= 200 && pitch >= w * (bpp / 8u)) {
            rpos = *(volatile uint8_t*)(mboot + 110); rsz = *(volatile uint8_t*)(mboot + 111);
            gpos = *(volatile uint8_t*)(mboot + 112); gsz = *(volatile uint8_t*)(mboot + 113);
            bpos = *(volatile uint8_t*)(mboot + 114); bsz = *(volatile uint8_t*)(mboot + 115);
            if (rsz == 0 || rsz > 8 || gsz == 0 || gsz > 8 || bsz == 0 || bsz > 8 || rpos > 24 || gpos > 24 || bpos > 24) {
                rpos = 16; rsz = 8; gpos = 8; gsz = 8; bpos = 0; bsz = 8;
            }
            bytes_pp = bpp / 8;
            fb = (volatile uint8_t*)lo; fb_pitch = pitch; fb_w = w; fb_h = h;
            cw = (int)(w / COLS); if (cw > 16) cw = 16;
            ch = (int)(h / ROWS); if (ch > 32) ch = 32;
            ox = (w - (uint32_t)cw * COLS) / 2; oy = (h - (uint32_t)ch * ROWS) / 2;
            for (uint32_t y = 0; y < h; y++) {
                volatile uint8_t* r = fb + y * pitch;
                for (uint32_t x = 0; x < w; x++) put(r + x * bytes_pp, 0);
            }
            mode = 1;
            serial_printf("[fbtext] cell %dx%d at %d,%d\n", cw, ch, (int)ox, (int)oy);
        }
    }
    if (!mode) serial_printf("[fbtext] no usable framebuffer: using VGA text memory\n");
}

void fbtext_set_cursor(int x, int y) {
    cursor_pos = y * COLS + x;
    fbtext_present();
}

void fbtext_present(void) {
    if (in_present) { again = 1; return; }          // called from an IRQ while the main code was drawing
    in_present = 1;
    do {
        again = 0;
        if (mode == 0) {
            volatile uint16_t* t = (volatile uint16_t*)0xB8000;
            for (int i = 0; i < COLS * ROWS; i++) t[i] = fbtext_mem[i];
            outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(cursor_pos & 0xFF));
            outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((cursor_pos >> 8) & 0xFF));
        } else {
            for (int i = 0; i < COLS * ROWS; i++) {
                uint16_t v = fbtext_mem[i];
                int cur = i == cursor_pos;
                uint32_t key = 0x80000000u | v | ((uint32_t)cur << 16);
                if (shadow[i] != key) { draw_cell(i, v, cur); shadow[i] = key; }
            }
        }
    } while (again);
    in_present = 0;
}
