#include "terminal.h"
#include "vga.h"
#include "io.h"
#include <stdint.h>

static uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;

#define STATUS_ROW 0
#define CONTENT_START 1

static struct terminal terms[TERM_MAX];
static int active_term = 0;
static int term_count = 0;
static uint8_t current_fg = 0x0F;
static uint8_t current_bg = 0x00;

#define STATUS_FG VGA_WHITE
#define STATUS_BG VGA_BLUE

static uint8_t make_color(uint8_t fg, uint8_t bg) {
    return fg | (bg << 4);
}

static uint16_t make_entry(char c, uint8_t color) {
    return (uint16_t)color << 8 | (uint16_t)(uint8_t)c;
}

static void update_hw_cursor(int x, int y) {
    uint16_t pos = y * TERM_WIDTH + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void cursor_enable(uint8_t cursor_start, uint8_t cursor_end) {
    outb(0x3D4, 0x0A);
    uint8_t cur = (inb(0x3D5) & 0xC0) | cursor_start;
    outb(0x3D5, cur);
    outb(0x3D4, 0x0B);
    cur = (inb(0x3D5) & 0xE0) | cursor_end;
    outb(0x3D5, cur);
}

static void push_scrollback(struct terminal* t) {
    // Save top line of current buffer into scrollback
    int slot = t->scrollback_head;
    for (int i = 0; i < TERM_WIDTH; i++) {
        t->scrollback[slot * TERM_WIDTH + i] = t->buffer[i];
    }
    t->scrollback_head = (slot + 1) % SCROLLBACK_SIZE;
    if (t->scrollback_lines < SCROLLBACK_SIZE) {
        t->scrollback_lines++;
    }
}

static void term_scroll(struct terminal* t) {
    if (t->cursor_y >= CONTENT_ROWS) {
        push_scrollback(t);

        for (int i = 0; i < (CONTENT_ROWS - 1) * TERM_WIDTH; i++) {
            t->buffer[i] = t->buffer[i + TERM_WIDTH];
        }
        uint8_t def_color = make_color(current_fg, current_bg);
        for (int i = (CONTENT_ROWS - 1) * TERM_WIDTH; i < CONTENT_ROWS * TERM_WIDTH; i++) {
            t->buffer[i] = make_entry(' ', def_color);
        }
        t->cursor_y = CONTENT_ROWS - 1;
    }
}

static void draw_status_bar(void) {
    uint8_t color = make_color(STATUS_FG, STATUS_BG);

    for (int i = 0; i < TERM_WIDTH; i++) {
        VGA_BUFFER[STATUS_ROW * TERM_WIDTH + i] = make_entry(' ', color);
    }

    // Left: "okernel"
    const char* title = " okernel ";
    int pos = 0;
    while (*title && pos < TERM_WIDTH) {
        VGA_BUFFER[STATUS_ROW * TERM_WIDTH + pos] = make_entry(*title, color);
        pos++;
        title++;
    }

    // Center: "Terminal N"
    char mid[] = "Terminal N";
    mid[9] = '0' + active_term;
    int mid_start = (TERM_WIDTH - 10) / 2;
    for (int i = 0; i < 10 && mid_start + i < TERM_WIDTH; i++) {
        VGA_BUFFER[STATUS_ROW * TERM_WIDTH + mid_start + i] = make_entry(mid[i], color);
    }

    // Right: scroll indicator or count
    struct terminal* t = &terms[active_term];
    const char* right;
    int right_len;
    char count_buf[8];

    if (t->scroll_offset > 0) {
        right = " SCROLLED UP (wheel down to return) ";
        right_len = 0;
        while (right[right_len]) right_len++;
    } else {
        count_buf[0] = '0' + term_count;
        count_buf[1] = '/';
        count_buf[2] = '0' + TERM_MAX;
        count_buf[3] = ' ';
        count_buf[4] = 0;
        right = count_buf;
        right_len = 4;
    }

    int right_start = TERM_WIDTH - right_len;
    for (int i = 0; i < right_len && right_start + i < TERM_WIDTH; i++) {
        VGA_BUFFER[STATUS_ROW * TERM_WIDTH + right_start + i] = make_entry(right[i], color);
    }
}

// Get a line from scrollback history (0 = oldest, scrollback_lines-1 = newest)
static void get_scrollback_line(struct terminal* t, int line_index, uint16_t* dest) {
    // line_index 0 = oldest line, scrollback_lines-1 = newest line
    // In the ring buffer, newest line is at (scrollback_head - 1), oldest at (scrollback_head - scrollback_lines)
    int ring_pos = (t->scrollback_head - t->scrollback_lines + line_index + SCROLLBACK_SIZE * 2) % SCROLLBACK_SIZE;
    for (int i = 0; i < TERM_WIDTH; i++) {
        dest[i] = t->scrollback[ring_pos * TERM_WIDTH + i];
    }
}

void terminal_init(void) {
    for (int i = 0; i < TERM_MAX; i++) {
        terms[i].active = 0;
        terms[i].id = -1;
        terms[i].cursor_x = 0;
        terms[i].cursor_y = 0;
        terms[i].scrollback_head = 0;
        terms[i].scrollback_lines = 0;
        terms[i].scroll_offset = 0;
        uint8_t def = make_color(current_fg, current_bg);
        for (int j = 0; j < TERM_WIDTH * CONTENT_ROWS; j++) {
            terms[i].buffer[j] = make_entry(' ', def);
        }
    }

    terms[0].active = 1;
    terms[0].id = 0;
    active_term = 0;
    term_count = 1;

    for (int i = 0; i < TERM_WIDTH * TERM_HEIGHT; i++) {
        VGA_BUFFER[i] = make_entry(' ', make_color(current_fg, current_bg));
    }

    cursor_enable(13, 15);
    terminal_flush();
}

int terminal_create(void) {
    for (int i = 0; i < TERM_MAX; i++) {
        if (!terms[i].active) {
            terms[i].active = 1;
            terms[i].id = i;
            terms[i].cursor_x = 0;
            terms[i].cursor_y = 0;
            terms[i].scrollback_head = 0;
            terms[i].scrollback_lines = 0;
            terms[i].scroll_offset = 0;
            uint8_t def = make_color(current_fg, current_bg);
            for (int j = 0; j < TERM_WIDTH * CONTENT_ROWS; j++) {
                terms[i].buffer[j] = make_entry(' ', def);
            }
            term_count++;
            return i;
        }
    }
    return -1;
}

void terminal_destroy(int id) {
    if (id <= 0 || id >= TERM_MAX || !terms[id].active) return;
    terms[id].active = 0;
    terms[id].id = -1;
    term_count--;
    if (active_term == id) terminal_switch(0);
}

void terminal_switch(int id) {
    if (id < 0 || id >= TERM_MAX || !terms[id].active) return;
    active_term = id;
    terminal_flush();
}

int terminal_get_active(void) { return active_term; }
int terminal_get_count(void) { return term_count; }

void terminal_putchar(char c) {
    struct terminal* t = &terms[active_term];

    // Jump back to bottom on new output
    if (t->scroll_offset != 0) {
        t->scroll_offset = 0;
    }

    uint8_t color = make_color(current_fg, current_bg);

    if (c == '\n') {
        t->cursor_x = 0;
        t->cursor_y++;
    } else if (c == '\r') {
        t->cursor_x = 0;
    } else if (c == '\b') {
        if (t->cursor_x > 0) {
            t->cursor_x--;
            t->buffer[t->cursor_y * TERM_WIDTH + t->cursor_x] = make_entry(' ', color);
        }
    } else if (c == '\t') {
        t->cursor_x = (t->cursor_x + 8) & ~7;
    } else {
        t->buffer[t->cursor_y * TERM_WIDTH + t->cursor_x] = make_entry(c, color);
        t->cursor_x++;
    }

    if (t->cursor_x >= TERM_WIDTH) {
        t->cursor_x = 0;
        t->cursor_y++;
    }

    term_scroll(t);
    terminal_flush();
}

void terminal_puts(const char* str) {
    while (*str) terminal_putchar(*str++);
}

void terminal_clear(void) {
    struct terminal* t = &terms[active_term];
    uint8_t def = make_color(current_fg, current_bg);
    for (int i = 0; i < TERM_WIDTH * CONTENT_ROWS; i++) {
        t->buffer[i] = make_entry(' ', def);
    }
    t->cursor_x = 0;
    t->cursor_y = 0;
    t->scroll_offset = 0;
    terminal_flush();
}

void terminal_set_color(uint8_t fg, uint8_t bg) {
    current_fg = fg;
    current_bg = bg;
}

void terminal_set_cursor(int x, int y) {
    terms[active_term].cursor_x = x;
    terms[active_term].cursor_y = y;
}

void terminal_scroll(int lines) {
    struct terminal* t = &terms[active_term];

    int max_scroll = t->scrollback_lines;
    if (max_scroll > CONTENT_ROWS) max_scroll = CONTENT_ROWS;

    // lines > 0 = toward history (scroll up), lines < 0 = toward bottom (scroll down)
    t->scroll_offset += lines;
    if (t->scroll_offset < 0) t->scroll_offset = 0;
    if (t->scroll_offset > max_scroll) t->scroll_offset = max_scroll;

    terminal_flush();
}

int terminal_is_scrolled(void) {
    return terms[active_term].scroll_offset != 0;
}

void terminal_scroll_bottom(void) {
    terms[active_term].scroll_offset = 0;
    terminal_flush();
}

void terminal_flush(void) {
    draw_status_bar();

    struct terminal* t = &terms[active_term];

    if (t->scroll_offset > 0) {
        for (int row = 0; row < CONTENT_ROWS; row++) {
            int line_idx = t->scrollback_lines - t->scroll_offset + row;

            if (line_idx >= 0 && line_idx < t->scrollback_lines) {
                // Scrollback line
                uint16_t line_buf[TERM_WIDTH];
                get_scrollback_line(t, line_idx, line_buf);
                for (int col = 0; col < TERM_WIDTH; col++) {
                    VGA_BUFFER[(CONTENT_START + row) * TERM_WIDTH + col] = line_buf[col];
                }
            } else if (line_idx >= t->scrollback_lines) {
                // Below scrollback — show current buffer
                int buf_row = line_idx - t->scrollback_lines;
                for (int col = 0; col < TERM_WIDTH; col++) {
                    VGA_BUFFER[(CONTENT_START + row) * TERM_WIDTH + col] =
                        t->buffer[buf_row * TERM_WIDTH + col];
                }
            } else {
                // Above scrollback — blank
                uint8_t def_color = make_color(current_fg, current_bg);
                for (int col = 0; col < TERM_WIDTH; col++) {
                    VGA_BUFFER[(CONTENT_START + row) * TERM_WIDTH + col] =
                        make_entry(' ', def_color);
                }
            }
        }

        update_hw_cursor(0, 0);
    } else {
        for (int i = 0; i < TERM_WIDTH * CONTENT_ROWS; i++) {
            VGA_BUFFER[(CONTENT_START * TERM_WIDTH) + i] = t->buffer[i];
        }
        update_hw_cursor(t->cursor_x, t->cursor_y + CONTENT_START);
    }
}

int terminal_is_active(int id) {
    if (id < 0 || id >= TERM_MAX) return 0;
    return terms[id].active;
}
