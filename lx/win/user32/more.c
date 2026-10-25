// More user32: window properties, placement, monitors, WinEvent hooks (accepted, never fired), DPI.
#include "u32.h"

extern LONG_PTR GetWindowLongPtrW(HWND h, int i);
extern BOOL GetWindowRect(HWND h, RECT *r);
extern BOOL IsIconic(HWND h);
extern BOOL IsZoomed(HWND h);
extern BOOL IsWindowVisible(HWND h);
extern BOOL MoveWindow(HWND h, int x, int y, int w, int h2, BOOL repaint);
extern BOOL ShowWindow(HWND h, int cmd);
extern void *memset(void *d, int c, SIZE_T n);

#define MAX_PROPS 256
static struct { HWND h; unsigned long long key; HANDLE val; int used; } props[MAX_PROPS];

static unsigned long long prop_key(LPCWSTR s) {
    if ((ULONG_PTR)s < 0x10000) return (ULONG_PTR)s;                 // an atom
    unsigned long long h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s; h *= 1099511628211ull; }
    return h | 0x10000;
}

EXPORT BOOL SetPropW(HWND h, LPCWSTR s, HANDLE v) {
    unsigned long long k = prop_key(s);
    int free_ = -1;
    for (int i = 0; i < MAX_PROPS; i++) {
        if (props[i].used && props[i].h == h && props[i].key == k) { props[i].val = v; return 1; }
        if (!props[i].used && free_ < 0) free_ = i;
    }
    if (free_ < 0) return 0;
    props[free_].used = 1; props[free_].h = h; props[free_].key = k; props[free_].val = v;
    return 1;
}
EXPORT HANDLE GetPropW(HWND h, LPCWSTR s) {
    unsigned long long k = prop_key(s);
    for (int i = 0; i < MAX_PROPS; i++) if (props[i].used && props[i].h == h && props[i].key == k) return props[i].val;
    return 0;
}
EXPORT HANDLE RemovePropW(HWND h, LPCWSTR s) {
    unsigned long long k = prop_key(s);
    for (int i = 0; i < MAX_PROPS; i++) if (props[i].used && props[i].h == h && props[i].key == k) { props[i].used = 0; return props[i].val; }
    return 0;
}

typedef struct { UINT length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT;

EXPORT BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *p) {
    if (!p) return 0;
    memset(p, 0, sizeof *p);
    p->length = sizeof *p;
    p->showCmd = IsIconic(h) ? 2 : IsZoomed(h) ? 3 : 1;
    p->ptMinPosition.x = p->ptMinPosition.y = -1; p->ptMaxPosition.x = p->ptMaxPosition.y = -1;
    return GetWindowRect(h, &p->rcNormalPosition);
}
EXPORT BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *p) {
    if (!p) return 0;
    MoveWindow(h, p->rcNormalPosition.left, p->rcNormalPosition.top, p->rcNormalPosition.right - p->rcNormalPosition.left, p->rcNormalPosition.bottom - p->rcNormalPosition.top, 1);
    if (p->showCmd == 2 || p->showCmd == 3) ShowWindow(h, (int)p->showCmd);
    return 1;
}

EXPORT HANDLE MonitorFromWindow(HWND h, DWORD flags) { (void)h; (void)flags; return (HANDLE)(ULONG_PTR)1; }
EXPORT HANDLE SetWinEventHook(DWORD lo, DWORD hi, HANDLE mod, void *proc, DWORD pid, DWORD tid, DWORD flags) { (void)lo; (void)hi; (void)mod; (void)proc; (void)pid; (void)tid; (void)flags; return (HANDLE)(ULONG_PTR)1; }
EXPORT BOOL UnhookWinEvent(HANDLE h) { (void)h; return 1; }
EXPORT void NotifyWinEvent(DWORD ev, HWND h, LONG obj, LONG child) { (void)ev; (void)h; (void)obj; (void)child; }
EXPORT HANDLE SetThreadDpiAwarenessContext(HANDLE c) { return c; }
EXPORT BOOL SystemParametersInfoForDpi(UINT a, UINT b, void *c, UINT d, UINT dpi) { (void)dpi; extern BOOL SystemParametersInfoW(UINT, UINT, void*, UINT); return SystemParametersInfoW(a, b, c, d); }
