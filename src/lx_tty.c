// Terminals of the Linux subsystem: the standard files of a program that runs in a terminal window. Output goes to the
// window (and the serial log); keys typed into the window arrive through lx_tty_key() and are line-edited here
// (canonical mode) or passed through (raw mode) according to the termios the program set.
#include "lx.h"
#include "serial.h"
#include "string.h"
#include "window.h"

#define TTY_MAX 8
#define TTY_Q 4096

#define ICANON 0000002
#define ECHO 0000010
#define ISIG 0000001

struct lx_tty {
    int win;                                 // terminal window id, -1 = unused
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[19];
    uint8_t q[TTY_Q];                        // bytes a read can take
    uint32_t qh, qt;
    uint8_t line[512];                       // the line being typed (canonical mode)
    int llen;
    int eof;                                 // ctrl-D on an empty line: the next read returns 0
};

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
}

static struct lx_tty *tty_for(int win, int create) {
    if (!inited) { for (int i = 0; i < TTY_MAX; i++) ttys[i].win = -1; inited = 1; }
    for (int i = 0; i < TTY_MAX; i++) if (ttys[i].win == win) return &ttys[i];
    if (!create) return 0;
    for (int i = 0; i < TTY_MAX; i++)
        if (ttys[i].win < 0) { tty_defaults(&ttys[i]); ttys[i].win = win; return &ttys[i]; }
    return 0;
}

static void q_put(struct lx_tty *t, uint8_t c) {
    uint32_t n = (t->qt + 1) % TTY_Q;
    if (n == t->qh) return;                  // full: drop
    t->q[t->qt] = c; t->qt = n;
}

static void out_char(int win, uint8_t c) {
    serial_putchar((char)c);
    if (win >= 0 && window_get(win)) window_put_char(win, (char)c);
}

void lx_tty_write(struct lx_task *t, const uint8_t *buf, uint32_t len) {
    int win = t->term_win;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == '\r') continue;             // the window's newline already returns the carriage
        out_char(win, c);
    }
}

int lx_tty_read_ready(struct lx_task *t) {
    struct lx_tty *tt = tty_for(t->term_win, 1);
    if (!tt) return 0;
    return tt->qh != tt->qt || tt->eof;
}

int lx_tty_wake(struct lx_task *t) { return lx_tty_read_ready(t); }

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
        if (t->lflag & ECHO) { out_char(win, 8); out_char(win, ' '); out_char(win, 8); }
    }
}

// A key typed into terminal window `win`. Returns 1 when a program of the Linux subsystem owns the window.
int lx_tty_key(int win, int c) {
    struct lx_task *owner = 0;
    if (!lx_tty_owner(win, &owner)) return 0;
    struct lx_tty *t = tty_for(win, 1);
    if (!t) return 0;
    if ((t->lflag & ISIG) && c == t->cc[0]) {                          // ctrl-C
        out_char(win, '^'); out_char(win, 'C'); out_char(win, '\n');
        t->llen = 0;
        lx_signal_fg(win, 2);
        return 1;
    }
    if (c == '\r') c = '\n';
    if (!(t->lflag & ICANON)) {                                        // raw: straight through
        q_put(t, (uint8_t)c);
        if (t->lflag & ECHO) out_char(win, (uint8_t)c);
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
        if (t->lflag & ECHO) out_char(win, '\n');
        return 1;
    }
    if (c < 32 && c != '\t') return 1;
    if (t->llen < (int)sizeof t->line - 1) {
        t->line[t->llen++] = (uint8_t)c;
        if (t->lflag & ECHO) out_char(win, (uint8_t)c);
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

