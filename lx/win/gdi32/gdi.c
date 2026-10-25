// gdi32: device contexts over pixel surfaces (window buffers, DIB sections, memory bitmaps), pens, brushes, fonts, drawing.
#include "gui.h"

extern LPVOID HeapAlloc(HANDLE h, DWORD f, SIZE_T n);
extern BOOL HeapFree(HANDLE h, DWORD f, LPVOID p);
extern HANDLE GetProcessHeap(void);
extern int MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR s, int sl, LPWSTR o, int oc);
extern int WideCharToMultiByte(UINT cp, DWORD fl, LPCWSTR w, int wl, LPSTR o, int oc, LPCSTR d, BOOL *u);

void *memcpy(void *d, const void *s, SIZE_T n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, SIZE_T n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
static SIZE_T slen(const char *s) { SIZE_T n = 0; while (s[n]) n++; return n; }
static void *gmalloc(SIZE_T n) { void *p = HeapAlloc(GetProcessHeap(), 8, n); return p; }
static void gfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

// ---- objects ----------------------------------------------------------------------------------------------------------------

struct surf { unsigned *px; int w, h, stride; };

struct gdiobj {
    int type;
    int stock;
    COLORREF color;                          // pen / brush colour
    int style, width;                        // pen style, width; brush style
    int fheight, fweight, fitalic, fmono, fserif, funder;     // font
    char face[32];
    struct surf bm; int bm_own; int bm_bpp; // bitmap (always held as 32-bit BGRX)
};

struct dc {
    int magic;
    struct surf s;
    struct gdiobj *pen, *brush, *font, *bmp;
    COLORREF tc, bc, dcbrush, dcpen;
    int bkmode, cx, cy, orgx, orgy, rop2;
    RECT clip; int has_clip;
    int own_surface;
    unsigned *saved_px;
};
#define DC_MAGIC 0x44435f32

static struct gdiobj stock[24];
static int stock_init;

static struct gdiobj *new_obj(int type) { struct gdiobj *o = gmalloc(sizeof *o); if (o) o->type = type; return o; }

static void init_stock(void) {
    if (stock_init) return;
    stock_init = 1;
    COLORREF cols[5] = { RGB(255, 255, 255), RGB(192, 192, 192), RGB(128, 128, 128), RGB(64, 64, 64), RGB(0, 0, 0) };
    for (int i = 0; i < 5; i++) { stock[i].type = OBJ_BRUSH; stock[i].stock = 1; stock[i].color = cols[i]; stock[i].style = BS_SOLID; }
    stock[NULL_BRUSH].type = OBJ_BRUSH; stock[NULL_BRUSH].stock = 1; stock[NULL_BRUSH].style = BS_NULL;
    stock[WHITE_PEN].type = OBJ_PEN; stock[WHITE_PEN].stock = 1; stock[WHITE_PEN].color = RGB(255, 255, 255); stock[WHITE_PEN].width = 1;
    stock[BLACK_PEN].type = OBJ_PEN; stock[BLACK_PEN].stock = 1; stock[BLACK_PEN].color = 0; stock[BLACK_PEN].width = 1;
    stock[NULL_PEN].type = OBJ_PEN; stock[NULL_PEN].stock = 1; stock[NULL_PEN].style = PS_NULL;
    for (int f = 11; f <= 17; f++) { stock[f].type = OBJ_FONT; stock[f].stock = 1; stock[f].fheight = -14; stock[f].fweight = 400; }
    stock[ANSI_FIXED_FONT].fmono = 1; stock[SYSTEM_FIXED_FONT].fmono = 1;
    stock[DC_BRUSH].type = OBJ_BRUSH; stock[DC_BRUSH].stock = 1; stock[DC_BRUSH].style = BS_SOLID; stock[DC_BRUSH].color = RGB(255, 255, 255);
    stock[DC_PEN].type = OBJ_PEN; stock[DC_PEN].stock = 1; stock[DC_PEN].width = 1;
}

EXPORT HGDIOBJ GetStockObject(int i) { init_stock(); return (i >= 0 && i < 24 && stock[i].type) ? &stock[i] : 0; }
EXPORT HBRUSH CreateSolidBrush(COLORREF c) { struct gdiobj *o = new_obj(OBJ_BRUSH); if (o) { o->color = c; o->style = BS_SOLID; } return o; }
EXPORT HBRUSH CreateHatchBrush(int h, COLORREF c) { (void)h; return CreateSolidBrush(c); }
EXPORT HBRUSH CreateBrushIndirect(const LOGBRUSH *lb) { return lb->lbStyle == BS_NULL ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(lb->lbColor); }
EXPORT HPEN CreatePen(int style, int width, COLORREF c) { struct gdiobj *o = new_obj(OBJ_PEN); if (o) { o->color = c; o->style = style; o->width = width < 1 ? 1 : width; } return o; }
EXPORT HPEN ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *lb, DWORD n, const DWORD *d) { (void)n; (void)d; return CreatePen((int)(style & 0xF), (int)width, lb->lbColor); }

static void font_from(struct gdiobj *o, LONG height, LONG weight, int italic, int under, const char *face) {
    o->fheight = height; o->fweight = weight ? weight : 400; o->fitalic = italic; o->funder = under;
    char lf[32]; int i = 0; for (; face[i] && i < 31; i++) lf[i] = (char)(face[i] >= 'A' && face[i] <= 'Z' ? face[i] + 32 : face[i]); lf[i] = 0;
    for (int k = 0; k < 31 && face[k]; k++) o->face[k] = face[k];
    const char *mono[] = { "courier", "consolas", "lucida console", "terminal", "fixedsys", "monaco", "mono", 0 };
    const char *serif[] = { "times", "georgia", "serif", "garamond", "cambria", 0 };
    for (int m = 0; mono[m]; m++) { SIZE_T l = slen(mono[m]); for (int k = 0; lf[k]; k++) { int ok = 1; for (SIZE_T j = 0; j < l; j++) if (lf[k + j] != mono[m][j]) { ok = 0; break; } if (ok) o->fmono = 1; } }
    for (int m = 0; serif[m]; m++) { SIZE_T l = slen(serif[m]); for (int k = 0; lf[k]; k++) { int ok = 1; for (SIZE_T j = 0; j < l; j++) if (lf[k + j] != serif[m][j]) { ok = 0; break; } if (ok && !o->fmono && !(lf[k] == 's' && lf[k + 1] == 'a')) o->fserif = 1; } }
}
EXPORT HFONT CreateFontA(int h, int w, int esc, int ori, int weight, DWORD it, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, LPCSTR face) {
    (void)w; (void)esc; (void)ori; (void)so; (void)cs; (void)op; (void)cp; (void)q; (void)pf;
    struct gdiobj *o = new_obj(OBJ_FONT); if (o) font_from(o, h, weight, (int)it, (int)ul, face ? face : ""); return o;
}
EXPORT HFONT CreateFontW(int h, int w, int esc, int ori, int weight, DWORD it, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, LPCWSTR face) {
    char f[64] = ""; if (face) WideCharToMultiByte(65001, 0, face, -1, f, sizeof f, 0, 0);
    return CreateFontA(h, w, esc, ori, weight, it, ul, so, cs, op, cp, q, pf, f);
}
EXPORT HFONT CreateFontIndirectA(const LOGFONTA *lf) { struct gdiobj *o = new_obj(OBJ_FONT); if (o) font_from(o, lf->lfHeight, lf->lfWeight, lf->lfItalic, lf->lfUnderline, lf->lfFaceName); return o; }
EXPORT HFONT CreateFontIndirectW(const LOGFONTW *lf) { char f[64] = ""; WideCharToMultiByte(65001, 0, lf->lfFaceName, -1, f, sizeof f, 0, 0); struct gdiobj *o = new_obj(OBJ_FONT); if (o) font_from(o, lf->lfHeight, lf->lfWeight, lf->lfItalic, lf->lfUnderline, f); return o; }

EXPORT BOOL DeleteObject(HGDIOBJ h) {
    struct gdiobj *o = h;
    if (!o || o->stock) return FALSE;
    if (o->type == OBJ_BITMAP && o->bm_own) gfree(o->bm.px);
    gfree(o);
    return TRUE;
}
EXPORT int GetObjectType(HGDIOBJ h) { return h ? ((struct gdiobj*)h)->type : 0; }
EXPORT int GetObjectA(HGDIOBJ h, int n, LPVOID out) {
    struct gdiobj *o = h;
    if (!o) return 0;
    if (o->type == OBJ_BITMAP && n >= (int)sizeof(BITMAP) && out) { BITMAP *b = out; memset(b, 0, sizeof *b); b->bmWidth = o->bm.w; b->bmHeight = o->bm.h; b->bmWidthBytes = o->bm.stride * 4; b->bmPlanes = 1; b->bmBitsPixel = 32; b->bmBits = o->bm.px; return sizeof *b; }
    if (o->type == OBJ_FONT && out && n >= (int)sizeof(LOGFONTA)) { LOGFONTA *l = out; memset(l, 0, sizeof *l); l->lfHeight = o->fheight; l->lfWeight = o->fweight; for (int i = 0; i < 31; i++) l->lfFaceName[i] = o->face[i]; return sizeof *l; }
    return 0;
}
EXPORT int GetObjectW(HGDIOBJ h, int n, LPVOID out) { return GetObjectA(h, n, out); }

// ---- surfaces and device contexts -------------------------------------------------------------------------------------------------

HDC __gdi_make_dc(unsigned *px, int w, int h, int stride) {
    init_stock();
    struct dc *d = gmalloc(sizeof *d);
    if (!d) return 0;
    d->magic = DC_MAGIC;
    d->s.px = px; d->s.w = w; d->s.h = h; d->s.stride = stride;
    d->pen = &stock[BLACK_PEN]; d->brush = &stock[WHITE_BRUSH]; d->font = &stock[DEFAULT_GUI_FONT]; d->bmp = 0;
    d->tc = 0; d->bc = RGB(255, 255, 255); d->bkmode = OPAQUE; d->rop2 = 13;
    d->clip.left = 0; d->clip.top = 0; d->clip.right = w; d->clip.bottom = h;
    return d;
}
void __gdi_dc_set_surface(HDC hdc, unsigned *px, int w, int h, int stride) {
    struct dc *d = hdc; d->s.px = px; d->s.w = w; d->s.h = h; d->s.stride = stride;
    d->clip.left = 0; d->clip.top = 0; d->clip.right = w; d->clip.bottom = h; d->has_clip = 0;
}
void __gdi_free_dc(HDC hdc) { gfree(hdc); }

EXPORT HDC CreateCompatibleDC(HDC ref) {
    (void)ref;
    HDC d = __gdi_make_dc(0, 1, 1, 1);                                    // a 1x1 monochrome bitmap is selected by default in Windows; here: nothing
    return d;
}
EXPORT BOOL DeleteDC(HDC hdc) { struct dc *d = hdc; if (!d || d->magic != DC_MAGIC) return FALSE; gfree(d); return TRUE; }
EXPORT int SaveDC(HDC hdc) { (void)hdc; return 1; }
EXPORT BOOL RestoreDC(HDC hdc, int n) { (void)hdc; (void)n; return TRUE; }

static struct gdiobj *new_bitmap(int w, int h, unsigned *px, int own) {
    struct gdiobj *o = new_obj(OBJ_BITMAP);
    if (!o) return 0;
    o->bm.w = w; o->bm.h = h; o->bm.stride = w; o->bm_own = own;
    o->bm.px = px ? px : gmalloc((SIZE_T)w * (SIZE_T)h * 4 + 4);
    if (!px) o->bm_own = 1;
    return o;
}
EXPORT HBITMAP CreateCompatibleBitmap(HDC hdc, int w, int h) {
    (void)hdc;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    struct gdiobj *o = new_bitmap(w, h, 0, 1);
    if (o) for (SIZE_T i = 0; i < (SIZE_T)w * (SIZE_T)h; i++) o->bm.px[i] = 0x00FFFFFF;
    return o;
}
EXPORT HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void *bits) {
    (void)planes; (void)bpp; (void)bits;
    return CreateCompatibleBitmap(0, w, h);
}
EXPORT HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE sec, DWORD off) {
    (void)hdc; (void)usage; (void)sec; (void)off;
    int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight; int top_down = h < 0; if (top_down) h = -h;
    struct gdiobj *o = new_bitmap(w, h, 0, 1);
    if (!o) return 0;
    o->bm_bpp = bi->bmiHeader.biBitCount;
    if (bits) *bits = o->bm.px;
    (void)top_down;
    return o;
}
EXPORT HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) {
    struct dc *d = hdc; struct gdiobj *o = h, *old = 0;
    if (!d || !o) return 0;
    switch (o->type) {
    case OBJ_PEN: old = d->pen; d->pen = o; break;
    case OBJ_BRUSH: old = d->brush; d->brush = o; break;
    case OBJ_FONT: old = d->font; d->font = o; break;
    case OBJ_BITMAP:
        old = d->bmp; d->bmp = o;
        d->s = o->bm; d->clip.left = 0; d->clip.top = 0; d->clip.right = o->bm.w; d->clip.bottom = o->bm.h;
        break;
    default: break;
    }
    return old;
}
EXPORT HGDIOBJ GetCurrentObject(HDC hdc, UINT type) { struct dc *d = hdc; return type == OBJ_PEN ? d->pen : type == OBJ_BRUSH ? d->brush : type == OBJ_FONT ? d->font : d->bmp; }

EXPORT COLORREF SetTextColor(HDC hdc, COLORREF c) { struct dc *d = hdc; COLORREF o = d->tc; d->tc = c; return o; }
EXPORT COLORREF GetTextColor(HDC hdc) { return ((struct dc*)hdc)->tc; }
EXPORT COLORREF SetBkColor(HDC hdc, COLORREF c) { struct dc *d = hdc; COLORREF o = d->bc; d->bc = c; return o; }
EXPORT COLORREF GetBkColor(HDC hdc) { return ((struct dc*)hdc)->bc; }
EXPORT int SetBkMode(HDC hdc, int m) { struct dc *d = hdc; int o = d->bkmode; d->bkmode = m; return o; }
EXPORT int GetBkMode(HDC hdc) { return ((struct dc*)hdc)->bkmode; }
EXPORT int SetROP2(HDC hdc, int r) { struct dc *d = hdc; int o = d->rop2; d->rop2 = r; return o; }
EXPORT COLORREF SetDCBrushColor(HDC hdc, COLORREF c) { (void)hdc; stock[DC_BRUSH].color = c; return c; }
EXPORT COLORREF SetDCPenColor(HDC hdc, COLORREF c) { (void)hdc; stock[DC_PEN].color = c; return c; }
EXPORT int SetMapMode(HDC hdc, int m) { (void)hdc; (void)m; return 1; }
EXPORT int GetMapMode(HDC hdc) { (void)hdc; return 1; }
EXPORT int SetStretchBltMode(HDC hdc, int m) { (void)hdc; (void)m; return 1; }
EXPORT UINT SetTextAlign(HDC hdc, UINT a) { (void)hdc; (void)a; return 0; }
EXPORT BOOL SetViewportOrgEx(HDC hdc, int x, int y, POINT *old) { struct dc *d = hdc; if (old) { old->x = d->orgx; old->y = d->orgy; } d->orgx = x; d->orgy = y; return TRUE; }
EXPORT BOOL SetWindowOrgEx(HDC hdc, int x, int y, POINT *old) { return SetViewportOrgEx(hdc, -x, -y, old); }
EXPORT BOOL GetViewportOrgEx(HDC hdc, POINT *p) { struct dc *d = hdc; p->x = d->orgx; p->y = d->orgy; return TRUE; }
EXPORT int GetDeviceCaps(HDC hdc, int i) {
    (void)hdc;
    switch (i) { case 8: return 1920; case 10: return 1080; case 12: return 32; case 14: return 1; case 88: case 90: return 96; case 24: return 1; case 104: return 256; case 2: return 1; default: return 0; }
}
EXPORT int SelectClipRgn(HDC hdc, void *rgn) { (void)hdc; (void)rgn; return 1; }
EXPORT int IntersectClipRect(HDC hdc, int l, int t, int r, int b) {
    struct dc *d = hdc;
    if (l + d->orgx > d->clip.left) d->clip.left = l + d->orgx;
    if (t + d->orgy > d->clip.top) d->clip.top = t + d->orgy;
    if (r + d->orgx < d->clip.right) d->clip.right = r + d->orgx;
    if (b + d->orgy < d->clip.bottom) d->clip.bottom = b + d->orgy;
    d->has_clip = 1;
    return 2;
}

// ---- pixels -----------------------------------------------------------------------------------------------------------------------

static inline unsigned bgr(COLORREF c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }     // COLORREF -> 0x00RRGGBB

static inline void put(struct dc *d, int x, int y, unsigned c) {
    x += d->orgx; y += d->orgy;
    if (x < d->clip.left || x >= d->clip.right || y < d->clip.top || y >= d->clip.bottom || !d->s.px) return;
    if (x >= d->s.w || y >= d->s.h) return;
    unsigned *p = &d->s.px[y * d->s.stride + x];
    switch (d->rop2) { case 7: *p ^= c; return; case 9: *p |= c; return; case 8: *p &= c; return; case 6: *p = ~*p & 0xFFFFFF; return; default: *p = c; }
}
static inline void blend(struct dc *d, int x, int y, unsigned c, unsigned a) {
    x += d->orgx; y += d->orgy;
    if (x < d->clip.left || x >= d->clip.right || y < d->clip.top || y >= d->clip.bottom || !d->s.px || x >= d->s.w || y >= d->s.h) return;
    unsigned *p = &d->s.px[y * d->s.stride + x];
    if (a >= 255) { *p = c; return; }
    unsigned dst = *p;
    unsigned rb = ((c & 0xFF00FF) * a + (dst & 0xFF00FF) * (255 - a) + 0x800080); rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    unsigned g = ((c & 0x00FF00) * a + (dst & 0x00FF00) * (255 - a) + 0x008000); g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    *p = rb | g;
}

static void fill_span(struct dc *d, int x0, int x1, int y, unsigned c) {
    y += d->orgy; x0 += d->orgx; x1 += d->orgx;
    if (y < d->clip.top || y >= d->clip.bottom || !d->s.px || y >= d->s.h) return;
    if (x0 < d->clip.left) x0 = d->clip.left;
    if (x1 > d->clip.right) x1 = d->clip.right;
    if (x1 > d->s.w) x1 = d->s.w;
    unsigned *p = &d->s.px[y * d->s.stride];
    if (d->rop2 == 13) for (int x = x0; x < x1; x++) p[x] = c;
    else for (int x = x0; x < x1; x++) { unsigned *q = &p[x]; if (d->rop2 == 7) *q ^= c; else if (d->rop2 == 9) *q |= c; else if (d->rop2 == 8) *q &= c; else *q = c; }
}
static void fill_box(struct dc *d, int x0, int y0, int x1, int y1, unsigned c) { for (int y = y0; y < y1; y++) fill_span(d, x0, x1, y, c); }

BOOL __gdi_rect_fill_color(HDC hdc, int x0, int y0, int x1, int y1, COLORREF c) { fill_box(hdc, x0, y0, x1, y1, bgr(c)); return TRUE; }

static int brush_visible(struct gdiobj *b) { return b && b->style != BS_NULL; }
static unsigned brush_color(struct dc *d, struct gdiobj *b) { return b == &stock[DC_BRUSH] ? bgr(stock[DC_BRUSH].color) : bgr(b->color); }
static int pen_visible(struct gdiobj *p) { return p && p->style != PS_NULL; }
static unsigned pen_color(struct gdiobj *p) { return p == &stock[DC_PEN] ? bgr(stock[DC_PEN].color) : bgr(p->color); }

int __gdi_FillRect(HDC hdc, const RECT *r, HBRUSH hb) {
    struct dc *d = hdc; struct gdiobj *b = hb;
    if (!b) return 0;
    if (b->style != BS_NULL) fill_box(d, r->left, r->top, r->right, r->bottom, brush_color(d, b));
    return 1;
}
int __gdi_FrameRect(HDC hdc, const RECT *r, HBRUSH hb) {
    struct dc *d = hdc; unsigned c = brush_color(d, hb);
    fill_box(d, r->left, r->top, r->right, r->top + 1, c); fill_box(d, r->left, r->bottom - 1, r->right, r->bottom, c);
    fill_box(d, r->left, r->top, r->left + 1, r->bottom, c); fill_box(d, r->right - 1, r->top, r->right, r->bottom, c);
    return 1;
}
// a window DC: the surface is the whole top-level buffer, origin = the window's client origin in it, clip given in surface coordinates
void __gdi_dc_setup(HDC hdc, unsigned *px, int w, int h, int stride, int orgx, int orgy, const RECT *clip) {
    struct dc *d = hdc;
    d->s.px = px; d->s.w = w; d->s.h = h; d->s.stride = stride; d->orgx = orgx; d->orgy = orgy;
    d->clip = *clip; d->has_clip = 1;
}
void __gdi_reset_dc(HDC hdc) {
    struct dc *d = hdc;
    d->pen = &stock[BLACK_PEN]; d->brush = &stock[WHITE_BRUSH]; d->font = &stock[DEFAULT_GUI_FONT]; d->bmp = 0;
    d->tc = 0; d->bc = RGB(255, 255, 255); d->bkmode = OPAQUE; d->rop2 = 13; d->cx = d->cy = 0;
}
int __gdi_is_dc(HDC hdc) { return hdc && ((struct dc*)hdc)->magic == DC_MAGIC; }

EXPORT BOOL PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop) {
    struct dc *d = hdc;
    unsigned c = rop == BLACKNESS ? 0 : rop == WHITENESS ? 0xFFFFFF : brush_color(d, d->brush);
    if (rop == DSTINVERT || rop == PATINVERT) { int old = d->rop2; d->rop2 = 7; fill_box(d, x, y, x + w, y + h, 0xFFFFFF); d->rop2 = old; return TRUE; }
    fill_box(d, x, y, x + w, y + h, c);
    return TRUE;
}
EXPORT COLORREF SetPixel(HDC hdc, int x, int y, COLORREF c) { put(hdc, x, y, bgr(c)); return c; }
EXPORT BOOL SetPixelV(HDC hdc, int x, int y, COLORREF c) { put(hdc, x, y, bgr(c)); return TRUE; }
EXPORT COLORREF GetPixel(HDC hdc, int x, int y) {
    struct dc *d = hdc; x += d->orgx; y += d->orgy;
    if (x < 0 || y < 0 || x >= d->s.w || y >= d->s.h || !d->s.px) return CLR_INVALID;
    unsigned p = d->s.px[y * d->s.stride + x];
    return ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
}

// lines: Bresenham; a pen wider than 1 stamps a square
static void plot_pen(struct dc *d, int x, int y, int w, unsigned c) {
    if (w <= 1) { put(d, x, y, c); return; }
    int h = w / 2;
    fill_box(d, x - h, y - h, x - h + w, y - h + w, c);
}
static void line(struct dc *d, int x0, int y0, int x1, int y1) {
    if (!pen_visible(d->pen)) return;
    unsigned c = pen_color(d->pen); int w = d->pen->width ? d->pen->width : 1;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy, n = 0;
    for (;;) {
        int on = 1;
        if (d->pen->style == PS_DASH) on = (n / 6) % 2 == 0; else if (d->pen->style == PS_DOT) on = (n / 2) % 2 == 0;
        if (on) plot_pen(d, x0, y0, w, c);
        n++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
EXPORT BOOL MoveToEx(HDC hdc, int x, int y, POINT *old) { struct dc *d = hdc; if (old) { old->x = d->cx; old->y = d->cy; } d->cx = x; d->cy = y; return TRUE; }
EXPORT BOOL LineTo(HDC hdc, int x, int y) { struct dc *d = hdc; line(d, d->cx, d->cy, x, y); d->cx = x; d->cy = y; return TRUE; }
EXPORT BOOL Polyline(HDC hdc, const POINT *p, int n) { struct dc *d = hdc; for (int i = 0; i + 1 < n; i++) line(d, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y); return TRUE; }

EXPORT BOOL Rectangle(HDC hdc, int l, int t, int r, int b) {
    struct dc *d = hdc;
    if (brush_visible(d->brush)) fill_box(d, l, t, r, b, brush_color(d, d->brush));
    if (pen_visible(d->pen)) { line(d, l, t, r - 1, t); line(d, r - 1, t, r - 1, b - 1); line(d, r - 1, b - 1, l, b - 1); line(d, l, b - 1, l, t); }
    return TRUE;
}
static int isqrt_(long long v) { long long r = 0; while ((r + 1) * (r + 1) <= v) r++; return (int)r; }
EXPORT BOOL Ellipse(HDC hdc, int l, int t, int r, int b) {
    struct dc *d = hdc;
    long long a = (r - l), bb = (b - t);
    if (a <= 0 || bb <= 0) return TRUE;
    // scanline fill and outline from the implicit equation, in doubled coordinates
    for (int y = t; y < b; y++) {
        long long dy2 = 2 * (long long)y + 1 - (t + b);                      // 2*(y+0.5 - cy)
        long long num = bb * bb - dy2 * dy2;
        if (num < 0) continue;
        long long half2 = (long long)a * isqrt_((num * 1000000) / ((long long)bb * bb)) / 1000;   // a * sqrt(1 - (dy/b)^2) in doubled units / 1
        int x0 = (l + r - (int)half2) / 2, x1 = (l + r + (int)half2 + 1) / 2;
        if (brush_visible(d->brush)) fill_span(d, x0, x1, y, brush_color(d, d->brush));
        if (pen_visible(d->pen)) { unsigned c = pen_color(d->pen); put(d, x0, y, c); put(d, x1 - 1, y, c); }
    }
    if (pen_visible(d->pen)) {                                                // top and bottom edges
        unsigned c = pen_color(d->pen);
        long long w1 = a / 4;
        fill_span(d, (l + r) / 2 - (int)w1, (l + r) / 2 + (int)w1, t, c);
        fill_span(d, (l + r) / 2 - (int)w1, (l + r) / 2 + (int)w1, b - 1, c);
    }
    return TRUE;
}
EXPORT BOOL RoundRect(HDC hdc, int l, int t, int r, int b, int w, int h) { (void)w; (void)h; return Rectangle(hdc, l, t, r, b); }
EXPORT BOOL Polygon(HDC hdc, const POINT *p, int n) {
    struct dc *d = hdc;
    if (n < 3) return FALSE;
    if (brush_visible(d->brush)) {
        int ymin = p[0].y, ymax = p[0].y;
        for (int i = 1; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
        unsigned c = brush_color(d, d->brush);
        for (int y = ymin; y <= ymax; y++) {
            int xs[64], nx = 0;
            for (int i = 0, j = n - 1; i < n && nx < 62; j = i++) {
                int yi = p[i].y, yj = p[j].y;
                if ((yi <= y && yj > y) || (yj <= y && yi > y)) xs[nx++] = p[i].x + (int)((long long)(y - yi) * (p[j].x - p[i].x) / (yj - yi));
            }
            for (int a = 1; a < nx; a++) { int v = xs[a], k = a - 1; while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; } xs[k + 1] = v; }
            for (int a = 0; a + 1 < nx; a += 2) fill_span(d, xs[a], xs[a + 1] + 1, y, c);
        }
    }
    if (pen_visible(d->pen)) for (int i = 0; i < n; i++) line(d, p[i].x, p[i].y, p[(i + 1) % n].x, p[(i + 1) % n].y);
    return TRUE;
}
EXPORT BOOL Arc(HDC hdc, int a, int b, int c, int d_, int e, int f, int g, int h) { (void)hdc; (void)a; (void)b; (void)c; (void)d_; (void)e; (void)f; (void)g; (void)h; return TRUE; }
EXPORT int GetROP2(HDC hdc) { return ((struct dc*)hdc)->rop2; }

// ---- bitmaps ------------------------------------------------------------------------------------------------------------------------

static void blit(struct dc *dd, int dx, int dy, int dw, int dh, struct dc *sd, int sx, int sy, int sw, int sh, DWORD rop) {
    if (rop == BLACKNESS || rop == WHITENESS || rop == PATCOPY || rop == PATINVERT || rop == DSTINVERT) { PatBlt(dd, dx, dy, dw, dh, rop); return; }
    if (!sd || !sd->s.px) return;
    for (int y = 0; y < dh; y++) {
        int syy = sh == dh ? sy + y : sy + (int)((long long)y * sh / dh);
        if (syy < 0 || syy >= sd->s.h) continue;
        for (int x = 0; x < dw; x++) {
            int sxx = sw == dw ? sx + x : sx + (int)((long long)x * sw / dw);
            if (sxx < 0 || sxx >= sd->s.w) continue;
            unsigned s = sd->s.px[syy * sd->s.stride + sxx] & 0xFFFFFF;
            int ox = dx + x + dd->orgx, oy = dy + y + dd->orgy;
            if (ox < dd->clip.left || ox >= dd->clip.right || oy < dd->clip.top || oy >= dd->clip.bottom || ox >= dd->s.w || oy >= dd->s.h) continue;
            unsigned *p = &dd->s.px[oy * dd->s.stride + ox];
            switch (rop) { case SRCAND: *p &= s; break; case SRCPAINT: *p |= s; break; case SRCINVERT: *p ^= s; break; case NOTSRCCOPY: *p = ~s & 0xFFFFFF; break; default: *p = s; }
        }
    }
}
EXPORT BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) { blit(dst, x, y, w, h, src, sx, sy, w, h, rop); return TRUE; }
EXPORT BOOL StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop) {
    if (w < 0) { w = -w; } if (h < 0) { h = -h; }
    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return FALSE;
    blit(dst, x, y, w, h, src, sx, sy, sw, sh, rop); return TRUE;
}
EXPORT BOOL MaskBlt(HDC a, int b, int c, int d, int e, HDC f, int g, int h, HBITMAP m, int i, int j, DWORD rop) { (void)m; (void)i; (void)j; return BitBlt(a, b, c, d, e, f, g, h, rop); }

static unsigned dib_pixel(const BITMAPINFO *bi, const unsigned char *bits, int x, int y, int w, int h_abs, int top_down) {
    int bpp = bi->bmiHeader.biBitCount;
    int row = top_down ? y : h_abs - 1 - y;
    int stride = ((w * bpp + 31) / 32) * 4;
    const unsigned char *p = bits + (SIZE_T)row * (SIZE_T)stride;
    if (bpp == 32) return *(const unsigned*)(p + x * 4) & 0xFFFFFF;
    if (bpp == 24) return (unsigned)p[x * 3 + 2] << 16 | (unsigned)p[x * 3 + 1] << 8 | p[x * 3];
    if (bpp == 8) { const RGBQUAD *q = &bi->bmiColors[p[x]]; return (unsigned)q->rgbRed << 16 | (unsigned)q->rgbGreen << 8 | q->rgbBlue; }
    if (bpp == 4) { int v = (x & 1) ? (p[x / 2] & 15) : (p[x / 2] >> 4); const RGBQUAD *q = &bi->bmiColors[v]; return (unsigned)q->rgbRed << 16 | (unsigned)q->rgbGreen << 8 | q->rgbBlue; }
    if (bpp == 1) { int v = (p[x / 8] >> (7 - (x & 7))) & 1; const RGBQUAD *q = &bi->bmiColors[v]; return (unsigned)q->rgbRed << 16 | (unsigned)q->rgbGreen << 8 | q->rgbBlue; }
    if (bpp == 16) { unsigned v = *(const unsigned short*)(p + x * 2); return ((v >> 10) & 31) * 255 / 31 << 16 | ((v >> 5) & 31) * 255 / 31 << 8 | (v & 31) * 255 / 31; }
    return 0;
}
EXPORT int StretchDIBits(HDC hdc, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, const void *bits, const BITMAPINFO *bi, UINT usage, DWORD rop) {
    (void)usage; (void)rop;
    struct dc *d = hdc;
    int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight, td = h < 0; int ha = td ? -h : h;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return 0;
    for (int y = 0; y < dh; y++) {
        int syy = sy + (int)((long long)y * sh / dh);
        if (syy < 0 || syy >= ha) continue;
        for (int x = 0; x < dw; x++) {
            int sxx = sx + (int)((long long)x * sw / dw);
            if (sxx < 0 || sxx >= w) continue;
            put(d, dx + x, dy + y, dib_pixel(bi, bits, sxx, syy, w, ha, td));
        }
    }
    return dh;
}
EXPORT int SetDIBitsToDevice(HDC hdc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines, const void *bits, const BITMAPINFO *bi, UINT usage) {
    (void)start; (void)lines; return StretchDIBits(hdc, x, y, (int)w, (int)h, sx, sy, (int)w, (int)h, bits, bi, usage, SRCCOPY);
}
EXPORT int GetDIBits(HDC hdc, HBITMAP hb, UINT start, UINT n, LPVOID bits, BITMAPINFO *bi, UINT usage) {
    (void)hdc; (void)start; (void)usage;
    struct gdiobj *o = hb;
    if (!o) return 0;
    if (bi && !bits) { bi->bmiHeader.biWidth = o->bm.w; bi->bmiHeader.biHeight = o->bm.h; bi->bmiHeader.biBitCount = 32; bi->bmiHeader.biPlanes = 1; return o->bm.h; }
    if (bits && bi) {
        int w = o->bm.w;
        for (UINT y = 0; y < n && y < (UINT)o->bm.h; y++) for (int x = 0; x < w; x++) ((unsigned*)bits)[y * w + x] = o->bm.px[(o->bm.h - 1 - y) * o->bm.stride + x];
        return (int)n;
    }
    return 0;
}


// an ARGB (non-premultiplied) image scaled to dw x dh and blended at (x, y): icons
BOOL __gdi_blend_argb(HDC hdc, int x, int y, int dw, int dh, const unsigned *argb, int sw, int sh) {
    struct dc *d = hdc;
    if (!d || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return FALSE;
    for (int j = 0; j < dh; j++) {
        int sy = sh == dh ? j : j * sh / dh;
        for (int i = 0; i < dw; i++) {
            int sx = sw == dw ? i : i * sw / dw;
            unsigned p = argb[sy * sw + sx], a = p >> 24;
            if (a) blend(d, x + i, y + j, p & 0xFFFFFF, a);
        }
    }
    return TRUE;
}

// ---- text ---------------------------------------------------------------------------------------------------------------------------------

static int okw_fd = -2;
static int get_okw(void) { if (okw_fd == -2) okw_fd = (int)SC3(LX_open, "/dev/okwin", O_RDWR, 0); return okw_fd; }

typedef unsigned long long u64_;
struct tx { u64_ str; int len, size, flags, family; u64_ mask; int cap_w, cap_h, out_w, out_asc, out_desc, pad; };

static int font_px(struct gdiobj *f) {
    int h = f->fheight;
    int px = h < 0 ? -h : (h * 82 + 50) / 100;
    if (px < 6) px = 6;
    if (px > 200) px = 200;
    return px;
}

static int run_text(struct dc *d, const char *s, int len, int x, int y, int draw, int *w_out, int *asc_out, int *desc_out) {
    struct gdiobj *f = d->font ? d->font : &stock[DEFAULT_GUI_FONT];
    int fd = get_okw();
    if (w_out) *w_out = 0;
    if (asc_out) *asc_out = 12;
    if (desc_out) *desc_out = 4;
    if (fd < 0 || len <= 0) return 0;
    struct tx t;
    memset(&t, 0, sizeof t);
    t.str = (u64_)s; t.len = len > 1000 ? 1000 : len; t.size = font_px(f);
    t.flags = (f->fweight >= 600 ? 1 : 0) | (f->fitalic ? 2 : 0); t.family = f->fmono ? 2 : f->fserif ? 1 : 0;
    t.flags |= 4;                                                          // measure only
    if (SC3(LX_ioctl, fd, OKW_TEXT, &t) < 0) return 0;
    int w = t.out_w, asc = t.out_asc, desc = t.out_desc;
    if (w_out) *w_out = w;
    if (asc_out) *asc_out = asc;
    if (desc_out) *desc_out = desc;
    if (!draw) return w;
    int cw = w + 4, ch = asc + desc + 2;
    if (cw > 1024) cw = 1024;
    if (ch > 256) ch = 256;
    unsigned char *mask = gmalloc((SIZE_T)cw * (SIZE_T)ch);
    if (!mask) return w;
    t.flags &= ~4; t.mask = (u64_)mask; t.cap_w = cw; t.cap_h = ch;
    if (SC3(LX_ioctl, fd, OKW_TEXT, &t) == 0) {
        if (d->bkmode == OPAQUE) fill_box(d, x, y, x + w, y + asc + desc, bgr(d->bc));
        unsigned c = bgr(d->tc);
        for (int yy = 0; yy < ch; yy++) for (int xx = 0; xx < cw; xx++) { unsigned a = mask[yy * cw + xx]; if (a) blend(d, x + xx, y + yy, c, a); }
        if (f->funder) fill_box(d, x, y + asc + 1, x + w, y + asc + 2, c);
    }
    gfree(mask);
    return w;
}

EXPORT BOOL TextOutA(HDC hdc, int x, int y, LPCSTR s, int n) { int w; run_text(hdc, s, n, x, y, 1, &w, 0, 0); return TRUE; }
EXPORT BOOL TextOutW(HDC hdc, int x, int y, LPCWSTR s, int n) {
    char u[2048]; int l = WideCharToMultiByte(65001, 0, s, n, u, sizeof u, 0, 0);
    return TextOutA(hdc, x, y, u, l > 0 ? l : 0);
}
EXPORT BOOL ExtTextOutA(HDC hdc, int x, int y, UINT opts, const RECT *r, LPCSTR s, UINT n, const int *dx) {
    (void)dx;
    struct dc *d = hdc;
    if ((opts & 2) && r) fill_box(d, r->left, r->top, r->right, r->bottom, bgr(d->bc));            // ETO_OPAQUE
    if (s && n) { int bk = d->bkmode; d->bkmode = TRANSPARENT; TextOutA(hdc, x, y, s, (int)n); d->bkmode = bk; }
    return TRUE;
}
EXPORT BOOL ExtTextOutW(HDC hdc, int x, int y, UINT opts, const RECT *r, LPCWSTR s, UINT n, const int *dx) {
    char u[2048]; int l = s ? WideCharToMultiByte(65001, 0, s, (int)n, u, sizeof u, 0, 0) : 0;
    return ExtTextOutA(hdc, x, y, opts, r, u, (UINT)(l > 0 ? l : 0), dx);
}
EXPORT BOOL GetTextExtentPoint32A(HDC hdc, LPCSTR s, int n, SIZE *sz) {
    int w, a, de;
    run_text(hdc, s, n, 0, 0, 0, &w, &a, &de);
    sz->cx = w; sz->cy = a + de;
    return TRUE;
}
EXPORT BOOL GetTextExtentPoint32W(HDC hdc, LPCWSTR s, int n, SIZE *sz) {
    char u[2048]; int l = WideCharToMultiByte(65001, 0, s, n, u, sizeof u, 0, 0);
    return GetTextExtentPoint32A(hdc, u, l > 0 ? l : 0, sz);
}
EXPORT BOOL GetTextExtentPointA(HDC hdc, LPCSTR s, int n, SIZE *sz) { return GetTextExtentPoint32A(hdc, s, n, sz); }
EXPORT BOOL GetTextExtentPointW(HDC hdc, LPCWSTR s, int n, SIZE *sz) { return GetTextExtentPoint32W(hdc, s, n, sz); }
EXPORT BOOL GetTextMetricsA(HDC hdc, TEXTMETRICA *tm) {
    int w, a, de, cw;
    run_text(hdc, "x", 1, 0, 0, 0, &cw, &a, &de);
    run_text(hdc, "W", 1, 0, 0, 0, &w, &a, &de);
    struct gdiobj *f = ((struct dc*)hdc)->font;
    memset(tm, 0, sizeof *tm);
    tm->tmHeight = a + de; tm->tmAscent = a; tm->tmDescent = de; tm->tmAveCharWidth = cw; tm->tmMaxCharWidth = w; tm->tmWeight = f ? f->fweight : 400;
    tm->tmFirstChar = 32; tm->tmLastChar = 126; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' '; tm->tmPitchAndFamily = 0x16; tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
    return TRUE;
}
EXPORT BOOL GetTextMetricsW(HDC hdc, TEXTMETRICW *tm) {
    TEXTMETRICA a; GetTextMetricsA(hdc, &a);
    memset(tm, 0, sizeof *tm);
    tm->tmHeight = a.tmHeight; tm->tmAscent = a.tmAscent; tm->tmDescent = a.tmDescent; tm->tmAveCharWidth = a.tmAveCharWidth; tm->tmMaxCharWidth = a.tmMaxCharWidth; tm->tmWeight = a.tmWeight;
    tm->tmFirstChar = 32; tm->tmLastChar = 126; tm->tmDefaultChar = '?'; tm->tmBreakChar = ' '; tm->tmPitchAndFamily = a.tmPitchAndFamily;
    return TRUE;
}
EXPORT BOOL GetCharWidth32A(HDC hdc, UINT a, UINT b, int *out) { for (UINT c = a; c <= b; c++) { char ch = (char)c; int w, as, de; run_text(hdc, &ch, 1, 0, 0, 0, &w, &as, &de); out[c - a] = w; } return TRUE; }
EXPORT int EnumFontFamiliesA(HDC hdc, LPCSTR f, void *cb, LPARAM l) { (void)hdc; (void)f; (void)cb; (void)l; return 0; }
EXPORT DWORD GetGlyphOutlineA(HDC h, UINT c, UINT f, void *g, DWORD n, void *b, const void *m) { (void)h; (void)c; (void)f; (void)g; (void)n; (void)b; (void)m; return 0xFFFFFFFFu; }
EXPORT int AddFontResourceA(LPCSTR f) { (void)f; return 0; }
EXPORT HGDIOBJ CreatePalette(const void *p) { (void)p; return GetStockObject(WHITE_BRUSH); }
EXPORT HGDIOBJ SelectPalette(HDC d, HGDIOBJ p, BOOL f) { (void)d; (void)p; (void)f; return 0; }
EXPORT UINT RealizePalette(HDC d) { (void)d; return 0; }
EXPORT int GdiFlush(void) { return 1; }
EXPORT COLORREF GetNearestColor(HDC h, COLORREF c) { (void)h; return c; }
EXPORT HGDIOBJ CreateRectRgn(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return new_obj(OBJ_REGION); }
EXPORT int CombineRgn(HGDIOBJ d, HGDIOBJ a, HGDIOBJ b, int m) { (void)d; (void)a; (void)b; (void)m; return 2; }

BOOL DllMain(HMODULE m, DWORD reason, LPVOID res) { (void)m; (void)reason; (void)res; return TRUE; }
