// A raw-protocol X11 test client (no Xlib): window, child window, GC, shapes, text, pixmap copy, properties, events.
#include "../../src/lxsys.h"

typedef unsigned short u16;
static void p16(u8 *b, int o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static void p32(u8 *b, int o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); b[o + 2] = (u8)(v >> 16); b[o + 3] = (u8)(v >> 24); }
static u32 g16(const u8 *b, int o) { return (u32)b[o] | ((u32)b[o + 1] << 8); }
static u32 g32(const u8 *b, int o) { return g16(b, o) | (g16(b, o + 2) << 16); }

static int fd;
#define NOLOOP __attribute__((optimize("no-tree-loop-distribute-patterns")))
NOLOOP void *memcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
NOLOOP void *memset(void *d, int c, u64 n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }

static void sendall(const u8 *b, int n) {
    while (n > 0) { i64 r = sys_write(fd, b, (u64)n); if (r <= 0) { puts_("xtest: write failed\n"); sys_exit(1); } b += r; n -= (int)r; }
}
static void readn(u8 *b, int n) {
    while (n > 0) { i64 r = sys_read(fd, b, (u64)n); if (r <= 0) { puts_("xtest: connection closed\n"); sys_exit(1); } b += r; n -= (int)r; }
}

static u32 base, root, nextid;
static u32 newid(void) { return base | nextid++; }

// a request is built in `q` and sent; `words` is the length in 4-byte units
static u8 q[4096];
static void req(int op, int data, int words) { q[0] = (u8)op; q[1] = (u8)data; p16(q, 2, (u32)words); sendall(q, words * 4); memset(q, 0, 64); }

static u8 rep[4096 + 64];
static u8 pending[32][32]; static int npend, ipend;
static void read_reply(void) {          // reads the next reply into rep; events that arrive first are queued
    for (;;) {
        readn(rep, 32);
        if (rep[0] == 1) { u32 extra = g32(rep, 4) * 4; if (extra) readn(rep + 32, (int)extra); return; }
        if (rep[0] == 0) { puts_("xtest: error code "); put_dec(rep[1]); puts_(" op "); put_dec(rep[10]); puts_("\n"); return; }
        if (npend < 32) { memcpy(pending[npend], rep, 32); npend++; }
    }
}

static u32 intern(const char *s) {
    int n = (int)slen(s);
    memset(q, 0, 64);
    p16(q, 4, (u32)n);
    memcpy(q + 8, s, (u64)n);
    req(16, 0, 2 + (n + 3) / 4);
    read_reply();
    return g32(rep, 8);
}

static void change_gc(u32 gc, u32 mask, u32 v0, u32 v1) {
    memset(q, 0, 64);
    p32(q, 4, gc); p32(q, 8, mask); p32(q, 12, v0); p32(q, 16, v1);
    req(56, 0, 3 + (mask & (mask - 1) ? 2 : 1));
}
static void set_fg(u32 gc, u32 c) { change_gc(gc, 4, c, 0); }

static void rects(int op, u32 d, u32 gc, const int *r, int n) {
    memset(q, 0, 64);
    p32(q, 4, d); p32(q, 8, gc);
    for (int i = 0; i < n; i++) { p16(q, 12 + 8 * i, (u32)r[4 * i]); p16(q, 14 + 8 * i, (u32)r[4 * i + 1]); p16(q, 16 + 8 * i, (u32)r[4 * i + 2]); p16(q, 18 + 8 * i, (u32)r[4 * i + 3]); }
    req(op, 0, 3 + 2 * n);
}
static void fillrect(u32 d, u32 gc, int x, int y, int w, int h) { int r[4] = { x, y, w, h }; rects(70, d, gc, r, 1); }

static void text(u32 d, u32 gc, int x, int y, const char *s) {
    int n = (int)slen(s);
    memset(q, 0, 64);
    p32(q, 4, d); p32(q, 8, gc); p16(q, 12, (u32)x); p16(q, 14, (u32)y);
    memcpy(q + 16, s, (u64)n);
    req(76, n, 4 + (n + 3) / 4);
}

static u32 win, child, gc, pix, atom_proto, atom_delete;

static void points(int op, u32 d, u32 g, const int *p, int n, int extra_word) {     // PolyLine (65) / FillPoly (69)
    memset(q, 0, 64);
    p32(q, 4, d); p32(q, 8, g);
    int off = 12;
    if (op == 69) off = 16;
    for (int i = 0; i < n; i++) { p16(q, off + 4 * i, (u32)p[2 * i]); p16(q, off + 2 + 4 * i, (u32)p[2 * i + 1]); }
    (void)extra_word;
    req(op, 0, off / 4 + n);
}

static void arc(int op, u32 d, u32 g, int x, int y, int w, int h) {
    memset(q, 0, 64);
    p32(q, 4, d); p32(q, 8, g);
    p16(q, 12, (u32)x); p16(q, 14, (u32)y); p16(q, 16, (u32)w); p16(q, 18, (u32)h); p16(q, 20, 0); p16(q, 22, 360 * 64);
    req(op, 0, 3 + 3);
}

static void draw(void) {
    set_fg(gc, 0xFFFFFF); fillrect(win, gc, 0, 0, 400, 300);
    set_fg(gc, 0xC00000); fillrect(win, gc, 20, 20, 100, 60);
    set_fg(gc, 0x00A000); fillrect(win, gc, 140, 20, 100, 60);
    set_fg(gc, 0x0000C0); fillrect(win, gc, 260, 20, 100, 60);
    change_gc(gc, 4 | 0x10, 0x000000, 3);                       // black, line width 3
    int ln[] = { 20, 100, 120, 130, 220, 100, 380, 130 };
    points(65, win, gc, ln, 4, 0);
    change_gc(gc, 4 | 0x10, 0xFF8000, 1);
    arc(71, win, gc, 30, 150, 70, 70);                          // filled disc
    change_gc(gc, 4, 0x000000, 0);
    arc(68, win, gc, 30, 150, 70, 70);                          // circle outline
    int tri[] = { 140, 220, 190, 150, 240, 220 };
    set_fg(gc, 0x8000C0);
    points(69, win, gc, tri, 3, 0);
    // a pixmap with a pattern copied into the window
    set_fg(gc, 0x00C0C0); fillrect(pix, gc, 0, 0, 64, 64);
    set_fg(gc, 0x000000); { int r[4] = { 8, 8, 48, 48 }; rects(67, pix, gc, r, 1); }
    memset(q, 0, 64);
    p32(q, 4, pix); p32(q, 8, win); p32(q, 12, gc); p16(q, 16, 0); p16(q, 18, 0); p16(q, 20, 300); p16(q, 22, 150); p16(q, 24, 64); p16(q, 26, 64);
    req(62, 0, 8);
    set_fg(gc, 0x000000);
    change_gc(gc, 8, 0xFFFFFF, 0);
    text(win, gc, 20, 250, "Hello, X11 from xokai");
}

static void draw_child(void) {
    set_fg(gc, 0xFFFF00);
    fillrect(child, gc, 0, 0, 80, 60);
    set_fg(gc, 0x000000);
    change_gc(gc, 8, 0xFFFF00, 0);
    text(child, gc, 6, 30, "child");
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    fd = (int)sc3(41, 1, 1, 0);
    struct { u16 fam; char path[108]; } sa;
    memset(&sa, 0, sizeof sa);
    sa.fam = 1;
    const char *p = "/tmp/.X11-unix/X0";
    for (int i = 0; p[i]; i++) sa.path[i] = p[i];
    if (sc3(42, fd, (i64)&sa, 2 + (i64)slen(p) + 1) < 0) { puts_("xtest: cannot connect\n"); sys_exit(1); }
    u8 setup[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    sendall(setup, 12);
    u8 hdr[8];
    readn(hdr, 8);
    if (hdr[0] != 1) { puts_("xtest: setup refused\n"); sys_exit(1); }
    static u8 body[4096];
    int blen = (int)g16(hdr, 6) * 4;
    readn(body, blen);
    // body starts at offset 8 of the reply: release(4) base(4) mask(4) motion(4) vlen(2) maxreq(2) nroots(1) nfmt(1) ...
    base = g32(body, 4);
    int vlen = (int)g16(body, 16), nfmt = body[21];
    root = g32(body, 32 + ((vlen + 3) & ~3) + 8 * nfmt);
    nextid = 1;
    puts_("xtest: connected, root "); put_dec(root); puts_("\n");

    atom_proto = intern("WM_PROTOCOLS");
    atom_delete = intern("WM_DELETE_WINDOW");

    win = newid();
    memset(q, 0, 64);
    p32(q, 4, win); p32(q, 8, root); p16(q, 12, 0); p16(q, 14, 0); p16(q, 16, 400); p16(q, 18, 300); p16(q, 20, 0); p16(q, 22, 1);
    p32(q, 28, 0x2 | 0x800); p32(q, 32, 0xFFFFFF);
    p32(q, 36, 0x8000 | 0x4 | 0x1 | 0x20000 | 0x40);           // Exposure ButtonPress KeyRelease... StructureNotify PointerMotion
    req(1, 0, 10);

    // WM_NAME
    const char *title = "xtest";
    memset(q, 0, 64);
    p32(q, 4, win); p32(q, 8, 39); p32(q, 12, 31); q[16] = 8; p32(q, 20, (u32)slen(title)); memcpy(q + 24, title, slen(title));
    req(18, 0, 6 + (int)((slen(title) + 3) / 4));
    // WM_PROTOCOLS = [WM_DELETE_WINDOW]
    memset(q, 0, 64);
    p32(q, 4, win); p32(q, 8, atom_proto); p32(q, 12, 4); q[16] = 32; p32(q, 20, 1); p32(q, 24, atom_delete);
    req(18, 0, 7);

    gc = newid();
    memset(q, 0, 64);
    p32(q, 4, gc); p32(q, 8, win); p32(q, 12, 4 | 8); p32(q, 16, 0); p32(q, 20, 0xFFFFFF);
    req(55, 0, 6);

    // font "fixed"
    u32 font = newid();
    memset(q, 0, 64);
    p32(q, 4, font); p16(q, 8, 5); memcpy(q + 12, "fixed", 5);
    req(45, 0, 5);
    change_gc(gc, 0x4000, font, 0);

    pix = newid();
    memset(q, 0, 64);
    q[1] = 24; p32(q, 4, pix); p32(q, 8, win); p16(q, 12, 64); p16(q, 14, 64);
    req(53, 24, 4);

    // a child window
    child = newid();
    memset(q, 0, 64);
    p32(q, 4, child); p32(q, 8, win); p16(q, 12, 300); p16(q, 14, 220); p16(q, 16, 80); p16(q, 18, 60); p16(q, 22, 1);
    p32(q, 28, 0x2 | 0x800); p32(q, 32, 0xFFFF00); p32(q, 36, 0x8000);
    req(1, 0, 10);
    memset(q, 0, 64); p32(q, 4, child); req(8, 0, 2);          // MapWindow child
    memset(q, 0, 64); p32(q, 4, win); req(8, 0, 2);            // MapWindow win

    // a round trip proves the server has been answering: GetInputFocus
    memset(q, 0, 64); req(43, 0, 1); read_reply();
    puts_("xtest: mapped\n");

    // AllocNamedColor
    memset(q, 0, 64);
    p32(q, 4, 0x22); p16(q, 8, 6); memcpy(q + 12, "orange", 6);
    req(85, 0, 5);
    read_reply();
    puts_("xtest: orange pixel "); put_dec(g32(rep, 8)); puts_("\n");

    // GetProperty WM_NAME back
    memset(q, 0, 64);
    p32(q, 4, win); p32(q, 8, 39); p32(q, 12, 0); p32(q, 16, 0); p32(q, 20, 16);
    req(20, 0, 6);
    read_reply();
    puts_("xtest: wm_name len "); put_dec(g32(rep, 16)); puts_("\n");

    int exposes = 0;
    static u8 ev[32];
    for (;;) {
        if (ipend < npend) memcpy(ev, pending[ipend++], 32); else { if (ipend == npend) { ipend = npend = 0; } readn(ev, 32); }
        int t = ev[0] & 0x7F;
        if (t == 0) { puts_("xtest: error "); put_dec(ev[1]); puts_("\n"); continue; }
        if (t == 12) {                                         // Expose
            u32 w = g32(ev, 4);
            if (g16(ev, 16) == 0) {
                puts_("xtest: expose "); put_dec(w == win ? 1 : 2); puts_("\n");
                if (w == win) draw(); else if (w == child) draw_child();
                exposes++;
                memset(q, 0, 64); req(43, 0, 1); read_reply();      // sync, then report
                puts_("xtest: drawn\n");
            }
        } else if (t == 4) {                                   // ButtonPress
            puts_("xtest: button "); put_dec(ev[1]); puts_(" at "); put_dec(g16(ev, 24)); puts_(","); put_dec(g16(ev, 26));
            puts_(" win "); put_dec(g32(ev, 12) == child ? 2 : 1); puts_("\n");
        } else if (t == 2) {                                   // KeyPress
            puts_("xtest: key "); put_dec(ev[1]); puts_("\n");
            if (ev[1] == 9) break;
        } else if (t == 22) {
            puts_("xtest: configure "); put_dec(g16(ev, 20)); puts_("x"); put_dec(g16(ev, 22)); puts_("\n");
        } else if (t == 6) {
            puts_("xtest: motion\n");
        } else if (t == 33) {
            if (g32(ev, 12) == atom_delete) { puts_("xtest: close\n"); break; }
        }
    }
    puts_("xtest: bye\n");
    sys_exit(0);
}
