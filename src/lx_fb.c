// /dev/fb0 and /dev/input/event0 for the Linux subsystem: a program that opens the framebuffer gets a desktop window; the
// memory it mmaps is that window's picture. Keys and the pointer inside the window come out of the event device as
// struct input_event, like evdev.
#include "lx.h"
#include "cursor.h"
#include "graphics.h"
#include "keyboard.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "string.h"
#include "window.h"

extern uint32_t tick_count;

#define FB_W 800
#define FB_H 600

static struct {
    int win;                     // desktop window, -1 = none
    int owner;                   // pid of the task that opened it
    uint32_t w, h;
    uint8_t *raw;                // kmalloc block
    uint32_t *pix;               // page aligned inside raw
    uint32_t last_tick;
    int mapped;
    // input
    uint8_t keys[256];           // last seen state of the scancodes
    uint8_t extkeys[128];
    int last_mx, last_my, last_left, have_pos;
    struct { uint16_t type, code; int32_t value; uint32_t sec, usec; } q[256];
    uint32_t qh, qt;
} F = { .win = -1 };

int lx_fb_win(void) { return F.win; }

static void evq(uint16_t type, uint16_t code, int32_t value) {
    uint32_t n = (F.qt + 1) % 256;
    if (n == F.qh) return;
    F.q[F.qt].type = type; F.q[F.qt].code = code; F.q[F.qt].value = value;
    F.q[F.qt].sec = tick_count / 100; F.q[F.qt].usec = (tick_count % 100) * 10000;
    F.qt = n;
}

static void fb_destroy(void) {
    if (F.win >= 0) {
        window_set_pixels(F.win, 0, 0, 0, 0, 0, 0);
        window_destroy(F.win);
        F.win = -1;
    }
    if (F.raw && !F.mapped) { kfree(F.raw); F.raw = 0; F.pix = 0; }
}

// The window is gone (closed by the user): the program that drew in it is hung up.
void lx_fb_close(void) {
    int pid = F.owner;
    fb_destroy();
    F.owner = 0;
    struct lx_task *t = 0;
    extern int lx_task_by_pid(int pid, struct lx_task **out);
    extern void lx_signal(struct lx_task *t, int sig);
    if (pid && lx_task_by_pid(pid, &t)) lx_signal(t, 1);
}

// The task is going away: unmapped pages are not freed by it, the window follows.
void lx_fb_owner_gone(int pid) {
    if (F.win >= 0 && F.owner == pid) { fb_destroy(); F.owner = 0; }
    if (F.raw && F.win < 0) { kfree(F.raw); F.raw = 0; F.pix = 0; F.mapped = 0; }
}

int lx_fb_open_hook(struct lx_task *t) {
    if (F.win >= 0) return 0;
    uint32_t w = FB_W, h = FB_H;
    if (h + WIN_TITLE_H + 2 * WIN_BORDER > (uint32_t)(SCREEN_H - TASKBAR_H - 8)) { w = 640; h = 480; }
    if (!F.raw) {
        F.raw = (uint8_t*)kmalloc(w * h * 4 + 4096);
        if (!F.raw) return -LX_ENOMEM;
        F.pix = (uint32_t*)(((uint32_t)F.raw + 4095) & ~4095u);
        memset(F.pix, 0, w * h * 4);
    }
    F.w = w; F.h = h;
    int ww = (int)w + 2 * WIN_BORDER, wh = (int)h + WIN_TITLE_H + 2 * WIN_BORDER;
    F.win = window_create("linux", (SCREEN_W - ww) / 2, (SCREEN_H - TASKBAR_H - wh) / 2 > 4 ? (SCREEN_H - TASKBAR_H - wh) / 2 : 4, ww, wh);
    if (F.win < 0) return -LX_ENOMEM;
    window_set_close_button(F.win, 1);
    window_set_minimize_button(F.win, 1);
    window_set_hide_cursor(F.win, 1);
    window_set_focus(F.win);
    F.owner = t->pid;
    F.qh = F.qt = 0;
    memset(F.keys, 0, sizeof F.keys);
    memset(F.extkeys, 0, sizeof F.extkeys);
    F.have_pos = 0;
    window_set_pixels(F.win, F.pix, 0, 0, (int)w, (int)h, (int)w);
    { struct window *fw = window_get(F.win);
      serial_printf("[lx] framebuffer window %ux%u for pid %d content at %d,%d\n", w, h, t->pid, fw ? fw->x + WIN_BORDER : 0, fw ? fw->y + WIN_TITLE_H + WIN_BORDER : 0); }
    return 0;
}

// mmap of /dev/fb0: the pixel pages go straight into the task's address space.
int64_t lx_fb_mmap(struct lx_task *t, uint32_t len) {
    if (F.win < 0 || !F.pix) return -LX_ENODEV;
    uint32_t size = (F.w * F.h * 4 + 0xFFF) & ~0xFFFu;
    if (len > size) len = size;
    uint32_t va = lx_find_free(t, len);
    if (!va) return -LX_ENOMEM;
    int r = lx_map_region(t, va, len, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, 0, 0);
    if (r) return r;
    struct lx_vma *v = lx_vma_find(t, va);
    if (v) v->shared_fb = 1;
    uint32_t phys = V2P((uint32_t)F.pix);
    for (uint32_t o = 0; o < len; o += 0x1000)
        if (aspace_map(t->mm->cr3, va + o, phys + o, 1)) return -LX_ENOMEM;
    F.mapped = 1;
    return va;
}

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606
#define FBIO_WAITFORVSYNC 0x40044620

int lx_fb_ioctl(struct lx_task *t, uint64_t req, uint64_t arg) {
    if (F.win < 0) return -LX_ENODEV;
    switch (req) {
    case FBIOGET_VSCREENINFO: case FBIOPUT_VSCREENINFO: {
        uint32_t v[40];
        memset(v, 0, sizeof v);
        v[0] = F.w; v[1] = F.h; v[2] = F.w; v[3] = F.h;               // xres yres xres_virtual yres_virtual
        v[6] = 32;                                                    // bits_per_pixel
        v[8] = 16; v[9] = 8;                                          // red   offset 16, length 8
        v[11] = 8; v[12] = 8;                                         // green offset 8, length 8
        v[14] = 0; v[15] = 8;                                         // blue  offset 0, length 8
        v[17] = 24; v[18] = 8;                                        // transp offset 24, length 8
        v[22] = 0xFFFFFFFFu; v[23] = 0xFFFFFFFFu;                     // height, width in mm: unknown
        if (req == FBIOPUT_VSCREENINFO) return 0;
        return lx_copy_out(t, arg, v, 160);
    }
    case FBIOGET_FSCREENINFO: {
        uint8_t f[80];
        memset(f, 0, sizeof f);
        memcpy(f, "KAnarchy fb", 12);
        uint64_t smem = V2P((uint32_t)F.pix);
        uint32_t len = F.w * F.h * 4, type = 0, visual = 2, line = F.w * 4;
        memcpy(f + 16, &smem, 8); memcpy(f + 24, &len, 4);
        memcpy(f + 28, &type, 4); memcpy(f + 36, &visual, 4);
        memcpy(f + 48, &line, 4);
        return lx_copy_out(t, arg, f, sizeof f);
    }
    case FBIOPAN_DISPLAY: case FBIO_WAITFORVSYNC: return 0;
    default: return -LX_ENOTTY;
    }
}

// ---- the event device -------------------------------------------------------------------------------------------

int lx_input_ready(void) { return F.qh != F.qt; }
int lx_input_wake(struct lx_task *t) { (void)t; return F.qh != F.qt || F.win < 0; }

int lx_input_read(struct lx_task *t, uint64_t ubuf, uint64_t len) {
    uint64_t done = 0;
    while (done + 24 <= len && F.qh != F.qt) {
        uint8_t ev[24];
        memset(ev, 0, sizeof ev);
        uint64_t sec = F.q[F.qh].sec, usec = F.q[F.qh].usec;
        memcpy(ev, &sec, 8); memcpy(ev + 8, &usec, 8);
        memcpy(ev + 16, &F.q[F.qh].type, 2); memcpy(ev + 18, &F.q[F.qh].code, 2); memcpy(ev + 20, &F.q[F.qh].value, 4);
        if (lx_copy_out(t, ubuf + done, ev, 24)) return done ? (int)done : -LX_EFAULT;
        F.qh = (F.qh + 1) % 256;
        done += 24;
    }
    return (int)done;
}

// Linux key codes of the extended (0xE0) scancodes
static int ext_code(int sc) {
    switch (sc) {
    case 0x48: return 103; case 0x50: return 108; case 0x4B: return 105; case 0x4D: return 106;
    case 0x47: return 102; case 0x4F: return 107; case 0x49: return 104; case 0x51: return 109;
    case 0x52: return 110; case 0x53: return 111; case 0x1D: return 97; case 0x38: return 100; case 0x1C: return 96;
    default: return 0;
    }
}

// Main loop: show the program's picture, turn keys and the pointer into events.
void lx_fb_poll(void) {
    if (F.win < 0) return;
    struct window *w = window_get(F.win);
    if (!w) { lx_fb_close(); return; }
    int focused = window_get_focused() == F.win;
    int any = 0;
    if (focused) {
        for (int sc = 1; sc < 0x59; sc++) {
            int d = keyboard_scancode_held(sc, 0);
            if (d != F.keys[sc]) { F.keys[sc] = (uint8_t)d; evq(1, (uint16_t)sc, d); any = 1; }
        }
        for (int sc = 0; sc < 0x54; sc++) {
            int code = ext_code(sc);
            if (!code) continue;
            int d = keyboard_scancode_held(sc, 1);
            if (d != F.extkeys[sc]) { F.extkeys[sc] = (uint8_t)d; evq(1, (uint16_t)code, d); any = 1; }
        }
    }
    int mx = mouse_get_x(), my = mouse_get_y();
    int cx = w->x + WIN_BORDER, cy = w->y + WIN_TITLE_H + WIN_BORDER;
    int inside = mx >= cx && my >= cy && mx < cx + (int)F.w && my < cy + (int)F.h;
    if (inside && w->visible) {
        if (F.have_pos) {
            if (mx != F.last_mx) { evq(2, 0, mx - F.last_mx); any = 1; }
            if (my != F.last_my) { evq(2, 1, my - F.last_my); any = 1; }
        }
        int left = mouse_get_left_button();
        if (left != F.last_left) { evq(1, 0x110, left); any = 1; }
        F.last_left = left;
        F.have_pos = 1;
    } else F.have_pos = 0;
    F.last_mx = mx; F.last_my = my;
    if (any) evq(0, 0, 0);                                       // EV_SYN
    // the picture: at most ~30 times a second
    if (tick_count - F.last_tick >= 3 && w->visible) {
        F.last_tick = tick_count;
        window_set_pixels(F.win, F.pix, 0, 0, (int)F.w, (int)F.h, (int)F.w);
    }
}
