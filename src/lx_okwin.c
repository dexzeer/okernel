// /dev/okwin: windows with shared pixel buffers and input events for programs of the Linux subsystem (see lx_okwin.h).
#include "lx.h"
#include "lx_okwin.h"
#include "cursor.h"
#include "graphics.h"
#include "keyboard.h"
#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"
#include "string.h"
#include "web/font.h"
#include "web/surface.h"
#include "window.h"

extern uint32_t tick_count;

#define OKW_MAX MAX_WINDOWS
#define EVQ 512

struct okconn {
    int refs;
    int pid;
    struct okw_event q[EVQ];
    uint32_t qh, qt;
};

struct okwin {
    int used;
    int wid;                       // desktop window id
    struct okconn *conn;
    int w, h;
    uint8_t *raw;
    uint32_t *pix;                 // page aligned inside raw
    struct lx_mm *map_mm;          // the address space that mapped it (and where)
    uint32_t map_va, map_len;
    int last_focus, last_w, last_h;
    int frameless;
};

static struct okwin wins[OKW_MAX];
static int grab = -1;              // index of the window holding the mouse (a button is down)
static int last_mx = -1, last_my = -1, last_mb;
static uint8_t keyheld[256];
static uint8_t extheld[128];
static int inited;

extern void lx_mm_unmap(struct lx_mm *m, uint32_t va, uint32_t len);
extern void lx_mm_hold(struct lx_mm *m);
extern void lx_mm_release(struct lx_mm *m);

static struct okwin *by_wid(int wid) {
    for (int i = 0; i < OKW_MAX; i++) if (wins[i].used && wins[i].wid == wid) return &wins[i];
    return 0;
}
int lx_okwin_is(int wid) { return by_wid(wid) != 0; }

static void evq(struct okconn *c, unsigned type, int wid, int a, int b, int cc, int d) {
    uint32_t n = (c->qt + 1) % EVQ;
    if (n == c->qh) return;
    struct okw_event *e = &c->q[c->qt];
    e->type = type; e->wid = wid; e->a = a; e->b = b; e->c = cc; e->d = d;
    c->qt = n;
}

// ---- connections ----------------------------------------------------------------------------------------------------------

void *lx_okwin_open(struct lx_task *t) {
    struct okconn *c = (struct okconn*)kcalloc(1, sizeof *c);
    if (!c) return 0;
    c->refs = 1; c->pid = t->tgid;
    if (!inited) { memset(wins, 0, sizeof wins); inited = 1; }
    return c;
}

static void win_free(struct okwin *w) {
    if (w->map_mm) { lx_mm_unmap(w->map_mm, w->map_va, w->map_len); lx_mm_release(w->map_mm); w->map_mm = 0; }
    if (w->wid >= 0 && window_get(w->wid)) { window_set_pixels(w->wid, 0, 0, 0, 0, 0, 0); window_destroy(w->wid); }
    if (w->raw) kfree(w->raw);
    memset(w, 0, sizeof *w);
}

void lx_okwin_close(void *conn) {
    struct okconn *c = (struct okconn*)conn;
    for (int i = 0; i < OKW_MAX; i++) if (wins[i].used && wins[i].conn == c) { if (grab == i) grab = -1; win_free(&wins[i]); }
    kfree(c);
}

int lx_okwin_ready(void *conn) { struct okconn *c = (struct okconn*)conn; return c->qh != c->qt; }

int64_t lx_okwin_read(struct lx_task *t, void *conn, uint64_t ubuf, uint64_t len) {
    struct okconn *c = (struct okconn*)conn;
    uint64_t done = 0;
    while (done + sizeof(struct okw_event) <= len && c->qh != c->qt) {
        if (lx_copy_out(t, ubuf + done, &c->q[c->qh], sizeof(struct okw_event))) return done ? (int64_t)done : -LX_EFAULT;
        c->qh = (c->qh + 1) % EVQ;
        done += sizeof(struct okw_event);
    }
    return (int64_t)done;
}

// ---- windows --------------------------------------------------------------------------------------------------------------

static int alloc_buffer(struct okwin *w, int pw, int ph) {
    uint8_t *raw = (uint8_t*)kmalloc((uint32_t)pw * (uint32_t)ph * 4 + 4096);
    if (!raw) return -LX_ENOMEM;
    w->raw = raw;
    w->pix = (uint32_t*)(((uint32_t)raw + 4095) & ~4095u);
    memset(w->pix, 0xFF, (uint32_t)pw * (uint32_t)ph * 4);
    w->w = pw; w->h = ph;
    return 0;
}

static int64_t do_create(struct lx_task *t, struct okconn *c, uint64_t uarg) {
    struct okw_create a;
    if (lx_copy_in(t, &a, uarg, sizeof a)) return -LX_EFAULT;
    if (a.w < 16 || a.h < 16 || a.w > 2048 || a.h > 1200) return -LX_EINVAL;
    int slot = -1;
    for (int i = 0; i < OKW_MAX; i++) if (!wins[i].used) { slot = i; break; }
    if (slot < 0) return -LX_ENOMEM;
    struct okwin *w = &wins[slot];
    memset(w, 0, sizeof *w);
    int r = alloc_buffer(w, a.w, a.h);
    if (r) return r;
    a.title[63] = 0;
    int ww = a.w + 2 * WIN_BORDER, wh = a.h + WIN_TITLE_H + 2 * WIN_BORDER;
    int x = a.x, y = a.y;
    if (x < 0 || y < 0 || x > SCREEN_W - 40 || y > SCREEN_H - 40) { x = 60 + 30 * slot; y = 40 + 30 * slot; }
    w->wid = window_create(a.title[0] ? a.title : "window", x, y, ww, wh);
    if (w->wid < 0) { kfree(w->raw); memset(w, 0, sizeof *w); return -LX_ENOMEM; }
    window_set_close_button(w->wid, 1);
    window_set_minimize_button(w->wid, 1);
    window_set_hide_cursor(w->wid, 1);
    window_set_focus(w->wid);
    w->used = 1; w->conn = c; w->last_focus = 1; w->last_w = a.w; w->last_h = a.h;
    window_set_pixels(w->wid, w->pix, 0, 0, a.w, a.h, a.w);
    a.out_wid = w->wid; a.out_stride = a.w * 4;
    if (lx_copy_out(t, uarg, &a, sizeof a)) return -LX_EFAULT;
    return 0;
}

static int64_t do_map(struct lx_task *t, struct okconn *c, uint64_t uarg) {
    struct okw_map a;
    if (lx_copy_in(t, &a, uarg, sizeof a)) return -LX_EFAULT;
    struct okwin *w = by_wid(a.wid);
    if (!w || w->conn != c) return -LX_EINVAL;
    uint32_t len = ((uint32_t)w->w * (uint32_t)w->h * 4 + 0xFFF) & ~0xFFFu;
    if (w->map_mm) { lx_mm_unmap(w->map_mm, w->map_va, w->map_len); lx_mm_release(w->map_mm); w->map_mm = 0; }
    uint32_t va = lx_find_free(t, len);
    if (!va) return -LX_ENOMEM;
    if (lx_map_region(t, va, len, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, 0, 0)) return -LX_ENOMEM;
    struct lx_vma *v = lx_vma_find(t, va);
    if (v) v->shared_fb = 1;
    uint32_t phys = V2P((uint32_t)w->pix);
    for (uint32_t o = 0; o < len; o += 0x1000) if (aspace_map(t->mm->cr3, va + o, phys + o, 1)) return -LX_ENOMEM;
    lx_mm_hold(t->mm);
    w->map_mm = t->mm; w->map_va = va; w->map_len = len;
    a.out_addr = va; a.out_size = len;
    return lx_copy_out(t, uarg, &a, sizeof a);
}

static int64_t do_resize(struct lx_task *t, struct okconn *c, uint64_t uarg) {
    struct okw_resize a;
    if (lx_copy_in(t, &a, uarg, sizeof a)) return -LX_EFAULT;
    struct okwin *w = by_wid(a.wid);
    if (!w || w->conn != c || a.w < 16 || a.h < 16 || a.w > 2048 || a.h > 1200) return -LX_EINVAL;
    uint8_t *old_raw = w->raw;
    struct lx_mm *old_mm = w->map_mm; uint32_t old_va = w->map_va, old_len = w->map_len;
    w->map_mm = 0;
    window_set_pixels(w->wid, 0, 0, 0, 0, 0, 0);
    int r = alloc_buffer(w, a.w, a.h);
    if (r) { w->raw = old_raw; return r; }
    if (old_mm) { lx_mm_unmap(old_mm, old_va, old_len); lx_mm_release(old_mm); }
    kfree(old_raw);
    struct window *win = window_get(w->wid);
    if (win) window_resize(w->wid, a.w + 2 * WIN_BORDER, a.h + WIN_TITLE_H + 2 * WIN_BORDER);
    window_set_pixels(w->wid, w->pix, 0, 0, a.w, a.h, a.w);
    w->last_w = a.w; w->last_h = a.h;
    {   // map the new buffer
        uint32_t len = ((uint32_t)a.w * (uint32_t)a.h * 4 + 0xFFF) & ~0xFFFu;
        uint32_t va = lx_find_free(t, len);
        if (!va || lx_map_region(t, va, len, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, 0, 0)) return -LX_ENOMEM;
        struct lx_vma *v = lx_vma_find(t, va);
        if (v) v->shared_fb = 1;
        uint32_t phys = V2P((uint32_t)w->pix);
        for (uint32_t o = 0; o < len; o += 0x1000) if (aspace_map(t->mm->cr3, va + o, phys + o, 1)) return -LX_ENOMEM;
        lx_mm_hold(t->mm);
        w->map_mm = t->mm; w->map_va = va; w->map_len = len;
        a.out_addr = va;
    }
    return lx_copy_out(t, uarg, &a, sizeof a);
}

// OKW_TEXT: rasterise UTF-8 text with the system font into an 8-bit coverage mask the caller then blends
struct okw_text_k { unsigned long long str; int len; int size; int flags; int family; unsigned long long mask; int cap_w, cap_h; int out_w, out_asc, out_desc, pad; };
static int64_t do_text(struct lx_task *t, uint64_t uarg) {
    struct okw_text_k a;
    if (lx_copy_in(t, &a, uarg, sizeof a)) return -LX_EFAULT;
    if (a.len < 0 || a.len > 1024 || a.size < 4 || a.size > 200) return -LX_EINVAL;
    char s[1025];
    if (a.len && lx_copy_in(t, s, a.str, (uint64_t)a.len)) return -LX_EFAULT;
    struct wfont f = { (uint8_t)(a.family & 3), (uint8_t)((a.flags & 1) != 0), (uint8_t)((a.flags & 2) != 0), (uint16_t)a.size };
    struct wfmetrics m;
    wfont_metrics(&f, &m);
    int32_t adv = a.len ? wfont_measure(&f, s, a.len) : 0;
    a.out_w = (adv + 63) / 64; a.out_asc = (m.ascent + 63) / 64; a.out_desc = (m.descent + 63) / 64;
    if (a.mask && !(a.flags & 4) && a.cap_w > 0 && a.cap_h > 0 && a.len) {
        int cw = a.cap_w, ch = a.cap_h;
        if (cw > 1024) cw = 1024;
        if (ch > 256) ch = 256;
        uint32_t *px = (uint32_t*)kcalloc((uint32_t)cw * (uint32_t)ch, 4);
        uint8_t *mk = (uint8_t*)kmalloc((uint32_t)cw * (uint32_t)ch);
        if (!px || !mk) { if (px) kfree(px); if (mk) kfree(mk); return -LX_ENOMEM; }
        struct wsurf surf = { px, cw, ch, cw, 0, 0, cw, ch };
        wfont_draw(&surf, &f, 0, m.ascent, 0xFFFFFFFFu, s, a.len, 0, 0);
        for (int i = 0; i < cw * ch; i++) mk[i] = (uint8_t)(px[i] & 0xFF);
        for (int y = 0; y < ch && y < a.cap_h; y++)
            if (lx_copy_out(t, a.mask + (uint64_t)y * (uint64_t)a.cap_w, mk + y * cw, (uint64_t)cw)) { kfree(px); kfree(mk); return -LX_EFAULT; }
        kfree(px); kfree(mk);
    }
    return lx_copy_out(t, uarg, &a, sizeof a);
}

int64_t lx_okwin_ioctl(struct lx_task *t, void *conn, uint64_t req, uint64_t arg) {
    struct okconn *c = (struct okconn*)conn;
    switch (req) {
    case OKW_CREATE: return do_create(t, c, arg);
    case OKW_DESTROY: { struct okwin *w = by_wid((int)arg); if (!w || w->conn != c) return -LX_EINVAL; if (grab == (int)(w - wins)) grab = -1; win_free(w); return 0; }
    case OKW_MAP: return do_map(t, c, arg);
    case OKW_RESIZE: return do_resize(t, c, arg);
    case OKW_PRESENT: {
        struct okw_present p;
        if (lx_copy_in(t, &p, arg, sizeof p)) return -LX_EFAULT;
        struct okwin *w = by_wid(p.wid);
        if (!w || w->conn != c) return -LX_EINVAL;
        if (p.w <= 0 || p.h <= 0) { p.x = 0; p.y = 0; p.w = w->w; p.h = w->h; }
        if (p.x < 0) { p.w += p.x; p.x = 0; }
        if (p.y < 0) { p.h += p.y; p.y = 0; }
        if (p.x + p.w > w->w) p.w = w->w - p.x;
        if (p.y + p.h > w->h) p.h = w->h - p.y;
        if (p.w > 0 && p.h > 0) window_dirty_pixels(w->wid, p.x, p.y, p.w, p.h);
        return 0;
    }
    case OKW_SETTITLE: {
        struct okw_title a;
        if (lx_copy_in(t, &a, arg, sizeof a)) return -LX_EFAULT;
        struct okwin *w = by_wid(a.wid);
        if (!w || w->conn != c) return -LX_EINVAL;
        a.title[63] = 0;
        window_set_title(w->wid, a.title);
        return 0;
    }
    case OKW_MOVE: {
        struct okw_move a;
        if (lx_copy_in(t, &a, arg, sizeof a)) return -LX_EFAULT;
        struct okwin *w = by_wid(a.wid);
        struct window *win = w ? window_get(w->wid) : 0;
        if (!win) return -LX_EINVAL;
        { extern int needs_redraw; win->x = a.x; win->y = a.y; win->dirty = 1; needs_redraw = 1; }
        return 0;
    }
    case OKW_SHOW: {
        struct okw_show a;
        if (lx_copy_in(t, &a, arg, sizeof a)) return -LX_EFAULT;
        struct okwin *w = by_wid(a.wid);
        if (!w || w->conn != c) return -LX_EINVAL;
        if (a.show) { window_restore(w->wid); window_raise(w->wid); } else window_minimize(w->wid);
        return 0;
    }
    case OKW_FOCUS: { struct okwin *w = by_wid((int)arg); if (!w || w->conn != c) return -LX_EINVAL; window_set_focus(w->wid); window_raise(w->wid); return 0; }
    case OKW_INFO: {
        struct okw_info i = { SCREEN_W, SCREEN_H, WIN_TITLE_H, WIN_BORDER };
        return lx_copy_out(t, arg, &i, sizeof i);
    }
    case OKW_QUERY: {
        struct okw_query q;
        if (lx_copy_in(t, &q, arg, sizeof q)) return -LX_EFAULT;
        struct okwin *w = by_wid(q.wid);
        struct window *win = w ? window_get(w->wid) : 0;
        q.x = q.y = q.w = q.h = 0; q.focused = 0;
        if (win) { q.x = win->x; q.y = win->y; q.w = win->w - 2 * WIN_BORDER; q.h = win->h - WIN_TITLE_H - 2 * WIN_BORDER; q.focused = window_get_focused() == w->wid; }
        q.mx = mouse_get_x(); q.my = mouse_get_y(); q.mb = mouse_get_buttons();
        return lx_copy_out(t, arg, &q, sizeof q);
    }
    case OKW_TEXT: return do_text(t, arg);
    default: return -LX_ENOTTY;
    }
}

// ---- input, called every pass of the main loop -------------------------------------------------------------------------------

static int mods_now(void) {
    int m = 0;
    if (keyboard_scancode_held(0x2A, 0) || keyboard_scancode_held(0x36, 0)) m |= 1;
    if (keyboard_scancode_held(0x1D, 0) || keyboard_scancode_held(0x1D, 1)) m |= 2;
    if (keyboard_scancode_held(0x38, 0) || keyboard_scancode_held(0x38, 1)) m |= 4;
    return m;
}

static int ext_code(int sc) {
    switch (sc) {
    case 0x48: return 103; case 0x50: return 108; case 0x4B: return 105; case 0x4D: return 106;
    case 0x47: return 102; case 0x4F: return 107; case 0x49: return 104; case 0x51: return 109;
    case 0x52: return 110; case 0x53: return 111; case 0x1D: return 97; case 0x38: return 100; case 0x1C: return 96; case 0x35: return 98;
    default: return 0;
    }
}

static int pending_wheel;
void lx_okwin_wheel(int notches) { pending_wheel += notches; }

void lx_okwin_poll(void) {
    if (!inited) return;
    int any = 0;
    for (int i = 0; i < OKW_MAX; i++) if (wins[i].used) { any = 1; break; }
    if (!any) { pending_wheel = 0; return; }
    int focus = window_get_focused();
    for (int i = 0; i < OKW_MAX; i++) {
        struct okwin *w = &wins[i];
        if (!w->used) continue;
        struct window *win = window_get(w->wid);
        if (!win) { evq(w->conn, OKE_CLOSE, w->wid, 0, 0, 0, 0); w->used = 0; continue; }
        int f = focus == w->wid;
        if (f != w->last_focus) { evq(w->conn, OKE_FOCUS, w->wid, f, 0, 0, 0); w->last_focus = f; }
        int cw = win->w - 2 * WIN_BORDER, ch = win->h - WIN_TITLE_H - 2 * WIN_BORDER;
        if ((cw != w->last_w || ch != w->last_h) && cw >= 16 && ch >= 16) { evq(w->conn, OKE_RESIZE, w->wid, cw, ch, 0, 0); w->last_w = cw; w->last_h = ch; }
    }
    // keys go to the focused window of this kind
    struct okwin *fw = by_wid(focus);
    if (fw) {
        int mods = mods_now();
        for (int sc = 1; sc < 0x59; sc++) {
            int d = keyboard_scancode_held(sc, 0);
            if (d != keyheld[sc]) { keyheld[sc] = (uint8_t)d; evq(fw->conn, OKE_KEY, fw->wid, sc, d, 0, mods); }
        }
        for (int sc = 0; sc < 0x54; sc++) {
            int code = ext_code(sc);
            if (!code) continue;
            int d = keyboard_scancode_held(sc, 1);
            if (d != extheld[sc]) { extheld[sc] = (uint8_t)d; evq(fw->conn, OKE_KEY, fw->wid, code, d, 0, mods); }
        }
    }
    // the pointer
    int mx = mouse_get_x(), my = mouse_get_y(), mb = mouse_get_buttons();
    int target = -1;
    if (grab >= 0 && wins[grab].used) target = grab;
    else {
        int wid = window_from_point(mx, my);
        struct okwin *tw = wid >= 0 ? by_wid(wid) : 0;
        if (tw) target = (int)(tw - wins);
    }
    if (target >= 0) {
        struct okwin *w = &wins[target];
        struct window *win = window_get(w->wid);
        int cx = win ? win->x + WIN_BORDER : 0, cy = win ? win->y + WIN_TITLE_H + WIN_BORDER : 0;
        int px = mx - cx, py = my - cy;
        int inside = px >= 0 && py >= 0 && px < w->w && py < w->h;
        if (inside || grab == target) {
            if (mx != last_mx || my != last_my) evq(w->conn, OKE_MOUSE_MOVE, w->wid, px, py, 0, 0);
            for (int b = 0; b < 3; b++) {
                int now = (mb >> b) & 1, was = (last_mb >> b) & 1;
                if (now != was) {
                    evq(w->conn, OKE_MOUSE_BUTTON, w->wid, px, py, b == 0 ? 1 : b == 1 ? 2 : 3, now);
                    if (now && grab < 0) grab = target;
                }
            }
            if (pending_wheel) evq(w->conn, OKE_WHEEL, w->wid, px, py, -pending_wheel, 0);
        }
    }
    pending_wheel = 0;
    if (!mb) grab = -1;
    last_mx = mx; last_my = my; last_mb = mb;
}

// A typed character (the desktop hands over what it would give to the focused window). 1 = consumed.
int lx_okwin_char(int wid, int c) {
    struct okwin *w = by_wid(wid);
    if (!w) return 0;
    evq(w->conn, OKE_CHAR, w->wid, c & 0xFF, 0, 0, mods_now());
    return 1;
}

// The user clicked the close button: the program decides.
void lx_okwin_close_request(int wid) {
    struct okwin *w = by_wid(wid);
    if (w) evq(w->conn, OKE_CLOSE, w->wid, 0, 0, 0, 0);
}

// wake-up test of a task blocked in read(): block_arg = the descriptor
int lx_okwin_wake(struct lx_task *t) {
    int fd = (int)t->block_arg;
    struct lx_file *f = (fd >= 0 && fd < LX_MAX_FDS) ? t->files->fds[fd] : 0;
    return !f || !f->priv || lx_okwin_ready(f->priv);
}
