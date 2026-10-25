// user32: dialog boxes: templates (DLGTEMPLATE and DLGTEMPLATEEX from resources or memory), modal and modeless dialogs, the
// dialog window procedure, IsDialogMessage (Tab, Enter, Escape, radio button arrows) and the dialog helper functions.
#include "u32.h"

extern HRSRC FindResourceExW(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang);
extern HGLOBAL LoadResource(HMODULE m, HRSRC r);
extern HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern BOOL GetMessageA(MSG *m, HWND f, UINT lo, UINT hi);
extern BOOL TranslateMessage(const MSG *m);
extern LRESULT DispatchMessageA(const MSG *m);
extern void PostQuitMessage(int code);
extern ATOM RegisterClassExA(const WNDCLASSEXA *c);
extern BOOL EnableWindow(HWND h, BOOL e);
extern BOOL AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex);
extern BOOL GetWindowRect(HWND h, RECT *r);
extern BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint);
extern BOOL IsWindowVisible(HWND h);
extern BOOL DestroyWindow(HWND h);
extern HWND GetParent(HWND h);
extern BOOL BringWindowToTop(HWND h);

#define DS_SETFONT 0x40
#define DS_MODALFRAME 0x80
#define DS_CENTER 0x0800
#define DM_GETDEFID 0x400
#define DM_SETDEFID 0x401
#define DC_HASDEFID 0x534B
#define WM_NEXTDLGCTL 0x28
#define DWLP_MSGRESULT 0
#define DWLP_DLGPROC 8
#define DWLP_USER 16
#define BS_TYPEMASK_ 0xF

typedef INT_PTR (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);

// ---- dialog units --------------------------------------------------------------------------------------------------------------

static int base_x, base_y;
static void base_units(void) {
    if (base_x) return;
    HDC dc = __gdi_make_dc(0, 1, 1, 1);
    SIZE sz; GetTextExtentPoint32A(dc, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", 52, &sz);
    base_x = (sz.cx / 26 + 1) / 2;
    base_y = sz.cy;
    __gdi_free_dc(dc);
    if (base_x < 4) base_x = 6;
    if (base_y < 8) base_y = 13;
}
EXPORT LONG GetDialogBaseUnits(void) { base_units(); return (LONG)(((unsigned)base_y << 16) | (unsigned)base_x); }
EXPORT BOOL MapDialogRect(HWND h, RECT *r) {
    (void)h; base_units();
    r->left = r->left * base_x / 4; r->right = r->right * base_x / 4;
    r->top = r->top * base_y / 8; r->bottom = r->bottom * base_y / 8;
    return TRUE;
}

// ---- reading templates -----------------------------------------------------------------------------------------------------------

struct rd { const BYTE *base, *p; };
static unsigned rd16(struct rd *r) { unsigned v = *(const WORD*)r->p; r->p += 2; return v; }
static unsigned rd32(struct rd *r) { unsigned v = *(const DWORD*)r->p; r->p += 4; return v; }
static void align4(struct rd *r) { r->p = r->base + (((SIZE_T)(r->p - r->base) + 3) & ~(SIZE_T)3); }
// a "name or ordinal": 0000 = none, FFFF + id = ordinal, else a NUL terminated string. Returns the ordinal (0 for a string/none).
static unsigned rd_name(struct rd *r, const WCHAR **str) {
    *str = 0;
    unsigned w = *(const WORD*)r->p;
    if (w == 0) { r->p += 2; return 0; }
    if (w == 0xFFFF) { r->p += 2; return rd16(r); }
    *str = (const WCHAR*)r->p;
    while (*(const WORD*)r->p) r->p += 2;
    r->p += 2;
    return 0;
}

struct dlgctl { DWORD style, ex; int x, y, cx, cy; DWORD id; unsigned cls_atom, title_atom; const WCHAR *cls, *title; const BYTE *extra; int extra_n; };
struct dlg { DWORD style, ex; int n; int x, y, cx, cy; const WCHAR *title; int ex_fmt; int has_font; int pt; const WCHAR *face; const BYTE *items; struct rd cur; };

static int parse_header(const void *tmpl, struct dlg *d) {
    struct rd r = { tmpl, tmpl };
    memset(d, 0, sizeof *d);
    int ex = *(const WORD*)((const BYTE*)tmpl + 2) == 0xFFFF;
    d->ex_fmt = ex;
    if (ex) { r.p += 4; rd32(&r); d->ex = rd32(&r); d->style = rd32(&r); }
    else { d->style = rd32(&r); d->ex = rd32(&r); }
    d->n = (int)rd16(&r);
    d->x = (short)rd16(&r); d->y = (short)rd16(&r); d->cx = (short)rd16(&r); d->cy = (short)rd16(&r);
    const WCHAR *s;
    rd_name(&r, &s);                                                   // menu
    rd_name(&r, &s);                                                   // class
    rd_name(&r, &d->title);                                            // the title is a string (or an ordinal that means "none")
    if (d->style & DS_SETFONT) {
        d->has_font = 1;
        d->pt = (int)rd16(&r);
        if (ex) { rd16(&r); r.p += 2; }                                // weight, italic, charset
        d->face = (const WCHAR*)r.p;
        while (*(const WORD*)r.p) r.p += 2;
        r.p += 2;
    }
    align4(&r);
    d->cur = r;
    return 1;
}
static int next_control(struct dlg *d, struct dlgctl *c) {
    struct rd *r = &d->cur;
    memset(c, 0, sizeof *c);
    if (d->ex_fmt) { rd32(r); c->ex = rd32(r); c->style = rd32(r); }
    else { c->style = rd32(r); c->ex = rd32(r); }
    c->x = (short)rd16(r); c->y = (short)rd16(r); c->cx = (short)rd16(r); c->cy = (short)rd16(r);
    c->id = d->ex_fmt ? rd32(r) : rd16(r);
    c->cls_atom = rd_name(r, &c->cls);
    c->title_atom = rd_name(r, &c->title);
    int extra = (int)rd16(r);
    c->extra = r->p; c->extra_n = extra;
    r->p += extra;
    align4(r);
    return 1;
}
static const char *atom_class(unsigned a) {
    switch (a) { case 0x80: return "BUTTON"; case 0x81: return "EDIT"; case 0x82: return "STATIC"; case 0x83: return "LISTBOX"; case 0x84: return "SCROLLBAR"; case 0x85: return "COMBOBOX"; default: return 0; }
}

// ---- dialog window procedure ---------------------------------------------------------------------------------------------------------

static LRESULT dlg_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    DLGPROC dp = (DLGPROC)GetWindowLongPtrA(h, DWLP_DLGPROC);
    if (!dp) return DefWindowProcA(h, m, wp, lp);
    if (m == WM_GETFONT) return (LRESULT)w->font;
    INT_PTR r = dp(h, m, wp, lp);
    switch (m) {
    case WM_INITDIALOG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLOREDIT: case 0x132 /* DLG */: case 0x134 /* LISTBOX */:
    case 0x39 /* COMPAREITEM */: case 0x2E /* VKTOITEM */: case 0x2F /* CHARTOITEM */: case 0x37 /* QUERYDRAGICON */:
        return r;
    default: break;
    }
    if (r) return GetWindowLongPtrA(h, DWLP_MSGRESULT);
    switch (m) {
    case DM_GETDEFID: return w->dlg_defid ? (LRESULT)((DC_HASDEFID << 16) | (w->dlg_defid & 0xFFFF)) : 0;
    case DM_SETDEFID: w->dlg_defid = (int)wp; return TRUE;
    case WM_CLOSE: { HWND c = GetDlgItem(h, IDCANCEL); if (c && IsWindowEnabled(c)) U_send(w, WM_COMMAND, (WPARAM)IDCANCEL, (LPARAM)c); return 0; }
    case WM_NEXTDLGCTL: return 0;
    default: return DefWindowProcA(h, m, wp, lp);
    }
}

static void register_dialog_class(void) {
    static int done;
    if (done) return;
    done = 1;
    WNDCLASSEXA c; memset(&c, 0, sizeof c);
    c.cbSize = sizeof c; c.lpfnWndProc = dlg_wndproc; c.lpszClassName = "#32770"; c.cbWndExtra = 32;
    c.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
    RegisterClassExA(&c);
}

// the dialog manager moves the focus: an edit control gets all its text selected
static void dlg_set_focus(struct wnd *c) {
    SetFocus(c);
    if (c->cls && c->cls->builtin && !ustricmp(c->cls->name, "EDIT")) U_send(c, 0xB1 /* EM_SETSEL */, 0, (LPARAM)-1);
}

static struct wnd *first_tabstop(struct wnd *d) {
    for (struct wnd *c = d->first; c; c = c->next) if ((c->style & WS_TABSTOP) && c->visible && c->enabled) return c;
    return 0;
}

// Build the dialog window and its controls. Returns the window (not yet shown) or 0.
static HWND create_dialog(HINSTANCE inst, const void *tmpl, HWND owner, DLGPROC dp, LPARAM init, int *visible) {
    U_init();
    register_dialog_class();
    base_units();
    struct dlg d;
    parse_header(tmpl, &d);
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    int cw = d.cx * base_x / 4, ch = d.cy * base_y / 8;
    DWORD style = d.style & ~(DWORD)(WS_VISIBLE | DS_SETFONT | DS_CENTER | DS_MODALFRAME);
    if (!(style & WS_CAPTION) && !(style & WS_CHILD)) style |= WS_POPUP | WS_CAPTION;           // a dialog without a caption still gets one here: the desktop draws it
    *visible = (d.style & WS_VISIBLE) != 0;
    RECT rc = { 0, 0, cw, ch };
    AdjustWindowRectEx(&rc, style, FALSE, d.ex);
    int ow = rc.right - rc.left, oh = rc.bottom - rc.top;
    int x = d.x * base_x / 4, y = d.y * base_y / 8;
    struct wnd *ow_ = U_wnd(owner);
    if (ow_ && !(d.style & DS_CENTER) && x == 0 && y == 0) d.style |= DS_CENTER;
    if (d.style & DS_CENTER || (x == 0 && y == 0)) {
        RECT or_ = { 0, 0, sw, sh };
        if (ow_ && !ow_->parent) GetWindowRect(ow_, &or_);
        x = or_.left + ((or_.right - or_.left) - ow) / 2;
        y = or_.top + ((or_.bottom - or_.top) - oh) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (y + oh > sh - 40) y = sh - 40 - oh;
    }
    char tbuf[128] = "";
    if (d.title) WideCharToMultiByte(65001, 0, d.title, -1, tbuf, sizeof tbuf, 0, 0);
    HWND hw = U_create_window(d.ex, "#32770", tbuf, style & ~WS_VISIBLE, x, y, ow, oh, owner, 0, inst, 0, 0);
    if (!hw) return 0;
    struct wnd *dw = hw;
    SetWindowLongPtrA(hw, DWLP_DLGPROC, (LONG_PTR)dp);
    SetWindowLongPtrA(hw, DWLP_USER, 0);
    // the controls
    for (int i = 0; i < d.n; i++) {
        struct dlgctl c;
        next_control(&d, &c);
        const char *an = c.cls_atom ? atom_class(c.cls_atom) : 0;
        WCHAR wc[64];
        const WCHAR *clsw = c.cls;
        if (an) { MultiByteToWideChar(65001, 0, an, -1, wc, 64); clsw = wc; }
        if (!clsw) continue;
        static const WCHAR empty[1] = { 0 };
        const WCHAR *tw = c.title_atom || !c.title ? empty : c.title;
        HWND ch_ = CreateWindowExW(c.ex, clsw, tw, (c.style | WS_CHILD) & ~(DWORD)0, c.x * base_x / 4, c.y * base_y / 8, c.cx * base_x / 4, c.cy * base_y / 8, hw, (HMENU)(ULONG_PTR)c.id, inst, 0);
        (void)ch_;
    }
    // the initial focus: the first tab stop (or what the dialog procedure picks)
    struct wnd *ft = first_tabstop(dw);
    INT_PTR r = U_send(dw, WM_INITDIALOG, (WPARAM)ft, init);
    if (r && ft) dlg_set_focus(ft);
    return hw;
}

EXPORT HWND CreateDialogIndirectParamA(HINSTANCE inst, const void *tmpl, HWND owner, DLGPROC dp, LPARAM init) {
    int vis;
    HWND h = create_dialog(inst, tmpl, owner, dp, init, &vis);
    if (h && vis) ShowWindow(h, SW_SHOW);
    return h;
}
EXPORT HWND CreateDialogIndirectParamW(HINSTANCE inst, const void *tmpl, HWND owner, DLGPROC dp, LPARAM init) { return CreateDialogIndirectParamA(inst, tmpl, owner, dp, init); }
static const void *find_dialog(HINSTANCE inst, LPCWSTR name) {
    HRSRC r = FindResourceExW(inst, (LPCWSTR)(ULONG_PTR)5, name, 0);               // RT_DIALOG
    return r ? LoadResource(inst, r) : 0;
}
EXPORT HWND CreateDialogParamW(HINSTANCE inst, LPCWSTR name, HWND owner, DLGPROC dp, LPARAM init) {
    const void *t = find_dialog(inst, name);
    return t ? CreateDialogIndirectParamA(inst, t, owner, dp, init) : 0;
}
EXPORT HWND CreateDialogParamA(HINSTANCE inst, LPCSTR name, HWND owner, DLGPROC dp, LPARAM init) {
    WCHAR w[64];
    if ((ULONG_PTR)name < 0x10000) return CreateDialogParamW(inst, (LPCWSTR)name, owner, dp, init);
    MultiByteToWideChar(65001, 0, name, -1, w, 64);
    return CreateDialogParamW(inst, w, owner, dp, init);
}

EXPORT INT_PTR DialogBoxIndirectParamA(HINSTANCE inst, const void *tmpl, HWND owner, DLGPROC dp, LPARAM init) {
    int vis;
    HWND h = create_dialog(inst, tmpl, owner, dp, init, &vis);
    if (!h) return -1;
    struct wnd *dw = h;
    struct wnd *ow = U_wnd(owner);
    if (ow) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    if (!(U_focus() && U_top(U_focus()) == dw)) { struct wnd *ft = first_tabstop(dw); if (ft) dlg_set_focus(ft); else SetFocus(dw); }
    MSG msg;
    while (dw->used && !dw->dlg_done) {
        if (!GetMessageA(&msg, 0, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        if (IsDialogMessageA(h, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    INT_PTR res = dw->dlg_result;
    if (ow) { EnableWindow(owner, TRUE); SC3(LX_ioctl, U_fd(), OKW_FOCUS, U_top(ow)->wid); }
    if (dw->used) DestroyWindow(h);
    return res;
}
EXPORT INT_PTR DialogBoxIndirectParamW(HINSTANCE inst, const void *tmpl, HWND owner, DLGPROC dp, LPARAM init) { return DialogBoxIndirectParamA(inst, tmpl, owner, dp, init); }
EXPORT INT_PTR DialogBoxParamW(HINSTANCE inst, LPCWSTR name, HWND owner, DLGPROC dp, LPARAM init) {
    const void *t = find_dialog(inst, name);
    return t ? DialogBoxIndirectParamA(inst, t, owner, dp, init) : -1;
}
EXPORT INT_PTR DialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND owner, DLGPROC dp, LPARAM init) {
    WCHAR w[64];
    if ((ULONG_PTR)name < 0x10000) return DialogBoxParamW(inst, (LPCWSTR)name, owner, dp, init);
    MultiByteToWideChar(65001, 0, name, -1, w, 64);
    return DialogBoxParamW(inst, w, owner, dp, init);
}
EXPORT BOOL EndDialog(HWND h, INT_PTR r) { struct wnd *w = U_wnd(h); if (!w) return FALSE; w->dlg_result = (int)r; w->dlg_done = 1; return TRUE; }

// ---- keyboard handling inside dialogs --------------------------------------------------------------------------------------------------

static int is_tab_item(struct wnd *c) { return (c->style & WS_TABSTOP) && c->visible && c->enabled; }
static struct wnd *tab_step(struct wnd *dlg, struct wnd *from, int prev) {
    if (!dlg->first) return 0;
    struct wnd *c = from ? (prev ? from->prev : from->next) : (prev ? dlg->last : dlg->first);
    for (int k = 0; k < 256; k++) {
        if (!c) c = prev ? dlg->last : dlg->first;
        if (c == from) return from;
        if (is_tab_item(c)) return c;
        c = prev ? c->prev : c->next;
    }
    return 0;
}
EXPORT HWND GetNextDlgTabItem(HWND d, HWND c, BOOL prev) { struct wnd *dw = U_wnd(d); return dw ? tab_step(dw, U_wnd(c), prev) : 0; }
EXPORT HWND GetNextDlgGroupItem(HWND d, HWND c, BOOL prev) {
    struct wnd *dw = U_wnd(d), *cw = U_wnd(c);
    if (!dw || !cw) return 0;
    struct wnd *n = prev ? cw->prev : cw->next;
    return n && !(n->style & WS_GROUP) ? n : (prev ? cw : n);
}
static int btn_kind(struct wnd *c) { return c->cls && c->cls->builtin && !ustricmp(c->cls->name, "BUTTON") ? (int)(c->style & BS_TYPEMASK_) : -1; }

static struct wnd *radio_next(struct wnd *f, int back) {
    struct wnd *s = f;
    while (s->prev && !(s->style & WS_GROUP) && btn_kind(s->prev) == BS_AUTORADIOBUTTON) s = s->prev;
    if (!back) { struct wnd *n = f->next; return (n && !(n->style & WS_GROUP) && btn_kind(n) == BS_AUTORADIOBUTTON) ? n : s; }
    if (f != s) return f->prev;
    struct wnd *e = f;
    while (e->next && !(e->next->style & WS_GROUP) && btn_kind(e->next) == BS_AUTORADIOBUTTON) e = e->next;
    return e;
}

static struct wnd *default_button(struct wnd *d) {
    if (d->dlg_defid) { HWND b = GetDlgItem(d, d->dlg_defid); if (b) return b; }
    for (struct wnd *c = d->first; c; c = c->next) if (btn_kind(c) == BS_DEFPUSHBUTTON) return c;
    return 0;
}

EXPORT BOOL IsDialogMessageA(HWND hd, MSG *m) {
    struct wnd *d = U_wnd(hd), *t = U_wnd(m->hwnd);
    if (!d || !t || (t != d && !IsChild(d, t))) return FALSE;
    if (m->message != WM_KEYDOWN) return FALSE;
    struct wnd *f = U_focus();
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (m->wParam) {
    case VK_TAB: {
        struct wnd *n = tab_step(d, f && f != d && IsChild(d, f) ? f : 0, shift);
        if (n) dlg_set_focus(n);
        return TRUE;
    }
    case VK_RETURN: {
        if (f && f != d && btn_kind(f) >= 0 && btn_kind(f) <= BS_DEFPUSHBUTTON) { U_send(f, BM_CLICK, 0, 0); return TRUE; }
        if (f && f->cls && f->cls->builtin && !ustricmp(f->cls->name, "EDIT") && (f->style & 4 /* ES_MULTILINE */) && (f->style & 0x1000 /* ES_WANTRETURN */)) return FALSE;
        struct wnd *b = default_button(d);
        if (b && b->enabled) { U_send(d, WM_COMMAND, (WPARAM)(b->id & 0xFFFF), (LPARAM)b); return TRUE; }
        U_send(d, WM_COMMAND, (WPARAM)IDOK, 0);
        return TRUE;
    }
    case VK_ESCAPE: {
        HWND c = GetDlgItem(d, IDCANCEL);
        U_send(d, WM_COMMAND, (WPARAM)IDCANCEL, (LPARAM)c);
        return TRUE;
    }
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        if (f && btn_kind(f) == BS_AUTORADIOBUTTON) {                               // arrows move through a radio group and check the target
            int back = m->wParam == VK_LEFT || m->wParam == VK_UP;
            struct wnd *n = radio_next(f, back);
            if (n && n != f) { SetFocus(n); U_send(n, BM_CLICK, 0, 0); }
            return TRUE;
        }
        return FALSE;
    default: return FALSE;
    }
}
EXPORT BOOL IsDialogMessageW(HWND hd, MSG *m) { return IsDialogMessageA(hd, m); }

// ---- helpers ----------------------------------------------------------------------------------------------------------------------------

EXPORT UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sgn) {
    char b[32]; GetWindowTextA(GetDlgItem(h, id), b, sizeof b);
    long v = 0; int i = 0, neg = 0; if (sgn && b[0] == '-') { neg = 1; i++; }
    for (; b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
    if (ok) *ok = i > 0;
    return (UINT)(neg ? -v : v);
}
EXPORT BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sgn) {
    char b[32]; int n = 0; long long x = sgn ? (long long)(int)v : (long long)v; int neg = x < 0; if (neg) x = -x;
    char r[24]; int k = 0; do { r[k++] = (char)('0' + x % 10); x /= 10; } while (x);
    if (neg) b[n++] = '-';
    while (k) b[n++] = r[--k];
    b[n] = 0;
    return SetDlgItemTextA(h, id, b);
}
EXPORT BOOL CheckDlgButton(HWND h, int id, UINT c) { HWND b = GetDlgItem(h, id); if (!b) return FALSE; SendMessageA(b, BM_SETCHECK, c, 0); return TRUE; }
EXPORT UINT IsDlgButtonChecked(HWND h, int id) { HWND b = GetDlgItem(h, id); return b ? (UINT)SendMessageA(b, BM_GETCHECK, 0, 0) : 0; }
EXPORT BOOL CheckRadioButton(HWND h, int first, int last, int check) { for (int i = first; i <= last; i++) CheckDlgButton(h, i, i == check); return TRUE; }
EXPORT LRESULT SendDlgItemMessageA(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessageA(c, m, wp, lp) : 0; }
EXPORT LRESULT SendDlgItemMessageW(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessageW(c, m, wp, lp) : 0; }
EXPORT BOOL SetDlgItemTextW(HWND h, int id, LPCWSTR s) { HWND c = GetDlgItem(h, id); return c ? SetWindowTextW(c, s) : FALSE; }
EXPORT UINT GetDlgItemTextW(HWND h, int id, LPWSTR out, int cap) { HWND c = GetDlgItem(h, id); return c ? (UINT)GetWindowTextW(c, out, cap) : 0; }
EXPORT LRESULT DefDlgProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, m, wp, lp); }
EXPORT LRESULT DefDlgProcW(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, m, wp, lp); }
