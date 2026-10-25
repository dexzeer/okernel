// user32: MessageBox and the dialog helper functions
#include "u32.h"

extern int DrawTextA(HDC dc, LPCSTR s, int n, RECT *rc, UINT fl);
extern HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern BOOL GetMessageA(MSG *m, HWND f, UINT lo, UINT hi);
extern LRESULT DispatchMessageA(const MSG *m);
extern void PostQuitMessage(int code);
extern ATOM RegisterClassExA(const WNDCLASSEXA *c);
extern BOOL EnableWindow(HWND h, BOOL e);

static LRESULT msgbox_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    if (m == WM_COMMAND) {
        int id = (int)(wp & 0xFFFF);
        if (id >= 1 && id <= 7) { w->dlg_result = id; w->dlg_done = 1; }
        return 0;
    }
    if (m == WM_CLOSE) { w->dlg_result = w->dlg_result ? w->dlg_result : IDCANCEL; w->dlg_done = 1; return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

void U_run_modal(struct wnd *dlg) {
    MSG msg;
    while (dlg->used && !dlg->dlg_done) {
        if (!GetMessageA(&msg, 0, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        DispatchMessageA(&msg);
    }
}

static int msgbox(HWND owner, const char *text, const char *caption, UINT type) {
    static int reg;
    U_init();
    if (!reg) {
        reg = 1;
        WNDCLASSEXA c; memset(&c, 0, sizeof c);
        c.cbSize = sizeof c; c.lpfnWndProc = msgbox_proc; c.lpszClassName = "#32770"; c.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
        RegisterClassExA(&c);
    }
    static const char *names[8] = { "", "OK", "Cancel", "Abort", "Retry", "Ignore", "Yes", "No" };
    int ids[3] = { IDOK, 0, 0 }, nb = 1;
    switch (type & 0xF) {
    case 1: ids[0] = IDOK; ids[1] = IDCANCEL; nb = 2; break;
    case 2: ids[0] = 3; ids[1] = 4; ids[2] = 5; nb = 3; break;
    case 3: ids[0] = IDYES; ids[1] = IDNO; ids[2] = IDCANCEL; nb = 3; break;
    case 4: ids[0] = IDYES; ids[1] = IDNO; nb = 2; break;
    case 5: ids[0] = 4; ids[1] = IDCANCEL; nb = 2; break;
    }
    HDC mdc = GetDC(0);
    RECT tr = { 0, 0, 360, 0 };
    DrawTextA(mdc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK);
    __gdi_free_dc(mdc);
    int tw = tr.right - tr.left, th = tr.bottom - tr.top;
    int bw = 80, bh = 26, gap = 8;
    int cw = tw + 40, need = nb * bw + (nb - 1) * gap + 40;
    if (cw < need) cw = need;
    int ch = th + 40 + bh + 20;
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    int ow = cw + 2 * border, oh = ch + title + 2 * border;
    HWND d = CreateWindowExA(0, "#32770", caption ? caption : "", WS_POPUP | WS_CAPTION | WS_SYSMENU, (sw - ow) / 2, (sh - oh) / 3, ow, oh, owner, 0, 0, 0);
    if (!d) return 0;
    struct wnd *dw = d;
    CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, tw + 2, th + 2, d, (HMENU)(LONG_PTR)-1, 0, 0);
    int x = (cw - (nb * bw + (nb - 1) * gap)) / 2;
    HWND first = 0;
    for (int i = 0; i < nb; i++) {
        HWND b = CreateWindowExA(0, "BUTTON", names[ids[i]], WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == 0 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x + i * (bw + gap), ch - bh - 16, bw, bh, d, (HMENU)(LONG_PTR)ids[i], 0, 0);
        if (!first) first = b;
    }
    ShowWindow(d, SW_SHOW);
    if (first) SetFocus(first);
    if (owner) EnableWindow(owner, FALSE);
    U_run_modal(dw);
    int res = dw->dlg_result;
    if (owner) EnableWindow(owner, TRUE);
    if (dw->used) U_destroy(dw);
    return res;
}
EXPORT int MessageBoxA(HWND owner, LPCSTR text, LPCSTR cap, UINT type) { return msgbox(owner, text ? text : "", cap, type); }
EXPORT int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR cap, UINT type) {
    char t[1024], c[128];
    if (text) WideCharToMultiByte(65001, 0, text, -1, t, sizeof t, 0, 0); else t[0] = 0;
    if (cap) WideCharToMultiByte(65001, 0, cap, -1, c, sizeof c, 0, 0); else c[0] = 0;
    return msgbox(owner, t, c, type);
}
EXPORT int MessageBoxExA(HWND owner, LPCSTR text, LPCSTR cap, UINT type, WORD lang) { (void)lang; return MessageBoxA(owner, text, cap, type); }
EXPORT int MessageBoxExW(HWND owner, LPCWSTR text, LPCWSTR cap, UINT type, WORD lang) { (void)lang; return MessageBoxW(owner, text, cap, type); }

EXPORT INT_PTR DialogBoxParamA(HINSTANCE h, LPCSTR t, HWND p, void *proc, LPARAM lp) { (void)h; (void)t; (void)p; (void)proc; (void)lp; return -1; }
EXPORT INT_PTR DialogBoxParamW(HINSTANCE h, LPCWSTR t, HWND p, void *proc, LPARAM lp) { (void)h; (void)t; (void)p; (void)proc; (void)lp; return -1; }
EXPORT HWND CreateDialogParamA(HINSTANCE h, LPCSTR t, HWND p, void *proc, LPARAM lp) { (void)h; (void)t; (void)p; (void)proc; (void)lp; return 0; }
EXPORT HWND CreateDialogParamW(HINSTANCE h, LPCWSTR t, HWND p, void *proc, LPARAM lp) { (void)h; (void)t; (void)p; (void)proc; (void)lp; return 0; }
EXPORT BOOL EndDialog(HWND h, INT_PTR r) { struct wnd *w = U_wnd(h); if (!w) return FALSE; w->dlg_result = (int)r; w->dlg_done = 1; return TRUE; }
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
EXPORT BOOL CheckRadioButton(HWND h, int first, int last, int check) {
    for (int i = first; i <= last; i++) CheckDlgButton(h, i, i == check);
    return TRUE;
}
EXPORT LRESULT SendDlgItemMessageA(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessageA(c, m, wp, lp) : 0; }
EXPORT LRESULT SendDlgItemMessageW(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) { HWND c = GetDlgItem(h, id); return c ? SendMessageW(c, m, wp, lp) : 0; }
EXPORT HWND GetNextDlgTabItem(HWND d, HWND c, BOOL prev) { (void)d; (void)c; (void)prev; return 0; }
EXPORT BOOL MapDialogRect(HWND h, RECT *r) { (void)h; (void)r; return TRUE; }
EXPORT LONG GetDialogBaseUnits(void) { return (LONG)((13 << 16) | 7); }

BOOL DllMain(HMODULE m, DWORD reason, LPVOID res) { (void)m; (void)reason; (void)res; return TRUE; }
