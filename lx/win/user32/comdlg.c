// comdlg32 (inside user32.dll): GetOpenFileName / GetSaveFileName, FindText / ReplaceText, GetFileTitle, CommDlgExtendedError.
// The dialogs are built from our own controls; they browse the file system of the Linux subsystem through the Win32 file API.
#include "u32.h"

extern HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern BOOL EnableWindow(HWND h, BOOL e);
extern BOOL GetWindowRect(HWND h, RECT *r);
extern int GetWindowTextA(HWND h, LPSTR out, int cap);
extern BOOL SetWindowTextA(HWND h, LPCSTR s);
extern int MessageBoxA(HWND owner, LPCSTR text, LPCSTR cap, UINT type);
extern BOOL PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp);
extern UINT RegisterWindowMessageA(LPCSTR s);
extern BOOL DestroyWindow(HWND h);
extern BOOL IsWindow(HWND h);
extern ATOM RegisterClassExA(const WNDCLASSEXA *c);
extern BOOL SetForegroundWindow(HWND h);
extern HANDLE FindFirstFileA(LPCSTR pat, WIN32_FIND_DATAA *fd);
extern BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
extern BOOL FindClose(HANDLE h);
extern DWORD GetFileAttributesA(LPCSTR p);
extern DWORD GetCurrentDirectoryA(DWORD n, LPSTR b);
extern BOOL CreateDirectoryA(LPCSTR p, void *sa);
extern BOOL GetMessageA(MSG *m, HWND f, UINT lo, UINT hi);
extern LRESULT DispatchMessageA(const MSG *m);
extern void PostQuitMessage(int code);
extern UINT IsDlgButtonChecked(HWND h, int id);
extern BOOL CheckDlgButton(HWND h, int id, UINT c);
extern BOOL CheckRadioButton(HWND h, int first, int last, int check);

#define LBN_SELCHANGE_ 1
#define LBN_DBLCLK_ 2
#define CB_ADDSTRING_ 0x143
#define CB_GETCURSEL_ 0x147
#define CB_SETCURSEL_ 0x14E
#define CBN_SELCHANGE_ 1
#define OFN_OVERWRITEPROMPT 2
#define OFN_FILEMUSTEXIST 0x1000
#define OFN_PATHMUSTEXIST 0x800
#define OFN_HIDEREADONLY 4
#define FR_DOWN 1
#define FR_WHOLEWORD 2
#define FR_MATCHCASE 4
#define FR_FINDNEXT 8
#define FR_REPLACE 0x10
#define FR_REPLACEALL 0x20
#define FR_DIALOGTERM 0x40
#define FR_NOUPDOWN 0x400
#define FR_NOMATCHCASE 0x800
#define FR_NOWHOLEWORD 0x1000
#define FR_HIDEUPDOWN 0x4000
#define FR_HIDEMATCHCASE 0x8000
#define FR_HIDEWHOLEWORD 0x10000
#define ID_LIST 100
#define ID_NAME 101
#define ID_FILTER 102
#define ID_UP 103
#define ID_DIRLABEL 104
#define ID_NEWDIR 105

struct ofn {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpstrFilter; LPWSTR lpstrCustomFilter; DWORD nMaxCustFilter, nFilterIndex;
    LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle; LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags;
    WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt; LPARAM lCustData; void *lpfnHook; LPCWSTR lpTemplateName; void *pvReserved; DWORD dwReserved, FlagsEx;
};

static DWORD last_error;
EXPORT DWORD CommDlgExtendedError(void) { return last_error; }

static void to_u8(LPCWSTR w, char *out, int cap) { if (w) WideCharToMultiByte(65001, 0, w, -1, out, cap, 0, 0); else out[0] = 0; }

static int gmatch(const char *p, const char *s) {
    for (;;) {
        if (*p == '*') { while (*p == '*') p++; if (!*p) return 1; for (; *s; s++) if (gmatch(p, s)) return 1; return 0; }
        if (!*p) return !*s;
        if (!*s) return 0;
        int a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != '?' && a != b) return 0;
        p++; s++;
    }
}
static int match_any(const char *pats, const char *name) {          // "*.txt;*.log"
    char one[64]; int n = 0;
    for (const char *p = pats;; p++) {
        if (*p == ';' || !*p) { one[n] = 0; if (n && gmatch(one, name)) return 1; n = 0; if (!*p) break; }
        else if (n < 63) one[n++] = *p;
    }
    return 0;
}

// ---- the file dialog ----------------------------------------------------------------------------------------------------------------------------------
#define MAXENT 600
struct ent { char name[104]; int dir; };
struct fdlg {
    struct ofn *o; int save;
    char cwd[300];                              // "C:\dir\sub"
    char filt_desc[24][80], filt_pat[24][96]; int nfilt, curfilt;
    char custom_pat[96];
    struct ent ents[MAXENT]; int nent;
    HWND list, edit, combo, dirlabel;
    char defext[16];
    int done, result;
};

static int cmp_ci(const char *a, const char *b) { return ustricmp(a, b); }

static void fd_list(struct fdlg *f) {
    f->nent = 0;
    char pat[330];
    int l = ustrlen(f->cwd);
    ustrcpy(pat, f->cwd, sizeof pat);
    if (l && f->cwd[l - 1] != 92) { pat[l++] = 92; }
    pat[l++] = '*'; pat[l] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    const char *active = f->custom_pat[0] ? f->custom_pat : (f->nfilt ? f->filt_pat[f->curfilt] : "*.*");
    if (h != (HANDLE)(LONG_PTR)-1 && h) {
        do {
            if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
            int dir = (fd.dwFileAttributes & 0x10) != 0;
            if (!dir && !match_any(active, fd.cFileName)) continue;
            if (f->nent >= MAXENT) break;
            ustrcpy(f->ents[f->nent].name, fd.cFileName, sizeof f->ents[0].name);
            f->ents[f->nent].dir = dir;
            f->nent++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (int i = 1; i < f->nent; i++) {                                 // directories first, then by name
        struct ent e = f->ents[i];
        int j = i - 1;
        while (j >= 0 && ((f->ents[j].dir < e.dir) || (f->ents[j].dir == e.dir && cmp_ci(f->ents[j].name, e.name) > 0))) { f->ents[j + 1] = f->ents[j]; j--; }
        f->ents[j + 1] = e;
    }
    SendMessageA(f->list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < f->nent; i++) {
        char t[112];
        if (f->ents[i].dir) { t[0] = '['; ustrcpy(t + 1, f->ents[i].name, 100); int n = ustrlen(t); t[n] = ']'; t[n + 1] = 0; }
        else ustrcpy(t, f->ents[i].name, sizeof t);
        SendMessageA(f->list, LB_ADDSTRING, 0, (LPARAM)t);
    }
    SetWindowTextA(f->dirlabel, f->cwd);
}

static void fd_goto(struct fdlg *f, const char *dir) {
    ustrcpy(f->cwd, dir, sizeof f->cwd);
    f->custom_pat[0] = 0;
    fd_list(f);
}

static void fd_up(struct fdlg *f) {
    int l = ustrlen(f->cwd);
    if (l <= 3) return;
    while (l > 3 && f->cwd[l - 1] == 92) l--;
    while (l > 3 && f->cwd[l - 1] != 92) l--;
    if (l > 3) l--;
    f->cwd[l] = 0;
    if (l == 2) { f->cwd[2] = 92; f->cwd[3] = 0; }
    f->custom_pat[0] = 0;
    fd_list(f);
}

static int has_wild(const char *s) { for (; *s; s++) if (*s == '*' || *s == '?') return 1; return 0; }

static void fd_accept(struct fdlg *f, HWND dlg) {
    char name[300], full[600];
    GetWindowTextA(f->edit, name, sizeof name);
    int n = ustrlen(name);
    while (n > 0 && name[n - 1] == ' ') name[--n] = 0;
    if (!n) return;
    for (int i = 0; i < n; i++) if (name[i] == '/') name[i] = 92;
    if (has_wild(name)) { ustrcpy(f->custom_pat, name, sizeof f->custom_pat); fd_list(f); return; }
    if ((name[1] == ':' && name[2] == 92) || name[0] == 92) ustrcpy(full, name, sizeof full);
    else {
        ustrcpy(full, f->cwd, sizeof full);
        int l = ustrlen(full);
        if (l && full[l - 1] != 92) full[l++] = 92;
        ustrcpy(full + l, name, (int)sizeof full - l);
    }
    DWORD at = GetFileAttributesA(full);
    int exists = at != 0xFFFFFFFFu;
    if (exists && (at & 0x10)) { fd_goto(f, full); SetWindowTextA(f->edit, ""); return; }
    if (!f->save && !exists && (f->o->Flags & OFN_FILEMUSTEXIST)) {
        char m[400]; ustrcpy(m, name, 300); int k = ustrlen(m); ustrcpy(m + k, "\nFile not found.\nCheck the file name and try again.", (int)sizeof m - k);
        MessageBoxA(dlg, m, f->o->lpstrTitle ? "Open" : "Open", 0x30);
        return;
    }
    if (f->save && f->defext[0]) {                                       // the default extension when the name has none
        const char *base = full; for (const char *p = full; *p; p++) if (*p == 92) base = p + 1;
        int dot = 0; for (const char *p = base; *p; p++) if (*p == '.') dot = 1;
        if (!dot) { int k = ustrlen(full); full[k++] = '.'; ustrcpy(full + k, f->defext, (int)sizeof full - k); exists = GetFileAttributesA(full) != 0xFFFFFFFFu; }
    }
    if (f->save && exists && (f->o->Flags & OFN_OVERWRITEPROMPT)) {
        const char *base = full; for (const char *p = full; *p; p++) if (*p == 92) base = p + 1;
        char m[400]; ustrcpy(m, base, 300); int k = ustrlen(m); ustrcpy(m + k, " already exists.\nDo you want to replace it?", (int)sizeof m - k);
        if (MessageBoxA(dlg, m, "Confirm Save As", 0x34) != 6) return;     // MB_YESNO | MB_ICONWARNING; IDYES = 6
    }
    // the result
    struct ofn *o = f->o;
    int fl = ustrlen(full);
    if ((DWORD)fl + 2 > o->nMaxFile) { last_error = 0x3003; f->result = 0; f->done = 1; return; }       // FNERR_BUFFERTOOSMALL
    MultiByteToWideChar(65001, 0, full, -1, o->lpstrFile, (int)o->nMaxFile);
    int off = 0, ext = 0;
    for (int i = 0; full[i]; i++) { if (full[i] == 92) { off = i + 1; ext = 0; } else if (full[i] == '.') ext = i + 1; }
    o->nFileOffset = (WORD)off; o->nFileExtension = (WORD)(ext && ext > off ? ext : 0);
    if (o->lpstrFileTitle && o->nMaxFileTitle) MultiByteToWideChar(65001, 0, full + off, -1, o->lpstrFileTitle, (int)o->nMaxFileTitle);
    o->nFilterIndex = (DWORD)f->curfilt + 1;
    f->result = 1; f->done = 1;
}

static LRESULT fdlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct fdlg *f = (struct fdlg*)w->userdata;
    if (m == WM_COMMAND && f) {
        int id = (int)(wp & 0xFFFF), code = (int)((wp >> 16) & 0xFFFF);
        if (id == IDOK) { fd_accept(f, h); return 0; }
        if (id == IDCANCEL) { f->result = 0; f->done = 1; return 0; }
        if (id == ID_UP) { fd_up(f); return 0; }
        if (id == ID_LIST) {
            int sel = (int)SendMessageA(f->list, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= f->nent) return 0;
            if (code == LBN_SELCHANGE_) { if (!f->ents[sel].dir) SetWindowTextA(f->edit, f->ents[sel].name); }
            else if (code == LBN_DBLCLK_) {
                if (f->ents[sel].dir) {
                    char p[420]; ustrcpy(p, f->cwd, sizeof p);
                    int l = ustrlen(p); if (l && p[l - 1] != 92) p[l++] = 92;
                    ustrcpy(p + l, f->ents[sel].name, (int)sizeof p - l);
                    fd_goto(f, p);
                } else { SetWindowTextA(f->edit, f->ents[sel].name); fd_accept(f, h); }
            }
            return 0;
        }
        if (id == ID_FILTER && code == CBN_SELCHANGE_) {
            int s = (int)SendMessageA(f->combo, CB_GETCURSEL_, 0, 0);
            if (s >= 0 && s < f->nfilt) { f->curfilt = s; f->custom_pat[0] = 0; fd_list(f); }
            return 0;
        }
        return 0;
    }
    if (m == WM_CLOSE && f) { f->result = 0; f->done = 1; return 0; }
    return DefWindowProcA(h, m, wp, lp);
}

static int file_dialog(struct ofn *o, int save) {
    U_init();
    static int reg;
    if (!reg) {
        reg = 1;
        WNDCLASSEXA c; memset(&c, 0, sizeof c);
        c.cbSize = sizeof c; c.lpfnWndProc = fdlg_proc; c.lpszClassName = "OKFileDlg"; c.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
        RegisterClassExA(&c);
    }
    static struct fdlg F;                                     // (one file dialog at a time)
    memset(&F, 0, sizeof F);
    struct fdlg *f = &F;
    f->o = o; f->save = save;
    // filters: "Text (*.txt)\0*.txt\0All\0*.*\0\0"
    if (o->lpstrFilter) {
        const WCHAR *p = o->lpstrFilter;
        while (*p && f->nfilt < 24) {
            to_u8(p, f->filt_desc[f->nfilt], sizeof f->filt_desc[0]);
            while (*p) p++;
            p++;
            if (!*p) break;
            to_u8(p, f->filt_pat[f->nfilt], sizeof f->filt_pat[0]);
            while (*p) p++;
            p++;
            f->nfilt++;
        }
    }
    if (!f->nfilt) { ustrcpy(f->filt_desc[0], "All Files (*.*)", 80); ustrcpy(f->filt_pat[0], "*.*", 96); f->nfilt = 1; }
    f->curfilt = o->nFilterIndex >= 1 && (int)o->nFilterIndex <= f->nfilt ? (int)o->nFilterIndex - 1 : 0;
    if (o->lpstrDefExt) to_u8(o->lpstrDefExt, f->defext, sizeof f->defext);
    // start directory: the initial directory if it exists, else the root
    char dir[300] = "";
    to_u8(o->lpstrInitialDir, dir, sizeof dir);
    if (!dir[0] || GetFileAttributesA(dir) == 0xFFFFFFFFu || !(GetFileAttributesA(dir) & 0x10)) {
        char fn[300]; to_u8(o->lpstrFile, fn, sizeof fn);                   // a path typed into the file name buffer
        int l = ustrlen(fn);
        while (l > 0 && fn[l - 1] != 92) l--;
        if (l > 3) { fn[l - 1] = 0; ustrcpy(dir, fn, sizeof dir); }
        if (!dir[0] || !(GetFileAttributesA(dir) & 0x10) || GetFileAttributesA(dir) == 0xFFFFFFFFu) { dir[0] = 'C'; dir[1] = ':'; dir[2] = 92; dir[3] = 0; }
    }
    ustrcpy(f->cwd, dir, sizeof f->cwd);
    char title[96]; to_u8(o->lpstrTitle, title, sizeof title);
    if (!title[0]) ustrcpy(title, save ? "Save As" : "Open", sizeof title);
    int ttl, border, sw, sh; U_info(&ttl, &border, &sw, &sh);
    int cw = 520, ch = 360;
    int ow = cw + 2 * border, oh = ch + ttl + 2 * border;
    int x = (sw - ow) / 2, y = (sh - oh) / 3;
    struct wnd *owner = o->hwndOwner ? U_wnd(o->hwndOwner) : 0;
    if (owner) { RECT r; GetWindowRect(o->hwndOwner, &r); x = r.left + ((r.right - r.left) - ow) / 2; y = r.top + 30; if (x < 0) x = 0; if (y < 0) y = 0; if (x + ow > sw) x = sw - ow; if (y + oh > sh - 40) y = sh - 40 - oh; }
    HWND d = CreateWindowExA(0, "OKFileDlg", title, WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, ow, oh, o->hwndOwner, 0, 0, 0);
    if (!d) { last_error = 0x3002; return 0; }
    struct wnd *dw = d;
    dw->userdata = (LONG_PTR)f;
    f->dirlabel = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 10, cw - 100, 18, d, (HMENU)(LONG_PTR)ID_DIRLABEL, 0, 0);
    CreateWindowExA(0, "BUTTON", "Up", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cw - 80, 6, 68, 24, d, (HMENU)(LONG_PTR)ID_UP, 0, 0);
    f->list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | 1 /* LBS_NOTIFY */, 12, 38, cw - 24, ch - 38 - 118, d, (HMENU)(LONG_PTR)ID_LIST, 0, 0);
    CreateWindowExA(0, "STATIC", "File name:", WS_CHILD | WS_VISIBLE | SS_LEFT, 12, ch - 104, 80, 18, d, (HMENU)(LONG_PTR)-1, 0, 0);
    f->edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 96, ch - 108, cw - 108 - 100, 24, d, (HMENU)(LONG_PTR)ID_NAME, 0, 0);
    CreateWindowExA(0, "STATIC", save ? "Save as type:" : "Files of type:", WS_CHILD | WS_VISIBLE | SS_LEFT, 12, ch - 70, 84, 18, d, (HMENU)(LONG_PTR)-1, 0, 0);
    f->combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | 3 /* CBS_DROPDOWNLIST */, 96, ch - 74, cw - 108 - 100, 200, d, (HMENU)(LONG_PTR)ID_FILTER, 0, 0);
    for (int i = 0; i < f->nfilt; i++) SendMessageA(f->combo, CB_ADDSTRING_, 0, (LPARAM)f->filt_desc[i]);
    SendMessageA(f->combo, CB_SETCURSEL_, (WPARAM)f->curfilt, 0);
    CreateWindowExA(0, "BUTTON", save ? "Save" : "Open", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cw - 92, ch - 110, 80, 26, d, (HMENU)(LONG_PTR)IDOK, 0, 0);
    CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cw - 92, ch - 76, 80, 26, d, (HMENU)(LONG_PTR)IDCANCEL, 0, 0);
    if (o->lpstrFile && o->lpstrFile[0]) {                                      // a suggested name
        char nm[300]; to_u8(o->lpstrFile, nm, sizeof nm);
        const char *base = nm; for (const char *p = nm; *p; p++) if (*p == 92) base = p + 1;
        SetWindowTextA(f->edit, base);
        if (has_wild(base)) ustrcpy(f->custom_pat, base, sizeof f->custom_pat);        // "*.txt" in the name field is a filter
    }
    fd_list(f);
    ShowWindow(d, SW_SHOW);
    SetFocus(f->edit);
    SendMessageA(f->edit, EM_SETSEL, 0, -1);                                  // typing replaces the suggested name
    if (o->hwndOwner) EnableWindow(o->hwndOwner, FALSE);
    dw->dlg_defid = IDOK;
    while (dw->used && !f->done) {
        MSG msg;
        if (!GetMessageA(&msg, 0, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        if (IsDialogMessageA(d, &msg)) continue;
        DispatchMessageA(&msg);
    }
    if (o->hwndOwner) EnableWindow(o->hwndOwner, TRUE);
    if (dw->used) U_destroy(dw);
    if (o->hwndOwner) SetForegroundWindow(o->hwndOwner);
    return f->result;
}

EXPORT BOOL GetOpenFileNameW(struct ofn *o) { last_error = 0; return o ? file_dialog(o, 0) : 0; }
EXPORT BOOL GetSaveFileNameW(struct ofn *o) { last_error = 0; return o ? file_dialog(o, 1) : 0; }
EXPORT short GetFileTitleW(LPCWSTR file, LPWSTR title, WORD n) {
    int l = 0; while (file[l]) l++;
    int off = 0;
    for (int i = 0; i < l; i++) if (file[i] == '\\' || file[i] == '/' || file[i] == ':') off = i + 1;
    int tl = l - off;
    if (tl + 1 > (int)n) return (short)(tl + 1);
    for (int i = 0; i <= tl; i++) title[i] = file[off + i];
    return 0;
}

// ---- Find / Replace (modeless) --------------------------------------------------------------------------------------------------------------------------
struct findrep {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; DWORD Flags; LPWSTR lpstrFindWhat, lpstrReplaceWith; WORD wFindWhatLen, wReplaceWithLen;
    LPARAM lCustData; void *lpfnHook; LPCWSTR lpTemplateName;
};
#define ID_FIND_WHAT 201
#define ID_REPL_WITH 202
#define ID_CASE 203
#define ID_WORD 204
#define ID_UPDIR 205
#define ID_DOWNDIR 206
#define ID_FINDNEXT 1
#define ID_REPLACE 207
#define ID_REPLACEALL 208

struct frdlg { struct findrep *fr; int replace; HWND edit_find, edit_repl; UINT msg; };

static void fr_post(struct frdlg *s, HWND dlg, DWORD action) {
    struct findrep *fr = s->fr;
    char t[300];
    GetWindowTextA(s->edit_find, t, sizeof t);
    MultiByteToWideChar(65001, 0, t, -1, fr->lpstrFindWhat, fr->wFindWhatLen);
    if (s->replace && fr->lpstrReplaceWith) { GetWindowTextA(s->edit_repl, t, sizeof t); MultiByteToWideChar(65001, 0, t, -1, fr->lpstrReplaceWith, fr->wReplaceWithLen); }
    DWORD fl = fr->Flags & ~(FR_FINDNEXT | FR_REPLACE | FR_REPLACEALL | FR_MATCHCASE | FR_WHOLEWORD | FR_DOWN);
    if (IsDlgButtonChecked(dlg, ID_CASE)) fl |= FR_MATCHCASE;
    if (IsDlgButtonChecked(dlg, ID_WORD)) fl |= FR_WHOLEWORD;
    if (!(fr->Flags & (FR_HIDEUPDOWN | FR_NOUPDOWN)) ? IsDlgButtonChecked(dlg, ID_DOWNDIR) : 1) fl |= FR_DOWN;
    fr->Flags = fl | action;
    PostMessageA(fr->hwndOwner, s->msg, 0, (LPARAM)fr);
}

static LRESULT frdlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct frdlg *s = (struct frdlg*)w->userdata;
    if (m == WM_COMMAND && s) {
        int id = (int)(wp & 0xFFFF);
        if (id == ID_FINDNEXT || id == IDOK) { fr_post(s, h, FR_FINDNEXT); return 0; }
        if (id == ID_REPLACE) { fr_post(s, h, FR_REPLACE); return 0; }
        if (id == ID_REPLACEALL) { fr_post(s, h, FR_REPLACEALL); return 0; }
        if (id == IDCANCEL) { s->fr->Flags |= FR_DIALOGTERM; PostMessageA(s->fr->hwndOwner, s->msg, 0, (LPARAM)s->fr); DestroyWindow(h); return 0; }
        return 0;
    }
    if (m == WM_CLOSE && s) { s->fr->Flags |= FR_DIALOGTERM; PostMessageA(s->fr->hwndOwner, s->msg, 0, (LPARAM)s->fr); DestroyWindow(h); return 0; }
    if (m == WM_DESTROY && s) { ufree(s); w->userdata = 0; }
    return DefWindowProcA(h, m, wp, lp);
}

static HWND find_dialog(struct findrep *fr, int replace) {
    U_init();
    static int reg;
    if (!reg) {
        reg = 1;
        WNDCLASSEXA c; memset(&c, 0, sizeof c);
        c.cbSize = sizeof c; c.lpfnWndProc = frdlg_proc; c.lpszClassName = "OKFindDlg"; c.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1);
        RegisterClassExA(&c);
    }
    struct frdlg *s = (struct frdlg*)umalloc(sizeof *s);
    s->fr = fr; s->replace = replace; s->msg = RegisterWindowMessageA("commdlg_FindReplace");
    int ttl, border, sw, sh; U_info(&ttl, &border, &sw, &sh);
    int cw = 420, ch = replace ? 190 : 150;
    int ow = cw + 2 * border, oh = ch + ttl + 2 * border;
    int x = 200, y = 160;
    if (fr->hwndOwner) { RECT r; GetWindowRect(fr->hwndOwner, &r); x = r.left + 60; y = r.top + 70; }
    if (x + ow > sw) x = sw - ow;
    if (y + oh > sh - 40) y = sh - 40 - oh;
    HWND d = CreateWindowExA(0, "OKFindDlg", replace ? "Replace" : "Find", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, ow, oh, fr->hwndOwner, 0, 0, 0);
    if (!d) { ufree(s); return 0; }
    struct wnd *dw = d;
    dw->userdata = (LONG_PTR)s;
    CreateWindowExA(0, "STATIC", "Find what:", WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 16, 84, 18, d, (HMENU)(LONG_PTR)-1, 0, 0);
    s->edit_find = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 100, 12, 200, 24, d, (HMENU)(LONG_PTR)ID_FIND_WHAT, 0, 0);
    int y2 = 48;
    if (replace) {
        CreateWindowExA(0, "STATIC", "Replace with:", WS_CHILD | WS_VISIBLE | SS_LEFT, 12, 52, 88, 18, d, (HMENU)(LONG_PTR)-1, 0, 0);
        s->edit_repl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 100, 48, 200, 24, d, (HMENU)(LONG_PTR)ID_REPL_WITH, 0, 0);
        y2 = 84;
    }
    if (!(fr->Flags & (FR_HIDEWHOLEWORD | FR_NOWHOLEWORD))) CreateWindowExA(0, "BUTTON", "Match whole word only", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 12, y2, 200, 20, d, (HMENU)(LONG_PTR)ID_WORD, 0, 0);
    if (!(fr->Flags & (FR_HIDEMATCHCASE | FR_NOMATCHCASE))) CreateWindowExA(0, "BUTTON", "Match case", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 12, y2 + 24, 200, 20, d, (HMENU)(LONG_PTR)ID_CASE, 0, 0);
    if (!(fr->Flags & (FR_HIDEUPDOWN | FR_NOUPDOWN))) {
        CreateWindowExA(0, "BUTTON", "Up", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 230, y2 + 4, 60, 20, d, (HMENU)(LONG_PTR)ID_UPDIR, 0, 0);
        CreateWindowExA(0, "BUTTON", "Down", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 230, y2 + 28, 60, 20, d, (HMENU)(LONG_PTR)ID_DOWNDIR, 0, 0);
        CheckRadioButton(d, ID_UPDIR, ID_DOWNDIR, (fr->Flags & FR_DOWN) ? ID_DOWNDIR : ID_UPDIR);
    }
    if (fr->Flags & FR_MATCHCASE) CheckDlgButton(d, ID_CASE, 1);
    if (fr->Flags & FR_WHOLEWORD) CheckDlgButton(d, ID_WORD, 1);
    CreateWindowExA(0, "BUTTON", "Find Next", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cw - 108, 12, 96, 26, d, (HMENU)(LONG_PTR)ID_FINDNEXT, 0, 0);
    if (replace) {
        CreateWindowExA(0, "BUTTON", "Replace", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cw - 108, 44, 96, 26, d, (HMENU)(LONG_PTR)ID_REPLACE, 0, 0);
        CreateWindowExA(0, "BUTTON", "Replace All", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cw - 108, 76, 96, 26, d, (HMENU)(LONG_PTR)ID_REPLACEALL, 0, 0);
    }
    CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cw - 108, replace ? 108 : 44, 96, 26, d, (HMENU)(LONG_PTR)IDCANCEL, 0, 0);
    char init[300];
    to_u8(fr->lpstrFindWhat, init, sizeof init);
    SetWindowTextA(s->edit_find, init);
    if (replace && fr->lpstrReplaceWith) { to_u8(fr->lpstrReplaceWith, init, sizeof init); SetWindowTextA(s->edit_repl, init); }
    dw->dlg_defid = ID_FINDNEXT;
    ShowWindow(d, SW_SHOW);
    SetFocus(s->edit_find);
    return d;
}

EXPORT HWND FindTextW(struct findrep *fr) { return fr ? find_dialog(fr, 0) : 0; }
EXPORT HWND ReplaceTextW(struct findrep *fr) { return fr ? find_dialog(fr, 1) : 0; }

// ---- dialogs that are not there: they fail with CDERR_INITIALIZATION ------------------------------------------------------------------------------------
EXPORT BOOL PageSetupDlgW(void *p) { (void)p; last_error = 2; return 0; }
EXPORT LONG PrintDlgExW(void *p) { (void)p; last_error = 2; return (LONG)0x80004005; }
EXPORT BOOL ChooseFontW(void *p) { (void)p; last_error = 2; return 0; }
