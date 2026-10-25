// user32: scroll bars. One engine (geometry, painting, mouse handling) serves the SCROLLBAR control and the scroll bars of a
// window (WS_VSCROLL / WS_HSCROLL, set with SetScrollInfo & co.), which live in a 17 px margin right of / below the client area.
#include "u32.h"

extern UINT_PTR SetTimerU(void);
extern ULONG_PTR SetTimer(HWND h, ULONG_PTR id, UINT ms, void *proc);
extern BOOL KillTimer(HWND h, ULONG_PTR id);

#define SBT 17                       // thickness
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL 2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINEDOWN 1
#define SB_PAGEUP 2
#define SB_PAGEDOWN 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_BOTTOM 7
#define SB_ENDSCROLL 8
#define SIF_RANGE 1
#define SIF_PAGE 2
#define SIF_POS 4
#define SIF_DISABLENOSCROLL 8
#define SIF_TRACKPOS 0x10
#define SIF_ALL 0x17
#define SBM_SETPOS 0xE0
#define SBM_GETPOS 0xE1
#define SBM_SETRANGE 0xE2
#define SBM_GETRANGE 0xE3
#define SBM_ENABLE_ARROWS 0xE4
#define SBM_SETRANGEREDRAW 0xE6
#define SBM_SETSCROLLINFO 0xE9
#define SBM_GETSCROLLINFO 0xEA
#define SBS_VERT 1
#define SBTIMER 0x7F01

typedef struct { UINT cbSize, fMask; int nMin, nMax; UINT nPage; int nPos, nTrackPos; } SCROLLINFO;

int U_sbv(struct wnd *w) { return (w->style & WS_VSCROLL) ? SBT : 0; }
int U_sbh(struct wnd *w) { return (w->style & WS_HSCROLL) ? SBT : 0; }

// geometry of one bar of length len: parts 1 (arrow before), 2 (track before the thumb), 3 (thumb), 4 (track after), 5 (arrow after)
struct sbgeo { int a1, t0, th0, th1, t1, a2; };
static void geo(const struct sbi *s, int len, struct sbgeo *g) {
    int arrow = len >= 2 * SBT ? SBT : len / 2;
    g->a1 = arrow; g->t0 = arrow; g->t1 = len - arrow; g->a2 = arrow;
    int track = g->t1 - g->t0;
    int range = s->max - s->min + 1;
    int page = s->page ? (int)s->page : 0;
    int thumb = (page > 0 && range > 0) ? track * page / range : SBT;
    if (thumb < 10) thumb = 10;
    if (thumb > track) thumb = track;
    int movable = range - page;
    if (page == 0) movable = range - 1;
    int off = movable > 0 ? (track - thumb) * (s->pos - s->min) / movable : 0;
    if (off < 0) off = 0;
    if (off > track - thumb) off = track - thumb;
    g->th0 = g->t0 + off; g->th1 = g->th0 + thumb;
}
static int value_of(const struct sbi *s, int len, int thumb_off) {                // thumb offset in the track -> position
    struct sbgeo g; geo(s, len, &g);
    int track = g.t1 - g.t0, thumb = g.th1 - g.th0;
    int page = (int)s->page, movable = (s->max - s->min + 1) - page;
    if (page == 0) movable = s->max - s->min;
    if (track - thumb <= 0) return s->min;
    int v = s->min + thumb_off * movable / (track - thumb);
    int hi = s->max - (page ? page - 1 : 0);
    if (v > hi) v = hi;
    if (v < s->min) v = s->min;
    return v;
}
static int part_at(const struct sbi *s, int len, int p) {
    struct sbgeo g; geo(s, len, &g);
    if (p < g.a1) return 1;
    if (p >= len - g.a2) return 5;
    if (p < g.th0) return 2;
    if (p < g.th1) return 3;
    return 4;
}

static void paint_bar(HDC dc, int x, int y, int w, int h, int vert, const struct sbi *s, int enabled, int hot) {
    int len = vert ? h : w;
    struct sbgeo g; geo(s, len, &g);
    __gdi_rect_fill_color(dc, x, y, x + w, y + h, RGB(240, 240, 240));
    COLORREF arrow = enabled ? RGB(96, 96, 96) : RGB(190, 190, 190);
    for (int k = 0; k < 2; k++) {                                           // the arrows: a small triangle on a flat button
        int a0 = k == 0 ? 0 : len - g.a2, a1 = k == 0 ? g.a1 : len;
        int cx = vert ? x + w / 2 : x + (a0 + a1) / 2, cy = vert ? y + (a0 + a1) / 2 : y + h / 2;
        if (hot == (k == 0 ? 1 : 5) && enabled) __gdi_rect_fill_color(dc, vert ? x : x + a0, vert ? y + a0 : y, vert ? x + w : x + a1, vert ? y + a1 : y + h, RGB(218, 218, 218));
        for (int i = 0; i < 4; i++) {
            int n = i;                                                      // width of this row of the triangle
            if (vert) { int yy = k == 0 ? cy - 2 + i : cy + 1 - i; __gdi_rect_fill_color(dc, cx - n, yy, cx + n + 1, yy + 1, arrow); }
            else { int xx = k == 0 ? cx - 2 + i : cx + 1 - i; __gdi_rect_fill_color(dc, xx, cy - n, xx + 1, cy + n + 1, arrow); }
        }
    }
    if (enabled && g.th1 > g.th0 && (s->max - s->min) > 0) {
        COLORREF tc = hot == 3 ? RGB(120, 120, 120) : hot == -3 ? RGB(96, 96, 96) : RGB(205, 205, 205);
        if (vert) __gdi_rect_fill_color(dc, x + 2, y + g.th0, x + w - 2, y + g.th1, tc);
        else __gdi_rect_fill_color(dc, x + g.th0, y + 2, x + g.th1, y + h - 2, tc);
    }
}

// ---- interaction -----------------------------------------------------------------------------------------------------------------

static struct {
    struct wnd *w; int bar;              // the window and SB_HORZ / SB_VERT, or SB_CTL for a SCROLLBAR control
    int part, dragging, grab, start_pos;
    int len;                             // length of the bar along its axis
    int active;
} A;

static struct sbi *info_of(struct wnd *w, int bar) { return &w->sb[bar == SB_VERT ? 1 : 0]; }

static void send_scroll(struct wnd *w, int bar, int code, int pos) {
    if (bar == SB_CTL) {
        struct wnd *p = w->parent;
        if (p) U_send(p, (w->style & SBS_VERT) ? WM_VSCROLL : WM_HSCROLL, (WPARAM)((code & 0xFFFF) | ((pos & 0xFFFF) << 16)), (LPARAM)w);
    } else U_send(w, bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, (WPARAM)((code & 0xFFFF) | ((pos & 0xFFFF) << 16)), 0);
}
static void redraw(struct wnd *w, int bar) { if (bar == SB_CTL) U_invalidate(w, 0, 0); else U_nc_paint(w); }

static void step(int code) {
    struct sbi *s = info_of(A.w, A.bar);
    send_scroll(A.w, A.bar, code, s->pos);
}

void U_sb_timer(struct wnd *w) {                                                    // WM_TIMER SBTIMER
    if (!A.active || A.w != w || A.dragging) return;
    if (A.part == 1) step(SB_LINEUP); else if (A.part == 5) step(SB_LINEDOWN);
    else if (A.part == 2) step(SB_PAGEUP); else if (A.part == 4) step(SB_PAGEDOWN);
}

// a mouse message at (p along the bar, q across it); returns 1 when it was a scroll bar message
static int sb_mouse(struct wnd *w, int bar, int p, int q, int len, int type, int down) {
    struct sbi *s = info_of(w, bar);
    (void)q;
    if (A.active && A.w == w && A.bar == bar && A.dragging) {                          // dragging the thumb
        if (type == OKE_MOUSE_BUTTON && !down) {
            send_scroll(w, bar, SB_THUMBPOSITION, s->pos);
            send_scroll(w, bar, SB_ENDSCROLL, s->pos);
            A.active = 0; A.dragging = 0; U_set_capture(0); redraw(w, bar); return 1;
        }
        struct sbgeo g; geo(s, len, &g);
        int off = p - A.grab - g.t0;
        int v = value_of(s, len, off);
        if (v != s->pos) { s->pos = v; send_scroll(w, bar, SB_THUMBTRACK, v); redraw(w, bar); }
        return 1;
    }
    if (type == OKE_MOUSE_BUTTON && down) {
        int part = part_at(s, len, p);
        A.w = w; A.bar = bar; A.part = part; A.len = len; A.active = 1; A.dragging = 0;
        if (part == 3) {
            struct sbgeo g; geo(s, len, &g);
            A.dragging = 1; A.grab = p - g.th0; A.start_pos = s->pos;
            U_set_capture(w);
        } else {
            step(part == 1 ? SB_LINEUP : part == 5 ? SB_LINEDOWN : part == 2 ? SB_PAGEUP : SB_PAGEDOWN);
            SetTimer(w, SBTIMER, 90, 0);
        }
        redraw(w, bar);
        return 1;
    }
    if (type == OKE_MOUSE_BUTTON && !down && A.active && A.w == w) {
        KillTimer(w, SBTIMER);
        send_scroll(w, bar, SB_ENDSCROLL, s->pos);
        A.active = 0; redraw(w, bar);
        return 1;
    }
    return 0;
}

// ---- the margin scroll bars of a window ------------------------------------------------------------------------------------------------

static HDC nc_dc(struct wnd *t) {
    struct wnd *top = U_top(t);
    if (!top || !top->px) return 0;
    HDC dc = __gdi_make_dc(top->px, top->bw, top->bh, top->stride);
    if (!dc) return 0;
    int ox, oy; U_origin(t, &ox, &oy);
    RECT clip;
    if (!t->parent) { clip.left = 0; clip.top = t->mh; clip.right = top->bw; clip.bottom = top->bh; }
    else {
        RECT pc; U_clip(t->parent, &pc);
        int px, py; U_origin(t->parent, &px, &py);
        RECT o = { px + t->x, py + t->y, px + t->x + t->w, py + t->y + t->h };
        clip.left = o.left > pc.left ? o.left : pc.left; clip.top = o.top > pc.top ? o.top : pc.top;
        clip.right = o.right < pc.right ? o.right : pc.right; clip.bottom = o.bottom < pc.bottom ? o.bottom : pc.bottom;
    }
    __gdi_dc_setup(dc, top->px, top->bw, top->bh, top->stride, ox, oy, &clip);
    return dc;
}

void U_nc_paint(struct wnd *w) {
    int sv = U_sbv(w), sh = U_sbh(w);
    if ((!sv && !sh) || !w->visible) return;
    HDC dc = nc_dc(w);
    if (!dc) return;
    if (sv) paint_bar(dc, w->cw, 0, SBT, w->ch, 1, &w->sb[1], 1, A.active && A.w == w && A.bar == SB_VERT ? (A.dragging ? -3 : A.part) : 0);
    if (sh) paint_bar(dc, 0, w->ch, w->cw, SBT, 0, &w->sb[0], 1, A.active && A.w == w && A.bar == SB_HORZ ? (A.dragging ? -3 : A.part) : 0);
    if (sv && sh) __gdi_rect_fill_color(dc, w->cw, w->ch, w->cw + SBT, w->ch + SBT, RGB(240, 240, 240));
    __gdi_free_dc(dc);
    RECT r = { 0, 0, w->cw + sv, w->ch + sh };
    struct wnd *top = U_top(w);
    int ox, oy; U_origin(w, &ox, &oy);
    struct okw_present pr = { top->wid, ox, oy, r.right, r.bottom };
    SC3(LX_ioctl, U_fd(), OKW_PRESENT, &pr);
}

// a mouse event of a window with scroll bars; (lx, ly) in its client coordinates. 1 = consumed
int U_nc_event(struct wnd *w, int lx, int ly, int type, int down) {
    int sv = U_sbv(w), sh = U_sbh(w);
    if (A.active && A.w == w && A.bar != SB_CTL) {
        int vert = A.bar == SB_VERT;
        return sb_mouse(w, A.bar, vert ? ly : lx, vert ? lx - w->cw : ly - w->ch, vert ? w->ch : w->cw, type, down);
    }
    if (type != OKE_MOUSE_BUTTON) return 0;
    if (sv && lx >= w->cw && lx < w->cw + SBT && ly >= 0 && ly < w->ch) return sb_mouse(w, SB_VERT, ly, lx - w->cw, w->ch, type, down);
    if (sh && ly >= w->ch && ly < w->ch + SBT && lx >= 0 && lx < w->cw) return sb_mouse(w, SB_HORZ, lx, ly - w->ch, w->cw, type, down);
    return (sv && lx >= w->cw && lx < w->cw + SBT) || (sh && ly >= w->ch && ly < w->ch + SBT);        // the corner and the like: swallowed
}
int U_nc_active(struct wnd *w) { return A.active && A.w == w && A.bar != SB_CTL; }

// ---- the window API ----------------------------------------------------------------------------------------------------------------------

static struct sbi *bar_info(struct wnd *w, int bar) { return bar == SB_VERT ? &w->sb[1] : &w->sb[0]; }
void U_recalc_client(struct wnd *w) {
    int ocw = w->cw, och = w->ch;
    if (!w->parent) { w->cw = w->bw - U_sbv(w); w->ch = w->bh - w->mh - U_sbh(w); }
    else { w->cw = w->w - 2 * w->bd - U_sbv(w); w->ch = w->h - 2 * w->bd - U_sbh(w); }
    if (w->cw < 0) w->cw = 0;
    if (w->ch < 0) w->ch = 0;
    if (w->cw != ocw || w->ch != och) U_send(w, WM_SIZE, 0, MAKELPARAM(w->cw, w->ch));
    U_invalidate(w, 0, 1);
}
EXPORT BOOL ShowScrollBar(HWND h, int bar, BOOL show) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    DWORD add = (bar == SB_VERT ? WS_VSCROLL : bar == SB_HORZ ? WS_HSCROLL : WS_VSCROLL | WS_HSCROLL);
    DWORD old = w->style;
    if (show) w->style |= add; else w->style &= ~add;
    if (old != w->style) U_recalc_client(w);
    return TRUE;
}
EXPORT BOOL EnableScrollBar(HWND h, UINT bar, UINT a) { (void)h; (void)bar; (void)a; return TRUE; }
static void apply_info(struct sbi *s, const SCROLLINFO *i) {
    if (i->fMask & SIF_RANGE) { s->min = i->nMin; s->max = i->nMax; }
    if (i->fMask & SIF_PAGE) s->page = (int)i->nPage;
    if (i->fMask & SIF_POS) s->pos = i->nPos;
    int hi = s->max - (s->page ? s->page - 1 : 0);
    if (s->pos > hi) s->pos = hi;
    if (s->pos < s->min) s->pos = s->min;
}
EXPORT int SetScrollInfo(HWND h, int bar, const SCROLLINFO *i, BOOL redraw) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    if (bar == SB_CTL) return (int)U_send(w, SBM_SETSCROLLINFO, redraw, (LPARAM)i);
    struct sbi *s = bar_info(w, bar);
    apply_info(s, i);
    if (redraw) U_nc_paint(w);
    return s->pos;
}
EXPORT BOOL GetScrollInfo(HWND h, int bar, SCROLLINFO *i) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    if (bar == SB_CTL) return (BOOL)U_send(w, SBM_GETSCROLLINFO, 0, (LPARAM)i);
    struct sbi *s = bar_info(w, bar);
    if (i->fMask & SIF_RANGE) { i->nMin = s->min; i->nMax = s->max; }
    if (i->fMask & SIF_PAGE) i->nPage = (UINT)s->page;
    if (i->fMask & SIF_POS) i->nPos = s->pos;
    if (i->fMask & SIF_TRACKPOS) i->nTrackPos = s->pos;
    return TRUE;
}
EXPORT int SetScrollPos(HWND h, int bar, int p, BOOL redraw) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    if (bar == SB_CTL) return (int)U_send(w, SBM_SETPOS, (WPARAM)p, redraw);
    struct sbi *s = bar_info(w, bar);
    int old = s->pos;
    SCROLLINFO i = { sizeof i, SIF_POS, 0, 0, 0, p, 0 };
    apply_info(s, &i);
    if (redraw) U_nc_paint(w);
    return old;
}
EXPORT int GetScrollPos(HWND h, int bar) { struct wnd *w = U_wnd(h); if (!w) return 0; if (bar == SB_CTL) return (int)U_send(w, SBM_GETPOS, 0, 0); return bar_info(w, bar)->pos; }
EXPORT BOOL SetScrollRange(HWND h, int bar, int lo, int hi, BOOL redraw) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    if (bar == SB_CTL) return (BOOL)U_send(w, SBM_SETRANGE, (WPARAM)lo, (LPARAM)hi);
    SCROLLINFO i = { sizeof i, SIF_RANGE, lo, hi, 0, 0, 0 };
    apply_info(bar_info(w, bar), &i);
    if (redraw) U_nc_paint(w);
    return TRUE;
}
EXPORT BOOL GetScrollRange(HWND h, int bar, int *lo, int *hi) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    if (bar == SB_CTL) { U_send(w, SBM_GETRANGE, (WPARAM)lo, (LPARAM)hi); return TRUE; }
    struct sbi *s = bar_info(w, bar);
    *lo = s->min; *hi = s->max;
    return TRUE;
}

// ---- the SCROLLBAR control --------------------------------------------------------------------------------------------------------------------

struct sbctl { struct sbi s; int enabled; };

LRESULT U_scrollbar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    struct wnd *w = h;
    struct sbctl *c = w->ctl;
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CREATE: { c = umalloc(sizeof *c); w->ctl = c; if (c) { c->s.min = 0; c->s.max = 100; c->enabled = 1; } return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (!dc) return 0;
        int vert = (w->style & SBS_VERT) != 0;
        paint_bar(dc, 0, 0, w->cw, w->ch, vert, &c->s, w->enabled, A.active && A.w == w ? (A.dragging ? -3 : A.part) : 0);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (!w->enabled) return 0;
        int vert = (w->style & SBS_VERT) != 0, len = vert ? w->ch : w->cw;
        SetCapture(w);
        sb_mouse(w, SB_CTL, vert ? GET_Y(lp) : GET_X(lp), 0, len, OKE_MOUSE_BUTTON, 1);
        w->font = w->font;                                                       // (the control keeps no font)
        return 0;
    }
    case WM_MOUSEMOVE: {
        int vert = (w->style & SBS_VERT) != 0, len = vert ? w->ch : w->cw;
        if (A.active && A.w == w && A.dragging) sb_mouse(w, SB_CTL, vert ? GET_Y(lp) : GET_X(lp), 0, len, OKE_MOUSE_MOVE, 0);
        return 0;
    }
    case WM_LBUTTONUP: {
        int vert = (w->style & SBS_VERT) != 0, len = vert ? w->ch : w->cw;
        if (A.active && A.w == w) sb_mouse(w, SB_CTL, vert ? GET_Y(lp) : GET_X(lp), 0, len, OKE_MOUSE_BUTTON, 0);
        ReleaseCapture();
        return 0;
    }
    case WM_TIMER: if (wp == SBTIMER) U_sb_timer(w); return 0;
    case SBM_SETPOS: { int old = c->s.pos; SCROLLINFO i = { sizeof i, SIF_POS, 0, 0, 0, (int)wp, 0 }; apply_info(&c->s, &i); if (lp) U_invalidate(w, 0, 0); return old; }
    case SBM_GETPOS: return c->s.pos;
    case SBM_SETRANGE: case SBM_SETRANGEREDRAW: { SCROLLINFO i = { sizeof i, SIF_RANGE, (int)wp, (int)lp, 0, 0, 0 }; apply_info(&c->s, &i); U_invalidate(w, 0, 0); return 0; }
    case SBM_GETRANGE: if (wp) *(int*)wp = c->s.min; if (lp) *(int*)lp = c->s.max; return 0;
    case SBM_SETSCROLLINFO: apply_info(&c->s, (const SCROLLINFO*)lp); if (wp) U_invalidate(w, 0, 0); return c->s.pos;
    case SBM_GETSCROLLINFO: {
        SCROLLINFO *i = (SCROLLINFO*)lp;
        if (i->fMask & SIF_RANGE) { i->nMin = c->s.min; i->nMax = c->s.max; }
        if (i->fMask & SIF_PAGE) i->nPage = (UINT)c->s.page;
        if (i->fMask & (SIF_POS | SIF_TRACKPOS)) { i->nPos = c->s.pos; i->nTrackPos = c->s.pos; }
        return TRUE;
    }
    case WM_ENABLE: U_invalidate(w, 0, 0); return 0;
    default: return U_def_proc(w, m, wp, lp, w->unicode);
    }
}
