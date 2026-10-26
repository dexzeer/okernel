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
        if (id >= 1) { w->dlg_result = id; w->dlg_done = 1; }
        return 0;
    }
    if (m == WM_CLOSE) { w->dlg_result = w->dlg_result ? w->dlg_result : IDCANCEL; w->dlg_done = 1; return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

void U_run_modal(struct wnd *dlg) {
    MSG msg;
    while (dlg->used && !dlg->dlg_done) {
        if (!GetMessageA(&msg, 0, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        if (IsDialogMessageA(dlg, &msg)) continue;                                  // Tab, Enter (the default button), Escape
        DispatchMessageA(&msg);
    }
}

static int taskbox(HWND owner, const char *text, const char *caption, int nb, const int *ids, const char *const *names, int def);

static int msgbox(HWND owner, const char *text, const char *caption, UINT type) {
    U_init();
    static const char *const names[8] = { "", "OK", "Cancel", "Abort", "Retry", "Ignore", "Yes", "No" };
    int ids[3] = { IDOK, 0, 0 }, nb = 1;
    switch (type & 0xF) {
    case 1: ids[0] = IDOK; ids[1] = IDCANCEL; nb = 2; break;
    case 2: ids[0] = 3; ids[1] = 4; ids[2] = 5; nb = 3; break;
    case 3: ids[0] = IDYES; ids[1] = IDNO; ids[2] = IDCANCEL; nb = 3; break;
    case 4: ids[0] = IDYES; ids[1] = IDNO; nb = 2; break;
    case 5: ids[0] = 4; ids[1] = IDCANCEL; nb = 2; break;
    }
    const char *lab[3];
    for (int i = 0; i < nb; i++) lab[i] = names[ids[i]];
    return taskbox(owner, text, caption, nb, ids, lab, 0);
}

static int taskbox(HWND owner, const char *text, const char *caption, int nb, const int *ids, const char *const *names, int def) {
    static int reg;
    if (!reg) {
        reg = 1;
        WNDCLASSEXA c; memset(&c, 0, sizeof c);
        c.cbSize = sizeof c; c.lpfnWndProc = msgbox_proc; c.lpszClassName = "OKMsgBox"; c.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
        RegisterClassExA(&c);
    }
    HDC mdc = GetDC(0);
    RECT tr = { 0, 0, 360, 0 };
    DrawTextA(mdc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK);
    __gdi_free_dc(mdc);
    int tw = tr.right - tr.left, th = tr.bottom - tr.top;
    int bw = 80, bh = 26, gap = 8;
    for (int i = 0; i < nb; i++) { int need_w = (int)ustrlen(names[i]) * 8 + 24; if (need_w > bw) bw = need_w; }
    int cw = tw + 40, need = nb * bw + (nb - 1) * gap + 40;
    if (cw < need) cw = need;
    int ch = th + 40 + bh + 20;
    int title, border, sw, sh; U_info(&title, &border, &sw, &sh);
    int ow = cw + 2 * border, oh = ch + title + 2 * border;
    HWND d = CreateWindowExA(0, "OKMsgBox", caption ? caption : "", WS_POPUP | WS_CAPTION | WS_SYSMENU, (sw - ow) / 2, (sh - oh) / 3, ow, oh, owner, 0, 0, 0);
    if (!d) return 0;
    struct wnd *dw = d;
    CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, tw + 2, th + 2, d, (HMENU)(LONG_PTR)-1, 0, 0);
    int x = (cw - (nb * bw + (nb - 1) * gap)) / 2;
    HWND first = 0;
    for (int i = 0; i < nb; i++) {
        HWND b = CreateWindowExA(0, "BUTTON", names[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), x + i * (bw + gap), ch - bh - 16, bw, bh, d, (HMENU)(LONG_PTR)ids[i], 0, 0);
        if (i == def) first = b;
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

BOOL DllMain(HMODULE m, DWORD reason, LPVOID res) { (void)m; (void)reason; (void)res; return TRUE; }

int U_taskbox(HWND owner, const char *text, const char *caption, int nb, const int *ids, const char *const *names, int def) { U_init(); return taskbox(owner, text, caption, nb, ids, names, def); }
