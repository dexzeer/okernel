// Menus: a menu bar with popups and a submenu, an accelerator, a context menu (TrackPopupMenu with TPM_RETURNCMD).
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
extern HDC BeginPaint(HWND h, PAINTSTRUCT *ps);
extern BOOL EndPaint(HWND h, const PAINTSTRUCT *ps);
extern BOOL GetClientRect(HWND h, RECT *r);
extern BOOL InvalidateRect(HWND h, const RECT *r, BOOL e);
extern BOOL DestroyWindow(HWND h);
extern int wsprintfA(LPSTR out, LPCSTR fmt, ...);
extern BOOL TextOutA(HDC dc, int x, int y, LPCSTR s, int n);
extern int SetBkMode(HDC dc, int m);
extern HMENU CreateMenu(void);
extern HMENU CreatePopupMenu(void);
extern BOOL AppendMenuA(HMENU m, UINT f, ULONG_PTR id, LPCSTR s);
extern BOOL SetMenu(HWND h, HMENU m);
extern DWORD CheckMenuItem(HMENU m, UINT id, UINT f);
extern BOOL EnableMenuItem(HMENU m, UINT id, UINT f);
extern int TrackPopupMenu(HMENU m, UINT f, int x, int y, int r, HWND h, const RECT *rc);
extern BOOL GetMenuItemRect(HWND h, HMENU m, UINT pos, RECT *r);
extern BOOL ClientToScreen(HWND h, POINT *p);
extern int GetMenuItemCount(HMENU m);
extern HANDLE CreateAcceleratorTableA(const void *a, int n);
extern int TranslateAcceleratorA(HWND h, HANDLE a, MSG *m);
extern HCURSOR LoadCursorA(HINSTANCE h, LPCSTR n);

#define MF_STRING 0
#define MF_POPUP 0x10
#define MF_SEPARATOR 0x800
#define MF_CHECKED 8
#define MF_GRAYED 1
#define MF_BYCOMMAND 0
#define TPM_RETURNCMD 0x100
#define LOWORD_(l) ((unsigned)(l) & 0xFFFF)
#define HIWORD_(l) (((unsigned)(l) >> 16) & 0xFFFF)

#define ID_NEW 101
#define ID_OPEN 102
#define ID_EXIT 103
#define ID_COPY 201
#define ID_PASTE 202
#define ID_DEEP 301
#define ID_CTX1 401
#define ID_CTX2 402

static HMENU g_bar, g_ctx;
static HANDLE g_acc;
static char g_last[80] = "none";

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { DWORD w, n = 0; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }
static void sayf(const char *fmt, int a, int b, int c, int d) { char buf[160]; wsprintfA(buf, fmt, a, b, c, d); say(buf); }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HMENU file = CreatePopupMenu(), edit = CreatePopupMenu(), more = CreatePopupMenu();
        AppendMenuA(file, MF_STRING, ID_NEW, "&New\tCtrl+N");
        AppendMenuA(file, MF_STRING, ID_OPEN, "&Open...");
        AppendMenuA(file, MF_SEPARATOR, 0, 0);
        AppendMenuA(file, MF_STRING, ID_EXIT, "E&xit");
        AppendMenuA(more, MF_STRING, ID_DEEP, "&Deep item");
        AppendMenuA(edit, MF_STRING, ID_COPY, "&Copy");
        AppendMenuA(edit, MF_STRING | MF_CHECKED, ID_PASTE, "&Paste");
        AppendMenuA(edit, MF_POPUP, (ULONG_PTR)more, "&More");
        g_bar = CreateMenu();
        AppendMenuA(g_bar, MF_POPUP, (ULONG_PTR)file, "&File");
        AppendMenuA(g_bar, MF_POPUP, (ULONG_PTR)edit, "&Edit");
        SetMenu(h, g_bar);
        g_ctx = CreatePopupMenu();
        AppendMenuA(g_ctx, MF_STRING, ID_CTX1, "Context one");
        AppendMenuA(g_ctx, MF_STRING | MF_GRAYED, ID_CTX2, "Context two (gray)");
        struct { BYTE fVirt; WORD key, cmd; } acc[1] = { { 1 | 8, 'N', ID_NEW } };     // FVIRTKEY | FCONTROL
        g_acc = CreateAcceleratorTableA(acc, 1);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 20, 20, g_last, slen(g_last));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = (int)LOWORD_(wp), src = (int)HIWORD_(wp);
        sayf("cmd %d src %d\n", id, src, 0, 0);
        wsprintfA(g_last, "last command %d (%s)", id, 0, 0, 0);
        InvalidateRect(h, 0, TRUE);
        if (id == ID_EXIT) DestroyWindow(h);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p = { (short)(lp & 0xFFFF), (short)((lp >> 16) & 0xFFFF) };
        ClientToScreen(h, &p);
        int r = TrackPopupMenu(g_ctx, TPM_RETURNCMD, p.x, p.y, 0, h, 0);
        sayf("context result %d\n", r, 0, 0, 0);
        return 0;
    }
    case 0x117: say("initpopup\n"); return 0;                                // WM_INITMENUPOPUP
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int entry(void *peb) {
    (void)peb;
    WNDCLASSEXA c;
    char *p = (char*)&c; for (unsigned i = 0; i < sizeof c; i++) p[i] = 0;
    c.cbSize = sizeof c; c.lpfnWndProc = wndproc; c.lpszClassName = "MenuDemo"; c.hbrBackground = (HBRUSH)(LONG_PTR)(COLOR_WINDOW + 1); c.hCursor = LoadCursorA(0, (LPCSTR)32512);
    RegisterClassExA(&c);
    HWND w = CreateWindowExA(0, "MenuDemo", "Menu demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 1000, 90, 440, 300, 0, 0, 0, 0);
    if (!w) { say("create failed\n"); return 1; }
    UpdateWindow(w);
    for (int i = 0; i < GetMenuItemCount(g_bar); i++) { RECT r; GetMenuItemRect(w, g_bar, (UINT)i, &r); sayf("bar%d %d,%d", i, r.left, r.top, 0); sayf(" %d,%d\n", r.right, r.bottom, 0, 0); }
    say("menu ready\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) { if (TranslateAcceleratorA(w, g_acc, &msg)) continue; TranslateMessage(&msg); DispatchMessageA(&msg); }
    say("bye\n");
    return 0;
}
