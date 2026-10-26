// user32: the built-in window classes BUTTON, STATIC, EDIT and LISTBOX, drawn with the flat classic look
#include "u32.h"

extern HDC BeginPaint(HWND h, PAINTSTRUCT *ps);
extern BOOL EndPaint(HWND h, const PAINTSTRUCT *ps);
extern int DrawTextA(HDC dc, LPCSTR s, int n, RECT *rc, UINT fl);
extern int FillRect(HDC dc, const RECT *r, HBRUSH b);
extern BOOL DrawFocusRect(HDC dc, const RECT *r);
extern BOOL Ellipse(HDC dc, int l, int t, int r, int b);
extern BOOL SetCapture_(HWND);
extern HWND SetCapture(HWND h);
extern BOOL ReleaseCapture(void);
extern HWND SetFocus(HWND h);
extern BOOL InvalidateRect(HWND h, const RECT *r, BOOL e);
extern LRESULT SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp);

#define WM_DRAWITEM 0x2B
#define WM_COPY 0x301
#define WM_CUT 0x300
#define WM_PASTE 0x302
#define WM_CLEAR 0x303
#define BS_OWNERDRAW 0xB
#define BS_PUSHLIKE 0x1000
#define ES_CENTER 1
#define ES_RIGHT 2
#define ES_UPPERCASE 8
#define ES_LOWERCASE 0x10
#define LBS_NOTIFY 1
#define LBS_SORT 2

static COLORREF col(int i) { return (COLORREF)U_get_color(i); }
static void apply_font(struct wnd *w, HDC dc) { if (w->font) SelectObject(dc, w->font); }

static HBRUSH ctlcolor(struct wnd *w, HDC dc, UINT msg) {
    if (!w->parent) return 0;
    LRESULT r = U_send(w->parent, msg, (WPARAM)dc, (LPARAM)w);
    return (HBRUSH)r;
}
static void notify(struct wnd *w, int code) {
    if (w->parent) U_send(w->parent, WM_COMMAND, (WPARAM)((w->id & 0xFFFF) | ((unsigned)code << 16)), (LPARAM)w);
}
static void fill(HDC dc, const RECT *r, COLORREF c) { __gdi_rect_fill_color(dc, r->left, r->top, r->right, r->bottom, c); }
static LRESULT common(struct wnd *w, UINT m, WPARAM wp, LPARAM lp, int *handled) {
    *handled = 1;
    switch (m) {
    case WM_SETFONT: w->font = (void*)wp; if (lp) U_invalidate(w, 0, 1); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS: U_invalidate(w, 0, 1); *handled = 0; return 0;
    default: *handled = 0; return 0;
    }
}

// ---- BUTTON ---------------------------------------------------------------------------------------------------------------------------------

struct btn { int checked, pushed, capture, over; };
static int btn_type(struct wnd *w) { return (int)(w->style & BS_TYPEMASK); }
static int is_check(int t) { return t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE; }
static int is_radio(int t) { return t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON; }

static void btn_paint(struct wnd *w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    if (!dc) return;
    struct btn *b = w->ctl;
    int t = btn_type(w);
    RECT r = { 0, 0, w->cw, w->ch };
    apply_font(w, dc);
    HBRUSH cb = ctlcolor(w, dc, WM_CTLCOLORBTN);
    if (cb) FillRect(dc, &r, cb); else fill(dc, &r, col(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, w->enabled ? col(COLOR_WINDOWTEXT) : col(17));
    int focused = U_focus() == w;
    if (t == BS_OWNERDRAW) {
        struct { UINT ctype, cid, iid, action, state; HWND item; HDC dc; RECT rc; ULONG_PTR data; } d;
        d.ctype = 4; d.cid = (UINT)w->id; d.iid = 0; d.action = 1; d.state = (b->pushed ? 1 : 0) | (w->enabled ? 0 : 4) | (focused ? 0x10 : 0); d.item = w; d.dc = dc; d.rc = r; d.data = 0;
        U_send(w->parent, WM_DRAWITEM, (WPARAM)w->id, (LPARAM)&d);
    } else if (t == BS_GROUPBOX) {
        int th = U_text_height(dc);
        RECT fr = { 0, th / 2, w->cw, w->ch };
        HBRUSH g = CreateSolidBrush(col(16)); __gdi_FrameRect(dc, &fr, g); DeleteObject(g);
        int tw = U_text_width(dc, w->text, ustrlen(w->text));
        RECT gap = { 8, 0, 8 + tw + 6, th }; fill(dc, &gap, col(COLOR_BTNFACE));
        TextOutA(dc, 11, 0, w->text, ustrlen(w->text));
    } else if (is_check(t) || is_radio(t)) {
        int th = U_text_height(dc);
        int by = (w->ch - 13) / 2;
        RECT box = { 0, by, 13, by + 13 };
        if (is_check(t)) {
            fill(dc, &box, w->enabled ? RGB(255, 255, 255) : col(COLOR_BTNFACE));
            HBRUSH g = CreateSolidBrush(col(16)); __gdi_FrameRect(dc, &box, g); DeleteObject(g);
            if (b->checked) {
                SelectObject(dc, CreatePen(PS_SOLID, 1, w->enabled ? RGB(0, 0, 0) : col(17)));
                for (int k = 0; k < 2; k++) { MoveToEx(dc, 3, by + 6 + k, 0); LineTo(dc, 5, by + 8 + k); LineTo(dc, 10, by + 3 + k); }
            }
        } else {
            SelectObject(dc, GetStockObject(WHITE_BRUSH));
            HPEN pen = CreatePen(PS_SOLID, 1, col(16)); HGDIOBJ op = SelectObject(dc, pen);
            Ellipse(dc, 0, by, 13, by + 13);
            if (b->checked) { SelectObject(dc, GetStockObject(BLACK_BRUSH)); SelectObject(dc, GetStockObject(BLACK_PEN)); Ellipse(dc, 4, by + 4, 9, by + 9); }
            SelectObject(dc, op); DeleteObject(pen);
        }
        RECT tr = { 19, 0, w->cw, w->ch };
        DrawTextA(dc, w->text, -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
        if (focused) { RECT fr = { 17, (w->ch - th) / 2 - 1, 19 + U_text_width(dc, w->text, ustrlen(w->text)) + 2, (w->ch + th) / 2 + 1 }; DrawFocusRect(dc, &fr); }
    } else {
        int def = t == BS_DEFPUSHBUTTON;
        RECT e = r;
        if (def) { HBRUSH k = CreateSolidBrush(RGB(0, 0, 0)); __gdi_FrameRect(dc, &r, k); DeleteObject(k); e.left++; e.top++; e.right--; e.bottom--; }
        U_draw_edge(dc, &e, !b->pushed, 0);
        RECT tr = { 3 + (b->pushed ? 1 : 0), 3 + (b->pushed ? 1 : 0), w->cw - 3, w->ch - 3 };
        DrawTextA(dc, w->text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (focused) { RECT fr = { 4, 4, w->cw - 4, w->ch - 4 }; DrawFocusRect(dc, &fr); }
    }
    EndPaint(w, &ps);
}
static void btn_click(struct wnd *w) {
    struct btn *b = w->ctl;
    int t = btn_type(w);
    if (t == BS_AUTOCHECKBOX) b->checked = !b->checked;
    else if (t == BS_AUTO3STATE) b->checked = (b->checked + 1) % 3;
    else if (t == BS_AUTORADIOBUTTON) {
        for (struct wnd *s = w->prev; s && btn_type(s) == BS_AUTORADIOBUTTON; s = s->prev) { ((struct btn*)s->ctl)->checked = 0; U_invalidate(s, 0, 1); if (s->style & WS_GROUP) break; }
        for (struct wnd *s = w->next; s && btn_type(s) == BS_AUTORADIOBUTTON && !(s->style & WS_GROUP); s = s->next) { ((struct btn*)s->ctl)->checked = 0; U_invalidate(s, 0, 1); }
        b->checked = 1;
    }
    U_invalidate(w, 0, 1);
    notify(w, BN_CLICKED);
}
LRESULT U_button_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct btn *b = w->ctl;
    int hd; LRESULT r = common(w, m, wp, lp, &hd);
    if (hd) return r;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CREATE: w->ctl = umalloc(sizeof(struct btn)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: btn_paint(w); return 0;
    case WM_SETTEXT: r = U_def_proc(w, m, wp, lp, w->unicode); U_invalidate(w, 0, 1); return r;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (!w->enabled) return 0;
        SetCapture(w); b->capture = 1; b->pushed = 1;
        if (U_focus() != w) SetFocus(w);
        U_invalidate(w, 0, 1);
        return 0;
    case WM_MOUSEMOVE:
        if (b->capture) {
            int in = GET_X(lp) >= 0 && GET_Y(lp) >= 0 && GET_X(lp) < w->cw && GET_Y(lp) < w->ch;
            if (in != b->pushed) { b->pushed = in; U_invalidate(w, 0, 1); }
        }
        return 0;
    case WM_LBUTTONUP:
        if (b->capture) {
            int fire = b->pushed;
            b->capture = 0; b->pushed = 0; ReleaseCapture(); U_invalidate(w, 0, 1);
            if (fire) btn_click(w);
        }
        return 0;
    case WM_KEYDOWN: if (wp == VK_SPACE && w->enabled) { b->pushed = 1; U_invalidate(w, 0, 1); } return 0;
    case WM_KEYUP: if (wp == VK_SPACE && b->pushed) { b->pushed = 0; btn_click(w); } return 0;
    case BM_GETCHECK: return b->checked;
    case BM_SETCHECK: b->checked = (int)wp; U_invalidate(w, 0, 1); return 0;
    case BM_GETSTATE: return (b->pushed ? 4 : 0) | (b->checked ? 3 : 0) | (U_focus() == w ? 8 : 0);
    case BM_SETSTATE: b->pushed = wp != 0; U_invalidate(w, 0, 1); return 0;
    case BM_CLICK: btn_click(w); return 0;
    default: return U_def_proc(w, m, wp, lp, w->unicode);
    }
}

// ---- STATIC ---------------------------------------------------------------------------------------------------------------------------------

LRESULT U_static_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    int hd; LRESULT r = common(w, m, wp, lp, &hd);
    if (hd) return r;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_SETTEXT: r = U_def_proc(w, m, wp, lp, w->unicode); U_invalidate(w, 0, 1); return r;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (!dc) return 0;
        RECT rc = { 0, 0, w->cw, w->ch };
        int t = (int)(w->style & SS_TYPEMASK);
        apply_font(w, dc);
        HBRUSH cb = ctlcolor(w, dc, WM_CTLCOLORSTATIC);
        if (t == SS_BLACKRECT) fill(dc, &rc, RGB(0, 0, 0));
        else if (t == SS_GRAYRECT) fill(dc, &rc, col(16));
        else if (t == SS_WHITERECT) fill(dc, &rc, RGB(255, 255, 255));
        else {
            if (cb) FillRect(dc, &rc, cb); else fill(dc, &rc, col(COLOR_BTNFACE));
            if (t == 0x10 || t == 0x11 || t == 0x12) {                                 // etched lines and frames
                HBRUSH g = CreateSolidBrush(col(16));
                if (t == 0x10) { RECT l = { 0, w->ch / 2, w->cw, w->ch / 2 + 1 }; fill(dc, &l, col(16)); }
                else if (t == 0x11) { RECT l = { w->cw / 2, 0, w->cw / 2 + 1, w->ch }; fill(dc, &l, col(16)); }
                else __gdi_FrameRect(dc, &rc, g);
                DeleteObject(g);
            } else if (t != SS_ICON && t != 0xE) {
                SetBkMode(dc, TRANSPARENT);
                if (!cb) SetTextColor(dc, w->enabled ? col(COLOR_WINDOWTEXT) : col(17));
                UINT fl = (t == SS_CENTER ? DT_CENTER : t == SS_RIGHT ? DT_RIGHT : DT_LEFT) | (t == 0xC ? DT_SINGLELINE : DT_WORDBREAK) | (w->style & 0x80 ? DT_NOPREFIX : 0);
                if (t == 0xC || (w->style & 0x200)) fl |= DT_VCENTER | DT_SINGLELINE;
                DrawTextA(dc, w->text, -1, &rc, fl);
            }
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: if (w->style & 0x100) notify(w, 0); return 0;           // SS_NOTIFY: STN_CLICKED
    default: return U_def_proc(w, m, wp, lp, w->unicode);
    }
}

// ---- EDIT: see edit.c ---------------------------------------------------------------------------------------------------------------------------

// ---- LISTBOX --------------------------------------------------------------------------------------------------------------------------------

struct lbox { char **items; int n, cap, sel, top, drag; };
static int lb_rowh(struct wnd *w, HDC dc) { (void)w; return U_text_height(dc) + 2; }
static void lb_paint(struct wnd *w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    if (!dc) return;
    struct lbox *l = w->ctl;
    RECT rc = { 0, 0, w->cw, w->ch };
    apply_font(w, dc);
    fill(dc, &rc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    int rh = lb_rowh(w, dc);
    for (int i = l->top, y = 0; i < l->n && y < w->ch; i++, y += rh) {
        RECT ir = { 0, y, w->cw - 1, y + rh };
        if (i == l->sel) { fill(dc, &ir, col(13)); SetTextColor(dc, col(14)); } else SetTextColor(dc, w->enabled ? col(COLOR_WINDOWTEXT) : col(17));
        TextOutA(dc, 3, y + 1, l->items[i], ustrlen(l->items[i]));
        if (i == l->sel && U_focus() == w) DrawFocusRect(dc, &ir);
    }
    int vis = w->ch / rh;
    if (l->n > vis && vis > 0) {                                                  // a thin scroll bar
        RECT tr = { w->cw - 6, 0, w->cw, w->ch }; fill(dc, &tr, col(COLOR_BTNFACE));
        int th = w->ch * vis / l->n; if (th < 10) th = 10;
        int ty = (w->ch - th) * l->top / (l->n - vis);
        RECT thumb = { w->cw - 6, ty, w->cw, ty + th }; fill(dc, &thumb, col(16));
    }
    EndPaint(w, &ps);
}
static void lb_select(struct wnd *w, struct lbox *l, int i) {
    if (i < -1) i = -1;
    if (i >= l->n) i = l->n - 1;
    if (i == l->sel) return;
    l->sel = i;
    HDC dc = GetDC(w); apply_font(w, dc); int rh = lb_rowh(w, dc); ReleaseDC(w, dc);
    int vis = w->ch / rh; if (vis < 1) vis = 1;
    if (i >= 0 && i < l->top) l->top = i;
    if (i >= l->top + vis) l->top = i - vis + 1;
    U_invalidate(w, 0, 1);
    notify(w, LBN_SELCHANGE);
}
LRESULT U_listbox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct lbox *l = w->ctl;
    int hd; LRESULT r = common(w, m, wp, lp, &hd);
    if (hd) return r;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CREATE: l = umalloc(sizeof *l); w->ctl = l; if (l) l->sel = -1; return 0;
    case WM_NCDESTROY: if (l) { for (int i = 0; i < l->n; i++) ufree(l->items[i]); ufree(l->items); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: lb_paint(w); return 0;
    case LB_ADDSTRING: case LB_INSERTSTRING: {
        char t[512];
        const char *s = (const char*)lp;
        int at = m == LB_ADDSTRING ? l->n : (int)wp;
        if (w->unicode) { WideCharToMultiByte(65001, 0, (LPCWSTR)lp, -1, t, sizeof t, 0, 0); s = t; }
        if (at < 0 || at > l->n) at = l->n;
        if (l->n == l->cap) { int nc = l->cap ? l->cap * 2 : 16; char **ni = umalloc((SIZE_T)nc * sizeof(char*)); if (!ni) return -1; if (l->items) { memcpy(ni, l->items, (SIZE_T)l->n * sizeof(char*)); ufree(l->items); } l->items = ni; l->cap = nc; }
        char *cp = umalloc((SIZE_T)ustrlen(s) + 1); if (!cp) return -1;
        ustrcpy(cp, s, ustrlen(s) + 1);
        for (int i = l->n; i > at; i--) l->items[i] = l->items[i - 1];
        l->items[at] = cp; l->n++;
        U_invalidate(w, 0, 1);
        return at;
    }
    case LB_DELETESTRING: { int i = (int)wp; if (i < 0 || i >= l->n) return -1; ufree(l->items[i]); for (int k = i; k + 1 < l->n; k++) l->items[k] = l->items[k + 1]; l->n--; if (l->sel >= l->n) l->sel = l->n - 1; U_invalidate(w, 0, 1); return l->n; }
    case LB_RESETCONTENT: for (int i = 0; i < l->n; i++) ufree(l->items[i]); l->n = 0; l->sel = -1; l->top = 0; U_invalidate(w, 0, 1); return 0;
    case LB_GETCOUNT: return l->n;
    case LB_GETCURSEL: return l->sel < 0 ? -1 : l->sel;
    case LB_SETCURSEL: { int i = (int)wp; if (i >= l->n) i = -1; l->sel = i; if (i >= 0 && i < l->top) l->top = i; U_invalidate(w, 0, 1); return i < 0 ? -1 : i; }
    case LB_GETTEXTLEN: return (int)wp < l->n && (int)wp >= 0 ? ustrlen(l->items[wp]) : -1;
    case LB_GETTEXT: {
        if ((int)wp >= l->n || (int)wp < 0) return -1;
        if (w->unicode) return MultiByteToWideChar(65001, 0, l->items[wp], -1, (LPWSTR)lp, 512) - 1;
        ustrcpy((char*)lp, l->items[wp], 512); return ustrlen((char*)lp);
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (U_focus() != w) SetFocus(w);
        HDC dc = GetDC(w); apply_font(w, dc); int rh = lb_rowh(w, dc); ReleaseDC(w, dc);
        int i = l->top + GET_Y(lp) / rh;
        if (i < l->n) lb_select(w, l, i);
        if (m == WM_LBUTTONDBLCLK) notify(w, LBN_DBLCLK);
        return 0;
    }
    case WM_MOUSEWHEEL: { int d = (short)(wp >> 16) / 120; l->top -= d * 3; if (l->top < 0) l->top = 0; if (l->top > l->n - 1) l->top = l->n > 0 ? l->n - 1 : 0; U_invalidate(w, 0, 1); return 0; }
    case WM_KEYDOWN:
        if (wp == VK_UP) lb_select(w, l, l->sel <= 0 ? 0 : l->sel - 1);
        else if (wp == VK_DOWN) lb_select(w, l, l->sel + 1);
        else if (wp == 36) lb_select(w, l, 0);
        else if (wp == 35) lb_select(w, l, l->n - 1);
        return 0;
    default: return U_def_proc(w, m, wp, lp, w->unicode);
    }
}

void U_register_builtin(void) {
    HBRUSH face = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1), win = (HBRUSH)(ULONG_PTR)(COLOR_WINDOW + 1);
    U_add_class("BUTTON", U_button_proc, CS_DBLCLKS, 0, 0, face, 0, 0, 1);
    U_add_class("STATIC", U_static_proc, 0, 0, 0, face, 0, 0, 1);
    U_add_class("EDIT", U_edit_proc, CS_DBLCLKS, 0, 0, win, 0, 0, 1);
    U_add_class("LISTBOX", U_listbox_proc, CS_DBLCLKS, 0, 0, win, 0, 0, 1);
    U_add_class("SCROLLBAR", U_scrollbar_proc, CS_DBLCLKS, 0, 0, face, 0, 0, 1);
    U_add_class("COMBOBOX", U_combobox_proc, CS_DBLCLKS, 0, 0, win, 0, 0, 1);
    U_register_comctl();
}
