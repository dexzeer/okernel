#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

// Main font: Terminus 16x32 (tools/bdf2font.py from terminus-font ter-u32n.bdf),
// rendered 1:1 (FONT_SCALE 1) so glyphs are crisp, not upscaled/pixelated.
// Width exceeds 8 here (2 bytes per row); FONT_BPR = bytes per glyph row.
// Swap sizes by regenerating src/font_data.c and updating FONT_W/FONT_H here.
#define FONT_W 16
#define FONT_H 32
#define FONT_SCALE 1
#define FONT_BPR ((FONT_W + 7) / 8)
#define CHAR_W (FONT_W * FONT_SCALE)
#define CHAR_H (FONT_H * FONT_SCALE)

// Font bitmap data lives in src/font_data.c (generated). Keep the second
// dimension in sync with FONT_H * FONT_BPR when you change the font size.
extern const uint8_t font8x16[95][FONT_H * FONT_BPR];

// Extended font (Cyrillic + symbols, slots >= 0x80 from the HTML decoder)
// keeps its own size so it can stay 8x16 while the main font is upgraded.
#define FONT_EXT_W 8
#define FONT_EXT_H 16
#define FONT_EXT_BPR ((FONT_EXT_W + 7) / 8)

// Content text (terminal + webpage body) renders at 3/4 of the base glyph
// size: 12x24 px instead of 16x32 — ~25% smaller so more text fits. OS and
// browser chrome keep the full 16x32 via draw_char / draw_string_fg.
#define CONTENT_GW (FONT_W * FONT_SCALE * 3 / 4)
#define CONTENT_GH (FONT_H * FONT_SCALE * 3 / 4)

// Initialize graphics with multiboot info for framebuffer
void graphics_init(uint32_t mboot_addr);

// Set a pixel at (x, y) to color
void putpixel(int x, int y, uint32_t color);

// Draw a filled rectangle
void rect_fill(int x, int y, int w, int h, uint32_t color);

// Draw a rectangle outline
void rect_outline(int x, int y, int w, int h, uint32_t color, int thickness);

// Linear gradient fill between c0 and c1 (vertical: top->bottom, else left->right)
void gradient_fill(int x, int y, int w, int h, uint32_t c0, uint32_t c1, int vertical);

// Draw a horizontal line
void hline(int x, int y, int len, uint32_t color);

// Draw a vertical line
void vline(int x, int y, int len, uint32_t color);

// Draw a line (Bresenham)
void line(int x0, int y0, int x1, int y1, uint32_t color);

// Draw a character (8x16 font, scaled 2x = 16x16 pixels)
void draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);

// Draw a character into an exact tw x th box (nearest-neighbor); used by
// content text at CONTENT_GW x CONTENT_GH (12x24).
void draw_char_sized(int x, int y, char c, uint32_t fg, uint32_t bg, int tw, int th);

// Draw a character with extra scaling
void draw_char_scaled(int x, int y, char c, uint32_t fg, uint32_t bg, int scale);

// Draw a string
void draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg);

// Draw a string painting only the glyph pixels (no background fill) —
// for text over gradients, where a flat glyph-cell bg would show as a patch
void draw_string_fg(int x, int y, const char* str, uint32_t fg);

// 1x-size text (8x16 px, no FONT_SCALE doubling) for tight chrome areas
void draw_char_1x(int x, int y, char c, uint32_t fg, uint32_t bg);
void draw_string_1x(int x, int y, const char* str, uint32_t fg, uint32_t bg);

// Filled rect with rounded corners (corner pixels not drawn — background
// shows through)
void round_rect_fill(int x, int y, int w, int h, uint32_t c, int r);

// Draw a string with extra scaling
void draw_string_scaled(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale);

// Get screen buffer pointer
uint32_t* graphics_get_buffer(void);

// Mark a row as dirty for optimized flushing
void graphics_mark_dirty(int y);

// Clip rectangle
void graphics_set_clip(int x, int y, int w, int h);
void graphics_clip_reset(void);

// Direct backbuffer write
void graphics_write_pixel(int x, int y, uint32_t color);

// Move a rectangle within the backbuffer by (dx, dy) — used by window
// dragging so unchanged window pixels are copied instead of re-rendered
void graphics_blit_rect(int sx, int sy, int w, int h, int dx, int dy);

// Wallpaper
void graphics_cache_wallpaper(const unsigned char* pixels, const unsigned char* palette, int src_w, int src_h);
void graphics_blit_wallpaper(void);
void graphics_blit_wallpaper_rect(int x, int y, int w, int h);

// Fill entire screen
void graphics_fill(uint32_t color);

// Flush backbuffer to VGA framebuffer
void graphics_flush(void);

// Draw a scaled bitmap (for icons)
void graphics_draw_bitmap(int x, int y, const uint8_t* bitmap, int bmp_w, int bmp_h, uint32_t fg, uint32_t bg, int scale);

#endif
