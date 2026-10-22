// Read-only text viewer windows (see textview.h).
#include "textview.h"
#include "window.h"
#include "graphics.h"
#include "memory.h"
#include "string.h"

#define TV_MAX 4
#define TV_FG 0xD8DEE9
#define TV_BG 0x10131A
#define TV_DIM 0x6B7385

struct tv {
    int win;
    char* text;
    uint32_t len;
    uint32_t top;                // first visible (wrapped) line
    int cols, rows;              // the grid the wrap was made for
};
static struct tv tvs[TV_MAX] = { { -1 }, { -1 }, { -1 }, { -1 } };

static struct tv* find(int win) {
    for (int i = 0; i < TV_MAX; i++) if (tvs[i].win >= 0 && tvs[i].win == win) return &tvs[i];
    return 0;
}

int textview_owns(int win) { return find(win) != 0; }

// The offset of wrapped line n (lines wrap at cols), or len.
static uint32_t line_start(const struct tv* t, uint32_t n, int cols) {
    uint32_t i = 0, l = 0;
    int col = 0;
    while (l < n && i < t->len) {
        char c = t->text[i++];
        if (c == '\n') { l++; col = 0; }
        else if (++col == cols) { l++; col = 0; if (i < t->len && t->text[i] == '\n') i++; }
    }
    return i;
}

static uint32_t line_count(const struct tv* t, int cols) {
    uint32_t l = 0;
    int col = 0;
    for (uint32_t i = 0; i < t->len; i++) {
        char c = t->text[i];
        if (c == '\n') { l++; col = 0; }
        else if (++col == cols) { l++; col = 0; if (i + 1 < t->len && t->text[i + 1] == '\n') i++; }
    }
    return col ? l + 1 : l;
}

static void draw(struct tv* t) {
    struct window* w = window_get(t->win);
    if (!w || !w->content) return;
    int cols = w->content_w, rows = w->content_h - 1;          // the last row is the status line
    if (cols < 8 || rows < 1) return;
    t->cols = w->content_w;
    t->rows = w->content_h;
    uint32_t total = line_count(t, cols);
    uint32_t maxtop = total > (uint32_t)rows ? total - (uint32_t)rows : 0;
    if (t->top > maxtop) t->top = maxtop;
    uint32_t i = line_start(t, t->top, cols);
    for (int r = 0; r < rows; r++) {
        int c = 0;
        while (i < t->len && c < cols) {
            char ch = t->text[i];
            if (ch == '\n') break;
            window_write_cell_rgb(t->win, r, c++, (ch >= 32 && ch < 127) ? ch : '.', TV_FG, TV_BG);
            i++;
        }
        if (i < t->len && t->text[i] == '\n') i++;
        for (; c < cols; c++) window_write_cell_rgb(t->win, r, c, ' ', TV_FG, TV_BG);
    }
    // status: position
    char st[96];
    uint32_t last = t->top + (uint32_t)rows < total ? t->top + (uint32_t)rows : total;
    int n = 0;
    uint32_t v[3] = { total ? t->top + 1 : 0, last, total };
    for (int k = 0; k < 3; k++) {
        char d[12]; int m = 0; uint32_t x = v[k];
        do { d[m++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (m) st[n++] = d[--m];
        if (k < 2) st[n++] = k == 0 ? '-' : '/';
    }
    st[n] = 0;
    int c = 0;
    for (; c < cols - n - 1; c++) window_write_cell_rgb(t->win, rows, c, ' ', TV_DIM, TV_BG);
    for (int k = 0; k < n && c < cols; k++) window_write_cell_rgb(t->win, rows, c++, st[k], TV_DIM, TV_BG);
    for (; c < cols; c++) window_write_cell_rgb(t->win, rows, c, ' ', TV_DIM, TV_BG);
    window_set_dirty(t->win);
}

int textview_open(const char* title, const char* text, uint32_t len) {
    struct tv* t = 0;
    for (int i = 0; i < TV_MAX; i++) if (tvs[i].win < 0) { t = &tvs[i]; break; }
    if (!t) return -1;
    t->text = (char*)kmalloc(len ? len : 1);
    if (!t->text) return -1;
    memcpy(t->text, text, len);
    t->len = len;
    t->top = 0;
    int slot = (int)(t - tvs);
    int w = SCREEN_W * 2 / 3, h = SCREEN_H * 3 / 4;
    t->win = window_create(title, SCREEN_W / 6 + slot * 40, 40 + slot * 40, w, h);
    if (t->win < 0) { kfree(t->text); t->text = 0; return -1; }
    window_set_close_button(t->win, 1);
    window_set_minimize_button(t->win, 1);
    window_set_hide_cursor(t->win, 1);
    window_set_content_bg_rgb(t->win, TV_BG);
    window_set_focus(t->win);
    draw(t);
    return t->win;
}

void textview_close(int win) {
    struct tv* t = find(win);
    if (!t) return;
    window_destroy(t->win);
    kfree(t->text);
    t->text = 0;
    t->win = -1;
}

static void scroll(struct tv* t, int lines) {
    if (lines < 0 && (uint32_t)(-lines) > t->top) t->top = 0;
    else t->top += (uint32_t)lines;                           // draw() clamps the end
    draw(t);
}

void textview_key(int win, char c) {
    struct tv* t = find(win);
    if (!t) return;
    int page = t->rows > 2 ? t->rows - 2 : 1;
    if (c == 'q' || c == 27) textview_close(win);
    else if (c == '\x11' || c == 'k') scroll(t, -1);
    else if (c == '\x10' || c == 'j' || c == '\n') scroll(t, 1);
    else if (c == '\x12' || c == 'b') scroll(t, -page);
    else if (c == '\x04' || c == ' ') scroll(t, page);
    else if (c == 'g') { t->top = 0; draw(t); }
    else if (c == 'G') { t->top = 0xFFFFFFFFu; draw(t); }
}

void textview_wheel(int win, int notches) {
    struct tv* t = find(win);
    if (t) scroll(t, notches * 3);
}

void textview_redraw_if_resized(void) {
    for (int i = 0; i < TV_MAX; i++) {
        if (tvs[i].win < 0) continue;
        struct window* w = window_get(tvs[i].win);
        if (w && (w->content_w != tvs[i].cols || w->content_h != tvs[i].rows)) draw(&tvs[i]);
    }
}
