#ifndef VGA_H
#define VGA_H

#include <stdint.h>

// FBTEXT build (text kernel booted by UEFI, no VGA text memory): the cells live in RAM and fbtext.c draws them
#ifdef FBTEXT
extern uint16_t fbtext_mem[80 * 25];
void fbtext_init(uint32_t mboot);
void fbtext_set_cursor(int x, int y);
void fbtext_present(void);
#define VGA_TEXT_BASE (fbtext_mem)
#else
#define VGA_TEXT_BASE ((uint16_t*)0xB8000)
#endif

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

// VGA text mode colors
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15,
};

// Create a color byte from foreground and background
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

// Initialize VGA driver
void vga_init(void);

// Print a character at current cursor position
void vga_putchar(char c);

// Print a string
void vga_puts(const char* str);

// Clear the screen
void vga_clear(void);

// Set cursor position
void vga_set_cursor(int x, int y);

// Set current text color
void vga_set_color(uint8_t fg, uint8_t bg);

// Get/set the VGA buffer pointer (for terminal multiplexing)
uint16_t* vga_get_buffer(void);
void vga_set_buffer(uint16_t* buffer);

#endif
