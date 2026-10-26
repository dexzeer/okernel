// The Wi-Fi window; see wifigui.h.
#include "wifigui.h"
#include "cursor.h"
#include "devlog.h"
#include "memory.h"
#include "rtw/rtw.h"
#include "serial.h"
#include "string.h"
#include "ui_draw.h"
#include "window.h"

extern uint32_t tick_count;

#define WIN_W 620
#define WIN_H 700
#define MAXNET 48
#define M 24                         // outer margin
#define ROW_H 62
#define PW_H 104                     // extra height of the selected, secured row (password field + buttons)
#define OPEN_H 60                    // extra height of the selected open row (Connect button)
#define FOOT_H 44

// colours (0x00RRGGBB)
#define C_BG     0xF3F4F7
#define C_CARD   0xFFFFFF
#define C_LINE   0xE1E4EA
#define C_TEXT   0x1D2026
#define C_SUB    0x6B7280
#define C_ACCENT 0x2F6FED
#define C_ACC_D  0x2459C4
#define C_HOVER  0xEDF1F8
#define C_SEL    0xE4ECFC
#define C_GREEN  0x2E9E5B
#define C_RED    0xC8392B
#define C_GRAY   0xC9CED8
#define C_BARS_OFF 0xD3D7DF

struct rect { int x, y, w, h; };
enum { H_NONE, H_SCAN, H_DISC, H_FIELD, H_SHOW, H_CONNECT, H_CANCEL, H_POWER, H_ROW = 100 };

static struct {
    int win;
    uint32_t* px;
    int sw, sh, cap;
    int demo;
    struct rtw_ui_bss list[MAXNET];
    int n;
    struct rtw_ui_link link;
    int adapter;
    int sel;                          // selected row or -1
    char sel_ssid[33];
    char pw[72];
    int pwlen, show, short_pw;
    int scroll;
    int hover;
    int starting, start_result;
    uint32_t sig, last_poll;
    int auto_scanned;
    // geometry (surface coordinates), recomputed by layout()
    struct rect scan, card, disc, field, showb, connect, cancel, power, listv;
    struct rect rows[MAXNET];
    int content_h;
    int logged_sig;
} G = { .win = -1, .sel = -1, .hover = H_NONE };

int wifigui_win(void) { return G.win; }

// ---- state ----------------------------------------------------------------------------------------------------------------
static int row_extra(int i) {
    if (i != G.sel) return 0;
    return G.list[i].sec == 0 ? OPEN_H : G.list[i].sec == 3 ? PW_H : 40;
}

static void layout(void) {
    int W = G.sw, H = G.sh;
    G.scan = (struct rect){ W - M - 118, 24, 118, 38 };
    int y = 88;
    int show_card = G.link.state != RTW_LINK_IDLE && G.adapter == 2;
    G.card = (struct rect){ M, y, W - 2 * M, show_card ? 84 : 0 };
    G.disc = (struct rect){ W - M - 16 - 120, y + 24, 120, 36 };
    if (show_card) y += 84 + 16;
    G.listv = (struct rect){ M, y, W - 2 * M, H - FOOT_H - y };
    G.power = (struct rect){ W / 2 - 80, 250, 160, 42 };
    int ry = y - G.scroll;
    for (int i = 0; i < G.n; i++) {
        int h = ROW_H + row_extra(i);
        G.rows[i] = (struct rect){ M, ry, W - 2 * M, h };
        if (i == G.sel) {
            int by = ry + ROW_H;
            if (G.list[i].sec == 3) {
                G.field = (struct rect){ M + 60, by + 4, G.rows[i].w - 60 - 80 - 16, 38 };
                G.showb = (struct rect){ G.field.x + G.field.w + 8, by + 4, 64, 38 };
                G.connect = (struct rect){ M + 60, by + 52, 110, 38 };
                G.cancel = (struct rect){ M + 60 + 110 + 10, by + 52, 90, 38 };
            } else if (G.list[i].sec == 0) {
                G.field = (struct rect){ 0, 0, 0, 0 }; G.showb = G.field;
                G.connect = (struct rect){ M + 60, by + 4, 110, 38 };
                G.cancel = (struct rect){ M + 60 + 110 + 10, by + 4, 90, 38 };
            } else {
                G.field = (struct rect){ 0, 0, 0, 0 }; G.showb = G.field; G.connect = G.field; G.cancel = G.field;
            }
        }
        ry += h + 6;
    }
    G.content_h = ry + G.scroll - G.listv.y;
    if (G.sel < 0) { G.field = (struct rect){ 0, 0, 0, 0 }; G.showb = G.field; G.connect = G.field; G.cancel = G.field; }
}

static int in_rect(const struct rect* r, int x, int y) { return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

static int hit_at(int x, int y) {
    if (G.adapter == 1) return in_rect(&G.power, x, y) ? H_POWER : H_NONE;
    if (G.adapter != 2) return H_NONE;
    if (in_rect(&G.scan, x, y)) return H_SCAN;
    if (G.card.h && in_rect(&G.disc, x, y)) return H_DISC;
    if (G.sel >= 0) {
        if (in_rect(&G.connect, x, y)) return H_CONNECT;
        if (in_rect(&G.cancel, x, y)) return H_CANCEL;
        if (in_rect(&G.showb, x, y)) return H_SHOW;
        if (in_rect(&G.field, x, y)) return H_FIELD;
    }
    if (y >= G.listv.y && y < G.listv.y + G.listv.h)
        for (int i = 0; i < G.n; i++)
            if (in_rect(&G.rows[i], x, y)) return H_ROW + i;
    return H_NONE;
}

// ---- drawing -----------------------------------------------------------------------------------------------------------------
static struct wsurf S;

static void fill(int x, int y, int w, int h, uint32_t c) { ws_fill_rect(&S, x, y, w, h, 0xFF000000u | c); }
static void rr(int x, int y, int w, int h, int r, uint32_t c) { ui_rrect(&S, x, y, w, h, r, c); }
static int text(int x, int cy, int px, int bold, uint32_t c, const char* s, int maxw, uint32_t bg) { return ui_text_draw(&S, x, cy, px, bold, c, s, -1, maxw, bg); }

static void button(const struct rect* r, const char* label, int primary, int enabled, int hover) {
    uint32_t bg, fg;
    if (!enabled) { bg = 0xE3E6EC; fg = 0x9AA0AB; }
    else if (primary) { bg = hover ? C_ACC_D : C_ACCENT; fg = 0xFFFFFF; }
    else { bg = hover ? 0xE9ECF2 : 0xFFFFFF; fg = C_TEXT; }
    if (!primary && enabled) rr(r->x, r->y, r->w, r->h, 8, C_LINE);
    if (!primary && enabled) rr(r->x + 1, r->y + 1, r->w - 2, r->h - 2, 7, bg);
    else rr(r->x, r->y, r->w, r->h, 8, bg);
    int tw = ui_text_w(15, 1, label, -1);
    text(r->x + (r->w - tw) / 2, r->y + r->h / 2, 15, 1, fg, label, r->w, bg);
}

static int bars_for(int rssi) { return rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0; }

static void icon_signal(int x, int y, int bars, int dim) {
    for (int i = 0; i < 4; i++) {
        int h = 7 + i * 5;
        rr(x + i * 8, y + 22 - h, 5, h, 2, i < bars ? (dim ? 0x9DB5EF : C_ACCENT) : C_BARS_OFF);
    }
}

static void icon_lock(int x, int y, uint32_t c) {
    struct ui_pen p; ui_pen_at(&p, x, y, 190);
    ui_path_begin();
    int32_t pts[12] = { UI_U(3.5), UI_U(9), UI_U(3.5), UI_U(5.5), UI_U(5), UI_U(3), UI_U(9), UI_U(3), UI_U(10.5), UI_U(5.5), UI_U(10.5), UI_U(9) };
    ui_polyline(&p, pts, 6);
    ui_path_fill(&S, c);
    rr(x, y + 8, 14, 11, 3, c);
}

static void icon_spinner(int x, int y, int phase) {
    struct ui_pen p; ui_pen_at(&p, x, y, 220);
    ui_path_begin();
    ui_arc(&p, UI_U(10), UI_U(10), UI_U(8), UI_U(8), 0, 64);
    ui_path_fill(&S, 0x30000000u | C_ACCENT);
    ui_path_begin();
    ui_arc(&p, UI_U(10), UI_U(10), UI_U(8), UI_U(8), phase * 4, 20);
    ui_path_fill(&S, C_ACCENT);
}

static const char* sec_name(int sec) { return sec == 0 ? "open" : sec == 1 ? "wep" : sec == 2 ? "wpa" : sec == 3 ? "wpa2" : "wpa3 / enterprise"; }

static int band_5g(int ch) { return ch > 14; }

static void itoa_(char* o, int v) {
    char t[12]; int n = 0, neg = v < 0;
    if (neg) v = -v;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    int k = 0;
    if (neg) o[k++] = '-';
    while (n) o[k++] = t[--n];
    o[k] = 0;
}

static void cat(char* d, const char* s, int cap) { int n = (int)strlen(d); for (int i = 0; s[i] && n < cap - 1; i++) d[n++] = s[i]; d[n] = 0; }

static void draw(void) {
    int W = G.sw, H = G.sh;
    S.px = G.px; S.w = W; S.h = H; S.stride = W; ws_reset_clip(&S);
    fill(0, 0, W, H, C_BG);
    int phase = (int)((tick_count / 6) & 15);

    // header
    text(M, 38, 26, 1, C_TEXT, "wi-fi", 300, C_BG);
    char name[80];
    rtw_ui_adapter_name(name, sizeof name);
    text(M, 66, 13, 0, C_SUB, name, W - 2 * M - 140, C_BG);

    if (G.adapter == 0) {
        text(M, 230, 17, 1, C_TEXT, "no wi-fi adapter found", W - 2 * M, C_BG);
        text(M, 262, 14, 0, C_SUB, "plug in a usb wi-fi adapter (realtek rtl8811cu / 8821cu).", W - 2 * M, C_BG);
        text(M, 284, 14, 0, C_SUB, "it is detected automatically.", W - 2 * M, C_BG);
        return;
    }
    if (G.adapter == 1) {
        text(M, 200, 17, 1, C_TEXT, G.starting ? "starting the adapter..." : "wi-fi adapter found", W - 2 * M, C_BG);
        text(M, 232, 14, 0, C_SUB, G.starting ? "this takes a few seconds." : "turn it on to look for networks.", W - 2 * M, C_BG);
        if (!G.starting) button(&G.power, "turn on", 1, 1, G.hover == H_POWER);
        if (G.start_result) text(M, 330, 14, 0, C_RED, "the adapter did not start (see `wifi log`).", W - 2 * M, C_BG);
        return;
    }

    int scanning = rtw_ui_scanning();
    int can_scan = !scanning && G.link.state != RTW_LINK_BUSY && G.link.state != RTW_LINK_UP;
    button(&G.scan, scanning ? "scanning..." : "scan", 1, can_scan, G.hover == H_SCAN && can_scan);

    // connection card
    if (G.card.h) {
        const struct rect* c = &G.card;
        rr(c->x, c->y, c->w, c->h, 12, C_LINE);
        rr(c->x + 1, c->y + 1, c->w - 2, c->h - 2, 11, C_CARD);
        char line[96] = "";
        const char* act = "disconnect";
        if (G.link.state == RTW_LINK_UP) {
            rr(c->x + 18, c->y + 30, 18, 18, 9, C_GREEN);
            cat(line, "connected to ", sizeof line); cat(line, G.link.ssid, sizeof line);
            text(c->x + 52, c->y + 28, 17, 1, C_TEXT, line, c->w - 52 - 150, C_CARD);
            char d[100] = "address "; cat(d, G.link.ip, sizeof d); cat(d, "   gateway ", sizeof d); cat(d, G.link.gateway, sizeof d);
            text(c->x + 52, c->y + 56, 13, 0, C_SUB, d, c->w - 52 - 150, C_CARD);
        } else if (G.link.state == RTW_LINK_BUSY) {
            icon_spinner(c->x + 14, c->y + 26, phase);
            cat(line, "connecting to ", sizeof line); cat(line, G.link.ssid, sizeof line);
            text(c->x + 52, c->y + 28, 17, 1, C_TEXT, line, c->w - 52 - 150, C_CARD);
            text(c->x + 52, c->y + 56, 13, 0, C_SUB, G.link.msg, c->w - 52 - 150, C_CARD);
            act = "cancel";
        } else {
            rr(c->x + 18, c->y + 30, 18, 18, 9, C_RED);
            cat(line, "could not connect to ", sizeof line); cat(line, G.link.ssid, sizeof line);
            text(c->x + 52, c->y + 28, 17, 1, C_TEXT, line, c->w - 52 - 150, C_CARD);
            text(c->x + 52, c->y + 56, 13, 0, C_RED, G.link.msg, c->w - 52 - 150, C_CARD);
            act = "dismiss";
        }
        button(&G.disc, act, 0, 1, G.hover == H_DISC);
    }

    // network list
    struct rect lv = G.listv;
    if (G.n == 0) {
        text(M, lv.y + 40, 16, 1, C_TEXT, scanning ? "looking for networks..." : "no networks yet", W - 2 * M, C_BG);
        text(M, lv.y + 68, 13, 0, C_SUB, scanning ? "this takes a few seconds." : "press scan to look again.", W - 2 * M, C_BG);
        if (scanning) icon_spinner(W - M - 40, lv.y + 28, phase);
    }
    int old[4];
    ws_push_clip(&S, lv.x, lv.y, lv.x + lv.w, lv.y + lv.h, old);
    for (int i = 0; i < G.n; i++) {
        const struct rect* r = &G.rows[i];
        if (r->y + r->h < lv.y || r->y > lv.y + lv.h) continue;
        const struct rtw_ui_bss* b = &G.list[i];
        int connected = G.link.state == RTW_LINK_UP && !strcmp(G.link.ssid, b->ssid);
        uint32_t bg = i == G.sel ? C_SEL : G.hover == H_ROW + i ? C_HOVER : C_CARD;
        rr(r->x, r->y, r->w, r->h, 12, C_LINE);
        rr(r->x + 1, r->y + 1, r->w - 2, r->h - 2, 11, bg);
        icon_signal(r->x + 20, r->y + 18, bars_for(b->rssi), b->sec == 4);
        text(r->x + 60, r->y + 22, 16, 1, b->sec == 4 ? C_SUB : C_TEXT, b->ssid, r->w - 60 - 120, bg);
        char sub[96] = "";
        cat(sub, sec_name(b->sec), sizeof sub); cat(sub, ", channel ", sizeof sub);
        char t[12]; itoa_(t, b->ch); cat(sub, t, sizeof sub);
        cat(sub, band_5g(b->ch) ? " (5 ghz), " : " (2.4 ghz), ", sizeof sub);
        itoa_(t, b->rssi); cat(sub, t, sizeof sub); cat(sub, " dbm", sizeof sub);
        if (b->sec == 4) cat(sub, " - not supported", sizeof sub);
        text(r->x + 60, r->y + 44, 12, 0, C_SUB, sub, r->w - 60 - 20, bg);
        if (connected) text(r->x + r->w - 110, r->y + 30, 13, 1, C_GREEN, "connected", 100, bg);
        else if (b->sec != 0) icon_lock(r->x + r->w - 36, r->y + 22, 0x8A909C);
        if (i == G.sel) {
            int by = r->y + ROW_H;
            if (b->sec == 3) {
                rr(G.field.x, G.field.y, G.field.w, G.field.h, 8, C_ACCENT);
                rr(G.field.x + 2, G.field.y + 2, G.field.w - 4, G.field.h - 4, 6, 0xFFFFFF);
                int tx = G.field.x + 14, cy = G.field.y + G.field.h / 2;
                if (G.pwlen == 0) text(tx, cy, 14, 0, 0x9AA0AB, "password", G.field.w - 28, 0xFFFFFF);
                else if (G.show) tx += text(tx, cy, 15, 0, C_TEXT, G.pw, G.field.w - 40, 0xFFFFFF);
                else for (int k = 0; k < G.pwlen && tx < G.field.x + G.field.w - 24; k++, tx += 12) rr(tx, cy - 4, 9, 9, 4, C_TEXT);
                if (((tick_count / 50) & 1) == 0) fill(tx + 1, cy - 11, 2, 22, C_ACCENT);
                button(&G.showb, G.show ? "hide" : "show", 0, 1, G.hover == H_SHOW);
                if (G.short_pw) text(M + 60 + 220, G.connect.y + G.connect.h / 2, 12, 0, C_RED, "use 8 to 63 characters", 200, C_SEL);
            }
            if (b->sec == 3 || b->sec == 0) {
                int ok = b->sec == 0 || G.pwlen >= 8;
                button(&G.connect, "connect", 1, ok, G.hover == H_CONNECT && ok);
                button(&G.cancel, "cancel", 0, 1, G.hover == H_CANCEL);
            } else {
                text(M + 60, by + 20, 13, 0, C_SUB, "this kind of network can't be joined from here yet.", r->w - 80, C_SEL);
            }
        }
    }
    ws_pop_clip(&S, old);

    // scrollbar when the list is taller than the window
    if (G.content_h > lv.h && lv.h > 0) {
        int th = lv.h * lv.h / G.content_h;
        if (th < 36) th = 36;
        int maxs = G.content_h - lv.h;
        int ty = lv.y + (lv.h - th) * G.scroll / (maxs > 0 ? maxs : 1);
        rr(W - 12, ty, 6, th, 3, 0xB8BEC9);
    }

    // footer
    fill(0, H - FOOT_H, W, FOOT_H, C_BG);
    text(M, H - FOOT_H / 2, 12, 0, C_SUB, "click a network, type its password, press enter.   esc cancels.", W - 2 * M, C_BG);
}

// ---- keeping the picture current ---------------------------------------------------------------------------------------------------
static void log_geometry(void) {
    struct window* w = window_get(G.win);
    if (!w) return;
    int ox = w->x + WIN_BORDER, oy = w->y + WIN_BORDER + WIN_TITLE_H;
    uint32_t sig = (uint32_t)(ox * 31 + oy) ^ ((uint32_t)G.n << 20) ^ (uint32_t)(G.sel + 3) << 12 ^ (uint32_t)G.card.h << 4 ^ (uint32_t)G.scroll ^ (uint32_t)G.adapter << 28;
    if ((int)sig == G.logged_sig) return;
    G.logged_sig = (int)sig;
    serial_printf("[wifigui] ui scan=%d,%d,%d,%d\n", ox + G.scan.x, oy + G.scan.y, G.scan.w, G.scan.h);
    serial_printf("[wifigui] ui power=%d,%d,%d,%d\n", ox + G.power.x, oy + G.power.y, G.power.w, G.power.h);
    if (G.card.h) serial_printf("[wifigui] ui disconnect=%d,%d,%d,%d\n", ox + G.disc.x, oy + G.disc.y, G.disc.w, G.disc.h);
    if (G.sel >= 0) {
        serial_printf("[wifigui] ui field=%d,%d,%d,%d\n", ox + G.field.x, oy + G.field.y, G.field.w, G.field.h);
        serial_printf("[wifigui] ui connect=%d,%d,%d,%d\n", ox + G.connect.x, oy + G.connect.y, G.connect.w, G.connect.h);
        serial_printf("[wifigui] ui cancel=%d,%d,%d,%d\n", ox + G.cancel.x, oy + G.cancel.y, G.cancel.w, G.cancel.h);
    }
    for (int i = 0; i < G.n; i++)
        serial_printf("[wifigui] row %d %d,%d,%d,%d %s\n", i, ox + G.rows[i].x, oy + G.rows[i].y, G.rows[i].w, G.rows[i].h, G.list[i].ssid);
}

static void realloc_surface(int w, int h) {
    if (w < 200 || h < 200) return;
    if (w * h > G.cap) {
        if (G.px) kfree(G.px);
        G.px = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4);
        G.cap = G.px ? w * h : 0;
    }
    G.sw = w; G.sh = h;
}

static void refresh(int force) {
    struct window* w = window_get(G.win);
    if (!w) return;
    int cw = w->w - 2 * WIN_BORDER, ch = w->h - WIN_TITLE_H - 2 * WIN_BORDER;
    if (cw != G.sw || ch != G.sh) { realloc_surface(cw, ch); force = 1; }
    if (!G.px) return;

    G.adapter = rtw_ui_adapter();
    struct rtw_ui_bss prev_sel = G.sel >= 0 && G.sel < G.n ? G.list[G.sel] : (struct rtw_ui_bss){ "", 0, 0, 0 };
    int n = rtw_ui_list(G.list, MAXNET);
    G.n = n;
    if (G.sel >= 0) {                                      // the selection follows the network name when the list changes
        int f = -1;
        for (int i = 0; i < n; i++) if (!strcmp(G.list[i].ssid, G.sel_ssid)) f = i;
        G.sel = f;
        (void)prev_sel;
        if (f < 0) { G.pwlen = 0; G.pw[0] = 0; }
    }
    rtw_ui_link_info(&G.link);

    // pointer hover (only when the pointer is over this window)
    int mx = mouse_get_x(), my = mouse_get_y();
    int ox = w->x + WIN_BORDER, oy = w->y + WIN_BORDER + WIN_TITLE_H;
    int over = window_from_point(mx, my) == G.win;
    int maxs = G.content_h - G.listv.h; if (maxs < 0) maxs = 0;
    if (G.scroll > maxs) G.scroll = maxs;
    layout();
    G.hover = over ? hit_at(mx - ox, my - oy) : H_NONE;

    int phase = (G.link.state == RTW_LINK_BUSY || rtw_ui_scanning()) ? (int)(tick_count / 6) : 0;
    int caret = G.sel >= 0 ? (int)((tick_count / 50) & 1) : 0;
    uint32_t sig = 2166136261u;
#define MIX(v) sig = (sig ^ (uint32_t)(v)) * 16777619u
    MIX(G.adapter); MIX(G.n); MIX(G.sel); MIX(G.scroll); MIX(G.hover); MIX(G.pwlen); MIX(G.show); MIX(G.short_pw); MIX(G.starting);
    MIX(G.link.state); MIX(phase); MIX(caret); MIX(rtw_ui_scanning()); MIX(G.start_result);
    for (int i = 0; i < n; i++) { MIX(G.list[i].rssi); MIX(G.list[i].ssid[0]); MIX(G.list[i].ssid[1]); }
    for (int i = 0; G.link.msg[i]; i++) MIX(G.link.msg[i]);
    for (int i = 0; G.link.ssid[i]; i++) MIX(G.link.ssid[i]);
    for (int i = 0; G.link.ip[i]; i++) MIX(G.link.ip[i]);
    if (G.show) for (int i = 0; i < G.pwlen; i++) MIX(G.pw[i]);
#undef MIX
    static int last_state = -1;
    if (G.link.state != last_state) {
        last_state = G.link.state;
        serial_printf("[wifigui] state %s ssid=%s ip=%s msg=%s\n", G.link.state == RTW_LINK_UP ? "up" : G.link.state == RTW_LINK_BUSY ? "busy" : G.link.state == RTW_LINK_FAILED ? "failed" : "idle",
                      G.link.ssid, G.link.ip, G.link.msg);
    }
    if (!force && sig == G.sig) return;
    G.sig = sig;
    draw();
    window_set_pixels(G.win, G.px, 0, 0, G.sw, G.sh, G.sw);
    log_geometry();
}

// ---- entry points --------------------------------------------------------------------------------------------------------------------
void wifigui_open(int demo) {
    if (G.win >= 0) { window_set_focus(G.win); window_raise(G.win); return; }
    if (demo) rtw_ui_demo(1);
    G.demo = demo;
    int x = (SCREEN_W - WIN_W) / 2, y = (SCREEN_H - WIN_H - TASKBAR_H) / 2;
    if (y < 8) y = 8;
    if (x < 0) x = 0;
    G.win = window_create("wi-fi", x, y, WIN_W, WIN_H);
    if (G.win < 0) return;
    window_set_close_button(G.win, 1);
    window_set_minimize_button(G.win, 1);
    window_set_hide_cursor(G.win, 1);
    G.sel = -1; G.pwlen = 0; G.pw[0] = 0; G.show = 0; G.scroll = 0; G.starting = 0; G.start_result = 0; G.auto_scanned = 0; G.logged_sig = 0; G.sig = 0;
    G.sw = G.sh = 0;
    window_set_focus(G.win);
    serial_printf("[wifigui] open%s\n", demo ? " (demo)" : "");
    refresh(1);
}

void wifigui_close(void) {
    if (G.win < 0) return;
    window_set_pixels(G.win, 0, 0, 0, 0, 0, 0);
    window_destroy(G.win);
    G.win = -1;
    if (G.px) { kfree(G.px); G.px = 0; G.cap = 0; }
    serial_printf("[wifigui] closed\n");
}

static void do_connect(void) {
    if (G.sel < 0 || G.sel >= G.n) return;
    const struct rtw_ui_bss* b = &G.list[G.sel];
    if (b->sec == 3 && (G.pwlen < 8 || G.pwlen > 63)) { G.short_pw = 1; return; }
    serial_printf("[wifigui] connect %s\n", b->ssid);
    rtw_ui_connect(b->ssid, b->sec == 0 ? "" : G.pw);
    G.sel = -1; G.pwlen = 0; G.pw[0] = 0; G.show = 0; G.short_pw = 0;
}

static void select_row(int i) {
    if (G.sel == i) { G.sel = -1; return; }
    G.sel = i;
    strncpy(G.sel_ssid, G.list[i].ssid, 32); G.sel_ssid[32] = 0;
    G.pwlen = 0; G.pw[0] = 0; G.show = 0; G.short_pw = 0;
    serial_printf("[wifigui] select %s\n", G.sel_ssid);
}

void wifigui_click(int x, int y) {
    if (G.win < 0) return;
    layout();
    int h = hit_at(x, y);
    switch (h) {
    case H_POWER: G.starting = 1; G.start_result = 0; break;
    case H_SCAN: serial_printf("[wifigui] scan\n"); rtw_ui_scan(); break;
    case H_DISC: serial_printf("[wifigui] disconnect\n"); rtw_ui_disconnect(); break;
    case H_SHOW: G.show = !G.show; break;
    case H_CONNECT: do_connect(); break;
    case H_CANCEL: G.sel = -1; G.pwlen = 0; G.pw[0] = 0; break;
    case H_FIELD: break;
    case H_NONE: break;
    default: if (h >= H_ROW) select_row(h - H_ROW); break;
    }
    G.sig = 0;
    refresh(1);
}

void wifigui_key(char c) {
    if (G.win < 0) return;
    if (G.sel >= 0 && G.sel < G.n) {
        const struct rtw_ui_bss* b = &G.list[G.sel];
        if (c == 27) { G.sel = -1; G.pwlen = 0; G.pw[0] = 0; }
        else if (c == '\n' || c == '\r') do_connect();
        else if (b->sec == 3) {
            if (c == '\b') { if (G.pwlen) G.pw[--G.pwlen] = 0; G.short_pw = 0; }
            else if (c == '\t') G.show = !G.show;
            else if ((unsigned char)c >= 32 && (unsigned char)c < 127 && G.pwlen < 63) { G.pw[G.pwlen++] = c; G.pw[G.pwlen] = 0; G.short_pw = 0; }
        }
    }
    G.sig = 0;
    refresh(1);
}

void wifigui_wheel(int notches) {
    if (G.win < 0) return;
    serial_printf("[wifigui] wheel %d\n", notches);
    G.scroll += notches * 48;
    if (G.scroll < 0) G.scroll = 0;
    G.sig = 0;
    refresh(1);
}

void wifigui_poll(void) {
    if (G.win < 0) return;
    struct window* w = window_get(G.win);
    if (!w || !w->visible) return;
    if (G.starting && !G.start_result) {                                  // the "starting" picture has been shown: do the slow part now
        static int shown;
        if (!shown) { shown = 1; refresh(1); return; }
        shown = 0;
        int r = rtw_ui_start();
        G.starting = 0;
        G.start_result = r ? r : 0;
        serial_printf("[wifigui] adapter start result %d\n", r);
        if (!r) { rtw_ui_scan(); G.auto_scanned = 1; }
        refresh(1);
        return;
    }
    if ((int)(tick_count - G.last_poll) < 4) return;                      // ~25 Hz
    G.last_poll = tick_count;
    if (rtw_ui_adapter() == 2 && !G.auto_scanned) { G.auto_scanned = 1; rtw_ui_scan(); }
    refresh(0);
}
