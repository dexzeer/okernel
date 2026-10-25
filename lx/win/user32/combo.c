// user32: the COMBOBOX control (drop-down list, drop-down with an edit field, simple = always open is treated like drop-down)
#include "u32.h"

extern HDC BeginPaint(HWND h, PAINTSTRUCT *ps);
extern BOOL EndPaint(HWND h, const PAINTSTRUCT *ps);
extern int DrawTextA(HDC dc, LPCSTR s, int n, RECT *rc, UINT fl);
extern HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern HMENU CreatePopupMenu(void);
extern BOOL AppendMenuA(HMENU m, UINT f, UINT_PTR id, LPCSTR s);
extern BOOL DestroyMenu(HMENU m);
extern int U_list_popup(struct wnd *owner, struct menu *m, int sx, int sy, int minw, int sel);
extern BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint);
extern BOOL DrawFocusRect(HDC dc, const RECT *r);
extern int FillRect(HDC dc, const RECT *r, HBRUSH b);

#define CBS_SIMPLE 1
#define CBS_DROPDOWN 2
#define CBS_DROPDOWNLIST 3
#define CBS_TYPE 3
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define CBN_DROPDOWN 7
#define CBN_CLOSEUP 8
#define CBN_SELENDOK 9
#define CB_GETEDITSEL 0x140
#define CB_LIMITTEXT 0x141
#define CB_SETEDITSEL 0x142
#define CB_ADDSTRING 0x143
#define CB_DELETESTRING 0x144
#define CB_GETCOUNT 0x146
#define CB_GETCURSEL 0x147
#define CB_GETLBTEXT 0x148
#define CB_GETLBTEXTLEN 0x149
#define CB_INSERTSTRING 0x14A
#define CB_RESETCONTENT 0x14B
#define CB_FINDSTRING 0x14C
#define CB_SELECTSTRING 0x14D
#define CB_SETCURSEL 0x14E
#define CB_SHOWDROPDOWN 0x14F
#define CB_GETITEMDATA 0x150
#define CB_SETITEMDATA 0x151
#define CB_SETITEMHEIGHT 0x153
#define CB_GETITEMHEIGHT 0x154
#define CB_GETDROPPEDSTATE 0x157
#define CB_FINDSTRINGEXACT 0x158
#define CB_SETDROPPEDWIDTH 0x160
#define BTN_W 17
#define ID_EDIT 1001

struct combo { char **it; ULONG_PTR *data; int n, cap, sel, dropped; struct wnd *edit; };

static int field_h(struct wnd *w) {
    HDC dc = __gdi_make_dc(0, 1, 1, 1);
    if (w->font) SelectObject(dc, w->font);
    int h = U_text_height(dc) + 8;
    __gdi_free_dc(dc);
    return h;
}
static int is_list(struct wnd *w) { return (w->style & CBS_TYPE) == CBS_DROPDOWNLIST; }
static void notify(struct wnd *w, int code) { if (w->parent) U_send(w->parent, WM_COMMAND, (WPARAM)((w->id & 0xFFFF) | ((unsigned)code << 16)), (LPARAM)w); }

static void layout_edit(struct wnd *w, struct combo *c) {
    if (!c->edit) return;
    int fh = field_h(w);
    MoveWindow(c->edit, 3, 3, w->cw - 3 - BTN_W - 2, fh - 6, TRUE);
}
static int cb_insert(struct combo *c, int at, const char *s) {
    if (at < 0 || at > c->n) at = c->n;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 16;
        char **ni = umalloc((SIZE_T)nc * sizeof(char*)); ULONG_PTR *nd = umalloc((SIZE_T)nc * sizeof(ULONG_PTR));
        if (!ni || !nd) return -1;
        if (c->it) { memcpy(ni, c->it, (SIZE_T)c->n * sizeof(char*)); memcpy(nd, c->data, (SIZE_T)c->n * sizeof(ULONG_PTR)); ufree(c->it); ufree(c->data); }
        c->it = ni; c->data = nd; c->cap = nc;
    }
    char *cp = umalloc((SIZE_T)ustrlen(s) + 1);
    if (!cp) return -1;
    ustrcpy(cp, s, ustrlen(s) + 1);
    for (int i = c->n; i > at; i--) { c->it[i] = c->it[i - 1]; c->data[i] = c->data[i - 1]; }
    c->it[at] = cp; c->data[at] = 0; c->n++;
    if (c->sel >= at && c->sel >= 0 && c->n > 1) c->sel++;
    return at;
}
static void select_item(struct wnd *w, struct combo *c, int i) {
    c->sel = i;
    if (c->edit && i >= 0 && i < c->n) U_send(c->edit, WM_SETTEXT, 0, (LPARAM)c->it[i]);
    U_invalidate(w, 0, 0);
}
static void open_list(struct wnd *w, struct combo *c) {
    if (c->dropped || !c->n) return;
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < c->n; i++) {
        char esc[200]; int k = 0;
        for (const char *p = c->it[i]; *p && k < 190; p++) { if (*p == '&') esc[k++] = '&'; esc[k++] = *p; }
        esc[k] = 0;
        AppendMenuA(m, 0, (UINT_PTR)(i + 1), esc);
    }
    int sx, sy; U_screen_origin(w, &sx, &sy);
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    int fh = field_h(w);
    c->dropped = 1;
    notify(w, CBN_DROPDOWN);
    int id = U_list_popup(w, U_menu(m), sx, sy + fh, w->w - 2 * border, c->sel);
    c->dropped = 0;
    DestroyMenu(m);
    notify(w, CBN_CLOSEUP);
    if (id > 0) { select_item(w, c, id - 1); notify(w, CBN_SELENDOK); notify(w, CBN_SELCHANGE); }
}
static int find_string(struct combo *c, int from, const char *s, int exact) {
    int n = ustrlen(s);
    for (int k = 1; k <= c->n; k++) {
        int i = (from + k) % c->n;
        if (exact ? !ustricmp(c->it[i], s) : !ustricmp_n(c->it[i], s, n)) return i;
    }
    return -1;
}

LRESULT U_combobox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct combo *c = w->ctl;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CREATE:
        c = umalloc(sizeof *c); w->ctl = c;
        if (c) { memset(c, 0, sizeof *c); c->sel = -1; }
        if (c && !is_list(w)) {
            c->edit = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 3, 3, w->cw - 3 - BTN_W - 2, field_h(w) - 6, w, (HMENU)(LONG_PTR)ID_EDIT, 0, 0);
            if (c->edit && w->font) U_send(c->edit, WM_SETFONT, (WPARAM)w->font, 0);
        }
        return 0;
    case WM_NCDESTROY: if (c) { for (int i = 0; i < c->n; i++) ufree(c->it[i]); ufree(c->it); ufree(c->data); } return 0;
    case WM_SIZE: layout_edit(w, c); return 0;
    case WM_SETFONT: w->font = (void*)wp; if (c->edit) U_send(c->edit, WM_SETFONT, wp, lp); layout_edit(w, c); U_invalidate(w, 0, 1); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: if (c->edit) SetFocus(c->edit); U_invalidate(w, 0, 0); notify(w, CBN_SETFOCUS); return 0;
    case WM_KILLFOCUS: U_invalidate(w, 0, 0); notify(w, CBN_KILLFOCUS); return 0;
    case WM_ENABLE: U_invalidate(w, 0, 1); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (!dc) return 0;
        int fh = field_h(w);
        RECT fr = { 0, 0, w->cw, fh };
        __gdi_rect_fill_color(dc, 0, 0, w->cw, w->ch, RGB(240, 240, 240));                         // (what the parent shows under the closed list part)
        __gdi_rect_fill_color(dc, 0, 0, w->cw, fh, w->enabled ? RGB(255, 255, 255) : RGB(240, 240, 240));
        { COLORREF bc = w->enabled ? RGB(130, 130, 130) : RGB(190, 190, 190);                  // a flat one pixel frame
          __gdi_rect_fill_color(dc, 0, 0, w->cw, 1, bc); __gdi_rect_fill_color(dc, 0, fh - 1, w->cw, fh, bc);
          __gdi_rect_fill_color(dc, 0, 0, 1, fh, bc); __gdi_rect_fill_color(dc, w->cw - 1, 0, w->cw, fh, bc); (void)fr; }
        int bx = w->cw - BTN_W - 1;
        __gdi_rect_fill_color(dc, bx, 1, w->cw - 1, fh - 1, RGB(225, 225, 225));
        for (int i = 0; i < 4; i++) __gdi_rect_fill_color(dc, bx + BTN_W / 2 - 3 + i, fh / 2 - 2 + i, bx + BTN_W / 2 + 4 - i, fh / 2 - 1 + i, w->enabled ? RGB(80, 80, 80) : RGB(170, 170, 170));
        if (is_list(w)) {
            if (w->font) SelectObject(dc, w->font);
            SetBkMode(dc, TRANSPARENT);
            int focused = U_focus() == w;
            RECT tr = { 4, 2, bx - 2, fh - 2 };
            if (focused) __gdi_rect_fill_color(dc, 2, 2, bx - 1, fh - 2, RGB(0, 120, 215));
            SetTextColor(dc, focused ? RGB(255, 255, 255) : w->enabled ? RGB(0, 0, 0) : RGB(150, 150, 150));
            if (c->sel >= 0 && c->sel < c->n) DrawTextA(dc, c->it[c->sel], -1, &tr, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (!w->enabled) return 0;
        int bx = w->cw - BTN_W - 1;
        if (is_list(w) || GET_X(lp) >= bx) { if (U_focus() != w && is_list(w)) SetFocus(w); if (GET_Y(lp) < field_h(w)) open_list(w, c); }
        return 0;
    }
    case WM_KEYDOWN:
        if (!is_list(w)) return 0;
        if (wp == VK_DOWN && c->sel + 1 < c->n) { select_item(w, c, c->sel + 1); notify(w, CBN_SELCHANGE); }
        else if (wp == VK_UP && c->sel > 0) { select_item(w, c, c->sel - 1); notify(w, CBN_SELCHANGE); }
        else if (wp == 0x73 /* F4 */) open_list(w, c);
        return 0;
    case WM_CHAR:
        if (is_list(w) && wp >= 32 && c->n) { char s[2] = { (char)wp, 0 }; int i = find_string(c, c->sel, s, 0); if (i >= 0) { select_item(w, c, i); notify(w, CBN_SELCHANGE); } }
        return 0;
    case WM_COMMAND:
        if (lp && (struct wnd*)lp == c->edit && HIWORD(wp) == 0x300) notify(w, CBN_EDITCHANGE);
        return 0;
    case WM_SETTEXT: {
        if (c->edit) return U_send(c->edit, WM_SETTEXT, wp, lp);
        char t[200];
        if (w->unicode) WideCharToMultiByte(65001, 0, (LPCWSTR)lp, -1, t, sizeof t, 0, 0); else ustrcpy(t, (LPCSTR)lp, sizeof t);
        int i = find_string(c, -1, t, 1);
        if (i >= 0) select_item(w, c, i);
        return TRUE;
    }
    case WM_GETTEXT: case WM_GETTEXTLENGTH: {
        if (c->edit) return U_send(c->edit, m, wp, lp);
        const char *s = (c->sel >= 0 && c->sel < c->n) ? c->it[c->sel] : "";
        if (m == WM_GETTEXTLENGTH) return ustrlen(s);
        if (!wp) return 0;
        if (w->unicode) return MultiByteToWideChar(65001, 0, s, -1, (LPWSTR)lp, (int)wp) - 1;
        ustrcpy((char*)lp, s, (int)wp);
        return ustrlen((char*)lp);
    }
    case CB_ADDSTRING: case CB_INSERTSTRING: {
        char t[200];
        if (w->unicode) WideCharToMultiByte(65001, 0, (LPCWSTR)lp, -1, t, sizeof t, 0, 0); else ustrcpy(t, (LPCSTR)lp, sizeof t);
        return cb_insert(c, m == CB_ADDSTRING ? -1 : (int)wp, t);
    }
    case CB_DELETESTRING: {
        int i = (int)wp;
        if (i < 0 || i >= c->n) return -1;
        ufree(c->it[i]);
        for (int k = i; k + 1 < c->n; k++) { c->it[k] = c->it[k + 1]; c->data[k] = c->data[k + 1]; }
        c->n--;
        if (c->sel >= c->n) c->sel = c->n - 1;
        U_invalidate(w, 0, 0);
        return c->n;
    }
    case CB_RESETCONTENT: for (int i = 0; i < c->n; i++) ufree(c->it[i]); c->n = 0; c->sel = -1; if (c->edit) U_send(c->edit, WM_SETTEXT, 0, (LPARAM)""); U_invalidate(w, 0, 0); return 0;
    case CB_GETCOUNT: return c->n;
    case CB_GETCURSEL: return c->sel;
    case CB_SETCURSEL: { int i = (int)wp; if (i >= c->n) i = -1; c->sel = i; if (c->edit && i >= 0) U_send(c->edit, WM_SETTEXT, 0, (LPARAM)c->it[i]); U_invalidate(w, 0, 0); return i < 0 ? -1 : i; }
    case CB_GETLBTEXT: {
        int i = (int)wp;
        if (i < 0 || i >= c->n) return -1;
        if (w->unicode) return MultiByteToWideChar(65001, 0, c->it[i], -1, (LPWSTR)lp, 256) - 1;
        ustrcpy((char*)lp, c->it[i], 256);
        return ustrlen((char*)lp);
    }
    case CB_GETLBTEXTLEN: return (int)wp >= 0 && (int)wp < c->n ? ustrlen(c->it[wp]) : -1;
    case CB_FINDSTRING: case CB_FINDSTRINGEXACT: case CB_SELECTSTRING: {
        char t[200];
        if (w->unicode) WideCharToMultiByte(65001, 0, (LPCWSTR)lp, -1, t, sizeof t, 0, 0); else ustrcpy(t, (LPCSTR)lp, sizeof t);
        if (!c->n) return -1;
        int i = find_string(c, (int)wp, t, m == CB_FINDSTRINGEXACT);
        if (m == CB_SELECTSTRING && i >= 0) select_item(w, c, i);
        return i;
    }
    case CB_GETITEMDATA: return (int)wp >= 0 && (int)wp < c->n ? (LRESULT)c->data[wp] : -1;
    case CB_SETITEMDATA: if ((int)wp < 0 || (int)wp >= c->n) return -1; c->data[wp] = (ULONG_PTR)lp; return 0;
    case CB_SHOWDROPDOWN: if (wp) open_list(w, c); return TRUE;
    case CB_GETDROPPEDSTATE: return c->dropped;
    case CB_LIMITTEXT: if (c->edit) U_send(c->edit, EM_LIMITTEXT, wp, 0); return TRUE;
    case CB_SETITEMHEIGHT: case CB_GETITEMHEIGHT: case CB_SETDROPPEDWIDTH: return 20;
    default: return U_def_proc(w, m, wp, lp, w->unicode);
    }
}
