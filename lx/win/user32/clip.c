// user32: the clipboard. Text is shared with every program through /dev/okwin (OKW_CLIPBOARD, UTF-8); other formats live in the
// process only (a table of HGLOBALs).
#include "u32.h"

extern HGLOBAL GlobalAlloc(DWORD flags, SIZE_T n);
extern SIZE_T GlobalSize(HGLOBAL h);
extern HGLOBAL GlobalFree(HGLOBAL h);

#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_OEMTEXT 7
#define CF_UNICODETEXT 13
#define CF_HDROP 15
#define CLIPMAX 65536

static struct cf { UINT fmt; HGLOBAL h; } tab[16];
static int ntab, opened;
static HWND owner_wnd;
static unsigned long long mine_seq, pulled_seq;
static char names[16][48];
static int nnames;

static unsigned long long kernel_clip(int op, char *buf, int *len) {
    struct okw_clip a;
    a.op = op; a.len = *len; a.buf = (unsigned long long)buf; a.seq = 0;
    if (U_fd() < 0 || SC3(LX_ioctl, U_fd(), OKW_CLIPBOARD, &a) < 0) { *len = 0; return 0; }
    *len = a.len;
    return a.seq;
}

// the text others put there (or an empty clipboard): rebuild CF_TEXT / CF_UNICODETEXT unless it is ours
static void pull(void) {
    int len = 0;
    unsigned long long seq = kernel_clip(1, 0, &len);
    if (seq == mine_seq || seq == pulled_seq) return;
    pulled_seq = seq;
    for (int i = 0; i < ntab; i++) if (tab[i].h) GlobalFree(tab[i].h);
    ntab = 0;
    if (len <= 0) return;
    if (len > CLIPMAX) len = CLIPMAX;
    char *u = umalloc((SIZE_T)len + 1);
    if (!u) return;
    int n = len;
    kernel_clip(1, u, &n);
    u[len] = 0;
    HGLOBAL a = GlobalAlloc(0x42, (SIZE_T)len + 1);
    if (a) { memcpy(a, u, (SIZE_T)len + 1); tab[ntab].fmt = CF_TEXT; tab[ntab++].h = a; }
    int wn = MultiByteToWideChar(65001, 0, u, -1, 0, 0);
    HGLOBAL w = wn > 0 ? GlobalAlloc(0x42, (SIZE_T)wn * 2) : 0;
    if (w) { MultiByteToWideChar(65001, 0, u, -1, w, wn); tab[ntab].fmt = CF_UNICODETEXT; tab[ntab++].h = w; }
    ufree(u);
}
static struct cf *find(UINT fmt) { for (int i = 0; i < ntab; i++) if (tab[i].fmt == fmt) return &tab[i]; return 0; }

void U_clip_set_text(const char *s, int n) {
    int len = n;
    mine_seq = kernel_clip(0, (char*)s, &len);
    for (int i = 0; i < ntab; i++) if (tab[i].h) GlobalFree(tab[i].h);
    ntab = 0;
    pulled_seq = 0;
}
int U_clip_text(char *out, int cap) {                                  // the clipboard text as UTF-8, NUL terminated; returns its length
    int len = 0;
    kernel_clip(1, 0, &len);
    if (len > cap - 1) len = cap - 1;
    if (len <= 0) { if (cap) out[0] = 0; return 0; }
    int n = len;
    kernel_clip(1, out, &n);
    out[len] = 0;
    return len;
}

EXPORT BOOL OpenClipboard(HWND h) { if (opened) return FALSE; opened = 1; owner_wnd = h; pull(); return TRUE; }
EXPORT BOOL CloseClipboard(void) { if (!opened) return FALSE; opened = 0; return TRUE; }
EXPORT BOOL EmptyClipboard(void) {
    if (!opened) return FALSE;
    for (int i = 0; i < ntab; i++) if (tab[i].h) GlobalFree(tab[i].h);
    ntab = 0;
    int zero = 0;
    mine_seq = kernel_clip(0, "", &zero);
    pulled_seq = 0;
    return TRUE;
}
EXPORT HANDLE SetClipboardData(UINT fmt, HANDLE h) {
    if (!opened) return 0;
    struct cf *e = find(fmt);
    if (!e && ntab < 16) { e = &tab[ntab++]; e->fmt = fmt; }
    if (!e) return 0;
    e->h = h;
    if (h && (fmt == CF_TEXT || fmt == CF_OEMTEXT || fmt == CF_UNICODETEXT)) {
        char *u = umalloc(CLIPMAX + 1);
        if (u) {
            int n;
            if (fmt == CF_UNICODETEXT) { n = WideCharToMultiByte(65001, 0, h, -1, u, CLIPMAX, 0, 0); n = n > 0 ? n - 1 : 0; }
            else { const char *s = h; n = 0; while (s[n] && n < CLIPMAX) { u[n] = s[n]; n++; } }
            int len = n;
            mine_seq = kernel_clip(0, u, &len);
            ufree(u);
        }
    }
    return h;
}
EXPORT HANDLE GetClipboardData(UINT fmt) {
    pull();
    struct cf *e = find(fmt);
    if (e) return e->h;
    if (fmt == CF_TEXT || fmt == CF_UNICODETEXT || fmt == CF_OEMTEXT) {                 // the other text format, made on demand
        struct cf *src = find(fmt == CF_UNICODETEXT ? CF_TEXT : CF_UNICODETEXT);
        if (!src || !src->h || ntab >= 16) return 0;
        HGLOBAL out;
        if (fmt == CF_UNICODETEXT) {
            int wn = MultiByteToWideChar(65001, 0, src->h, -1, 0, 0);
            out = GlobalAlloc(0x42, (SIZE_T)wn * 2); if (!out) return 0;
            MultiByteToWideChar(65001, 0, src->h, -1, out, wn);
        } else {
            int n = WideCharToMultiByte(65001, 0, src->h, -1, 0, 0, 0, 0);
            out = GlobalAlloc(0x42, (SIZE_T)n); if (!out) return 0;
            WideCharToMultiByte(65001, 0, src->h, -1, out, n, 0, 0);
        }
        tab[ntab].fmt = fmt; tab[ntab].h = out; ntab++;
        return out;
    }
    return 0;
}
EXPORT BOOL IsClipboardFormatAvailable(UINT fmt) {
    pull();
    if (find(fmt)) return TRUE;
    return (fmt == CF_TEXT || fmt == CF_UNICODETEXT || fmt == CF_OEMTEXT) && (find(CF_TEXT) || find(CF_UNICODETEXT));
}
EXPORT int CountClipboardFormats(void) { pull(); return ntab; }
EXPORT UINT EnumClipboardFormats(UINT prev) {
    pull();
    if (prev == 0) return ntab ? tab[0].fmt : 0;
    for (int i = 0; i < ntab; i++) if (tab[i].fmt == prev) return i + 1 < ntab ? tab[i + 1].fmt : 0;
    return 0;
}
EXPORT HWND GetClipboardOwner(void) { return owner_wnd; }
EXPORT HWND GetOpenClipboardWindow(void) { return opened ? owner_wnd : 0; }
EXPORT DWORD GetClipboardSequenceNumber(void) { int l = 0; return (DWORD)kernel_clip(1, 0, &l); }
EXPORT UINT RegisterClipboardFormatA(LPCSTR name) {
    for (int i = 0; i < nnames; i++) if (!ustricmp(names[i], name)) return 0xC100 + (UINT)i;
    if (nnames >= 16) return 0;
    ustrcpy(names[nnames], name, sizeof names[0]);
    return 0xC100 + (UINT)nnames++;
}
EXPORT UINT RegisterClipboardFormatW(LPCWSTR name) { char b[48]; WideCharToMultiByte(65001, 0, name, -1, b, sizeof b, 0, 0); return RegisterClipboardFormatA(b); }
EXPORT int GetClipboardFormatNameA(UINT fmt, LPSTR out, int cap) {
    if (fmt < 0xC100 || fmt >= 0xC100 + (UINT)nnames || cap <= 0) return 0;
    ustrcpy(out, names[fmt - 0xC100], cap);
    return ustrlen(out);
}
EXPORT int GetClipboardFormatNameW(UINT fmt, LPWSTR out, int cap) { char b[48]; int n = GetClipboardFormatNameA(fmt, b, sizeof b); if (n > 0) MultiByteToWideChar(65001, 0, b, -1, out, cap); return n; }
EXPORT HWND SetClipboardViewer(HWND h) { (void)h; return 0; }
EXPORT BOOL ChangeClipboardChain(HWND h, HWND n) { (void)h; (void)n; return TRUE; }
EXPORT BOOL AddClipboardFormatListener(HWND h) { (void)h; return TRUE; }
EXPORT BOOL RemoveClipboardFormatListener(HWND h) { (void)h; return TRUE; }
