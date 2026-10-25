// Dialog boxes: DialogBoxIndirectParam with templates built in memory, both formats (DLGTEMPLATE and DLGTEMPLATEEX): controls of the
// built-in classes, WM_INITDIALOG, default button, Tab / Enter / Escape / arrow keys through IsDialogMessage, EndDialog.
#include "winbase.h"
#include "gui.h"

extern HANDLE GetStdHandle(DWORD n);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern INT_PTR DialogBoxIndirectParamA(HINSTANCE h, const void *t, HWND owner, void *proc, LPARAM lp);
extern BOOL EndDialog(HWND h, INT_PTR r);
extern int wsprintfA(LPSTR out, LPCSTR fmt, ...);
extern int GetDlgItemTextA(HWND h, int id, LPSTR out, int cap);
extern BOOL SetDlgItemTextA(HWND h, int id, LPCSTR s);
extern UINT IsDlgButtonChecked(HWND h, int id);
extern BOOL CheckRadioButton(HWND h, int first, int last, int check);
extern LONG GetDialogBaseUnits(void);

#define WM_INITDIALOG 0x110
#define LOWORD_(l) ((unsigned)(l) & 0xFFFF)
#define WS_GROUP 0x20000
#define WS_TABSTOP 0x10000
#define BS_AUTOCHECKBOX_ 3
#define BS_AUTORADIOBUTTON_ 9
#define BS_DEFPUSHBUTTON_ 1
#define ES_AUTOHSCROLL_ 0x80
#define DS_MODALFRAME_ 0x80
#define DS_SETFONT_ 0x40

static BYTE tmpl[2048];
static int tn;
static void p16(unsigned v) { tmpl[tn++] = (BYTE)v; tmpl[tn++] = (BYTE)(v >> 8); }
static void p32(unsigned v) { p16(v & 0xFFFF); p16(v >> 16); }
static void pstr(const char *s) { do { p16((unsigned char)*s); } while (*s++); }
static void al4(void) { while (tn & 3) tmpl[tn++] = 0; }

static void ctl(int ex, unsigned style, int x, int y, int cx, int cy, unsigned id, unsigned cls_atom, const char *text) {
    al4();
    if (ex) { p32(0); p32(0); p32(style | WS_CHILD | WS_VISIBLE); }
    else { p32(style | WS_CHILD | WS_VISIBLE); p32(0); }
    p16(x); p16(y); p16(cx); p16(cy);
    if (ex) p32(id); else p16(id);
    p16(0xFFFF); p16(cls_atom);
    pstr(text);
    p16(0);                                                            // no creation data
}
static void build(int ex) {
    tn = 0;
    unsigned style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME_ | WS_VISIBLE;
    if (ex) { p16(1); p16(0xFFFF); p32(0); p32(0); p32(style); }
    else { p32(style); p32(0); }
    p16(8);                                                            // control count
    p16(0); p16(0); p16(180); p16(110);                                // x y cx cy in dialog units
    p16(0); p16(0);                                                    // no menu, default class
    pstr(ex ? "Extended dialog" : "Old dialog");
    ctl(ex, 0, 8, 8, 40, 10, 0xFFFF, 0x82, "Name:");
    ctl(ex, WS_TABSTOP | ES_AUTOHSCROLL_ | WS_BORDER, 52, 6, 120, 14, 101, 0x81, "");
    ctl(ex, WS_TABSTOP | BS_AUTOCHECKBOX_, 8, 28, 90, 12, 102, 0x80, "&Check me");
    ctl(ex, WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON_, 8, 46, 70, 12, 103, 0x80, "Option &A");
    ctl(ex, BS_AUTORADIOBUTTON_, 8, 60, 70, 12, 104, 0x80, "Option &B");
    ctl(ex, WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON_, 40, 86, 44, 16, IDOK, 0x80, "OK");
    ctl(ex, WS_TABSTOP, 92, 86, 44, 16, IDCANCEL, 0x80, "Cancel");
    ctl(ex, 0, 100, 46, 70, 30, 0xFFFF, 0x82, "A static\nlabel");
}

static void say(const char *s) { DWORD w, n = 0; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static INT_PTR CALLBACK dlgproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextA(h, 101, "hello");
        CheckRadioButton(h, 103, 104, 103);
        say("dialog ready\n");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD_(wp) == IDOK) {
            char t[64], out[160];
            GetDlgItemTextA(h, 101, t, sizeof t);
            wsprintfA(out, "dialog ok name=%s check=%d radio=%d", t, (int)IsDlgButtonChecked(h, 102), IsDlgButtonChecked(h, 103) ? 103 : IsDlgButtonChecked(h, 104) ? 104 : 0);
            say(out); say("\n");
            EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD_(wp) == IDCANCEL) { say("dialog cancelled\n"); EndDialog(h, 0); return TRUE; }
        return FALSE;
    }
    return FALSE;
}

int entry(void *peb) {
    (void)peb;
    char b[80];
    LONG bu = GetDialogBaseUnits();
    wsprintfA(b, "base units %d x %d\n", (int)(bu & 0xFFFF), (int)((unsigned)bu >> 16)); say(b);
    build(0);
    INT_PTR r = DialogBoxIndirectParamA(0, tmpl, 0, dlgproc, 0);
    wsprintfA(b, "first dialog result %d\n", (int)r); say(b);
    build(1);
    r = DialogBoxIndirectParamA(0, tmpl, 0, dlgproc, 0);
    wsprintfA(b, "second dialog result %d\n", (int)r); say(b);
    (void)slen;
    say("dlgdemo done\n");
    return 0;
}
