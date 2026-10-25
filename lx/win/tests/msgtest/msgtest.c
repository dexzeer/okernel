// FormatMessage from a message table that lives in <exe>.mui (needs build-host/extra_win/win/en-US/msgtest.exe.mui = any real .mui)
#include "winbase.h"

extern HANDLE GetStdHandle(DWORD n);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern DWORD FormatMessageA(DWORD flags, LPCVOID src, DWORD id, DWORD lang, LPSTR out, DWORD cap, void **args);
extern HMODULE GetModuleHandleA(LPCSTR name);
extern DWORD GetLastError(void);

static void say(const char *s) { DWORD w, n = 0; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }
static void num(const char *tag, unsigned v) { char b[40]; int n = 0; char r[12]; int k = 0; do { r[k++] = (char)('0' + v % 10); v /= 10; } while (v); for (const char *t = tag; *t; t++) b[n++] = *t; while (k) b[n++] = r[--k]; b[n++] = '\n'; b[n] = 0; say(b); }

int entry(void *peb) {
    (void)peb;
    char buf[512];
    void *args[2] = { (void*)"findstr", (void*)"x.txt" };
    DWORD n = FormatMessageA(0x800 | 0x2000, GetModuleHandleA(0), 4, 0, buf, sizeof buf, args);
    num("len ", n);
    num("err ", GetLastError());
    say(buf);
    return 0;
}
