#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

// Font: 8x16 source, FONT_SCALE 2 = 16x32 rendered characters
#define FONT_W 8
#define FONT_H 16
#define FONT_SCALE 2
#define CHAR_W (FONT_W * FONT_SCALE)
#define CHAR_H (FONT_H * FONT_SCALE)

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
