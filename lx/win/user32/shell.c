// shell32 / winspool / urlmon / propsys / advapi32 odds and ends that Notepad-class programs resolve at run time.
#include "u32.h"

extern void *CoTaskMemAlloc(SIZE_T n);
extern int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type);

#define E_NOTIMPL ((LONG)0x80004001)
#define E_FAIL ((LONG)0x80004005)

static const WCHAR docs_path[] = { 'C', ':', '\\', 'U', 's', 'e', 'r', 's', '\\', 'u', 's', 'e', 'r', '\\', 'D', 'o', 'c', 'u', 'm', 'e', 'n', 't', 's', 0 };

EXPORT LONG SHGetKnownFolderPath(const void *rfid, DWORD flags, HANDLE token, LPWSTR *out) {
    (void)rfid; (void)flags; (void)token;
    int n = 0;
    while (docs_path[n]) n++;
    LPWSTR p = (LPWSTR)CoTaskMemAlloc((n + 1) * 2);
    if (!p) { *out = 0; return (LONG)0x8007000E; }
    for (int i = 0; i <= n; i++) p[i] = docs_path[i];
    *out = p;
    return 0;
}
EXPORT LONG SHCreateItemFromParsingName(LPCWSTR path, void *ctx, const void *iid, void **out) { (void)path; (void)ctx; (void)iid; if (out) *out = 0; return E_NOTIMPL; }
EXPORT HANDLE ShellExecuteW(HWND h, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show) { (void)h; (void)op; (void)file; (void)params; (void)dir; (void)show; return (HANDLE)(ULONG_PTR)31; }
EXPORT BOOL ShellAboutW(HWND h, LPCWSTR app, LPCWSTR other, HICON icon) {
    (void)icon;
    WCHAR title[128], text[512];
    static const WCHAR about[] = { 'A', 'b', 'o', 'u', 't', ' ', 0 };
    int n = 0, k = 0;
    for (int i = 0; about[i]; i++) title[n++] = about[i];
    if (app) for (int i = 0; app[i] && app[i] != '#' && n < 127; i++) title[n++] = app[i];          // "title#application": the title part
    title[n] = 0;
    if (app) {                                                                                       // the first line: after the '#' when there is one
        int from = 0;
        for (int i = 0; app[i]; i++) if (app[i] == '#') { from = i + 1; break; }
        for (int i = from; app[i] && k < 300; i++) text[k++] = app[i];
    }
    static const char ver[] = "\nVersion 10.0 (Build 19045)";                                        // what GetVersionEx reports
    for (int i = 0; ver[i]; i++) text[k++] = (WCHAR)ver[i];
    if (other && other[0]) { text[k++] = 10; for (int i = 0; other[i] && k < 510; i++) text[k++] = other[i]; }
    text[k] = 0;
    MessageBoxW(h, text, title, 0);
    return 1;
}
EXPORT UINT DragQueryFileW(HANDLE drop, UINT i, LPWSTR out, UINT n) { (void)drop; (void)i; (void)out; (void)n; return 0; }
EXPORT void DragFinish(HANDLE drop) { (void)drop; }
EXPORT void DragAcceptFiles(HWND h, BOOL accept) { (void)h; (void)accept; }
EXPORT void SHAddToRecentDocs(UINT flags, const void *pv) { (void)flags; (void)pv; }
