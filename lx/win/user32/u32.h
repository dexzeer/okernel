// user32 internals: window tree, message queues, timers, built-in controls
#ifndef U32_H
#define U32_H
#include "../gui.h"

extern LPVOID HeapAlloc(HANDLE h, DWORD f, SIZE_T n);
extern BOOL HeapFree(HANDLE h, DWORD f, LPVOID p);
extern HANDLE GetProcessHeap(void);
extern DWORD GetTickCount(void);
extern DWORD GetCurrentThreadId(void);
extern int MultiByteToWideChar(UINT cp, DWORD fl, LPCSTR s, int sl, LPWSTR o, int oc);
extern int WideCharToMultiByte(UINT cp, DWORD fl, LPCWSTR w, int wl, LPSTR o, int oc, LPCSTR d, BOOL *u);
extern HGDIOBJ GetStockObject(int i);
extern HGDIOBJ SelectObject(HDC dc, HGDIOBJ o);
extern HBRUSH CreateSolidBrush(COLORREF c);
extern HPEN CreatePen(int style, int w, COLORREF c);
extern BOOL DeleteObject(HGDIOBJ o);
extern BOOL DeleteDC(HDC dc);
extern COLORREF SetTextColor(HDC dc, COLORREF c);
extern COLORREF SetBkColor(HDC dc, COLORREF c);
extern int SetBkMode(HDC dc, int m);
extern BOOL TextOutA(HDC dc, int x, int y, LPCSTR s, int n);
extern BOOL GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, SIZE *sz);
extern BOOL MoveToEx(HDC dc, int x, int y, POINT *old);
extern BOOL LineTo(HDC dc, int x, int y);
extern BOOL Rectangle(HDC dc, int l, int t, int r, int b);

#define COLOR_HIGHLIGHT 13
#define MAXW 512
#define MSGQ 2048

struct cls {
    struct cls *next;
    char name[64];
    WNDPROC proc;
    UINT style;
    int cbwnd, cbcls;
    HBRUSH bg;
    HINSTANCE inst;
    int unicode, builtin, atom;
    HCURSOR cursor;
    HICON icon;
};

struct wnd {
    int used, magic;
    struct wnd *parent, *owner, *first, *last, *prev, *next;      // children in z order, last = top
    struct cls *cls;
    WNDPROC proc;
    DWORD style, exstyle;
    int x, y, w, h;                    // top level: screen, outer; child: relative to the parent's client origin
    int cw, ch;                        // client size
    int visible, enabled, unicode;
    char text[256];
    LONG_PTR userdata, id;
    HINSTANCE inst;
    int wid;                           // /dev/okwin window (top-level only)
    unsigned *px; int stride, bw, bh;  // its buffer
    RECT inval; int has_inval; int erase;
    DWORD tid;
    void *extra; int nextra;
    void *ctl;                         // control state (built-in classes)
    struct wnd *focus_child;           // top level: the child that has the keyboard
    int in_destroy, bd, activated;
    struct menu *menu; int mh, noframe;        // top level: its menu bar and the bar height inside the buffer
    void *font;
    int dlg_result, dlg_done;
};

void *memset(void *d, int c, SIZE_T n);
void *memcpy(void *d, const void *s, SIZE_T n);
BOOL ShowWindow(HWND h, int cmd);
void U_destroy(struct wnd *w);
HWND U_create_window(DWORD ex, const void *cls, const void *name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param, int unicode);
void U_drop_messages(struct wnd *w);
void U_clip(struct wnd *w, RECT *r);
void U_screen_origin(struct wnd *w, int *sx, int *sy);
int U_okw_resize(struct wnd *w, int cw, int ch);
int U_find_wid(int wid);
int U_fd(void);
void U_info(int *title, int *border, int *sw, int *sh);
void U_init(void);
struct wnd *U_desktop(void);
struct wnd *U_active(void);
void U_set_active(struct wnd *w);
struct wnd *U_pool(int i);
void U_set_text(struct wnd *w, const char *s);
void U_clear_focus_refs(struct wnd *w);
struct cls *U_add_class(const char *name, WNDPROC proc, UINT style, int cbcls, int cbwnd, HBRUSH bg, HINSTANCE inst, int unicode, int builtin);
void U_activate(struct wnd *top, int on);
LRESULT U_def_proc(struct wnd *w, UINT m, WPARAM wp, LPARAM lp, int unicode);
void U_paint_window(struct wnd *w);                      // deliver WM_PAINT now
struct wnd *U_next_invalid(DWORD tid);
void U_run_modal(struct wnd *dlg);
int U_wsprintf(char *out, int cap, const char *fmt, void *args, int wide_s);
LRESULT SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp);
LRESULT SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp);
BOOL IsChild(HWND p, HWND c);
SHORT GetKeyState(int vk);
HDC GetDC(HWND h);
int ReleaseDC(HWND h, HDC dc);
HWND SetFocus(HWND h);
HWND SetCapture(HWND h);
BOOL ReleaseCapture(void);
int mods_cache(void);
HWND GetDlgItem(HWND h, int id);
int GetWindowTextA(HWND h, LPSTR out, int cap);
BOOL SetDlgItemTextA(HWND h, int id, LPCSTR s);
struct menu *U_menu(HMENU h);
int U_menubar_height(void);
void U_menubar_paint(struct wnd *w);
int U_menubar_event(struct wnd *T, const struct okw_event *e);
void U_menubar_leave(struct wnd *T);
int U_pump(int ms);
extern int (*U_event_hook)(const struct okw_event *);
struct wnd *U_wnd(HWND h);
struct wnd *U_top(struct wnd *w);
void U_origin(struct wnd *w, int *ox, int *oy);                  // client origin of w inside its top level's buffer
LRESULT U_send(struct wnd *w, UINT m, WPARAM wp, LPARAM lp);
void U_invalidate(struct wnd *w, const RECT *r, int erase);
void U_present_rect(struct wnd *w, const RECT *r);              // client rectangle of w -> screen
int U_get_color(int idx);
void U_post(HWND h, UINT m, WPARAM wp, LPARAM lp);
void U_fill_sys(HDC dc, const RECT *r, int idx);
void U_draw_edge(HDC dc, RECT *r, int raised, int thin);
int U_text_width(HDC dc, const char *s, int n);
int U_text_height(HDC dc);
struct wnd *U_focus(void);
void U_set_focus(struct wnd *w);
void U_set_capture(struct wnd *w);
struct wnd *U_capture(void);
struct wnd *U_child_from_point(struct wnd *top, int x, int y);   // x,y in top-level client coordinates

// built-in controls (controls.c)
void U_register_builtin(void);
LRESULT U_button_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT U_static_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT U_edit_proc(HWND, UINT, WPARAM, LPARAM);
LRESULT U_listbox_proc(HWND, UINT, WPARAM, LPARAM);

LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);

static inline void *umalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 8, n ? n : 1); }
static inline void ufree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static inline int ustrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline void ustrcpy(char *d, const char *s, int cap) { int i = 0; for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
static inline int ustricmp(const char *a, const char *b) {
    for (;; a++, b++) { int x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return x - y; if (!x) return 0; }
}

#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((DWORD_PTR)(l) >> 16) & 0xFFFF))
#define MAKELPARAM(a, b) ((LPARAM)(((DWORD)(WORD)(a)) | ((DWORD)(WORD)(b) << 16)))
#define GET_X(l) ((int)(short)LOWORD(l))
#define GET_Y(l) ((int)(short)HIWORD(l))

// more messages
#define BM_GETCHECK 0xF0
#define BM_SETCHECK 0xF1
#define BM_GETSTATE 0xF2
#define BM_SETSTATE 0xF3
#define BM_CLICK 0xF5
#define BN_CLICKED 0
#define BS_PUSHBUTTON 0
#define BS_DEFPUSHBUTTON 1
#define BS_CHECKBOX 2
#define BS_AUTOCHECKBOX 3
#define BS_RADIOBUTTON 4
#define BS_3STATE 5
#define BS_AUTO3STATE 6
#define BS_GROUPBOX 7
#define BS_AUTORADIOBUTTON 9
#define BS_TYPEMASK 0xF
#define SS_LEFT 0
#define SS_CENTER 1
#define SS_RIGHT 2
#define SS_ICON 3
#define SS_BLACKRECT 4
#define SS_GRAYRECT 5
#define SS_WHITERECT 6
#define SS_TYPEMASK 0x1F
#define ES_MULTILINE 4
#define ES_PASSWORD 0x20
#define ES_AUTOHSCROLL 0x80
#define ES_READONLY 0x800
#define ES_NUMBER 0x2000
#define EN_CHANGE 0x300
#define EM_GETSEL 0xB0
#define EM_SETSEL 0xB1
#define EM_REPLACESEL 0xC2
#define EM_SETREADONLY 0xCF
#define EM_LIMITTEXT 0xC5
#define EM_GETLINECOUNT 0xBA
#define WM_COPYDATA 0x4A
#define WM_NOTIFY 0x4E
#define WM_SETFONT 0x30
#define WM_GETFONT 0x31
#define WM_INITDIALOG 0x110
#define WM_CTLCOLORSTATIC 0x138
#define WM_CTLCOLORBTN 0x135
#define WM_CTLCOLOREDIT 0x133
#define WM_SYSCOMMAND 0x112
#define WM_MOUSELEAVE 0x2A3
#define WM_SIZING 0x214
#define WM_WINDOWPOSCHANGED 0x47
#define WM_WINDOWPOSCHANGING 0x46
#define SIZE_RESTORED 0
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define HTCLIENT 1
#define HTCAPTION 2
#define HTCLOSE 20
#define SC_CLOSE 0xF060
#define LB_ADDSTRING 0x180
#define LB_RESETCONTENT 0x184
#define LB_GETCURSEL 0x188
#define LB_SETCURSEL 0x186
#define LB_GETCOUNT 0x18B
#define LB_GETTEXT 0x189
#define LB_GETTEXTLEN 0x18A
#define LB_INSERTSTRING 0x181
#define LB_DELETESTRING 0x182
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2

#define WS_TABSTOP 0x00010000
#define WS_GROUP 0x00020000
#define WS_DISABLED 0x08000000
#define WS_CLIPCHILDREN 0x02000000
#define WS_CLIPSIBLINGS 0x04000000
#define WS_VSCROLL 0x00200000
#define WS_HSCROLL 0x00100000
#define WS_DLGFRAME 0x00400000
#define WS_EX_CLIENTEDGE 0x200
#define WS_EX_TOOLWINDOW 0x80
#define WS_EX_APPWINDOW 0x40000
#define WS_EX_DLGMODALFRAME 1

#endif
