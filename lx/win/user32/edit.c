// user32: the EDIT control. Unicode (UTF-16) text, single and multiple lines, word wrap, selection with mouse and keyboard,
// scroll bars, single-level undo, the clipboard and the EM_ messages programs like Notepad rely on (EM_GETHANDLE included).
#include "u32.h"

extern HDC BeginPaint(HWND h, PAINTSTRUCT *ps);
extern BOOL EndPaint(HWND h, const PAINTSTRUCT *ps);
extern int FillRect(HDC dc, const RECT *r, HBRUSH b);
extern BOOL TextOutW(HDC dc, int x, int y, LPCWSTR s, int n);
extern BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR s, int n, SIZE *sz);
extern BOOL GetTextMetricsW(HDC dc, TEXTMETRICW *tm);
extern int SetScrollInfo(HWND h, int bar, const void *info, BOOL redraw);
extern BOOL GetScrollInfo(HWND h, int bar, void *info);
extern HWND SetCapture(HWND h);
extern BOOL ReleaseCapture(void);
extern HWND SetFocus(HWND h);
extern BOOL InvalidateRect(HWND h, const RECT *r, BOOL e);
extern BOOL GetClientRect(HWND h, RECT *r);
extern HANDLE LocalAlloc(DWORD flags, SIZE_T n);
extern HANDLE LocalFree(HANDLE h);
extern void *LocalLock(HANDLE h);
extern HANDLE LocalReAlloc(HANDLE h, SIZE_T n, DWORD f);
extern BOOL LocalUnlock(HANDLE h);
extern void U_clip_set_text(const char *s, int n);
extern int U_clip_text(char *out, int cap);
extern BOOL TrackPopupMenu(HMENU hm, UINT flags, int x, int y, int res, HWND hw, const RECT *rc);
extern HMENU CreatePopupMenu(void);
extern BOOL AppendMenuW(HMENU m, UINT flags, UINT_PTR id, LPCWSTR s);
extern BOOL DestroyMenu(HMENU m);
extern BOOL GetCursorPos(POINT *p);

#define ES_LEFT 0
#define ES_CENTER 1
#define ES_RIGHT 2
#define ES_UPPERCASE 8
#define ES_LOWERCASE 0x10
#define ES_AUTOVSCROLL 0x40
#define ES_NOHIDESEL 0x100
#define ES_WANTRETURN 0x1000
#define EN_SETFOCUS 0x100
#define EN_KILLFOCUS 0x200
#define EN_UPDATE 0x400
#define EN_ERRSPACE 0x500
#define EN_MAXTEXT 0x501
#define EN_HSCROLL 0x601
#define EN_VSCROLL 0x602
#define EM_GETRECT 0xB2
#define EM_SETRECT 0xB3
#define EM_SETRECTNP 0xB4
#define EM_SCROLL 0xB5
#define EM_LINESCROLL 0xB6
#define EM_SCROLLCARET 0xB7
#define EM_GETMODIFY 0xB8
#define EM_SETMODIFY 0xB9
#define EM_GETLINECOUNT 0xBA
#define EM_LINEINDEX 0xBB
#define EM_SETHANDLE 0xBC
#define EM_GETHANDLE 0xBD
#define EM_GETTHUMB 0xBE
#define EM_LINELENGTH 0xC1
#define EM_REPLACESEL 0xC2
#define EM_GETLINE 0xC4
#define EM_LIMITTEXT 0xC5
#define EM_CANUNDO 0xC6
#define EM_UNDO 0xC7
#define EM_FMTLINES 0xC8
#define EM_LINEFROMCHAR 0xC9
#define EM_SETTABSTOPS 0xCB
#define EM_SETPASSWORDCHAR 0xCC
#define EM_EMPTYUNDOBUFFER 0xCD
#define EM_GETFIRSTVISIBLELINE 0xCE
#define EM_SETREADONLY 0xCF
#define EM_SETWORDBREAKPROC 0xD0
#define EM_GETWORDBREAKPROC 0xD1
#define EM_GETPASSWORDCHAR 0xD2
#define EM_SETMARGINS 0xD3
#define EM_GETMARGINS 0xD4
#define EM_GETLIMITTEXT 0xD5
#define EM_POSFROMCHAR 0xD6
#define EM_CHARFROMPOS 0xD7
#define EM_SETIMESTATUS 0xD8
#define EC_LEFTMARGIN 1
#define EC_RIGHTMARGIN 2
#define EC_USEFONTINFO 0xFFFF
#define WM_UNDO 0x304
#define WM_CUT 0x300
#define WM_COPY 0x301
#define WM_PASTE 0x302
#define WM_CLEAR 0x303
#define WM_SETREDRAW 0xB
#define WM_CONTEXTMENU 0x7B
#define WM_GETDLGCODE 0x87
#define DLGC_WANTARROWS 1
#define DLGC_WANTCHARS 0x80
#define DLGC_HASSETSEL 0x8
#define DLGC_WANTALLKEYS 4
#define SBV 1
#define SBH 0
#define SIF_ALL_ 0x17

typedef struct { UINT cbSize, fMask; int nMin, nMax; UINT nPage; int nPos, nTrackPos; } SCINFO;

typedef WCHAR wc;

struct edit {
    wc *buf; int len, cap;
    int a, b;                                    // selection anchor and caret (indexes)
    int *ls, *le; int nls, capls;                // lines: start and end (without the line break)
    int lay_w, lay_valid;                        // layout made for this client width
    int sy, sx, maxw;                            // first visible line, horizontal scroll (px), widest line (px)
    int lh, tabw;                                // line height, tab stop width
    int readonly, maxlen, modified, drag, drag_unit, drag_anchor_lo, drag_anchor_hi;
    wc pwchar;
    int lmargin, rmargin;
    int want_x;                                  // the x the caret column keeps over up/down moves (-1 none)
    int redraw, in_layout;
    // single-level undo: `ins` characters at `upos` replace the saved text
    wc *utext; int ulen, upos, uins, has_undo;
    int typing;                                  // the last operation was a typed character (merges into one undo step)
    HANDLE hlocal;                               // the buffer handed out by EM_GETHANDLE / taken by EM_SETHANDLE
    // character widths
    void *wfont; short w8[256]; unsigned char w8ok[256]; int wx_n; unsigned wx_ch[256]; short wx_w[256];
};

static void notify(struct wnd *w, int code) { if (w->parent) U_send(w->parent, WM_COMMAND, (WPARAM)((w->id & 0xFFFF) | ((unsigned)code << 16)), (LPARAM)w); }
static int multi(struct wnd *w) { return (w->style & ES_MULTILINE) != 0; }
static int wrapping(struct wnd *w) { return multi(w) && !(w->style & ES_AUTOHSCROLL); }

static void apply_font(struct wnd *w, HDC dc) { if (w->font) SelectObject(dc, w->font); }

// ---- widths ------------------------------------------------------------------------------------------------------------------------------------------
static int cwidth(struct wnd *w, struct edit *e, HDC dc, wc ch) {
    if (ch == '\t') return e->tabw;
    if (w->style & ES_PASSWORD) ch = e->pwchar ? e->pwchar : '*';
    if (ch < 256) {
        if (!e->w8ok[ch]) { SIZE sz; wc c = ch; GetTextExtentPoint32W(dc, &c, 1, &sz); e->w8[ch] = (short)sz.cx; e->w8ok[ch] = 1; }
        return e->w8[ch];
    }
    for (int i = 0; i < e->wx_n; i++) if (e->wx_ch[i] == ch) return e->wx_w[i];
    SIZE sz; wc c = ch; GetTextExtentPoint32W(dc, &c, 1, &sz);
    if (e->wx_n < 256) { e->wx_ch[e->wx_n] = ch; e->wx_w[e->wx_n] = (short)sz.cx; e->wx_n++; }
    return sz.cx;
}

static void metrics(struct wnd *w, struct edit *e, HDC dc) {            // font changes flush the caches
    if (e->wfont == w->font && e->lh) return;
    e->wfont = w->font;
    memset(e->w8ok, 0, sizeof e->w8ok);
    e->wx_n = 0;
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    e->lh = tm.tmHeight > 0 ? tm.tmHeight : 16;
    SIZE sz; wc sp = ' ';
    GetTextExtentPoint32W(dc, &sp, 1, &sz);
    e->tabw = sz.cx * 8 > 0 ? sz.cx * 8 : 32;
    e->lay_valid = 0;
}

// ---- buffer ------------------------------------------------------------------------------------------------------------------------------------------
static int reserve(struct edit *e, int need) {
    if (need + 1 <= e->cap) return 1;
    int nc = e->cap ? e->cap : 256;
    while (nc < need + 1) nc *= 2;
    wc *nb = (wc*)umalloc((SIZE_T)nc * 2);
    if (!nb) return 0;
    if (e->buf) { memcpy(nb, e->buf, (SIZE_T)(e->len + 1) * 2); ufree(e->buf); } else nb[0] = 0;
    e->buf = nb; e->cap = nc;
    return 1;
}

static void mv(void *d, const void *s, SIZE_T n) { char *a = (char*)d; const char *b = (const char*)s; if (a < b) { for (SIZE_T i = 0; i < n; i++) a[i] = b[i]; } else { while (n--) a[n] = b[n]; } }
static int wlen(const wc *s) { int n = 0; while (s && s[n]) n++; return n; }

// ---- layout ------------------------------------------------------------------------------------------------------------------------------------------
static void add_line(struct edit *e, int s, int en) {
    if (e->nls == e->capls) {
        int nc = e->capls ? e->capls * 2 : 256;
        int *a = (int*)umalloc((SIZE_T)nc * sizeof(int)), *b = (int*)umalloc((SIZE_T)nc * sizeof(int));
        if (e->nls) { memcpy(a, e->ls, (SIZE_T)e->nls * sizeof(int)); memcpy(b, e->le, (SIZE_T)e->nls * sizeof(int)); }
        ufree(e->ls); ufree(e->le);
        e->ls = a; e->le = b; e->capls = nc;
    }
    e->ls[e->nls] = s; e->le[e->nls] = en; e->nls++;
}

static int avail_w(struct wnd *w, struct edit *e) { int a = w->cw - e->lmargin - e->rmargin; return a > 8 ? a : 8; }

static void layout(struct wnd *w, struct edit *e) {
    if (e->lay_valid && e->lay_w == w->cw) return;
    HDC dc = GetDC(w);
    apply_font(w, dc);
    metrics(w, e, dc);
    e->nls = 0; e->maxw = 0;
    int n = e->len;
    if (!multi(w)) {                                              // one line: the whole text
        int x = 0;
        for (int k = 0; k < n; k++) x += cwidth(w, e, dc, e->buf[k]);
        e->maxw = x;
        add_line(e, 0, n);
    } else {
        int wrap = wrapping(w), avail = avail_w(w, e);
        int i = 0;
        for (;;) {
            int s = i, x = 0, brk = -1, brk_x = 0, hard = 0, end, next, lw;
            for (;;) {
                if (i >= n) { end = next = n; lw = x; break; }
                wc ch = e->buf[i];
                if (ch == '\n' || (ch == '\r' && i + 1 < n && e->buf[i + 1] == '\n')) { end = i; next = i + (ch == '\r' ? 2 : 1); lw = x; hard = 1; break; }
                int cwid = cwidth(w, e, dc, ch);
                if (wrap && x + cwid > avail && i > s) {
                    if (brk > s) { end = next = brk; lw = brk_x; } else { end = next = i; lw = x; }
                    break;
                }
                x += cwid;
                i++;
                if (ch == ' ' || ch == '\t' || ch == '-') { brk = i; brk_x = x; }
            }
            add_line(e, s, end);
            if (lw > e->maxw) e->maxw = lw;
            if (hard) { i = next; if (i >= n) { add_line(e, n, n); break; } continue; }         // a break at the very end leaves an empty last line
            if (next >= n && end == n) break;
            i = next;
        }
    }
    if (!e->nls) add_line(e, 0, 0);
    ReleaseDC(w, dc);
    e->lay_w = w->cw; e->lay_valid = 1;
}

static int line_of(struct edit *e, int idx) {                           // the line holding character index idx (binary search)
    int lo = 0, hi = e->nls - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (e->ls[mid] <= idx) lo = mid; else hi = mid - 1; }
    // an index at the end of a soft-wrapped line belongs to the next line only if the line ends exactly at idx and wrapped
    return lo;
}

static int xof(struct wnd *w, struct edit *e, HDC dc, int line, int idx) {
    int x = 0;
    int s = e->ls[line];
    if (idx > e->le[line]) idx = e->le[line];
    for (int k = s; k < idx; k++) x += cwidth(w, e, dc, e->buf[k]);
    return x;
}

static int visible_lines(struct wnd *w, struct edit *e) { int v = (w->ch - 2) / (e->lh ? e->lh : 16); return v < 1 ? 1 : v; }

static void clamp_scroll(struct wnd *w, struct edit *e);

// Scroll bars of the window come and go with the text: a bar is shown only when its direction needs one. A bar that appears
// narrows the client area, which can make the other one necessary, so this settles in a few passes.
static void update_bars(struct wnd *w, struct edit *e) {
    if (!multi(w) || e->in_layout) return;
    e->in_layout = 1;
    for (int pass = 0; pass < 3; pass++) {
        layout(w, e);
        int vis = visible_lines(w, e);
        int need_v = (w->style & WS_VSCROLL) && e->nls > vis;
        int need_h = (w->style & WS_HSCROLL) && !wrapping(w) && e->maxw + e->lmargin + e->rmargin > w->cw;
        int cur_v = U_sbv(w) != 0, cur_h = U_sbh(w) != 0;
        if ((!(w->style & WS_VSCROLL) || need_v == cur_v) && (!(w->style & WS_HSCROLL) || need_h == cur_h)) break;
        if ((w->style & WS_VSCROLL) && need_v != cur_v) U_sb_visible(w, 1, need_v);
        if ((w->style & WS_HSCROLL) && need_h != cur_h) U_sb_visible(w, 0, need_h);
        e->lay_valid = 0;                                                    // the client size changed
    }
    layout(w, e);
    clamp_scroll(w, e);
    if (U_sbv(w)) {
        SCINFO si = { sizeof si, SIF_ALL_, 0, e->nls > 0 ? e->nls - 1 : 0, (UINT)visible_lines(w, e), e->sy, 0 };
        SetScrollInfo(w, SBV, &si, 1);
    }
    if (U_sbh(w)) {
        SCINFO si = { sizeof si, SIF_ALL_, 0, e->maxw + e->lmargin + e->rmargin, (UINT)w->cw, e->sx, 0 };
        SetScrollInfo(w, SBH, &si, 1);
    }
    e->in_layout = 0;
}

static void clamp_scroll(struct wnd *w, struct edit *e) {
    int vis = visible_lines(w, e);
    int maxsy = e->nls - vis;
    if (maxsy < 0) maxsy = 0;
    if (e->sy > maxsy) e->sy = maxsy;
    if (e->sy < 0) e->sy = 0;
    int maxsx = e->maxw + e->lmargin + e->rmargin - w->cw;
    if (wrapping(w) || maxsx < 0) maxsx = 0;
    if (e->sx > maxsx) e->sx = maxsx;
    if (e->sx < 0) e->sx = 0;
}

static void scroll_to_caret(struct wnd *w, struct edit *e) {
    layout(w, e);
    HDC dc = GetDC(w); apply_font(w, dc); metrics(w, e, dc);
    int line = line_of(e, e->b);
    int vis = visible_lines(w, e);
    if (multi(w)) {
        if (line < e->sy) e->sy = line;
        if (line >= e->sy + vis) e->sy = line - vis + 1;
    }
    int x = xof(w, e, dc, line, e->b) + e->lmargin;
    if (!wrapping(w)) {
        if (x - e->sx > w->cw - 4) e->sx = x - (w->cw - 4);
        if (x - e->sx < 0) e->sx = x > 8 ? x - 8 : 0;
    }
    ReleaseDC(w, dc);
    clamp_scroll(w, e);
}

static void relayout(struct wnd *w, struct edit *e) {
    e->lay_valid = 0;
    layout(w, e);
    update_bars(w, e);
    clamp_scroll(w, e);
}

static void invalidate(struct wnd *w, struct edit *e) { if (e->redraw) U_invalidate(w, 0, 1); }

// ---- editing -----------------------------------------------------------------------------------------------------------------------------------------
static int sel_lo(struct edit *e) { return e->a < e->b ? e->a : e->b; }
static int sel_hi(struct edit *e) { return e->a < e->b ? e->b : e->a; }

static void undo_clear(struct edit *e) { ufree(e->utext); e->utext = 0; e->ulen = e->upos = e->uins = 0; e->has_undo = 0; e->typing = 0; }

// replace [lo, hi) with n characters; records the undo step; keeps the caret after the inserted text
static int replace(struct wnd *w, struct edit *e, int lo, int hi, const wc *s, int n, int typed, int record) {
    if (lo < 0) lo = 0;
    if (hi > e->len) hi = e->len;
    if (lo > hi) lo = hi;
    if (e->maxlen && e->len - (hi - lo) + n > e->maxlen) { n = e->maxlen - (e->len - (hi - lo)); if (n < 0) n = 0; notify(w, EN_MAXTEXT); }
    if (!n && hi == lo) return 0;
    if (!reserve(e, e->len - (hi - lo) + n)) { notify(w, EN_ERRSPACE); return 0; }
    if (record) {
        if (typed && e->typing && e->has_undo && lo == e->upos + e->uins && hi == lo) { e->uins += n; }          // consecutive typing: one step
        else if (typed && e->typing && e->has_undo && n == 0 && hi == lo + 1 && lo + 1 == e->upos) {            // backspace over typed text: shrink / extend
            wc *nt = (wc*)umalloc((SIZE_T)(e->ulen + 1 + 1) * 2);
            nt[0] = e->buf[lo]; memcpy(nt + 1, e->utext, (SIZE_T)e->ulen * 2);
            ufree(e->utext); e->utext = nt; e->ulen++; e->upos = lo;
        } else {
            ufree(e->utext);
            e->ulen = hi - lo;
            e->utext = (wc*)umalloc((SIZE_T)(e->ulen + 1) * 2);
            if (e->ulen) memcpy(e->utext, e->buf + lo, (SIZE_T)e->ulen * 2);
            e->upos = lo; e->uins = n; e->has_undo = 1;
        }
        e->typing = typed;
    }
    mv(e->buf + lo + n, e->buf + hi, (SIZE_T)(e->len - hi + 1) * 2);
    if (n) memcpy(e->buf + lo, s, (SIZE_T)n * 2);
    e->len += n - (hi - lo);
    e->a = e->b = lo + n;
    e->modified = 1; e->want_x = -1;
    relayout(w, e);
    scroll_to_caret(w, e);
    update_bars(w, e);
    notify(w, EN_UPDATE);                                              // after the text changed, before it is redrawn
    invalidate(w, e);
    notify(w, EN_CHANGE);
    return 1;
}

static void do_undo(struct wnd *w, struct edit *e) {
    if (!e->has_undo) return;
    wc *old = e->utext; int olen = e->ulen, pos = e->upos, ins = e->uins;
    // the inverse step: the `ins` characters at pos go, `old` comes back; what goes is saved for a redo
    wc *gone = (wc*)umalloc((SIZE_T)(ins + 1) * 2);
    if (ins) memcpy(gone, e->buf + pos, (SIZE_T)ins * 2);
    e->utext = 0; e->has_undo = 0;
    replace(w, e, pos, pos + ins, old, olen, 0, 0);
    ufree(e->utext);
    e->utext = gone; e->ulen = ins; e->upos = pos; e->uins = olen; e->has_undo = 1; e->typing = 0;
    ufree(old);
    e->a = pos; e->b = pos + olen;
    invalidate(w, e);
}

static void set_text(struct wnd *w, struct edit *e, const wc *s, int n) {
    if (e->maxlen && n > e->maxlen) n = e->maxlen;
    reserve(e, n);
    if (n) memcpy(e->buf, s, (SIZE_T)n * 2);
    e->buf[n] = 0;
    e->len = n; e->a = e->b = 0; e->sy = e->sx = 0; e->modified = 0; e->want_x = -1;
    undo_clear(e);
    relayout(w, e);
    invalidate(w, e);
}

// text between the application's (maybe ANSI) world and ours
static int to_wide(struct wnd *w, LPARAM lp, wc **out) {
    if (!lp) { *out = (wc*)umalloc(2); return 0; }
    if (w->unicode) { int n = wlen((const wc*)lp); *out = (wc*)umalloc((SIZE_T)(n + 1) * 2); memcpy(*out, (const void*)lp, (SIZE_T)(n + 1) * 2); return n; }
    int n = MultiByteToWideChar(65001, 0, (LPCSTR)lp, -1, 0, 0);
    if (n <= 0) n = 1;
    *out = (wc*)umalloc((SIZE_T)(n + 1) * 2);
    MultiByteToWideChar(65001, 0, (LPCSTR)lp, -1, *out, n);
    return n - 1;
}

// copy the selection to the clipboard (UTF-8)
static void do_copy(struct edit *e) {
    int lo = sel_lo(e), hi = sel_hi(e);
    if (lo == hi) return;
    int cap = (hi - lo) * 3 + 4;
    if (cap > 65000) { cap = 65000; }
    char *t = (char*)umalloc((SIZE_T)cap);
    int n = WideCharToMultiByte(65001, 0, e->buf + lo, hi - lo, t, cap - 1, 0, 0);
    if (n < 0) n = 0;
    U_clip_set_text(t, n);
    ufree(t);
}

static void do_paste(struct wnd *w, struct edit *e) {
    char *t = (char*)umalloc(65536);
    int n = U_clip_text(t, 65536);
    if (n > 0) {
        wc *wt = (wc*)umalloc((SIZE_T)(n + 1) * 2 * 2);
        int m = MultiByteToWideChar(65001, 0, t, n, wt, n + 1);
        if (m < 0) m = 0;
        // one line: stop at the first line break; several: CR LF pairs
        int o = 0;
        for (int i = 0; i < m; i++) {
            wc ch = wt[i];
            if (!multi(w) && (ch == '\r' || ch == '\n')) break;
            if (multi(w) && ch == '\n' && !(i > 0 && wt[i - 1] == '\r')) wt[o++] = '\r';
            if (ch == '\r' && multi(w) && !(i + 1 < m && wt[i + 1] == '\n')) { wt[o++] = '\r'; wt[o++] = '\n'; continue; }
            wt[o++] = ch;
        }
        replace(w, e, sel_lo(e), sel_hi(e), wt, o, 0, 1);
        ufree(wt);
    }
    ufree(t);
}

// ---- positions ----------------------------------------------------------------------------------------------------------------------------------------
static int hit(struct wnd *w, struct edit *e, HDC dc, int x, int y) {
    int line = multi(w) ? e->sy + (y - 1) / e->lh : 0;
    if (y < 1 && multi(w)) line = e->sy - 1;
    if (line < 0) line = 0;
    if (line >= e->nls) line = e->nls - 1;
    int target = x + e->sx - e->lmargin;
    int i = e->ls[line], le = e->le[line], px = 0;
    while (i < le) {
        int cw_ = cwidth(w, e, dc, e->buf[i]);
        if (target < px + cw_ / 2) break;
        px += cw_; i++;
    }
    return i;
}

static int is_word(wc c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c > 127; }
static int word_start(struct edit *e, int i) { while (i > 0 && !is_word(e->buf[i - 1]) && e->buf[i - 1] != '\n') i--; while (i > 0 && is_word(e->buf[i - 1])) i--; return i; }
static int word_end(struct edit *e, int i) { while (i < e->len && is_word(e->buf[i])) i++; while (i < e->len && (e->buf[i] == ' ' || e->buf[i] == '\t')) i++; return i; }

static void move_to(struct wnd *w, struct edit *e, int to, int extend, int keep_x) {
    if (to < 0) to = 0;
    if (to > e->len) to = e->len;
    e->b = to;
    if (!extend) e->a = to;
    if (!keep_x) e->want_x = -1;
    e->typing = 0;
    scroll_to_caret(w, e);
    update_bars(w, e);
    invalidate(w, e);
}

static int prev_char(struct edit *e, int i) { if (i > 1 && e->buf[i - 1] == '\n' && e->buf[i - 2] == '\r') return i - 2; return i > 0 ? i - 1 : 0; }
static int next_char(struct edit *e, int i) { if (i + 1 < e->len && e->buf[i] == '\r' && e->buf[i + 1] == '\n') return i + 2; return i < e->len ? i + 1 : i; }

// ---- painting -----------------------------------------------------------------------------------------------------------------------------------------
static void paint(struct wnd *w) {
    struct edit *e = (struct edit*)w->ctl;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    if (!dc) return;
    layout(w, e);
    apply_font(w, dc);
    RECT rc = { 0, 0, w->cw, w->ch };
    LRESULT cb = w->parent ? U_send(w->parent, WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)w) : 0;
    if (cb) FillRect(dc, &rc, (HBRUSH)cb);
    else __gdi_rect_fill_color(dc, 0, 0, w->cw, w->ch, (w->enabled && !e->readonly) ? RGB(255, 255, 255) : (COLORREF)U_get_color(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    COLORREF fg = w->enabled ? (COLORREF)U_get_color(COLOR_WINDOWTEXT) : (COLORREF)U_get_color(17);
    SetTextColor(dc, fg);
    int focused = U_focus() == w;
    int showsel = focused || (w->style & ES_NOHIDESEL);
    int lo = sel_lo(e), hi = sel_hi(e);
    int first = e->sy, vis = visible_lines(w, e) + 1;
    int pw = (w->style & ES_PASSWORD) != 0;
    for (int ln = first; ln < first + vis && ln < e->nls; ln++) {
        int s = e->ls[ln], en = e->le[ln];
        int y = 1 + (ln - first) * e->lh, x0 = e->lmargin - e->sx;
        if (!multi(w) && (w->style & (ES_CENTER | ES_RIGHT))) {                  // single line alignment
            int tw = xof(w, e, dc, ln, en);
            x0 = (w->style & ES_RIGHT) ? w->cw - tw - e->rmargin : (w->cw - tw) / 2;
        }
        int x = x0, k = s;
        COLORREF selbg = focused ? (COLORREF)U_get_color(13) : (COLORREF)U_get_color(16);
        while (k < en) {
            int sel = showsel && k >= lo && k < hi;
            if (e->buf[k] == '\t') {                                              // a tab: blank, highlighted when selected
                if (sel) __gdi_rect_fill_color(dc, x, y, x + e->tabw, y + e->lh, selbg);
                x += e->tabw; k++;
                continue;
            }
            int r = k + 1;
            while (r < en && e->buf[r] != '\t' && (showsel && r >= lo && r < hi) == sel) r++;
            int rw = 0;
            for (int q = k; q < r; q++) rw += cwidth(w, e, dc, e->buf[q]);
            if (sel) { __gdi_rect_fill_color(dc, x, y, x + rw, y + e->lh, selbg); SetTextColor(dc, focused ? (COLORREF)U_get_color(14) : fg); }
            if (pw) { wc stars[256]; int cnt = r - k > 255 ? 255 : r - k; for (int q = 0; q < cnt; q++) stars[q] = e->pwchar ? e->pwchar : '*'; TextOutW(dc, x, y, stars, cnt); }
            else TextOutW(dc, x, y, e->buf + k, r - k);
            if (sel) SetTextColor(dc, fg);
            x += rw; k = r;
        }
        if (showsel && ln + 1 < e->nls && e->ls[ln + 1] > en && lo <= en && hi > en)           // a selected line break: a small block
            __gdi_rect_fill_color(dc, x, y, x + cwidth(w, e, dc, ' ') / 2 + 2, y + e->lh, selbg);
    }
    if (focused && w->enabled) {
        int line = line_of(e, e->b);
        if (line >= first && line < first + vis) {
            int x = e->lmargin - e->sx + xof(w, e, dc, line, e->b);
            if (!multi(w) && (w->style & (ES_CENTER | ES_RIGHT))) { int tw = xof(w, e, dc, line, e->le[line]); x += (w->style & ES_RIGHT) ? w->cw - tw - e->rmargin - e->lmargin : (w->cw - tw) / 2 - e->lmargin; }
            int y = 1 + (line - first) * e->lh;
            __gdi_rect_fill_color(dc, x, y, x + 1, y + e->lh, fg);
        }
    }
    EndPaint(w, &ps);
}

// ---- keyboard ---------------------------------------------------------------------------------------------------------------------------------------
static void vertical_move(struct wnd *w, struct edit *e, int dlines, int extend) {
    layout(w, e);
    HDC dc = GetDC(w); apply_font(w, dc); metrics(w, e, dc);
    int line = line_of(e, e->b);
    if (e->want_x < 0) e->want_x = xof(w, e, dc, line, e->b);
    int nl = line + dlines;
    if (nl < 0) nl = 0;
    if (nl >= e->nls) nl = e->nls - 1;
    int i = e->ls[nl], le = e->le[nl], px = 0;
    while (i < le) { int cw_ = cwidth(w, e, dc, e->buf[i]); if (e->want_x < px + cw_ / 2) break; px += cw_; i++; }
    ReleaseDC(w, dc);
    if (nl == line && dlines != 0) i = dlines < 0 ? 0 : e->len;                    // up on the first line / down on the last: to the ends
    move_to(w, e, i, extend, 1);
}

static void key_down(struct wnd *w, struct edit *e, WPARAM vk) {
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    layout(w, e);
    int line = line_of(e, e->b);
    switch (vk) {
    case VK_LEFT:
        if (e->a != e->b && !shift) move_to(w, e, sel_lo(e), 0, 0);
        else move_to(w, e, ctl ? word_start(e, e->b > 0 ? prev_char(e, e->b) : 0) : prev_char(e, e->b), shift, 0);
        break;
    case VK_RIGHT:
        if (e->a != e->b && !shift) move_to(w, e, sel_hi(e), 0, 0);
        else move_to(w, e, ctl ? word_end(e, e->b) : next_char(e, e->b), shift, 0);
        break;
    case VK_UP: if (multi(w)) vertical_move(w, e, -1, shift); break;
    case VK_DOWN: if (multi(w)) vertical_move(w, e, 1, shift); break;
    case 33: if (multi(w)) vertical_move(w, e, -visible_lines(w, e), shift); break;          // Page Up
    case 34: if (multi(w)) vertical_move(w, e, visible_lines(w, e), shift); break;           // Page Down
    case 36: move_to(w, e, ctl ? 0 : e->ls[line], shift, 0); break;                           // Home
    case 35: move_to(w, e, ctl ? e->len : e->le[line], shift, 0); break;                      // End
    case VK_DELETE:
        if (e->readonly || !w->enabled) break;
        if (shift && e->a != e->b) { do_copy(e); replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); break; }
        if (e->a != e->b) replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1);
        else if (e->b < e->len) replace(w, e, e->b, ctl ? word_end(e, e->b) : next_char(e, e->b), 0, 0, 1, 1);
        break;
    case 45:                                                                                  // Insert: Shift+Insert pastes, Ctrl+Insert copies
        if (shift && !e->readonly) do_paste(w, e); else if (ctl) do_copy(e);
        break;
    default: break;
    }
}

static void on_char(struct wnd *w, struct edit *e, int c) {
    if (!w->enabled) return;
    if (c == 3) { do_copy(e); return; }                                                       // Ctrl+C
    if (c == 1) { e->a = 0; e->b = e->len; invalidate(w, e); return; }                        // Ctrl+A
    if (e->readonly) return;
    if (c == 24) { do_copy(e); replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); return; }     // Ctrl+X
    if (c == 22) { do_paste(w, e); return; }                                                  // Ctrl+V
    if (c == 26) { do_undo(w, e); return; }                                                   // Ctrl+Z
    if (c == 8) {
        if (e->a != e->b) replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1);
        else if (e->b > 0) replace(w, e, prev_char(e, e->b), e->b, 0, 0, 1, 1);
        return;
    }
    if (c == 127) { if (e->a == e->b && e->b > 0) e->a = word_start(e, e->b); replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); return; }
    if (c == 13 || c == 10) {
        if (!multi(w)) return;
        static const wc crlf[2] = { '\r', '\n' };
        replace(w, e, sel_lo(e), sel_hi(e), crlf, 2, 0, 1);
        return;
    }
    if (c == 9 && !multi(w)) return;
    if (c < 32 && c != 9) return;
    if ((w->style & ES_NUMBER) && (c < '0' || c > '9')) return;
    if ((w->style & ES_UPPERCASE) && c >= 'a' && c <= 'z') c -= 32;
    if ((w->style & ES_LOWERCASE) && c >= 'A' && c <= 'Z') c += 32;
    wc ch = (wc)c;
    int lo = sel_lo(e), hi = sel_hi(e);
    replace(w, e, lo, hi, &ch, 1, lo == hi, 1);
}

// ---- the context menu ------------------------------------------------------------------------------------------------------------------------------------
static void context_menu(struct wnd *w, struct edit *e, int sx, int sy) {
    static const WCHAR t_undo[] = { 'U', 'n', 'd', 'o', 0 }, t_cut[] = { 'C', 'u', 't', 0 }, t_copy[] = { 'C', 'o', 'p', 'y', 0 }, t_paste[] = { 'P', 'a', 's', 't', 'e', 0 },
        t_del[] = { 'D', 'e', 'l', 'e', 't', 'e', 0 }, t_all[] = { 'S', 'e', 'l', 'e', 'c', 't', ' ', 'A', 'l', 'l', 0 };
    HMENU m = CreatePopupMenu();
    int has_sel = e->a != e->b, ro = e->readonly;
    AppendMenuW(m, (!ro && e->has_undo) ? 0 : 3, 1, t_undo);
    AppendMenuW(m, 0x800, 0, 0);
    AppendMenuW(m, (!ro && has_sel) ? 0 : 3, 2, t_cut);
    AppendMenuW(m, has_sel ? 0 : 3, 3, t_copy);
    AppendMenuW(m, ro ? 3 : 0, 4, t_paste);
    AppendMenuW(m, (!ro && has_sel) ? 0 : 3, 5, t_del);
    AppendMenuW(m, 0x800, 0, 0);
    AppendMenuW(m, e->len ? 0 : 3, 6, t_all);
    int cmd = TrackPopupMenu(m, 0x100 /* TPM_RETURNCMD */, sx, sy, 0, w, 0);
    DestroyMenu(m);
    switch (cmd) {
    case 1: do_undo(w, e); break;
    case 2: do_copy(e); replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); break;
    case 3: do_copy(e); break;
    case 4: do_paste(w, e); break;
    case 5: replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); break;
    case 6: e->a = 0; e->b = e->len; invalidate(w, e); break;
    }
}

// ---- the window procedure --------------------------------------------------------------------------------------------------------------------------------
static void free_edit(struct edit *e) {
    ufree(e->buf); ufree(e->ls); ufree(e->le); ufree(e->utext);
    if (e->hlocal) LocalFree(e->hlocal);
    ufree(e);
}

static const wc empty_w[1] = { 0 };
static int line_len_at(struct edit *e, int line) { return e->le[line] - e->ls[line]; }

LRESULT U_edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct edit *e = (struct edit*)w->ctl;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CREATE: {
        e = (struct edit*)umalloc(sizeof *e);
        w->ctl = e;
        if (!e) return -1;
        e->redraw = 1; e->want_x = -1; e->lmargin = e->rmargin = multi(w) ? 1 : 2;
        e->lay_w = -1;
        {   // the initial text is the window text (UTF-8)
            int need = MultiByteToWideChar(65001, 0, w->text, -1, 0, 0);
            wc *wt = (wc*)umalloc((SIZE_T)(need > 0 ? need : 1) * 2 + 2);
            int k = need > 0 ? MultiByteToWideChar(65001, 0, w->text, -1, wt, need) - 1 : 0;
            if (k < 0) k = 0;
            reserve(e, k);
            if (k) memcpy(e->buf, wt, (SIZE_T)k * 2);
            e->buf[k] = 0; e->len = k;
            ufree(wt);
        }
        if (w->style & ES_READONLY) e->readonly = 1;
        return 0;
    }
    case WM_NCDESTROY: if (e) { free_edit(e); w->ctl = 0; } return 0;
    case WM_ERASEBKGND: return 1;
    default: break;
    }
    if (!e) return U_def_proc(w, m, wp, lp, w->unicode);
    switch (m) {
    case WM_SIZE: relayout(w, e); invalidate(w, e); return 0;
    case WM_SETFONT: w->font = (void*)wp; e->wfont = 0; e->lh = 0; e->lay_valid = 0; relayout(w, e); if (lp) invalidate(w, e); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case WM_SETREDRAW: e->redraw = wp != 0; if (e->redraw) U_invalidate(w, 0, 1); return 0;
    case WM_ENABLE: U_invalidate(w, 0, 1); return 0;
    case WM_SETFOCUS: notify(w, EN_SETFOCUS); U_invalidate(w, 0, 1); return 0;
    case WM_KILLFOCUS: notify(w, EN_KILLFOCUS); U_invalidate(w, 0, 1); return 0;
    case WM_GETDLGCODE: return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_HASSETSEL | ((multi(w) && (w->style & ES_WANTRETURN)) ? DLGC_WANTALLKEYS : 0);
    case WM_PAINT: paint(w); return 0;
    case WM_SETTEXT: {
        wc *t; int n = to_wide(w, lp, &t);
        set_text(w, e, t, n);
        ufree(t);
        notify(w, EN_CHANGE);
        return TRUE;
    }
    case WM_GETTEXT: {
        if (!wp) return 0;
        int n = e->len;
        if (w->unicode) { int c = n < (int)wp - 1 ? n : (int)wp - 1; memcpy((void*)lp, e->buf, (SIZE_T)c * 2); ((wc*)lp)[c] = 0; return c; }
        int r = WideCharToMultiByte(65001, 0, e->buf, n, (char*)lp, (int)wp - 1, 0, 0);
        if (r < 0) r = 0;
        ((char*)lp)[r] = 0;
        return r;
    }
    case WM_GETTEXTLENGTH:
        if (w->unicode) return e->len;
        { int r = WideCharToMultiByte(65001, 0, e->buf, e->len, 0, 0, 0, 0); return r < 0 ? 0 : r; }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (U_focus() != w) SetFocus(w);
        layout(w, e);
        HDC dc = GetDC(w); apply_font(w, dc); metrics(w, e, dc);
        int off = hit(w, e, dc, GET_X(lp), GET_Y(lp));
        ReleaseDC(w, dc);
        int shift = (wp & 4) != 0;                                                            // MK_SHIFT
        e->typing = 0; e->want_x = -1;
        if (m == WM_LBUTTONDBLCLK) {
            int s = off; while (s > 0 && is_word(e->buf[s - 1])) s--;
            int t = off; while (t < e->len && is_word(e->buf[t])) t++;
            if (s == t && off < e->len) t = off + 1;
            e->a = s; e->b = t; e->drag = 1; e->drag_unit = 1; e->drag_anchor_lo = s; e->drag_anchor_hi = t;
            SetCapture(w);
        } else {
            if (shift) e->b = off; else { e->a = e->b = off; }
            e->drag = 1; e->drag_unit = 0;
            SetCapture(w);
        }
        invalidate(w, e);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->drag) {
            layout(w, e);
            HDC dc = GetDC(w); apply_font(w, dc); metrics(w, e, dc);
            int off = hit(w, e, dc, GET_X(lp), GET_Y(lp));
            ReleaseDC(w, dc);
            if (e->drag_unit == 1) {
                int s = off; while (s > 0 && is_word(e->buf[s - 1])) s--;
                int t = off; while (t < e->len && is_word(e->buf[t])) t++;
                if (off < e->drag_anchor_lo) { e->a = e->drag_anchor_hi; e->b = s; } else { e->a = e->drag_anchor_lo; e->b = t > e->drag_anchor_hi ? t : e->drag_anchor_hi; }
            } else e->b = off;
            // dragging above / below the control scrolls
            if (multi(w)) { int y = GET_Y(lp); if (y < 0 && e->sy > 0) e->sy--; else if (y > w->ch && e->sy < e->nls - 1) e->sy++; }
            scroll_to_caret(w, e);
            update_bars(w, e);
            invalidate(w, e);
        }
        return 0;
    case WM_LBUTTONUP: if (e->drag) { e->drag = 0; ReleaseCapture(); } return 0;
    case WM_RBUTTONDOWN: if (U_focus() != w) SetFocus(w); return 0;
    case WM_CONTEXTMENU: {
        POINT p; GetCursorPos(&p);
        int sx = (int)(short)LOWORD(lp), sy = (int)(short)HIWORD(lp);
        if (lp == (LPARAM)-1 || (sx == -1 && sy == -1)) { sx = p.x; sy = p.y; }
        context_menu(w, e, sx, sy);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (multi(w)) {
            int d = (short)(wp >> 16) / 120;
            layout(w, e);
            e->sy -= d * 3;
            clamp_scroll(w, e);
            update_bars(w, e);
            invalidate(w, e);
            notify(w, EN_VSCROLL);
        }
        return 0;
    case WM_VSCROLL: case WM_HSCROLL: {
        int code = (int)(wp & 0xFFFF);
        SCINFO si = { sizeof si, SIF_ALL_, 0, 0, 0, 0, 0 };
        int bar = m == WM_VSCROLL ? SBV : SBH;
        GetScrollInfo(w, bar, &si);
        int pos = bar == SBV ? e->sy : e->sx;
        int line = bar == SBV ? 1 : 8, page = (int)si.nPage > 1 ? (int)si.nPage - (bar == SBV ? 1 : 8) : 1;
        switch (code) {
        case 0: pos -= line; break;
        case 1: pos += line; break;
        case 2: pos -= page; break;
        case 3: pos += page; break;
        case 4: case 5: pos = si.nTrackPos; break;
        case 6: pos = 0; break;
        case 7: pos = 1 << 30; break;
        default: return 0;
        }
        if (bar == SBV) e->sy = pos; else e->sx = pos;
        clamp_scroll(w, e);
        update_bars(w, e);
        invalidate(w, e);
        notify(w, bar == SBV ? EN_VSCROLL : EN_HSCROLL);
        return 0;
    }
    case WM_CHAR: on_char(w, e, (int)wp); return 0;
    case WM_KEYDOWN: key_down(w, e, wp); return 0;
    case WM_CUT: if (!e->readonly) { do_copy(e); replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); } return 0;
    case WM_COPY: do_copy(e); return 0;
    case WM_PASTE: if (!e->readonly) do_paste(w, e); return 0;
    case WM_CLEAR: if (!e->readonly) replace(w, e, sel_lo(e), sel_hi(e), 0, 0, 0, 1); return 0;
    case WM_UNDO: case EM_UNDO: if (!e->has_undo) return FALSE; do_undo(w, e); return TRUE;
    case EM_CANUNDO: return e->has_undo;
    case EM_EMPTYUNDOBUFFER: undo_clear(e); return 0;
    case EM_SETSEL: {
        int a = (int)wp, b = (int)lp;
        if (a == -1) { e->a = e->b; invalidate(w, e); return 0; }                              // deselect
        if (b < 0 || b > e->len) b = e->len;
        if (a > e->len) a = e->len;
        e->a = a; e->b = b; e->typing = 0;
        invalidate(w, e);
        return 0;
    }
    case EM_GETSEL: {
        int lo = sel_lo(e), hi = sel_hi(e);
        if (wp) *(DWORD*)wp = (DWORD)lo;
        if (lp) *(DWORD*)lp = (DWORD)hi;
        return (LRESULT)(((unsigned)(hi > 0xFFFF ? 0xFFFF : hi) << 16) | (unsigned)(lo > 0xFFFF ? 0xFFFF : lo));
    }
    case EM_REPLACESEL: {
        wc *t; int n = to_wide(w, lp, &t);
        replace(w, e, sel_lo(e), sel_hi(e), t, n, 0, wp != 0);
        ufree(t);
        return 0;
    }
    case EM_SCROLLCARET: scroll_to_caret(w, e); update_bars(w, e); invalidate(w, e); return 0;
    case EM_LIMITTEXT: e->maxlen = (int)wp; return 0;
    case EM_GETLIMITTEXT: return e->maxlen ? e->maxlen : 0x7FFFFFFE;
    case EM_SETREADONLY: e->readonly = wp != 0; U_invalidate(w, 0, 1); return TRUE;
    case EM_GETMODIFY: return e->modified;
    case EM_SETMODIFY: e->modified = wp != 0; return 0;
    case EM_GETLINECOUNT: layout(w, e); return e->nls;
    case EM_LINEINDEX: {
        layout(w, e);
        int ln = (int)wp;
        if (ln == -1) ln = line_of(e, e->b);
        return ln >= 0 && ln < e->nls ? e->ls[ln] : -1;
    }
    case EM_LINEFROMCHAR: layout(w, e); { int i = (int)wp; if (i == -1) i = sel_lo(e); if (i > e->len) i = e->len; return line_of(e, i); }
    case EM_LINELENGTH: {
        layout(w, e);
        int i = (int)wp;
        if (i == -1) { int lo = sel_lo(e), hi = sel_hi(e); int l1 = line_of(e, lo), l2 = line_of(e, hi); return (lo - e->ls[l1]) + (hi <= e->le[l2] ? e->le[l2] - hi : 0); }
        if (i > e->len) return 0;
        return line_len_at(e, line_of(e, i));
    }
    case EM_GETLINE: {
        layout(w, e);
        int ln = (int)wp;
        if (ln < 0 || ln >= e->nls) return 0;
        int n = line_len_at(e, ln);
        if (w->unicode) {
            int cap = *(WORD*)lp;
            if (n > cap) n = cap;
            memcpy((void*)lp, e->buf + e->ls[ln], (SIZE_T)n * 2);
            if (n < cap) ((wc*)lp)[n] = 0;
            return n;
        }
        int cap = *(WORD*)lp;
        int r = WideCharToMultiByte(65001, 0, e->buf + e->ls[ln], n, (char*)lp, cap, 0, 0);
        if (r < 0) r = 0;
        if (r < cap) ((char*)lp)[r] = 0;
        return r;
    }
    case EM_GETFIRSTVISIBLELINE: return e->sy;
    case EM_LINESCROLL: layout(w, e); e->sx += (int)wp * 8; e->sy += (int)lp; clamp_scroll(w, e); update_bars(w, e); invalidate(w, e); return TRUE;
    case EM_SCROLL: {
        layout(w, e);
        int vis = visible_lines(w, e);
        int old = e->sy;
        if (wp == 0) e->sy--; else if (wp == 1) e->sy++; else if (wp == 2) e->sy -= vis - 1; else if (wp == 3) e->sy += vis - 1;
        clamp_scroll(w, e); update_bars(w, e); invalidate(w, e);
        return (LRESULT)(0x80000000u | ((unsigned)(e->sy - old) & 0xFFFF));
    }
    case EM_GETHANDLE: {
        int n = e->len;
        if (!e->hlocal) e->hlocal = LocalAlloc(0x40, (SIZE_T)(n + 1) * 2);
        else e->hlocal = LocalReAlloc(e->hlocal, (SIZE_T)(n + 1) * 2, 0);
        if (e->hlocal) { wc *p = (wc*)LocalLock(e->hlocal); memcpy(p, e->buf, (SIZE_T)n * 2); p[n] = 0; LocalUnlock(e->hlocal); }
        return (LRESULT)e->hlocal;
    }
    case EM_SETHANDLE: {
        HANDLE nh = (HANDLE)wp;
        if (!nh) return 0;
        if (e->hlocal && e->hlocal != nh) LocalFree(e->hlocal);
        e->hlocal = nh;
        wc *p = (wc*)LocalLock(nh);
        int n = wlen(p);
        set_text(w, e, p ? p : empty_w, n);
        LocalUnlock(nh);
        return 0;
    }
    case EM_SETPASSWORDCHAR: e->pwchar = (wc)wp; if (wp) w->style |= ES_PASSWORD; else w->style &= ~(DWORD)ES_PASSWORD; e->w8ok[0] = 0; memset(e->w8ok, 0, sizeof e->w8ok); e->lay_valid = 0; U_invalidate(w, 0, 1); return 0;
    case EM_GETPASSWORDCHAR: return e->pwchar ? e->pwchar : ((w->style & ES_PASSWORD) ? '*' : 0);
    case EM_SETMARGINS:
        if (wp & EC_LEFTMARGIN) e->lmargin = LOWORD(lp) == EC_USEFONTINFO ? 2 : LOWORD(lp);
        if (wp & EC_RIGHTMARGIN) e->rmargin = HIWORD(lp) == EC_USEFONTINFO ? 2 : HIWORD(lp);
        e->lay_valid = 0; relayout(w, e); invalidate(w, e);
        return 0;
    case EM_GETMARGINS: return (LRESULT)((unsigned)e->lmargin | ((unsigned)e->rmargin << 16));
    case EM_SETTABSTOPS: if (wp >= 1 && lp) { e->tabw = (*(int*)lp) * 2; e->lay_valid = 0; } return TRUE;
    case EM_GETRECT: { RECT *r = (RECT*)lp; if (r) { r->left = e->lmargin; r->top = 0; r->right = w->cw - e->rmargin; r->bottom = w->ch; } return 0; }
    case EM_SETRECT: case EM_SETRECTNP: return 0;
    case EM_FMTLINES: return FALSE;
    case EM_SETWORDBREAKPROC: return 0;
    case EM_GETWORDBREAKPROC: return 0;
    case EM_POSFROMCHAR: {
        layout(w, e);
        HDC dc = GetDC(w); apply_font(w, dc); metrics(w, e, dc);
        int i = (int)wp; if (i > e->len) i = e->len;
        int ln = line_of(e, i);
        int x = e->lmargin - e->sx + xof(w, e, dc, ln, i), y = 1 + (ln - e->sy) * e->lh;
        ReleaseDC(w, dc);
        return (LRESULT)(((unsigned)(y & 0xFFFF) << 16) | ((unsigned)x & 0xFFFF));
    }
    case EM_CHARFROMPOS: {
        layout(w, e);
        HDC dc = GetDC(w); apply_font(w, dc); metrics(w, e, dc);
        int i = hit(w, e, dc, GET_X(lp), GET_Y(lp));
        ReleaseDC(w, dc);
        return (LRESULT)(((unsigned)line_of(e, i) << 16) | ((unsigned)i & 0xFFFF));
    }
    case EM_SETIMESTATUS: return 0;
    default: return U_def_proc(w, m, wp, lp, w->unicode);
    }
}
