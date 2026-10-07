/* Host unit test for the editor text/read-only paths (offline, stubbed windows):
   slot reuse after close, UTF-8 draw decoding with byte-exact saves,
   whole-sequence backspace, binary sniffing, read-only text guards. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/editor.h"
#include "../src/window.h"
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

// ---- window stubs (record the emitted frame + emulate the grid) ----
static struct window fake_win;
static char frame_log[49152];
static int frame_n = 0;
#define GW 40
#define GH 12
static char grid[GH][GW];
static int gx, gy;
// faithful kernel window_put_char core (cursor, wrap, scroll w/ ring drop)
static void emit(char c) {
    if (c == '\n') { gx = 0; gy++; }
    else if (c == '\r') { gx = 0; }
    else if (c == '\b') { if (gx > 0) gx--; }
    else {
        if (gx < GW && gy < GH) grid[gy][gx] = c;
        gx++;
    }
    if (gx >= GW) { gx = 0; gy++; }
    while (gy >= GH) {
        for (int r = 0; r < GH - 1; r++)
            for (int q = 0; q < GW; q++) grid[r][q] = grid[r + 1][q];
        for (int q = 0; q < GW; q++) grid[GH - 1][q] = ' ';
        gy = GH - 1;
    }
    fake_win.cursor_x = gx; fake_win.cursor_y = gy;
}

struct window* window_get(int id) { (void)id; return &fake_win; }
int window_create(const char* t, int x, int y, int w, int h) {
    (void)t; (void)x; (void)y; (void)w; (void)h;
    fake_win.content_w = 40; fake_win.content_h = 12;
    fake_win.cursor_x = 0; fake_win.cursor_y = 0;
    fake_win.dirty = 0;
    return 7;
}
void window_destroy(int id) { (void)id; }
void window_set_focus(int id) { (void)id; }
int window_get_focused(void) { return 7; }
void window_set_close_button(int id, int h) { (void)id; (void)h; }
void window_set_minimize_button(int id, int h) { (void)id; (void)h; }
void window_set_hide_cursor(int id, int f) { (void)id; (void)f; }
void window_set_text_color(int id, uint8_t f, uint8_t b) {
    (void)id; (void)f; (void)b;
}
void window_set_cursor(int id, int r, int c) {
    (void)id;
    fake_win.cursor_x = c; fake_win.cursor_y = r;
}
void window_put_char(int id, char c) {
    (void)id;
    if (frame_n < (int)sizeof(frame_log) - 1) frame_log[frame_n++] = c;
    emit(c);
}
void window_puts(int id, const char* s) {
    (void)id;
    while (*s) {
        if (frame_n < (int)sizeof(frame_log) - 1) frame_log[frame_n++] = *s;
        emit(*s);
        s++;
    }
}
void window_clear(int id) {
    (void)id; frame_n = 0; gx = 0; gy = 0;
    fake_win.cursor_x = 0; fake_win.cursor_y = 0;
    for (int r = 0; r < GH; r++)
        for (int q = 0; q < GW; q++) grid[r][q] = ' ';
}
static void grid_row(int r, char* out) {
    for (int q = 0; q < GW; q++) out[q] = grid[r][q];
    out[GW] = 0;
    int e = GW;
    while (e > 0 && out[e - 1] == ' ') e--;
    out[e] = 0;
}

// ---- serial stubs ----
void serial_puts(const char* s) { (void)s; }
void serial_putchar(char c) { (void)c; }

// ---- in-memory fs ----
#define MEMFS_N 8
static char mem_name[MEMFS_N][32];
static uint8_t mem_data[MEMFS_N][4096];
static int mem_size[MEMFS_N];
static int mem_used = 0;
static int memfs_find(const char* n) {
    for (int i = 0; i < mem_used; i++)
        if (!strcmp(mem_name[i], n)) return i;
    return -1;
}
int fs_exists(const char* n) { return memfs_find(n) >= 0; }
int fs_create(const char* n) {
    if (memfs_find(n) >= 0) return 0;
    if (mem_used >= MEMFS_N) return -1;
    int i = mem_used++;
    strncpy(mem_name[i], n, 31); mem_name[i][31] = 0;
    mem_size[i] = 0;
    return 0;
}
int fs_write(const char* n, const uint8_t* d, int len) {
    int i = memfs_find(n);
    if (i < 0) { if (fs_create(n) != 0) return -1; i = memfs_find(n); }
    if (len > 4096) len = 4096;
    memcpy(mem_data[i], d, len);
    mem_size[i] = len;
    return len;
}
int fs_read(const char* n, uint8_t* buf, int max_len) {
    int i = memfs_find(n);
    if (i < 0) return -1;
    int len = mem_size[i] < max_len ? mem_size[i] : max_len;
    memcpy(buf, mem_data[i], len);
    return len;
}
static void memfs_put(const char* n, const uint8_t* d, int len) {
    fs_write(n, d, len);
}

// find byte run in frame log
static int log_has(const uint8_t* pat, int plen) {
    if (plen <= 0 || plen > frame_n) return 0;
    for (int i = 0; i + plen <= frame_n; i++)
        if (!memcmp(frame_log + i, pat, plen)) return 1;
    return 0;
}

int main(void) {
    editor_init();

    // ---- slot reuse: fill all 4, close all, open again ----
    {
        int e[4];
        for (int i = 0; i < 4; i++) {
            char nm[16]; snprintf(nm, sizeof(nm), "/f%d", i);
            e[i] = editor_open(nm);
        }
        CHECK(e[0] >= 0 && e[3] >= 0, "4 editors open");
        CHECK(editor_open("/full") < 0, "5th concurrent open refused (cap holds)");
        for (int i = 0; i < 4; i++) editor_close(e[i]);
        int e5 = editor_open("/five");
        CHECK(e5 >= 0, "5th open succeeds after closing all (no slot leak)");
        editor_close(e5);
    }

    // ---- UTF-8 decode: A + U+041F + P + U+0439 + B ----
    {
        const uint8_t raw[] = { 'A', 0xD0, 0x9F, 'P', 0xD0, 0xB9, 'B' };
        memfs_put("/u.txt", raw, sizeof(raw));
        int e = editor_open("/u.txt");
        struct editor* ed = editor_get(e);
        CHECK(ed && !ed->read_only, "utf-8 text opens in text mode");
        editor_draw(e);
        const uint8_t want[] = { 'A', 0x8F, 'P', 0xA9, 'B' };
        CHECK(log_has(want, sizeof(want)), "multibyte seqs drawn as single slots");
        // byte-exact save preserved
        editor_handle_key(e, 0x13); // Ctrl+S
        int i = memfs_find("/u.txt");
        CHECK(i >= 0 && mem_size[i] == 7 && !memcmp(mem_data[i], raw, 7),
              "save writes back raw bytes, not slots");
        editor_close(e);
    }

    // ---- whole-sequence backspace ----
    {
        const uint8_t raw[] = { 'A', 0xD0, 0x9F };
        memfs_put("/bs.txt", raw, sizeof(raw));
        int e = editor_open("/bs.txt");
        struct editor* ed = editor_get(e);
        ed->cursor_pos = 3; // end (open leaves it at 0)
        editor_handle_key(e, '\b');
        CHECK(ed->text_len == 1 && ed->text[0] == 'A',
              "backspace erases whole 2-byte sequence");
        editor_close(e);
    }

    // ---- malformed UTF-8 never hangs, draws '?' ----
    {
        const uint8_t raw[] = { 'x', 0x80, 0xD0 }; // lone cont + truncated seq
        memfs_put("/bad.txt", raw, sizeof(raw));
        int e = editor_open("/bad.txt");
        editor_draw(e); // must terminate
        const uint8_t q1[] = { 'x', '?', '?' };
        CHECK(log_has(q1, sizeof(q1)), "malformed sequences draw as ?");
        editor_close(e);
    }

    // ---- binary sniff + read-only guards (no hex anywhere) ----
    {
        const uint8_t bin[] = { 0x7F, 'E', 'L', 'F', 0x00, 0x01, 0x02, 'H', 'i' };
        memfs_put("/b.bin", bin, sizeof(bin));
        int e = editor_open("/b.bin");
        struct editor* ed = editor_get(e);
        CHECK(ed && ed->read_only, "NUL bytes -> read-only");
        editor_draw(e);
        const uint8_t t[] = { '.', 'E', 'L', 'F', '.', '.', '.', 'H', 'i' };
        CHECK(log_has(t, sizeof(t)), "binary shows textified .ELF...Hi");
        const uint8_t hx[] = { '7', 'F' };
        CHECK(!log_has(hx, sizeof(hx)), "no hex digits in binary view");
        int before = mem_size[memfs_find("/b.bin")];
        editor_handle_key(e, 'A'); // ignored in read-only mode
        editor_handle_key(e, 0x13); // Ctrl+S ignored (never saves)
        CHECK(mem_size[memfs_find("/b.bin")] == before &&
              !memcmp(mem_data[memfs_find("/b.bin")], bin, before),
              "read-only keystrokes change nothing on disk");
        editor_handle_key(e, 0x18); // Ctrl+X closes
        CHECK(editor_get(e) == 0, "Ctrl+X closes read-only view");
    }

    // ---- plain ASCII exactness + real line breaks ----
    {
        const uint8_t raw[] = { 'h', 'i', '\n', 'y', 'o' };
        memfs_put("/a.txt", raw, sizeof(raw));
        int e = editor_open("/a.txt");
        CHECK(!editor_get(e)->read_only, "ascii opens as text");
        editor_draw(e);
        char r0[64], r1[64], r2[64];
        grid_row(0, r0); grid_row(1, r1); grid_row(2, r2);
        CHECK(strstr(r0, "Ctrl+S") != 0, "row 0 keeps the status bar");
        CHECK(!strcmp(r1, "hi"), "row 1 shows first line alone");
        CHECK(!strcmp(r2, "yo"), "row 2 shows second line (breaks!)");
        editor_close(e);
    }
    // ---- longer content: no line may vanish (in-guest regression: the
    // first of two lines failed to display while the file was intact) ----
    {
        const char* s = "first line\nsecond line here\n";
        memfs_put("/v.txt", (const uint8_t*)s, strlen(s));
        int e = editor_open("/v.txt");
        editor_draw(e);
        char r1[64], r2[64];
        grid_row(1, r1); grid_row(2, r2);
        CHECK(!strcmp(r1, "first line"), "long: first line intact");
        CHECK(!strcmp(r2, "second line here"), "long: second line intact");
        editor_close(e);
    }

    printf(fails ? "EDITOR TEXT FAIL (%d)\n" : "EDITOR TEXT PASS\n", fails);
    return fails != 0;
}
