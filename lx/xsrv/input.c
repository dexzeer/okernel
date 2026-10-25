// Input: okwin events -> X events (keys, buttons, motion, crossing, focus, close), grabs, event delivery.
#include "xsrv.h"

struct win *focus_win;                  // SetInputFocus target: 0 = None/PointerRoot behaviour
struct win *ptr_win;
struct win *grab_ptr_win, *grab_kb_win;
struct client *grab_ptr_client, *grab_kb_client;
int grab_ptr_implicit, grab_ptr_owner_events; u32 grab_ptr_mask;
int ptr_root_x, ptr_root_y; u32 ptr_state;
static struct win *kb_top;              // the top-level window that has the desktop's keyboard focus
static int ptr_lx, ptr_ly;              // pointer position in its top-level window
static struct win *ptr_top;
static int mod_state;                   // X modifier bits (shift 1, control 4, mod1 8)
static int btn_state;                   // X button bits 0x100..0x1000

// ---- keymap (US layout; X keycode = Linux key code + 8) -------------------------------------------------------------------------------
static u32 ksym[256][2];

static void kset(int lin, u32 a, u32 b) { ksym[lin + 8][0] = a; ksym[lin + 8][1] = b; }

void input_init(void) {
    static const char row_num[] = "1234567890-=", row_num_s[] = "!@#$%^&*()_+";
    for (int i = 0; i < 12; i++) kset(2 + i, (u32)row_num[i], (u32)row_num_s[i]);
    static const char r1[] = "qwertyuiop[]", r1s[] = "QWERTYUIOP{}";
    for (int i = 0; i < 12; i++) kset(16 + i, (u32)r1[i], (u32)r1s[i]);
    static const char r2[] = "asdfghjkl;'`", r2s[] = "ASDFGHJKL:\"~";
    for (int i = 0; i < 12; i++) kset(30 + i, (u32)r2[i], (u32)r2s[i]);
    kset(43, '\\', '|');
    static const char r3[] = "zxcvbnm,./", r3s[] = "ZXCVBNM<>?";
    for (int i = 0; i < 10; i++) kset(44 + i, (u32)r3[i], (u32)r3s[i]);
    kset(1, 0xff1b, 0xff1b); kset(14, 0xff08, 0xff08); kset(15, 0xff09, 0xfe20); kset(28, 0xff0d, 0xff0d);
    kset(29, 0xffe3, 0xffe3); kset(42, 0xffe1, 0xffe1); kset(54, 0xffe2, 0xffe2); kset(56, 0xffe9, 0xffe9);
    kset(97, 0xffe4, 0xffe4); kset(100, 0xffea, 0xffea);
    kset(57, ' ', ' '); kset(58, 0xffe5, 0xffe5);
    for (int i = 0; i < 10; i++) kset(59 + i, 0xffbe + (u32)i, 0xffbe + (u32)i);
    kset(87, 0xffc8, 0xffc8); kset(88, 0xffc9, 0xffc9);
    kset(102, 0xff50, 0xff50); kset(103, 0xff52, 0xff52); kset(104, 0xff55, 0xff55); kset(105, 0xff51, 0xff51);
    kset(106, 0xff53, 0xff53); kset(107, 0xff57, 0xff57); kset(108, 0xff54, 0xff54); kset(109, 0xff56, 0xff56);
    kset(110, 0xff63, 0xff63); kset(111, 0xffff, 0xffff);
    kset(55, 0xffaa, 0xffaa); kset(74, 0xffad, 0xffad); kset(78, 0xffab, 0xffab); kset(98, 0xffaf, 0xffaf); kset(96, 0xff8d, 0xff8d);
    kset(83, 0xffae, 0xffae);
}

u32 keysym_for(int code, int idx);
u32 keysym_for(int code, int idx) { return code >= 0 && code < 256 && idx >= 0 && idx < 2 ? ksym[code][idx] : 0; }

// ---- delivery ---------------------------------------------------------------------------------------------------------------------------
int win_has_mask(struct win *w, u32 mask) {
    for (struct evsel *s = w->sel; s; s = s->next) if (s->mask & mask) return 1;
    return 0;
}

void ev_to_win(struct win *w, u32 mask, u8 *ev, int woff, int propagate) {
    for (struct win *cur = w; cur; cur = cur->parent) {
        int n = 0;
        for (struct evsel *s = cur->sel; s; s = s->next)
            if (s->mask & mask) { if (woff >= 0) p32(ev, woff, cur->id); ev_to_client(s->c, ev); n++; }
        if (n || !propagate || (cur->dnp & mask)) return;
    }
}

static struct client *first_selector(struct win *w, u32 mask) {
    for (struct evsel *s = w->sel; s; s = s->next) if (s->mask & mask) return s->c;
    return 0;
}

static void stamp_xy(u8 *ev, struct win *evwin, struct win *child, struct win *top, int lx, int ly) {
    int ax, ay;
    win_abs(evwin, &ax, &ay);
    p32(ev, 12, evwin->id);
    p32(ev, 16, child ? child->id : 0);
    p16(ev, 24, (u32)(lx - ax)); p16(ev, 26, (u32)(ly - ay));
    (void)top;
}

// deliver a pointer/key event starting at `start`, climbing to the parents until a client has selected `mask`; returns the window delivered on
static struct win *deliver_xy(struct win *start, u32 mask, u8 *ev, struct win *top, int lx, int ly, struct client *only, int propagate) {
    struct win *child = 0;
    for (struct win *cur = start; cur; child = cur, cur = cur->parent) {
        int n = 0;
        for (struct evsel *s = cur->sel; s; s = s->next)
            if ((s->mask & mask) && (!only || s->c == only)) { stamp_xy(ev, cur, child, top, lx, ly); ev_to_client(s->c, ev); n++; }
        if (n) return cur;
        if (!propagate || (cur->dnp & mask) || cur == top) return 0;
    }
    return 0;
}

static void base_ev(u8 *ev, int type, int detail, struct win *top, u32 state) {
    memset(ev, 0, 32);
    ev[0] = (u8)type; ev[1] = (u8)detail;
    p32(ev, 4, srv_time()); p32(ev, 8, XRoot);
    p16(ev, 20, (u32)ptr_root_x); p16(ev, 22, (u32)ptr_root_y);
    p16(ev, 28, state); ev[30] = 1;
    (void)top;
}

static u32 cur_state(void) { ptr_state = (u32)mod_state | (u32)btn_state; return ptr_state; }

static struct win *top_of_wid(int wid) {
    for (struct win *w = root->first; w; w = w->next) if (w->wid == wid) return w;
    return 0;
}

// ---- pointer ------------------------------------------------------------------------------------------------------------------------------
static void pointer_pos(struct win *t, int lx, int ly) {
    struct okw_query q;
    memset(&q, 0, sizeof q);
    q.wid = t->wid;
    if (okw_ioctl(OKW_QUERY, &q) == 0) { ptr_root_x = q.mx; ptr_root_y = q.my; t->rx0 = q.mx - lx; t->ry0 = q.my - ly; }
    else { ptr_root_x = t->rx0 + lx; ptr_root_y = t->ry0 + ly; }
    ptr_lx = lx; ptr_ly = ly; ptr_top = t;
}

static void crossing(struct win *t, struct win *nw, int lx, int ly) {
    if (nw == ptr_win) return;
    struct win *old = ptr_win;
    ptr_win = nw;
    u8 ev[32];
    if (old && res_get(old->id, RES_WINDOW) == old) {
        base_ev(ev, EV_Leave, 0, t, cur_state());
        ev[31] = 1;
        deliver_xy(old, LeaveWindowMask, ev, win_top(old), ptr_lx, ptr_ly, 0, 0);
    }
    if (nw) {
        base_ev(ev, EV_Enter, 0, t, cur_state());
        ev[31] = 1;
        deliver_xy(nw, EnterWindowMask, ev, t, lx, ly, 0, 0);
    }
}

static int deliver_grabbed(int type, int detail, u32 evmask, struct win *t, int lx, int ly) {
    u8 ev[32];
    base_ev(ev, type, detail, t, cur_state());
    if (grab_ptr_implicit) return deliver_xy(grab_ptr_win, evmask, ev, win_top(grab_ptr_win), lx, ly, grab_ptr_client, 0) != 0;
    // explicit grab: owner_events first (windows of the grabbing client under the pointer), then the grab window with the grab's mask
    if (grab_ptr_owner_events && ptr_win && deliver_xy(ptr_win, evmask, ev, t, lx, ly, grab_ptr_client, 1)) return 1;
    if (!(grab_ptr_mask & evmask)) return 0;
    stamp_xy(ev, grab_ptr_win, 0, win_top(grab_ptr_win), lx, ly);
    ev_to_client(grab_ptr_client, ev);
    return 1;
}

static void on_motion(struct win *t, int lx, int ly) {
    pointer_pos(t, lx, ly);
    struct win *w = win_at(t, lx, ly);
    if (!grab_ptr_win) crossing(t, w, lx, ly);
    else ptr_win = w;
    u32 m = PointerMotionMask;
    if (btn_state) {
        m |= ButtonMotionMask;
        for (int b = 0; b < 5; b++) if (btn_state & (0x100 << b)) m |= (u32)Button1MotionMask << b;
    }
    if (grab_ptr_win) { deliver_grabbed(EV_Motion, 0, m, t, lx, ly); return; }
    u8 ev[32];
    base_ev(ev, EV_Motion, 0, t, cur_state());
    deliver_xy(w, m, ev, t, lx, ly, 0, 1);
}

static void on_button(struct win *t, int lx, int ly, int xb, int down) {
    pointer_pos(t, lx, ly);
    struct win *w = win_at(t, lx, ly);
    if (!grab_ptr_win) crossing(t, w, lx, ly);
    u32 bit = 0x100u << (xb - 1);
    u8 ev[32];
    if (down) {
        u32 st = cur_state();
        btn_state |= (int)bit;
        base_ev(ev, EV_ButtonPress, xb, t, st);                              // state is that before the press
        if (grab_ptr_win) { deliver_grabbed(EV_ButtonPress, xb, ButtonPressMask, t, lx, ly); return; }
        struct win *got = deliver_xy(w, ButtonPressMask, ev, t, lx, ly, 0, 1);
        if (got) {
            grab_ptr_win = got; grab_ptr_client = first_selector(got, ButtonPressMask); grab_ptr_implicit = 1;
        }
    } else {
        u32 st = cur_state();
        btn_state &= ~(int)bit;
        base_ev(ev, EV_ButtonRelease, xb, t, st);
        if (grab_ptr_win) {
            u8 e2[32];
            memcpy(e2, ev, 32);
            if (grab_ptr_implicit) deliver_xy(grab_ptr_win, ButtonReleaseMask, e2, win_top(grab_ptr_win), lx, ly, grab_ptr_client, 0);
            else deliver_grabbed(EV_ButtonRelease, xb, ButtonReleaseMask, t, lx, ly);
            if (grab_ptr_implicit && !btn_state) { grab_ptr_win = 0; grab_ptr_client = 0; grab_ptr_implicit = 0; }
            return;
        }
        deliver_xy(w, ButtonReleaseMask, ev, t, lx, ly, 0, 1);
    }
}

static void on_wheel(struct win *t, int lx, int ly, int notches) {
    pointer_pos(t, lx, ly);
    int b = notches > 0 ? 4 : 5, n = notches > 0 ? notches : -notches;
    struct win *w = win_at(t, lx, ly);
    for (int i = 0; i < n && i < 8; i++)
        for (int down = 1; down >= 0; down--) {
            u8 ev[32];
            base_ev(ev, down ? EV_ButtonPress : EV_ButtonRelease, b, t, cur_state());
            if (grab_ptr_win && !grab_ptr_implicit) deliver_grabbed(down ? EV_ButtonPress : EV_ButtonRelease, b, down ? ButtonPressMask : ButtonReleaseMask, t, lx, ly);
            else deliver_xy(w, down ? ButtonPressMask : ButtonReleaseMask, ev, t, lx, ly, 0, 1);
        }
}

// ---- keyboard -----------------------------------------------------------------------------------------------------------------------------
static void on_key(struct win *t, int code, int down, int mods) {
    mod_state = ((mods & 1) ? 1 : 0) | ((mods & 2) ? 4 : 0) | ((mods & 4) ? 8 : 0);
    if (code <= 0 || code > 247) return;
    struct win *k = kb_top ? kb_top : t;
    if (!k) return;
    u8 ev[32];
    base_ev(ev, down ? EV_KeyPress : EV_KeyRelease, code + 8, k, cur_state());
    struct win *target;
    struct client *only = 0;
    if (grab_kb_win && win_viewable(grab_kb_win)) { target = grab_kb_win; only = grab_kb_client; }
    else if (focus_win && win_viewable(focus_win)) target = focus_win;
    else if (ptr_win && win_top(ptr_win) == k) target = ptr_win;
    else target = k;
    struct win *tt = win_top(target);
    int lx = ptr_top == tt ? ptr_lx : 0, ly = ptr_top == tt ? ptr_ly : 0;
    if (only) {
        u8 e2[32]; memcpy(e2, ev, 32);
        stamp_xy(e2, target, 0, tt, lx, ly);
        ev_to_client(only, e2);
        return;
    }
    deliver_xy(target, down ? KeyPressMask : KeyReleaseMask, ev, tt, lx, ly, 0, 1);
}

static void focus_ev(struct win *w, int in, int mode) {
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = (u8)(in ? EV_FocusIn : EV_FocusOut); ev[1] = 3; ev[8] = (u8)mode;
    ev_to_win(w, FocusChangeMask, ev, 4, 0);
}

static void on_focus(struct win *t, int gained) {
    if (gained) {
        kb_top = t;
        focus_ev(t, 1, 0);
        if (focus_win && win_top(focus_win) == t && focus_win != t) focus_ev(focus_win, 1, 0);
        // the modifier state starts clean
    } else {
        if (kb_top == t) kb_top = 0;
        if (focus_win && win_top(focus_win) == t && focus_win != t) focus_ev(focus_win, 0, 0);
        focus_ev(t, 0, 0);
        mod_state = 0;
    }
}

static void on_close(struct win *t) {
    if (t->wm_delete) {
        u8 ev[32];
        memset(ev, 0, 32);
        ev[0] = EV_ClientMessage; ev[1] = 32;
        p32(ev, 4, t->id); p32(ev, 8, a_wm_protocols); p32(ev, 12, a_wm_delete); p32(ev, 16, srv_time());
        struct client *c = t->owner;
        if (c) ev_to_client(c, ev);
        else ev_to_win(t, 0xFFFFFFFFu, ev, -1, 0);
    } else if (t->owner) {
        t->owner->dead = 1;                                     // no WM_DELETE_WINDOW: close the connection like a window manager's kill
    }
}

void input_poll(void) {
    struct okw_event evs[32];
    i64 n = sys_read(okfd, evs, sizeof evs);
    if (n <= 0) return;
    for (int i = 0; i < (int)(n / (i64)sizeof evs[0]); i++) {
        struct okw_event *e = &evs[i];
        struct win *t = top_of_wid(e->wid);
        if (!t) continue;
        switch (e->type) {
        case OKE_MOUSE_MOVE: on_motion(t, e->a, e->b); break;
        case OKE_MOUSE_BUTTON: on_button(t, e->a, e->b, e->c == 1 ? 1 : e->c == 2 ? 3 : 2, e->d != 0); break;
        case OKE_WHEEL: on_wheel(t, e->a, e->b, e->c); break;
        case OKE_KEY: on_key(t, e->a, e->b != 0, e->d); break;
        case OKE_FOCUS: on_focus(t, e->a != 0); break;
        case OKE_CLOSE: on_close(t); break;
        case OKE_RESIZE: {
            u32 v[2] = { (u32)e->a, (u32)e->b };
            if (e->a != t->bw || e->b != t->bh) win_configure(t, 4 | 8, v, 0);
            break;
        }
        }
    }
    flush_clients();
}

int input_query_pointer(void) { return ptr_root_x; }

void input_win_gone(struct win *w) {
    if (!w) return;
    if (grab_ptr_win == w) { grab_ptr_win = 0; grab_ptr_client = 0; grab_ptr_implicit = 0; }
    if (grab_kb_win == w) { grab_kb_win = 0; grab_kb_client = 0; }
    if (focus_win == w) focus_win = 0;
    if (ptr_win == w) ptr_win = 0;
    if (kb_top == w) kb_top = 0;
    if (ptr_top == w) ptr_top = 0;
}

void input_client_gone(struct client *c) {
    if (grab_ptr_client == c) { grab_ptr_win = 0; grab_ptr_client = 0; grab_ptr_implicit = 0; }
    if (grab_kb_client == c) { grab_kb_win = 0; grab_kb_client = 0; }
}

