#include "winbase.h"

extern HANDLE GetStdHandle(DWORD n);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern DWORD GetCurrentProcessId(void);
extern LPSTR GetCommandLineA(void);

static void say(const char *s) {
    DWORD w, n = 0; while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0);
}

int entry(void *peb) {
    (void)peb;
    say("hello from a Windows program\r\n");
    say("command line: "); say(GetCommandLineA()); say("\r\n");
    return 7;
}
