#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

#define TERM_MAX 8
#define TERM_WIDTH 80
#define TERM_HEIGHT 25
#define CONTENT_ROWS 24
#define SCROLLBACK_SIZE 200  // Lines of scrollback history

// A terminal screen buffer
struct terminal {
    uint16_t buffer[TERM_WIDTH * CONTENT_ROWS]; // Current visible content
    int cursor_x;
    int cursor_y;
    int id;
    int active;

    // Scrollback history (ring buffer)
    uint16_t scrollback[SCROLLBACK_SIZE * TERM_WIDTH];
    int scrollback_head;    // Next write position
    int scrollback_lines;   // How many lines are stored
    int scroll_offset;      // How far up we've scrolled (0 = at bottom)
};

// Initialize the terminal system
void terminal_init(void);

// Create a new terminal
int terminal_create(void);

// Destroy a terminal
void terminal_destroy(int id);

// Switch to a terminal
void terminal_switch(int id);

int terminal_get_active(void);
int terminal_get_count(void);

// Print to active terminal
void terminal_putchar(char c);
void terminal_puts(const char* str);
void terminal_clear(void);
void terminal_set_color(uint8_t fg, uint8_t bg);
void terminal_set_cursor(int x, int y);
void terminal_flush(void);

// Scroll: positive = down, negative = up
void terminal_scroll(int lines);

// Check if scrolled back (non-zero offset)
int terminal_is_scrolled(void);

// Return to bottom of scroll
void terminal_scroll_bottom(void);

int terminal_is_active(int id);

#endif
