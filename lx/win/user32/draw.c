// user32: painting (BeginPaint, GetDC, invalidation), DrawText and friends, rectangles, system colours and metrics, wsprintf
#include "u32.h"

void *memset(void *d, int c, SIZE_T n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, SIZE_T n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }

// ---- colours --------------------------------------------------------------------------------------------------------------------------

static const COLORREF sys_colors[31] = {
    RGB(200, 200, 200), RGB(0, 0, 0), RGB(0, 120, 215), RGB(191, 205, 219), RGB(240, 240, 240), RGB(255, 255, 255), RGB(100, 100, 100), RGB(0, 0, 0),
    RGB(0, 0, 0), RGB(255, 255, 255), RGB(180, 180, 180), RGB(244, 247, 252), RGB(171, 171, 171), RGB(0, 120, 215), RGB(255, 255, 255), RGB(240, 240, 240),
    RGB(160, 160, 160), RGB(109, 109, 109), RGB(0, 0, 0), RGB(67, 78, 84), RGB(255, 255, 255), RGB(105, 105, 105), RGB(227, 227, 227), RGB(0, 0, 0),
    RGB(255, 255, 225), RGB(0, 102, 204), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(240, 240, 240) };
int U_get_color(int i) { return (i >= 0 && i < 31) ? (int)sys_colors[i] : 0; }
EXPORT DWORD GetSysColor(int i) { return (DWORD)U_get_color(i); }
EXPORT HBRUSH GetSysColorBrush(int i) {
    static HBRUSH cache[31];
    if (i < 0 || i >= 31) return 0;
    if (!cache[i]) cache[i] = CreateSolidBrush(sys_colors[i]);
    return cache[i];
}
EXPORT BOOL SetSysColors(int n, const INT *e, const COLORREF *c) { (void)n; (void)e; (void)c; return TRUE; }

// ---- rectangles --------------------------------------------------------------------------------------------------------------------------

EXPORT BOOL SetRect(RECT *r, int l, int t, int rr, int b) { r->left = l; r->top = t; r->right = rr; r->bottom = b; return TRUE; }
EXPORT BOOL SetRectEmpty(RECT *r) { r->left = r->top = r->right = r->bottom = 0; return TRUE; }
EXPORT BOOL CopyRect(RECT *d, const RECT *s) { *d = *s; return TRUE; }
EXPORT BOOL InflateRect(RECT *r, int dx, int dy) { r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy; return TRUE; }
EXPORT BOOL OffsetRect(RECT *r, int dx, int dy) { r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return TRUE; }
EXPORT BOOL IsRectEmpty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }
EXPORT BOOL EqualRect(const RECT *a, const RECT *b) { return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }
EXPORT BOOL PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
EXPORT BOOL IntersectRect(RECT *d, const RECT *a, const RECT *b) {
    RECT r = { a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top, a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom };
    if (r.right <= r.left || r.bottom <= r.top) { d->left = d->top = d->right = d->bottom = 0; return FALSE; }
    *d = r; return TRUE;
}
EXPORT BOOL UnionRect(RECT *d, const RECT *a, const RECT *b) {
    if (IsRectEmpty(a)) { if (IsRectEmpty(b)) { SetRectEmpty(d); return FALSE; } *d = *b; return TRUE; }
    if (IsRectEmpty(b)) { *d = *a; return TRUE; }
    RECT r = { a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top, a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom };
    *d = r; return TRUE;
}

// ---- invalidation and painting --------------------------------------------------------------------------------------------------------

void U_invalidate(struct wnd *w, const RECT *r, int erase) {
    if (!w || !w->used) return;
    RECT c = { 0, 0, w->cw, w->ch };
    if (r) { if (r->left > c.left) c.left = r->left; if (r->top > c.top) c.top = r->top; if (r->right < c.right) c.right = r->right; if (r->bottom < c.bottom) c.bottom = r->bottom; }
    if (c.right <= c.left || c.bottom <= c.top) return;
    if (w->has_inval) UnionRect(&w->inval, &w->inval, &c); else { w->inval = c; w->has_inval = 1; }
    if (erase) w->erase = 1;
}
EXPORT BOOL InvalidateRect(HWND h, const RECT *r, BOOL erase) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    U_invalidate(w, r, erase);
    return TRUE;
}
EXPORT BOOL InvalidateRgn(HWND h, void *rgn, BOOL erase) { (void)rgn; return InvalidateRect(h, 0, erase); }
EXPORT BOOL ValidateRect(HWND h, const RECT *r) { (void)r; struct wnd *w = U_wnd(h); if (w) { w->has_inval = 0; w->erase = 0; } return TRUE; }
EXPORT BOOL ValidateRgn(HWND h, void *rgn) { (void)rgn; return ValidateRect(h, 0); }
EXPORT BOOL GetUpdateRect(HWND h, RECT *r, BOOL erase) {
    struct wnd *w = U_wnd(h);
    (void)erase;
    if (!w || !w->has_inval) { if (r) SetRectEmpty(r); return FALSE; }
    if (r) *r = w->inval;
    return TRUE;
}
EXPORT int GetUpdateRgn(HWND h, void *rgn, BOOL erase) { (void)rgn; (void)erase; struct wnd *w = U_wnd(h); return w && w->has_inval ? 2 : 1; }

static void preorder_invalid(struct wnd *w, struct wnd **out) {
    if (*out || !w->used) return;
    if (w->has_inval && w->visible) { *out = w; return; }
    for (struct wnd *c = w->first; c && !*out; c = c->next) if (c->visible) preorder_invalid(c, out);
}
struct wnd *U_next_invalid(DWORD tid) {
    (void)tid;
    struct wnd *r = 0;
    for (int i = 1; i < MAXW && !r; i++) {
        struct wnd *w = U_pool(i);
        if (w->used && !w->parent && w->visible) preorder_invalid(w, &r);
    }
    return r;
}

void U_present_rect(struct wnd *w, const RECT *r) {
    struct wnd *t = U_top(w);
    if (!t || t->wid < 0) return;
    int ox, oy; U_origin(w, &ox, &oy);
    RECT c; U_clip(w, &c);
    RECT a = { r->left + ox, r->top + oy, r->right + ox, r->bottom + oy };
    if (a.left < c.left) a.left = c.left;
    if (a.top < c.top) a.top = c.top;
    if (a.right > c.right) a.right = c.right;
    if (a.bottom > c.bottom) a.bottom = c.bottom;
    if (a.right <= a.left || a.bottom <= a.top) return;
    struct okw_present p = { t->wid, a.left, a.top, a.right - a.left, a.bottom - a.top };
    SC3(LX_ioctl, U_fd(), OKW_PRESENT, &p);
}

static HDC window_dc(struct wnd *w, const RECT *extra_clip) {
    struct wnd *t = U_top(w);
    if (!t || !t->px) return __gdi_make_dc(0, 1, 1, 1);
    HDC dc = __gdi_make_dc(t->px, t->bw, t->bh, t->stride);
    if (!dc) return 0;
    RECT c; U_clip(w, &c);
    int ox, oy; U_origin(w, &ox, &oy);
    if (extra_clip) {
        RECT e = { extra_clip->left + ox, extra_clip->top + oy, extra_clip->right + ox, extra_clip->bottom + oy };
        if (e.left > c.left) c.left = e.left;
        if (e.top > c.top) c.top = e.top;
        if (e.right < c.right) c.right = e.right;
        if (e.bottom < c.bottom) c.bottom = e.bottom;
        if (c.right < c.left) c.right = c.left;
        if (c.bottom < c.top) c.bottom = c.top;
    }
    __gdi_dc_setup(dc, t->px, t->bw, t->bh, t->stride, ox, oy, &c);
    return dc;
}
EXPORT HDC GetDC(HWND h) {
    struct wnd *w = U_wnd(h);
    if (!w) return __gdi_make_dc(0, 1920, 1080, 1920);
    return window_dc(w, 0);
}
EXPORT HDC GetWindowDC(HWND h) { return GetDC(h); }
EXPORT HDC GetDCEx(HWND h, void *rgn, DWORD fl) { (void)rgn; (void)fl; return GetDC(h); }
EXPORT int ReleaseDC(HWND h, HDC dc) {
    struct wnd *w = U_wnd(h);
    if (w) { RECT r = { 0, 0, w->cw, w->ch }; U_present_rect(w, &r); }
    __gdi_free_dc(dc);
    return 1;
}

void U_erase_background(struct wnd *w, HDC dc) {
    RECT r = { 0, 0, w->cw, w->ch };
    HBRUSH b = w->cls ? w->cls->bg : 0;
    if (!b) return;
    if ((ULONG_PTR)b < 32) __gdi_rect_fill_color(dc, r.left, r.top, r.right, r.bottom, (COLORREF)U_get_color((int)(ULONG_PTR)b - 1));
    else __gdi_FillRect(dc, &r, b);
}
static void draw_border(struct wnd *w) {              // the one- or two-pixel frame of a child that has WS_BORDER / WS_EX_CLIENTEDGE
    if (!w->parent || !w->bd) return;
    HDC dc = window_dc(w->parent, 0);
    if (!dc) return;
    RECT r = { w->x, w->y, w->x + w->w, w->y + w->h };
    if (w->bd >= 2) U_draw_edge(dc, &r, 0, 0); else { HBRUSH b = CreateSolidBrush(RGB(100, 100, 100)); __gdi_FrameRect(dc, &r, b); DeleteObject(b); }
    __gdi_free_dc(dc);
    U_present_rect(w->parent, &r);
}

EXPORT HDC BeginPaint(HWND h, PAINTSTRUCT *ps) {
    struct wnd *w = U_wnd(h);
    memset(ps, 0, sizeof *ps);
    if (!w) return 0;
    RECT r = w->has_inval ? w->inval : (RECT){ 0, 0, w->cw, w->ch };
    int erase = w->erase;
    w->has_inval = 0; w->erase = 0;
    ps->rcPaint = r;
    HDC dc = window_dc(w, &r);
    ps->hdc = dc;
    if (erase) {
        draw_border(w);
        if (U_send(w, WM_ERASEBKGND, (WPARAM)dc, 0)) ps->fErase = FALSE; else ps->fErase = TRUE;
    }
    for (struct wnd *c = w->first; c; c = c->next) {             // children that overlap are painted afterwards, over what the parent just drew
        if (!c->visible) continue;
        RECT cr = { c->x, c->y, c->x + c->w, c->y + c->h }, x;
        if (IntersectRect(&x, &cr, &r)) U_invalidate(c, 0, 1);
    }
    return dc;
}
EXPORT BOOL EndPaint(HWND h, const PAINTSTRUCT *ps) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    U_present_rect(w, &ps->rcPaint);
    __gdi_free_dc(ps->hdc);
    return TRUE;
}
void U_paint_window(struct wnd *w) {
    U_send(w, WM_PAINT, 0, 0);
    w->has_inval = 0;
}
static void update_tree(struct wnd *w) {
    if (!w->visible) return;
    if (w->has_inval) U_paint_window(w);
    for (struct wnd *c = w->first; c; c = c->next) update_tree(c);
}
EXPORT BOOL UpdateWindow(HWND h) { struct wnd *w = U_wnd(h); if (!w) return FALSE; update_tree(w); return TRUE; }
EXPORT BOOL RedrawWindow(HWND h, const RECT *r, void *rgn, UINT fl) {
    struct wnd *w = U_wnd(h);
    (void)rgn;
    if (!w) return FALSE;
    if (fl & 1) U_invalidate(w, r, (fl & 4) != 0);
    if (fl & 0x80) for (struct wnd *c = w->first; c; c = c->next) RedrawWindow(c, 0, 0, fl & ~0x100u);
    if (fl & 0x100) update_tree(w);
    return TRUE;
}
EXPORT BOOL LockWindowUpdate(HWND h) { (void)h; return TRUE; }
EXPORT BOOL ScrollWindow(HWND h, int dx, int dy, const RECT *r, const RECT *c) { (void)dx; (void)dy; (void)r; (void)c; return InvalidateRect(h, 0, TRUE); }
EXPORT int ScrollWindowEx(HWND h, int dx, int dy, const RECT *r, const RECT *c, void *rgn, RECT *u, UINT f) { (void)dx; (void)dy; (void)r; (void)c; (void)rgn; (void)u; (void)f; InvalidateRect(h, 0, TRUE); return 1; }

// ---- drawing helpers ---------------------------------------------------------------------------------------------------------------------

EXPORT int FillRect(HDC dc, const RECT *r, HBRUSH b) {
    if ((ULONG_PTR)b >= 1 && (ULONG_PTR)b <= 31) { __gdi_rect_fill_color(dc, r->left, r->top, r->right, r->bottom, (COLORREF)U_get_color((int)(ULONG_PTR)b - 1)); return 1; }
    return __gdi_FillRect(dc, r, b);
}
EXPORT int FrameRect(HDC dc, const RECT *r, HBRUSH b) {
    if ((ULONG_PTR)b >= 1 && (ULONG_PTR)b <= 31) b = GetSysColorBrush((int)(ULONG_PTR)b - 1);
    return __gdi_FrameRect(dc, r, b);
}
EXPORT BOOL InvertRect(HDC dc, const RECT *r) {
    extern int SetROP2(HDC, int);
    int old = SetROP2(dc, 7);
    __gdi_rect_fill_color(dc, r->left, r->top, r->right, r->bottom, RGB(255, 255, 255));
    SetROP2(dc, old);
    return TRUE;
}
EXPORT BOOL DrawFocusRect(HDC dc, const RECT *r) {
    extern int SetROP2(HDC, int);
    int old = SetROP2(dc, 7);
    for (int x = r->left; x < r->right; x += 2) { __gdi_rect_fill_color(dc, x, r->top, x + 1, r->top + 1, RGB(255, 255, 255)); __gdi_rect_fill_color(dc, x, r->bottom - 1, x + 1, r->bottom, RGB(255, 255, 255)); }
    for (int y = r->top; y < r->bottom; y += 2) { __gdi_rect_fill_color(dc, r->left, y, r->left + 1, y + 1, RGB(255, 255, 255)); __gdi_rect_fill_color(dc, r->right - 1, y, r->right, y + 1, RGB(255, 255, 255)); }
    SetROP2(dc, old);
    return TRUE;
}
// a classic bevel: raised or sunken, one or two pixels
void U_draw_edge(HDC dc, RECT *r, int raised, int thin) {
    COLORREF hi = RGB(255, 255, 255), lo = RGB(100, 100, 100), dk = RGB(30, 30, 30), li = RGB(227, 227, 227);
    COLORREF tl = raised ? hi : lo, br = raised ? dk : hi;
    __gdi_rect_fill_color(dc, r->left, r->top, r->right - 1, r->top + 1, tl);
    __gdi_rect_fill_color(dc, r->left, r->top, r->left + 1, r->bottom - 1, tl);
    __gdi_rect_fill_color(dc, r->left, r->bottom - 1, r->right, r->bottom, br);
    __gdi_rect_fill_color(dc, r->right - 1, r->top, r->right, r->bottom, br);
    if (!thin) {
        COLORREF tl2 = raised ? li : dk, br2 = raised ? lo : li;
        __gdi_rect_fill_color(dc, r->left + 1, r->top + 1, r->right - 2, r->top + 2, tl2);
        __gdi_rect_fill_color(dc, r->left + 1, r->top + 1, r->left + 2, r->bottom - 2, tl2);
        __gdi_rect_fill_color(dc, r->left + 1, r->bottom - 2, r->right - 1, r->bottom - 1, br2);
        __gdi_rect_fill_color(dc, r->right - 2, r->top + 1, r->right - 1, r->bottom - 1, br2);
    }
}
EXPORT BOOL DrawEdge(HDC dc, RECT *r, UINT edge, UINT flags) {
    int raised = (edge & 5) != 0 && (edge & 10) == 0 ? 1 : (edge & 4) ? 1 : 0;       // BDR_RAISEDOUTER 1, SUNKENOUTER 2, RAISEDINNER 4, SUNKENINNER 8
    if (edge & 0xA) raised = 0;
    U_draw_edge(dc, r, raised, 0);
    if (flags & 0x800) { r->left += 2; r->top += 2; r->right -= 2; r->bottom -= 2; }  // BF_ADJUST
    return TRUE;
}
EXPORT BOOL DrawFrameControl(HDC dc, RECT *r, UINT type, UINT state) {
    (void)type;
    U_draw_edge(dc, r, !(state & 0x200), 0);
    return TRUE;
}

int U_text_width(HDC dc, const char *s, int n) { SIZE sz; if (n <= 0) return 0; GetTextExtentPoint32A(dc, s, n, &sz); return sz.cx; }
int U_text_height(HDC dc) { SIZE sz; GetTextExtentPoint32A(dc, "Hg", 2, &sz); return sz.cy; }

static int draw_text(HDC dc, const char *s, int len, RECT *rc, UINT fl) {
    if (len < 0) len = ustrlen(s);
    char buf[1024];
    if (len > 1000) len = 1000;
    int n = 0;
    for (int i = 0; i < len; i++) {                                      // strip the '&' accelerator markers
        if (s[i] == '&' && !(fl & DT_NOPREFIX)) { if (i + 1 < len && s[i + 1] == '&') { buf[n++] = '&'; i++; } continue; }
        buf[n++] = s[i];
    }
    int lh = U_text_height(dc);
    int maxw = rc->right - rc->left;
    // break into lines
    int ls[128], le[128], nl = 0, i = 0;
    while (i <= n && nl < 128) {
        int st = i, last_sp = -1, w = 0;
        while (i < n && buf[i] != '\n') {
            if ((fl & DT_WORDBREAK) && !(fl & DT_SINGLELINE)) {
                if (buf[i] == ' ') last_sp = i;
                w = U_text_width(dc, buf + st, i - st + 1);
                if (w > maxw && i > st) {
                    if (last_sp > st) { i = last_sp; }
                    break;
                }
            }
            i++;
        }
        int e = i;
        if (e > st && buf[e - 1] == '\r') e--;
        ls[nl] = st; le[nl] = e; nl++;
        if (i < n && buf[i] == '\n') i++;
        else if (i < n && buf[i] == ' ' && (fl & DT_WORDBREAK)) i++;
        if (i >= n) break;
        if (fl & DT_SINGLELINE) break;
    }
    if (nl == 0) { ls[0] = le[0] = 0; nl = 1; }
    int total = nl * lh, maxlw = 0;
    for (int k = 0; k < nl; k++) { int lw = U_text_width(dc, buf + ls[k], le[k] - ls[k]); if (lw > maxlw) maxlw = lw; }
    if (fl & DT_CALCRECT) {
        rc->right = rc->left + ((fl & DT_WORDBREAK) && maxlw > maxw ? maxw : maxlw);
        rc->bottom = rc->top + total;
        return total;
    }
    int y = rc->top;
    if (fl & DT_VCENTER) y = rc->top + ((rc->bottom - rc->top) - total) / 2; else if (fl & DT_BOTTOM) y = rc->bottom - total;
    for (int k = 0; k < nl; k++) {
        int lw = U_text_width(dc, buf + ls[k], le[k] - ls[k]);
        const char *p = buf + ls[k]; int pl = le[k] - ls[k];
        char el[1024];
        if ((fl & DT_END_ELLIPSIS) && lw > maxw && pl > 3) {
            int cut = pl;
            while (cut > 0 && U_text_width(dc, p, cut) + U_text_width(dc, "...", 3) > maxw) cut--;
            memcpy(el, p, cut); el[cut] = '.'; el[cut + 1] = '.'; el[cut + 2] = '.'; p = el; pl = cut + 3; lw = U_text_width(dc, p, pl);
        }
        int x = rc->left;
        if ((fl & 3) == DT_CENTER) x = rc->left + (maxw - lw) / 2; else if ((fl & 3) == DT_RIGHT) x = rc->right - lw;
        TextOutA(dc, x, y + k * lh, p, pl);
    }
    return total;
}
EXPORT int DrawTextA(HDC dc, LPCSTR s, int n, RECT *rc, UINT fl) { return draw_text(dc, s, n, rc, fl); }
EXPORT int DrawTextW(HDC dc, LPCWSTR s, int n, RECT *rc, UINT fl) {
    char u[1100]; int l = WideCharToMultiByte(65001, 0, s, n, u, sizeof u, 0, 0);
    return draw_text(dc, u, l > 0 ? (n < 0 ? l - 1 : l) : 0, rc, fl);
}
EXPORT int DrawTextExA(HDC dc, LPSTR s, int n, RECT *rc, UINT fl, void *p) { (void)p; return draw_text(dc, s, n, rc, fl); }
EXPORT int DrawTextExW(HDC dc, LPWSTR s, int n, RECT *rc, UINT fl, void *p) { (void)p; return DrawTextW(dc, s, n, rc, fl); }
EXPORT LONG TabbedTextOutA(HDC dc, int x, int y, LPCSTR s, int n, int nt, const INT *tabs, int org) { (void)nt; (void)tabs; (void)org; TextOutA(dc, x, y, s, n); return U_text_width(dc, s, n); }
EXPORT BOOL DrawIcon(HDC dc, int x, int y, HICON i) { (void)dc; (void)x; (void)y; (void)i; return TRUE; }
EXPORT BOOL DrawIconEx(HDC dc, int x, int y, HICON i, int w, int h, UINT s, HBRUSH b, UINT f) { (void)dc; (void)x; (void)y; (void)i; (void)w; (void)h; (void)s; (void)b; (void)f; return TRUE; }

// ---- metrics ------------------------------------------------------------------------------------------------------------------------------

EXPORT int GetSystemMetrics(int i) {
    int title, border, sw, sh;
    U_init();
    U_info(&title, &border, &sw, &sh);
    switch (i) {
    case 0: case 16: case 78: return sw;
    case 1: case 79: return sh;
    case 17: return sh - 40;
    case 2: case 3: return 17;
    case 4: return title; case 51: return title;
    case 5: case 6: return 1;
    case 7: case 8: return 3;
    case 32: case 33: return border;
    case 11: case 12: case 13: case 14: return 32;
    case 15: return 20;
    case 19: return 1;
    case 23: return 0;
    case 28: return 136; case 29: return 39;
    case 30: case 31: return 18;
    case 34: return 136; case 35: return 39;
    case 36: case 37: return 4;
    case 43: return 3;
    case 45: case 46: return 2;
    case 49: case 50: return 16;
    case 75: return 1;
    case 80: return 1;
    case 76: case 77: return 0;
    case 10: return 17;
    default: return 0;
    }
}
EXPORT int GetSystemMetricsForDpi(int i, UINT dpi) { (void)dpi; return GetSystemMetrics(i); }
EXPORT BOOL SystemParametersInfoA(UINT a, UINT b, void *c, UINT d) {
    (void)b; (void)d;
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    if (a == 0x30) { RECT *r = c; r->left = 0; r->top = 0; r->right = sw; r->bottom = sh - 40; return TRUE; }          // SPI_GETWORKAREA
    if (a == 0x68 || a == 0x6C) { *(UINT*)c = 3; return TRUE; }                                                              // wheel scroll lines / chars
    if (a == 0x2A || a == 0x1010) return TRUE;
    return FALSE;
}
EXPORT BOOL SystemParametersInfoW(UINT a, UINT b, void *c, UINT d) { return SystemParametersInfoA(a, b, c, d); }
EXPORT BOOL SetProcessDPIAware(void) { return TRUE; }
EXPORT BOOL SetProcessDpiAwarenessContext(void *c) { (void)c; return TRUE; }
EXPORT UINT GetDpiForWindow(HWND h) { (void)h; return 96; }
EXPORT UINT GetDpiForSystem(void) { return 96; }

// ---- resources and little things ----------------------------------------------------------------------------------------------------------------

EXPORT HCURSOR LoadCursorA(HINSTANCE h, LPCSTR n) { (void)h; return (HCURSOR)((ULONG_PTR)n < 0x10000 ? 0x1000 + (ULONG_PTR)n : 0x2000); }
EXPORT HCURSOR LoadCursorW(HINSTANCE h, LPCWSTR n) { return LoadCursorA(h, (LPCSTR)n); }
EXPORT HICON LoadIconA(HINSTANCE h, LPCSTR n) { (void)h; return (HICON)((ULONG_PTR)n < 0x10000 ? 0x3000 + (ULONG_PTR)n : 0x4000); }
EXPORT HICON LoadIconW(HINSTANCE h, LPCWSTR n) { return LoadIconA(h, (LPCSTR)n); }
EXPORT HANDLE LoadImageA(HINSTANCE h, LPCSTR n, UINT t, int w, int hh, UINT f) { (void)h; (void)n; (void)t; (void)w; (void)hh; (void)f; return (HANDLE)0x5000; }
EXPORT HANDLE LoadImageW(HINSTANCE h, LPCWSTR n, UINT t, int w, int hh, UINT f) { return LoadImageA(h, (LPCSTR)n, t, w, hh, f); }
EXPORT HBITMAP LoadBitmapA(HINSTANCE h, LPCSTR n) { (void)h; (void)n; return 0; }
EXPORT int LoadStringA(HINSTANCE h, UINT id, LPSTR buf, int cap) { (void)h; (void)id; if (cap > 0) buf[0] = 0; return 0; }
EXPORT int LoadStringW(HINSTANCE h, UINT id, LPWSTR buf, int cap) { (void)h; (void)id; if (cap > 0) buf[0] = 0; return 0; }
EXPORT BOOL DestroyIcon(HICON i) { (void)i; return TRUE; }
EXPORT BOOL DestroyCursor(HCURSOR c) { (void)c; return TRUE; }
EXPORT HANDLE LoadAcceleratorsA(HINSTANCE h, LPCSTR n) { (void)h; (void)n; return (HANDLE)1; }
EXPORT HANDLE LoadAcceleratorsW(HINSTANCE h, LPCWSTR n) { (void)h; (void)n; return (HANDLE)1; }
EXPORT HMENU CreateMenu(void) { return (HMENU)1; }
EXPORT HMENU CreatePopupMenu(void) { return (HMENU)1; }
EXPORT HMENU LoadMenuA(HINSTANCE h, LPCSTR n) { (void)h; (void)n; return (HMENU)1; }
EXPORT HMENU LoadMenuW(HINSTANCE h, LPCWSTR n) { (void)h; (void)n; return (HMENU)1; }
EXPORT BOOL SetMenu(HWND h, HMENU m) { (void)h; (void)m; return TRUE; }
EXPORT HMENU GetMenu(HWND h) { (void)h; return 0; }
EXPORT HMENU GetSystemMenu(HWND h, BOOL r) { (void)h; (void)r; return 0; }
EXPORT HMENU GetSubMenu(HMENU m, int i) { (void)m; (void)i; return 0; }
EXPORT BOOL DestroyMenu(HMENU m) { (void)m; return TRUE; }
EXPORT BOOL DrawMenuBar(HWND h) { (void)h; return TRUE; }
EXPORT BOOL AppendMenuA(HMENU m, UINT f, ULONG_PTR id, LPCSTR s) { (void)m; (void)f; (void)id; (void)s; return TRUE; }
EXPORT BOOL AppendMenuW(HMENU m, UINT f, ULONG_PTR id, LPCWSTR s) { (void)m; (void)f; (void)id; (void)s; return TRUE; }
EXPORT BOOL InsertMenuA(HMENU m, UINT p, UINT f, ULONG_PTR id, LPCSTR s) { (void)m; (void)p; (void)f; (void)id; (void)s; return TRUE; }
EXPORT BOOL RemoveMenu(HMENU m, UINT p, UINT f) { (void)m; (void)p; (void)f; return TRUE; }
EXPORT BOOL DeleteMenu(HMENU m, UINT p, UINT f) { (void)m; (void)p; (void)f; return TRUE; }
EXPORT DWORD CheckMenuItem(HMENU m, UINT i, UINT f) { (void)m; (void)i; (void)f; return 0; }
EXPORT BOOL EnableMenuItem(HMENU m, UINT i, UINT f) { (void)m; (void)i; (void)f; return 0; }
EXPORT BOOL TrackPopupMenu(HMENU m, UINT f, int x, int y, int r, HWND h, const RECT *rc) { (void)m; (void)f; (void)x; (void)y; (void)r; (void)h; (void)rc; return 0; }
EXPORT BOOL OpenClipboard(HWND h) { (void)h; return FALSE; }
EXPORT BOOL CloseClipboard(void) { return TRUE; }
EXPORT BOOL EmptyClipboard(void) { return TRUE; }
EXPORT HANDLE GetClipboardData(UINT f) { (void)f; return 0; }
EXPORT HANDLE SetClipboardData(UINT f, HANDLE h) { (void)f; (void)h; return 0; }
EXPORT BOOL IsClipboardFormatAvailable(UINT f) { (void)f; return FALSE; }
EXPORT BOOL SetScrollInfo(HWND h, int b, const void *i, BOOL r) { (void)h; (void)b; (void)i; (void)r; return TRUE; }
EXPORT BOOL GetScrollInfo(HWND h, int b, void *i) { (void)h; (void)b; (void)i; return FALSE; }
EXPORT int SetScrollPos(HWND h, int b, int p, BOOL r) { (void)h; (void)b; (void)r; return p; }
EXPORT int GetScrollPos(HWND h, int b) { (void)h; (void)b; return 0; }
EXPORT BOOL SetScrollRange(HWND h, int b, int lo, int hi, BOOL r) { (void)h; (void)b; (void)lo; (void)hi; (void)r; return TRUE; }
EXPORT BOOL ShowScrollBar(HWND h, int b, BOOL s) { (void)h; (void)b; (void)s; return TRUE; }
EXPORT BOOL EnableScrollBar(HWND h, UINT b, UINT a) { (void)h; (void)b; (void)a; return TRUE; }
EXPORT int SetWindowRgn(HWND h, void *r, BOOL red) { (void)h; (void)r; (void)red; return 1; }
EXPORT BOOL SetLayeredWindowAttributes(HWND h, COLORREF c, BYTE a, DWORD f) { (void)h; (void)c; (void)a; (void)f; return TRUE; }
EXPORT BOOL DragDetect(HWND h, POINT p) { (void)h; (void)p; return FALSE; }
EXPORT BOOL SetPropA(HWND h, LPCSTR n, HANDLE d) { (void)h; (void)n; (void)d; return TRUE; }
EXPORT HANDLE GetPropA(HWND h, LPCSTR n) { (void)h; (void)n; return 0; }
EXPORT HANDLE RemovePropA(HWND h, LPCSTR n) { (void)h; (void)n; return 0; }

// character functions
EXPORT LPSTR CharUpperA(LPSTR s) { if ((ULONG_PTR)s < 0x10000) { char c = (char)(ULONG_PTR)s; return (LPSTR)(ULONG_PTR)(unsigned char)((c >= 'a' && c <= 'z') ? c - 32 : c); } for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
EXPORT LPSTR CharLowerA(LPSTR s) { if ((ULONG_PTR)s < 0x10000) { char c = (char)(ULONG_PTR)s; return (LPSTR)(ULONG_PTR)(unsigned char)((c >= 'A' && c <= 'Z') ? c + 32 : c); } for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
EXPORT DWORD CharUpperBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32; return n; }
EXPORT DWORD CharLowerBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32; return n; }
EXPORT LPWSTR CharUpperW(LPWSTR s) { if ((ULONG_PTR)s < 0x10000) { WCHAR c = (WCHAR)(ULONG_PTR)s; return (LPWSTR)(ULONG_PTR)((c >= 'a' && c <= 'z') ? c - 32 : c); } for (WCHAR *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
EXPORT LPWSTR CharLowerW(LPWSTR s) { if ((ULONG_PTR)s < 0x10000) { WCHAR c = (WCHAR)(ULONG_PTR)s; return (LPWSTR)(ULONG_PTR)((c >= 'A' && c <= 'Z') ? c + 32 : c); } for (WCHAR *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; return s; }
EXPORT LPSTR CharNextA(LPCSTR s) { return (LPSTR)(*s ? s + 1 : s); }
EXPORT LPSTR CharPrevA(LPCSTR start, LPCSTR cur) { return (LPSTR)(cur > start ? cur - 1 : start); }
EXPORT LPWSTR CharNextW(LPCWSTR s) { return (LPWSTR)(*s ? s + 1 : s); }
EXPORT BOOL IsCharAlphaA(CHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
EXPORT BOOL IsCharAlphaNumericA(CHAR c) { return IsCharAlphaA(c) || (c >= '0' && c <= '9'); }
EXPORT BOOL IsCharUpperA(CHAR c) { return c >= 'A' && c <= 'Z'; }
EXPORT BOOL IsCharLowerA(CHAR c) { return c >= 'a' && c <= 'z'; }
EXPORT BOOL CharToOemA(LPCSTR s, LPSTR d) { int i = 0; for (; s[i]; i++) d[i] = s[i]; d[i] = 0; return TRUE; }
EXPORT BOOL OemToCharA(LPCSTR s, LPSTR d) { return CharToOemA(s, d); }

// wsprintf: the subset programs use
typedef __builtin_va_list va_list_;
#define VA_ARG(ap, T) __builtin_va_arg(ap, T)
static int fmt_impl(char *out, int cap, const char *f, va_list_ ap, int wide_s) {
    int n = 0;
#define PUT(c) do { if (n < cap - 1) out[n] = (c); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, plus = 0;
        for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; else if (*f == ' ' || *f == '#') ; else break; }
        if (*f == '*') { width = VA_ARG(ap, int); f++; } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = VA_ARG(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        while (*f == 'l' || *f == 'h' || *f == 'I' || *f == 'z' || *f == 'j') { if (*f == 'l') lng++; if (*f == 'I' && f[1] == '6') { lng = 2; f += 2; } f++; }
        char tmp[40]; const char *s = tmp; int sl = 0;
        char c = *f;
        if (c == 'd' || c == 'i') {
            long long v = lng >= 2 ? VA_ARG(ap, long long) : lng ? (long long)VA_ARG(ap, long) : (long long)VA_ARG(ap, int);
            unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            char r[24]; int k = 0; do { r[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) tmp[sl++] = '-'; else if (plus) tmp[sl++] = '+';
            while (k) tmp[sl++] = r[--k];
        } else if (c == 'u' || c == 'x' || c == 'X') {
            unsigned long long u = lng >= 2 ? VA_ARG(ap, unsigned long long) : lng ? (unsigned long long)VA_ARG(ap, unsigned long) : (unsigned long long)VA_ARG(ap, unsigned);
            unsigned b = c == 'u' ? 10 : 16;
            char r[24]; int k = 0; do { unsigned d = (unsigned)(u % b); r[k++] = (char)(d < 10 ? '0' + d : (c == 'X' ? 'A' : 'a') + d - 10); u /= b; } while (u);
            while (k) tmp[sl++] = r[--k];
        } else if (c == 'c') { tmp[0] = (char)VA_ARG(ap, int); sl = 1; }
        else if (c == 's' || c == 'S') {
            const void *p = VA_ARG(ap, const void*);
            static char conv[512];
            int is_wide = (c == 's') ? wide_s : !wide_s;
            if (!p) { s = "(null)"; sl = 6; }
            else if (is_wide) { int l = WideCharToMultiByte(65001, 0, p, -1, conv, sizeof conv, 0, 0); sl = l > 0 ? l - 1 : 0; s = conv; }
            else { s = p; while (s[sl] && (prec < 0 || sl < prec)) sl++; }
            if (prec >= 0 && sl > prec) sl = prec;
        } else if (c == 'p') { unsigned long long u = (unsigned long long)VA_ARG(ap, void*); char r[24]; int k = 0; do { unsigned d = (unsigned)(u & 15); r[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u >>= 4; } while (u); while (k) tmp[sl++] = r[--k]; }
        else if (c == '%') { tmp[0] = '%'; sl = 1; }
        else if (!c) break;
        else { tmp[0] = c; sl = 1; }
        int pad = width > sl ? width - sl : 0;
        if (!left && !zero) while (pad-- > 0) PUT(' ');
        if (!left && zero && (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') && sl && (s[0] == '-' || s[0] == '+')) { PUT(s[0]); s++; sl--; while (pad-- > 0) PUT('0'); }
        else if (!left && zero) while (pad-- > 0) PUT('0');
        for (int k = 0; k < sl; k++) PUT(s[k]);
        if (left) while (pad-- > 0) PUT(' ');
    }
    if (cap > 0) out[n < cap ? n : cap - 1] = 0;
    return n;
#undef PUT
}
int U_wsprintf(char *out, int cap, const char *fmt, void *args, int wide_s) { (void)args; (void)wide_s; (void)out; (void)cap; (void)fmt; return 0; }
EXPORT int wsprintfA(LPSTR out, LPCSTR fmt, ...) { va_list_ ap; __builtin_va_start(ap, fmt); int n = fmt_impl(out, 1 << 20, fmt, ap, 0); __builtin_va_end(ap); return n; }
EXPORT int wvsprintfA(LPSTR out, LPCSTR fmt, va_list_ ap) { return fmt_impl(out, 1 << 20, fmt, ap, 0); }
EXPORT int wsprintfW(LPWSTR out, LPCWSTR fmt, ...) {
    char f[512], o[2048]; va_list_ ap;
    WideCharToMultiByte(65001, 0, fmt, -1, f, sizeof f, 0, 0);
    __builtin_va_start(ap, fmt); int n = fmt_impl(o, sizeof o, f, ap, 1); __builtin_va_end(ap);
    MultiByteToWideChar(65001, 0, o, -1, out, 2048);
    return n;
}
EXPORT int wvsprintfW(LPWSTR out, LPCWSTR fmt, va_list_ ap) {
    char f[512], o[2048];
    WideCharToMultiByte(65001, 0, fmt, -1, f, sizeof f, 0, 0);
    int n = fmt_impl(o, sizeof o, f, ap, 1);
    MultiByteToWideChar(65001, 0, o, -1, out, 2048);
    return n;
}
