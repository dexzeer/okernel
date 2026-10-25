// Resources and the clipboard: LoadString, LoadBitmap, LoadIcon + DrawIconEx, LoadMenu, accelerators, DialogBoxParam from a template
// resource, the clipboard (CF_TEXT / CF_UNICODETEXT, WM_PASTE of an edit control). Built with the resources of rsrc.txt.
#include "winbase.h"
#include "gui.h"

extern HANDLE GetStdHandle(DWORD n);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern HMODULE GetModuleHandleA(LPCSTR name);
extern HGLOBAL GlobalAlloc(DWORD f, SIZE_T n);
extern LPVOID GlobalLock(HGLOBAL h);
extern int LoadStringA(HINSTANCE h, UINT id, LPSTR buf, int cap);
extern HBITMAP LoadBitmapA(HINSTANCE h, LPCSTR n);
extern HICON LoadIconA(HINSTANCE h, LPCSTR n);
extern BOOL DrawIconEx(HDC dc, int x, int y, HICON i, int cx, int cy, UINT step, HBRUSH b, UINT f);
extern BOOL GetIconInfo(HICON i, void *info);
extern HMENU LoadMenuA(HINSTANCE h, LPCSTR n);
extern int GetMenuItemCount(HMENU m);
extern HMENU GetSubMenu(HMENU m, int i);
extern int GetMenuStringA(HMENU m, UINT id, LPSTR out, int cap, UINT f);
extern HANDLE LoadAcceleratorsA(HINSTANCE h, LPCSTR n);
extern INT_PTR DialogBoxParamA(HINSTANCE h, LPCSTR t, HWND o, void *proc, LPARAM lp);
extern BOOL EndDialog(HWND h, INT_PTR r);
extern int GetDlgItemTextA(HWND h, int id, LPSTR out, int cap);
extern LRESULT SendDlgItemMessageA(HWND h, int id, UINT m, WPARAM wp, LPARAM lp);
extern BOOL OpenClipboard(HWND h);
extern BOOL EmptyClipboard(void);
extern HANDLE SetClipboardData(UINT f, HANDLE h);
extern HANDLE GetClipboardData(UINT f);
extern BOOL CloseClipboard(void);
extern int wsprintfA(LPSTR out, LPCSTR fmt, ...);
extern int GetObjectA(HGDIOBJ h, int n, LPVOID out);
extern HDC CreateCompatibleDC(HDC dc);
extern HGDIOBJ SelectObject(HDC dc, HGDIOBJ o);
extern COLORREF GetPixel(HDC dc, int x, int y);
extern HBITMAP CreateCompatibleBitmap(HDC dc, int w, int h);
extern BOOL DeleteDC(HDC dc);
extern HRSRC FindResourceA(HMODULE m, LPCSTR n, LPCSTR t);
extern HGLOBAL LoadResource(HMODULE m, HRSRC r);
extern DWORD SizeofResource(HMODULE m, HRSRC r);
extern HBRUSH CreateSolidBrush(COLORREF c);
extern int FillRect(HDC dc, const RECT *r, HBRUSH b);

static int failures;
static void say(const char *s) { DWORD w, n = 0; while (s[n]) n++; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, 0); }
static void expect(int ok, const char *what) { say(ok ? "ok   " : "FAIL "); say(what); say("\n"); if (!ok) failures++; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static char dlg_text[64], paste_text[64];
static INT_PTR CALLBACK dlgproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (m == 0x110) {
        GetDlgItemTextA(h, 101, dlg_text, sizeof dlg_text);
        SendDlgItemMessageA(h, 101, 0xB1, 0, (LPARAM)-1);                          // EM_SETSEL(0, -1)
        SendDlgItemMessageA(h, 101, 0x302, 0, 0);                                  // WM_PASTE
        GetDlgItemTextA(h, 101, paste_text, sizeof paste_text);
        EndDialog(h, 7);
        return TRUE;
    }
    return FALSE;
}

int entry(void *peb) {
    (void)peb;
    HINSTANCE inst = GetModuleHandleA(0);
    char buf[80];

    expect(LoadStringA(inst, 1, buf, sizeof buf) == 12 && same(buf, "Hello string"), "LoadString from a string table (block 1)");
    expect(LoadStringA(inst, 17, buf, sizeof buf) == 12 && same(buf, "Second block"), "LoadString from the second block");

    HRSRC rd = FindResourceA(inst, (LPCSTR)10, (LPCSTR)10);                          // RT_RCDATA
    expect(rd && SizeofResource(inst, rd) == 9 && same((const char*)LoadResource(inst, rd), "RCDATA-OK"), "FindResource / LoadResource of RCDATA");

    HBITMAP bm = LoadBitmapA(inst, (LPCSTR)100);
    BITMAP b; GetObjectA(bm, sizeof b, &b);
    HDC dc = CreateCompatibleDC(0);
    SelectObject(dc, bm);
    expect(bm && b.bmWidth == 16 && b.bmHeight == 16, "LoadBitmap gives a 16 x 16 bitmap");
    expect(GetPixel(dc, 2, 2) == RGB(255, 0, 0) && GetPixel(dc, 13, 2) == RGB(0, 0, 255), "bitmap pixels decoded from the 24 bit DIB (red | blue)");

    HICON ic = LoadIconA(inst, (LPCSTR)1);
    struct { BOOL f; DWORD x, y; HBITMAP mask, color; } ii;
    GetIconInfo(ic, &ii);
    BITMAP ib; GetObjectA(ii.color, sizeof ib, &ib);
    expect(ic && ib.bmWidth == 32, "LoadIcon picks the 32 x 32 image");
    HBITMAP target = CreateCompatibleBitmap(0, 40, 40);
    HDC dc2 = CreateCompatibleDC(0);
    SelectObject(dc2, target);
    RECT all = { 0, 0, 40, 40 };
    FillRect(dc2, &all, CreateSolidBrush(RGB(255, 255, 255)));
    DrawIconEx(dc2, 0, 0, ic, 0, 0, 0, 0, 3);
    expect(GetPixel(dc2, 10, 10) == RGB(255, 0, 0), "DrawIconEx draws the 8 bit palette image");
    expect(GetPixel(dc2, 10, 0) == RGB(255, 255, 255), "the AND mask keeps the top row transparent");
    HICON small = LoadIconA(inst, (LPCSTR)1);
    (void)small;

    HMENU mn = LoadMenuA(inst, (LPCSTR)300);
    expect(mn && GetMenuItemCount(mn) == 2, "LoadMenu: two top level items");
    HMENU file = GetSubMenu(mn, 0);
    expect(file && GetMenuItemCount(file) == 2, "the File popup has two items");
    GetMenuStringA(file, 103, buf, sizeof buf, 0);
    expect(same(buf, "E&xit"), "menu item text");
    expect(LoadAcceleratorsA(inst, (LPCSTR)500) != 0, "LoadAccelerators");

    OpenClipboard(0); EmptyClipboard();
    HGLOBAL g = GlobalAlloc(0x42, 16);
    char *p = GlobalLock(g);
    const char *src = "clip text"; int i = 0; while ((p[i] = src[i])) i++;
    SetClipboardData(1, g);
    CloseClipboard();
    OpenClipboard(0);
    const WCHAR *wt = GetClipboardData(13);
    const char *at = GetClipboardData(1);
    expect(at && same(at, "clip text"), "CF_TEXT round trip through the shared clipboard");
    expect(wt && wt[0] == 'c' && wt[1] == 'l' && wt[9] == 0, "CF_UNICODETEXT is made from it");
    CloseClipboard();

    INT_PTR r = DialogBoxParamA(inst, (LPCSTR)400, 0, dlgproc, 0);
    expect(r == 7, "DialogBoxParam from a dialog resource, EndDialog result");
    expect(same(dlg_text, "from resource"), "the dialog template's edit control text");
    expect(same(paste_text, "clip text"), "WM_PASTE puts the clipboard text into the edit control");
    (void)dc;
    say(failures ? "RSRC FAIL\n" : "RSRC PASS\n");
    return failures;
}
