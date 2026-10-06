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
#include "../src/layout.h"
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
// Stubs for newer kernel services okai.c references (preview never fetches).
uint32_t tick_count = 0;
void draw_char_sized(int x, int y, char c, uint32_t f, uint32_t b, int tw, int th) {
    (void)x; (void)y; (void)c; (void)f; (void)b; (void)tw; (void)th;
}
void https_get_port(const char* h, const char* p, uint16_t port) {
    (void)h; (void)p; (void)port;
}
int http_dechunk(char* buf, int len) { (void)buf; return len; }
void js_run(const char* code) { (void)code; }
void window_set_dirty(int id) { (void)id; }
void graphics_set_clip(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void graphics_clip_reset(void) {}

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
void draw_char_cell(int x, int y, char c, uint32_t f, uint32_t b, int tw, int th, uint8_t attr) {
    (void)x; (void)y; (void)c; (void)f; (void)b; (void)tw; (void)th; (void)attr;
}
void window_write_cell_attr(int id, int row, int col, uint8_t attr) {
    (void)id; (void)row; (void)col; (void)attr;
}
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
const struct layout* preview_layout(void);
struct okai*   okai_get_for_preview(void);
void           okai_render_content_for_preview(void);

// ---- painting ---------------------------------------------------------------
static uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// Glyph metrics mirror the kernel sampler (graphics.c glyph_rows/font_bit):
// main Terminus font 16x32, 2 bytes/row MSB-first; ext slots 8x16, 1 row
// byte MSB-first. Main table comes from ../src/font_data.c (linked in).
static const uint8_t* glyph(char c, int* gw, int* gh, int* gbpr) {
    unsigned char u = (unsigned char)c;
    if (u < 32 || u == 127) return 0;
    if (u < 128) { *gw = 16; *gh = 32; *gbpr = 2; return font8x16[u - 32]; }
    *gw = 8; *gh = 16; *gbpr = 1; return font8x16_ext[u - 0x80];
}

static int glyph_bit(const uint8_t* g, int gbpr, int x, int y) {
    return (g[y * gbpr + (x >> 3)] >> (7 - (x & 7))) & 1;
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
    static char buf[2 * 1024 * 1024];
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

    static char inline_css[INLINE_CSS_SCRATCH];
    int css_len = html_extract_css(buf, len, inline_css, INLINE_CSS_SCRATCH);
    T->css_n = css_parse(inline_css, css_len, T->css_rules, CSS_MAX_RULES);
    // Optional external stylesheet (e.g. a real site's skin CSS) appended after
    // the inline <style> rules — mirrors the kernel's external-CSS fetch.
    {
        const char* cssfile = getenv("PREVIEW_CSS");
        if (cssfile) {
            FILE* cf = fopen(cssfile, "rb");
            if (cf) {
                static char cbuf[512 * 1024];
                int cl = (int)fread(cbuf, 1, sizeof(cbuf) - 1, cf);
                fclose(cf);
                if (cl > 0)
                    T->css_n += css_parse(cbuf, cl, T->css_rules + T->css_n,
                                          CSS_MAX_RULES - T->css_n);
            }
        }
    }
    dom_build(&T->dom, buf, len);
    T->token_count = T->dom.node_count > 0 ? T->dom.node_count : -1;
    if (getenv("PREVIEW_DUMP"))
        fprintf(stderr, "[dom] nodes=%d text=%d trunc=%d\n",
                T->dom.node_count, T->dom.text_len, T->dom.truncated);
    html_get_title(buf, len, T->title, sizeof(T->title));

    okai_render_content_for_preview();
    const struct layout* L = preview_layout();
    int doc_lines = L->height;
    uint32_t pfg = L->page_fg, pbg = L->page_bg;
    (void)pfg;

    // Build a per-cell character + color map from the display list, then paint.
    int maxr = doc_lines < 400 ? doc_lines : 400;
    int W = cols * 16, H = maxr * 32;

    // per-cell scratch
    static char cellch[400 * 256];
    static uint32_t cellfg[400 * 256], cellbg[400 * 256];
    static uint8_t cellattr[400 * 256];
    memset(cellch, 0, (size_t)maxr * cols);
    memset(cellattr, 0, (size_t)maxr * cols);
    for (int i = 0; i < maxr * cols; i++) { cellfg[i] = pfg; cellbg[i] = pbg; }

    // bands first
    for (int bi = 0; bi < L->n_items; bi++) {
        const struct layout_item* it = &L->items[bi];
        if (it->kind != LOUT_BAND || !it->draw_box) continue;
        for (int r = it->row; r < it->row + it->height && r < maxr; r++) {
            if (r < 0) continue;
            for (int c = it->box_left; c < it->box_left + it->box_width && c < cols; c++) {
                if (c < 0) continue;
                cellbg[r * cols + c] = it->box_bg;
                cellch[r * cols + c] = ' ';
            }
        }
    }
    // lines
    for (int li = 0; li < L->n_items; li++) {
        const struct layout_item* it = &L->items[li];
        if (it->kind != LOUT_LINE || it->row >= maxr) continue;
        for (int ri = it->run_start; ri < it->run_start + it->run_count; ri++) {
            const struct layout_run* run = &L->runs[ri];
            for (int k = 0; k < run->text_len; k++) {
                int c = run->col + k;
                if (c < 0 || c >= cols) continue;
                int idx = it->row * cols + c;
                cellch[idx] = L->text[run->text_off + k];
                cellfg[idx] = run->fg;
                cellbg[idx] = run->bg;
                cellattr[idx] = run->flags;
            }
        }
    }

    if (getenv("PREVIEW_DUMP")) {
        printf("---- doc dump (%d lines, left=%d width=%d) ----\n",
               doc_lines, L->page_left, L->width);
        int dump_rows = doc_lines < 200 ? doc_lines : 200;
        for (int r = 0; r < dump_rows; r++) {
            char line[512]; int n = 0;
            for (int c = 0; c < L->width && c < cols && n < 500; c++) {
                char ch = cellch[r * cols + c];
                line[n++] = ((unsigned char)ch < 32) ? ' ' : ch;
            }
            while (n > 0 && line[n-1] == ' ') n--;
            line[n] = 0;
            printf("%3d|%s\n", r, line);
        }
        printf("---- end dump ----\n");
    }

    // Heading lines (H1=3x, H2=2x): blank their grid cells and paint the whole
    // scaled glyph below, exactly like the kernel's pixel overlay.
    static uint8_t heading_scale[400];
    memset(heading_scale, 0, sizeof(heading_scale));
    for (int li = 0; li < L->n_items; li++) {
        const struct layout_item* it = &L->items[li];
        if (it->kind == LOUT_LINE && it->heading && it->row < 400) {
            int s = it->heading <= 1 ? 3 : it->heading == 2 ? 2 : 1;
            if (s > 1) {
                heading_scale[it->row] = (uint8_t)s;
                for (int c = 0; c < cols; c++) {
                    cellch[it->row * cols + c] = ' ';
                }
            }
        }
    }

    int rows = maxr;
    uint8_t* rgb = calloc((size_t)W * H * 3, 1);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            char ch = cellch[idx];
            uint32_t fg = cellfg[idx], bg = cellbg[idx];
            for (int py = r * 32; py < (r + 1) * 32 && py < H; py++) {
                for (int px = c * 16; px < (c + 1) * 16 && px < W; px++) {
                    int o = (py * W + px) * 3;
                    rgb[o] = bg >> 16; rgb[o + 1] = bg >> 8; rgb[o + 2] = bg;
                }
            }
            if (ch == ' ' || (unsigned char)ch < 33) continue;
            int gw, gh, gbpr;
            const uint8_t* g = glyph(ch, &gw, &gh, &gbpr);
            if (!g) continue;
            int attr = cellattr[idx];
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    if (!glyph_bit(g, gbpr, gx, gy)) continue;
                    int px = c * 16 + gx, py = r * 32 + gy;
                    if (px >= W || py >= H) continue;
                    int o = (py * W + px) * 3;
                    rgb[o] = fg >> 16; rgb[o + 1] = fg >> 8; rgb[o + 2] = fg;
                    if ((attr & 1) && px + 1 < (c + 1) * 16 && px + 1 < W) {
                        int o2 = (py * W + px + 1) * 3;
                        rgb[o2] = fg >> 16; rgb[o2 + 1] = fg >> 8; rgb[o2 + 2] = fg;
                    }
                }
            }
            if (attr & 2) {
                for (int ux = 0; ux < 16; ux++) {
                    int px = c * 16 + ux, py = r * 32 + 30;
                    if (px >= W || py >= H) continue;
                    int o = (py * W + px) * 3;
                    rgb[o] = fg >> 16; rgb[o + 1] = fg >> 8; rgb[o + 2] = fg;
                }
            }
        }
    }

    // Paint scaled headings last (over the blanked rows).
    for (int li = 0; li < L->n_items; li++) {
        const struct layout_item* it = &L->items[li];
        if (it->kind != LOUT_LINE || !heading_scale[it->row]) continue;
        int scale = heading_scale[it->row];
        for (int ri = it->run_start; ri < it->run_start + it->run_count; ri++) {
            const struct layout_run* run = &L->runs[ri];
            for (int k = 0; k < run->text_len; k++) {
                int c = run->col + k * scale; // heading runs are scale-columns apart
                if (c < 0 || c >= cols) continue;
                char ch = L->text[run->text_off + k];
                if (ch == ' ' || (unsigned char)ch < 33) continue;
                int gw, gh, gbpr;
                const uint8_t* g = glyph(ch, &gw, &gh, &gbpr);
                if (!g) continue;
                for (int gy = 0; gy < gh; gy++) {
                    for (int gx = 0; gx < gw; gx++) {
                        if (!glyph_bit(g, gbpr, gx, gy)) continue;
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                int px = c * 16 + gx * scale + sx;
                                int py = it->row * 32 + gy * scale + sy;
                                if (px >= W || py >= H) continue;
                                int o = (py * W + px) * 3;
                                rgb[o] = run->fg >> 16; rgb[o + 1] = run->fg >> 8; rgb[o + 2] = run->fg;
                                // bold overstrike
                                int px2 = px + 1;
                                if (px2 < W) {
                                    int o2 = (py * W + px2) * 3;
                                    rgb[o2] = run->fg >> 16; rgb[o2 + 1] = run->fg >> 8; rgb[o2 + 2] = run->fg;
                                }
                            }
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
    printf("okai preview: %d dom nodes, %d items, %d runs, %d css rules, %d doc lines -> %s (%dx%d)\n",
           T->dom.node_count, L->n_items, L->n_runs, T->css_n, doc_lines, out_path, W, H);
    return 0;
}
