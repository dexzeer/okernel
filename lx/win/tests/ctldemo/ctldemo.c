// Controls: COMBOBOX (drop-down list and drop-down with an edit), SCROLLBAR controls, the scroll bars of a window.
#include "winbase.h"
#include "gui.h"

extern HANDLE GetStdHandle(DWORD n);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern ATOM RegisterClassExA(const WNDCLASSEXA *c);
extern HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern BOOL ShowWindow(HWND h, int c);
extern BOOL UpdateWindow(HWND h);
extern BOOL GetMessageA(MSG *m, HWND f, UINT lo, UINT hi);
extern BOOL TranslateMessage(const MSG *m);
extern LRESULT DispatchMessageA(const MSG *m);
extern LRESULT DefWindowProcA(HWND h, UINT m, WPARAM wp, LPARAM lp);
extern void PostQuitMessage(int c);
extern BOOL DestroyWindow(HWND h);
extern int wsprintfA(LPSTR out, LPCSTR fmt, ...);
extern LRESULT SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp);
extern BOOL GetWindowRect(HWND h, RECT *r);
extern BOOL GetClientRect(HWND h, RECT *r);
extern BOOL ClientToScreen(HWND h, POINT *p);
extern HCURSOR LoadCursorA(HINSTANCE h, LPCSTR n);
extern int SetScrollInfo(HWND h, int bar, const void *i, BOOL redraw);
extern int GetScrollPos(HWND h, int bar);
extern int SetScrollPos(HWND h, int bar, int p, BOOL redraw);

#define LOWORD_(l) ((unsigned)(l) & 0xFFFF)
#define HIWORD_(l) (((unsigned)(l) >> 16) & 0xFFFF)
#define CBS_DROPDOWN 2
#define CBS_DROPDOWNLIST 3
#define CB_ADDSTRING 0x143
#define CB_SETCURSEL 0x14E
#define CB_GETCURSEL 0x147
#define SBS_VERT 1
#define SBM_SETRANGE 0xE2
#define SBM_SETPOS 0xE0
#define SBM_SETSCROLLINFO 0xE9
#define WS_VSCROLL_ 0x00200000
#define ID_COMBO1 201
#define ID_COMBO2 202
#define ID_VSB 203
#define ID_HSB 204

typedef struct { UINT cbSize, fMask; int nMin, nMax; UINT nPage; int nPos, nTrackPos; } SCROLLINFO;

static HWND g_combo1, g_combo2, g_vsb, g_hsb, g_top;

static void say(const char *s) { DWORD w, n = 0; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }
static void sayf(const char *fmt, int a, int b, int c, int d) { char buf[160]; wsprintfA(buf, fmt, a, b, c, d); say(buf); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_combo1 = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 20, 20, 200, 160, h, (HMENU)(LONG_PTR)ID_COMBO1, 0, 0);
        const char *items[4] = { "Alpha", "Beta", "Gamma", "Delta" };
        for (int i = 0; i < 4; i++) SendMessageA(g_combo1, CB_ADDSTRING, 0, (LPARAM)items[i]);
        SendMessageA(g_combo1, CB_SETCURSEL, 0, 0);
        g_combo2 = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN, 20, 70, 200, 160, h, (HMENU)(LONG_PTR)ID_COMBO2, 0, 0);
        for (int i = 0; i < 4; i++) SendMessageA(g_combo2, CB_ADDSTRING, 0, (LPARAM)items[i]);
        g_vsb = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_VERT, 260, 20, 17, 150, h, (HMENU)(LONG_PTR)ID_VSB, 0, 0);
        g_hsb = CreateWindowExA(0, "SCROLLBAR", "", WS_CHILD | WS_VISIBLE, 20, 190, 200, 17, h, (HMENU)(LONG_PTR)ID_HSB, 0, 0);
        SendMessageA(g_vsb, SBM_SETRANGE, 0, 100);
        SendMessageA(g_hsb, SBM_SETRANGE, 0, 100);
        SCROLLINFO si = { sizeof si, 0x17, 0, 99, 10, 0, 0 };
        SetScrollInfo(h, 1, &si, TRUE);                                       // SB_VERT of the window itself
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD_(wp) == ID_COMBO1 || LOWORD_(wp) == ID_COMBO2) {
            int code = (int)HIWORD_(wp);
            if (code == 7) say("dropdown\n");
            else if (code == 1) sayf("selchange %d sel=%d\n", (int)LOWORD_(wp), (int)SendMessageA((HWND)lp, CB_GETCURSEL, 0, 0), 0, 0);
        }
        return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        int code = (int)LOWORD_(wp), pos = (int)HIWORD_(wp);
        if (lp) { if ((HWND)lp == g_vsb) { SendMessageA(g_vsb, SBM_SETPOS, (WPARAM)(code == 1 ? 1 : code == 0 ? 0 : code == 3 ? 10 : code == 2 ? 0 : pos), 1); } sayf("ctl %d code %d pos %d\n", (int)((HWND)lp == g_vsb ? 1 : 2), code, pos, 0); }
        else {
            int p = GetScrollPos(h, 1);
            if (code == 1) p += 1; else if (code == 0) p -= 1; else if (code == 3) p += 10; else if (code == 2) p -= 10; else if (code == 5 || code == 4) p = pos;
            SetScrollPos(h, 1, p, TRUE);
            sayf("window scroll code %d pos %d\n", code, GetScrollPos(h, 1), 0, 0);
        }
        return 0;
    }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int entry(void *peb) {
    (void)peb;
    WNDCLASSEXA c;
    char *p = (char*)&c; for (unsigned i = 0; i < sizeof c; i++) p[i] = 0;
    c.cbSize = sizeof c; c.lpfnWndProc = wndproc; c.lpszClassName = "CtlDemo"; c.hbrBackground = (HBRUSH)(LONG_PTR)(15 + 1); c.hCursor = LoadCursorA(0, (LPCSTR)32512);
    RegisterClassExA(&c);
    g_top = CreateWindowExA(0, "CtlDemo", "Controls", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_VSCROLL_, 1000, 90, 340, 300, 0, 0, 0, 0);
    if (!g_top) { say("create failed\n"); return 1; }
    UpdateWindow(g_top);
    RECT r;
    GetWindowRect(g_combo1, &r); sayf("combo1 %d,%d %d,%d\n", r.left, r.top, r.right, r.bottom);
    GetWindowRect(g_combo2, &r); sayf("combo2 %d,%d %d,%d\n", r.left, r.top, r.right, r.bottom);
    GetWindowRect(g_vsb, &r); sayf("vsb %d,%d %d,%d\n", r.left, r.top, r.right, r.bottom);
    GetWindowRect(g_hsb, &r); sayf("hsb %d,%d %d,%d\n", r.left, r.top, r.right, r.bottom);
    POINT o = { 0, 0 }; ClientToScreen(g_top, &o); GetClientRect(g_top, &r);
    sayf("client %d,%d %d,%d\n", o.x, o.y, r.right, r.bottom);
    say("controls ready\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    say("bye\n");
    return 0;
}
