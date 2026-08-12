#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 480

// Initialize graphics with multiboot info for framebuffer
void graphics_init(uint32_t mboot_addr);

// Set a pixel at (x, y) to color
void putpixel(int x, int y, uint8_t color);

// Draw a filled rectangle
void rect_fill(int x, int y, int w, int h, uint8_t color);

// Draw a rectangle outline
void rect_outline(int x, int y, int w, int h, uint8_t color, int thickness);

// Draw a horizontal line
void hline(int x, int y, int len, uint8_t color);

// Draw a vertical line
void vline(int x, int y, int len, uint8_t color);

// Draw a line (Bresenham)
void line(int x0, int y0, int x1, int y1, uint8_t color);

// Draw a character bitmap (8x8 font)
void draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);

// Draw a string
void draw_string(int x, int y, const char* str, uint8_t fg, uint8_t bg);

// Get screen buffer pointer
uint8_t* graphics_get_buffer(void);

// Fill entire screen with a color
void graphics_fill(uint8_t color);

// Flush buffer to VGA memory
void graphics_flush(void);

// Set background color for text drawing
void graphics_set_bg(uint8_t bg);

#endif
