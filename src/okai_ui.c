// okai browser chrome — a Firefox "Proton"-style tab strip + nav bar.
//
// Everything is drawn with the web engine's anti-aliased primitives: the
// path rasterizer (raster.c) for rounded boxes and stroked icons, and the
// TrueType renderer (font.c, Noto Sans) for text. The band above the page
// (CHROME_PX tall) is rendered into a per-window pixel cache and only
// re-rendered when its inputs change (state signature: tabs, URL, focus,
// hover, spinner phase); each overlay paint then just blits the cache. The
// security panel and the status pill sit over the page and are drawn
// straight into the backbuffer.
//
// ui_layout() is the ONE source of chrome geometry: drawing and every hit
// test (tabs, nav buttons, address bar, identity box) use it, and it is
// logged as `[okai] ui ...` lines so headless tests aim from the log
// instead of hard-coded pixel positions. The caption buttons (min/max/close)
// come from window_ctrl_rect() — the window system owns their hit testing.
#include "okai.h"
#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "web/surface.h"
#include "web/raster.h"
#include "web/font.h"
#include <stdint.h>

extern uint32_t tick_count;

// okai.c helpers
int  okai_tab_busy(int id, int tab);
long okai_fetch_bytes(int id, int* subs);

#define CHROME_PX (CHROME_TAB_H + CHROME_TOOL_H)

// ---- geometry -----------------------------------------------------------

#define TAB_MAX_W   240
#define TAB_MIN_W   76
#define TAB_GAP     4
#define TAB_PAD_Y   4     // strip margin above/below a tab
#define NEWTAB_SZ   32
#define NAVBTN      36
#define NAVGAP      2
#define URL_H       40
#define IDENT_SZ    32
#define TABX_SZ     24

enum { HV_NONE = 0, HV_TAB = 10, HV_TABX = 20, HV_NEW = 30, HV_CTRL = 40,
       HV_NAV = 50, HV_URL = 60, HV_IDENT = 61 };

struct ui_lay {
    int x0, y0, w;                 // chrome band (screen)
    int tool_y;
    int n;                         // tabs laid out
    int tx[OKAI_MAX_TABS], tw[OKAI_MAX_TABS]; // current (animated) tab x / width
    int tab_y, tab_h;
    int tab_target;                // settled tab width
    int newtab[4];
    int ctrl[3][4], has_ctrl[3];
    int nav[4][4];                 // back, forward, reload, home
    int url[4];
    int ident[4];
};

static int in_rect(const int r[4], int x, int y) {
    return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
}
static void set_rect(int r[4], int x, int y, int w, int h) { r[0] = x; r[1] = y; r[2] = w; r[3] = h; }

// Settled width of each tab for this window width (the open/close animation
// eases anim_w toward it).
int okai_ui_tab_width(struct okai* b) {
    struct window* w = window_get(b->win_id);
    if (!w) return TAB_MIN_W;
    int cw = w->w - 2 * WIN_BORDER;
    int right = cw - (w->no_titlebar ? 3 * WIN_CTRL_W : 0) - 48; // keep a drag gap
    int avail = right - 8 - NEWTAB_SZ - 8;
    int n = b->tab_count < 1 ? 1 : b->tab_count;
    int tw = avail / n - TAB_GAP;
    if (tw > TAB_MAX_W) tw = TAB_MAX_W;
    if (tw < TAB_MIN_W) tw = TAB_MIN_W;
    return tw;
}

static int ui_layout(struct okai* b, struct ui_lay* L) {
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible) return 0;
    L->x0 = w->x + WIN_BORDER;
    L->y0 = w->y + WIN_BORDER + (w->no_titlebar ? 0 : WIN_TITLE_H);
    L->w = w->w - 2 * WIN_BORDER;
    if (L->w <= 0) return 0;
    L->tool_y = L->y0 + CHROME_TAB_H;
    L->tab_y = L->y0 + TAB_PAD_Y;
    L->tab_h = CHROME_TAB_H - 2 * TAB_PAD_Y;
    L->tab_target = okai_ui_tab_width(b);

    int x = L->x0 + 8;
    L->n = b->tab_count;
    for (int i = 0; i < b->tab_count; i++) {
        int tw = b->tabs[i].anim_w;
        L->tx[i] = x;
        L->tw[i] = tw;
        if (tw >= 2) x += tw + TAB_GAP;
    }
    set_rect(L->newtab, x, L->y0 + (CHROME_TAB_H - NEWTAB_SZ) / 2, NEWTAB_SZ, NEWTAB_SZ);
    for (int k = 0; k < 3; k++) L->has_ctrl[k] = window_ctrl_rect(b->win_id, k, L->ctrl[k]);

    int by = L->tool_y + (CHROME_TOOL_H - NAVBTN) / 2;
    int bx = L->x0 + 8;
    for (int i = 0; i < 4; i++) {
        set_rect(L->nav[i], bx, by, NAVBTN, NAVBTN);
        bx += NAVBTN + NAVGAP;
    }
    int ux = bx + 10;
    int uw = L->x0 + L->w - 10 - ux;
    if (uw < 40) uw = 40;
    set_rect(L->url, ux, L->tool_y + (CHROME_TOOL_H - URL_H) / 2, uw, URL_H);
    set_rect(L->ident, ux + 4, L->url[1] + (URL_H - IDENT_SZ) / 2, IDENT_SZ, IDENT_SZ);
    return 1;
}

static void tab_close_rect(const struct ui_lay* L, int i, int r[4]) {
    set_rect(r, L->tx[i] + L->tw[i] - 6 - TABX_SZ, L->tab_y + (L->tab_h - TABX_SZ) / 2,
             TABX_SZ, TABX_SZ);
}

// ---- hit tests (desktop.c routes clicks here) -----------------------------

int okai_tab_hit(int id, int mx, int my, int* on_close) {
    if (on_close) *on_close = 0;
    struct okai* b = okai_get(id);
    struct ui_lay L;
    if (!b || !ui_layout(b, &L)) return -1;
    for (int i = 0; i < L.n; i++) {
        if (L.tw[i] < 2 || b->tabs[i].closing) continue;
        int r[4];
        set_rect(r, L.tx[i], L.tab_y, L.tw[i], L.tab_h);
        if (!in_rect(r, mx, my)) continue;
        int xr[4];
        tab_close_rect(&L, i, xr);
        if (on_close && L.tw[i] >= TAB_MIN_W && in_rect(xr, mx, my)) *on_close = 1;
        return i;
    }
    return -1;
}

int okai_check_nav_click(int id, int mx, int my) {
    struct okai* b = okai_get(id);
    struct ui_lay L;
    if (!b || !ui_layout(b, &L)) return NAV_NONE;
    static const int acts[4] = { NAV_BACK, NAV_FWD, NAV_RELOAD, NAV_HOME };
    for (int i = 0; i < 4; i++)
        if (in_rect(L.nav[i], mx, my)) return acts[i];
    if (b->tab_count < OKAI_MAX_TABS && in_rect(L.newtab, mx, my)) return NAV_NEWTAB;
    return NAV_NONE;
}

int okai_lock_hit(int id, int mx, int my) {
    struct okai* b = okai_get(id);
    struct ui_lay L;
    if (!b || !ui_layout(b, &L)) return 0;
    struct window* w = window_get(b->win_id);
    if (!w || w->minimized) return 0;
    return in_rect(L.ident, mx, my);
}

int okai_addr_bar_hit(int id, int mx, int my) {
    struct okai* b = okai_get(id);
    struct ui_lay L;
    if (!b || !ui_layout(b, &L)) return 0;
    return in_rect(L.url, mx, my) && !in_rect(L.ident, mx, my);
}

static int hover_of(struct okai* b, const struct ui_lay* L, int mx, int my) {
    struct window* w = window_get(b->win_id);
    if (!w || window_from_point(mx, my) != b->win_id) return HV_NONE;
    if (my < L->y0 || my >= L->y0 + CHROME_PX) return HV_NONE;
    for (int k = 0; k < 3; k++)
        if (L->has_ctrl[k] && in_rect(L->ctrl[k], mx, my)) return HV_CTRL + k;
    for (int i = 0; i < L->n; i++) {
        if (L->tw[i] < 2) continue;
        int r[4];
        set_rect(r, L->tx[i], L->tab_y, L->tw[i], L->tab_h);
        if (!in_rect(r, mx, my)) continue;
        int xr[4];
        tab_close_rect(L, i, xr);
        return in_rect(xr, mx, my) ? HV_TABX + i : HV_TAB + i;
    }
    if (in_rect(L->newtab, mx, my)) return HV_NEW;
    for (int i = 0; i < 4; i++) if (in_rect(L->nav[i], mx, my)) return HV_NAV + i;
    if (in_rect(L->ident, mx, my)) return HV_IDENT;
    if (in_rect(L->url, mx, my)) return HV_URL;
    return HV_NONE;
}

// ---- drawing primitives ---------------------------------------------------

static struct wraster ras;
static int ras_ready;

struct span_ctx { struct wsurf* s; uint32_t c; };

static void span_cb(void* p, int y, int x, int len, const uint8_t* cov) {
    struct span_ctx* sc = (struct span_ctx*)p;
    struct wsurf* s = sc->s;
    unsigned ca = sc->c >> 24;
    uint32_t* row = s->px + y * s->stride;
    for (int i = 0; i < len; i++) {
        unsigned a = cov[i];
        if (!a) continue;
        if (ca < 255) a = (a * ca + 127) / 255;
        row[x + i] = ws_blend(row[x + i], sc->c, a);
    }
}

static void path_begin(void) {
    if (!ras_ready) { wr_init(&ras); ras_ready = 1; }
    wr_reset(&ras);
}

// Fill the current path; c is 0x00RRGGBB (opaque) or AARRGGBB with A < 0xFF.
static void path_fill(struct wsurf* s, uint32_t c) {
    if ((c >> 24) == 0) c |= 0xFF000000u;
    struct span_ctx sc = { s, c };
    wr_fill(&ras, s->cx0, s->cy0, s->cx1, s->cy1, 0, span_cb, &sc);
}

static void rrect(struct wsurf* s, int x, int y, int w, int h, int r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    path_begin();
    int32_t rad[4] = { WR_FIX(r), WR_FIX(r), WR_FIX(r), WR_FIX(r) };
    wr_rrect(&ras, WR_FIX(x), WR_FIX(y), WR_FIX(w), WR_FIX(h), rad);
    path_fill(s, c);
}

// Soft drop shadow: a few expanding translucent rounded rects.
static void shadow(struct wsurf* s, int x, int y, int w, int h, int r, int spread, unsigned alpha) {
    for (int k = spread; k >= 1; k--) {
        unsigned a = alpha * (unsigned)(spread - k + 1) / (unsigned)(spread * 2);
        if (!a) continue;
        rrect(s, x - k, y - k + 1, w + 2 * k, h + 2 * k, r + k, (a << 24) | 0x000000);
    }
}

// cos(2*pi*k/64) * 1024
static const int16_t COS64[64] = {
    1024,1019,1004,980,946,903,851,792,724,650,569,483,392,297,200,100,0,-100,-200,-297,
    -392,-483,-569,-650,-724,-792,-851,-903,-946,-980,-1004,-1019,-1024,-1019,-1004,-980,
    -946,-903,-851,-792,-724,-650,-569,-483,-392,-297,-200,-100,0,100,200,297,392,483,569,
    650,724,792,851,903,946,980,1004,1019 };
static int cos64(int k) { return COS64[k & 63]; }
static int sin64(int k) { return COS64[(k - 16) & 63]; }

// Stroke pen: icon coordinates are in 1/256 px (WR units) relative to an
// origin; every segment is a quad + round joins, all emitted with the same
// winding so the nonzero fill unions them.
struct ui_pen { int32_t ox, oy, hw; };

static void poly_emit(const int32_t* xy, int n) {
    int64_t area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (int64_t)xy[2 * i] * xy[2 * j + 1] - (int64_t)xy[2 * j] * xy[2 * i + 1];
    }
    if (area < 0) {
        wr_move(&ras, xy[0], xy[1]);
        for (int i = n - 1; i >= 1; i--) wr_line(&ras, xy[2 * i], xy[2 * i + 1]);
    } else {
        wr_move(&ras, xy[0], xy[1]);
        for (int i = 1; i < n; i++) wr_line(&ras, xy[2 * i], xy[2 * i + 1]);
    }
    wr_close(&ras);
}

static void dot(const struct ui_pen* p, int32_t x, int32_t y, int32_t r) {
    int32_t xy[32];
    for (int k = 0; k < 16; k++) {
        xy[2 * k] = p->ox + x + r * cos64(k * 4) / 1024;
        xy[2 * k + 1] = p->oy + y + r * sin64(k * 4) / 1024;
    }
    poly_emit(xy, 16);
}

static int32_t isqrt32(int32_t v) {
    if (v <= 0) return 0;
    int32_t r = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (int32_t)r;
}

static void seg(const struct ui_pen* p, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int caps) {
    // icon segments are < 64px: every product below fits in 32 bits
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t len = isqrt32(dx * dx + dy * dy);
    if (len > 0) {
        int32_t nx = -dy * p->hw / len;
        int32_t ny = dx * p->hw / len;
        int32_t xy[8] = {
            p->ox + x0 + nx, p->oy + y0 + ny, p->ox + x1 + nx, p->oy + y1 + ny,
            p->ox + x1 - nx, p->oy + y1 - ny, p->ox + x0 - nx, p->oy + y0 - ny };
        poly_emit(xy, 4);
    }
    if (caps) { dot(p, x0, y0, p->hw); dot(p, x1, y1, p->hw); }
}

// Polyline through n points (round joins + caps).
static void polyline(const struct ui_pen* p, const int32_t* pts, int n) {
    for (int i = 0; i + 1 < n; i++) seg(p, pts[2 * i], pts[2 * i + 1], pts[2 * i + 2], pts[2 * i + 3], 0);
    for (int i = 0; i < n; i++) dot(p, pts[2 * i], pts[2 * i + 1], p->hw);
}

// Arc (ellipse rx, ry) from angle index a0 sweeping `sweep` 1/64 turns
// clockwise on screen (y down).
static void arc(const struct ui_pen* p, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int a0, int sweep) {
    int32_t pts[2 * 66];
    int n = 0;
    for (int k = 0; k <= sweep && n < 66; k++, n++) {
        pts[2 * n] = cx + rx * cos64(a0 + k) / 1024;
        pts[2 * n + 1] = cy + ry * sin64(a0 + k) / 1024;
    }
    polyline(p, pts, n);
}

#define U(v) ((int32_t)((v) * 256))   // icon units (px, may be fractional) -> WR units

static void pen_at(struct ui_pen* p, int x, int y, int hw256) {
    p->ox = WR_FIX(x); p->oy = WR_FIX(y); p->hw = hw256;
}

// ---- icons (20px box unless noted; stroke ~1.7px) -----------------------

#define STROKE 218   // half-width 0.85px

static void icon_back(struct wsurf* s, int x, int y, uint32_t c, int dir) {
    struct ui_pen p; pen_at(&p, x, y, STROKE);
    path_begin();
    if (dir < 0) {
        int32_t head[6] = { U(9.5), U(4.5), U(4), U(10), U(9.5), U(15.5) };
        polyline(&p, head, 3);
        seg(&p, U(4.5), U(10), U(16), U(10), 1);
    } else {
        int32_t head[6] = { U(10.5), U(4.5), U(16), U(10), U(10.5), U(15.5) };
        polyline(&p, head, 3);
        seg(&p, U(4), U(10), U(15.5), U(10), 1);
    }
    path_fill(s, c);
}

static void icon_reload(struct wsurf* s, int x, int y, uint32_t c) {
    struct ui_pen p; pen_at(&p, x, y, STROKE);
    path_begin();
    // ↻: ring from -20deg clockwise round to -80deg (gap at the top right),
    // arrowhead at the end pointing along the clockwise tangent
    arc(&p, U(10), U(10), U(6), U(6), 60, 52);
    int e = 60 + 52;
    int32_t ex = U(10) + U(6) * cos64(e) / 1024, ey = U(10) + U(6) * sin64(e) / 1024;
    int32_t tx = -sin64(e), ty = cos64(e);          // tangent (x1024)
    int32_t nx = cos64(e), ny = sin64(e);           // outward normal (x1024)
    int32_t tri[6];
    tri[0] = p.ox + ex + tx * U(3.4) / 1024;        tri[1] = p.oy + ey + ty * U(3.4) / 1024;
    tri[2] = p.ox + ex - tx * U(0.8) / 1024 + nx * U(3.4) / 1024;
    tri[3] = p.oy + ey - ty * U(0.8) / 1024 + ny * U(3.4) / 1024;
    tri[4] = p.ox + ex - tx * U(0.8) / 1024 - nx * U(3.4) / 1024;
    tri[5] = p.oy + ey - ty * U(0.8) / 1024 - ny * U(3.4) / 1024;
    poly_emit(tri, 3);
    path_fill(s, c);
}

static void icon_home(struct wsurf* s, int x, int y, uint32_t c) {
    struct ui_pen p; pen_at(&p, x, y, STROKE);
    path_begin();
    int32_t roof[6] = { U(2.5), U(10), U(10), U(3), U(17.5), U(10) };
    polyline(&p, roof, 3);
    int32_t body[8] = { U(5), U(8.5), U(5), U(16.5), U(15), U(16.5), U(15), U(8.5) };
    polyline(&p, body, 4);
    int32_t door[8] = { U(8.75), U(16.5), U(8.75), U(13), U(11.25), U(13), U(11.25), U(16.5) };
    polyline(&p, door, 4);
    path_fill(s, c);
}

// 16px box
static void icon_lock(struct wsurf* s, int x, int y, uint32_t c, int broken) {
    struct ui_pen p; pen_at(&p, x, y, 192);
    path_begin();
    arc(&p, U(8), U(6.5), U(3.2), U(3.2), 32, 32);         // shackle
    seg(&p, U(4.8), U(6.5), U(4.8), U(7.5), 0);
    seg(&p, U(11.2), U(6.5), U(11.2), U(7.5), 0);
    path_fill(s, c);
    rrect(s, x + 2, y + 7, 12, 8, 2, c);                   // body
    if (broken) {
        struct ui_pen q; pen_at(&q, x, y, 200);
        path_begin();
        seg(&q, U(1), U(15), U(15), U(1), 1);
        path_fill(s, FX_INSECURE);
    }
}

static void icon_search(struct wsurf* s, int x, int y, uint32_t c) {
    struct ui_pen p; pen_at(&p, x, y, 192);
    path_begin();
    arc(&p, U(7), U(7), U(4.6), U(4.6), 0, 64);
    seg(&p, U(10.4), U(10.4), U(14), U(14), 1);
    path_fill(s, c);
}

// 16px generic page globe (tab favicon placeholder)
static void icon_globe(struct wsurf* s, int x, int y, uint32_t c) {
    struct ui_pen p; pen_at(&p, x, y, 160);
    path_begin();
    arc(&p, U(8), U(8), U(6.4), U(6.4), 0, 64);
    arc(&p, U(8), U(8), U(2.8), U(6.4), 0, 64);
    seg(&p, U(1.8), U(8), U(14.2), U(8), 0);
    path_fill(s, c);
}

static void icon_spinner(struct wsurf* s, int x, int y, int phase) {
    struct ui_pen p; pen_at(&p, x, y, 200);
    path_begin();
    arc(&p, U(8), U(8), U(6.2), U(6.2), 0, 64);
    path_fill(s, 0x30000000u | FX_FOCUS);
    path_begin();
    arc(&p, U(8), U(8), U(6.2), U(6.2), phase * 4, 20);
    path_fill(s, FX_FOCUS);
}

// 12px cross centered in a box of size sz at (x, y)
static void icon_x(struct wsurf* s, int x, int y, int sz, int half, int hw, uint32_t c) {
    struct ui_pen p; pen_at(&p, x, y, hw);
    int32_t m = U(sz) / 2;
    path_begin();
    seg(&p, m - U(half), m - U(half), m + U(half), m + U(half), 1);
    seg(&p, m + U(half), m - U(half), m - U(half), m + U(half), 1);
    path_fill(s, c);
}

static void icon_plus(struct wsurf* s, int x, int y, int sz, uint32_t c) {
    struct ui_pen p; pen_at(&p, x, y, STROKE);
    int32_t m = U(sz) / 2, h = U(6.5);
    path_begin();
    seg(&p, m, m - h, m, m + h, 1);
    seg(&p, m - h, m, m + h, m, 1);
    path_fill(s, c);
}

// Caption glyphs: thin 1px lines on pixel centers (Windows 11 look).
static void caption_glyph(struct wsurf* s, int which, int maxed, int cx, int cy, uint32_t c) {
    struct ui_pen p; pen_at(&p, 0, 0, 128);
    path_begin();
    int32_t x = WR_FIX(cx) + 128, y = WR_FIX(cy) + 128, h = U(5);
    if (which == WIN_CTRL_MIN) {
        seg(&p, x - h, y, x + h, y, 0);
    } else if (which == WIN_CTRL_MAX) {
        if (!maxed) {
            int32_t sq[10] = { x - h, y - h, x + h, y - h, x + h, y + h, x - h, y + h, x - h, y - h };
            polyline(&p, sq, 5);
        } else {
            int32_t b2 = U(2);
            int32_t sq[10] = { x - h, y - h + b2, x + h - b2, y - h + b2, x + h - b2, y + h,
                               x - h, y + h, x - h, y - h + b2 };
            polyline(&p, sq, 5);
            int32_t bk[6] = { x - h + b2, y - h + b2, x - h + b2, y - h, x + h, y - h };
            polyline(&p, bk, 3);
            int32_t bk2[4] = { x + h, y - h, x + h, y + h - b2 };
            polyline(&p, bk2, 2);
        }
    } else {
        seg(&p, x - h, y - h, x + h, y + h, 0);
        seg(&p, x + h, y - h, x - h, y + h, 0);
    }
    path_fill(s, c);
}

// ---- text -----------------------------------------------------------------

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static struct wfont font_of(int px, int bold) {
    struct wfont f;
    f.family = WF_FAMILY_SANS; f.bold = (uint8_t)bold; f.italic = 0; f.px = (uint16_t)px;
    return f;
}

static int text_w(int px, int bold, const char* str, int len) {
    struct wfont f = font_of(px, bold);
    return (int)((wfont_measure(&f, str, len) + 63) >> 6);
}

// Draw text vertically centered on cy, clipped to maxw; when it overflows,
// the last 24px fade into bg (Firefox's tab-title fade). Returns the width.
static int text_draw(struct wsurf* s, int x, int cy, int px, int bold, uint32_t c,
                     const char* str, int len, int maxw, uint32_t bg) {
    if (len < 0) len = slen(str);
    if (maxw <= 0 || len <= 0) return 0;
    struct wfont f = font_of(px, bold);
    struct wfmetrics m;
    wfont_metrics(&f, &m);
    int32_t base = ((int32_t)cy << 6) + (m.ascent - m.descent) / 2;
    int w = text_w(px, bold, str, len);
    int old[4];
    ws_push_clip(s, x, cy - px, x + maxw, cy + px, old);
    wfont_draw(s, &f, (int32_t)x << 6, base, 0xFF000000u | c, str, len, 0, 0);
    if (w > maxw) {
        int fw = 24 < maxw ? 24 : maxw;
        int fx0 = x + maxw - fw;
        for (int yy = cy - px; yy < cy + px; yy++) {
            if (yy < s->cy0 || yy >= s->cy1) continue;
            uint32_t* row = s->px + yy * s->stride;
            for (int i = 0; i < fw; i++) {
                int xx = fx0 + i;
                if (xx < s->cx0 || xx >= s->cx1) continue;
                row[xx] = ws_blend(row[xx], bg, (unsigned)(255 * (i + 1) / fw));
            }
        }
    }
    ws_pop_clip(s, old);
    return w < maxw ? w : maxw;
}


// ---- shared exports (ui_draw.h: taskbar, title bars) ---------------------

void ui_pen_at(struct ui_pen* p, int x, int y, int hw256) { pen_at(p, x, y, hw256); }
void ui_path_begin(void) { path_begin(); }
void ui_path_fill(struct wsurf* s, uint32_t c) { path_fill(s, c); }
void ui_seg(const struct ui_pen* p, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int caps) {
    seg(p, x0, y0, x1, y1, caps);
}
void ui_polyline(const struct ui_pen* p, const int32_t* pts, int n) { polyline(p, pts, n); }
void ui_arc(const struct ui_pen* p, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int a0, int sweep) {
    arc(p, cx, cy, rx, ry, a0, sweep);
}
void ui_rrect(struct wsurf* s, int x, int y, int w, int h, int r, uint32_t c) { rrect(s, x, y, w, h, r, c); }
int ui_text_w(int px, int bold, const char* str, int len) {
    if (len < 0) len = slen(str);
    return text_w(px, bold, str, len);
}
int ui_text_draw(struct wsurf* s, int x, int cy, int px, int bold, uint32_t c,
                 const char* str, int len, int maxw, uint32_t bg) {
    return text_draw(s, x, cy, px, bold, c, str, len, maxw, bg);
}

void ui_screen_surf(struct wsurf* s) {
    s->px = graphics_get_buffer();
    s->w = SCREEN_W; s->h = SCREEN_H; s->stride = SCREEN_W;
    graphics_get_clip(&s->cx0, &s->cy0, &s->cx1, &s->cy1);
}

void ui_screen_done(int y0, int h) {
    for (int y = y0; y < y0 + h; y++)
        if (y >= 0 && y < SCREEN_H) graphics_mark_dirty(y);
}

static int has_prefix(const char* s, const char* p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

// ---- per-window chrome cache ------------------------------------------------

struct ui_cache {
    uint32_t* px;
    int w, cap;
    uint32_t sig;
    int hover;
    int spin;           // spinner phase drawn
    uint32_t geo_sig;   // last logged geometry
};
static struct ui_cache cache[MAX_OKAIS];

static int spinner_phase(void) { return (int)(tick_count / 8) & 15; } // ~12 fps

static int any_busy(int id, struct okai* b) {
    for (int i = 0; i < b->tab_count; i++) if (okai_tab_busy(id, i)) return 1;
    return 0;
}

static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t mix_s(uint32_t h, const char* s) {
    while (*s) h = mix(h, (uint8_t)*s++);
    return mix(h, 0xFF);
}

static uint32_t state_sig(int id, struct okai* b, const struct ui_lay* L, int hover) {
    struct window* w = window_get(b->win_id);
    struct okai_tab* T = okai_tab_of(b);
    uint32_t h = 2166136261u;
    h = mix(h, (uint32_t)L->w); h = mix(h, (uint32_t)w->focused);
    h = mix(h, (uint32_t)window_is_maximized(b->win_id));
    h = mix(h, (uint32_t)b->tab_count); h = mix(h, (uint32_t)b->active_tab);
    h = mix(h, (uint32_t)hover);
    int busy = 0;
    for (int i = 0; i < b->tab_count; i++) {
        h = mix(h, (uint32_t)L->tw[i]);
        h = mix_s(h, b->tabs[i].title);
        h = mix_s(h, b->tabs[i].url);
        int bz = okai_tab_busy(id, i);
        h = mix(h, (uint32_t)bz);
        busy |= bz;
    }
    if (busy) h = mix(h, (uint32_t)spinner_phase());
    h = mix(h, (uint32_t)b->addr_bar_focused);
    if (b->addr_bar_focused) h = mix_s(h, b->addr_input);
    h = mix(h, (uint32_t)T->is_https);
    h = mix(h, (uint32_t)T->history_pos); h = mix(h, (uint32_t)T->history_count);
    return h;
}

static void log_geometry(struct okai* b, struct ui_cache* C, const struct ui_lay* L) {
    for (int i = 0; i < b->tab_count; i++)
        if (L->tw[i] != L->tab_target || b->tabs[i].closing) return; // settle first
    uint32_t g = 2166136261u;
    g = mix(g, (uint32_t)L->x0); g = mix(g, (uint32_t)L->y0); g = mix(g, (uint32_t)L->w);
    g = mix(g, (uint32_t)b->tab_count); g = mix(g, (uint32_t)L->tab_target);
    if (g == C->geo_sig) return;
    C->geo_sig = g;
    static const char* nn[4] = { "back", "fwd", "reload", "home" };
    serial_printf("[okai] ui");
    for (int i = 0; i < 4; i++)
        serial_printf(" %s=%d,%d,%d,%d", nn[i], L->nav[i][0], L->nav[i][1], L->nav[i][2], L->nav[i][3]);
    serial_printf(" url=%d,%d,%d,%d ident=%d,%d,%d,%d newtab=%d,%d,%d,%d",
                  L->url[0], L->url[1], L->url[2], L->url[3],
                  L->ident[0], L->ident[1], L->ident[2], L->ident[3],
                  L->newtab[0], L->newtab[1], L->newtab[2], L->newtab[3]);
    static const char* cn[3] = { "min", "max", "close" };
    for (int k = 0; k < 3; k++)
        if (L->has_ctrl[k])
            serial_printf(" %s=%d,%d,%d,%d", cn[k], L->ctrl[k][0], L->ctrl[k][1], L->ctrl[k][2], L->ctrl[k][3]);
    serial_printf("\n[okai] ui tabs n=%d", b->tab_count);
    for (int i = 0; i < b->tab_count; i++) {
        int xr[4];
        tab_close_rect(L, i, xr);
        serial_printf(" tab%d=%d,%d,%d,%d tabx%d=%d,%d,%d,%d", i, L->tx[i], L->tab_y, L->tw[i], L->tab_h,
                      i, xr[0], xr[1], xr[2], xr[3]);
    }
    serial_printf("\n");
}

// Address-bar text: host emphasized, scheme/path secondary (Firefox trims
// "https://"); the focused field shows the typed text and a caret.
static void draw_url_text(struct wsurf* s, struct okai* b, const struct ui_lay* L, int ox, int oy, uint32_t bg) {
    struct okai_tab* T = okai_tab_of(b);
    int x = L->url[0] - ox + 44;
    int cy = L->url[1] - oy + URL_H / 2;
    int maxw = L->url[2] - 44 - 12;
    const int px = 17;
    if (b->addr_bar_focused) {
        const char* t = b->addr_input;
        int n = b->addr_input_len;
        if (n == 0) {
            text_draw(s, x, cy, px, 0, FX_TEXT_2, "Search or enter address", -1, maxw, bg);
            ws_fill_rect(s, x, cy - 11, 2, 22, 0xFF000000u | FX_TEXT);
            return;
        }
        // keep the end (and the caret) visible
        int start = 0;
        while (start < n && text_w(px, 0, t + start, n - start) > maxw - 4) start++;
        int w = text_draw(s, x, cy, px, 0, FX_TEXT, t + start, n - start, maxw, bg);
        ws_fill_rect(s, x + w + 1, cy - 11, 2, 22, 0xFF000000u | FX_TEXT);
        return;
    }
    const char* u = T->url;
    if (!u[0] || has_prefix(u, "okai:")) {
        text_draw(s, x, cy, px, 0, FX_TEXT_2, "Search or enter address", -1, maxw, bg);
        return;
    }
    int n = slen(u);
    int sch = 0;
    if (has_prefix(u, "https://")) { u += 8; n -= 8; }
    else if (has_prefix(u, "http://")) sch = 7;
    int hs = sch, he = sch;
    while (he < n && u[he] != '/' && u[he] != '?' && u[he] != '#') he++;
    // root path "/" of https pages is hidden, like Firefox
    int tail = n - he;
    if (!sch && tail == 1 && u[he] == '/') tail = 0;
    int cx = x, left = maxw;
    if (sch) { int w = text_draw(s, cx, cy, px, 0, FX_TEXT_2, u, sch, left, bg); cx += w; left -= w; }
    if (left > 0) { int w = text_draw(s, cx, cy, px, 0, FX_TEXT, u + hs, he - hs, left, bg); cx += w; left -= w; }
    if (left > 0 && tail > 0) text_draw(s, cx, cy, px, 0, FX_TEXT_2, u + he, tail, left, bg);
}

static void render_chrome(int id, struct okai* b, const struct ui_lay* L, struct wsurf* s, int hover) {
    struct window* w = window_get(b->win_id);
    struct okai_tab* T = okai_tab_of(b);
    int ox = L->x0, oy = L->y0;
    uint32_t frame = w->focused ? FX_FRAME : FX_FRAME_U;

    ws_fill_rect(s, 0, 0, s->w, CHROME_TAB_H, 0xFF000000u | frame);
    ws_fill_rect(s, 0, CHROME_TAB_H, s->w, CHROME_TOOL_H - 1, 0xFF000000u | FX_TOOLBAR);
    ws_fill_rect(s, 0, CHROME_PX - 1, s->w, 1, 0xFF000000u | FX_TOOLBAR_SEP);

    // ---- tabs ----
    int phase = spinner_phase();
    for (int i = 0; i < b->tab_count; i++) {
        int tw = L->tw[i];
        if (tw < 2) continue;
        int tx = L->tx[i] - ox, ty = L->tab_y - oy, th = L->tab_h;
        int sel = (i == b->active_tab);
        uint32_t bg = frame;
        int old[4];
        ws_push_clip(s, tx - 6, 0, tx + tw + 6, CHROME_TAB_H, old);
        if (sel) {
            shadow(s, tx, ty, tw, th, 5, 3, 0x30);
            rrect(s, tx, ty, tw, th, 5, FX_TAB_SEL);
            bg = FX_TAB_SEL;
        } else if (hover == HV_TAB + i || hover == HV_TABX + i) {
            rrect(s, tx, ty, tw, th, 5, FX_TAB_HOVER);
            bg = FX_TAB_HOVER;
        }
        ws_pop_clip(s, old);
        ws_push_clip(s, tx, ty, tx + tw, ty + th, old);
        struct okai_tab* TT = &b->tabs[i];
        int icx = tx + 12, icy = ty + (th - 16) / 2;
        if (okai_tab_busy(id, i)) icon_spinner(s, icx, icy, phase);
        else if (has_prefix(TT->url, "okai:")) icon_search(s, icx, icy, FX_TEXT_2);
        else icon_globe(s, icx, icy, FX_TEXT_2);
        const char* t = TT->title[0] ? TT->title : TT->url;
        if (has_prefix(TT->url, "okai:home")) t = "New Tab";
        int show_x = tw >= TAB_MIN_W && (sel || tw >= 110 || hover == HV_TAB + i || hover == HV_TABX + i);
        int text_x = tx + 36;
        int text_max = tx + tw - (show_x ? 6 + TABX_SZ + 4 : 10) - text_x;
        text_draw(s, text_x, ty + th / 2, 15, 0, FX_TEXT, t, slen(t), text_max, bg);
        if (show_x) {
            int xr[4];
            tab_close_rect(L, i, xr);
            int xx = xr[0] - ox, xy = xr[1] - oy;
            if (hover == HV_TABX + i) rrect(s, xx, xy, TABX_SZ, TABX_SZ, 4, sel ? 0x00E0E0E6 : 0x00CFCFD8);
            icon_x(s, xx, xy, TABX_SZ, 4, 150, FX_TEXT);
        }
        ws_pop_clip(s, old);
    }

    // ---- new tab ----
    if (b->tab_count < OKAI_MAX_TABS) {
        int nx = L->newtab[0] - ox, ny = L->newtab[1] - oy;
        if (hover == HV_NEW) rrect(s, nx, ny, NEWTAB_SZ, NEWTAB_SZ, 4, FX_TAB_HOVER);
        icon_plus(s, nx, ny, NEWTAB_SZ, FX_ICON);
    }

    // ---- caption buttons ----
    int maxed = window_is_maximized(b->win_id);
    for (int k = 0; k < 3; k++) {
        if (!L->has_ctrl[k]) continue;
        int cx = L->ctrl[k][0] - ox, cy = L->ctrl[k][1] - oy;
        int cw = L->ctrl[k][2], ch = L->ctrl[k][3];
        uint32_t gc = FX_TEXT;
        if (hover == HV_CTRL + k) {
            uint32_t hb = k == WIN_CTRL_CLOSE ? FX_CLOSE_HOVER : FX_TAB_HOVER;
            ws_fill_rect(s, cx, cy, cw, ch, 0xFF000000u | hb);
            if (k == WIN_CTRL_CLOSE) gc = 0xFFFFFF;
        }
        caption_glyph(s, k, maxed, cx + cw / 2, cy + ch / 2, gc);
    }

    // ---- nav buttons ----
    int can_back = T->history_pos > 0;
    int can_fwd = T->history_pos + 1 < T->history_count;
    for (int i = 0; i < 4; i++) {
        int bx = L->nav[i][0] - ox, by = L->nav[i][1] - oy;
        int enabled = i == 0 ? can_back : i == 1 ? can_fwd : 1;
        if (hover == HV_NAV + i && enabled) rrect(s, bx, by, NAVBTN, NAVBTN, 5, FX_BTN_HOVER);
        uint32_t c = enabled ? FX_ICON : FX_ICON_OFF;
        int ix = bx + (NAVBTN - 20) / 2, iy = by + (NAVBTN - 20) / 2;
        if (i == 0) icon_back(s, ix, iy, c, -1);
        else if (i == 1) icon_back(s, ix, iy, c, 1);
        else if (i == 2) icon_reload(s, ix, iy, c);
        else icon_home(s, ix, iy, c);
    }

    // ---- address bar ----
    int ux = L->url[0] - ox, uy = L->url[1] - oy, uw = L->url[2];
    uint32_t ubg;
    if (b->addr_bar_focused) {
        rrect(s, ux - 1, uy - 1, uw + 2, URL_H + 2, 6, FX_FOCUS);
        rrect(s, ux + 1, uy + 1, uw - 2, URL_H - 2, 4, 0xFFFFFF);
        ubg = 0xFFFFFF;
    } else {
        ubg = (hover == HV_URL || hover == HV_IDENT) ? FX_URLBAR_HOVER : FX_URLBAR;
        rrect(s, ux, uy, uw, URL_H, 5, ubg);
    }
    int ix = L->ident[0] - ox, iy = L->ident[1] - oy;
    if (hover == HV_IDENT || b->show_security) rrect(s, ix, iy, IDENT_SZ, IDENT_SZ, 4, b->addr_bar_focused ? FX_URLBAR : 0x00DADAE0);
    if (b->addr_bar_focused || !T->url[0] || has_prefix(T->url, "okai:"))
        icon_search(s, ix + 8, iy + 8, FX_TEXT_2);
    else
        icon_lock(s, ix + 8, iy + 8, T->is_https ? FX_SECURE : FX_TEXT_2, !T->is_https);
    draw_url_text(s, b, L, ox, oy, ubg);
}

// Screen surface over the backbuffer, clipped to the current graphics clip.
static void screen_surf(struct wsurf* s) {
    s->px = graphics_get_buffer();
    s->w = SCREEN_W; s->h = SCREEN_H; s->stride = SCREEN_W;
    graphics_get_clip(&s->cx0, &s->cy0, &s->cx1, &s->cy1);
}

static void mark_rows(int y, int h) {
    for (int r = y < 0 ? 0 : y; r < y + h && r < SCREEN_H; r++) graphics_mark_dirty(r);
}

// Firefox-style site-identity panel under the address bar.
static void draw_security_panel(struct okai* b, const struct ui_lay* L) {
    struct okai_tab* T = okai_tab_of(b);
    struct wsurf s;
    screen_surf(&s);
    if (!s.px) return;
    int px = L->ident[0], py = L->url[1] + URL_H + 6;
    int pw = 380, ph = 168;
    shadow(&s, px, py, pw, ph, 8, 4, 0x38);
    rrect(&s, px, py, pw, ph, 8, 0xFFFFFF);
    char host[96]; int hi = 0;
    const char* hp = T->url;
    if (has_prefix(hp, "https://")) hp += 8; else if (has_prefix(hp, "http://")) hp += 7;
    while (*hp && *hp != '/' && *hp != ':' && hi < 95) host[hi++] = *hp++;
    host[hi] = 0;
    int x = px + 20, y = py + 30;
    text_draw(&s, x, y, 17, 1, FX_TEXT, host[0] ? host : "okai", -1, pw - 40, 0xFFFFFF);
    ws_fill_rect(&s, px, py + 56, pw, 1, 0xFF000000u | 0x00E0E0E6);
    int internal = !T->url[0] || has_prefix(T->url, "okai:");
    if (internal) {
        icon_search(&s, x, py + 72, FX_TEXT_2);
        text_draw(&s, x + 28, py + 80, 15, 1, FX_TEXT, "This is a built-in okai page", -1, pw - 60, 0xFFFFFF);
        text_draw(&s, x + 28, py + 108, 14, 0, FX_TEXT_2, "It is generated locally \xE2\x80\x94 nothing", -1, pw - 60, 0xFFFFFF);
        text_draw(&s, x + 28, py + 136, 14, 0, FX_TEXT_2, "was loaded from the network.", -1, pw - 60, 0xFFFFFF);
        mark_rows(py - 4, ph + 10);
        return;
    }
    icon_lock(&s, x, py + 72, T->is_https ? FX_SECURE : FX_TEXT_2, !T->is_https);
    if (T->is_https) {
        text_draw(&s, x + 28, py + 80, 15, 1, FX_TEXT, "Connection is secure", -1, pw - 60, 0xFFFFFF);
        text_draw(&s, x + 28, py + 108, 14, 0, FX_TEXT_2, "TLS 1.3 \xE2\x80\x94 certificate verified", -1, pw - 60, 0xFFFFFF);
        text_draw(&s, x + 28, py + 136, 14, 0, FX_TEXT_2, "Information you send is encrypted.", -1, pw - 60, 0xFFFFFF);
    } else {
        text_draw(&s, x + 28, py + 80, 15, 1, FX_TEXT, "Connection is not secure", -1, pw - 60, 0xFFFFFF);
        text_draw(&s, x + 28, py + 108, 14, 0, FX_TEXT_2, "Information you send to this site", -1, pw - 60, 0xFFFFFF);
        text_draw(&s, x + 28, py + 136, 14, 0, FX_TEXT_2, "could be seen by others.", -1, pw - 60, 0xFFFFFF);
    }
    mark_rows(py - 4, ph + 10);
}

// Status panel bottom-left of the page while the active tab loads.
static void draw_status(int id, struct okai* b, const struct ui_lay* L) {
    int subs = 0;
    long rxb = okai_fetch_bytes(id, &subs);
    if (rxb < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;
    char msg[48];
    int mi = 0;
    const char* pre = subs ? "Loading resources\xE2\x80\xA6 " : "Loading\xE2\x80\xA6 ";
    while (pre[mi] && mi < 30) { msg[mi] = pre[mi]; mi++; }
    long kb = rxb / 1024;
    char rev[12]; int rl = 0;
    if (kb == 0) rev[rl++] = '0';
    while (kb > 0 && rl < 11) { rev[rl++] = (char)('0' + kb % 10); kb /= 10; }
    while (rl > 0 && mi < 42) msg[mi++] = rev[--rl];
    msg[mi++] = ' '; msg[mi++] = 'K'; msg[mi++] = 'B';
    msg[mi] = 0;
    struct wsurf s;
    screen_surf(&s);
    if (!s.px) return;
    int tw = text_w(14, 0, msg, mi);
    int ph = 28, pw = tw + 20;
    int x = L->x0, y = w->y + w->h - WIN_BORDER - ph;
    int old[4];
    ws_push_clip(&s, x, y - 2, x + pw + 2, y + ph, old);
    rrect(&s, x - 6, y - 1, pw + 7, ph + 7, 5, FX_TOOLBAR_SEP);
    rrect(&s, x - 6, y, pw + 6, ph + 6, 4, FX_TOOLBAR);
    text_draw(&s, x + 10, y + ph / 2, 14, 0, FX_TEXT, msg, mi, tw + 2, FX_TOOLBAR);
    ws_pop_clip(&s, old);
    mark_rows(y - 2, ph + 2);
}

// Repaint request check for the desktop loop: hover target or spinner
// phase changed since the chrome was last rendered.
int okai_ui_needs_paint(int id) {
    struct okai* b = okai_get(id);
    if (!b) return 0;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return 0;
    struct ui_lay L;
    if (!ui_layout(b, &L)) return 0;
    struct ui_cache* C = &cache[id];
    int mx, my;
    mouse_get_position(&mx, &my);
    if (hover_of(b, &L, mx, my) != C->hover) return 1;
    if (any_busy(id, b) && spinner_phase() != C->spin) return 1;
    if (!any_busy(id, b) && C->spin >= 0) return 1;  // drop the last spinner frame
    return 0;
}

// Paint the chrome band (from the cache), then the panels over the page.
// Called by okai_paint_overlays* under the caller's clip rect.
void okai_ui_paint(int id) {
    struct okai* b = okai_get(id);
    if (!b) return;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return;
    struct ui_lay L;
    if (!ui_layout(b, &L)) return;
    struct ui_cache* C = &cache[id];
    int mx, my;
    mouse_get_position(&mx, &my);
    int hover = hover_of(b, &L, mx, my);
    if (L.w * CHROME_PX > C->cap) {
        if (C->px) kfree(C->px);
        C->px = (uint32_t*)kmalloc((uint32_t)L.w * CHROME_PX * 4);
        C->cap = C->px ? L.w * CHROME_PX : 0;
        C->sig = 0;
        if (!C->px) return;
    }
    uint32_t sig = state_sig(id, b, &L, hover);
    if (sig != C->sig || C->w != L.w) {
        struct wsurf s;
        s.px = C->px; s.w = L.w; s.h = CHROME_PX; s.stride = L.w;
        ws_reset_clip(&s);
        render_chrome(id, b, &L, &s, hover);
        C->sig = sig;
        C->w = L.w;
        C->hover = hover;
        C->spin = any_busy(id, b) ? spinner_phase() : -1;
        log_geometry(b, C, &L);
    }
    graphics_blit_pixels(L.x0, L.y0, C->px, L.w, CHROME_PX, L.w);
    draw_status(id, b, &L);
    if (b->show_security) draw_security_panel(b, &L);
}

void okai_ui_forget(int id) {
    if (id < 0 || id >= MAX_OKAIS) return;
    if (cache[id].px) kfree(cache[id].px);
    cache[id].px = 0; cache[id].cap = 0; cache[id].sig = 0; cache[id].geo_sig = 0;
}
