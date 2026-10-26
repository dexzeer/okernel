// comctl32 (inside user32.dll): window subclassing, the status bar, minimal image lists, LoadIconWithScaleDown, TaskDialogIndirect.
#include "u32.h"

extern int DrawTextA(HDC dc, LPCSTR s, int n, RECT *rc, UINT fl);
extern int FillRect(HDC dc, const RECT *r, HBRUSH b);
extern BOOL GetClientRect(HWND h, RECT *r);
extern BOOL MoveWindow(HWND h, int x, int y, int w, int h2, BOOL repaint);
extern HWND GetParent(HWND h);
extern HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
extern HICON LoadIconW(HINSTANCE h, LPCWSTR name);
extern BOOL EnableWindow(HWND h, BOOL e);
extern ATOM RegisterClassExA(const WNDCLASSEXA *c);
extern LRESULT CallWindowProcW(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp);

#define WS_CHILD_ 0x40000000
#define WS_VISIBLE_ 0x10000000

// ---- window subclassing ---------------------------------------------------------------------------------------------------------------------
typedef LRESULT (*SUBCLASSPROC)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
struct sub { HWND h; SUBCLASSPROC fn; UINT_PTR id; DWORD_PTR ref; struct sub *next; };       // newest first = called first
struct subhead { HWND h; WNDPROC orig; struct sub *chain; struct subhead *next; };
static struct subhead *heads;
static struct sub *cur_sub;                                                                     // the entry being run (DefSubclassProc continues after it)

static struct subhead *head_of(HWND h) { for (struct subhead *s = heads; s; s = s->next) if (s->h == h) return s; return 0; }

static LRESULT sub_dispatch(HWND h, UINT m, WPARAM wp, LPARAM lp);

static LRESULT run_from(struct subhead *hd, struct sub *s, HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (!s) return CallWindowProcW(hd->orig, h, m, wp, lp);
    struct sub *save = cur_sub;
    cur_sub = s;
    LRESULT r = s->fn(h, m, wp, lp, s->id, s->ref);
    cur_sub = save;
    return r;
}

static LRESULT sub_dispatch(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct subhead *hd = head_of(h);
    if (!hd) return 0;
    return run_from(hd, hd->chain, h, m, wp, lp);
}

EXPORT BOOL SetWindowSubclass(HWND h, SUBCLASSPROC fn, UINT_PTR id, DWORD_PTR ref) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    struct subhead *hd = head_of(h);
    if (!hd) {
        hd = (struct subhead*)umalloc(sizeof *hd);
        hd->h = h; hd->orig = w->proc; hd->chain = 0; hd->next = heads; heads = hd;
        w->proc = sub_dispatch;
    }
    for (struct sub *s = hd->chain; s; s = s->next) if (s->fn == fn && s->id == id) { s->ref = ref; return 1; }
    struct sub *s = (struct sub*)umalloc(sizeof *s);
    s->h = h; s->fn = fn; s->id = id; s->ref = ref; s->next = hd->chain; hd->chain = s;
    return 1;
}

EXPORT BOOL GetWindowSubclass(HWND h, SUBCLASSPROC fn, UINT_PTR id, DWORD_PTR *ref) {
    struct subhead *hd = head_of(h);
    if (!hd) return 0;
    for (struct sub *s = hd->chain; s; s = s->next) if (s->fn == fn && s->id == id) { if (ref) *ref = s->ref; return 1; }
    return 0;
}

EXPORT BOOL RemoveWindowSubclass(HWND h, SUBCLASSPROC fn, UINT_PTR id) {
    struct subhead *hd = head_of(h);
    if (!hd) return 0;
    for (struct sub **pp = &hd->chain; *pp; pp = &(*pp)->next)
        if ((*pp)->fn == fn && (*pp)->id == id) {
            struct sub *d = *pp;
            *pp = d->next;
            if (cur_sub == d) cur_sub = d->next;                // removed while running: continue with the next one
            ufree(d);
            break;
        }
    if (!hd->chain) {
        struct wnd *w = U_wnd(h);
        if (w) w->proc = hd->orig;
        for (struct subhead **pp = &heads; *pp; pp = &(*pp)->next) if (*pp == hd) { *pp = hd->next; break; }
        ufree(hd);
    }
    return 1;
}

EXPORT LRESULT DefSubclassProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct subhead *hd = head_of(h);
    if (!hd) return DefWindowProcW(h, m, wp, lp);
    return run_from(hd, cur_sub ? cur_sub->next : hd->chain, h, m, wp, lp);
}

EXPORT BOOL InitCommonControlsEx(const void *icc) { (void)icc; return 1; }
EXPORT void InitCommonControls(void) {}

// ---- image lists: sizes and counts only (nothing here draws icons yet) -----------------------------------------------------------------------------
struct imagelist { int cx, cy, n; COLORREF bk; };
EXPORT void *ImageList_Create(int cx, int cy, UINT flags, int initial, int grow) {
    (void)flags; (void)initial; (void)grow;
    struct imagelist *l = (struct imagelist*)umalloc(sizeof *l);
    l->cx = cx; l->cy = cy; l->n = 0; l->bk = 0xFFFFFFFFu;
    return l;
}
EXPORT BOOL ImageList_Destroy(void *il) { ufree(il); return 1; }
EXPORT COLORREF ImageList_SetBkColor(void *il, COLORREF c) { struct imagelist *l = (struct imagelist*)il; COLORREF o = l->bk; l->bk = c; return o; }
EXPORT COLORREF ImageList_GetBkColor(void *il) { return ((struct imagelist*)il)->bk; }
EXPORT int ImageList_ReplaceIcon(void *il, int i, HICON icon) { (void)icon; struct imagelist *l = (struct imagelist*)il; if (i < 0) i = l->n++; return i; }
EXPORT int ImageList_Add(void *il, HBITMAP bm, HBITMAP mask) { (void)bm; (void)mask; return ((struct imagelist*)il)->n++; }
EXPORT int ImageList_AddMasked(void *il, HBITMAP bm, COLORREF c) { (void)bm; (void)c; return ((struct imagelist*)il)->n++; }
EXPORT int ImageList_GetImageCount(void *il) { return ((struct imagelist*)il)->n; }
EXPORT BOOL ImageList_Draw(void *il, int i, HDC dc, int x, int y, UINT style) { (void)il; (void)i; (void)dc; (void)x; (void)y; (void)style; return 1; }
EXPORT BOOL ImageList_GetIconSize(void *il, int *cx, int *cy) { struct imagelist *l = (struct imagelist*)il; if (cx) *cx = l->cx; if (cy) *cy = l->cy; return 1; }
EXPORT BOOL ImageList_Remove(void *il, int i) { struct imagelist *l = (struct imagelist*)il; if (i < 0) l->n = 0; else if (l->n > 0) l->n--; return 1; }

EXPORT LONG LoadIconWithScaleDown(HINSTANCE h, LPCWSTR name, int cx, int cy, HICON *out) { (void)cx; (void)cy; if (out) *out = LoadIconW(h, name); return 0; }

// ---- status bar (msctls_statusbar32) ------------------------------------------------------------------------------------------------------------------
#define SB_SETTEXTA 0x401
#define SB_GETTEXTA 0x402
#define SB_GETTEXTLENGTHA 0x403
#define SB_SETPARTS 0x404
#define SB_GETPARTS 0x406
#define SB_GETBORDERS 0x407
#define SB_SETMINHEIGHT 0x408
#define SB_SIMPLE 0x409
#define SB_GETRECT 0x40A
#define SB_SETTEXTW 0x40B
#define SB_GETTEXTLENGTHW 0x40C
#define SB_GETTEXTW 0x40D
#define SB_ISSIMPLE 0x40E
#define SB_SETBKCOLOR 0x2001
#define SBARS_SIZEGRIP 0x100
#define CCS_TOP 1
#define CCS_NOPARENTALIGN 8
#define CCS_NORESIZE 4
#define SB_MAXPARTS 16

struct sbar { int nparts, right[SB_MAXPARTS], simple, minh; char text[SB_MAXPARTS][160]; };

static int sb_height(struct wnd *w) { struct sbar *s = (struct sbar*)w->ctl; return s && s->minh > 22 ? s->minh : 22; }

static void sb_align(struct wnd *w) {
    if (!w->parent || (w->style & (CCS_NOPARENTALIGN | CCS_NORESIZE))) return;
    RECT pr;
    GetClientRect(w->parent, &pr);
    int h = sb_height(w);
    if (w->style & CCS_TOP) MoveWindow(w, 0, 0, pr.right - pr.left, h, 1);
    else MoveWindow(w, 0, pr.bottom - h, pr.right - pr.left, h, 1);
}

static LRESULT statusbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct sbar *s = (struct sbar*)w->ctl;
    switch (m) {
    case WM_NCCREATE:
        s = (struct sbar*)umalloc(sizeof *s);
        s->nparts = 1; s->right[0] = -1;
        w->ctl = s;
        return TRUE;
    case WM_CREATE: sb_align(w); return 0;
    case WM_NCDESTROY: ufree(w->ctl); w->ctl = 0; return DefWindowProcW(h, m, wp, lp);
    case WM_SIZE: sb_align(w); U_invalidate(w, 0, 1); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETTEXT: ustrcpy(s->text[0], (const char*)lp, sizeof s->text[0]); s->simple = 1; U_invalidate(w, 0, 1); return TRUE;
    case SB_SETTEXTA: case SB_SETTEXTW: {
        int part = (int)(wp & 0xFF);
        if (part == 255) { part = 0; s->simple = 1; }
        if (part < 0 || part >= SB_MAXPARTS) return FALSE;
        if (m == SB_SETTEXTW) {
            const WCHAR *src = (const WCHAR*)lp;
            if (src) WideCharToMultiByte(65001, 0, src, -1, s->text[part], sizeof s->text[part], 0, 0); else s->text[part][0] = 0;
        } else ustrcpy(s->text[part], lp ? (const char*)lp : "", sizeof s->text[part]);
        U_invalidate(w, 0, 1);
        return TRUE;
    }
    case SB_GETTEXTLENGTHA: case SB_GETTEXTLENGTHW: { int p = (int)(wp & 0xFF); return p < SB_MAXPARTS ? ustrlen(s->text[p]) : 0; }
    case SB_GETTEXTA: { int p = (int)(wp & 0xFF); if (p >= SB_MAXPARTS) return 0; ustrcpy((char*)lp, s->text[p], 160); return ustrlen(s->text[p]); }
    case SB_GETTEXTW: {
        int p = (int)(wp & 0xFF);
        if (p >= SB_MAXPARTS) return 0;
        MultiByteToWideChar(65001, 0, s->text[p], -1, (WCHAR*)lp, 160);
        return ustrlen(s->text[p]);
    }
    case SB_SETPARTS: {
        int n = (int)wp;
        if (n < 1) n = 1;
        if (n > SB_MAXPARTS) n = SB_MAXPARTS;
        s->nparts = n;
        for (int i = 0; i < n; i++) s->right[i] = ((const int*)lp)[i];
        U_invalidate(w, 0, 1);
        return TRUE;
    }
    case SB_GETPARTS: {
        int n = s->nparts;
        if (lp) for (int i = 0; i < (int)wp && i < n; i++) ((int*)lp)[i] = s->right[i];
        return n;
    }
    case SB_SETMINHEIGHT: s->minh = (int)wp; sb_align(w); return 0;
    case SB_SIMPLE: s->simple = wp != 0; U_invalidate(w, 0, 1); return TRUE;
    case SB_ISSIMPLE: return s->simple;
    case SB_GETBORDERS: if (lp) { ((int*)lp)[0] = 0; ((int*)lp)[1] = 2; ((int*)lp)[2] = 2; } return TRUE;
    case SB_GETRECT: {
        RECT *r = (RECT*)lp;
        int p = (int)wp;
        if (!r || p < 0 || p >= s->nparts) return FALSE;
        int left = p ? s->right[p - 1] : 0, right = s->right[p] < 0 ? w->cw : s->right[p];
        r->left = left; r->right = right; r->top = 0; r->bottom = w->ch;
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (!dc) return 0;
        RECT all = { 0, 0, w->cw, w->ch };
        FillRect(dc, &all, (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1));
        if (w->font) SelectObject(dc, w->font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (COLORREF)U_get_color(COLOR_WINDOWTEXT));
        int n = s->simple ? 1 : s->nparts;
        for (int i = 0; i < n; i++) {
            int left = (s->simple || !i) ? 0 : s->right[i - 1], right = (s->simple || s->right[i] < 0) ? w->cw : s->right[i];
            RECT r = { left + 1, 2, right - 1, w->ch - 1 };
            if (i == n - 1 && (w->style & SBARS_SIZEGRIP)) r.right -= 14;
            RECT edge = { left, 1, right - (i == n - 1 ? 0 : 0), w->ch };
            U_draw_edge(dc, &edge, 0, 1);
            RECT t = { r.left + 4, r.top, r.right - 2, r.bottom };
            DrawTextA(dc, s->text[s->simple ? 0 : i], -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(w, &ps);
        return 0;
    }
    default: return DefWindowProcW(h, m, wp, lp);
    }
}

EXPORT HWND CreateStatusWindowW(LONG style, LPCWSTR text, HWND parent, UINT id) {
    static const WCHAR cls[] = { 'm', 's', 'c', 't', 'l', 's', '_', 's', 't', 'a', 't', 'u', 's', 'b', 'a', 'r', '3', '2', 0 };
    return CreateWindowExW(0, cls, text, (DWORD)style | WS_CHILD_ | WS_VISIBLE_, 0, 0, 0, 0, parent, (HMENU)(ULONG_PTR)id, 0, 0);
}
EXPORT HWND CreateStatusWindowA(LONG style, LPCSTR text, HWND parent, UINT id) {
    return CreateWindowExA(0, "msctls_statusbar32", text, (DWORD)style | WS_CHILD_ | WS_VISIBLE_, 0, 0, 0, 0, parent, (HMENU)(ULONG_PTR)id, 0, 0);
}

// ---- TaskDialogIndirect: heading, text and buttons in one modal box (no icons, links, radio buttons or progress) -------------------------------------
extern int U_taskbox(HWND owner, const char *text, const char *caption, int nb, const int *ids, const char *const *names, int def);
static unsigned long long rd8(const BYTE *p) { unsigned long long v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static DWORD rd4(const BYTE *p) { return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24); }

extern int LoadStringW(HINSTANCE h, UINT id, LPWSTR buf, int cap);
// a string of the config: a pointer, or MAKEINTRESOURCE(id) of the program's string table
static void td_str(HINSTANCE h, ULONG_PTR p, char *out, int cap) {
    out[0] = 0;
    if (!p) return;
    WCHAR w[512];
    if (p < 0x10000) { if (LoadStringW(h, (UINT)p, w, 512) <= 0) return; WideCharToMultiByte(65001, 0, w, -1, out, cap, 0, 0); }
    else WideCharToMultiByte(65001, 0, (LPCWSTR)p, -1, out, cap, 0, 0);
}

EXPORT LONG TaskDialogIndirect(const BYTE *cfg, int *button, int *radio, BOOL *verified) {      // TASKDIALOGCONFIG is packed (1)
    if (!cfg) return (LONG)0x80070057;
    HWND owner = (HWND)rd8(cfg + 4);
    DWORD common = rd4(cfg + 24);
    HINSTANCE inst = (HINSTANCE)rd8(cfg + 12);
    ULONG_PTR title = (ULONG_PTR)rd8(cfg + 28), instr = (ULONG_PTR)rd8(cfg + 44), content = (ULONG_PTR)rd8(cfg + 52);
    DWORD nb = rd4(cfg + 60);
    const BYTE *btns = (const BYTE*)rd8(cfg + 64);
    int defb = (int)rd4(cfg + 72);
    char cap[128] = "", text[1024] = "", tmp[512];
    td_str(inst, title, cap, sizeof cap);
    td_str(inst, instr, text, sizeof text);
    if (content) {
        td_str(inst, content, tmp, sizeof tmp);
        int l = ustrlen(text);
        if (l && l < (int)sizeof text - 4) { text[l++] = 10; text[l++] = 10; text[l] = 0; }
        ustrcpy(text + l, tmp, (int)sizeof text - l);
    }
    int ids[16]; char labels[16][64]; const char *names[16]; int n = 0;
    for (DWORD i = 0; i < nb && n < 16; i++) {
        ids[n] = (int)rd4(btns + 12 * i);
        td_str(inst, (ULONG_PTR)rd8(btns + 12 * i + 4), labels[n], sizeof labels[n]);
        for (char *p = labels[n]; *p; p++) if (*p == 38) { char *q = p; while (*q) { *q = q[1]; q++; } break; }     // drop the & accelerator mark
        names[n] = labels[n]; n++;
    }
    static const struct { DWORD flag; int id; const char *name; } cb[] = { { 1, 1, "OK" }, { 2, 6, "Yes" }, { 4, 7, "No" }, { 0x10, 4, "Retry" }, { 8, 2, "Cancel" }, { 0x20, 8, "Close" } };
    for (int i = 0; i < 6 && n < 16; i++) if (common & cb[i].flag) { ids[n] = cb[i].id; names[n] = cb[i].name; n++; }
    if (!n) { ids[0] = 1; names[0] = "OK"; n = 1; }
    int def = 0;
    for (int i = 0; i < n; i++) if (ids[i] == defb) def = i;
    int r = U_taskbox(owner, text, cap, n, ids, names, def);
    if (button) *button = r;
    if (radio) *radio = 0;
    if (verified) *verified = 0;
    return 0;
}

void U_register_comctl(void) {
    U_add_class("msctls_statusbar32", statusbar_proc, 0, 0, 0, (HBRUSH)(ULONG_PTR)(COLOR_BTNFACE + 1), 0, 1, 1);
}
