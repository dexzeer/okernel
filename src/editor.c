#include "editor.h"
#include "window.h"
#include "graphics.h"
#include "filesystem.h"
#include "memory.h"
#include "serial.h"
#include "html.h"
#include <stdint.h>

#define MAX_EDITORS 4
static struct editor editors[MAX_EDITORS];
static int editor_count = 0;

void editor_init(void) {
    for (int i = 0; i < MAX_EDITORS; i++) {
        editors[i].win_id = -1;
        editors[i].text_len = 0;
        editors[i].text[0] = 0;
        editors[i].read_only = 0;
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

// Decode one UTF-8 sequence at raw offset p (bounded by end) into a font
// slot byte. ASCII passes through; 2/3-byte sequences map through the
// browser's slot table; malformed bytes (lone continuations, short reads,
// 4-byte sequences with no slot) become '?' consuming 1 byte — the same
// policy as the browser's charset pass, so both agree on every byte.
// The editor buffer stays RAW (byte-exact saves); only the draw path and
// cursor math use display columns computed through here.
static void editor_decode1(const char* text, int p, int end,
                           char* slot, int* seqlen) {
    unsigned char b = (unsigned char)text[p];
    if (b < 0x80) { *slot = (char)b; *seqlen = 1; return; }
    if ((b & 0xE0) == 0xC0 && p + 1 < end &&
        ((unsigned char)text[p+1] & 0xC0) == 0x80) {
        int cp = ((b & 0x1F) << 6) | ((unsigned char)text[p+1] & 0x3F);
        *slot = html_slot_for_codepoint(cp); *seqlen = 2; return;
    }
    if ((b & 0xF0) == 0xE0 && p + 2 < end &&
        ((unsigned char)text[p+1] & 0xC0) == 0x80 &&
        ((unsigned char)text[p+2] & 0xC0) == 0x80) {
        int cp = ((b & 0x0F) << 12) | (((unsigned char)text[p+1] & 0x3F) << 6)
               | ((unsigned char)text[p+2] & 0x3F);
        *slot = html_slot_for_codepoint(cp); *seqlen = 3; return;
    }
    *slot = '?'; *seqlen = 1;
}

int editor_open(const char* filename) {
    // Reuse freed slots: editor_count is a high-water mark, not a live
    // count (close only drops the window). Bump-allocating here leaked all
    // four slots permanently — the 5th open failed forever.
    int ed = -1;
    for (int i = 0; i < MAX_EDITORS; i++)
        if (editors[i].win_id < 0) { ed = i; break; }
    if (ed < 0) return -1;
    editors[ed].win_id = -1;
    editors[ed].text_len = 0;
    editors[ed].cursor_pos = 0;
    editors[ed].scroll_y = 0;
    editors[ed].dirty = 1;
    editors[ed].modified = 0;
    editors[ed].read_only = 0;

    // Copy filename
    int fi = 0;
    while (filename[fi] && fi < 31) {
        editors[ed].filename[fi] = filename[fi];
        fi++;
    }
    editors[ed].filename[fi] = 0;

    // Load file if it exists
    if (fs_exists(filename)) {
        int n = fs_read(filename, (uint8_t*)editors[ed].text, EDITOR_MAX_TEXT - 1);
        if (n < 0) n = 0; // unreadable (shouldn't happen after exists)
        editors[ed].text_len = n;
        editors[ed].text[n] = 0;
        // Binary sniff: NUL bytes never occur in text; a run of C0 controls
        // (outside tab/newline/CR) means binary too. Binaries are TEXTIFIED
        // in place (printable ASCII + newlines kept, everything else '.')
        // and open read-only — no hex dump, no raw control bytes on screen.
        // The original bytes stay on disk (saving is blocked below), so
        // nothing is lost; the buffer is just a view.
        int is_bin = 0;
        int ctrl = 0;
        for (int i = 0; i < n; i++) {
            unsigned char b = (unsigned char)editors[ed].text[i];
            if (b == 0) { is_bin = 1; break; }
            if ((b < 32 && b != '\t' && b != '\n' && b != '\r') || b == 127)
                ctrl++;
        }
        if (!is_bin && n > 0 && ctrl * 20 > n)
            is_bin = 1; // >5% control bytes: binary
        if (is_bin) {
            for (int i = 0; i < n; i++) {
                char c = editors[ed].text[i];
                if (c == '\n') continue;
                if (c == '\t' || c == '\r') editors[ed].text[i] = ' ';
                else if (c < 32 || c > 126) editors[ed].text[i] = '.';
            }
            editors[ed].read_only = 1;
        }
    }

    // Create window
    int win_w = 480;
    int win_h = 360;
    int x = 40 + (ed % 3) * 30;
    int y = 40 + (ed % 3) * 25;

    int win = window_create(filename, x, y, win_w, win_h);
    if (win < 0) return -1;

    window_set_close_button(win, 1);
    window_set_minimize_button(win, 1);
    if (editors[ed].read_only) window_set_hide_cursor(win, 1); // no caret in read-only view
    editors[ed].win_id = win;
    if (ed >= editor_count) editor_count = ed + 1; // high-water mark only
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

    // Read-only (binary textified at open): scrolling only, Ctrl+X closes
    // (never saves — the buffer holds '.' placeholders, writing it back
    // would corrupt the binary). j/k line-scroll, space pages down,
    // b pages up.
    if (ed->read_only) {
        int view_h = 10;
        struct window* hw = ed->win_id >= 0 ? window_get(ed->win_id) : 0;
        if (hw) view_h = hw->content_h - 1;
        int total = count_lines(ed->text, ed->text_len);
        int maxsy = total - view_h;
        if (maxsy < 0) maxsy = 0;
        if (ed->scroll_y > maxsy) ed->scroll_y = maxsy;
        if (c == '\x18') {
            if (ed->win_id >= 0) {
                window_destroy(ed->win_id);
                ed->win_id = -1;
            }
        } else if (c == ' ' || c == 'j') {
            ed->scroll_y += (c == ' ') ? view_h : 1;
            if (ed->scroll_y > maxsy) ed->scroll_y = maxsy;
            ed->dirty = 1;
        } else if (c == 'k' || c == 'b') {
            ed->scroll_y -= (c == 'b') ? view_h : 1;
            if (ed->scroll_y < 0) ed->scroll_y = 0;
            ed->dirty = 1;
        }
        return;
    }

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
        // Backspace: erase a whole UTF-8 sequence, not one byte (step back
        // over continuation bytes, then the lead byte).
        if (ed->cursor_pos > 0) {
            int p = ed->cursor_pos;
            do { p--; } while (p > 0 && ((unsigned char)ed->text[p] & 0xC0) == 0x80);
            int del = ed->cursor_pos - p;
            for (int i = p; i + del < ed->text_len; i++) {
                ed->text[i] = ed->text[i + del];
            }
            ed->cursor_pos = p;
            ed->text_len -= del;
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
    if (ed->read_only) {
        window_puts(ed->win_id, " (read-only)");
    } else if (ed->modified) {
        window_puts(ed->win_id, " *");
    }
    if (ed->read_only) window_puts(ed->win_id, "  Ctrl+X:Close");
    else window_puts(ed->win_id, "  Ctrl+S:Save  Ctrl+X:Close");
    // Fill rest of status bar
    int status_len = 3 + 1 + 1 + 30 + 1; // approximate
    for (int i = status_len; i < w->content_w; i++) {
        window_put_char(ed->win_id, ' ');
    }
    // Park the cursor at a fresh row: the status text + fill above can end
    // mid-row (long filenames), and content drawn there would glue onto the
    // status line ("fi" + missing rest). A conditional break keeps content
    // rows aligned without adding blanks when already home.
    if (w->cursor_x != 0) window_put_char(ed->win_id, '\n');
    window_set_text_color(ed->win_id, 15, 0); // White on black

    // Draw text content (binaries were textified at open, so the same path
    // renders both: editable text and read-only views alike — no hex).
    int view_h = w->content_h - 1; // Reserve 1 line for status bar
    int view_w = w->content_w;

    int line = 0;
    int pos = 0;

    // Skip to scroll_y lines
    while (line < ed->scroll_y && pos < ed->text_len) {
        if (ed->text[pos] == '\n') line++;
        pos++;
    }

    // Draw visible lines (decoded for display; text-mode ed->text stays raw
    // so saves are byte-exact — identical bytes to ASCII input; read-only
    // buffers were textified at open and are never saved back).
    // Caret tracking: the grid position is whatever put_char left behind
    // (status wraps, row wraps), so the caret is recorded from the live
    // cursor wherever the cursor's raw offset is emitted — never via a
    // row+1 formula (which rewound the shared cursor and made the next
    // row overwrite the current one). Emission continuity is preserved:
    // w->cursor is only assigned once, after the loop.
    int caret_x = -1, caret_y = -1;
    int want_caret = (ed->win_id == window_get_focused()) && !ed->read_only;
    for (int row = 0; row < view_h && pos <= ed->text_len; row++) {
        int line_start = pos;
        int line_end = pos;
        while (line_end < ed->text_len && ed->text[line_end] != '\n') line_end++;

        // Draw line content
        int dcol = 0, rp = line_start;
        if (want_caret && rp == ed->cursor_pos) {
            caret_x = w->cursor_x; caret_y = w->cursor_y;
        }
        while (rp < line_end && dcol < view_w) {
            char slot; int sl;
            editor_decode1(ed->text, rp, line_end, &slot, &sl);
            window_put_char(ed->win_id, slot);
            dcol++; rp += sl;
            if (want_caret && rp == ed->cursor_pos) {
                caret_x = w->cursor_x; caret_y = w->cursor_y;
            }
        }
        // Break the grid line (the old code never emitted newlines, so
        // multi-line files rendered as one wrapped stream). Guarded: no
        // break past the last drawn row (it would scroll), and none when
        // the row already wrapped exactly full (cursor is already home).
        if (row + 1 < view_h && pos <= ed->text_len &&
            line_end < ed->text_len && dcol < view_w)
            window_put_char(ed->win_id, '\n');

        // Move to next line
        pos = line_end;
        if (pos < ed->text_len && ed->text[pos] == '\n') pos++;
        line++;
    }
    // Place the caret where the cursor's raw offset was emitted (or leave
    // the cursor alone when it scrolled out of view).
    if (caret_x >= 0 && caret_y >= 0) {
        w->cursor_x = caret_x;
        w->cursor_y = caret_y;
    }

    // Draw line count at bottom-right
    int total_lines = count_lines(ed->text, ed->text_len);    char lc_buf[16];
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
