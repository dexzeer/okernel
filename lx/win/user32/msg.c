// user32: message queue, /dev/okwin event translation, timers, keyboard state, default window procedure
#include "u32.h"


static MSG q[MSGQ];
static int qn;
static volatile int qlock;
static int quit_posted, quit_code;

static void lock(void) { while (__sync_lock_test_and_set(&qlock, 1)) SC0(24); }       // sched_yield
static void unlock(void) { __sync_lock_release(&qlock); }

// ---- posting --------------------------------------------------------------------------------------------------------------------

void U_post(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    lock();
    if (qn < MSGQ) {
        MSG *x = &q[qn++];
        x->hwnd = h; x->message = m; x->wParam = wp; x->lParam = lp; x->time = GetTickCount(); x->pt.x = x->pt.y = 0;
    }
    unlock();
}
void U_drop_messages(struct wnd *w) {
    lock();
    int j = 0;
    for (int i = 0; i < qn; i++) if (q[i].hwnd != (HWND)w) q[j++] = q[i];
    qn = j;
    unlock();
}
EXPORT BOOL PostMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (h && !U_wnd(h)) return FALSE;
    U_post(h, m, wp, lp); return TRUE;
}
EXPORT BOOL PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) { return PostMessageA(h, m, wp, lp); }
EXPORT BOOL PostThreadMessageA(DWORD tid, UINT m, WPARAM wp, LPARAM lp) { (void)tid; U_post(0, m, wp, lp); return TRUE; }
EXPORT BOOL PostThreadMessageW(DWORD tid, UINT m, WPARAM wp, LPARAM lp) { return PostThreadMessageA(tid, m, wp, lp); }
EXPORT void PostQuitMessage(int code) { quit_posted = 1; quit_code = code; }

// ---- sending --------------------------------------------------------------------------------------------------------------------

LRESULT U_send(struct wnd *w, UINT m, WPARAM wp, LPARAM lp) {
    if (!w || !w->used || !w->proc) return 0;
    return w->proc(w, m, wp, lp);
}
static int ulen_w(const WCHAR *s) { int n = 0; while (s[n]) n++; return n; }
static LRESULT send_conv(HWND h, UINT m, WPARAM wp, LPARAM lp, int from_unicode) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    if (w->unicode != from_unicode) {
        if (m == WM_SETTEXT && lp) {
            if (from_unicode) { char t[256]; WideCharToMultiByte(65001, 0, (LPCWSTR)lp, -1, t, sizeof t, 0, 0); return U_send(w, m, wp, (LPARAM)t); }
            WCHAR t[256]; MultiByteToWideChar(65001, 0, (LPCSTR)lp, -1, t, 256); return U_send(w, m, wp, (LPARAM)t);
        }
        if (m == WM_GETTEXT && lp && wp) {
            if (from_unicode) { char t[256]; LRESULT r = U_send(w, m, wp > 256 ? 256 : wp, (LPARAM)t); MultiByteToWideChar(65001, 0, t, -1, (LPWSTR)lp, (int)wp); return r; }
            WCHAR t[256]; U_send(w, m, wp > 256 ? 256 : wp, (LPARAM)t); return WideCharToMultiByte(65001, 0, t, -1, (LPSTR)lp, (int)wp, 0, 0) - 1;
        }
    }
    (void)ulen_w;
    return U_send(w, m, wp, lp);
}
EXPORT LRESULT SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) { return send_conv(h, m, wp, lp, 0); }
EXPORT LRESULT SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) { return send_conv(h, m, wp, lp, 1); }
EXPORT LRESULT SendMessageTimeoutA(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT f, UINT t, ULONG_PTR *res) { (void)f; (void)t; LRESULT r = SendMessageA(h, m, wp, lp); if (res) *res = (ULONG_PTR)r; return 1; }
EXPORT LRESULT SendMessageTimeoutW(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT f, UINT t, ULONG_PTR *res) { (void)f; (void)t; LRESULT r = SendMessageW(h, m, wp, lp); if (res) *res = (ULONG_PTR)r; return 1; }
EXPORT BOOL SendNotifyMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) { SendMessageA(h, m, wp, lp); return TRUE; }
EXPORT BOOL SendNotifyMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp) { SendMessageW(h, m, wp, lp); return TRUE; }
EXPORT LRESULT CallWindowProcA(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p(h, m, wp, lp); }
EXPORT LRESULT CallWindowProcW(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p(h, m, wp, lp); }
EXPORT BOOL ReplyMessage(LRESULT r) { (void)r; return TRUE; }
EXPORT BOOL InSendMessage(void) { return FALSE; }
EXPORT UINT RegisterWindowMessageA(LPCSTR s) {
    static char names[64][64]; static int n;
    for (int i = 0; i < n; i++) if (!ustricmp(names[i], s)) return 0xC000 + 0x1000 + i;
    if (n >= 64) return 0;
    ustrcpy(names[n], s, 64);
    return 0xD000 + n++;
}
EXPORT UINT RegisterWindowMessageW(LPCWSTR s) { char u[64]; WideCharToMultiByte(65001, 0, s, -1, u, sizeof u, 0, 0); return RegisterWindowMessageA(u); }

// ---- timers ---------------------------------------------------------------------------------------------------------------------

struct timer { int used; struct wnd *w; ULONG_PTR id; DWORD interval, next; void (*proc)(HWND, UINT, ULONG_PTR, DWORD); };
static struct timer timers[64];
static ULONG_PTR next_free_id = 0x1000;

EXPORT ULONG_PTR SetTimer(HWND h, ULONG_PTR id, UINT ms, void (*proc)(HWND, UINT, ULONG_PTR, DWORD)) {
    struct wnd *w = U_wnd(h);
    if (!w) { id = next_free_id++; }
    for (int i = 0; i < 64; i++) if (timers[i].used && timers[i].w == w && timers[i].id == id) { timers[i].interval = ms < 10 ? 10 : ms; timers[i].next = GetTickCount() + timers[i].interval; timers[i].proc = proc; return id; }
    for (int i = 0; i < 64; i++) if (!timers[i].used) {
        timers[i].used = 1; timers[i].w = w; timers[i].id = id; timers[i].interval = ms < 10 ? 10 : ms; timers[i].next = GetTickCount() + timers[i].interval; timers[i].proc = proc;
        return id;
    }
    return 0;
}
EXPORT BOOL KillTimer(HWND h, ULONG_PTR id) {
    struct wnd *w = U_wnd(h);
    for (int i = 0; i < 64; i++) if (timers[i].used && timers[i].w == w && timers[i].id == id) { timers[i].used = 0; return TRUE; }
    return FALSE;
}
static struct timer *timer_due(DWORD now, DWORD *wait) {
    struct timer *best = 0;
    for (int i = 0; i < 64; i++) {
        struct timer *t = &timers[i];
        if (!t->used) continue;
        if (t->w && !t->w->used) { t->used = 0; continue; }
        int d = (int)(t->next - now);
        if (d <= 0) { if (!best || (int)(t->next - best->next) < 0) best = t; }
        else if (wait && (DWORD)d < *wait) *wait = (DWORD)d;
    }
    return best;
}

// ---- keyboard state ---------------------------------------------------------------------------------------------------------------

static unsigned char keys[256];                 // bit 7 = down, bit 0 = toggled
static int vk_of_linux(int c) {
    static const unsigned char row1[] = { 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 8, 9,
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 13, 0xA2, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0,
        0xA0, 0xDC, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0xA1, 0x6A, 0xA4, 32, 0x14,
        0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91,
        0x67, 0x68, 0x69, 0x6D, 0x64, 0x65, 0x66, 0x6B, 0x61, 0x62, 0x63, 0x60, 0x6E };
    if (c >= 0 && c <= 83) return row1[c];
    switch (c) {
    case 87: return 0x7A; case 88: return 0x7B; case 96: return 13; case 97: return 0xA3; case 98: return 0x6F; case 100: return 0xA5;
    case 102: return 36; case 103: return 38; case 104: return 33; case 105: return 37; case 106: return 39; case 107: return 35;
    case 108: return 40; case 109: return 34; case 110: return 45; case 111: return 46;
    default: return 0;
    }
}
static int scan_of_linux(int c) {
    switch (c) {
    case 96: return 0x1C; case 97: return 0x1D; case 98: return 0x35; case 100: return 0x38; case 102: return 0x47; case 103: return 0x48; case 104: return 0x49;
    case 105: return 0x4B; case 106: return 0x4D; case 107: return 0x4F; case 108: return 0x50; case 109: return 0x51; case 110: return 0x52; case 111: return 0x53;
    default: return c & 0x7F;
    }
}
static int is_ext(int c) { return c >= 96; }
static void set_key(int vk, int down) {
    if (!vk) return;
    if (down) { if (!(keys[vk] & 0x80) && (vk == 0x14 || vk == 0x90 || vk == 0x91)) keys[vk] ^= 1; keys[vk] |= 0x80; } else keys[vk] &= ~0x80;
    if (vk == 0xA0 || vk == 0xA1) { keys[16] = (keys[0xA0] | keys[0xA1]) & 0x80; }
    if (vk == 0xA2 || vk == 0xA3) { keys[17] = (keys[0xA2] | keys[0xA3]) & 0x80; }
    if (vk == 0xA4 || vk == 0xA5) { keys[18] = (keys[0xA4] | keys[0xA5]) & 0x80; }
}
EXPORT SHORT GetKeyState(int vk) {
    if (vk == 1) { struct okw_query qq; memset(&qq, 0, sizeof qq); qq.wid = -1; SC3(LX_ioctl, U_fd(), OKW_QUERY, &qq); return (SHORT)((qq.mb & 1) ? 0x8000 : 0); }
    if (vk < 0 || vk > 255) return 0;
    return (SHORT)((keys[vk] & 0x80 ? 0x8000 : 0) | (keys[vk] & 1));
}
EXPORT SHORT GetAsyncKeyState(int vk) {
    if (vk == 1 || vk == 2 || vk == 4) {
        struct okw_query qq; memset(&qq, 0, sizeof qq); qq.wid = -1; SC3(LX_ioctl, U_fd(), OKW_QUERY, &qq);
        int bit = vk == 1 ? 1 : vk == 2 ? 2 : 4;
        return (SHORT)((qq.mb & bit) ? 0x8000 : 0);
    }
    return GetKeyState(vk);
}
EXPORT BOOL GetKeyboardState(unsigned char *out) { for (int i = 0; i < 256; i++) out[i] = keys[i]; return TRUE; }
EXPORT BOOL SetKeyboardState(unsigned char *in) { for (int i = 0; i < 256; i++) keys[i] = in[i]; return TRUE; }
EXPORT UINT MapVirtualKeyA(UINT code, UINT type) {
    if (type == 2) { if (code >= 'A' && code <= 'Z') return code; if (code >= '0' && code <= '9') return code; return 0; }
    if (type == 0) { for (int i = 1; i < 112; i++) if (vk_of_linux(i) == (int)code) return (UINT)scan_of_linux(i); }
    if (type == 1) { for (int i = 1; i < 112; i++) if (scan_of_linux(i) == (int)code) return (UINT)vk_of_linux(i); }
    return 0;
}
EXPORT UINT MapVirtualKeyW(UINT code, UINT type) { return MapVirtualKeyA(code, type); }
EXPORT int GetKeyNameTextA(LONG lp, LPSTR out, int cap) { (void)lp; if (cap > 0) out[0] = 0; return 0; }
EXPORT SHORT VkKeyScanA(CHAR c) { if (c >= 'a' && c <= 'z') return (SHORT)(c - 32); if (c >= 'A' && c <= 'Z') return (SHORT)((c) | 0x100); if (c >= '0' && c <= '9') return c; return (SHORT)-1; }
EXPORT HANDLE GetKeyboardLayout(DWORD t) { (void)t; return (HANDLE)(ULONG_PTR)0x04090409; }
EXPORT int GetKeyboardType(int t) { return t == 0 ? 4 : t == 1 ? 0 : 12; }
EXPORT int ToAscii(UINT vk, UINT sc, const BYTE *st, WORD *out, UINT f) { (void)sc; (void)st; (void)f; if (vk >= 'A' && vk <= 'Z') { *out = (WORD)(vk + ((keys[16] & 0x80) ? 0 : 32)); return 1; } return 0; }

// ---- translating /dev/okwin events ------------------------------------------------------------------------------------------------

static int mouse_down_mask;
static DWORD rep_next; static WPARAM rep_vk; static LPARAM rep_lp; static struct wnd *rep_wnd;
static DWORD last_click_time; static int last_click_x, last_click_y, last_click_btn; static struct wnd *last_click_wnd;

static WPARAM key_flags(int mods) {
    WPARAM f = 0;
    if (mouse_down_mask & 1) f |= MK_LBUTTON;
    if (mouse_down_mask & 2) f |= MK_RBUTTON;
    if (mouse_down_mask & 4) f |= MK_MBUTTON;
    if (mods & 1) f |= MK_SHIFT;
    if (mods & 2) f |= MK_CONTROL;
    return f;
}
static int wants_focus(struct wnd *w) {
    if (w->style & WS_TABSTOP) return 1;
    if (w->cls && w->cls->builtin) {
        if (!ustricmp(w->cls->name, "EDIT") || !ustricmp(w->cls->name, "LISTBOX") || !ustricmp(w->cls->name, "BUTTON")) return 1;
    }
    return 0;
}
static struct wnd *mouse_target(struct wnd *T, int x, int y, int *lx, int *ly) {
    struct wnd *c = U_capture();
    if (c && c->used) {
        if (U_top(c) == T) { int ox, oy; U_origin(c, &ox, &oy); *lx = x - ox; *ly = y - oy; return c; }
        int sx, sy, cx, cy; U_screen_origin(T, &sx, &sy); U_screen_origin(c, &cx, &cy);
        *lx = x + sx - cx; *ly = y + sy - cy; return c;
    }
    struct wnd *h = U_child_from_point(T, x, y - T->mh);            // (x, y) are buffer coordinates, the client area starts below the menu bar
    int ox, oy; U_origin(h, &ox, &oy);
    *lx = x - ox; *ly = y - oy;
    return h;
}
static struct wnd *key_target(struct wnd *T) {
    struct wnd *f = U_focus();
    if (f && f->used && U_top(f) == T) return f;
    return T;
}
void U_activate(struct wnd *T, int on) {
    if (on) {
        struct wnd *prev = U_active();
        if (prev == T) return;
        if (prev && prev->used) U_send(prev, WM_ACTIVATE, WA_INACTIVE, 0);
        U_set_active(T);
        U_send(T, WM_ACTIVATE, WA_ACTIVE, 0);
        struct wnd *f = T->focus_child && T->focus_child->used ? T->focus_child : T;
        U_set_focus(f);
    } else if (U_active() == T) {
        U_send(T, WM_ACTIVATE, WA_INACTIVE, 0);
        U_set_active(0);
        U_set_focus(0);
    }
}

int (*U_event_hook)(const struct okw_event *);
static void handle_event(const struct okw_event *e) {
    if (U_event_hook && U_event_hook(e)) return;                      // a menu is open: it takes the events of the connection
    int i = U_find_wid(e->wid);
    if (i < 0) return;
    struct wnd *T = U_pool(i);
    int mods = e->d;
    switch (e->type) {
    case OKE_MOUSE_MOVE: {
        if (T->menu && !U_capture()) { if (e->b < T->mh) { if (U_menubar_event(T, e)) break; } else U_menubar_leave(T); }
        int lx, ly; struct wnd *t = mouse_target(T, e->a, e->b, &lx, &ly);
        U_post(t, WM_MOUSEMOVE, key_flags(mods_cache()), MAKELPARAM(lx, ly));
        break;
    }
    case OKE_MOUSE_BUTTON: {
        if (T->menu && e->b < T->mh && !U_capture() && U_menubar_event(T, e)) break;
        int lx, ly; struct wnd *t = mouse_target(T, e->a, e->b, &lx, &ly);
        int btn = e->c, down = e->d;
        if (down) mouse_down_mask |= 1 << (btn - 1); else mouse_down_mask &= ~(1 << (btn - 1));
        UINT m;
        if (btn == 1) m = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; else if (btn == 2) m = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; else m = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        if (down) {
            DWORD now = GetTickCount();
            if (last_click_wnd == t && last_click_btn == btn && now - last_click_time < 500 && e->a - last_click_x < 5 && last_click_x - e->a < 5 && e->b - last_click_y < 5 && last_click_y - e->b < 5 && t->cls && (t->cls->style & CS_DBLCLKS)) {
                m = btn == 1 ? WM_LBUTTONDBLCLK : btn == 2 ? 0x206 : 0x209;
                last_click_wnd = 0;
            } else { last_click_wnd = t; last_click_time = now; last_click_x = e->a; last_click_y = e->b; last_click_btn = btn; }
            struct wnd *f = t;
            while (f->parent && !wants_focus(f)) f = f->parent;
            if (f != U_focus() && f->enabled) U_set_focus(f);
            if (U_active() != T) U_activate(T, 1);
        }
        U_post(t, m, key_flags(mods_cache()), MAKELPARAM(lx, ly));
        break;
    }
    case OKE_WHEEL: {
        int lx, ly; struct wnd *t = mouse_target(T, e->a, e->b, &lx, &ly);
        struct wnd *f = U_focus();
        if (f && f->used && U_top(f) == T) t = f;
        int sx, sy; U_screen_origin(T, &sx, &sy);
        int delta = e->c * 120;
        U_post(t, WM_MOUSEWHEEL, (WPARAM)(((unsigned)(delta & 0xFFFF) << 16) | (key_flags(mods_cache()) & 0xFFFF)), MAKELPARAM(sx + e->a, sy - T->mh + e->b));
        break;
    }
    case OKE_KEY: {
        int vk = vk_of_linux(e->a), down = e->b;
        if (!vk) break;
        set_key(vk, down);
        struct wnd *t = key_target(T);
        int alt = (keys[18] & 0x80) != 0;
        UINT m = down ? (alt && vk != 0xA4 && vk != 0xA5 ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
        LPARAM lp = 1 | ((LPARAM)scan_of_linux(e->a) << 16) | (is_ext(e->a) ? 1 << 24 : 0) | (alt ? 1 << 29 : 0) | (down ? 0 : (3 << 30));
        U_post(t, m, (WPARAM)vk, lp);
        if (down && vk != 0xA0 && vk != 0xA1 && vk != 0xA2 && vk != 0xA3 && vk != 0xA4 && vk != 0xA5) { rep_vk = (WPARAM)vk; rep_lp = lp | (1 << 30); rep_next = GetTickCount() + 450; rep_wnd = T; }
        else if (!down && rep_vk == (WPARAM)vk) rep_vk = 0;
        break;
    }
    case OKE_CHAR: {
        int c = e->a;
        if (c == 10) c = 13;
        if (c == 127 || c == 0) break;
        U_post(key_target(T), WM_CHAR, (WPARAM)c, 1);
        break;
    }
    case OKE_FOCUS: U_activate(T, e->a != 0); if (!e->a) { rep_vk = 0; for (int k = 0; k < 256; k++) keys[k] &= ~0x80; mouse_down_mask = 0; } break;
    case OKE_CLOSE: U_post(T, WM_CLOSE, 0, 0); break;
    case OKE_RESIZE: {
        if (e->a != T->bw || e->b != T->bh) {
            if (U_okw_resize(T, e->a, e->b) == 0) {
                { int title, border, sw, sh; U_info(&title, &border, &sw, &sh); T->w = T->bw + 2 * border; T->h = T->bh + (T->noframe ? 0 : title) + 2 * border; }
                U_post(T, WM_SIZE, SIZE_RESTORED, MAKELPARAM(T->cw, T->ch));
                U_invalidate(T, 0, 1);
            }
        }
        break;
    }
    }
}
int mods_cache(void) {
    int m = 0;
    if (keys[16] & 0x80) m |= 1;
    if (keys[17] & 0x80) m |= 2;
    if (keys[18] & 0x80) m |= 4;
    return m;
}

struct pollfd_ { int fd; short events, revents; };
// read pending events; wait up to ms (-1 = forever, 0 = do not wait). Returns 1 if anything was read.
int U_pump(int ms) {
    int fd = U_fd();
    if (fd < 0) { if (ms > 0) { struct { long long s, n; } ts = { ms / 1000, (long long)(ms % 1000) * 1000000 }; SC2(LX_nanosleep, &ts, 0); } return 0; }
    struct pollfd_ p = { fd, 1, 0 };
    long r = SC3(7, &p, 1, ms);
    if (r <= 0 || !(p.revents & 1)) return 0;
    struct okw_event ev[32];
    int any = 0;
    for (;;) {
        long n = SC3(LX_read, fd, ev, sizeof ev);
        if (n <= 0) break;
        for (long k = 0; k + (long)sizeof(struct okw_event) <= n; k += sizeof(struct okw_event)) { handle_event(&ev[k / sizeof(struct okw_event)]); any = 1; }
        p.revents = 0;
        if (SC3(7, &p, 1, 0) <= 0 || !(p.revents & 1)) break;
    }
    return any;
}

// ---- retrieving ---------------------------------------------------------------------------------------------------------------------

static int match(const MSG *m, HWND f, UINT lo, UINT hi) {
    if (f && m->hwnd != f && !(m->hwnd && IsChild(f, m->hwnd))) return 0;
    if (lo || hi) { if (m->message < lo || (hi && m->message > hi)) return 0; }
    return 1;
}
static int take_posted(MSG *out, HWND f, UINT lo, UINT hi, int remove) {
    int found = 0;
    lock();
    for (int i = 0; i < qn; i++) {
        if (q[i].hwnd && !U_wnd(q[i].hwnd)) continue;
        if (!match(&q[i], f, lo, hi)) continue;
        *out = q[i];
        if (remove) { for (int j = i; j + 1 < qn; j++) q[j] = q[j + 1]; qn--; }
        found = 1; break;
    }
    unlock();
    return found;
}
static void fill_pt(MSG *m) {
    struct okw_query qq; memset(&qq, 0, sizeof qq); qq.wid = -1;
    if (SC3(LX_ioctl, U_fd(), OKW_QUERY, &qq) == 0) { m->pt.x = qq.mx; m->pt.y = qq.my; }
}

static int get_msg(MSG *m, HWND f, UINT lo, UINT hi, int remove, int block) {
    U_init();
    for (;;) {
        if (U_pump(0)) continue;
        DWORD now = GetTickCount();
        if (rep_vk && now - rep_next < 0x80000000u) {
            struct wnd *t = key_target(rep_wnd && rep_wnd->used ? rep_wnd : 0);
            if (t) U_post(t, (UINT)((keys[18] & 0x80) ? WM_SYSKEYDOWN : WM_KEYDOWN), rep_vk, rep_lp);
            rep_next = now + 33;
            continue;
        }
        if (take_posted(m, f, lo, hi, remove)) { fill_pt(m); return m->message == WM_QUIT ? 0 : 1; }
        if (quit_posted && (!f)) {
            if (remove) quit_posted = 0;
            m->hwnd = 0; m->message = WM_QUIT; m->wParam = (WPARAM)quit_code; m->lParam = 0; m->time = now; m->pt.x = m->pt.y = 0;
            return 0;
        }
        struct wnd *pw = U_next_invalid(0);
        if (pw && (!f || pw == f || IsChild(f, pw)) && (!lo || (lo <= WM_PAINT && (!hi || hi >= WM_PAINT)))) {
            m->hwnd = pw; m->message = WM_PAINT; m->wParam = 0; m->lParam = 0; m->time = now; fill_pt(m);
            return 1;
        }
        DWORD wait = 0xFFFFFFFFu;
        struct timer *t = timer_due(now, &wait);
        if (t && (!lo || (lo <= WM_TIMER && (!hi || hi >= WM_TIMER))) && (!f || t->w == f)) {
            t->next = now + t->interval;
            m->hwnd = t->w; m->message = WM_TIMER; m->wParam = t->id; m->lParam = (LPARAM)t->proc; m->time = now; fill_pt(m);
            return 1;
        }
        if (!block) return -1;
        int ms = wait == 0xFFFFFFFFu ? -1 : (int)wait;
        if (rep_vk) { int d = (int)(rep_next - now); if (d < 1) d = 1; if (ms < 0 || d < ms) ms = d; }
        U_pump(ms < 0 ? -1 : ms < 1 ? 1 : ms);
    }
}
EXPORT BOOL GetMessageA(MSG *m, HWND f, UINT lo, UINT hi) { int r = get_msg(m, f, lo, hi, 1, 1); return r < 0 ? -1 : r; }
EXPORT BOOL GetMessageW(MSG *m, HWND f, UINT lo, UINT hi) { return GetMessageA(m, f, lo, hi); }
EXPORT BOOL PeekMessageA(MSG *m, HWND f, UINT lo, UINT hi, UINT flags) {
    int r = get_msg(m, f, lo, hi, (flags & PM_REMOVE) != 0, 0);
    if (r < 0) return FALSE;
    if (m->message == WM_QUIT) return TRUE;
    return TRUE;
}
EXPORT BOOL PeekMessageW(MSG *m, HWND f, UINT lo, UINT hi, UINT flags) { return PeekMessageA(m, f, lo, hi, flags); }
EXPORT BOOL WaitMessage(void) { MSG m; get_msg(&m, 0, 0, 0, 0, 1); return TRUE; }
EXPORT DWORD GetQueueStatus(UINT f) { (void)f; U_pump(0); lock(); int n = qn; unlock(); return n ? 0x00040004u : 0; }
EXPORT BOOL TranslateMessage(const MSG *m) { (void)m; return TRUE; }
EXPORT BOOL IsDialogMessageA(HWND h, MSG *m) { (void)h; (void)m; return FALSE; }
EXPORT BOOL IsDialogMessageW(HWND h, MSG *m) { (void)h; (void)m; return FALSE; }
EXPORT LRESULT DispatchMessageA(const MSG *m) {
    if (m->message == WM_TIMER && m->lParam) { ((void (*)(HWND, UINT, ULONG_PTR, DWORD))m->lParam)(m->hwnd, WM_TIMER, m->wParam, m->time); return 0; }
    struct wnd *w = U_wnd(m->hwnd);
    if (!w) return 0;
    if (m->message == WM_PAINT) {
        U_send(w, WM_PAINT, 0, 0);
        w->has_inval = 0;                     // a procedure that did not validate would be asked forever
        return 0;
    }
    return U_send(w, m->message, m->wParam, m->lParam);
}
EXPORT LRESULT DispatchMessageW(const MSG *m) { return DispatchMessageA(m); }
EXPORT DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD mask) {
    extern DWORD WaitForMultipleObjects(DWORD, const HANDLE*, BOOL, DWORD);
    (void)mask;
    DWORD start = GetTickCount();
    for (;;) {
        if (U_pump(0)) return n;
        lock(); int have = qn; unlock();
        if (have || U_next_invalid(0)) return n;
        if (n) { DWORD r = WaitForMultipleObjects(n, hs, all, 0); if (r != 0x102) return r; }
        if (ms != INFINITE && GetTickCount() - start >= ms) return 0x102;
        U_pump(n ? 2 : (ms == INFINITE ? -1 : (int)(ms - (GetTickCount() - start))));
    }
}
EXPORT DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD mask, DWORD fl) { (void)fl; return MsgWaitForMultipleObjects(n, hs, FALSE, ms, mask); }
EXPORT DWORD GetMessageTime(void) { return GetTickCount(); }
EXPORT LONG GetMessagePos(void) { struct okw_query qq; memset(&qq, 0, sizeof qq); qq.wid = -1; SC3(LX_ioctl, U_fd(), OKW_QUERY, &qq); return (LONG)MAKELPARAM(qq.mx, qq.my); }

// ---- cursor and misc input ----------------------------------------------------------------------------------------------------------

EXPORT BOOL GetCursorPos(POINT *p) {
    struct okw_query qq; memset(&qq, 0, sizeof qq); qq.wid = -1;
    if (SC3(LX_ioctl, U_fd(), OKW_QUERY, &qq) < 0) return FALSE;
    p->x = qq.mx; p->y = qq.my; return TRUE;
}
EXPORT BOOL SetCursorPos(int x, int y) { (void)x; (void)y; return TRUE; }
EXPORT HCURSOR SetCursor(HCURSOR c) { static HCURSOR cur; HCURSOR o = cur; cur = c; return o; }
EXPORT HCURSOR GetCursor(void) { return (HCURSOR)1; }
EXPORT int ShowCursor(BOOL s) { static int cnt; cnt += s ? 1 : -1; return cnt; }
EXPORT BOOL ClipCursor(const RECT *r) { (void)r; return TRUE; }
EXPORT BOOL TrackMouseEvent(void *t) { (void)t; return TRUE; }
EXPORT UINT GetDoubleClickTime(void) { return 500; }
EXPORT BOOL SwapMouseButton(BOOL s) { (void)s; return FALSE; }
EXPORT BOOL CreateCaret(HWND h, HBITMAP b, int w, int hh) { (void)h; (void)b; (void)w; (void)hh; return TRUE; }
EXPORT BOOL DestroyCaret(void) { return TRUE; }
EXPORT BOOL ShowCaret(HWND h) { (void)h; return TRUE; }
EXPORT BOOL HideCaret(HWND h) { (void)h; return TRUE; }
EXPORT BOOL SetCaretPos(int x, int y) { (void)x; (void)y; return TRUE; }
EXPORT BOOL MessageBeep(UINT t) { (void)t; return TRUE; }
EXPORT BOOL FlashWindow(HWND h, BOOL i) { (void)h; (void)i; return TRUE; }
EXPORT HANDLE SetWindowsHookExA(int id, void *proc, HINSTANCE h, DWORD tid) { (void)id; (void)proc; (void)h; (void)tid; return (HANDLE)1; }
EXPORT HANDLE SetWindowsHookExW(int id, void *proc, HINSTANCE h, DWORD tid) { (void)id; (void)proc; (void)h; (void)tid; return (HANDLE)1; }
EXPORT BOOL UnhookWindowsHookEx(HANDLE h) { (void)h; return TRUE; }
EXPORT LRESULT CallNextHookEx(HANDLE h, int c, WPARAM w, LPARAM l) { (void)h; (void)c; (void)w; (void)l; return 0; }
EXPORT BOOL AttachThreadInput(DWORD a, DWORD b, BOOL f) { (void)a; (void)b; (void)f; return TRUE; }
EXPORT BOOL GetLastInputInfo(void *p) { (void)p; return FALSE; }

// ---- default procedure ---------------------------------------------------------------------------------------------------------------------

extern void U_erase_background(struct wnd *w, HDC dc);
extern HDC BeginPaint(HWND h, PAINTSTRUCT *ps);
extern BOOL EndPaint(HWND h, const PAINTSTRUCT *ps);

LRESULT U_def_proc(struct wnd *w, UINT m, WPARAM wp, LPARAM lp, int unicode) {
    switch (m) {
    case WM_NCCREATE: return TRUE;
    case WM_CLOSE: U_destroy(w); return 0;
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_CLOSE) U_post(w, WM_CLOSE, 0, 0); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(w, &ps); EndPaint(w, &ps); return 0; }
    case WM_ERASEBKGND: U_erase_background(w, (HDC)wp); return 1;
    case WM_SETTEXT: {
        char t[256];
        if (unicode) WideCharToMultiByte(65001, 0, (LPCWSTR)lp, -1, t, sizeof t, 0, 0); else ustrcpy(t, (LPCSTR)lp, sizeof t);
        U_set_text(w, t);
        return TRUE;
    }
    case WM_GETTEXT: {
        if (!wp) return 0;
        if (unicode) return MultiByteToWideChar(65001, 0, w->text, -1, (LPWSTR)lp, (int)wp) - 1;
        ustrcpy((char*)lp, w->text, (int)wp);
        return ustrlen((char*)lp);
    }
    case WM_GETTEXTLENGTH: return ustrlen(w->text);
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return 1;
    case WM_SETCURSOR: return 0;
    case WM_SHOWWINDOW: return 0;
    default: return 0;
    }
}
EXPORT LRESULT DefWindowProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) { struct wnd *w = U_wnd(h); return w ? U_def_proc(w, m, wp, lp, 0) : 0; }
EXPORT LRESULT DefWindowProcW(HWND h, UINT m, WPARAM wp, LPARAM lp) { struct wnd *w = U_wnd(h); return w ? U_def_proc(w, m, wp, lp, 1) : 0; }
EXPORT LRESULT DefDlgProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, m, wp, lp); }
EXPORT LRESULT DefDlgProcW(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, m, wp, lp); }
EXPORT LRESULT DefFrameProcA(HWND h, HWND c, UINT m, WPARAM wp, LPARAM lp) { (void)c; return DefWindowProcA(h, m, wp, lp); }
EXPORT LRESULT DefMDIChildProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, m, wp, lp); }
