// Window chrome effects: rounded corners and the open / minimize / restore animations.
//
// Corners. Windows are painted as plain rectangles straight into the backbuffer. Rounding is a pass over the four
// corner squares of the rectangle that was just painted: a pixel outside the arc becomes whatever lies behind the window
// (the scene below the window's z is recomposited into the square), the edge pixels get an anti-aliased mix of that and
// the border colour, the 2 px ring along the arc is border colour, and the pixels inside keep what the window painted.
// The result of a pixel never depends on its previous value except inside the arc, so the pass is idempotent and can be
// repeated by every painter (window, overlay, drag) without eating into the edge.
//
// Animations. The compositor leaves an animating window alone (winfx_hides). The animation owns two pixel buffers: a
// snapshot of the window drawn alone over what lies below it (alpha = 0 outside the rounded corners), and a copy of the
// scene without the window over the area the animation covers. Each frame writes, for the union of the previous and the
// new rectangle, the scene copy with the scaled, faded snapshot blended on top. Nothing is recomposited per frame.
#include "winfx.h"
#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "serial.h"
#include "taskbar.h"
#include <stdint.h>

extern uint32_t tick_count;
extern void desktop_paint_below(int zlimit, int x, int y, int w, int h);
extern void desktop_paint_window_alone(int id);
extern void desktop_paint_rect_pub(int x, int y, int w, int h);
extern uint32_t window_border_color(const struct window* w);

#define R WIN_RADIUS

// ---- corner masks -------------------------------------------------------------------------------------------------------

static uint8_t cov[R][R];     // coverage of the rounded rectangle for the top-left quadrant, 0..255
static uint8_t ring[R][R];    // 1 = border ring (inside the arc, within WIN_BORDER of its edge)
static int tab_ready;

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

static void tab_init(void) {
    if (tab_ready) return;
    for (int j = 0; j < R; j++)
        for (int i = 0; i < R; i++) {
            int dx = R * 16 - (i * 16 + 8), dy = R * 16 - (j * 16 + 8);      // pixel centre to the arc centre, 1/16 px
            int d = (int)isqrt32((uint32_t)(dx * dx + dy * dy));
            int c = (R * 16 - d + 8) * 255 / 16;                            // one pixel wide ramp around the arc
            cov[j][i] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
            ring[j][i] = d >= (R - WIN_BORDER) * 16 ? 1 : 0;
        }
    tab_ready = 1;
}

static uint32_t blend(uint32_t bg, uint32_t fg, uint32_t a) {   // a: 0..255 weight of fg
    uint32_t rb = (bg & 0x00FF00FF) * (255 - a) + (fg & 0x00FF00FF) * a + 0x00800080;
    rb = ((rb + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
    uint32_t g = (bg & 0x0000FF00) * (255 - a) + (fg & 0x0000FF00) * a + 0x00008000;
    g = ((g + ((g >> 8) & 0x0000FF00)) >> 8) & 0x0000FF00;
    return rb | g;
}

// ---- drop shadows ----------------------------------------------------------------------------------------------------------
// A window casts a soft shadow on everything below it (wallpaper, icons, lower windows): black, peak opacity SH_A, falling off
// over SH_R pixels beyond a core rectangle that is the window shifted down by SH_DY. Only pixels outside the window are
// touched (except its corner squares, whose outside-the-arc pixels show the scene below plus the shadow). Whoever repaints a
// region from the scene model draws the shadows in z order; whoever repaints part of a window over an existing shadow
// re-applies the shadows of the windows above it (winfx_shadows_above).
#define SH_R 16
#define SH_DY 6
#define SH_A 112
static uint8_t sh_g[SH_R + 1];
static int sh_ready;

static int has_shadow(const struct window* w, int id) {
    return w && !window_is_maximized(id) && w->w >= 4 * R && w->h >= 4 * R;
}

void winfx_shadow_bounds(int id, int* x, int* y, int* w, int* h) {
    struct window* win = window_get(id);
    if (!win) { *x = *y = *w = *h = 0; return; }
    if (!has_shadow(win, id)) { *x = win->x; *y = win->y; *w = win->w; *h = win->h; return; }
    *x = win->x - SH_R; *y = win->y + SH_DY - SH_R; *w = win->w + 2 * SH_R; *h = win->h + 2 * SH_R;
}

void winfx_grow_rect(int* x, int* y, int* w, int* h) {
    *x -= SH_R; *y += SH_DY - SH_R; *w += 2 * SH_R; *h += 2 * SH_R;
}

void winfx_shadow(int id, int rx, int ry, int rw, int rh, int inside) {
    struct window* win = window_get(id);
    if (!win || !win->visible || win->minimized || winfx_hides(id) || !has_shadow(win, id)) return;
    if (!sh_ready) { for (int d = 0; d <= SH_R; d++) sh_g[d] = (uint8_t)(255 * (SH_R - d) * (SH_R - d) / (SH_R * SH_R)); sh_ready = 1; }
    int bx, by, bw, bh;
    winfx_shadow_bounds(id, &bx, &by, &bw, &bh);
    int x0 = rx > bx ? rx : bx, y0 = ry > by ? ry : by;
    int x1 = rx + rw < bx + bw ? rx + rw : bx + bw, y1 = ry + rh < by + bh ? ry + rh : by + bh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H - TASKBAR_H) y1 = SCREEN_H - TASKBAR_H;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t* bb = graphics_get_buffer();
    int cx0 = win->x, cx1 = win->x + win->w, cy0 = win->y + SH_DY, cy1 = win->y + win->h + SH_DY;
    for (int y = y0; y < y1; y++) {
        int dy = y < cy0 ? cy0 - y : y >= cy1 ? y - cy1 + 1 : 0;
        if (dy > SH_R) continue;
        uint32_t gy = sh_g[dy];
        int sk0 = 0, sk1 = 0;                                        // x range that is the window itself: not touched
        if (!inside && y >= win->y && y < win->y + win->h) {
            int corner_row = y < win->y + R || y >= win->y + win->h - R;
            sk0 = corner_row ? win->x + R : win->x; sk1 = corner_row ? win->x + win->w - R : win->x + win->w;
        }
        uint32_t* row = bb + y * SCREEN_W;
        for (int x = x0; x < x1; x++) {
            if (x >= sk0 && x < sk1) { x = sk1 - 1; continue; }
            int dx = x < cx0 ? cx0 - x : x >= cx1 ? x - cx1 + 1 : 0;
            if (dx > SH_R) continue;
            uint32_t a = (uint32_t)SH_A * sh_g[dx] / 255 * gy / 255;
            if (a) row[x] = blend(row[x], 0, a);
        }
    }
    graphics_mark_rect(x0, y0, x1 - x0, y1 - y0);
}

// The shadows of the windows above z over a region that a lower window just repainted.
void winfx_shadows_above(int z, int x, int y, int w, int h) {
    int ids[MAX_WINDOWS], n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* o = window_get(i);
        if (o && o->visible && !o->minimized && o->z > z) ids[n++] = i;
    }
    for (int a = 1; a < n; a++) {
        int id = ids[a], zz = window_get(id)->z, b = a - 1;
        while (b >= 0 && window_get(ids[b])->z > zz) { ids[b + 1] = ids[b]; b--; }
        ids[b + 1] = id;
    }
    for (int a = 0; a < n; a++) winfx_shadow(ids[a], x, y, w, h, 0);
}

int winfx_radius(int id) {
    struct window* w = window_get(id);
    if (!w || !w->visible) return 0;
    if (window_is_maximized(id)) return 0;
    if (w->w < 4 * R || w->h < 4 * R) return 0;
    return R;
}

void winfx_round_corners(int id, int cx0, int cy0, int cx1, int cy1) {
    struct window* w = window_get(id);
    if (!w || winfx_radius(id) == 0) return;
    tab_init();
    uint32_t* bb = graphics_get_buffer();
    uint32_t bc = window_border_color(w);
    for (int c = 0; c < 4; c++) {
        int sx = (c & 1) ? w->x + w->w - R : w->x, sy = (c & 2) ? w->y + w->h - R : w->y;
        int x0 = sx > cx0 ? sx : cx0, y0 = sy > cy0 ? sy : cy0;
        int x1 = sx + R < cx1 ? sx + R : cx1, y1 = sy + R < cy1 ? sy + R : cy1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > SCREEN_W) x1 = SCREEN_W;
        if (y1 > SCREEN_H) y1 = SCREEN_H;
        if (x0 >= x1 || y0 >= y1) continue;
        uint32_t save[R * R];
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) save[(y - sy) * R + (x - sx)] = bb[y * SCREEN_W + x];
        desktop_paint_below(w->z, x0, y0, x1 - x0, y1 - y0);           // what is behind the window
        winfx_shadow(id, x0, y0, x1 - x0, y1 - y0, 1);                  // ... including this window's own shadow
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                int qi = (c & 1) ? R - 1 - (x - sx) : x - sx, qj = (c & 2) ? R - 1 - (y - sy) : y - sy;
                uint32_t cv = cov[qj][qi];
                uint32_t* p = &bb[y * SCREEN_W + x];
                if (cv == 255) *p = ring[qj][qi] ? bc : save[(y - sy) * R + (x - sx)];
                else if (cv) *p = blend(*p, bc, cv);
            }
        graphics_mark_rect(x0, y0, x1 - x0, y1 - y0);
    }
}

// ---- animations -------------------------------------------------------------------------------------------------------------

enum { FX_OPEN = 1, FX_MIN = 2, FX_RESTORE = 3, FX_CLOSE = 4 };
#define DUR_OPEN_TICKS    20      // 100 Hz ticks
#define DUR_MIN_TICKS     25
#define DUR_CLOSE_TICKS   14

struct anim {
    int active, id, kind;
    uint32_t t0, last_tick;
    int dur;
    int sw, sh, wx, wy;           // snapshot size and the window's origin
    uint32_t* snap;               // ARGB
    int ux, uy, uw, uh;           // area the animation may touch
    uint32_t* bg;                 // the scene without the window over that area
    int px, py, pw, ph, have_prev;
    int tcx, tcy;                 // minimize target: centre of the taskbar button
    int frames;
};

static struct anim A;
static uint8_t pending[MAX_WINDOWS];
static int snapshotting;
static int work_bottom(void) { return SCREEN_H - TASKBAR_H; }

int winfx_hides(int id) {
    if (snapshotting) return 0;
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    return pending[id] || (A.active && A.kind != FX_CLOSE && A.id == id);
}
int winfx_active(void) {
    if (A.active) return 1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (pending[i]) return 1;
    return 0;
}

static void free_buffers(void) {
    if (A.snap) { kfree(A.snap); A.snap = 0; }
    if (A.bg) { kfree(A.bg); A.bg = 0; }
}

// Leave the current animation at once (the end state is applied).
static void finish(void) {
    if (!A.active) return;
    int id = A.id, kind = A.kind, frames = A.frames;
    struct window* w = kind == FX_CLOSE ? 0 : window_get(id);     // (a closed window is gone; its id may belong to a new one)
    uint32_t ms = (tick_count - A.t0) * 10;
    int ux = A.ux, uy = A.uy, uw = A.uw, uh = A.uh;
    A.active = 0;
    free_buffers();
    if (kind == FX_CLOSE) desktop_paint_rect_pub(ux, uy, uw, uh);
    else if (w && w->visible) {
        desktop_paint_rect_pub(ux, uy, uw, uh);                     // the area the frames touched, from the scene model
        if (kind != FX_MIN) { w->dirty = 1; w->pr_valid = 0; w->pxd_valid = 0; }
    }
    serial_printf("[winfx] %s win=%d frames=%d ms=%u\n", kind == FX_OPEN ? "open" : kind == FX_MIN ? "minimize" : kind == FX_CLOSE ? "close" : "restore", id, frames, (unsigned)ms);
}

static int rect_union(int* x, int* y, int* w, int* h, int ax, int ay, int aw, int ah) {
    int x1 = *x + *w, y1 = *y + *h, bx1 = ax + aw, by1 = ay + ah;
    if (ax < *x) *x = ax;
    if (ay < *y) *y = ay;
    if (bx1 > x1) x1 = bx1;
    if (by1 > y1) y1 = by1;
    *w = x1 - *x; *h = y1 - *y;
    return 1;
}

// Draw `id` alone over what is below it and keep the pixels (alpha 0 outside the corners).
static int take_snapshot(struct window* w, int id) {
    int sw = w->w, sh = w->h;
    if (w->x < 0 || w->y < 0 || w->x + sw > SCREEN_W || w->y + sh > SCREEN_H) return 0;
    uint32_t* snap = (uint32_t*)kmalloc((uint32_t)sw * (uint32_t)sh * 4);
    if (!snap) return 0;
    snapshotting = 1;
    desktop_paint_window_alone(id);
    snapshotting = 0;
    uint32_t* bb = graphics_get_buffer();
    for (int y = 0; y < sh; y++) {
        const uint32_t* src = bb + (w->y + y) * SCREEN_W + w->x;
        uint32_t* dst = snap + y * sw;
        for (int x = 0; x < sw; x++) dst[x] = src[x] | 0xFF000000u;
    }
    if (winfx_radius(id)) {
        tab_init();
        uint32_t bc = window_border_color(w);
        for (int c = 0; c < 4; c++) {
            int ox = (c & 1) ? sw - R : 0, oy = (c & 2) ? sh - R : 0;
            for (int j = 0; j < R; j++)
                for (int i = 0; i < R; i++) {
                    int qi = (c & 1) ? R - 1 - i : i, qj = (c & 2) ? R - 1 - j : j;
                    uint32_t cv = cov[qj][qi];
                    if (cv == 255) continue;
                    snap[(oy + j) * sw + ox + i] = (cv << 24) | bc;
                }
        }
    }
    A.snap = snap; A.sw = sw; A.sh = sh; A.wx = w->x; A.wy = w->y;
    return 1;
}

static void target_center(int id, int* cx, int* cy) {
    int r[4];
    if (taskbar_button_rect(id, r)) { *cx = r[0] + r[2] / 2; *cy = r[1] + r[3] / 2; }
    else { *cx = SCREEN_W / 2; *cy = SCREEN_H - TASKBAR_H / 2; }
}

// Start an animation. `hidden_first`: the window is on screen now (minimize) or is drawn alone for the snapshot.
static int begin(int id, int kind) {
    struct window* w = window_get(id);
    if (!w || !w->visible || w->w < 32 || w->h < 32) return 0;
    finish();
    if (!take_snapshot(w, id)) return 0;
    A.id = id; A.kind = kind;
    A.dur = kind == FX_OPEN ? DUR_OPEN_TICKS : DUR_MIN_TICKS;
    A.active = 1;                                   // hides the window from now on
    // the scene without the window
    int ux, uy, uw, uh;
    winfx_shadow_bounds(id, &ux, &uy, &uw, &uh);                    // the window and its shadow: the shadow is gone while it animates
    if (kind != FX_OPEN) {
        target_center(id, &A.tcx, &A.tcy);
        rect_union(&ux, &uy, &uw, &uh, A.tcx - 4, A.tcy - 4, 8, 8);
    } else {
        rect_union(&ux, &uy, &uw, &uh, w->x, w->y, w->w, w->h + 24);   // the opening window slides up by 24 px at most
    }
    if (ux < 0) { uw += ux; ux = 0; }
    if (uy < 0) { uh += uy; uy = 0; }
    if (ux + uw > SCREEN_W) uw = SCREEN_W - ux;
    if (uy + uh > work_bottom()) uh = work_bottom() - uy;
    if (uw <= 0 || uh <= 0) { A.active = 0; free_buffers(); return 0; }
    A.ux = ux; A.uy = uy; A.uw = uw; A.uh = uh;
    desktop_paint_rect_pub(ux, uy, uw, uh);           // window hidden now: the scene as it is without it (and without its shadow)
    A.bg = (uint32_t*)kmalloc((uint32_t)uw * (uint32_t)uh * 4);
    if (!A.bg) { A.active = 0; free_buffers(); return 0; }
    uint32_t* bb = graphics_get_buffer();
    for (int y = 0; y < uh; y++)
        for (int x = 0; x < uw; x++) A.bg[y * uw + x] = bb[(uy + y) * SCREEN_W + ux + x];
    A.t0 = tick_count; A.last_tick = tick_count - 1; A.have_prev = 0; A.frames = 0;
    return 1;
}

static void geometry(int t1024, int* nx, int* ny, int* nw, int* nh, int* alpha) {
    int sw = A.sw, sh = A.sh;
    int cx = A.wx + sw / 2, cy = A.wy + sh / 2;
    if (A.kind == FX_OPEN) {
        int u = 1024 - t1024;                                       // easeOutCubic: e = 1 - (1-t)^3
        int e = 1024 - (int)(((int64_t)u * u >> 10) * u >> 10);
        int s = 920 + 104 * e / 1024;                              // 0.90 .. 1.0 (x1024)
        *nw = sw * s / 1024; *nh = sh * s / 1024;
        *nx = cx - *nw / 2; *ny = cy - *nh / 2 + 24 * (1024 - e) / 1024;
        *alpha = 255 * e / 1024;
        return;
    }
    if (A.kind == FX_CLOSE) {                                       // shrinks a little, sinks and fades out; slow start
        int e2 = (int)((int64_t)t1024 * t1024 >> 10);
        int s2 = 1024 - 70 * e2 / 1024;
        *nw = sw * s2 / 1024; *nh = sh * s2 / 1024;
        *nx = cx - *nw / 2; *ny = cy - *nh / 2 + 12 * e2 / 1024;
        *alpha = 255 - 255 * e2 / 1024;
        return;
    }
    int e;                                                          // 0 = the window, 1024 = collapsed into the button
    if (A.kind == FX_MIN) { e = (int)((int64_t)t1024 * t1024 * (3 * 1024 - 2 * t1024) >> 20); }              // smoothstep
    else { int u = 1024 - t1024; e = (int)(((int64_t)u * u >> 10) * u >> 10); }                              // (1-t)^3: fast start, soft landing
    if (e < 0) e = 0;
    if (e > 1024) e = 1024;
    int s = 1024 - 930 * e / 1024;                                  // 1.0 .. 0.09
    *nw = sw * s / 1024; *nh = sh * s / 1024;
    int ccx = cx + (A.tcx - cx) * e / 1024, ccy = cy + (A.tcy - cy) * e / 1024;
    *nx = ccx - *nw / 2; *ny = ccy - *nh / 2;
    *alpha = 255 - 235 * e / 1024;
}

static inline uint32_t lerp_lane(uint32_t a, uint32_t b, uint32_t f) { return ((a * (256 - f) + b * f) >> 8) & 0x00FF00FFu; }
static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t f) {
    return lerp_lane(a & 0x00FF00FFu, b & 0x00FF00FFu, f) | (lerp_lane((a >> 8) & 0x00FF00FFu, (b >> 8) & 0x00FF00FFu, f) << 8);
}

static int xs_tab[SCREEN_W_MAX];

static void draw_frame(int nx, int ny, int nw, int nh, int alpha) {
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    // area to write: union of the previous and the new rectangle, inside the animation's area and above the taskbar
    int x = nx, y = ny, w = nw, h = nh;
    if (A.have_prev) rect_union(&x, &y, &w, &h, A.px, A.py, A.pw, A.ph);
    int x0 = x < A.ux ? A.ux : x, y0 = y < A.uy ? A.uy : y;
    int x1 = x + w > A.ux + A.uw ? A.ux + A.uw : x + w, y1 = y + h > A.uy + A.uh ? A.uy + A.uh : y + h;
    if (x0 >= x1 || y0 >= y1) { A.px = nx; A.py = ny; A.pw = nw; A.ph = nh; A.have_prev = 1; return; }
    uint32_t* bb = graphics_get_buffer();
    int sw = A.sw, sh = A.sh;
    int same = nw == sw && nh == sh;
    // source column for every destination column, 1/256 px, pixel-centre aligned
    if (!same) {
        int cx0 = nx > x0 ? nx : x0, cx1 = nx + nw < x1 ? nx + nw : x1;
        for (int xx = cx0; xx < cx1; xx++) {
            int v = (int)(((uint32_t)(2 * (xx - nx) + 1) * (uint32_t)sw * 128u) / (uint32_t)nw) - 128;
            if (v < 0) v = 0;
            if (v > (sw - 1) * 256) v = (sw - 1) * 256;
            xs_tab[xx] = v;
        }
    }
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* dst = bb + yy * SCREEN_W;
        const uint32_t* bgrow = A.bg + (yy - A.uy) * A.uw - A.ux;
        int inside_y = yy >= ny && yy < ny + nh;
        if (!inside_y) { for (int xx = x0; xx < x1; xx++) dst[xx] = bgrow[xx]; continue; }
        int cx0 = nx > x0 ? nx : x0, cx1 = nx + nw < x1 ? nx + nw : x1;
        for (int xx = x0; xx < cx0; xx++) dst[xx] = bgrow[xx];
        for (int xx = cx1; xx < x1; xx++) dst[xx] = bgrow[xx];
        if (same) {
            const uint32_t* srow = A.snap + (yy - ny) * sw - nx;
            for (int xx = cx0; xx < cx1; xx++) {
                uint32_t s = srow[xx];
                uint32_t a = (s >> 24) * (uint32_t)alpha / 255;
                dst[xx] = a >= 255 ? (s & 0x00FFFFFF) : a == 0 ? bgrow[xx] : blend(bgrow[xx], s, a);
            }
            continue;
        }
        int vy = (int)(((uint32_t)(2 * (yy - ny) + 1) * (uint32_t)sh * 128u) / (uint32_t)nh) - 128;
        if (vy < 0) vy = 0;
        if (vy > (sh - 1) * 256) vy = (sh - 1) * 256;
        int sy0 = vy >> 8, fy = vy & 255, sy1 = sy0 + 1 < sh ? sy0 + 1 : sy0;
        const uint32_t* r0 = A.snap + sy0 * sw;
        const uint32_t* r1 = A.snap + sy1 * sw;
        for (int xx = cx0; xx < cx1; xx++) {
            int vx = xs_tab[xx];
            int sx0 = vx >> 8, fx = vx & 255, sx1 = sx0 + 1 < sw ? sx0 + 1 : sx0;
            uint32_t top = lerp_px(r0[sx0], r0[sx1], (uint32_t)fx);
            uint32_t bot = lerp_px(r1[sx0], r1[sx1], (uint32_t)fx);
            uint32_t s = lerp_px(top, bot, (uint32_t)fy);
            uint32_t a = (s >> 24) * (uint32_t)alpha / 255;
            dst[xx] = a >= 255 ? (s & 0x00FFFFFF) : a == 0 ? bgrow[xx] : blend(bgrow[xx], s, a);
        }
    }
    graphics_mark_rect(x0, y0, x1 - x0, y1 - y0);
    A.px = nx; A.py = ny; A.pw = nw; A.ph = nh; A.have_prev = 1;
}

// ---- entry points ------------------------------------------------------------------------------------------------------------

void winfx_open(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    pending[id] = 1;                           // (a popup: window_set_popup clears it, see winfx_step)
}

int winfx_minimize(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    pending[id] = 0;
    return begin(id, FX_MIN);
}

int winfx_restore(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return 0;
    pending[id] = 0;
    return begin(id, FX_RESTORE);
}

// Closing: the snapshot is taken while the window still exists (prepare), the animation starts once it is gone (commit).
static int close_ready;

int winfx_close_prepare(int id) {
    struct window* w = window_get(id);
    if (!w || !w->visible || w->minimized || w->is_popup || w->w < 64 || w->h < 64 || winfx_hides(id)) return 0;
    finish();
    if (!take_snapshot(w, id)) return 0;
    A.id = id; A.kind = FX_CLOSE; A.dur = DUR_CLOSE_TICKS;
    winfx_shadow_bounds(id, &A.ux, &A.uy, &A.uw, &A.uh);
    close_ready = 1;
    return 1;
}

void winfx_close_commit(void) {
    if (!close_ready) return;
    close_ready = 0;
    int ux = A.ux, uy = A.uy, uw = A.uw, uh = A.uh;
    if (ux < 0) { uw += ux; ux = 0; }
    if (uy < 0) { uh += uy; uy = 0; }
    if (ux + uw > SCREEN_W) uw = SCREEN_W - ux;
    if (uy + uh > work_bottom()) uh = work_bottom() - uy;
    if (uw <= 0 || uh <= 0) { free_buffers(); return; }
    A.ux = ux; A.uy = uy; A.uw = uw; A.uh = uh;
    desktop_paint_rect_pub(ux, uy, uw, uh);           // the scene without the window
    A.bg = (uint32_t*)kmalloc((uint32_t)uw * (uint32_t)uh * 4);
    if (!A.bg) { free_buffers(); return; }
    uint32_t* bb = graphics_get_buffer();
    for (int y = 0; y < uh; y++)
        for (int x = 0; x < uw; x++) A.bg[y * uw + x] = bb[(uy + y) * SCREEN_W + ux + x];
    A.t0 = tick_count; A.last_tick = tick_count - 1; A.have_prev = 0; A.frames = 0;
    A.active = 1;
}

void winfx_cancel(int id) {
    if (id < 0 || id >= MAX_WINDOWS) return;
    pending[id] = 0;
    if (A.active && A.kind != FX_CLOSE && A.id == id) {
        int ux = A.ux, uy = A.uy, uw = A.uw, uh = A.uh;
        A.active = 0;
        free_buffers();
        desktop_paint_rect_pub(ux, uy, uw, uh);
    }
}

void winfx_step(void) {
    // windows that were just created start their opening animation, one at a time
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!pending[i]) continue;
        struct window* w = window_get(i);
        if (w && w->visible && w->is_popup) {                      // a popup does not animate, but it casts a shadow
            int bx, by, bw, bh;
            winfx_shadow_bounds(i, &bx, &by, &bw, &bh);
            pending[i] = 0;
            desktop_paint_rect_pub(bx, by, bw, bh);
            continue;
        }
        if (!w || !w->visible || w->minimized) { pending[i] = 0; continue; }
        pending[i] = 0;
        if (!begin(i, FX_OPEN)) { w->dirty = 1; w->pr_valid = 0; }
    }
    if (!A.active) return;
    if (A.last_tick == tick_count) return;                   // one frame per tick is the most the clock can show
    A.last_tick = tick_count;
    int el = (int)(tick_count - A.t0);
    if (el >= A.dur) { finish(); return; }
    int t1024 = el * 1024 / A.dur;
    int nx, ny, nw, nh, a;
    geometry(t1024, &nx, &ny, &nw, &nh, &a);
    draw_frame(nx, ny, nw, nh, a);
    A.frames++;
}
