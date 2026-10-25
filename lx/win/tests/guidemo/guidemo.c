// A Win32 GUI program: window class, WM_PAINT with GDI, a button, an edit control, a timer, mouse and keyboard messages.
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
extern ULONG_PTR SetTimer(HWND h, ULONG_PTR id, UINT ms, void *proc);
extern BOOL DestroyWindow(HWND h);
extern int GetWindowTextA(HWND h, LPSTR out, int cap);
extern int wsprintfA(LPSTR out, LPCSTR fmt, ...);
extern HBRUSH CreateSolidBrush(COLORREF c);
extern HGDIOBJ SelectObject(HDC dc, HGDIOBJ o);
extern HPEN CreatePen(int style, int w, COLORREF c);
extern BOOL Rectangle(HDC dc, int l, int t, int r, int b);
extern BOOL Ellipse(HDC dc, int l, int t, int r, int b);
extern BOOL MoveToEx(HDC dc, int x, int y, POINT *old);
extern BOOL LineTo(HDC dc, int x, int y);
extern BOOL TextOutA(HDC dc, int x, int y, LPCSTR s, int n);
extern COLORREF SetTextColor(HDC dc, COLORREF c);
extern int SetBkMode(HDC dc, int m);
extern BOOL Polygon(HDC dc, const POINT *p, int n);
extern HGDIOBJ GetStockObject(int i);
extern HCURSOR LoadCursorA(HINSTANCE h, LPCSTR n);
extern int FillRect(HDC dc, const RECT *r, HBRUSH b);
extern HDC CreateCompatibleDC(HDC dc);
extern HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h);
extern BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
extern BOOL DeleteDC(HDC dc);
extern BOOL DeleteObject(HGDIOBJ o);
extern int MessageBoxA(HWND h, LPCSTR t, LPCSTR c, UINT ty);

#define LOWORD_(l) ((unsigned)(l) & 0xFFFF)
#define HIWORD_(l) (((unsigned)(l) >> 16) & 0xFFFF)
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
#define ID_BTN 101
#define ID_EDIT 102
#define EN_CHANGE_ 0x300

static HWND g_edit, g_btn;
static int g_clicks, g_ticks, g_mx, g_my;

static void say(const char *s) {
    DWORD w, n = 0; while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}
static void sayf(const char *fmt, int a, int b) { char buf[160]; wsprintfA(buf, fmt, a, b); say(buf); }

static void paint(HDC dc, int w, int h) {
    (void)h;
    HBRUSH red = CreateSolidBrush(RGB(200, 0, 0)), grn = CreateSolidBrush(RGB(0, 160, 0)), blu = CreateSolidBrush(RGB(0, 0, 200));
    SelectObject(dc, GetStockObject(NULL_PEN));
    SelectObject(dc, red); Rectangle(dc, 20, 20, 100, 80);
    SelectObject(dc, grn); Ellipse(dc, 120, 20, 200, 80);
    SelectObject(dc, blu);
    POINT tri[3] = { { 220, 80 }, { 260, 20 }, { 300, 80 } };
    Polygon(dc, tri, 3);
    SelectObject(dc, GetStockObject(BLACK_PEN));
    MoveToEx(dc, 20, 100, 0); LineTo(dc, w - 20, 100);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    char line[96];
    wsprintfA(line, "clicks %d  ticks %d", g_clicks, g_ticks);
    TextOutA(dc, 20, 110, line, slen(line));
    wsprintfA(line, "mouse %d,%d", g_mx, g_my);
    TextOutA(dc, 20, 130, line, slen(line));
    // an off-screen bitmap blitted in
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 40, 20);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT r = { 0, 0, 40, 20 };
    HBRUSH y = CreateSolidBrush(RGB(240, 200, 0));
    FillRect(mem, &r, y);
    BitBlt(dc, 320, 40, 40, 20, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); DeleteObject(y);
    DeleteObject(red); DeleteObject(grn); DeleteObject(blu);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        g_btn = CreateWindowExA(0, "BUTTON", "Click me", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 160, 100, 28, h, (HMENU)(LONG_PTR)ID_BTN, 0, 0);
        g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 140, 160, 200, 28, h, (HMENU)(LONG_PTR)ID_EDIT, 0, 0);
        SetTimer(h, 1, 100, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        paint(dc, rc.right, rc.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        g_ticks++;
        if (g_ticks == 3) say("timer 3\n");
        { RECT r = { 0, 105, 400, 150 }; InvalidateRect(h, &r, TRUE); }
        return 0;
    case WM_COMMAND:
        if (LOWORD_(wp) == ID_BTN) { g_clicks++; sayf("clicked %d\n", g_clicks, 0); InvalidateRect(h, 0, TRUE); }
        else if (LOWORD_(wp) == ID_EDIT && HIWORD_(wp) == EN_CHANGE_) { char t[80]; GetWindowTextA(g_edit, t, sizeof t); say("edit: "); say(t); say("\n"); }
        return 0;
    case WM_MOUSEMOVE: g_mx = (short)(lp & 0xFFFF); g_my = (short)((lp >> 16) & 0xFFFF); return 0;
    case WM_LBUTTONDOWN: sayf("lbutton %d,%d\n", (short)(lp & 0xFFFF), (short)((lp >> 16) & 0xFFFF)); return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { say("esc\n"); DestroyWindow(h); } return 0;
    case WM_CLOSE: say("close\n"); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(5); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int entry(void *peb) {
    (void)peb;
    WNDCLASSEXA c;
    char *p = (char*)&c; for (unsigned i = 0; i < sizeof c; i++) p[i] = 0;
    c.cbSize = sizeof c; c.lpfnWndProc = wndproc; c.lpszClassName = "GuiDemo"; c.hbrBackground = (HBRUSH)(LONG_PTR)(COLOR_WINDOW + 1); c.hCursor = LoadCursorA(0, (LPCSTR)32512);
    if (!RegisterClassExA(&c)) { say("register failed\n"); return 1; }
    HWND w = CreateWindowExA(0, "GuiDemo", "Win32 demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 90, 420 + 4, 260 + 30, 0, 0, 0, 0);
    if (!w) { say("create failed\n"); return 1; }
    UpdateWindow(w);
    say("window ready\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    say("bye\n");
    return (int)msg.wParam;
}
