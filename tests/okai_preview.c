/* okai_preview — render a web page exactly as okai lays it out, on the host.
 *
 * Compiles the REAL kernel sources (html.c tokenizer + charset/entities,
 * css.c engine, okai.c document layout) with small stubs for kernel
 * services, then paints the virtual document grid into a PNG with the real
 * font and VGA colors. Fidelity: identical text/CSS/layout to the kernel —
 * only the pixel chrome (toolbar/tabs) is absent.
 *
 * Build:  cd tests && make -f Makefile.preview okai_preview
 * Run:    ./okai_preview page.html out.png [cols]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <zlib.h>

#include "../src/html.h"
#include "../src/css.h"
#include "../src/okai.h"
#include "../src/window.h"
#include "../src/graphics.h"
#include "font_data.h"

// ---- kernel stubs -----------------------------------------------------------
static struct window fake_win;

struct window* window_get(int id) { (void)id; return &fake_win; }
void window_write_cell(int id, int row, int col, char c, uint8_t fg, uint8_t bg) {
    (void)id; (void)row; (void)col; (void)c; (void)fg; (void)bg;
}
void window_write_cell_rgb(int id, int row, int col, char c, uint32_t fg, uint32_t bg) {
    (void)id; (void)row; (void)col; (void)c; (void)fg; (void)bg;
}
void window_set_content_bg(int id, uint8_t bg) { (void)id; fake_win.content_bg = bg; }
void window_set_content_bg_rgb(int id, uint32_t bg) { (void)id; fake_win.content_bg = 0; }
void window_set_text_color_rgb(int id, uint32_t fg, uint32_t bg) { (void)id; (void)fg; (void)bg; }
void window_set_hide_cursor(int id, int flag) { (void)id; (void)flag; }
void window_set_title(int id, const char* t) { (void)id; (void)t; }
int  window_color_is_light(uint8_t idx) {
    return idx == 0 || idx == 7 || idx == 8 || idx == 15;
}
int  window_rgb_is_light(uint32_t c) {
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    return (r * 77 + g * 150 + b * 29) / 256 > 128;
}
void* kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void* p) { free(p); }

// okai.c references these for navigation; the preview never triggers them.
void http_get(const char* h, const char* p) { (void)h; (void)p; }
void http_get_port(const char* h, const char* p, uint16_t port) { (void)h; (void)p; (void)port; }
void http_reset_conn_attempts(void) {}
int  http_is_retry_pending(void) { return 0; }
void https_get(const char* h, const char* p) { (void)h; (void)p; }
int  tls_is_done(void) { return 0; }
int  tls_get_response_len(void) { return 0; }
char* tls_get_response(void) { return 0; }
void net_poll(void) {}

// graphics/window stubs — referenced by okai.c's chrome/nav code, which the
// preview never invokes.
const uint32_t vga_to_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
void rect_fill(int x, int y, int w, int h, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void rect_outline(int x, int y, int w, int h, uint32_t c, int t) { (void)x; (void)y; (void)w; (void)h; (void)c; (void)t; }
void gradient_fill(int x, int y, int w, int h, uint32_t a, uint32_t b, int v) { (void)x; (void)y; (void)w; (void)h; (void)a; (void)b; (void)v; }
void hline(int x, int y, int l, uint32_t c) { (void)x; (void)y; (void)l; (void)c; }
void vline(int x, int y, int l, uint32_t c) { (void)x; (void)y; (void)l; (void)c; }
void kline(int x0, int y0, int x1, int y1, uint32_t c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
void line(int x0, int y0, int x1, int y1, uint32_t c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
void round_rect_fill(int x, int y, int w, int h, uint32_t c, int r) { (void)x; (void)y; (void)w; (void)h; (void)c; (void)r; }
void draw_char(int x, int y, char c, uint32_t f, uint32_t b) { (void)x; (void)y; (void)c; (void)f; (void)b; }
void draw_char_scaled(int x, int y, char c, uint32_t f, uint32_t b, int s) { (void)x; (void)y; (void)c; (void)f; (void)b; (void)s; }
void draw_char_1x(int x, int y, char c, uint32_t f, uint32_t b) { (void)x; (void)y; (void)c; (void)f; (void)b; }
void draw_string_1x(int x, int y, const char* s, uint32_t f, uint32_t b) { (void)x; (void)y; (void)s; (void)f; (void)b; }
int  window_create(const char* t, int x, int y, int w, int h) { (void)t; (void)x; (void)y; (void)w; (void)h; return 1; }
void window_destroy(int id) { (void)id; }
void window_set_focus(int id) { (void)id; }
void window_set_close_button(int id, int c) { (void)id; (void)c; }
void window_set_minimize_button(int id, int c) { (void)id; (void)c; }
void window_put_char(int id, char c) { (void)id; (void)c; }
void window_puts(int id, const char* s) { (void)id; (void)s; }
void window_set_cursor(int id, int r, int c) { (void)id; (void)r; (void)c; }
void window_clear(int id) { (void)id; }
void window_set_text_color(int id, uint8_t fg, uint8_t bg) { (void)id; (void)fg; (void)bg; }
void window_set_no_titlebar(int id, int f) { (void)id; (void)f; }
void window_set_font_scale(int id, int s) { (void)id; (void)s; }

void serial_printf(const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
void serial_puts(const char* s) { fputs(s, stdout); }
void serial_putchar(char c) { putchar(c); }

int needs_redraw = 0; // window.c owns this; okai.c externs it

// provided by okai.c under HOST_PREVIEW
const char*    preview_doc_chars(void);
const uint32_t* preview_doc_fg_rgb(void);
const uint32_t* preview_doc_bg_rgb(void);
const uint8_t* preview_doc_kinds(void);
int            preview_doc_dims(int* left, int* width);
int            preview_doc_cols(void);
void           preview_page_colors(uint32_t* fg, uint32_t* bg);
struct okai*   okai_get_for_preview(void);
void           okai_render_content_for_preview(void);

// ---- painting ---------------------------------------------------------------
static uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static const uint8_t* glyph(char c) {
    unsigned char u = (unsigned char)c;
    if (u < 32 || u == 127) return 0;
    if (u < 128) return font8x16[u - 32];
    return font8x16_ext[u - 0x80];
}

static void be32(unsigned char* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t crc32_all(const unsigned char* p, int n) {
    static uint32_t tab[256]; static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void wchunk(FILE* o, const char* type, const unsigned char* payload, int len) {
    unsigned char lb[4]; be32(lb, (uint32_t)len); fwrite(lb, 1, 4, o);
    int tl = 0; while (type[tl]) tl++;
    fwrite(type, 1, tl, o);
    if (payload && len) fwrite(payload, 1, len, o);
    int total = tl + (len > 0 ? len : 0);
    unsigned char* both = malloc(total);
    memcpy(both, type, tl);
    if (payload && len) memcpy(both + tl, payload, len);
    unsigned char cb[4]; be32(cb, crc32_all(both, total));
    fwrite(cb, 1, 4, o);
    free(both);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <page.html> [out.png] [cols]\n", argv[0]);
        return 1;
    }
    const char* out_path = argc > 2 ? argv[2] : "okai_preview.png";

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static char buf[512 * 1024];
    int len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;

    int cols = argc > 3 ? atoi(argv[3]) : (880 - 4) / 16;
    fake_win.x = 1010; fake_win.y = 60; fake_win.w = 880; fake_win.h = 650;
    fake_win.no_titlebar = 1; fake_win.font_scale = 1; fake_win.visible = 1;
    fake_win.content = (uint16_t*)1; // non-NULL: okai_render_content requires it
    fake_win.content_w = cols;
    fake_win.content_h = (650 - 4) / 32;

    struct okai* b = okai_get_for_preview();
    memset(b, 0, sizeof(*b));
    b->win_id = 1;
    b->tab_count = 1;
    b->active_tab = 0;
    struct okai_tab* T = okai_tab_of(b);
    snprintf(T->url, sizeof(T->url), "preview://%s", argv[1]);

    int css_len = html_extract_css(buf, len, T->css_text, OKAI_CSS_TEXT);
    T->css_n = css_parse(T->css_text, css_len, T->css_rules, CSS_MAX_RULES);
    int count = html_parse(buf, len, T->tokens, OKAI_TAB_TOKENS);
    T->token_count = count;
    html_get_title(buf, len, T->title, sizeof(T->title));

    okai_render_content_for_preview();

    // ---- paint the document grid ------------------------------------------
    int left, width;
    int doc_lines = preview_doc_dims(&left, &width);
    const char* chars = preview_doc_chars();
    const uint32_t* fgs = preview_doc_fg_rgb();
    const uint32_t* bgs = preview_doc_bg_rgb();
    const uint8_t* kinds = preview_doc_kinds();
    int stride = preview_doc_cols();
    uint32_t pfg, pbg;
    preview_page_colors(&pfg, &pbg);

    int rows = doc_lines < 60 ? doc_lines : 60;
    int W = cols * 16, H = rows * 32;
    uint8_t* rgb = calloc((size_t)W * H * 3, 1);
    uint32_t bgc = pbg;
    for (int i = 0; i < W * H; i++) {
        rgb[i * 3] = bgc >> 16; rgb[i * 3 + 1] = bgc >> 8; rgb[i * 3 + 2] = bgc;
    }
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < width && c < cols; c++) {
            char ch = chars[r * stride + c];
            if (ch == ' ' || (unsigned char)ch < 33) continue;
            const uint8_t* g = glyph(ch);
            if (!g) continue;
            uint32_t fg = fgs[r * stride + c];
            int kind = kinds[r];
            int scale = kind == 2 ? 3 : kind == 1 ? 2 : 1; // heading sizes
            for (int gy = 0; gy < 16; gy++) {
                for (int gx = 0; gx < 8; gx++) {
                    if (!((g[gy] >> (7 - gx)) & 1)) continue;
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = (c * 8 + gx * scale + sx) * 2;
                            int py = (r * 16 + gy * scale + sy) * 2;
                            if (px >= W || py >= H) continue;
                            int o = (py * W + px) * 3;
                            rgb[o] = fg >> 16; rgb[o + 1] = fg >> 8; rgb[o + 2] = fg;
                        }
                    }
                }
            }
        }
    }

    // ---- write PNG ----------------------------------------------------------
    FILE* o = fopen(out_path, "wb");
    if (!o) { perror(out_path); return 1; }
    unsigned char sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, o);
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)W); be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    wchunk(o, "IHDR", ihdr, 13);
    long raw_len = (long)W * 3 + 1;
    unsigned char* raw = malloc((size_t)raw_len * H);
    for (int y = 0; y < H; y++) {
        raw[y * raw_len] = 0;
        memcpy(raw + y * raw_len + 1, rgb + (size_t)y * W * 3, (size_t)W * 3);
    }
    uLongf zlen = compressBound((uLong)raw_len * H);
    unsigned char* zbuf = malloc(zlen);
    if (compress2(zbuf, &zlen, raw, (uLong)raw_len * H, 9) != Z_OK) {
        fprintf(stderr, "zlib failed\n"); return 1;
    }
    wchunk(o, "IDAT", zbuf, (int)zlen);
    wchunk(o, "IEND", NULL, 0);
    fclose(o);
    printf("okai preview: %d tokens, %d css rules, %d doc lines -> %s (%dx%d)\n",
           count, T->css_n, doc_lines, out_path, W, H);
    return 0;
}
