// user32: window classes, the window tree, creation and destruction, geometry, focus
#include "u32.h"

static int fd = -2;
static int t_title = 24, t_border = 2, scr_w = 1920, scr_h = 1080;
static struct wnd pool[MAXW];
static struct cls *classes;
static int natoms;
static int inited;
static struct wnd *g_focus, *g_capture, *g_active;
int U_fd(void);

int U_fd(void) {
    if (fd == -2) {
        fd = (int)SC3(LX_open, "/dev/okwin", O_RDWR, 0);
        if (fd >= 0) {
            struct okw_info i;
            if (SC3(LX_ioctl, fd, OKW_INFO, &i) == 0) { t_title = i.title_h; t_border = i.border; scr_w = i.screen_w; scr_h = i.screen_h; }
        }
    }
    return fd;
}
void U_info(int *title, int *border, int *sw, int *sh) { U_fd(); *title = t_title; *border = t_border; *sw = scr_w; *sh = scr_h; }

static void init(void) {
    if (inited) return;
    inited = 1;
    U_fd();
    pool[0].used = 1; pool[0].visible = 1; pool[0].enabled = 1; pool[0].cw = pool[0].w = scr_w; pool[0].ch = pool[0].h = scr_h; pool[0].wid = -1;
    U_register_builtin();
}
void U_init(void) { init(); }

struct wnd *U_wnd(HWND h) { struct wnd *w = h; return (w >= pool && w < pool + MAXW && w->used) ? w : 0; }
struct wnd *U_desktop(void) { init(); return &pool[0]; }
struct wnd *U_pool(int i) { return i >= 0 && i < MAXW ? &pool[i] : 0; }
struct wnd *U_focus(void) { return g_focus; }
struct wnd *U_capture(void) { return g_capture; }
void U_set_capture(struct wnd *w) { g_capture = w; }
struct wnd *U_active(void) { return g_active; }
void U_set_active(struct wnd *w) { g_active = w; }
void U_clear_focus_refs(struct wnd *w) { if (g_focus == w) g_focus = 0; if (g_capture == w) g_capture = 0; if (g_active == w) g_active = 0; }

struct wnd *U_top(struct wnd *w) { while (w && w->parent) w = w->parent; return w; }
void U_origin(struct wnd *w, int *ox, int *oy) {
    int x = 0, y = 0;
    for (; w && w->parent; w = w->parent) { x += w->x + w->bd; y += w->y + w->bd; }
    if (w) y += w->mh;                                              // the top level: its menu bar is above the client area
    *ox = x; *oy = y;
}
// client rectangle of w clipped by its ancestors, in buffer coordinates
void U_clip(struct wnd *w, RECT *r) {
    int ox, oy; U_origin(w, &ox, &oy);
    r->left = ox; r->top = oy; r->right = ox + w->cw; r->bottom = oy + w->ch;
    for (struct wnd *p = w->parent; p; p = p->parent) {
        int px, py; U_origin(p, &px, &py);
        if (r->left < px) r->left = px;
        if (r->top < py) r->top = py;
        if (r->right > px + p->cw) r->right = px + p->cw;
        if (r->bottom > py + p->ch) r->bottom = py + p->ch;
    }
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
}

// ---- classes --------------------------------------------------------------------------------------------------------------

static struct cls *find_class(const char *name) {
    for (struct cls *c = classes; c; c = c->next) if (!ustricmp(c->name, name)) return c;
    return 0;
}
static struct cls *find_atom(ULONG_PTR a) { for (struct cls *c = classes; c; c = c->next) if (c->atom == (int)a) return c; return 0; }

struct cls *U_add_class(const char *name, WNDPROC proc, UINT style, int cbcls, int cbwnd, HBRUSH bg, HINSTANCE inst, int unicode, int builtin) {
    struct cls *c = find_class(name);
    if (c && !builtin) { set_error(1410); return 0; }                    // ERROR_CLASS_ALREADY_EXISTS
    c = umalloc(sizeof *c);
    if (!c) return 0;
    ustrcpy(c->name, name, sizeof c->name);
    c->proc = proc; c->style = style; c->cbcls = cbcls; c->cbwnd = cbwnd; c->bg = bg; c->inst = inst; c->unicode = unicode; c->builtin = builtin;
    c->atom = 0xC000 + natoms++;
    c->next = classes; classes = c;
    return c;
}
static int w2u(const WCHAR *w, char *out, int cap) {
    if (!w) { out[0] = 0; return 0; }
    int n = WideCharToMultiByte(65001, 0, w, -1, out, cap, 0, 0);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}
static int u2w(const char *s, WCHAR *out, int cap) {
    int n = MultiByteToWideChar(65001, 0, s, -1, out, cap);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

EXPORT ATOM RegisterClassExA(const WNDCLASSEXA *c) {
    init();
    char nm[64];
    if ((ULONG_PTR)c->lpszClassName < 0x10000) return 0;
    ustrcpy(nm, c->lpszClassName, sizeof nm);
    struct cls *k = U_add_class(nm, c->lpfnWndProc, c->style, c->cbClsExtra, c->cbWndExtra, c->hbrBackground, c->hInstance, 0, 0);
    if (!k) return 0;
    k->cursor = c->hCursor; k->icon = c->hIcon;
    return (ATOM)k->atom;
}
EXPORT ATOM RegisterClassExW(const WNDCLASSEXW *c) {
    init();
    char nm[64];
    if ((ULONG_PTR)c->lpszClassName < 0x10000) return 0;
    w2u(c->lpszClassName, nm, sizeof nm);
    struct cls *k = U_add_class(nm, c->lpfnWndProc, c->style, c->cbClsExtra, c->cbWndExtra, c->hbrBackground, c->hInstance, 1, 0);
    if (!k) return 0;
    k->cursor = c->hCursor; k->icon = c->hIcon;
    return (ATOM)k->atom;
}
EXPORT ATOM RegisterClassA(const WNDCLASSA *c) {
    WNDCLASSEXA e = { sizeof e, c->style, c->lpfnWndProc, c->cbClsExtra, c->cbWndExtra, c->hInstance, c->hIcon, c->hCursor, c->hbrBackground, c->lpszMenuName, c->lpszClassName, 0 };
    return RegisterClassExA(&e);
}
EXPORT ATOM RegisterClassW(const WNDCLASSW *c) {
    WNDCLASSEXW e = { sizeof e, c->style, c->lpfnWndProc, c->cbClsExtra, c->cbWndExtra, c->hInstance, c->hIcon, c->hCursor, c->hbrBackground, c->lpszMenuName, c->lpszClassName, 0 };
    return RegisterClassExW(&e);
}
EXPORT BOOL UnregisterClassA(LPCSTR name, HINSTANCE h) {
    (void)h;
    for (struct cls **p = &classes; *p; p = &(*p)->next)
        if ((ULONG_PTR)name < 0x10000 ? (*p)->atom == (int)(ULONG_PTR)name : !ustricmp((*p)->name, name)) { struct cls *k = *p; *p = k->next; ufree(k); return TRUE; }
    return FALSE;
}
EXPORT BOOL UnregisterClassW(LPCWSTR name, HINSTANCE h) { char u[64]; if ((ULONG_PTR)name < 0x10000) return UnregisterClassA((LPCSTR)name, h); w2u(name, u, sizeof u); return UnregisterClassA(u, h); }
static BOOL class_info(struct cls *k, HINSTANCE h, WNDCLASSEXA *out, int unicode) {
    (void)h;
    if (!k) return FALSE;
    out->cbSize = sizeof *out; out->style = k->style; out->lpfnWndProc = k->proc; out->cbClsExtra = k->cbcls; out->cbWndExtra = k->cbwnd; out->hInstance = k->inst;
    out->hIcon = k->icon; out->hCursor = k->cursor; out->hbrBackground = k->bg; out->lpszMenuName = 0; out->lpszClassName = unicode ? 0 : k->name; out->hIconSm = 0;
    return TRUE;
}
EXPORT BOOL GetClassInfoExA(HINSTANCE h, LPCSTR name, WNDCLASSEXA *out) {
    init();
    struct cls *k = (ULONG_PTR)name < 0x10000 ? find_atom((ULONG_PTR)name) : find_class(name);
    return class_info(k, h, out, 0);
}
EXPORT BOOL GetClassInfoExW(HINSTANCE h, LPCWSTR name, WNDCLASSEXW *out) {
    init();
    char u[64]; struct cls *k;
    if ((ULONG_PTR)name < 0x10000) k = find_atom((ULONG_PTR)name); else { w2u(name, u, sizeof u); k = find_class(u); }
    WNDCLASSEXA a;
    if (!class_info(k, h, &a, 1)) return FALSE;
    out->cbSize = sizeof *out; out->style = a.style; out->lpfnWndProc = a.lpfnWndProc; out->cbClsExtra = a.cbClsExtra; out->cbWndExtra = a.cbWndExtra; out->hInstance = a.hInstance;
    out->hIcon = a.hIcon; out->hCursor = a.hCursor; out->hbrBackground = a.hbrBackground; out->lpszMenuName = 0; out->lpszClassName = 0; out->hIconSm = 0;
    return TRUE;
}
EXPORT BOOL GetClassInfoA(HINSTANCE h, LPCSTR name, WNDCLASSA *out) {
    WNDCLASSEXA e; if (!GetClassInfoExA(h, name, &e)) return FALSE;
    out->style = e.style; out->lpfnWndProc = e.lpfnWndProc; out->cbClsExtra = e.cbClsExtra; out->cbWndExtra = e.cbWndExtra; out->hInstance = e.hInstance; out->hIcon = e.hIcon; out->hCursor = e.hCursor; out->hbrBackground = e.hbrBackground; out->lpszMenuName = 0; out->lpszClassName = e.lpszClassName;
    return TRUE;
}
EXPORT int GetClassNameA(HWND h, LPSTR out, int cap) {
    struct wnd *w = U_wnd(h);
    if (!w || !w->cls || cap <= 0) return 0;
    ustrcpy(out, w->cls->name, cap);
    return ustrlen(out);
}
EXPORT int GetClassNameW(HWND h, LPWSTR out, int cap) {
    struct wnd *w = U_wnd(h);
    if (!w || !w->cls || cap <= 0) return 0;
    return u2w(w->cls->name, out, cap);
}

// ---- tree -----------------------------------------------------------------------------------------------------------------

static void link_child(struct wnd *p, struct wnd *w) {
    w->parent = p; w->prev = p->last; w->next = 0;
    if (p->last) p->last->next = w; else p->first = w;
    p->last = w;
}
static void unlink_child(struct wnd *w) {
    struct wnd *p = w->parent;
    if (!p) return;
    if (w->prev) w->prev->next = w->next; else p->first = w->next;
    if (w->next) w->next->prev = w->prev; else p->last = w->prev;
    w->prev = w->next = 0;
}

// the buffer of a top-level window gets the size cw x bh (the client area is below the menu bar)
int U_okw_resize(struct wnd *w, int cw, int bh) {
    struct okw_resize r; r.wid = w->wid; r.w = cw; r.h = bh; r.pad = 0; r.out_addr = 0;
    if (SC3(LX_ioctl, U_fd(), OKW_RESIZE, &r) < 0) return -1;
    w->px = (unsigned*)r.out_addr; w->stride = cw; w->bw = cw; w->bh = bh; w->cw = cw; w->ch = bh - w->mh;
    U_menubar_paint(w);
    return 0;
}

static const char *cname_of(const void *c, int unicode, char *buf, int cap) {
    if ((ULONG_PTR)c < 0x10000) return 0;
    if (unicode) { w2u(c, buf, cap); return buf; }
    return c;
}

HWND U_create_window(DWORD ex, const void *cls, const void *name, DWORD style, int x, int y, int w, int h, HWND hparent, HMENU menu, HINSTANCE inst, LPVOID param, int unicode) {
    init();
    char cb[64], nb[256];
    struct cls *k;
    if ((ULONG_PTR)cls < 0x10000) k = find_atom((ULONG_PTR)cls);
    else { const char *cn = cname_of(cls, unicode, cb, sizeof cb); k = find_class(cn); }
    if (!k) { set_error(1407); return 0; }
    struct wnd *par = hparent ? U_wnd(hparent) : 0;
    if ((style & WS_CHILD) && !par) { set_error(1400); return 0; }
    int slot = -1;
    for (int i = 1; i < MAXW; i++) if (!pool[i].used) { slot = i; break; }
    if (slot < 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    struct wnd *me = &pool[slot];
    memset(me, 0, sizeof *me);
    me->used = 1; me->cls = k; me->proc = k->proc; me->style = style; me->exstyle = ex; me->unicode = unicode; me->inst = inst;
    me->enabled = !(style & WS_DISABLED); me->tid = GetCurrentThreadId(); me->wid = -1; me->id = (LONG_PTR)menu;
    if (name) { if (unicode) w2u(name, nb, sizeof nb); else ustrcpy(nb, name, sizeof nb); ustrcpy(me->text, (unicode || (ULONG_PTR)name >= 0x10000) ? nb : "", sizeof me->text); }
    if (k->cbwnd > 0) { me->extra = umalloc((SIZE_T)k->cbwnd + 8); me->nextra = k->cbwnd; }

    if (style & WS_CHILD) {
        link_child(par, me);
        me->bd = (ex & WS_EX_CLIENTEDGE) ? 2 : (style & WS_BORDER) ? 1 : 0;
        if (x == CW_USEDEFAULT) x = 0;
        if (y == CW_USEDEFAULT) y = 0;
        if (w == CW_USEDEFAULT) w = 0;
        if (h == CW_USEDEFAULT) h = 0;
        me->x = x; me->y = y; me->w = w; me->h = h;
        me->cw = w - 2 * me->bd; me->ch = h - 2 * me->bd;
        if (me->cw < 0) me->cw = 0;
        if (me->ch < 0) me->ch = 0;
    } else {
        me->owner = par;
        int noframe = (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION;     // no title bar (the desktop still draws a thin frame)
        if (w == CW_USEDEFAULT) w = 640;
        if (h == CW_USEDEFAULT) h = 480;
        struct menu *mn = menu ? U_menu(menu) : 0;                                   // hMenu of a top-level window is its menu
        int mh = mn ? U_menubar_height() : 0;
        int cw = w - 2 * t_border, ch = h - (noframe ? 0 : t_title) - 2 * t_border;  // = the size of the pixel buffer
        if (cw < 16) cw = 16;
        if (ch < 16) ch = 16;
        if (cw > 2048) cw = 2048;
        if (ch > 1200) ch = 1200;
        me->menu = mn; me->mh = mh; me->noframe = noframe; if (mn) me->id = 0;
        if (x == CW_USEDEFAULT) x = -1;
        if (y == CW_USEDEFAULT) y = -1;
        struct okw_create c;
        memset(&c, 0, sizeof c);
        c.x = x; c.y = y; c.w = cw; c.h = ch; c.flags = noframe ? OKW_F_NOFRAME : 0;
        ustrcpy(c.title, me->text, sizeof c.title);
        if (SC3(LX_ioctl, U_fd(), OKW_CREATE, &c) < 0) { me->used = 0; ufree(me->extra); set_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
        struct okw_map m; memset(&m, 0, sizeof m); m.wid = c.out_wid;
        if (SC3(LX_ioctl, U_fd(), OKW_MAP, &m) < 0) { SC3(LX_ioctl, U_fd(), OKW_DESTROY, c.out_wid); me->used = 0; ufree(me->extra); return 0; }
        me->wid = c.out_wid; me->px = (unsigned*)m.out_addr; me->stride = cw; me->bw = cw; me->bh = ch;
        me->x = x; me->y = y; me->cw = cw; me->ch = ch - mh;
        me->w = cw + 2 * t_border; me->h = ch + (noframe ? 0 : t_title) + 2 * t_border;
        U_menubar_paint(me);
        if (!(style & WS_VISIBLE)) { struct okw_show s = { me->wid, 0 }; SC3(LX_ioctl, U_fd(), OKW_SHOW, &s); }
    }

    CREATESTRUCTA cs;
    memset(&cs, 0, sizeof cs);
    cs.lpCreateParams = param; cs.hInstance = inst; cs.hMenu = menu; cs.hwndParent = hparent; cs.cy = h; cs.cx = w; cs.y = y; cs.x = x; cs.style = (LONG)style;
    cs.lpszName = name; cs.lpszClass = cls; cs.dwExStyle = ex;
    if (U_send(me, WM_NCCREATE, 0, (LPARAM)&cs) == 0) { U_destroy(me); return 0; }
    if (U_send(me, WM_CREATE, 0, (LPARAM)&cs) == (LRESULT)-1) { U_destroy(me); return 0; }
    U_send(me, WM_SIZE, SIZE_RESTORED, MAKELPARAM(me->cw, me->ch));
    U_send(me, WM_MOVE, 0, MAKELPARAM(me->x, me->y));
    if (style & WS_VISIBLE) ShowWindow(me, SW_SHOW);
    return me;
}

EXPORT HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    return U_create_window(ex, cls, name, style, x, y, w, h, parent, menu, inst, param, 0);
}
EXPORT HWND CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    return U_create_window(ex, cls, name, style, x, y, w, h, parent, menu, inst, param, 1);
}

void U_destroy(struct wnd *w) {
    if (!w || !w->used || w->in_destroy) return;
    w->in_destroy = 1;
    if (g_focus == w || (g_focus && U_top(g_focus) == w)) U_set_focus(w->parent ? w->parent : 0);
    U_send(w, WM_DESTROY, 0, 0);
    while (w->last) U_destroy(w->last);
    U_send(w, WM_NCDESTROY, 0, 0);
    if (!w->parent && w->wid >= 0) { SC3(LX_ioctl, U_fd(), OKW_DESTROY, w->wid); }
    if (w->parent) {
        RECT r = { w->x, w->y, w->x + w->w, w->y + w->h };
        struct wnd *p = w->parent;
        unlink_child(w);
        U_invalidate(p, &r, 1);                       // r is in the parent's client coordinates, minus its border
    }
    U_clear_focus_refs(w);
    if (w->ctl) ufree(w->ctl);
    ufree(w->extra);
    U_drop_messages(w);
    w->used = 0;
}
EXPORT BOOL DestroyWindow(HWND h) { struct wnd *w = U_wnd(h); if (!w) return FALSE; U_destroy(w); return TRUE; }
EXPORT BOOL CloseWindow(HWND h) { (void)h; return TRUE; }

// ---- properties -------------------------------------------------------------------------------------------------------------

EXPORT BOOL IsWindow(HWND h) { return U_wnd(h) != 0; }
EXPORT BOOL IsWindowVisible(HWND h) {
    for (struct wnd *w = U_wnd(h); w; w = w->parent) if (!w->visible) return FALSE; else if (!w->parent) return TRUE;
    return FALSE;
}
EXPORT BOOL IsWindowEnabled(HWND h) { struct wnd *w = U_wnd(h); return w && w->enabled; }
EXPORT BOOL IsWindowUnicode(HWND h) { struct wnd *w = U_wnd(h); return w && w->unicode; }
EXPORT BOOL IsIconic(HWND h) { (void)h; return FALSE; }
EXPORT BOOL IsZoomed(HWND h) { (void)h; return FALSE; }
EXPORT BOOL IsChild(HWND p, HWND c) { for (struct wnd *w = U_wnd(c); w; w = w->parent) if (w->parent == (struct wnd*)p) return TRUE; return FALSE; }
EXPORT HWND GetParent(HWND h) { struct wnd *w = U_wnd(h); return w ? (w->parent ? w->parent : (HWND)w->owner) : 0; }
EXPORT HWND SetParent(HWND h, HWND np) {
    struct wnd *w = U_wnd(h), *p = U_wnd(np);
    if (!w || !w->parent) return 0;
    struct wnd *old = w->parent;
    unlink_child(w); link_child(p ? p : old, w);
    U_invalidate(old, 0, 1); U_invalidate(w->parent, 0, 1);
    return old;
}
EXPORT HWND GetAncestor(HWND h, UINT flags) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    if (flags == 1) return w->parent;                                  // GA_PARENT
    return U_top(w);                                                  // GA_ROOT
}
EXPORT HWND GetDesktopWindow(void) { return U_desktop(); }
EXPORT HWND GetTopWindow(HWND h) { struct wnd *w = U_wnd(h); return w ? w->last : 0; }
EXPORT HWND GetWindow(HWND h, UINT cmd) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    switch (cmd) {
    case 0: { struct wnd *f = w; while (f->prev) f = f->prev; return f; }                 // GW_HWNDFIRST
    case 1: { struct wnd *f = w; while (f->next) f = f->next; return f; }                 // GW_HWNDLAST
    case 2: return w->prev;                                                               // GW_HWNDNEXT (z order: later = on top)
    case 3: return w->next;
    case 4: return w->owner;                                                              // GW_OWNER
    case 5: return w->last;                                                               // GW_CHILD
    default: return 0;
    }
}
EXPORT LONG_PTR GetWindowLongPtrA(HWND h, int i) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    switch (i) {
    case GWL_STYLE: return (LONG)w->style;
    case GWL_EXSTYLE: return (LONG)w->exstyle;
    case GWLP_USERDATA: return w->userdata;
    case GWLP_WNDPROC: return (LONG_PTR)w->proc;
    case GWLP_ID: return w->id;
    case -6: return (LONG_PTR)w->inst;                              // GWLP_HINSTANCE
    case -8: return (LONG_PTR)w->owner;                             // GWLP_HWNDPARENT
    default: if (i >= 0 && i + 8 <= w->nextra) return *(LONG_PTR*)((char*)w->extra + i); return 0;
    }
}
EXPORT LONG_PTR GetWindowLongPtrW(HWND h, int i) { return GetWindowLongPtrA(h, i); }
EXPORT LONG GetWindowLongA(HWND h, int i) {
    struct wnd *w = U_wnd(h);
    if (w && i >= 0 && i + 4 <= w->nextra) return *(LONG*)((char*)w->extra + i);
    return (LONG)GetWindowLongPtrA(h, i);
}
EXPORT LONG GetWindowLongW(HWND h, int i) { return GetWindowLongA(h, i); }
EXPORT LONG_PTR SetWindowLongPtrA(HWND h, int i, LONG_PTR v) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    LONG_PTR old = GetWindowLongPtrA(h, i);
    switch (i) {
    case GWL_STYLE: w->style = (DWORD)v; break;
    case GWL_EXSTYLE: w->exstyle = (DWORD)v; break;
    case GWLP_USERDATA: w->userdata = v; break;
    case GWLP_WNDPROC: w->proc = (WNDPROC)v; break;
    case GWLP_ID: w->id = v; break;
    default: if (i >= 0 && i + 8 <= w->nextra) *(LONG_PTR*)((char*)w->extra + i) = v; break;
    }
    return old;
}
EXPORT LONG_PTR SetWindowLongPtrW(HWND h, int i, LONG_PTR v) { return SetWindowLongPtrA(h, i, v); }
EXPORT LONG SetWindowLongA(HWND h, int i, LONG v) {
    struct wnd *w = U_wnd(h);
    if (w && i >= 0 && i + 4 <= w->nextra) { LONG o = *(LONG*)((char*)w->extra + i); *(LONG*)((char*)w->extra + i) = v; return o; }
    return (LONG)SetWindowLongPtrA(h, i, (LONG_PTR)(unsigned)v);
}
EXPORT LONG SetWindowLongW(HWND h, int i, LONG v) { return SetWindowLongA(h, i, v); }
EXPORT DWORD GetClassLongA(HWND h, int i) {
    struct wnd *w = U_wnd(h);
    if (!w || !w->cls) return 0;
    switch (i) { case -26: return w->cls->style; case -10: return (DWORD)(ULONG_PTR)w->cls->bg; case -12: return (DWORD)(ULONG_PTR)w->cls->cursor; case -14: return (DWORD)(ULONG_PTR)w->cls->icon; case -24: return (DWORD)w->cls->atom; case -8: return (DWORD)(ULONG_PTR)w->cls->proc; default: return 0; }
}
EXPORT DWORD GetClassLongW(HWND h, int i) { return GetClassLongA(h, i); }
EXPORT ULONG_PTR GetClassLongPtrA(HWND h, int i) {
    struct wnd *w = U_wnd(h);
    if (!w || !w->cls) return 0;
    switch (i) { case -10: return (ULONG_PTR)w->cls->bg; case -12: return (ULONG_PTR)w->cls->cursor; case -14: return (ULONG_PTR)w->cls->icon; case -24: return (ULONG_PTR)w->cls->atom; case -8: return (ULONG_PTR)w->cls->proc; default: return GetClassLongA(h, i); }
}
EXPORT ULONG_PTR GetClassLongPtrW(HWND h, int i) { return GetClassLongPtrA(h, i); }
EXPORT ULONG_PTR SetClassLongPtrA(HWND h, int i, ULONG_PTR v) {
    struct wnd *w = U_wnd(h);
    if (!w || !w->cls) return 0;
    ULONG_PTR old = GetClassLongPtrA(h, i);
    switch (i) { case -10: w->cls->bg = (HBRUSH)v; break; case -12: w->cls->cursor = (HCURSOR)v; break; case -14: w->cls->icon = (HICON)v; break; case -8: w->cls->proc = (WNDPROC)v; break; default: break; }
    return old;
}
EXPORT ULONG_PTR SetClassLongPtrW(HWND h, int i, ULONG_PTR v) { return SetClassLongPtrA(h, i, v); }
EXPORT DWORD SetClassLongA(HWND h, int i, LONG v) { return (DWORD)SetClassLongPtrA(h, i, (ULONG_PTR)(unsigned)v); }
EXPORT DWORD SetClassLongW(HWND h, int i, LONG v) { return SetClassLongA(h, i, v); }

EXPORT int GetWindowTextA(HWND h, LPSTR out, int cap) {
    struct wnd *w = U_wnd(h);
    if (!w || cap <= 0) return 0;
    out[0] = 0;
    return (int)SendMessageA(h, WM_GETTEXT, (WPARAM)cap, (LPARAM)out);
}
EXPORT int GetWindowTextW(HWND h, LPWSTR out, int cap) {
    struct wnd *w = U_wnd(h);
    if (!w || cap <= 0) return 0;
    out[0] = 0;
    return (int)SendMessageW(h, WM_GETTEXT, (WPARAM)cap, (LPARAM)out);
}
EXPORT int GetWindowTextLengthA(HWND h) { return U_wnd(h) ? (int)SendMessageA(h, WM_GETTEXTLENGTH, 0, 0) : 0; }
EXPORT int GetWindowTextLengthW(HWND h) { return U_wnd(h) ? (int)SendMessageW(h, WM_GETTEXTLENGTH, 0, 0) : 0; }
void U_set_text(struct wnd *w, const char *s) {
    ustrcpy(w->text, s, sizeof w->text);
    if (!w->parent && w->wid >= 0) { struct okw_title t; t.wid = w->wid; ustrcpy(t.title, s, sizeof t.title); SC3(LX_ioctl, U_fd(), OKW_SETTITLE, &t); }
    else U_invalidate(w, 0, 1);
}
EXPORT BOOL SetWindowTextA(HWND h, LPCSTR s) {
    if (!U_wnd(h)) return FALSE;
    SendMessageA(h, WM_SETTEXT, 0, (LPARAM)s);
    return TRUE;
}
EXPORT BOOL SetWindowTextW(HWND h, LPCWSTR s) {
    if (!U_wnd(h)) return FALSE;
    SendMessageW(h, WM_SETTEXT, 0, (LPARAM)s);
    return TRUE;
}

EXPORT int GetDlgCtrlID(HWND h) { struct wnd *w = U_wnd(h); return w ? (int)w->id : 0; }
EXPORT HWND GetDlgItem(HWND h, int id) {
    struct wnd *w = U_wnd(h);
    if (!w) return 0;
    for (struct wnd *c = w->first; c; c = c->next) if (c->id == id) return c;
    return 0;
}
EXPORT BOOL SetDlgItemTextA(HWND h, int id, LPCSTR s) { HWND c = GetDlgItem(h, id); return c ? SetWindowTextA(c, s) : FALSE; }
EXPORT UINT GetDlgItemTextA(HWND h, int id, LPSTR out, int cap) { HWND c = GetDlgItem(h, id); return c ? (UINT)GetWindowTextA(c, out, cap) : 0; }

// ---- geometry -----------------------------------------------------------------------------------------------------------------

static void sync_top(struct wnd *w) {
    if (w->parent || w->wid < 0) return;
    struct okw_query q; memset(&q, 0, sizeof q); q.wid = w->wid;
    if (SC3(LX_ioctl, U_fd(), OKW_QUERY, &q) == 0) { w->x = q.x; w->y = q.y; }
}
void U_screen_origin(struct wnd *w, int *sx, int *sy) {             // screen position of the client origin
    struct wnd *t = U_top(w);
    int ox, oy; U_origin(w, &ox, &oy);
    sync_top(t);
    *sx = t->x + t_border + ox;
    *sy = t->y + (t->noframe ? 0 : t_title) + t_border + oy;
}
EXPORT BOOL GetClientRect(HWND h, RECT *r) { struct wnd *w = U_wnd(h); if (!w) return FALSE; r->left = r->top = 0; r->right = w->cw; r->bottom = w->ch; return TRUE; }
EXPORT BOOL GetWindowRect(HWND h, RECT *r) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    if (!w->parent) { sync_top(w); r->left = w->x; r->top = w->y; r->right = w->x + w->w; r->bottom = w->y + w->h; return TRUE; }
    int sx, sy; U_screen_origin(w, &sx, &sy);
    r->left = sx - w->bd; r->top = sy - w->bd; r->right = r->left + w->w; r->bottom = r->top + w->h;
    return TRUE;
}
EXPORT BOOL ClientToScreen(HWND h, POINT *p) { struct wnd *w = U_wnd(h); if (!w) return FALSE; int sx, sy; U_screen_origin(w, &sx, &sy); p->x += sx; p->y += sy; return TRUE; }
EXPORT BOOL ScreenToClient(HWND h, POINT *p) { struct wnd *w = U_wnd(h); if (!w) return FALSE; int sx, sy; U_screen_origin(w, &sx, &sy); p->x -= sx; p->y -= sy; return TRUE; }
EXPORT int MapWindowPoints(HWND from, HWND to, POINT *p, UINT n) {
    int fx = 0, fy = 0, tx = 0, ty = 0;
    struct wnd *a = U_wnd(from), *b = U_wnd(to);
    if (a) U_screen_origin(a, &fx, &fy);
    if (b) U_screen_origin(b, &tx, &ty);
    for (UINT i = 0; i < n; i++) { p[i].x += fx - tx; p[i].y += fy - ty; }
    return MAKELPARAM(fx - tx, fy - ty);
}
EXPORT BOOL AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex) {
    (void)ex;
    init();
    int noframe = (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION;
    r->left -= t_border; r->right += t_border; r->top -= (noframe ? 0 : t_title) + t_border; r->bottom += t_border;
    if (menu) r->top -= U_menubar_height();
    return TRUE;
}
EXPORT BOOL AdjustWindowRect(RECT *r, DWORD style, BOOL menu) { return AdjustWindowRectEx(r, style, menu, 0); }

static void apply_size(struct wnd *w, int cw, int ch, int notify) {
    if (cw == w->cw && ch == w->ch) return;
    if (!w->parent && w->wid >= 0) {
        if (cw < 16) cw = 16;
        if (ch < 16) ch = 16;
        if (cw > 2048) cw = 2048;
        if (ch > 1200) ch = 1200;
        if (U_okw_resize(w, cw, ch + w->mh)) return;
        w->w = cw + 2 * t_border; w->h = ch + w->mh + (w->noframe ? 0 : t_title) + 2 * t_border;
    } else { w->cw = cw; w->ch = ch; w->w = cw + 2 * w->bd; w->h = ch + 2 * w->bd; }
    if (notify) U_send(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->cw, w->ch));
    U_invalidate(w, 0, 1);
}
EXPORT BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL repaint) {
    struct wnd *k = U_wnd(h);
    if (!k) return FALSE;
    if (!k->parent) {
        struct okw_move m = { k->wid, x, y };
        SC3(LX_ioctl, U_fd(), OKW_MOVE, &m);
        k->x = x; k->y = y;
        apply_size(k, w - 2 * t_border, hh - (k->noframe ? 0 : t_title) - 2 * t_border - k->mh, 1);
        U_send(k, WM_MOVE, 0, MAKELPARAM(0, 0));
    } else {
        RECT old = { k->x, k->y, k->x + k->w, k->y + k->h };
        U_invalidate(k->parent, &old, 1);
        k->x = x; k->y = y;
        int ow = k->cw, oh = k->ch;
        apply_size(k, w - 2 * k->bd, hh - 2 * k->bd, 0);
        if (ow != k->cw || oh != k->ch) U_send(k, WM_SIZE, SIZE_RESTORED, MAKELPARAM(k->cw, k->ch));
        U_send(k, WM_MOVE, 0, MAKELPARAM(x, y));
        U_invalidate(k, 0, 1);
        if (repaint) { RECT nr = { x, y, x + w, hh + y }; U_invalidate(k->parent, &nr, 1); }
    }
    return TRUE;
}
#define SWP_NOSIZE 1
#define SWP_NOMOVE 2
#define SWP_NOZORDER 4
#define SWP_SHOWWINDOW 0x40
#define SWP_HIDEWINDOW 0x80
EXPORT BOOL SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT flags) {
    struct wnd *k = U_wnd(h);
    if (!k) return FALSE;
    (void)after;
    RECT r; GetWindowRect(h, &r);
    int ox = k->parent ? k->x : r.left, oy = k->parent ? k->y : r.top, ow = k->w, oh = k->h;
    if (flags & SWP_NOMOVE) { x = ox; y = oy; }
    if (flags & SWP_NOSIZE) { w = ow; hh = oh; }
    if (!(flags & SWP_NOMOVE && flags & SWP_NOSIZE)) MoveWindow(h, x, y, w, hh, TRUE);
    if (flags & SWP_SHOWWINDOW) ShowWindow(h, SW_SHOW);
    if (flags & SWP_HIDEWINDOW) ShowWindow(h, SW_HIDE);
    if (after == (HWND)(LONG_PTR)-1 && !k->parent && k->wid >= 0) { SC3(LX_ioctl, U_fd(), OKW_FOCUS, k->wid); }
    return TRUE;
}
EXPORT BOOL BringWindowToTop(HWND h) {
    struct wnd *k = U_wnd(h);
    if (!k) return FALSE;
    if (!k->parent && k->wid >= 0) SC3(LX_ioctl, U_fd(), OKW_FOCUS, k->wid);
    else if (k->parent && k->parent->last != k) { struct wnd *p = k->parent; unlink_child(k); link_child(p, k); U_invalidate(k, 0, 1); }
    return TRUE;
}
EXPORT BOOL ShowWindow(HWND h, int cmd) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    int was = w->visible, show = cmd != SW_HIDE && cmd != SW_MINIMIZE;
    w->visible = show;
    if (!w->parent && w->wid >= 0) {
        struct okw_show s = { w->wid, show };
        SC3(LX_ioctl, U_fd(), OKW_SHOW, &s);
    }
    if (was != show) {
        U_send(w, WM_SHOWWINDOW, show, 0);
        if (w->parent) {
            RECT r = { w->x, w->y, w->x + w->w, w->y + w->h };
            U_invalidate(w->parent, &r, 1);
        }
    }
    if (show) {
        U_invalidate(w, 0, 1);
        if (!w->parent) { w->style |= WS_VISIBLE; if (!w->activated) { w->activated = 1; if (w->wid >= 0) SC3(LX_ioctl, U_fd(), OKW_FOCUS, w->wid); U_activate(w, 1); } }
    }
    return was;
}
EXPORT BOOL ShowWindowAsync(HWND h, int c) { return ShowWindow(h, c); }
EXPORT BOOL EnableWindow(HWND h, BOOL e) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    int was = !w->enabled;
    w->enabled = e != 0;
    if (w->enabled) w->style &= ~WS_DISABLED; else w->style |= WS_DISABLED;
    if (was != !w->enabled) { U_send(w, WM_ENABLE, e != 0, 0); U_invalidate(w, 0, 1); }
    return was;
}
EXPORT BOOL SetForegroundWindow(HWND h) {
    struct wnd *w = U_wnd(h);
    if (!w) return FALSE;
    struct wnd *t = U_top(w);
    if (t->wid >= 0) SC3(LX_ioctl, U_fd(), OKW_FOCUS, t->wid);
    return TRUE;
}
EXPORT HWND GetForegroundWindow(void) { return g_active; }
EXPORT HWND GetActiveWindow(void) { return g_active; }
EXPORT HWND SetActiveWindow(HWND h) { HWND o = g_active; SetForegroundWindow(h); return o; }
EXPORT HWND GetFocus(void) { return g_focus; }
EXPORT HWND SetFocus(HWND h) {
    HWND old = g_focus;
    struct wnd *w = U_wnd(h);
    if (w == g_focus) return old;
    U_set_focus(w);
    return old;
}
void U_set_focus(struct wnd *w) {
    struct wnd *old = g_focus;
    if (w && !w->enabled) return;
    g_focus = w;
    if (old && old->used) U_send(old, WM_KILLFOCUS, (WPARAM)w, 0);
    if (w) {
        struct wnd *t = U_top(w);
        t->focus_child = w == t ? 0 : w;
        U_send(w, WM_SETFOCUS, (WPARAM)old, 0);
    }
}
EXPORT HWND SetCapture(HWND h) { HWND o = g_capture; g_capture = U_wnd(h); return o; }
EXPORT BOOL ReleaseCapture(void) { g_capture = 0; return TRUE; }
EXPORT HWND GetCapture(void) { return g_capture; }

// ---- searching ----------------------------------------------------------------------------------------------------------------

EXPORT BOOL EnumWindows(BOOL (*cb)(HWND, LPARAM), LPARAM lp) {
    for (int i = 1; i < MAXW; i++) if (pool[i].used && !pool[i].parent) if (!cb(&pool[i], lp)) return TRUE;
    return TRUE;
}
EXPORT BOOL EnumChildWindows(HWND p, BOOL (*cb)(HWND, LPARAM), LPARAM lp) {
    struct wnd *w = U_wnd(p);
    if (!w) return FALSE;
    for (struct wnd *c = w->first; c; c = c->next) { if (!cb(c, lp)) return TRUE; EnumChildWindows(c, cb, lp); }
    return TRUE;
}
EXPORT BOOL EnumThreadWindows(DWORD tid, BOOL (*cb)(HWND, LPARAM), LPARAM lp) {
    for (int i = 1; i < MAXW; i++) if (pool[i].used && !pool[i].parent && pool[i].tid == tid) if (!cb(&pool[i], lp)) return TRUE;
    return TRUE;
}
EXPORT HWND FindWindowA(LPCSTR cls, LPCSTR title) {
    for (int i = 1; i < MAXW; i++) {
        struct wnd *w = &pool[i];
        if (!w->used || w->parent) continue;
        if (cls && ustricmp(w->cls->name, cls)) continue;
        if (title && ustricmp(w->text, title)) continue;
        return w;
    }
    return 0;
}
EXPORT HWND FindWindowW(LPCWSTR cls, LPCWSTR title) {
    char a[64], b[256];
    if (cls) w2u(cls, a, sizeof a);
    if (title) w2u(title, b, sizeof b);
    return FindWindowA(cls ? a : 0, title ? b : 0);
}
EXPORT HWND FindWindowExA(HWND p, HWND after, LPCSTR cls, LPCSTR title) {
    struct wnd *pw = U_wnd(p);
    if (!pw) return FindWindowA(cls, title);
    int seen = after == 0;
    for (struct wnd *c = pw->first; c; c = c->next) {
        if (!seen) { if (c == after) seen = 1; continue; }
        if (cls && ustricmp(c->cls->name, cls)) continue;
        if (title && ustricmp(c->text, title)) continue;
        return c;
    }
    return 0;
}
EXPORT HWND WindowFromPoint(POINT p) {
    for (int i = MAXW - 1; i >= 1; i--) {
        struct wnd *w = &pool[i];
        if (!w->used || w->parent || !w->visible) continue;
        RECT r; GetWindowRect(w, &r);
        if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) {
            int sx, sy; U_screen_origin(w, &sx, &sy);
            return U_child_from_point(w, p.x - sx, p.y - sy);
        }
    }
    return 0;
}
EXPORT DWORD GetWindowThreadProcessId(HWND h, DWORD *pid) { struct wnd *w = U_wnd(h); if (pid) *pid = (DWORD)SC0(LX_getpid); return w ? w->tid : 0; }

static int transparent_to_mouse(struct wnd *c) {
    return c->cls && c->cls->builtin && !ustricmp(c->cls->name, "STATIC") && !(c->style & 0x100);      // STATIC without SS_NOTIFY
}
struct wnd *U_child_from_point(struct wnd *top, int x, int y) {
    struct wnd *w = top;
    for (;;) {
        struct wnd *hit = 0;
        for (struct wnd *c = w->last; c; c = c->prev) {
            if (!c->visible || transparent_to_mouse(c)) continue;
            if (x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h) { hit = c; break; }
        }
        if (!hit) return w;
        x -= hit->x + hit->bd; y -= hit->y + hit->bd;
        w = hit;
    }
}

int U_find_wid(int wid) {
    for (int i = 1; i < MAXW; i++) if (pool[i].used && !pool[i].parent && pool[i].wid == wid) return i;
    return -1;
}
