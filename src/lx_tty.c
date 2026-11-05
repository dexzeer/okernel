// Terminals of the Linux subsystem: the standard files of a program that runs in a terminal window. Output goes to the
// window (and the serial log); keys typed into the window arrive through lx_tty_key() and are line-edited here
// (canonical mode) or passed through (raw mode) according to the termios the program set.
#include "lx.h"
#include "serial.h"
#include "string.h"
#include "window.h"
#include "graphics.h"
#include "keyboard.h"

#define TTY_MAX 8
#define VT_PARS 16
#define TTY_Q 4096

#define ICANON 0000002
#define ECHO 0000010
#define ISIG 0000001
#define ICRNL 0000400

struct lx_tty {
    int win;                                 // terminal window id, -1 = unused
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[19];
    uint8_t q[TTY_Q];                        // bytes a read can take
    uint32_t qh, qt;
    uint8_t line[512];                       // the line being typed (canonical mode)
    int llen;
    int eof;                                 // ctrl-D on an empty line: the next read returns 0
    // the output parser (see below)
    int st, npar, par[VT_PARS], priv, osc, osc_num;
    int u_need; uint32_t u_cp;               // UTF-8 sequence in progress
    uint32_t fg, bg; int fg_set, bg_set;     // not set = the window's own colours
    uint8_t attr; int inv;
    int top, bot;                            // scroll region (bot < 0: to the last row)
    int wrap, nowrap, irm, app_cursor;       // wrap: the next character goes on a new line
    int s_x, s_y, s_fg_set, s_bg_set, s_inv; uint32_t s_fg, s_bg; uint8_t s_attr;   // saved cursor
    void *alt;                               // alternate screen active: the saved main screen
    int alt_x, alt_y;
};

static void vt_defaults(struct lx_tty *t);

static struct lx_tty ttys[TTY_MAX];
static int inited;

extern int lx_tty_owner(int win, struct lx_task **out);
extern void lx_signal_fg(int win, int sig);

static void tty_defaults(struct lx_tty *t) {
    t->iflag = 0x500;                        // ICRNL | IXON
    t->oflag = 5;                            // OPOST | ONLCR
    t->cflag = 0xBF;                         // B38400 | CS8 | CREAD
    t->lflag = 0x8A3B;                       // ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN
    memset(t->cc, 0, sizeof t->cc);
    t->cc[0] = 3; t->cc[1] = 28; t->cc[2] = 127; t->cc[3] = 21; t->cc[4] = 4; t->cc[5] = 0; t->cc[6] = 1;
    t->cc[8] = 17; t->cc[9] = 19; t->cc[10] = 26; t->cc[12] = 18; t->cc[13] = 15; t->cc[14] = 23; t->cc[15] = 22;
    t->qh = t->qt = 0; t->llen = 0; t->eof = 0;
    vt_defaults(t);
}

static struct lx_tty *tty_for(int win, int create) {
    if (!inited) { for (int i = 0; i < TTY_MAX; i++) ttys[i].win = -1; inited = 1; }
    for (int i = 0; i < TTY_MAX; i++) if (ttys[i].win == win) return &ttys[i];
    if (!create) return 0;
    for (int i = 0; i < TTY_MAX; i++)
        if (ttys[i].win < 0) { ttys[i].alt = 0; tty_defaults(&ttys[i]); ttys[i].win = win; return &ttys[i]; }
    return 0;
}

static void q_put(struct lx_tty *t, uint8_t c) {
    uint32_t n = (t->qt + 1) % TTY_Q;
    if (n == t->qh) return;                  // full: drop
    t->q[t->qt] = c; t->qt = n;
}

// ---- terminal emulation: what TERM=xterm promises ---------------------------------------------------------------------
// Everything a program writes goes through this parser into the window's cell grid. The cursor is the window's own, so
// the shell carries on where a program stopped. Colours and the scroll region are per terminal; lx_tty_release puts it
// all back when the last program of the window exits.

static const uint32_t vt_palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

static uint32_t vt_256(int n) {
    if (n < 0) n = 0;
    if (n < 16) return vt_palette[n];
    if (n < 232) {
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return ((uint32_t)lv[n / 36] << 16) | ((uint32_t)lv[(n / 6) % 6] << 8) | lv[n % 6];
    }
    if (n > 255) n = 255;
    uint32_t g = (uint32_t)(8 + (n - 232) * 10);
    return (g << 16) | (g << 8) | g;
}

static void vt_defaults(struct lx_tty *t) {
    t->st = 0; t->npar = 0; t->priv = 0; t->u_need = 0;
    t->fg_set = t->bg_set = 0; t->attr = 0; t->inv = 0;
    t->top = 0; t->bot = -1; t->wrap = 0; t->irm = 0; t->app_cursor = 0; t->nowrap = 0;
    t->s_x = t->s_y = 0; t->s_fg_set = t->s_bg_set = 0; t->s_attr = 0; t->s_inv = 0;
}

static uint32_t vt_cfg(struct lx_tty *t, struct window *w) { return t->inv ? (t->bg_set ? t->bg : w->text_bg_rgb) : (t->fg_set ? t->fg : w->text_fg_rgb); }
static uint32_t vt_cbg(struct lx_tty *t, struct window *w) { return t->inv ? (t->fg_set ? t->fg : w->text_fg_rgb) : (t->bg_set ? t->bg : w->text_bg_rgb); }
static int vt_bot(struct lx_tty *t, struct window *w) { return t->bot >= 0 && t->bot < w->content_h ? t->bot : w->content_h - 1; }

static void vt_scroll(struct lx_tty *t, struct window *w, int win, int top, int bot, int n) {
    // Lines leaving a full-height scroll on the main screen are history; the alternate screen keeps none.
    window_term_scroll(win, top, bot, n, vt_cfg(t, w), vt_cbg(t, w), !t->alt && top == 0 && bot == w->content_h - 1);
}

static void vt_lf(struct lx_tty *t, struct window *w, int win) {
    int bot = vt_bot(t, w);
    if (w->cursor_y == bot) vt_scroll(t, w, win, t->top, bot, 1);
    else if (w->cursor_y < w->content_h - 1) window_term_move(win, w->cursor_y + 1, w->cursor_x);
}

static void vt_glyph(struct lx_tty *t, struct window *w, int win, char ch) {
    if (t->wrap) {
        t->wrap = 0;
        window_term_move(win, w->cursor_y, 0);
        vt_lf(t, w, win);
    }
    int x = w->cursor_x, y = w->cursor_y;
    if (t->irm) window_term_shift(win, y, x, 1, vt_cfg(t, w), vt_cbg(t, w));
    window_write_cell_rgb(win, y, x, ch, vt_cfg(t, w), vt_cbg(t, w));
    if (t->attr) window_write_cell_attr(win, y, x, t->attr);
    if (x >= w->content_w - 1) t->wrap = !t->nowrap;
    else window_term_move(win, y, x + 1);
}

// The font behind the cells is single-byte: what line drawing and bullets look like in ASCII, '?' for the rest.
static char vt_ascii(uint32_t cp) {
    if (cp >= 0x2500 && cp <= 0x257F) {
        if (cp <= 0x2503 || (cp >= 0x2504 && cp <= 0x250B) || (cp >= 0x2550 && cp <= 0x2551)) return (cp == 0x2502 || cp == 0x2503 || cp == 0x2551) ? '|' : '-';
        return '+';
    }
    if (cp >= 0x2580 && cp <= 0x259F) return '#';
    if (cp == 0x2022 || cp == 0xB7 || cp == 0x2219) return '.';
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    if (cp == 0x2026) return '.';
    return '?';
}

static void vt_sgr(struct lx_tty *t) {
    if (t->npar == 0) { t->npar = 1; t->par[0] = 0; }
    for (int i = 0; i < t->npar; i++) {
        int p = t->par[i];
        if (p < 0) p = 0;
        if (p == 0) { t->fg_set = t->bg_set = 0; t->attr = 0; t->inv = 0; }
        else if (p == 1) t->attr |= CELL_BOLD;
        else if (p == 4) t->attr |= CELL_UL;
        else if (p == 7) t->inv = 1;
        else if (p == 22) t->attr &= (uint8_t)~CELL_BOLD;
        else if (p == 24) t->attr &= (uint8_t)~CELL_UL;
        else if (p == 27) t->inv = 0;
        else if (p >= 30 && p <= 37) { t->fg = vt_palette[p - 30]; t->fg_set = 1; }
        else if (p == 39) t->fg_set = 0;
        else if (p >= 40 && p <= 47) { t->bg = vt_palette[p - 40]; t->bg_set = 1; }
        else if (p == 49) t->bg_set = 0;
        else if (p >= 90 && p <= 97) { t->fg = vt_palette[p - 90 + 8]; t->fg_set = 1; }
        else if (p >= 100 && p <= 107) { t->bg = vt_palette[p - 100 + 8]; t->bg_set = 1; }
        else if ((p == 38 || p == 48) && i + 2 < t->npar && t->par[i + 1] == 5) {
            uint32_t c = vt_256(t->par[i + 2]);
            if (p == 38) { t->fg = c; t->fg_set = 1; } else { t->bg = c; t->bg_set = 1; }
            i += 2;
        } else if ((p == 38 || p == 48) && i + 4 < t->npar && t->par[i + 1] == 2) {
            uint32_t c = ((uint32_t)(t->par[i + 2] & 255) << 16) | ((uint32_t)(t->par[i + 3] & 255) << 8) | (uint32_t)(t->par[i + 4] & 255);
            if (p == 38) { t->fg = c; t->fg_set = 1; } else { t->bg = c; t->bg_set = 1; }
            i += 4;
        }
    }
}

static void vt_reply(struct lx_tty *t, const char *s) { while (*s) q_put(t, (uint8_t)*s++); }

static void vt_reply_num(struct lx_tty *t, int v) {
    char b[12];
    int n = 0;
    if (v <= 0) b[n++] = '0';
    while (v > 0) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) q_put(t, (uint8_t)b[--n]);
}

static void vt_clear_all(struct lx_tty *t, struct window *w, int win) {
    for (int r = 0; r < w->content_h; r++) window_term_fill(win, r, 0, w->content_w, vt_cfg(t, w), vt_cbg(t, w));
}

static void vt_save(struct lx_tty *t, struct window *w) {
    t->s_x = w->cursor_x; t->s_y = w->cursor_y; t->s_fg = t->fg; t->s_bg = t->bg;
    t->s_fg_set = t->fg_set; t->s_bg_set = t->bg_set; t->s_attr = t->attr; t->s_inv = t->inv;
}

static void vt_restore(struct lx_tty *t, int win) {
    t->fg = t->s_fg; t->bg = t->s_bg; t->fg_set = t->s_fg_set; t->bg_set = t->s_bg_set; t->attr = t->s_attr; t->inv = t->s_inv;
    t->wrap = 0;
    window_term_move(win, t->s_y, t->s_x);
}

static void vt_alt(struct lx_tty *t, struct window *w, int win, int on) {
    if (on && !t->alt) {
        t->alt = window_term_save(win);
        if (!t->alt) return;                 // no memory: stay on the main screen, the program still works
        t->alt_x = w->cursor_x; t->alt_y = w->cursor_y;
        vt_clear_all(t, w, win);
        window_term_move(win, 0, 0);
    } else if (!on && t->alt) {
        window_term_restore(win, t->alt);
        t->alt = 0;
        window_term_move(win, t->alt_y, t->alt_x);
    }
    t->top = 0; t->bot = -1; t->wrap = 0;
}

static void vt_private(struct lx_tty *t, struct window *w, int win, int on) {
    for (int i = 0; i < t->npar; i++) {
        switch (t->par[i]) {
        case 1: t->app_cursor = on; break;   // DECCKM
        case 7: t->nowrap = !on; if (!on) t->wrap = 0; break;
        case 25: if (w->hide_cursor == on) window_set_hide_cursor(win, !on); break;
        case 47: case 1047: vt_alt(t, w, win, on); break;
        case 1048: if (on) vt_save(t, w); else vt_restore(t, win); break;
        case 1049:
            if (on) { vt_save(t, w); vt_alt(t, w, win, 1); }
            else { vt_alt(t, w, win, 0); vt_restore(t, win); }
            break;
        }                                    // mouse reporting, bracketed paste, blink: nothing to do
    }
}

static void vt_csi(struct lx_tty *t, struct window *w, int win, uint8_t f) {
    int a = t->npar > 0 && t->par[0] > 0 ? t->par[0] : 1;        // the usual default
    int b = t->npar > 1 && t->par[1] > 0 ? t->par[1] : 1;
    int raw = t->npar > 0 && t->par[0] > 0 ? t->par[0] : 0;
    int x = w->cursor_x, y = w->cursor_y, cw = w->content_w, ch = w->content_h;
    uint32_t fg = vt_cfg(t, w), bg = vt_cbg(t, w);
    if (t->priv == '?') {
        if (f == 'h') vt_private(t, w, win, 1);
        else if (f == 'l') vt_private(t, w, win, 0);
        return;
    }
    if (t->priv == '>') {
        if (f == 'c') vt_reply(t, "\x1b[>0;0;0c");
        return;
    }
    if (t->priv) return;
    if (f != 'm' && f != 'n' && f != 'c' && f != 't' && f != 'h' && f != 'l') t->wrap = 0;
    switch (f) {
    case 'A': { int top = y >= t->top ? t->top : 0; window_term_move(win, y - a < top ? top : y - a, x); break; }
    case 'B': case 'e': { int bot = y <= vt_bot(t, w) ? vt_bot(t, w) : ch - 1; window_term_move(win, y + a > bot ? bot : y + a, x); break; }
    case 'C': case 'a': window_term_move(win, y, x + a); break;
    case 'D': window_term_move(win, y, x - a); break;
    case 'E': window_term_move(win, y + a, 0); break;
    case 'F': window_term_move(win, y - a, 0); break;
    case 'G': case '`': window_term_move(win, y, a - 1); break;
    case 'd': window_term_move(win, a - 1, x); break;
    case 'H': case 'f': window_term_move(win, a - 1, b - 1); break;
    case 'J':
        if (raw == 0) {
            window_term_fill(win, y, x, cw, fg, bg);
            for (int r = y + 1; r < ch; r++) window_term_fill(win, r, 0, cw, fg, bg);
        } else if (raw == 1) {
            for (int r = 0; r < y; r++) window_term_fill(win, r, 0, cw, fg, bg);
            window_term_fill(win, y, 0, x + 1, fg, bg);
        } else if (raw == 2) {
            vt_clear_all(t, w, win);
        }
        break;
    case 'K':
        if (raw == 0) window_term_fill(win, y, x, cw, fg, bg);
        else if (raw == 1) window_term_fill(win, y, 0, x + 1, fg, bg);
        else window_term_fill(win, y, 0, cw, fg, bg);
        break;
    case 'X': window_term_fill(win, y, x, x + a > cw ? cw : x + a, fg, bg); break;
    case '@': window_term_shift(win, y, x, a, fg, bg); break;
    case 'P': window_term_shift(win, y, x, -a, fg, bg); break;
    case 'L': if (y >= t->top && y <= vt_bot(t, w)) { window_term_scroll(win, y, vt_bot(t, w), -a, fg, bg, 0); window_term_move(win, y, 0); } break;
    case 'M': if (y >= t->top && y <= vt_bot(t, w)) { window_term_scroll(win, y, vt_bot(t, w), a, fg, bg, 0); window_term_move(win, y, 0); } break;
    case 'S': vt_scroll(t, w, win, t->top, vt_bot(t, w), a); break;
    case 'T': window_term_scroll(win, t->top, vt_bot(t, w), -a, fg, bg, 0); break;
    case 'r': {
        int top = a - 1, bot = t->npar > 1 && t->par[1] > 0 ? t->par[1] - 1 : ch - 1;
        if (bot > ch - 1) bot = ch - 1;
        if (top < bot) { t->top = top; t->bot = bot; window_term_move(win, 0, 0); }
        break;
    }
    case 's': vt_save(t, w); break;
    case 'u': vt_restore(t, win); break;
    case 'm': vt_sgr(t); break;
    case 'h': case 'l': for (int i = 0; i < t->npar; i++) if (t->par[i] == 4) t->irm = f == 'h'; break;
    case 'n':
        if (raw == 5) vt_reply(t, "\x1b[0n");
        else if (raw == 6) { vt_reply(t, "\x1b["); vt_reply_num(t, y + 1); q_put(t, ';'); vt_reply_num(t, x + 1); q_put(t, 'R'); }
        break;
    case 'c': if (raw == 0) vt_reply(t, "\x1b[?1;2c"); break;
    }
}

// OSC 7777: the cell grid on the serial log, for tests (tests/headless/test_lx_term.py).
static void vt_dump(struct window *w, int win) {
    serial_printf("[termdump] %dx%d cursor %d,%d\n", w->content_w, w->content_h, w->cursor_x, w->cursor_y);
    for (int r = 0; r < w->content_h; r++) {
        serial_printf("[term%d]", r);
        int last = -1;
        for (int c = 0; c < w->content_w; c++) if (window_term_char(win, r, c) != ' ') last = c;
        for (int c = 0; c <= last; c++) serial_putchar(window_term_char(win, r, c));
        serial_putchar('\n');
    }
}

static void vt_byte(struct lx_tty *t, struct window *w, int win, uint8_t c) {
    switch (t->st) {
    case 1:                                  // after ESC
        t->st = 0;
        switch (c) {
        case '[': t->st = 2; t->npar = 0; t->priv = 0; t->par[0] = -1; break;
        case ']': case 'P': case '^': case '_': t->st = 3; t->osc = 0; t->osc_num = 1; break;
        case '(': case ')': case '*': case '+': case '#': case '%': case '-': case '.': case '/': case ' ': t->st = 4; break;
        case '7': vt_save(t, w); break;
        case '8': vt_restore(t, win); break;
        case 'D': t->wrap = 0; vt_lf(t, w, win); break;
        case 'E': t->wrap = 0; window_term_move(win, w->cursor_y, 0); vt_lf(t, w, win); break;
        case 'M':
            t->wrap = 0;
            if (w->cursor_y == t->top) window_term_scroll(win, t->top, vt_bot(t, w), -1, vt_cfg(t, w), vt_cbg(t, w), 0);
            else if (w->cursor_y > 0) window_term_move(win, w->cursor_y - 1, w->cursor_x);
            break;
        case 'c':
            vt_alt(t, w, win, 0);
            vt_defaults(t);
            vt_clear_all(t, w, win);
            window_term_move(win, 0, 0);
            if (w->hide_cursor) window_set_hide_cursor(win, 0);
            break;
        }                                    // ESC = and ESC > (keypad mode): the keypad sends plain keys either way
        return;
    case 2:                                  // CSI
        if (c >= '0' && c <= '9') {
            if (t->npar == 0) t->npar = 1;
            if (t->par[t->npar - 1] < 0) t->par[t->npar - 1] = 0;
            if (t->par[t->npar - 1] < 100000) t->par[t->npar - 1] = t->par[t->npar - 1] * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
            if (t->npar == 0) t->npar = 1;
            if (t->npar < VT_PARS) t->par[t->npar++] = -1;
        } else if (c >= 0x3C && c <= 0x3F) {
            t->priv = c;
        } else if (c >= 0x20 && c <= 0x2F) {
            t->priv = c;                     // intermediates: sequences we do not know, dropped by vt_csi
        } else if (c >= 0x40 && c <= 0x7E) {
            t->st = 0;
            vt_csi(t, w, win, c);
        } else if (c == 0x18 || c == 0x1A) {
            t->st = 0;
        } else if (c == 0x1B) {
            t->st = 1;
        }
        return;
    case 3:                                  // OSC, DCS ...: a string up to BEL or ST
        if (c == 7) { t->st = 0; if (t->osc_num && t->osc == 7777) vt_dump(w, win); }
        else if (c == 0x1B) t->st = 5;
        else if (t->osc_num && c >= '0' && c <= '9' && t->osc < 100000) t->osc = t->osc * 10 + (c - '0');
        else t->osc_num = 0;
        return;
    case 5:                                  // ESC inside a string
        t->st = c == '\\' ? 0 : 3;
        if (c == '\\' && t->osc_num && t->osc == 7777) vt_dump(w, win);
        return;
    case 4:                                  // ESC ( B and the like: one more byte, nothing to do
        t->st = 0;
        return;
    }
    if (t->u_need) {
        if ((c & 0xC0) == 0x80) {
            t->u_cp = (t->u_cp << 6) | (c & 0x3F);
            if (--t->u_need == 0) vt_glyph(t, w, win, vt_ascii(t->u_cp));
            return;
        }
        t->u_need = 0;                       // broken sequence: fall through and treat this byte normally
    }
    if (c >= 0xF0) { t->u_cp = c & 7; t->u_need = 3; return; }
    if (c >= 0xE0) { t->u_cp = c & 15; t->u_need = 2; return; }
    if (c >= 0xC0) { t->u_cp = c & 31; t->u_need = 1; return; }
    if (c >= 0x80) return;
    switch (c) {
    case 0x1B: t->st = 1; return;
    case '\r': t->wrap = 0; window_term_move(win, w->cursor_y, 0); return;
    case '\n': case 0x0B: case 0x0C:
        t->wrap = 0;
        if ((t->oflag & 5) == 5) window_term_move(win, w->cursor_y, 0);    // OPOST | ONLCR
        vt_lf(t, w, win);
        return;
    case '\b': t->wrap = 0; if (w->cursor_x > 0) window_term_move(win, w->cursor_y, w->cursor_x - 1); return;
    case '\t': {
        int nx = (w->cursor_x + 8) & ~7;
        window_term_move(win, w->cursor_y, nx >= w->content_w ? w->content_w - 1 : nx);
        return;
    }
    }
    if (c < 32 || c == 0x7F) return;
    vt_glyph(t, w, win, (char)c);
}

static void out_char(struct lx_tty *t, int win, uint8_t c) {
    if (c != '\r') serial_putchar((char)c);
    struct window *w = win >= 0 ? window_get(win) : 0;
    if (!w) return;
    if (t) vt_byte(t, w, win, c);
    else if (c != '\r') window_put_char(win, (char)c);    // more terminals than state slots: plain text
}

void lx_tty_write(struct lx_task *t, const uint8_t *buf, uint32_t len) {
    int win = t->term_win;
    struct lx_tty *tt = win >= 0 ? tty_for(win, 1) : 0;
    for (uint32_t i = 0; i < len; i++) out_char(tt, win, buf[i]);
}

// The last program of this window is gone: the shell gets a normal terminal back.
void lx_tty_release(int win) {
    struct lx_task *owner = 0;
    if (win < 0 || lx_tty_owner(win, &owner)) return;
    struct lx_tty *t = tty_for(win, 0);
    if (!t) return;
    struct window *w = window_get(win);
    if (w) {
        vt_alt(t, w, win, 0);
        if (w->hide_cursor) window_set_hide_cursor(win, 0);
    }
    void *keep_alt = t->alt;
    tty_defaults(t);
    t->alt = keep_alt;
}

int lx_tty_read_ready(struct lx_task *t) {
    struct lx_tty *tt = tty_for(t->term_win, 1);
    if (!tt) return 0;
    return tt->qh != tt->qt || tt->eof;
}

int lx_tty_wake(struct lx_task *t) { return lx_tty_read_ready(t); }

// Must a read wait? Not in raw mode with VMIN 0: it returns what there is, even nothing.
int lx_tty_read_returns(struct lx_task *t) {
    struct lx_tty *tt = tty_for(t->term_win, 1);
    if (tt && !(tt->lflag & ICANON) && tt->cc[6] == 0) return 1;
    return lx_tty_read_ready(t);
}

int lx_tty_read(struct lx_task *t, uint8_t *buf, uint32_t len) {
    struct lx_tty *tt = tty_for(t->term_win, 1);
    if (!tt) return 0;
    if (tt->qh == tt->qt && tt->eof) { tt->eof = 0; return 0; }
    uint32_t n = 0;
    while (n < len && tt->qh != tt->qt) {
        buf[n++] = tt->q[tt->qh];
        tt->qh = (tt->qh + 1) % TTY_Q;
        if ((tt->lflag & ICANON) && buf[n - 1] == '\n') break;     // a line per read
    }
    return (int)n;
}

static void erase_one(struct lx_tty *t, int win) {
    if (t->llen > 0) {
        t->llen--;
        if (t->lflag & ECHO) { out_char(t, win, 8); out_char(t, win, ' '); out_char(t, win, 8); }
    }
}

// A key typed into terminal window `win`. Returns 1 when a program of the Linux subsystem owns the window.
int lx_tty_key(int win, int c) {
    struct lx_task *owner = 0;
    if (!lx_tty_owner(win, &owner)) return 0;
    struct lx_tty *t = tty_for(win, 1);
    if (!t) return 0;
    if ((t->lflag & ISIG) && c == t->cc[0]) {                          // ctrl-C
        if (t->lflag & ECHO) { out_char(t, win, '^'); out_char(t, win, 'C'); out_char(t, win, '\n'); }
        t->llen = 0;
        lx_signal_fg(win, 2);
        return 1;
    }
    if (c == '\r') c = '\n';                                           // Enter arrives as either
    if (!(t->lflag & ICANON)) {                                        // raw: straight through
        if (c == '\n' && !(t->iflag & ICRNL)) c = '\r';                // what a real Enter key sends before ICRNL
        if (c == '\b') c = t->cc[2] ? t->cc[2] : 0x7F;                 // and what terminfo's kbs says for xterm
        q_put(t, (uint8_t)c);
        if (t->lflag & ECHO) out_char(t, win, (uint8_t)c);
        return 1;
    }
    if (c == t->cc[2] || c == 8) { erase_one(t, win); return 1; }
    if (c == t->cc[3]) { while (t->llen > 0) erase_one(t, win); return 1; }
    if (c == t->cc[4]) {                                               // ctrl-D
        if (t->llen == 0) t->eof = 1;
        else { for (int i = 0; i < t->llen; i++) q_put(t, t->line[i]); t->llen = 0; }
        return 1;
    }
    if (c == '\n') {
        for (int i = 0; i < t->llen; i++) q_put(t, t->line[i]);
        q_put(t, '\n');
        t->llen = 0;
        if (t->lflag & ECHO) out_char(t, win, '\n');
        return 1;
    }
    if (c < 32 && c != '\t') return 1;
    if (t->llen < (int)sizeof t->line - 1) {
        t->line[t->llen++] = (uint8_t)c;
        if (t->lflag & ECHO) out_char(t, win, (uint8_t)c);
    }
    return 1;
}

// A key without a character (keyboard.h KEY_EXT_*). Sent the way xterm does; a line being edited has no use for them.
int lx_tty_ext(int win, int code) {
    struct lx_task *owner = 0;
    if (!lx_tty_owner(win, &owner)) return 0;
    struct lx_tty *t = tty_for(win, 1);
    if (!t || (t->lflag & ICANON)) return 1;
    static const char *const tilde[] = { "\x1b[2~", "\x1b[3~", "\x1b[5~", "\x1b[6~" };   // insert delete pgup pgdn
    switch (code) {
    case KEY_EXT_UP: case KEY_EXT_DOWN: case KEY_EXT_RIGHT: case KEY_EXT_LEFT: case KEY_EXT_HOME: case KEY_EXT_END: {
        static const char fin[] = { 0, 'A', 'B', 'C', 'D', 'H', 'F' };
        q_put(t, 0x1B);
        q_put(t, t->app_cursor ? 'O' : '[');                           // DECCKM
        q_put(t, (uint8_t)fin[code]);
        return 1;
    }
    case KEY_EXT_ESC: q_put(t, 0x1B); return 1;
    case KEY_EXT_INSERT: vt_reply(t, tilde[0]); return 1;
    case KEY_EXT_DELETE: vt_reply(t, tilde[1]); return 1;
    case KEY_EXT_PGUP: vt_reply(t, tilde[2]); return 1;
    case KEY_EXT_PGDN: vt_reply(t, tilde[3]); return 1;
    }
    return 1;
}

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541B
#define TIOCSCTTY 0x540E

int lx_tty_ioctl(struct lx_task *t, uint64_t req, uint64_t arg) {
    struct lx_tty *tt = tty_for(t->term_win, 1);
    if (!tt) return -LX_ENOTTY;
    switch (req) {
    case TCGETS: {
        uint8_t k[36];
        memset(k, 0, sizeof k);
        memcpy(k, &tt->iflag, 4); memcpy(k + 4, &tt->oflag, 4); memcpy(k + 8, &tt->cflag, 4); memcpy(k + 12, &tt->lflag, 4);
        memcpy(k + 17, tt->cc, 19);
        return lx_copy_out(t, arg, k, sizeof k);
    }
    case TCSETS: case TCSETSW: case TCSETSF: {
        uint8_t k[36];
        if (lx_copy_in(t, k, arg, sizeof k)) return -LX_EFAULT;
        memcpy(&tt->iflag, k, 4); memcpy(&tt->oflag, k + 4, 4); memcpy(&tt->cflag, k + 8, 4); memcpy(&tt->lflag, k + 12, 4);
        memcpy(tt->cc, k + 17, 19);
        if (!(tt->lflag & ICANON)) tt->llen = 0;
        return 0;
    }
    case TIOCGWINSZ: {
        uint16_t ws[4] = { 24, 80, 0, 0 };
        struct window *w = t->term_win >= 0 ? window_get(t->term_win) : 0;
        if (w && w->content_w > 0 && w->content_h > 0) { ws[0] = (uint16_t)w->content_h; ws[1] = (uint16_t)w->content_w; }
        return lx_copy_out(t, arg, ws, 8);
    }
    case TIOCSWINSZ: case TIOCSCTTY: return 0;
    case TIOCGPGRP: { int32_t p = t->pid; return lx_copy_out(t, arg, &p, 4); }
    case TIOCSPGRP: return 0;
    case FIONREAD: { int32_t n = (int32_t)((tt->qt + TTY_Q - tt->qh) % TTY_Q); return lx_copy_out(t, arg, &n, 4); }
    default: return -LX_ENOTTY;
    }
}

