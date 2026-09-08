#include "editor.h"
#include "window.h"
#include "graphics.h"
#include "filesystem.h"
#include "memory.h"
#include "serial.h"
#include <stdint.h>

#define MAX_EDITORS 4
static struct editor editors[MAX_EDITORS];
static int editor_count = 0;

void editor_init(void) {
    for (int i = 0; i < MAX_EDITORS; i++) {
        editors[i].win_id = -1;
        editors[i].text_len = 0;
        editors[i].text[0] = 0;
    }
    editor_count = 0;
}

static int count_lines(const char* text, int len) {
    int lines = 1;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') lines++;
    }
    return lines;
}

static int find_line_start(const char* text, int len, int line) {
    int current_line = 0;
    int pos = 0;
    if (line == 0) return 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') {
            current_line++;
            if (current_line == line) return i + 1;
        }
    }
    return pos;
}

static int line_length(const char* text, int len, int pos) {
    int count = 0;
    for (int i = pos; i < len && text[i] != '\n'; i++) {
        count++;
    }
    return count;
}

int editor_open(const char* filename) {
    if (editor_count >= MAX_EDITORS) return -1;

    int ed = editor_count;
    editors[ed].win_id = -1;
    editors[ed].text_len = 0;
    editors[ed].cursor_pos = 0;
    editors[ed].scroll_y = 0;
    editors[ed].dirty = 1;
    editors[ed].modified = 0;

    // Copy filename
    int fi = 0;
    while (filename[fi] && fi < 31) {
        editors[ed].filename[fi] = filename[fi];
        fi++;
    }
    editors[ed].filename[fi] = 0;

    // Load file if it exists
    if (fs_exists(filename)) {
        editors[ed].text_len = fs_read(filename, (uint8_t*)editors[ed].text, EDITOR_MAX_TEXT - 1);
        editors[ed].text[editors[ed].text_len] = 0;
    }

    // Create window
    int win_w = 480;
    int win_h = 360;
    int x = 40 + (editor_count % 3) * 30;
    int y = 40 + (editor_count % 3) * 25;

    int win = window_create(filename, x, y, win_w, win_h);
    if (win < 0) return -1;

    window_set_close_button(win, 1);
    window_set_minimize_button(win, 1);
    editors[ed].win_id = win;
    editor_count++;
    window_set_focus(win); // take focus so typing goes into the editor

    // Draw status bar
    editor_draw(ed);

    serial_puts("[editor] opened: ");
    serial_puts(filename);
    serial_putchar('\n');

    return ed;
}

void editor_close(int ed_id) {
    if (ed_id < 0 || ed_id >= MAX_EDITORS) return;
    if (editors[ed_id].win_id >= 0) {
        window_destroy(editors[ed_id].win_id);
        editors[ed_id].win_id = -1;
    }
}

void editor_handle_key(int ed_id, char c) {
    if (ed_id < 0 || ed_id >= MAX_EDITORS) return;
    struct editor* ed = &editors[ed_id];

    // Ctrl+S (0x13) = save to VFS (write-through to disk via pfs hook).
    // Ctrl+X (0x18) = save + close. The status bar advertises both; until
    // now neither was wired (edits died with the window).
    if (c == '\x13' || c == '\x18') {
        fs_write(ed->filename, (uint8_t*)ed->text, ed->text_len);
        ed->modified = 0;
        ed->dirty = 1;
        serial_puts("[editor] saved: ");
        serial_puts(ed->filename);
        serial_putchar('\n');
        if (c == '\x18') {
            if (ed->win_id >= 0) {
                window_destroy(ed->win_id);
                ed->win_id = -1;
            }
        }
        return;
    }

    if (c == '\b') {
        // Backspace
        if (ed->cursor_pos > 0) {
            // Remove character before cursor
            for (int i = ed->cursor_pos - 1; i < ed->text_len - 1; i++) {
                ed->text[i] = ed->text[i + 1];
            }
            ed->cursor_pos--;
            ed->text_len--;
            ed->text[ed->text_len] = 0;
            ed->modified = 1;
            ed->dirty = 1;
        }
    } else if (c == '\n') {
        // Enter — insert newline
        if (ed->text_len < EDITOR_MAX_TEXT - 1) {
            for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                ed->text[i] = ed->text[i - 1];
            }
            ed->text[ed->cursor_pos] = '\n';
            ed->cursor_pos++;
            ed->text_len++;
            ed->text[ed->text_len] = 0;
            ed->modified = 1;
            ed->dirty = 1;
        }
    } else if (c == '\t') {
        // Tab — insert 4 spaces
        for (int s = 0; s < 4 && ed->text_len < EDITOR_MAX_TEXT - 1; s++) {
            for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                ed->text[i] = ed->text[i - 1];
            }
            ed->text[ed->cursor_pos] = ' ';
            ed->cursor_pos++;
            ed->text_len++;
        }
        ed->text[ed->text_len] = 0;
        ed->modified = 1;
        ed->dirty = 1;
    } else if (c >= 32 && c < 127) {
        // Printable character
        if (ed->text_len < EDITOR_MAX_TEXT - 1) {
            for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                ed->text[i] = ed->text[i - 1];
            }
            ed->text[ed->cursor_pos] = c;
            ed->cursor_pos++;
            ed->text_len++;
            ed->text[ed->text_len] = 0;
            ed->modified = 1;
            ed->dirty = 1;
        }
    }

    // Auto-scroll to keep cursor visible
    if (ed->win_id >= 0) {
        struct window* w = window_get(ed->win_id);
        if (w) {
            // Calculate current line
            int cur_line = 0;
            for (int i = 0; i < ed->cursor_pos; i++) {
                if (ed->text[i] == '\n') cur_line++;
            }

            // Content area height minus 1 for status bar
            int view_h = w->content_h - 1;
            if (cur_line < ed->scroll_y) {
                ed->scroll_y = cur_line;
            } else if (cur_line >= ed->scroll_y + view_h) {
                ed->scroll_y = cur_line - view_h + 1;
            }
            ed->dirty = 1;
        }
    }
}

void editor_draw(int ed_id) {
    if (ed_id < 0 || ed_id >= MAX_EDITORS) return;
    struct editor* ed = &editors[ed_id];
    if (ed->win_id < 0) return;

    struct window* w = window_get(ed->win_id);
    if (!w) return;

    // Clear the window content
    window_clear(ed->win_id);

    // Draw status bar at top
    window_set_text_color(ed->win_id, 0, 7); // Black on grey
    window_puts(ed->win_id, " ");
    window_puts(ed->win_id, ed->filename);
    if (ed->modified) {
        window_puts(ed->win_id, " *");
    }
    window_puts(ed->win_id, "  Ctrl+S:Save  Ctrl+X:Close");
    // Fill rest of status bar
    int status_len = 3 + 1 + 1 + 30 + 1; // approximate
    for (int i = status_len; i < w->content_w; i++) {
        window_put_char(ed->win_id, ' ');
    }
    window_set_text_color(ed->win_id, 15, 0); // White on black

    // Draw text content
    int view_h = w->content_h - 1; // Reserve 1 line for status bar
    int view_w = w->content_w;

    int line = 0;
    int pos = 0;

    // Skip to scroll_y lines
    while (line < ed->scroll_y && pos < ed->text_len) {
        if (ed->text[pos] == '\n') line++;
        pos++;
    }

    // Draw visible lines
    for (int row = 0; row < view_h && pos <= ed->text_len; row++) {
        int line_start = pos;
        int llen = line_length(ed->text, ed->text_len, pos);

        // Draw line content
        for (int col = 0; col < view_w && col < llen; col++) {
            window_put_char(ed->win_id, ed->text[line_start + col]);
        }

        // If this line contains the cursor, draw cursor indicator
        int cursor_line = 0;
        for (int i = 0; i < ed->cursor_pos; i++) {
            if (ed->text[i] == '\n') cursor_line++;
        }
        if (line == cursor_line && ed->win_id == window_get_focused()) {
            int cursor_col = ed->cursor_pos - line_start;
            if (cursor_col >= 0 && cursor_col < view_w) {
                w->cursor_x = cursor_col;
                w->cursor_y = row + 1; // +1 for status bar
            }
        }

        // Move to next line
        pos = line_start + llen;
        if (pos < ed->text_len && ed->text[pos] == '\n') pos++;
        line++;
    }

    // Draw line count at bottom-right
    int total_lines = count_lines(ed->text, ed->text_len);
    char lc_buf[16];
    int lc_idx = 0;
    int tmp = total_lines;
    if (tmp == 0) { lc_buf[lc_idx++] = '0'; }
    else {
        char rev[8]; int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        while (ri > 0) lc_buf[lc_idx++] = rev[--ri];
    }
    lc_buf[lc_idx] = 0;

    w->dirty = 1;
}

int editor_get_count(void) {
    return editor_count;
}

struct editor* editor_get(int ed_id) {
    if (ed_id < 0 || ed_id >= MAX_EDITORS) return 0;
    if (editors[ed_id].win_id < 0) return 0;
    return &editors[ed_id];
}

int editor_find_by_win(int win_id) {
    for (int i = 0; i < MAX_EDITORS; i++) {
        if (editors[i].win_id == win_id) return i;
    }
    return -1;
}
