// user32: bitmaps, icons and cursors from resources and files (LoadBitmap, LoadIcon, LoadImage, DrawIconEx, CreateIconIndirect ...)
#include "u32.h"

extern HRSRC FindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang);
extern HGLOBAL LoadResource(HMODULE m, HRSRC r);
extern DWORD SizeofResource(HMODULE m, HRSRC r);
extern HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE sec, DWORD off);
extern int GetObjectA(HGDIOBJ h, int n, LPVOID out);
extern HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h);

#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define LR_LOADFROMFILE 0x10
#define LR_DEFAULTSIZE 0x40
#define DI_MASK 1
#define DI_IMAGE 2
#define DI_NORMAL 3
#define DI_DEFAULTSIZE 8

#define ICON_MAGIC 0x49434F4E
struct icon { int magic, w, h, hotx, hoty, cursor; unsigned *argb; };
typedef struct { BOOL fIcon; DWORD xHotspot, yHotspot; HBITMAP hbmMask, hbmColor; } ICONINFO;

extern BOOL __gdi_blend_argb(HDC dc, int x, int y, int dw, int dh, const unsigned *argb, int sw, int sh);

static struct icon *icon_of(HANDLE h) { struct icon *i = h; return ((ULONG_PTR)h > 0x10000 && i->magic == ICON_MAGIC) ? i : 0; }

// ---- DIB decoding ----------------------------------------------------------------------------------------------------------------

// Decode the DIB at `bih` (its palette follows; the pixel rows follow the palette unless `bits` is given) into top-down ARGB.
// icon: the height counts the AND mask too, which follows the colour rows. Returns the pixel array (umalloc) or 0.
static unsigned *decode_dib(const BYTE *bih, const BYTE *bits, int icon, int *ow, int *oh) {
    DWORD hs = *(const DWORD*)bih;
    int w, h, bpp, comp = 0;
    DWORD clr = 0;
    if (hs == 12) { w = *(const WORD*)(bih + 4); h = *(const WORD*)(bih + 6); bpp = *(const WORD*)(bih + 10); }
    else { w = *(const int*)(bih + 4); h = *(const int*)(bih + 8); bpp = *(const WORD*)(bih + 14); comp = (int)*(const DWORD*)(bih + 16); clr = *(const DWORD*)(bih + 32); }
    int top_down = h < 0;
    if (h < 0) h = -h;
    if (icon) h /= 2;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    if (comp != 0 && comp != 3) return 0;                                      // no RLE / JPEG / PNG
    const BYTE *pal = bih + hs + ((comp == 3 && hs == 40) ? 12 : 0);
    DWORD npal = bpp <= 8 ? (clr ? clr : 1u << bpp) : 0;
    DWORD rmask = 0xFF0000, gmask = 0xFF00, bmask = 0xFF;
    if (comp == 3 && hs >= 40) { rmask = *(const DWORD*)(bih + hs); gmask = *(const DWORD*)(bih + hs + 4); bmask = *(const DWORD*)(bih + hs + 8); }
    const BYTE *px = bits ? bits : pal + npal * (hs == 12 ? 3 : 4);
    unsigned *out = umalloc((SIZE_T)w * (SIZE_T)h * 4);
    if (!out) return 0;
    int stride = ((w * bpp + 31) / 32) * 4;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const BYTE *row = px + (SIZE_T)(top_down ? y : h - 1 - y) * (SIZE_T)stride;
        unsigned *dst = out + y * w;
        for (int x = 0; x < w; x++) {
            unsigned c = 0, a = 255;
            if (bpp == 32) {
                unsigned v = *(const unsigned*)(row + x * 4);
                if (comp == 3) { c = ((((v & rmask) >> 16) & 0xFF) << 16) | (v & gmask & 0xFF00) | (v & bmask & 0xFF); }
                else { c = v & 0xFFFFFF; a = v >> 24; if (a) any_alpha = 1; }
            } else if (bpp == 24) c = (unsigned)row[x * 3 + 2] << 16 | (unsigned)row[x * 3 + 1] << 8 | row[x * 3];
            else if (bpp == 16) { unsigned v = *(const WORD*)(row + x * 2); c = ((v >> 10) & 31) * 255 / 31 << 16 | ((v >> 5) & 31) * 255 / 31 << 8 | (v & 31) * 255 / 31; }
            else {
                unsigned idx = bpp == 8 ? row[x] : bpp == 4 ? ((x & 1) ? row[x / 2] & 15 : row[x / 2] >> 4) : (row[x / 8] >> (7 - (x & 7))) & 1;
                if (idx >= npal) idx = 0;
                const BYTE *e = pal + idx * (hs == 12 ? 3 : 4);
                c = (unsigned)e[2] << 16 | (unsigned)e[1] << 8 | e[0];
            }
            dst[x] = (a << 24) | c;
        }
    }
    if (icon) {                                                              // the AND mask: 1 = transparent (unless the colours carry their own alpha)
        const BYTE *mask = px + (SIZE_T)stride * (SIZE_T)h;
        int mstride = ((w + 31) / 32) * 4;
        for (int y = 0; y < h; y++) {
            const BYTE *row = mask + (SIZE_T)(top_down ? y : h - 1 - y) * (SIZE_T)mstride;
            for (int x = 0; x < w; x++) {
                int transparent = (row[x / 8] >> (7 - (x & 7))) & 1;
                if (bpp == 32 && any_alpha) continue;
                out[y * w + x] = (out[y * w + x] & 0xFFFFFF) | (transparent ? 0 : 0xFF000000u);
            }
        }
    } else if (!any_alpha) for (int i = 0; i < w * h; i++) out[i] |= 0xFF000000u;
    *ow = w; *oh = h;
    return out;
}

static HANDLE make_icon(unsigned *argb, int w, int h, int cursor, int hx, int hy) {
    struct icon *i = umalloc(sizeof *i);
    if (!i) { ufree(argb); return 0; }
    i->magic = ICON_MAGIC; i->w = w; i->h = h; i->argb = argb; i->cursor = cursor; i->hotx = hx; i->hoty = hy;
    return i;
}

// an icon directory entry (ICONDIRENTRY in a file, GRPICONDIRENTRY in a resource): choose the one closest to want x want
struct dirent { int w, h, bpp; DWORD bytes, where; };
static int pick(const struct dirent *e, int n, int want) {
    int best = -1, score = 1 << 30;
    for (int i = 0; i < n; i++) {
        int d = (e[i].w > want ? e[i].w - want : want - e[i].w) * 1000 - e[i].bpp;
        if (d < score) { score = d; best = i; }
    }
    return best;
}

// ---- from resources -------------------------------------------------------------------------------------------------------------------

static HANDLE load_icon_res(HINSTANCE h, LPCWSTR name, int want, int cursor) {
    HRSRC g = FindResourceExW(h, (LPCWSTR)(ULONG_PTR)(cursor ? 12 : 14), name, 0);              // RT_GROUP_CURSOR / RT_GROUP_ICON
    if (!g) return 0;
    const BYTE *d = LoadResource(h, g);
    int n = *(const WORD*)(d + 4);
    struct dirent e[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        const BYTE *p = d + 6 + i * 14;
        e[i].w = p[0] ? p[0] : 256; e[i].h = p[1] ? p[1] : 256; e[i].bpp = *(const WORD*)(p + 6); e[i].bytes = *(const DWORD*)(p + 8); e[i].where = *(const WORD*)(p + 12);
        if (cursor) { e[i].w = *(const WORD*)p; e[i].h = *(const WORD*)(p + 2) / 2; }
    }
    int tried = 0;
    while (tried++ < n) {
        int k = pick(e, n, want);
        if (k < 0) return 0;
        HRSRC r = FindResourceExW(h, (LPCWSTR)(ULONG_PTR)(cursor ? 1 : 3), (LPCWSTR)(ULONG_PTR)e[k].where, 0);
        if (r) {
            const BYTE *img = LoadResource(h, r);
            int hx = 0, hy = 0;
            if (cursor) { hx = *(const WORD*)img; hy = *(const WORD*)(img + 2); img += 4; }
            int w, hh;
            unsigned *argb = decode_dib(img, 0, 1, &w, &hh);
            if (argb) return make_icon(argb, w, hh, cursor, hx, hy);
        }
        e[k].w = 100000;                                                    // that one cannot be used (PNG, RLE...): the next best
    }
    return 0;
}

EXPORT HICON LoadIconW(HINSTANCE h, LPCWSTR name) {
    if (!h && (ULONG_PTR)name < 0x10000) return (HICON)(0x3000 + (ULONG_PTR)name);               // IDI_APPLICATION & co: a stand-in handle
    HICON i = load_icon_res(h, name, 32, 0);
    return i ? i : (HICON)0x3001;
}
EXPORT HICON LoadIconA(HINSTANCE h, LPCSTR name) {
    WCHAR w[64];
    if ((ULONG_PTR)name < 0x10000) return LoadIconW(h, (LPCWSTR)name);
    MultiByteToWideChar(65001, 0, name, -1, w, 64);
    return LoadIconW(h, w);
}
EXPORT HBITMAP LoadBitmapW(HINSTANCE h, LPCWSTR name) {
    HRSRC r = FindResourceExW(h, (LPCWSTR)(ULONG_PTR)2, name, 0);                                // RT_BITMAP
    if (!r) return 0;
    int w, hh;
    unsigned *argb = decode_dib(LoadResource(h, r), 0, 0, &w, &hh);
    if (!argb) return 0;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = 0;
    HBITMAP bm = CreateDIBSection(0, &bi, 0, &bits, 0, 0);
    if (bm && bits) for (int i = 0; i < w * hh; i++) ((unsigned*)bits)[i] = argb[i] & 0xFFFFFF;
    ufree(argb);
    return bm;
}
EXPORT HBITMAP LoadBitmapA(HINSTANCE h, LPCSTR name) {
    WCHAR w[64];
    if ((ULONG_PTR)name < 0x10000) return LoadBitmapW(h, (LPCWSTR)name);
    MultiByteToWideChar(65001, 0, name, -1, w, 64);
    return LoadBitmapW(h, w);
}

// ---- from files ---------------------------------------------------------------------------------------------------------------------------

static BYTE *read_file(const char *path, long *size) {
    long fd = SC3(LX_open, path, O_RDONLY, 0);
    if (fd < 0) return 0;
    long end = SC3(LX_lseek, fd, 0, 2);
    SC3(LX_lseek, fd, 0, 0);
    BYTE *b = end > 0 && end < (64 << 20) ? umalloc((SIZE_T)end) : 0;
    long got = 0;
    while (b && got < end) { long r = SC3(LX_read, fd, b + got, end - got); if (r <= 0) break; got += r; }
    SC1(LX_close, fd);
    if (!b || got < end) { ufree(b); return 0; }
    *size = end;
    return b;
}
static void win_to_linux(const char *w, char *out, int cap) {
    int n = 0;
    const char *p = w;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
    for (; *p && n < cap - 1; p++) out[n++] = *p == 92 ? '/' : *p;
    out[n] = 0;
}
static HANDLE load_ico_file(const BYTE *f, long size, int want, int cursor) {
    if (size < 22 || *(const WORD*)(f + 2) != (cursor ? 2 : 1)) return 0;
    int n = *(const WORD*)(f + 4);
    struct dirent e[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++) {
        const BYTE *p = f + 6 + i * 16;
        e[i].w = p[0] ? p[0] : 256; e[i].h = p[1] ? p[1] : 256; e[i].bpp = *(const WORD*)(p + 6); e[i].bytes = *(const DWORD*)(p + 8); e[i].where = *(const DWORD*)(p + 12);
    }
    for (int tried = 0; tried < n; tried++) {
        int k = pick(e, n, want);
        if (k < 0 || e[k].where + e[k].bytes > (DWORD)size) return 0;
        int w, h;
        unsigned *argb = decode_dib(f + e[k].where, 0, 1, &w, &h);
        if (argb) return make_icon(argb, w, h, cursor, cursor ? *(const WORD*)(f + 6 + k * 16 + 4) : 0, cursor ? *(const WORD*)(f + 6 + k * 16 + 6) : 0);
        e[k].w = 100000;
    }
    return 0;
}
EXPORT HANDLE LoadImageW(HINSTANCE h, LPCWSTR name, UINT type, int cx, int cy, UINT flags) {
    (void)cy;
    int want = cx ? cx : 32;
    if (flags & LR_LOADFROMFILE) {
        char u[260], p[260]; long size;
        WideCharToMultiByte(65001, 0, name, -1, u, sizeof u, 0, 0);
        win_to_linux(u, p, sizeof p);
        BYTE *f = read_file(p, &size);
        if (!f) return 0;
        HANDLE r = 0;
        if (type == IMAGE_BITMAP && size > 54 && f[0] == 'B' && f[1] == 'M') {
            int w, hh;
            unsigned *argb = decode_dib(f + 14, f + *(const DWORD*)(f + 10), 0, &w, &hh);
            if (argb) {
                BITMAPINFO bi; memset(&bi, 0, sizeof bi);
                bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
                void *bits = 0;
                r = CreateDIBSection(0, &bi, 0, &bits, 0, 0);
                if (r && bits) for (int i = 0; i < w * hh; i++) ((unsigned*)bits)[i] = argb[i] & 0xFFFFFF;
                ufree(argb);
            }
        } else if (type == IMAGE_ICON || type == IMAGE_CURSOR) r = load_ico_file(f, size, want, type == IMAGE_CURSOR);
        ufree(f);
        return r;
    }
    if (type == IMAGE_BITMAP) return LoadBitmapW(h, name);
    if (type == IMAGE_ICON) { HANDLE i = load_icon_res(h, name, want, 0); return i ? i : (HANDLE)0x3001; }
    return (HANDLE)0x1000;                                                       // cursors are not shown: any handle
}
EXPORT HANDLE LoadImageA(HINSTANCE h, LPCSTR name, UINT type, int cx, int cy, UINT flags) {
    WCHAR w[260];
    if ((ULONG_PTR)name < 0x10000) return LoadImageW(h, (LPCWSTR)name, type, cx, cy, flags);
    MultiByteToWideChar(65001, 0, name, -1, w, 260);
    return LoadImageW(h, w, type, cx, cy, flags);
}

// ---- drawing and creating --------------------------------------------------------------------------------------------------------------------

EXPORT BOOL DrawIconEx(HDC dc, int x, int y, HICON hi, int cx, int cy, UINT step, HBRUSH br, UINT flags) {
    (void)step; (void)br;
    struct icon *i = icon_of(hi);
    if (!i || !(flags & DI_IMAGE)) return TRUE;                                  // stand-in handles draw nothing
    if (!cx || (flags & DI_DEFAULTSIZE)) cx = i->w;
    if (!cy || (flags & DI_DEFAULTSIZE)) cy = i->h;
    return __gdi_blend_argb(dc, x, y, cx, cy, i->argb, i->w, i->h);
}
EXPORT BOOL DrawIcon(HDC dc, int x, int y, HICON hi) { return DrawIconEx(dc, x, y, hi, 0, 0, 0, 0, DI_NORMAL); }
EXPORT BOOL DestroyIcon(HICON hi) { struct icon *i = icon_of(hi); if (i) { i->magic = 0; ufree(i->argb); ufree(i); } return TRUE; }
EXPORT BOOL DestroyCursor(HCURSOR c) { return DestroyIcon(c); }
EXPORT HICON CopyIcon(HICON hi) {
    struct icon *i = icon_of(hi);
    if (!i) return hi;
    unsigned *a = umalloc((SIZE_T)i->w * (SIZE_T)i->h * 4);
    if (!a) return 0;
    memcpy(a, i->argb, (SIZE_T)i->w * (SIZE_T)i->h * 4);
    return make_icon(a, i->w, i->h, i->cursor, i->hotx, i->hoty);
}
EXPORT HICON CreateIconIndirect(const ICONINFO *ii) {
    BITMAP bc, bm;
    memset(&bc, 0, sizeof bc); memset(&bm, 0, sizeof bm);
    if (!ii->hbmColor || !GetObjectA(ii->hbmColor, sizeof bc, &bc) || !bc.bmBits) return 0;
    int hasmask = ii->hbmMask && GetObjectA(ii->hbmMask, sizeof bm, &bm) && bm.bmBits && bm.bmWidth == bc.bmWidth;
    unsigned *a = umalloc((SIZE_T)bc.bmWidth * (SIZE_T)bc.bmHeight * 4);
    if (!a) return 0;
    for (int k = 0; k < bc.bmWidth * bc.bmHeight; k++) {
        unsigned c = ((unsigned*)bc.bmBits)[k] & 0xFFFFFF;
        unsigned alpha = 255;
        if (hasmask && bm.bmHeight >= bc.bmHeight) alpha = (((unsigned*)bm.bmBits)[k] & 0xFFFFFF) ? 0 : 255;
        a[k] = (alpha << 24) | c;
    }
    return make_icon(a, bc.bmWidth, bc.bmHeight, !ii->fIcon, (int)ii->xHotspot, (int)ii->yHotspot);
}
EXPORT BOOL GetIconInfo(HICON hi, ICONINFO *out) {
    struct icon *i = icon_of(hi);
    memset(out, 0, sizeof *out);
    if (!i) return FALSE;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = i->w; bi.bmiHeader.biHeight = -i->h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *cb = 0, *mb = 0;
    out->hbmColor = CreateDIBSection(0, &bi, 0, &cb, 0, 0);
    out->hbmMask = CreateDIBSection(0, &bi, 0, &mb, 0, 0);
    for (int k = 0; k < i->w * i->h && cb && mb; k++) { ((unsigned*)cb)[k] = i->argb[k] & 0xFFFFFF; ((unsigned*)mb)[k] = (i->argb[k] >> 24) ? 0 : 0xFFFFFF; }
    out->fIcon = !i->cursor; out->xHotspot = (DWORD)i->hotx; out->yHotspot = (DWORD)i->hoty;
    return TRUE;
}
EXPORT HICON CreateIcon(HINSTANCE h, int w, int hh, BYTE planes, BYTE bpp, const BYTE *andbits, const BYTE *xorbits) {
    (void)h; (void)planes;
    unsigned *a = umalloc((SIZE_T)w * (SIZE_T)hh * 4);
    if (!a) return 0;
    int astride = ((w + 15) / 16) * 2;                                          // CreateIcon rows are WORD aligned, top-down
    int xstride = ((w * bpp + 15) / 16) * 2;
    for (int y = 0; y < hh; y++) for (int x = 0; x < w; x++) {
        unsigned c = 0;
        if (bpp == 32) c = ((const unsigned*)(xorbits + y * xstride))[x] & 0xFFFFFF;
        else if (bpp == 1) c = ((xorbits[y * xstride + x / 8] >> (7 - (x & 7))) & 1) ? 0xFFFFFF : 0;
        int transparent = andbits ? (andbits[y * astride + x / 8] >> (7 - (x & 7))) & 1 : 0;
        a[y * w + x] = (transparent ? 0 : 0xFF000000u) | c;
    }
    return make_icon(a, w, hh, 0, 0, 0);
}
EXPORT HANDLE CopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags) { (void)type; (void)cx; (void)cy; (void)flags; return type == IMAGE_ICON ? CopyIcon(h) : h; }
