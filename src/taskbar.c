// Desktop taskbar: a dark frosted bar along the bottom edge.
//
// Left: one button per window (app icon + Noto Sans title, red pill under
// the focused one, grey pill under the others). Right: FPS chip and the
// clock (CMOS RTC, UTC). No launcher: KAnarchy does everything through the
// terminal (Ctrl+Alt+T / `terminal` open more).
//
// Like the okai chrome, it is rendered into a cached 1920 x TASKBAR_H pixel
// strip and re-rendered only when its state signature changes (window list,
// focus, titles, hover, fps, clock minute). Only taskbar_update() (main
// loop, unclipped) re-renders; taskbar_paint() (clipped damage repair, e.g.
// the cursor erase) only blits the cached strip — re-rendering there would
// put a new hover state on screen inside the repair rect only, and the full
// blit would never follow (the signature already matches): stale half-lit
// buttons. tb_layout() is the single source of geometry for drawing and for
// taskbar_hit(); it is logged as `[taskbar] btn` lines for headless tests.
#include "taskbar.h"
#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "serial.h"
#include "ui_draw.h"
#include "okai.h"
#include "rtc.h"
#include <stdint.h>

extern uint32_t tick_count;

#define TB_BTN_H   36
#define TB_BTN_MAX 220
#define TB_BTN_MIN 52
#define TB_GAP     4
#define TB_TRAY_W  220   // FPS chip + clock

#define TB_BG      0x000E0E12   // frosted tint over the wallpaper
#define TB_TEXT    0x00F2F2F5
#define TB_TEXT_2  0x009C9CA8
#define TB_RED     0x00E0283C

struct tb_lay {
    int y, by;
    int n, id[MAX_WINDOWS], bx[MAX_WINDOWS], bw;
};

static void tb_layout(struct tb_lay* L) {
    L->y = SCREEN_H - TASKBAR_H;
    L->by = L->y + (TASKBAR_H - TB_BTN_H) / 2;
    L->n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window* w = window_get(i);
        if (w && w->visible) L->id[L->n++] = i;
    }
    int x0 = 6, right = SCREEN_W - TB_TRAY_W;
    int bw = L->n ? (right - x0) / L->n - TB_GAP : TB_BTN_MAX;
    if (bw > TB_BTN_MAX) bw = TB_BTN_MAX;
    if (bw < TB_BTN_MIN) bw = TB_BTN_MIN;
    L->bw = bw;
    for (int k = 0; k < L->n; k++) L->bx[k] = x0 + k * (bw + TB_GAP);
}

int taskbar_hit(int mx, int my) {
    struct tb_lay L;
    tb_layout(&L);
    if (my < L.y || my >= SCREEN_H) return TB_HIT_NONE;
    for (int k = 0; k < L.n; k++)
        if (mx >= L.bx[k] && mx < L.bx[k] + L.bw) return L.id[k];
    return TB_HIT_NONE;
}

// ---- icons (20px box) -------------------------------------------------------

enum { APP_TERM, APP_WEB, APP_INFO, APP_DOC };

static int app_of(int id) {
    struct window* w = window_get(id);
    if (okai_find_by_win(id) >= 0) return APP_WEB;
    if (w && w->is_term) return APP_TERM;
    if (w && w->title[0] == 'S' && w->title[1] == 'y') return APP_INFO; // "System Info"
    return APP_DOC;
}

static void icon_app(struct wsurf* s, int app, int x, int y) {
    struct ui_pen p;
    if (app == APP_TERM) {
        ui_rrect(s, x, y + 1, 20, 18, 4, 0x005A5A64);
        ui_rrect(s, x + 1, y + 2, 18, 16, 3, 0x00141418);
        ui_pen_at(&p, x, y, 170);
        ui_path_begin();
        int32_t chev[6] = { UI_U(5), UI_U(6.5), UI_U(8.5), UI_U(10), UI_U(5), UI_U(13.5) };
        ui_polyline(&p, chev, 3);
        ui_seg(&p, UI_U(10), UI_U(14), UI_U(15), UI_U(14), 1);
        ui_path_fill(s, 0x0050E070);
    } else if (app == APP_WEB) {
        ui_rrect(s, x + 1, y + 1, 18, 18, 9, FX_FOCUS);
        ui_pen_at(&p, x, y, 150);
        ui_path_begin();
        ui_arc(&p, UI_U(10), UI_U(10), UI_U(3.4), UI_U(7.4), 0, 64);
        ui_seg(&p, UI_U(2.8), UI_U(10), UI_U(17.2), UI_U(10), 0);
        ui_seg(&p, UI_U(4), UI_U(6), UI_U(16), UI_U(6), 0);
        ui_seg(&p, UI_U(4), UI_U(14), UI_U(16), UI_U(14), 0);
        ui_path_fill(s, 0x00FFFFFF);
    } else if (app == APP_INFO) {
        ui_rrect(s, x + 1, y + 1, 18, 18, 9, 0x006A6A78);
        ui_rrect(s, x + 9, y + 9, 2, 6, 1, 0x00FFFFFF);
        ui_rrect(s, x + 9, y + 5, 2, 2, 1, 0x00FFFFFF);
    } else {
        ui_rrect(s, x + 3, y + 1, 14, 18, 2, 0x00E8E8EE);
        for (int k = 0; k < 4; k++)
            ui_rrect(s, x + 6, y + 5 + k * 3, k == 3 ? 5 : 8, 1, 0, 0x00808090);
    }
}

// ---- state + rendering ------------------------------------------------------

static uint32_t* strip;          // SCREEN_W x TASKBAR_H
static uint32_t strip_sig;
static int strip_valid;
static unsigned tb_fps;
static int clk_h = -1, clk_m, clk_y, clk_mo, clk_d;
static uint32_t clk_tick;
static uint32_t geo_sig;

static void clock_poll(void) {
    if (clk_h >= 0 && tick_count - clk_tick < 200) return; // 2s
    clk_tick = tick_count;
    x509_time t;
    if (rtc_read(&t) == 0) {
        clk_h = t.hour; clk_m = t.minute;
        clk_y = t.year; clk_mo = t.month; clk_d = t.day;
    } else if (clk_h < 0) {
        clk_h = 0; clk_m = 0; clk_y = 0;
    }
}

static int hover_of(const struct tb_lay* L, int mx, int my) {
    if (my < L->by || my >= L->by + TB_BTN_H) return -1;
    for (int k = 0; k < L->n; k++)
        if (mx >= L->bx[k] && mx < L->bx[k] + L->bw) return L->id[k];
    return -1;
}

static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static uint32_t tb_sig(const struct tb_lay* L, int hover) {
    uint32_t h = 2166136261u;
    for (int k = 0; k < L->n; k++) {
        struct window* w = window_get(L->id[k]);
        h = mix(h, (uint32_t)L->id[k]);
        h = mix(h, (uint32_t)(w->focused * 2 + w->minimized));
        h = mix(h, (uint32_t)app_of(L->id[k]));
        for (const char* t = w->title; *t; t++) h = mix(h, (uint8_t)*t);
    }
    h = mix(h, (uint32_t)hover);
    h = mix(h, tb_fps);
    h = mix(h, (uint32_t)(clk_h * 60 + clk_m));
    return h;
}

static void put2(char* b, int v) { b[0] = (char)('0' + v / 10 % 10); b[1] = (char)('0' + v % 10); }

static void render(const struct tb_lay* L, int hover) {
    struct wsurf s;
    s.px = strip; s.w = SCREEN_W; s.h = TASKBAR_H; s.stride = SCREEN_W;
    ws_reset_clip(&s);

    // Frosted background: the wallpaper under the bar, darkened.
    const uint32_t* wp = graphics_wallpaper();
    for (int yy = 0; yy < TASKBAR_H; yy++) {
        uint32_t* row = strip + yy * SCREEN_W;
        if (wp) {
            const uint32_t* src = wp + (L->y + yy) * SCREEN_W;
            for (int x = 0; x < SCREEN_W; x++) row[x] = ws_blend(src[x], TB_BG, 214);
        } else {
            for (int x = 0; x < SCREEN_W; x++) row[x] = TB_BG;
        }
    }
    for (int x = 0; x < SCREEN_W; x++) strip[x] = ws_blend(strip[x], 0xFFFFFF, 30); // top hairline

    int by = L->by - L->y;

    // window buttons
    for (int k = 0; k < L->n; k++) {
        int id = L->id[k];
        struct window* w = window_get(id);
        int bx = L->bx[k], bw = L->bw;
        int focused = w->focused && !w->minimized;
        if (focused) ui_rrect(&s, bx, by, bw, TB_BTN_H, 6, 0x26FFFFFFu);
        else if (hover == id) ui_rrect(&s, bx, by, bw, TB_BTN_H, 6, 0x16FFFFFFu);
        int ix = bw >= 90 ? bx + 10 : bx + (bw - 20) / 2;
        icon_app(&s, app_of(id), ix, by + (TB_BTN_H - 20) / 2 - 1);
        if (bw >= 90) {
            int tx = ix + 28, maxw = bx + bw - 10 - tx;
            uint32_t bg = strip[(by + TB_BTN_H / 2) * SCREEN_W + bx + bw - 4];
            ui_text_draw(&s, tx, by + TB_BTN_H / 2 - 1, 14, 0, w->minimized ? TB_TEXT_2 : TB_TEXT,
                         w->title, -1, maxw, bg);
        }
        int pw = focused ? 18 : 6;
        ui_rrect(&s, bx + (bw - pw) / 2, by + TB_BTN_H - 3, pw, 3, 1,
                 focused ? TB_RED : (w->minimized ? 0x00606068 : 0x009C9CA8));
    }

    // clock (right-aligned, two lines)
    int rx = SCREEN_W - 14;
    char tm[6], dt[11];
    put2(tm, clk_h); tm[2] = ':'; put2(tm + 3, clk_m); tm[5] = 0;
    int cw;
    if (clk_y) {
        put2(dt, clk_y / 100); put2(dt + 2, clk_y); dt[4] = '-';
        put2(dt + 5, clk_mo); dt[7] = '-'; put2(dt + 8, clk_d); dt[10] = 0;
        int tw = ui_text_w(14, 0, tm, -1), dw = ui_text_w(12, 0, dt, -1);
        cw = tw > dw ? tw : dw;
        ui_text_draw(&s, rx - tw, 13, 14, 0, TB_TEXT, tm, -1, tw + 2, TB_BG);
        ui_text_draw(&s, rx - dw, 31, 12, 0, TB_TEXT_2, dt, -1, dw + 2, TB_BG);
    } else {
        cw = ui_text_w(14, 0, tm, -1);
        ui_text_draw(&s, rx - cw, TASKBAR_H / 2, 14, 0, TB_TEXT, tm, -1, cw + 2, TB_BG);
    }

    // FPS chip
    char fb[12]; int fi = 0;
    char num[8]; int ni = 0; unsigned v = tb_fps;
    do { num[ni++] = (char)('0' + v % 10); v /= 10; } while (v && ni < 7);
    while (ni) fb[fi++] = num[--ni];
    fb[fi++] = ' '; fb[fi++] = 'F'; fb[fi++] = 'P'; fb[fi++] = 'S'; fb[fi] = 0;
    int fw = ui_text_w(12, 1, fb, -1) + 20;
    int fx = rx - cw - 18 - fw, fy = (TASKBAR_H - 24) / 2;
    ui_rrect(&s, fx, fy, fw, 24, 12, 0x1EFFFFFFu);
    ui_text_draw(&s, fx + 10, fy + 12, 12, 1, 0x00B8B8C4, fb, -1, fw, TB_BG);
}

static void log_geometry(const struct tb_lay* L) {
    uint32_t g = 2166136261u;
    for (int k = 0; k < L->n; k++) { g = mix(g, (uint32_t)L->id[k]); g = mix(g, (uint32_t)L->bx[k]); }
    g = mix(g, (uint32_t)L->bw);
    if (g == geo_sig) return;
    geo_sig = g;
    serial_printf("[taskbar] layout n=%d\n", L->n);
    for (int k = 0; k < L->n; k++)
        serial_printf("[taskbar] btn win=%d %d,%d,%d,%d\n", L->id[k], L->bx[k], L->by, L->bw, TB_BTN_H);
}

static int ensure(void) {
    if (!strip) strip = (uint32_t*)kmalloc(SCREEN_W * TASKBAR_H * sizeof(uint32_t));
    return strip != 0;
}

// Re-render if stale; returns 1 when the strip changed.
static int refresh(void) {
    if (!ensure()) return 0;
    struct tb_lay L;
    tb_layout(&L);
    clock_poll();
    int mx, my;
    mouse_get_position(&mx, &my);
    int hover = hover_of(&L, mx, my);
    uint32_t sig = tb_sig(&L, hover);
    if (strip_valid && sig == strip_sig) return 0;
    render(&L, hover);
    log_geometry(&L);
    strip_sig = sig; strip_valid = 1;
    return 1;
}

void taskbar_paint(void) {
    if (!strip_valid) refresh();
    if (strip) graphics_blit_pixels(0, SCREEN_H - TASKBAR_H, strip, SCREEN_W, TASKBAR_H, SCREEN_W);
}

int taskbar_update(unsigned fps) {
    tb_fps = fps;
    if (!refresh()) return 0;
    graphics_blit_pixels(0, SCREEN_H - TASKBAR_H, strip, SCREEN_W, TASKBAR_H, SCREEN_W);
    return 1;
}

void taskbar_invalidate(void) { strip_valid = 0; }
