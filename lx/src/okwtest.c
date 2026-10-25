#include "lxsys.h"
#include "../../src/lx_okwin.h"

#define O_RDWR 2
#define PROT_RW 3

static void rect(unsigned *p, int stride, int x, int y, int w, int h, unsigned c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) p[(y + j) * stride + x + i] = c;
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    int fd = (int)sc3(SYS_open, (i64)"/dev/okwin", O_RDWR, 0);
    if (fd < 0) { puts_("no okwin\n"); sys_exit(1); }
    struct okw_create c = { 100, 80, 400, 300, 0, "okwin test", 0, 0 };
    if (sc3(16, fd, OKW_CREATE, (i64)&c) < 0) { puts_("create failed\n"); sys_exit(1); }
    struct okw_map m = { c.out_wid, 0, 0, 0, 0 };
    if (sc3(16, fd, OKW_MAP, (i64)&m) < 0) { puts_("map failed\n"); sys_exit(1); }
    unsigned *px = (unsigned*)m.out_addr;
    int w = 400, h = 300;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[y * w + x] = 0x00E0E0E0;
    rect(px, w, 20, 20, 100, 60, 0x00C00000);       // red
    rect(px, w, 140, 20, 100, 60, 0x0000A000);      // green
    rect(px, w, 260, 20, 100, 60, 0x000000C0);      // blue
    struct okw_present p = { c.out_wid, 0, 0, 0, 0 };
    sc3(16, fd, OKW_PRESENT, (i64)&p);

    // measure and draw some text through the system font
    struct { u64 str; int len, size, flags, family; u64 mask; int cap_w, cap_h, out_w, out_asc, out_desc, pad; } tx;
    static unsigned char mask[300 * 40];
    const char *msg = "Hello, okwin";
    tx.str = (u64)msg; tx.len = 12; tx.size = 24; tx.flags = 0; tx.family = 0; tx.mask = (u64)mask; tx.cap_w = 300; tx.cap_h = 40;
    if (sc3(16, fd, OKW_TEXT, (i64)&tx) == 0) {
        puts_("text width "); put_dec(tx.out_w); puts_(" asc "); put_dec(tx.out_asc); puts_("\n");
        for (int y = 0; y < 40; y++) for (int x = 0; x < 300; x++) {
            unsigned a = mask[y * 300 + x];
            if (a && 20 + x < w && 120 + y < h) { unsigned v = 255 - a; px[(120 + y) * w + 20 + x] = (v << 16) | (v << 8) | v; }
        }
        p.x = 20; p.y = 120; p.w = 300; p.h = 40;
        sc3(16, fd, OKW_PRESENT, (i64)&p);
    }
    puts_("window ready\n");

    struct okw_event e;
    for (;;) {
        i64 n = sys_read(fd, &e, sizeof e);
        if (n != (i64)sizeof e) continue;
        if (e.type == OKE_MOUSE_BUTTON) { puts_("button "); put_dec(e.c); puts_(" down "); put_dec(e.d); puts_(" at "); put_dec(e.a); puts_(","); put_dec(e.b); puts_("\n"); }
        else if (e.type == OKE_KEY && e.b == 1) { puts_("key "); put_dec(e.a); puts_("\n"); if (e.a == 1) break; }
        else if (e.type == OKE_CHAR) { puts_("char "); put_dec(e.a); puts_("\n"); }
        else if (e.type == OKE_CLOSE) { puts_("close\n"); break; }
        else if (e.type == OKE_FOCUS) { puts_("focus "); put_dec(e.a); puts_("\n"); }
        else if (e.type == OKE_WHEEL) { puts_("wheel "); put_dec(e.c); puts_("\n"); }
    }
    sc3(16, fd, OKW_DESTROY, c.out_wid);
    puts_("bye\n");
    sys_exit(0);
}
