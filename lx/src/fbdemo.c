#include "lxsys.h"

#define O_RDWR 2
#define FBIOGET_VSCREENINFO 0x4600
#define PROT_RW 3
#define MAP_SHARED 1

struct ev { u64 sec, usec; unsigned short type, code; int value; };

static void rect(u32 *fb, int stride, int x, int y, int w, int h, u32 c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(y + j) * stride + x + i] = c;
}

__attribute__((force_align_arg_pointer)) void _start(void) {
    int fd = (int)sc3(SYS_open, (i64)"/dev/fb0", O_RDWR, 0);
    if (fd < 0) { puts_("no fb\n"); sys_exit(1); }
    u32 v[40];
    sc3(16, fd, FBIOGET_VSCREENINFO, (i64)v);
    int w = (int)v[0], h = (int)v[1];
    puts_("fb "); put_dec(w); puts_("x"); put_dec(h); puts_(" bpp "); put_dec(v[6]); puts_("\n");
    u32 *fb = (u32*)sc6(SYS_mmap, 0, (i64)w * h * 4, PROT_RW, MAP_SHARED, fd, 0);
    if ((i64)fb < 0 && (i64)fb > -4096) { puts_("mmap failed\n"); sys_exit(1); }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fb[y * w + x] = 0x00203040 + (u32)((x * 255 / w) << 8);
    rect(fb, w, 50, 50, 100, 100, 0x00FF0000);          // red
    rect(fb, w, 200, 50, 100, 100, 0x000000FF);         // blue
    rect(fb, w, 350, 50, 100, 100, 0x0000FF00);         // green
    puts_("drawn\n");
    int ev = (int)sc3(SYS_open, (i64)"/dev/input/event0", O_RDWR, 0);
    struct ev e;
    int x = 500, y = 300;
    for (;;) {
        i64 n = sys_read(ev, &e, sizeof e);
        if (n != (i64)sizeof e) continue;
        if (e.type == 1 && e.value == 1) {
            puts_("key "); put_dec(e.code); puts_("\n");
            if (e.code == 1) break;                       // ESC
            if (e.code == 30) { rect(fb, w, x, y, 20, 20, 0x00FFFF00); x += 24; puts_("a\n"); }
        }
        if (e.type == 2) { puts_("rel "); put_dec(e.code); puts_(" "); put_dec(e.value); puts_("\n"); }
    }
    puts_("bye\n");
    sys_exit(0);
}
