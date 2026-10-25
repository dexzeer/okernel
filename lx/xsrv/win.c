// The window tree: creation, mapping, geometry, stacking, exposure, properties; top-level windows are /dev/okwin windows.
#include "xsrv.h"

struct win *root;
int screen_w = 1920, screen_h = 1080;

void win_init(void) {
    struct okw_info inf;
    if (okw_ioctl(OKW_INFO, &inf) == 0 && inf.screen_w > 0) { screen_w = inf.screen_w; screen_h = inf.screen_h; }
    root = (struct win*)xcalloc(sizeof *root);
    root->id = XRoot; root->w = screen_w; root->h = screen_h; root->depth = 24; root->mapped = 1; root->wid = -1;
    root->bg_mode = 1; root->bg_pixel = 0x303030;
    root->colormap = XColormap;
    res_put(XRoot, RES_WINDOW, root);
}

// ---- helpers --------------------------------------------------------------------------------------------------------------------------
struct win *win_top(struct win *w) {
    while (w->parent && w->parent != root) w = w->parent;
    return w;
}

int win_viewable(struct win *w) {
    for (; w; w = w->parent) if (!w->mapped) return 0;
    return 1;
}

void win_abs(struct win *w, int *ax, int *ay) {
    int x = 0, y = 0;
    while (w && w->parent && w->parent != root) { x += w->x + w->border; y += w->y + w->border; w = w->parent; }
    *ax = x; *ay = y;
}

static void rect_outer(struct win *w, int pax, int pay, int *x, int *y, int *ow, int *oh) {
    *x = pax + w->x; *y = pay + w->y; *ow = w->w + 2 * w->border; *oh = w->h + 2 * w->border;
}

static void visible_ex(struct win *w, Region *g, int clip_children, int with_border) {
    struct win *t = win_top(w);
    int ax, ay;
    win_abs(w, &ax, &ay);
    if (w == t) rgn_set(g, 0, 0, w->bw ? w->bw : w->w, w->bh ? w->bh : w->h);
    else if (with_border) rgn_set(g, ax - w->border, ay - w->border, w->w + 2 * w->border, w->h + 2 * w->border);
    else rgn_set(g, ax, ay, w->w, w->h);
    for (struct win *cur = w; cur != t; cur = cur->parent) {
        struct win *p = cur->parent;
        int pax, pay;
        win_abs(p, &pax, &pay);
        rgn_isect(g, pax, pay, p->w, p->h);
        for (struct win *s = cur->next; s; s = s->next) {
            if (!s->mapped || s->inputonly) continue;
            int x, y, ow, oh;
            rect_outer(s, pax, pay, &x, &y, &ow, &oh);
            rgn_sub(g, x, y, ow, oh);
        }
    }
    rgn_isect(g, 0, 0, t->bw, t->bh);
    if (clip_children)
        for (struct win *c = w->first; c; c = c->next)
            if (c->mapped && !c->inputonly) { int x, y, ow, oh; rect_outer(c, ax, ay, &x, &y, &ow, &oh); rgn_sub(g, x, y, ow, oh); }
}

void win_visible(struct win *w, Region *g, int clip_children) { visible_ex(w, g, clip_children, 0); }

struct win *win_at(struct win *top, int x, int y) {
    struct win *cur = top;
    int ax = 0, ay = 0;
    for (;;) {
        struct win *hit = 0;
        for (struct win *c = cur->last; c; c = c->prev) {
            if (!c->mapped) continue;
            int cx = ax + c->x, cy = ay + c->y, ow = c->w + 2 * c->border, oh = c->h + 2 * c->border;
            if (x >= cx && x < cx + ow && y >= cy && y < cy + oh) { hit = c; break; }
        }
        if (!hit) return cur;
        ax += hit->x + hit->border; ay += hit->y + hit->border;
        cur = hit;
    }
}

// ---- okwin ------------------------------------------------------------------------------------------------------------------------------
void okw_map_top(struct win *t) {
    if (t->wid >= 0) return;
    int w = imin(imax(t->w, 16), 2048), h = imin(imax(t->h, 16), 1200);
    struct okw_create c;
    memset(&c, 0, sizeof c);
    c.x = t->x; c.y = t->y; c.w = w; c.h = h;
    if (!t->override_redirect && t->x == 0 && t->y == 0) c.x = c.y = -1;      // let the desktop place it
    if (t->override_redirect) c.flags |= OKW_F_NOFRAME;
    const char *nm = "x11";
    c.title[0] = 0;
    for (int i = 0; nm[i] && i < 63; i++) c.title[i] = nm[i];
    if (okw_ioctl(OKW_CREATE, &c) < 0) { LOG("okwin create failed\n"); return; }
    struct okw_map m;
    memset(&m, 0, sizeof m);
    m.wid = c.out_wid;
    if (okw_ioctl(OKW_MAP, &m) < 0) { LOG("okwin map failed\n"); okw_ioctl(OKW_DESTROY, (void*)(long)c.out_wid); return; }
    t->wid = c.out_wid; t->px = (u32*)m.out_addr; t->bw = w; t->bh = h;
    t->dx0 = t->dy0 = t->dx1 = t->dy1 = 0;
    win_set_title(t);
}

void top_dirty(struct win *t, int x0, int y0, int x1, int y1) {
    x0 = imax(x0, 0); y0 = imax(y0, 0); x1 = imin(x1, t->bw); y1 = imin(y1, t->bh);
    if (x1 <= x0 || y1 <= y0) return;
    if (t->dx1 <= t->dx0) { t->dx0 = x0; t->dy0 = y0; t->dx1 = x1; t->dy1 = y1; return; }
    t->dx0 = imin(t->dx0, x0); t->dy0 = imin(t->dy0, y0); t->dx1 = imax(t->dx1, x1); t->dy1 = imax(t->dy1, y1);
}

void top_flush(struct win *t) {
    if (t->wid < 0 || t->dx1 <= t->dx0) return;
    struct okw_present p = { t->wid, t->dx0, t->dy0, t->dx1 - t->dx0, t->dy1 - t->dy0 };
    okw_ioctl(OKW_PRESENT, &p);
    t->dx0 = t->dy0 = t->dx1 = t->dy1 = 0;
}

static struct prop *prop_find(struct win *w, u32 atom) {
    for (struct prop *p = w->props; p; p = p->next) if (p->name == atom) return p;
    return 0;
}

void win_set_title(struct win *t) {
    if (t->wid < 0) return;
    struct prop *p = prop_find(t, a_net_wm_name);
    if (!p) p = prop_find(t, A_WM_NAME);
    struct okw_title a;
    memset(&a, 0, sizeof a);
    a.wid = t->wid;
    int n = 0;
    if (p && p->format == 8) { for (; n < 63 && (u32)n < p->len; n++) a.title[n] = (char)p->data[n]; }
    else {
        struct prop *c = prop_find(t, A_WM_CLASS);                       // "instance\0Class\0": the class name
        if (c && c->format == 8) {
            u32 i = 0;
            while (i < c->len && c->data[i]) i++;
            i++;
            for (; i < c->len && n < 63 && c->data[i]; i++) a.title[n++] = (char)c->data[i];
        }
        if (!n) { const char *d = "x11"; while (*d) a.title[n++] = *d++; }
    }
    okw_ioctl(OKW_SETTITLE, &a);
}

// ---- events for window changes --------------------------------------------------------------------------------------------------------------
static void notify(struct win *w, int type, const u8 *proto, int event_off_self) {
    // `proto` carries the window fields; sent to StructureNotify of w (event = w) and SubstructureNotify of the parent (event = parent)
    u8 ev[32];
    memcpy(ev, proto, 32);
    ev[0] = (u8)type;
    p32(ev, event_off_self, w->id);
    ev_to_win(w, StructureNotifyMask, ev, -1, 0);
    if (w->parent) {
        p32(ev, event_off_self, w->parent->id);
        ev_to_win(w->parent, SubstructureNotifyMask, ev, -1, 0);
    }
}

// ---- exposure ---------------------------------------------------------------------------------------------------------------------------
static void expose_win(struct win *v, int cx, int cy, int cw, int ch, int paint) {
    if (v->inputonly || !v->mapped || v == root) return;
    struct win *t = win_top(v);
    if (!t->px) return;
    Region g;
    visible_ex(v, &g, 1, 0);
    if (cw > 0) rgn_isect(&g, cx, cy, cw, ch);
    if (!g.n) return;
    int ax, ay;
    win_abs(v, &ax, &ay);
    if (paint) {
        draw_bg(v, &g, ax, ay, t->px, t->bw, t->bh);
        for (int i = 0; i < g.n; i++) top_dirty(t, g.r[i].x, g.r[i].y, g.r[i].x + g.r[i].w, g.r[i].y + g.r[i].h);
    }
    if (win_has_mask(v, ExposureMask))
        for (int i = 0; i < g.n; i++) {
            u8 ev[32];
            memset(ev, 0, 32);
            ev[0] = EV_Expose;
            p32(ev, 4, v->id);
            p16(ev, 8, (u32)(g.r[i].x - ax)); p16(ev, 10, (u32)(g.r[i].y - ay));
            p16(ev, 12, (u32)g.r[i].w); p16(ev, 14, (u32)g.r[i].h);
            p16(ev, 16, (u32)(g.n - 1 - i));
            ev_to_win(v, ExposureMask, ev, -1, 0);
        }
}

static void paint_border(struct win *w) {
    if (!w->border || w->inputonly || w == root) return;
    struct win *t = win_top(w);
    if (w == t || !t->px) return;
    Region g;
    visible_ex(w, &g, 0, 1);
    int ax, ay;
    win_abs(w, &ax, &ay);
    rgn_sub(&g, ax, ay, w->w, w->h);
    for (int i = 0; i < g.n; i++) {
        Rect r = g.r[i];
        for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) t->px[y * t->bw + x] = w->border_pixel & 0xFFFFFF;
        top_dirty(t, r.x, r.y, r.x + r.w, r.y + r.h);
    }
}

void win_expose_tree(struct win *w) {
    if (!win_viewable(w)) return;
    paint_border(w);
    expose_win(w, 0, 0, 0, 0, 1);
    for (struct win *c = w->first; c; c = c->next) if (c->mapped) win_expose_tree(c);
}

static void expose_subtree_rect(struct win *v, int cx, int cy, int cw, int ch) {
    paint_border(v);
    expose_win(v, cx, cy, cw, ch, 1);
    for (struct win *c = v->first; c; c = c->next) if (c->mapped) expose_subtree_rect(c, cx, cy, cw, ch);
}

void win_expose_rect(struct win *top, int ax, int ay, int aw, int ah, int paint) {
    (void)paint;
    if (!top->px || !win_viewable(top)) return;
    expose_subtree_rect(top, ax, ay, aw, ah);
}

// ---- create / map / unmap ----------------------------------------------------------------------------------------------------------------
struct win *win_create(u32 id, struct win *parent, int x, int y, int w, int h, int border, int depth, int inputonly, struct client *owner) {
    struct win *n = (struct win*)xcalloc(sizeof *n);
    n->id = id; n->parent = parent; n->x = x; n->y = y; n->w = w; n->h = h; n->border = border;
    n->depth = depth ? depth : parent->depth; n->inputonly = inputonly; n->owner = owner;
    n->bg_mode = 0; n->wid = -1; n->colormap = XColormap;
    n->bit_gravity = 0; n->win_gravity = 1;
    n->last = 0;
    n->prev = parent->last;
    if (parent->last) parent->last->next = n; else parent->first = n;
    parent->last = n;
    res_put(id, RES_WINDOW, n);
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_CreateNotify;
    p32(ev, 4, parent->id); p32(ev, 8, id);
    p16(ev, 12, (u32)x); p16(ev, 14, (u32)y); p16(ev, 16, (u32)w); p16(ev, 18, (u32)h); p16(ev, 20, (u32)border);
    ev_to_win(parent, SubstructureNotifyMask, ev, -1, 0);
    return n;
}

void win_map(struct win *w) {
    if (w == root || w->mapped) return;
    w->mapped = 1;
    struct win *t = win_top(w);
    if (w == t) {
        okw_map_top(w);
        if (w->wid >= 0) { struct okw_show s = { w->wid, 1 }; okw_ioctl(OKW_SHOW, &s); }
    }
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_MapNotify; p32(ev, 8, w->id); ev[12] = (u8)w->override_redirect;
    notify(w, EV_MapNotify, ev, 4);
    if (win_viewable(w)) {
        if (w->inputonly) return;
        win_expose_tree(w);
        top_flush(t);
    }
}

void win_unmap(struct win *w) {
    if (w == root || !w->mapped) return;
    struct win *t = win_top(w);
    int was = win_viewable(w);
    int ax, ay;
    win_abs(w, &ax, &ay);
    w->mapped = 0;
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_UnmapNotify; p32(ev, 8, w->id);
    notify(w, EV_UnmapNotify, ev, 4);
    if (w == t) { if (w->wid >= 0) { struct okw_show s = { w->wid, 0 }; okw_ioctl(OKW_SHOW, &s); } input_win_gone(w); return; }
    if (was && t->px) {
        win_expose_rect(t, ax - w->border, ay - w->border, w->w + 2 * w->border, w->h + 2 * w->border, 1);
        top_flush(t);
    }
    input_win_gone(w);
}

void win_map_subs(struct win *w) { for (struct win *c = w->first; c; c = c->next) win_map(c); }
void win_unmap_subs(struct win *w) { for (struct win *c = w->last; c; c = c->prev) win_unmap(c); }

static void unlink_win(struct win *w) {
    struct win *p = w->parent;
    if (w->prev) w->prev->next = w->next; else p->first = w->next;
    if (w->next) w->next->prev = w->prev; else p->last = w->prev;
    w->prev = w->next = 0;
}

static void link_top(struct win *w, struct win *p) {
    w->parent = p;
    w->prev = p->last; w->next = 0;
    if (p->last) p->last->next = w; else p->first = w;
    p->last = w;
}

void win_stack(struct win *w, int above_all) {
    struct win *p = w->parent;
    if (!p) return;
    if (above_all && p->last == w) return;
    if (!above_all && p->first == w) return;
    unlink_win(w);
    if (above_all) link_top(w, p);
    else { w->parent = p; w->next = p->first; if (p->first) p->first->prev = w; else p->last = w; p->first = w; }
}

void win_destroy(struct win *w, int notify_) {
    if (w == root) return;
    while (w->last) win_destroy(w->last, 1);
    struct win *t = win_top(w);
    int was = win_viewable(w) && w != t;
    int ax, ay;
    win_abs(w, &ax, &ay);
    int bw = w->border, ww = w->w, hh = w->h;
    if (w->mapped) { w->mapped = 0; }
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_DestroyNotify; p32(ev, 8, w->id);
    if (notify_) notify(w, EV_DestroyNotify, ev, 4);
    input_win_gone(w);
    sel_win_gone(w);
    if (w == t && w->wid >= 0) { struct okw_show s = { w->wid, 0 }; okw_ioctl(OKW_SHOW, &s); okw_ioctl(OKW_DESTROY, (void*)(long)w->wid); }
    struct win *p = w->parent;
    unlink_win(w);
    res_del(w->id);
    while (w->sel) { struct evsel *s = w->sel; w->sel = s->next; xfree(s); }
    while (w->props) { struct prop *q = w->props; w->props = q->next; xfree(q->data); xfree(q); }
    xfree(w);
    if (was && t->px && p) {
        win_expose_rect(t, ax - bw, ay - bw, ww + 2 * bw, hh + 2 * bw, 1);
        top_flush(t);
    }
}

// ---- attributes / configure / reparent ----------------------------------------------------------------------------------------------------------
void win_change_attrs(struct win *w, u32 mask, const u32 *v) {
    int i = 0;
    if (mask & 0x1) { w->bg_mode = v[i] ? 2 : 0; w->bg_pixmap = v[i]; if (v[i] == 1) w->bg_mode = 3; i++; }
    if (mask & 0x2) { w->bg_mode = 1; w->bg_pixel = v[i++] & 0xFFFFFF; }
    if (mask & 0x4) i++;
    if (mask & 0x8) w->border_pixel = v[i++];
    if (mask & 0x10) w->bit_gravity = (int)v[i++];
    if (mask & 0x20) w->win_gravity = (int)v[i++];
    if (mask & 0x40) i++;
    if (mask & 0x80) i++;
    if (mask & 0x100) i++;
    if (mask & 0x200) w->override_redirect = (int)v[i++];
    if (mask & 0x400) i++;
    if (mask & 0x800) {
        u32 m = v[i++];
        struct evsel **pp = &w->sel;
        while (*pp && (*pp)->c != cc) pp = &(*pp)->next;
        if (*pp) { if (m) (*pp)->mask = m; else { struct evsel *d = *pp; *pp = d->next; xfree(d); } }
        else if (m) { struct evsel *s = (struct evsel*)xcalloc(sizeof *s); s->c = cc; s->mask = m; s->next = w->sel; w->sel = s; }
    }
    if (mask & 0x1000) w->dnp = v[i++];
    if (mask & 0x2000) w->colormap = v[i++];
    if (mask & 0x4000) w->cursor = v[i++];
}

void win_configure(struct win *w, u32 mask, const u32 *v, u32 sibling) {
    if (w == root) return;
    struct win *t = win_top(w);
    int ox, oy, ow, oh, ob;
    ox = w->x; oy = w->y; ow = w->w; oh = w->h; ob = w->border;
    int nx = ox, ny = oy, nw = ow, nh = oh, nb = ob, stack = -1, i = 0;
    if (mask & 1) nx = (int)v[i++];
    if (mask & 2) ny = (int)v[i++];
    if (mask & 4) nw = (int)v[i++];
    if (mask & 8) nh = (int)v[i++];
    if (mask & 0x10) nb = (int)v[i++];
    if (mask & 0x20) i++;
    if (mask & 0x40) stack = (int)v[i++];
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    int pax = 0, pay = 0;
    if (w != t) win_abs(w->parent, &pax, &pay);
    int was = win_viewable(w) && w != t && t->px;
    w->x = nx; w->y = ny; w->w = nw; w->h = nh; w->border = nb;
    if (w == t) {
        if (w->wid >= 0) {
            if ((mask & 3) && (nx != ox || ny != oy)) { struct okw_move m = { w->wid, nx, ny }; okw_ioctl(OKW_MOVE, &m); }
            int cw = imin(imax(nw, 16), 2048), ch = imin(imax(nh, 16), 1200);
            if (cw != w->bw || ch != w->bh) {
                struct okw_resize r;
                memset(&r, 0, sizeof r);
                r.wid = w->wid; r.w = cw; r.h = ch;
                if (okw_ioctl(OKW_RESIZE, &r) == 0) {
                    w->px = (u32*)r.out_addr; w->bw = cw; w->bh = ch;
                    w->dx0 = w->dy0 = w->dx1 = w->dy1 = 0;
                    if (win_viewable(w)) { win_expose_tree(w); top_flush(w); }
                }
            }
        }
    } else if (was) {
        if (stack >= 0) win_stack(w, stack == 0 || stack == 2 || stack == 4);
        int x0 = imin(ox, nx) - imax(ob, nb), y0 = imin(oy, ny) - imax(ob, nb);
        int x1 = imax(ox + ow, nx + nw) + 2 * imax(ob, nb), y1 = imax(oy + oh, ny + nh) + 2 * imax(ob, nb);
        win_expose_rect(t, pax + x0, pay + y0, x1 - x0, y1 - y0, 1);
        top_flush(t);
    } else if (stack >= 0) win_stack(w, stack == 0 || stack == 2 || stack == 4);
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_ConfigureNotify;
    p32(ev, 8, w->id);
    p32(ev, 12, w->prev ? w->prev->id : 0);
    p16(ev, 16, (u32)w->x); p16(ev, 18, (u32)w->y); p16(ev, 20, (u32)w->w); p16(ev, 22, (u32)w->h); p16(ev, 24, (u32)w->border);
    ev[26] = (u8)w->override_redirect;
    notify(w, EV_ConfigureNotify, ev, 4);
    (void)sibling;
}

void win_reparent(struct win *w, struct win *np, int x, int y) {
    if (w == root || np == w->parent) { w->x = x; w->y = y; return; }
    int was = w->mapped;
    if (was) win_unmap(w);
    struct win *oldt = win_top(w);
    int wastop = (w == oldt);
    if (wastop && w->wid >= 0) { struct okw_show s = { w->wid, 0 }; okw_ioctl(OKW_SHOW, &s); okw_ioctl(OKW_DESTROY, (void*)(long)w->wid); w->wid = -1; w->px = 0; }
    unlink_win(w);
    link_top(w, np);
    w->x = x; w->y = y;
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_ReparentNotify; p32(ev, 4, w->id); p32(ev, 8, w->id); p32(ev, 12, np->id); p16(ev, 16, (u32)x); p16(ev, 18, (u32)y);
    ev_to_win(w, StructureNotifyMask, ev, -1, 0);
    if (was) win_map(w);
}

// ---- properties ---------------------------------------------------------------------------------------------------------------------------------
void win_prop_changed(struct win *w, u32 atom, int deleted) {
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_PropertyNotify;
    p32(ev, 4, w->id); p32(ev, 8, atom); p32(ev, 12, srv_time()); ev[16] = (u8)(deleted ? 1 : 0);
    ev_to_win(w, PropertyChangeMask, ev, -1, 0);
    struct win *t = win_top(w);
    if (w == t) {
        if (atom == A_WM_NAME || atom == a_net_wm_name || atom == A_WM_CLASS) win_set_title(w);
        if (atom == a_wm_protocols) {
            struct prop *p = prop_find(w, atom);
            w->wm_delete = 0;
            if (p && p->format == 32) for (u32 i = 0; i + 4 <= p->len; i += 4) if (g32(p->data, (int)i) == a_wm_delete) w->wm_delete = 1;
        }
    }
}

static void strip_sel(struct win *w, struct client *c) {
    for (struct evsel **pp = &w->sel; *pp;) {
        if ((*pp)->c == c) { struct evsel *d = *pp; *pp = d->next; xfree(d); } else pp = &(*pp)->next;
    }
    for (struct win *k = w->first; k; k = k->next) strip_sel(k, c);
}

static struct win *find_owned(struct win *w, struct client *c) {
    for (struct win *k = w->first; k; k = k->next) {
        if (k->owner == c) return k;
        struct win *r = find_owned(k, c);
        if (r) return r;
    }
    return 0;
}

// windows owned by the client are destroyed (children first); its event selections are removed everywhere
void win_client_gone(struct client *c) {
    struct win *w;
    while ((w = find_owned(root, c)) != 0) win_destroy(w, 1);
    strip_sel(root, c);
}
