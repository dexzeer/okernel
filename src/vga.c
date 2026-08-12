#include "vga.h"
#include "io.h"

static uint16_t* vga_buffer = (uint16_t*)0xB8000;
static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t current_color = 0;

// Update the hardware cursor position
static void update_cursor(void) {
    uint16_t pos = cursor_y * VGA_WIDTH + cursor_x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

// Scroll the screen up by one line
static void scroll(void) {
    if (cursor_y >= VGA_HEIGHT) {
        // Move everything up one line
        for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
            vga_buffer[i] = vga_buffer[i + VGA_WIDTH];
        }
        // Clear the last line
        for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            vga_buffer[i] = 0x0F00; // space with default color
        }
        cursor_y = VGA_HEIGHT - 1;
    }
}

void vga_init(void) {
    vga_buffer = (uint16_t*)0xB8000;
    current_color = vga_entry_color(VGA_WHITE, VGA_BLACK);
    vga_clear();
}

void vga_putchar(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga_buffer[cursor_y * VGA_WIDTH + cursor_x] = vga_entry_color(VGA_WHITE, VGA_BLACK) << 8 | ' ';
        }
    } else if (c == '\t') {
        // Advance to next 8-column tab stop
        cursor_x = (cursor_x + 8) & ~7;
    } else {
        vga_buffer[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)current_color << 8 | (uint8_t)c;
        cursor_x++;
    }

    // Wrap to next line if needed
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }

    scroll();
    update_cursor();
}

void vga_puts(const char* str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = (uint16_t)current_color << 8 | ' ';
    }
    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

void vga_set_cursor(int x, int y) {
    cursor_x = x;
    cursor_y = y;
    update_cursor();
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    current_color = vga_entry_color(fg, bg);
}

uint16_t* vga_get_buffer(void) {
    return vga_buffer;
}

void vga_set_buffer(uint16_t* buffer) {
    vga_buffer = buffer;
}
