// okai on Windows -- the screen: a 32-bit DIB backbuffer with the kernel's
// damage model (row band + clip rectangle), blitted to the window.
//
// Pixel format is 0x00RRGGBB like graphics.h says. A BI_RGB 32bpp DIB stores
// each pixel as B, G, R, X in memory, which is exactly that DWORD on a
// little-endian machine, so no conversion is needed anywhere.

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "graphics.h"
#include "window.h"
#include "win_plat.h"

int gfx_w = 1280, gfx_h = 800;
int win_client_w, win_client_h;

static uint32_t* backbuf;
static int bb_owned;                  // 1 = calloc'd by us, 0 = the DIB's bits (never free())
static HDC mem_dc, win_dc;
static HBITMAP dib;
static HGDIOBJ dib_old;

// Rows [dirty_top, dirty_bot) changed since the last present.
static int dirty_top, dirty_bot;
static int cl_x0, cl_y0, cl_x1, cl_y1;   // current clip, [x0,x1) x [y0,y1)

static void mark_rows(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > gfx_h) y1 = gfx_h;
    if (y0 >= y1) return;
    if (dirty_top >= dirty_bot) { dirty_top = y0; dirty_bot = y1; return; }
    if (y0 < dirty_top) dirty_top = y0;
    if (y1 > dirty_bot) dirty_bot = y1;
}

void graphics_mark_dirty(int y) { mark_rows(y, y + 1); }

void graphics_mark_rect(int x, int y, int w, int h) {
    (void)x; (void)w;
    mark_rows(y, y + h);
}

void win_gfx_init(int client_w, int client_h) {
    // A second call is a resize (WM_SIZE during CreateWindowEx creates the
    // surface, and WinMain may ask again): tear the old one down first.
    if (mem_dc) win_gfx_free();
    if (client_w < 64) client_w = 64;
    if (client_h < 64) client_h = 64;
    if (client_w > SCREEN_W_MAX) client_w = SCREEN_W_MAX;
    if (client_h > SCREEN_H_MAX) client_h = SCREEN_H_MAX;
    win_client_w = client_w;
    win_client_h = client_h;
    gfx_w = client_w;
    gfx_h = client_h;              // the port has no taskbar: the screen IS the client area
    dirty_top = dirty_bot = 0;
    cl_x0 = 0; cl_y0 = 0; cl_x1 = gfx_w; cl_y1 = gfx_h;

    win_dc = GetDC(win_hwnd);
    mem_dc = CreateCompatibleDC(win_dc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = gfx_w;
    bi.bmiHeader.biHeight = -gfx_h;      // top-down: row 0 is the top
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, (void**)&backbuf, 0, 0);
    if (!dib || !backbuf) {
        win_logf("[okai-win] CreateDIBSection failed (%ld)\n", (long)GetLastError());
        MessageBoxA(win_hwnd, "Could not create the screen buffer.", "okai", MB_ICONERROR);
        ExitProcess(1);
    }
    bb_owned = 0;                        // the bits belong to the DIB
    dib_old = SelectObject(mem_dc, dib);
    SetBkMode(mem_dc, OPAQUE);
}

void win_gfx_free(void) {
    if (mem_dc) {
        if (dib_old) SelectObject(mem_dc, dib_old);
        dib_old = 0;
        if (dib) DeleteObject(dib);
        dib = 0;
        DeleteDC(mem_dc);
        mem_dc = 0;
    }
    if (win_dc) { ReleaseDC(win_hwnd, win_dc); win_dc = 0; }
    if (bb_owned) free(backbuf);
    bb_owned = 0;
    backbuf = 0;
}

static void present_rows(int y0, int y1) {
    if (!mem_dc || !win_dc || !backbuf) return;
    if (y0 < 0) y0 = 0;
    if (y1 > win_client_h) y1 = win_client_h;   // never show the off-screen band
    if (y0 >= y1) return;
    BitBlt(win_dc, 0, y0, gfx_w, y1 - y0, mem_dc, 0, y0, SRCCOPY);
}

void win_gfx_present(void) {
    if (dirty_top >= dirty_bot) return;
    present_rows(dirty_top, dirty_bot);
    dirty_top = dirty_bot = 0;
}

void win_gfx_present_all(void) {
    present_rows(0, gfx_h);
    dirty_top = dirty_bot = 0;
}

void win_gfx_invalidate(void) {
    dirty_top = 0;
    dirty_bot = gfx_h;
}

void win_gfx_resize(int client_w, int client_h) {
    if (client_w == win_client_w && client_h == win_client_h) return;
    win_gfx_free();
    win_gfx_init(client_w, client_h);
}

// ---- the graphics_* API the engine and okai call ----------------------------

uint32_t* graphics_get_buffer(void) { return backbuf; }

HDC win_gfx_memdc(void) { return mem_dc; }

void graphics_flush(void) { win_gfx_present(); }

void graphics_set_clip(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > gfx_w) x1 = gfx_w;
    if (y1 > gfx_h) y1 = gfx_h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    cl_x0 = x0; cl_y0 = y0; cl_x1 = x1; cl_y1 = y1;
}

void graphics_clip_reset(void) { cl_x0 = 0; cl_y0 = 0; cl_x1 = gfx_w; cl_y1 = gfx_h; }

void graphics_get_clip(int* x0, int* y0, int* x1, int* y1) {
    *x0 = cl_x0; *y0 = cl_y0; *x1 = cl_x1; *y1 = cl_y1;
}

void graphics_write_pixel(int x, int y, uint32_t color) {
    if (x < cl_x0 || x >= cl_x1 || y < cl_y0 || y >= cl_y1) return;
    backbuf[y * gfx_w + x] = color;
    mark_rows(y, y + 1);
}

void putpixel(int x, int y, uint32_t color) { graphics_write_pixel(x, y, color); }

void hline(int x, int y, int len, uint32_t color) {
    int x0 = x < cl_x0 ? cl_x0 : x;
    int x1 = x + len > cl_x1 ? cl_x1 : x + len;
    if (y < cl_y0 || y >= cl_y1 || x0 >= x1) return;
    uint32_t* p = backbuf + y * gfx_w;
    for (int i = x0; i < x1; i++) p[i] = color;
    mark_rows(y, y + 1);
}

void vline(int x, int y, int len, uint32_t color) {
    if (x < cl_x0 || x >= cl_x1) return;
    int y0 = y < cl_y0 ? cl_y0 : y;
    int y1 = y + len > cl_y1 ? cl_y1 : y + len;
    if (y0 >= y1) return;
    for (int j = y0; j < y1; j++) backbuf[j * gfx_w + x] = color;
    mark_rows(y0, y1);
}

void line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        graphics_write_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void rect_fill(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < cl_x0 ? cl_x0 : x, x1 = x + w > cl_x1 ? cl_x1 : x + w;
    int y0 = y < cl_y0 ? cl_y0 : y, y1 = y + h > cl_y1 ? cl_y1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        uint32_t* p = backbuf + (size_t)j * gfx_w;
        for (int i = x0; i < x1; i++) p[i] = color;
    }
    mark_rows(y0, y1);
}

void rect_outline(int x, int y, int w, int h, uint32_t color, int thickness) {
    for (int t = 0; t < thickness; t++) {
        hline(x + t, y + t, w - 2 * t, color);
        hline(x + t, y + h - 1 - t, w - 2 * t, color);
        vline(x + t, y + t, h - 2 * t, color);
        vline(x + w - 1 - t, y + t, h - 2 * t, color);
    }
}

void gradient_fill(int x, int y, int w, int h, uint32_t c0, uint32_t c1, int vertical) {
    if (w <= 0 || h <= 0) return;
    int n = vertical ? h : w;
    for (int k = 0; k < n; k++) {
        int t = n > 1 ? k * 255 / (n - 1) : 0;
        uint32_t r = ((c0 >> 16 & 0xFF) * (255 - t) + (c1 >> 16 & 0xFF) * t) / 255;
        uint32_t g = ((c0 >> 8 & 0xFF) * (255 - t) + (c1 >> 8 & 0xFF) * t) / 255;
        uint32_t b = ((c0 & 0xFF) * (255 - t) + (c1 & 0xFF) * t) / 255;
        uint32_t c = (r << 16) | (g << 8) | b;
        if (vertical) hline(x, y + k, w, c);
        else vline(x + k, y, h, c);
    }
}

static int isqrt_i(int v) {
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

// Filled rect with rounded corners; the corner pixels outside the arc are
// left alone (the background shows through), like the kernel's version.
void round_rect_fill(int x, int y, int w, int h, uint32_t c, int r) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { rect_fill(x, y, w, h, c); return; }
    for (int j = 0; j < h; j++) {
        int dy = j < r ? r - 1 - j : (j >= h - r ? j - (h - r) : -1);
        int inset = 0;
        if (dy >= 0) inset = r - isqrt_i(r * r - dy * dy);
        hline(x + inset, y + j, w - 2 * inset, c);
    }
}

// Copy an opaque XRGB block, honoring the clip.
void graphics_blit_pixels(int x, int y, const uint32_t* src, int w, int h, int stride) {
    if (!src || w <= 0 || h <= 0) return;
    int sx = 0, sy = 0;
    int dx = x, dy = y;
    if (dx < cl_x0) { sx = cl_x0 - dx; dx = cl_x0; }
    if (dy < cl_y0) { sy = cl_y0 - dy; dy = cl_y0; }
    int x1 = x + w; if (x1 > cl_x1) x1 = cl_x1;
    int y1 = y + h; if (y1 > cl_y1) y1 = cl_y1;
    int n = x1 - dx;
    if (n <= 0) return;
    for (int j = dy; j < y1; j++, sy++) {
        memcpy(backbuf + (size_t)j * gfx_w + dx, src + (size_t)sy * stride + sx, (size_t)n * 4);
    }
    mark_rows(dy, y1);
}

void graphics_blit_rect(int sx, int sy, int w, int h, int dx, int dy) {
    if (w <= 0 || h <= 0) return;
    if (dy < sy) {
        for (int j = 0; j < h; j++)
            memmove(backbuf + (size_t)(dy + j) * gfx_w + dx,
                    backbuf + (size_t)(sy + j) * gfx_w + sx, (size_t)w * 4);
    } else {
        for (int j = h - 1; j >= 0; j--)
            memmove(backbuf + (size_t)(dy + j) * gfx_w + dx,
                    backbuf + (size_t)(sy + j) * gfx_w + sx, (size_t)w * 4);
    }
    mark_rows(dy < sy ? dy : sy, (dy < sy ? sy : dy) + h);
}

void graphics_fill(uint32_t color) {
    int n = gfx_w * gfx_h;
    for (int i = 0; i < n; i++) backbuf[i] = color;
    mark_rows(0, gfx_h);
}
