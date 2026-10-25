// X11 core protocol: connection setup and request handlers.
#include "xsrv.h"

// ---- colours ---------------------------------------------------------------------------------------------------------------------------------
static const struct { const char *n; u8 r, g, b; } cnames[] = {
    {"black",0,0,0},{"white",255,255,255},{"red",255,0,0},{"green",0,255,0},{"blue",0,0,255},{"yellow",255,255,0},{"cyan",0,255,255},
    {"magenta",255,0,255},{"gray",190,190,190},{"grey",190,190,190},{"orange",255,165,0},{"pink",255,192,203},{"brown",165,42,42},
    {"purple",160,32,240},{"navy",0,0,128},{"navyblue",0,0,128},{"lightgray",211,211,211},{"lightgrey",211,211,211},{"darkgray",169,169,169},
    {"darkgrey",169,169,169},{"darkgreen",0,100,0},{"darkblue",0,0,139},{"darkred",139,0,0},{"gold",255,215,0},{"violet",238,130,238},
    {"tan",210,180,140},{"wheat",245,222,179},{"beige",245,245,220},{"ivory",255,255,240},{"khaki",240,230,140},{"maroon",176,48,96},
    {"turquoise",64,224,208},{"salmon",250,128,114},{"coral",255,127,80},{"skyblue",135,206,235},{"steelblue",70,130,180},
    {"forestgreen",34,139,34},{"lightblue",173,216,230},{"lightgreen",144,238,144},{"lightyellow",255,255,224},{"orchid",218,112,214},
    {"slategray",112,128,144},{"slategrey",112,128,144},{"darkorange",255,140,0},{"firebrick",178,34,34},{"sienna",160,82,45},
    {"limegreen",50,205,50},{"royalblue",65,105,225},{"dodgerblue",30,144,255},{"seagreen",46,139,87},{"chocolate",210,105,30},
    {"linen",250,240,230},{"snow",255,250,250},{"lavender",230,230,250},{"mintcream",245,255,250},{"aquamarine",127,255,212},
    {"crimson",220,20,60},{"indigo",75,0,130},{"plum",221,160,221},{"peru",205,133,63},{"azure",240,255,255},{"honeydew",240,255,240},
};

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

int color_by_name(const char *s, int n, u32 *r, u32 *g, u32 *b) {
    if (n > 0 && s[0] == '#') {
        int d = n - 1;
        if (d % 3 || d < 3 || d > 12) return 0;
        int k = d / 3;
        u32 v[3];
        for (int c = 0; c < 3; c++) {
            u32 x = 0;
            for (int i = 0; i < k; i++) { int h = hexv(s[1 + c * k + i]); if (h < 0) return 0; x = x * 16 + (u32)h; }
            v[c] = k == 1 ? x * 0x1111 : k == 2 ? x * 0x101 : k == 3 ? (x << 4) | (x >> 8) : x;
        }
        *r = v[0]; *g = v[1]; *b = v[2];
        return 1;
    }
    if (n > 4 && !memcmp(s, "rgb:", 4)) {
        u32 v[3]; int pos = 4;
        for (int c = 0; c < 3; c++) {
            u32 x = 0; int k = 0;
            while (pos < n && s[pos] != '/' && k < 4) { int h = hexv(s[pos]); if (h < 0) return 0; x = x * 16 + (u32)h; pos++; k++; }
            if (!k) return 0;
            v[c] = k == 1 ? x * 0x1111 : k == 2 ? x * 0x101 : k == 3 ? (x << 4) | (x >> 8) : x;
            if (c < 2) { if (pos >= n || s[pos] != '/') return 0; pos++; }
        }
        *r = v[0]; *g = v[1]; *b = v[2];
        return 1;
    }
    char t[40]; int m = 0;
    for (int i = 0; i < n && m < 39; i++) { char c = s[i]; if (c == ' ') continue; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); t[m++] = c; }
    t[m] = 0;
    if ((!memcmp(t, "gray", 4) || !memcmp(t, "grey", 4)) && t[4] >= '0' && t[4] <= '9') {
        int v = 0;
        for (int i = 4; t[i]; i++) { if (t[i] < '0' || t[i] > '9') return 0; v = v * 10 + (t[i] - '0'); }
        if (v > 100) return 0;
        u32 l = (u32)((v * 255 + 50) / 100) * 257;
        *r = *g = *b = l;
        return 1;
    }
    for (u32 i = 0; i < sizeof cnames / sizeof cnames[0]; i++)
        if (!strcmp(cnames[i].n, t)) { *r = cnames[i].r * 257u; *g = cnames[i].g * 257u; *b = cnames[i].b * 257u; return 1; }
    return 0;
}

static u32 pixel_rgb16(u32 r, u32 g, u32 b) { return ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8); }

// ---- setup -------------------------------------------------------------------------------------------------------------------------------------
void req_setup(struct client *c, u8 *r, u32 len) {
    (void)len;
    if (r[0] != 'l') {                                                      // only little-endian clients
        const char *why = "big-endian clients are not supported";
        u32 n = (u32)strlen(why);
        u8 *o = out_reserve(c, 8 + pad4(n));
        o[0] = 0; o[1] = (u8)n; p16(o, 2, 11); p16(o, 6, pad4(n) / 4);
        memcpy(o + 8, why, n);
        c->dead = 1;
        c->state = 1;
        return;
    }
    const char *vendor = "KAnarchy X";
    u32 vl = (u32)strlen(vendor);
    u32 body = 32 + pad4(vl) + 2 * 8 + 40 + 2 * 8 + 24;
    u8 *o = out_reserve(c, 8 + body);
    o[0] = 1; p16(o, 2, 11); p16(o, 4, 0); p16(o, 6, body / 4);
    p32(o, 8, 11000000); p32(o, 12, c->base); p32(o, 16, 0x1FFFFF); p32(o, 20, 256);
    p16(o, 24, vl); p16(o, 26, 65535); o[28] = 1; o[29] = 2; o[30] = 0; o[31] = 0; o[32] = 32; o[33] = 32; o[34] = 8; o[35] = 255;
    int p = 40;
    memcpy(o + p, vendor, vl); p += (int)pad4(vl);
    o[p] = 1; o[p + 1] = 1; o[p + 2] = 32; p += 8;                          // depth 1: 1 bit per pixel, scanline pad 32
    o[p] = 24; o[p + 1] = 32; o[p + 2] = 32; p += 8;
    p32(o, p, XRoot); p32(o, p + 4, XColormap); p32(o, p + 8, 0xFFFFFF); p32(o, p + 12, 0); p32(o, p + 16, 0);
    p16(o, p + 20, (u32)screen_w); p16(o, p + 22, (u32)screen_h);
    p16(o, p + 24, (u32)(screen_w * 254 / 960)); p16(o, p + 26, (u32)(screen_h * 254 / 960));
    p16(o, p + 28, 1); p16(o, p + 30, 1); p32(o, p + 32, XVisual); o[p + 36] = 0; o[p + 37] = 0; o[p + 38] = 24; o[p + 39] = 2;
    p += 40;
    o[p] = 1; p16(o, p + 2, 0); p += 8;                                     // depth 1, no visuals
    o[p] = 24; p16(o, p + 2, 1); p += 8;
    p32(o, p, XVisual); o[p + 4] = 4; o[p + 5] = 8; p16(o, p + 6, 256); p32(o, p + 8, 0xFF0000); p32(o, p + 12, 0xFF00); p32(o, p + 16, 0xFF);
    c->state = 1;
    LOG("xokai: client %d connected\n", c->idx);
}

// ---- properties -----------------------------------------------------------------------------------------------------------------------------------
static struct prop *prop_get(struct win *w, u32 a) {
    for (struct prop *p = w->props; p; p = p->next) if (p->name == a) return p;
    return 0;
}

static void prop_set(struct win *w, u32 name, u32 type, int fmt, const u8 *data, u32 bytes, int mode) {
    struct prop *p = prop_get(w, name);
    if (!p) {
        p = (struct prop*)xcalloc(sizeof *p);
        p->name = name; p->next = w->props; w->props = p;
        p->type = type; p->format = fmt;
        mode = 0;
    }
    if (mode == 0) {
        xfree(p->data);
        p->data = (u8*)xmalloc(bytes + 1);
        memcpy(p->data, data, bytes);
        p->len = bytes; p->type = type; p->format = fmt;
    } else {
        u8 *n = (u8*)xmalloc(p->len + bytes + 1);
        if (mode == 2) { memcpy(n, p->data, p->len); memcpy(n + p->len, data, bytes); }
        else { memcpy(n, data, bytes); memcpy(n + bytes, p->data, p->len); }
        xfree(p->data);
        p->data = n; p->len += bytes;
    }
    win_prop_changed(w, name, 0);
}

static void prop_delete(struct win *w, u32 name) {
    for (struct prop **pp = &w->props; *pp; pp = &(*pp)->next)
        if ((*pp)->name == name) {
            struct prop *d = *pp;
            *pp = d->next;
            xfree(d->data); xfree(d);
            win_prop_changed(w, name, 1);
            return;
        }
}

// ---- selections -----------------------------------------------------------------------------------------------------------------------------------
struct selrec { u32 atom; struct win *owner; struct client *c; u32 time; };
static struct selrec sels[32];

void sel_client_gone(struct client *c) { for (int i = 0; i < 32; i++) if (sels[i].c == c) sels[i].atom = 0; }
void sel_win_gone(struct win *w) { for (int i = 0; i < 32; i++) if (sels[i].owner == w) sels[i].atom = 0; }

static struct selrec *sel_find(u32 atom, int create) {
    for (int i = 0; i < 32; i++) if (sels[i].atom == atom) return &sels[i];
    if (!create) return 0;
    for (int i = 0; i < 32; i++) if (!sels[i].atom) return &sels[i];
    return 0;
}

static void sel_notify(struct client *to, u32 requestor, u32 sel, u32 target, u32 prop, u32 time) {
    u8 ev[32];
    memset(ev, 0, 32);
    ev[0] = EV_SelectionNotify;
    p32(ev, 4, time); p32(ev, 8, requestor); p32(ev, 12, sel); p32(ev, 16, target); p32(ev, 20, prop);
    ev_to_client(to, ev);
}

static void convert_selection(struct client *c, u32 req, u32 sel, u32 target, u32 prop, u32 time) {
    struct win *rw = win_of(req);
    if (!rw) return;
    struct selrec *s = sel_find(sel, 0);
    if (s && s->owner) {
        u8 ev[32];
        memset(ev, 0, 32);
        ev[0] = EV_SelectionRequest;
        p32(ev, 4, time); p32(ev, 8, s->owner->id); p32(ev, 12, req); p32(ev, 16, sel); p32(ev, 20, target); p32(ev, 24, prop);
        ev_to_client(s->c, ev);
        return;
    }
    if (!prop) prop = target;
    if (sel == A_PRIMARY || sel == a_clipboard) {                           // nobody owns it: offer the desktop's clipboard text
        if (target == a_targets) {
            u32 t[3] = { A_STRING, a_utf8, a_targets };
            u8 d[12];
            for (int i = 0; i < 3; i++) p32(d, i * 4, t[i]);
            prop_set(rw, prop, A_ATOM, 32, d, 12, 0);
            sel_notify(c, req, sel, target, prop, time);
            return;
        }
        if (target == A_STRING || target == a_utf8) {
            static char buf[65536];
            struct okw_clip k;
            k.op = 1; k.len = (int)sizeof buf; k.buf = (unsigned long long)buf; k.seq = 0;
            if (okw_ioctl(OKW_CLIPBOARD, &k) == 0 && k.len > 0) {
                prop_set(rw, prop, target, 8, (const u8*)buf, (u32)k.len, 0);
                sel_notify(c, req, sel, target, prop, time);
                return;
            }
        }
    }
    sel_notify(c, req, sel, target, 0, time);
}

// ---- GC ----------------------------------------------------------------------------------------------------------------------------------------------
static int popcnt(u32 v) { int n = 0; while (v) { n += (int)(v & 1); v >>= 1; } return n; }

static void gc_defaults(struct gc *g) {
    memset(g, 0, sizeof *g);
    g->function = 3; g->plane_mask = 0xFFFFFFFFu; g->fg = 0; g->bg = 1; g->cap_style = 1; g->arc_mode = 1;
    g->graphics_exposures = 1; g->nclip = -1;
}

static void gc_apply(struct gc *g, u32 mask, const u8 *v) {
    int i = 0;
#define NEXT g32(v, 4 * i++)
    if (mask & 0x1) g->function = (int)(NEXT & 15);
    if (mask & 0x2) g->plane_mask = NEXT;
    if (mask & 0x4) g->fg = NEXT;
    if (mask & 0x8) g->bg = NEXT;
    if (mask & 0x10) g->line_width = (int)(NEXT & 0xFFFF);
    if (mask & 0x20) g->line_style = (int)NEXT;
    if (mask & 0x40) g->cap_style = (int)NEXT;
    if (mask & 0x80) g->join_style = (int)NEXT;
    if (mask & 0x100) g->fill_style = (int)NEXT;
    if (mask & 0x200) g->fill_rule = (int)NEXT;
    if (mask & 0x400) g->tile = NEXT;
    if (mask & 0x800) g->stipple = NEXT;
    if (mask & 0x1000) g->ts_x = (i16)NEXT;
    if (mask & 0x2000) g->ts_y = (i16)NEXT;
    if (mask & 0x4000) g->font = NEXT;
    if (mask & 0x8000) g->subwindow_mode = (int)NEXT;
    if (mask & 0x10000) g->graphics_exposures = (int)NEXT;
    if (mask & 0x20000) g->clip_x = (i16)NEXT;
    if (mask & 0x40000) g->clip_y = (i16)NEXT;
    if (mask & 0x80000) { u32 m = NEXT; if (!m) { xfree(g->clip); g->clip = 0; g->nclip = -1; } }
    if (mask & 0x100000) g->dash_offset = (int)(NEXT & 0xFFFF);
    if (mask & 0x200000) { u32 d = NEXT & 255; g->ndash = 2; g->dashes[0] = g->dashes[1] = (u8)(d ? d : 1); }
    if (mask & 0x400000) g->arc_mode = (int)NEXT;
#undef NEXT
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------------------------------
#define NEED(n) do { if (len < (u32)(n)) { send_error(c, ER_Length, 0, op, 0); return; } } while (0)
#define BADWIN(id) do { send_error(c, ER_Window, (id), op, 0); return; } while (0)

static int id_ok(struct client *c, u32 id) { return (id & ~0x1FFFFFu) == c->base && (id & 0x1FFFFF) != 0 && res_free_id(id); }

static u8 *reply(struct client *c, int data, u32 extra, u32 **unused) {
    (void)unused;
    u8 *r = out_reserve(c, 32 + pad4(extra));
    reply_start(c, r, data, extra);
    return r;
}

static void root_pos(struct win *w, int *rx, int *ry) {
    struct win *t = win_top(w);
    int ax, ay;
    win_abs(w, &ax, &ay);
    if (w == root) { *rx = 0; *ry = 0; return; }
    *rx = t->rx0 + ax; *ry = t->ry0 + ay;
}

static void font_info(u8 *r, struct font *f) {                              // QueryFont body at r+8 (min/max bounds ... ascent/descent)
    int cw = f->cw;
    p16(r, 8, 0); p16(r, 10, (u32)cw); p16(r, 12, (u32)cw); p16(r, 14, (u32)f->asc); p16(r, 16, (u32)f->desc);
    p16(r, 24, 0); p16(r, 26, (u32)cw); p16(r, 28, (u32)cw); p16(r, 30, (u32)f->asc); p16(r, 32, (u32)f->desc);
    p16(r, 40, 32); p16(r, 42, 255); p16(r, 44, 32); p16(r, 46, 0);
    r[48] = 0; r[49] = 0; r[50] = 0; r[51] = 1;
    p16(r, 52, (u32)f->asc); p16(r, 54, (u32)f->desc);
}

static struct font *fontable(u32 id) {
    struct font *f = (struct font*)res_get(id, RES_FONT);
    if (f) return f;
    struct gc *g = (struct gc*)res_get(id, RES_GC);
    return g && g->font ? (struct font*)res_get(g->font, RES_FONT) : 0;
}

static void expose_clear(struct win *w, int x, int y, int cw, int ch, int exposures) {
    struct win *t = win_top(w);
    if (!t->px || !win_viewable(w) || w->inputonly) return;
    Region g;
    win_visible(w, &g, 1);
    int ax, ay;
    win_abs(w, &ax, &ay);
    if (cw <= 0) cw = w->w - x;
    if (ch <= 0) ch = w->h - y;
    rgn_isect(&g, ax + x, ay + y, cw, ch);
    draw_bg(w, &g, ax, ay, t->px, t->bw, t->bh);
    for (int i = 0; i < g.n; i++) top_dirty(t, g.r[i].x, g.r[i].y, g.r[i].x + g.r[i].w, g.r[i].y + g.r[i].h);
    if (exposures && win_has_mask(w, ExposureMask))
        for (int i = 0; i < g.n; i++) {
            u8 ev[32];
            memset(ev, 0, 32);
            ev[0] = EV_Expose; p32(ev, 4, w->id);
            p16(ev, 8, (u32)(g.r[i].x - ax)); p16(ev, 10, (u32)(g.r[i].y - ay)); p16(ev, 12, (u32)g.r[i].w); p16(ev, 14, (u32)g.r[i].h);
            p16(ev, 16, (u32)(g.n - 1 - i));
            ev_to_win(w, ExposureMask, ev, -1, 0);
        }
}

// ---- the dispatcher -----------------------------------------------------------------------------------------------------------------------------------
void req_dispatch(struct client *c, u8 *r, u32 len) {
    int op = r[0];
    switch (op) {
    case 1: {                                                                      // CreateWindow
        NEED(32);
        u32 id = g32(r, 4), par = g32(r, 8), mask = g32(r, 28);
        if (!id_ok(c, id)) { send_error(c, ER_IDChoice, id, op, 0); return; }
        struct win *p = win_of(par);
        if (!p) BADWIN(par);
        NEED(32 + 4 * popcnt(mask));
        int cls = (int)g16(r, 22);
        if (cls == 0) cls = p->inputonly ? 2 : 1;
        struct win *w = win_create(id, p, gi16(r, 12), gi16(r, 14), (int)g16(r, 16) ? (int)g16(r, 16) : 1, (int)g16(r, 18) ? (int)g16(r, 18) : 1,
                                   (int)g16(r, 20), r[1], cls == 2, c);
        win_change_attrs(w, mask, (const u32*)(void*)(r + 32));
        return;
    }
    case 2: {                                                                      // ChangeWindowAttributes
        NEED(12);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u32 mask = g32(r, 8);
        NEED(12 + 4 * popcnt(mask));
        win_change_attrs(w, mask, (const u32*)(void*)(r + 12));
        return;
    }
    case 3: {                                                                      // GetWindowAttributes
        NEED(8);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u8 *o = reply(c, 0, 12, 0);
        p32(o, 4, 3);
        p32(o, 8, XVisual); p16(o, 12, w->inputonly ? 2 : 1); o[14] = (u8)w->bit_gravity; o[15] = (u8)w->win_gravity;
        p32(o, 16, 0xFFFFFFFFu); o[25] = 1;
        o[26] = (u8)(!w->mapped ? 0 : win_viewable(w) ? 2 : 1); o[27] = (u8)w->override_redirect;
        p32(o, 28, w->colormap);
        u32 all = 0, mine = 0;
        for (struct evsel *s = w->sel; s; s = s->next) { all |= s->mask; if (s->c == c) mine = s->mask; }
        p32(o, 32, all); p32(o, 36, mine); p16(o, 40, w->dnp);
        return;
    }
    case 4: { NEED(8); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); win_destroy(w, 1); return; }
    case 5: { NEED(8); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); while (w->last) win_destroy(w->last, 1); return; }
    case 6: return;                                                                // ChangeSaveSet
    case 7: {                                                                      // ReparentWindow
        NEED(16);
        struct win *w = win_of(g32(r, 4)), *np = win_of(g32(r, 8));
        if (!w) BADWIN(g32(r, 4));
        if (!np) BADWIN(g32(r, 8));
        win_reparent(w, np, gi16(r, 12), gi16(r, 14));
        return;
    }
    case 8: { NEED(8); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); win_map(w); return; }
    case 9: { NEED(8); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); win_map_subs(w); return; }
    case 10: { NEED(8); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); win_unmap(w); return; }
    case 11: { NEED(8); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); win_unmap_subs(w); return; }
    case 12: {                                                                     // ConfigureWindow
        NEED(12);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u32 mask = g16(r, 8) & 0x7F;
        NEED(12 + 4 * popcnt(mask));
        u32 v[8];
        for (int i = 0; i < popcnt(mask) && i < 8; i++) v[i] = g32(r, 12 + 4 * i);
        win_configure(w, mask, v, 0);
        return;
    }
    case 13: {                                                                     // CirculateWindow
        NEED(8);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        if (r[1] == 0 && w->first) win_stack(w->first, 1); else if (w->last) win_stack(w->last, 0);
        return;
    }
    case 14: {                                                                     // GetGeometry
        NEED(8);
        u32 id = g32(r, 4);
        struct win *w = win_of(id);
        struct pix *pm = (struct pix*)res_get(id, RES_PIXMAP);
        if (!w && !pm) { send_error(c, ER_Drawable, id, op, 0); return; }
        u8 *o = reply(c, w ? w->depth : pm->depth, 0, 0);
        p32(o, 8, XRoot);
        if (w) { p16(o, 12, (u32)w->x); p16(o, 14, (u32)w->y); p16(o, 16, (u32)w->w); p16(o, 18, (u32)w->h); p16(o, 20, (u32)w->border); }
        else { p16(o, 16, (u32)pm->w); p16(o, 18, (u32)pm->h); }
        return;
    }
    case 15: {                                                                     // QueryTree
        NEED(8);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        int n = 0;
        for (struct win *k = w->first; k; k = k->next) n++;
        u8 *o = reply(c, 0, (u32)n * 4, 0);
        p32(o, 8, XRoot); p32(o, 12, w->parent ? w->parent->id : 0); p16(o, 16, (u32)n);
        int i = 0;
        for (struct win *k = w->first; k; k = k->next) p32(o, 32 + 4 * i++, k->id);
        return;
    }
    case 16: {                                                                     // InternAtom
        NEED(8);
        u32 n = g16(r, 4);
        NEED(8 + n);
        u32 a = atom_intern((const char*)r + 8, (int)n, !r[1]);
        u8 *o = reply(c, 0, 0, 0);
        p32(o, 8, a);
        return;
    }
    case 17: {                                                                     // GetAtomName
        NEED(8);
        const char *nm = atom_name(g32(r, 4));
        if (!nm) { send_error(c, ER_Atom, g32(r, 4), op, 0); return; }
        u32 n = (u32)strlen(nm);
        u8 *o = reply(c, 0, n, 0);
        p16(o, 8, n);
        memcpy(o + 32, nm, n);
        return;
    }
    case 18: {                                                                     // ChangeProperty
        NEED(24);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        int fmt = r[16];
        u32 n = g32(r, 20);
        if (fmt != 8 && fmt != 16 && fmt != 32) { send_error(c, ER_Value, (u32)fmt, op, 0); return; }
        u32 bytes = n * (u32)(fmt / 8);
        if (bytes > (1u << 24)) { send_error(c, ER_Alloc, 0, op, 0); return; }
        NEED(24 + bytes);
        struct prop *old = prop_get(w, g32(r, 8));
        if (old && r[1] != 0 && (old->format != fmt || old->type != g32(r, 12))) { send_error(c, ER_Match, 0, op, 0); return; }
        prop_set(w, g32(r, 8), g32(r, 12), fmt, r + 24, bytes, r[1]);
        return;
    }
    case 19: { NEED(12); struct win *w = win_of(g32(r, 4)); if (!w) BADWIN(g32(r, 4)); prop_delete(w, g32(r, 8)); return; }
    case 20: {                                                                     // GetProperty
        NEED(24);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u32 name = g32(r, 8), type = g32(r, 12), off = g32(r, 16) * 4, lenq = g32(r, 20) * 4;
        struct prop *p = prop_get(w, name);
        if (!p) { reply(c, 0, 0, 0); return; }
        if (type && type != p->type) {
            u8 *o = reply(c, p->format, 0, 0);
            p32(o, 8, p->type); p32(o, 12, p->len);
            return;
        }
        if (off > p->len) { send_error(c, ER_Value, g32(r, 16), op, 0); return; }
        u32 n = p->len - off;
        if (n > lenq) n = lenq;
        u32 after = p->len - off - n;
        u8 *o = reply(c, p->format, n, 0);
        p32(o, 8, p->type); p32(o, 12, after); p32(o, 16, n / (u32)(p->format / 8));
        memcpy(o + 32, p->data + off, n);
        if (r[1] && after == 0) prop_delete(w, name);
        return;
    }
    case 21: {                                                                     // ListProperties
        NEED(8);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        int n = 0;
        for (struct prop *p = w->props; p; p = p->next) n++;
        u8 *o = reply(c, 0, (u32)n * 4, 0);
        p16(o, 8, (u32)n);
        int i = 0;
        for (struct prop *p = w->props; p; p = p->next) p32(o, 32 + 4 * i++, p->name);
        return;
    }
    case 22: {                                                                     // SetSelectionOwner
        NEED(16);
        u32 ow = g32(r, 4), sel = g32(r, 8);
        struct win *w = ow ? win_of(ow) : 0;
        if (ow && !w) BADWIN(ow);
        struct selrec *s = sel_find(sel, 1);
        if (!s) return;
        if (s->atom && s->owner && s->c != c) {
            u8 ev[32];
            memset(ev, 0, 32);
            ev[0] = EV_SelectionClear; p32(ev, 4, srv_time()); p32(ev, 8, s->owner->id); p32(ev, 12, sel);
            ev_to_client(s->c, ev);
        }
        s->atom = sel; s->owner = w; s->c = c; s->time = g32(r, 12);
        if (!w) s->atom = 0;
        return;
    }
    case 23: {                                                                     // GetSelectionOwner
        NEED(8);
        struct selrec *s = sel_find(g32(r, 4), 0);
        u8 *o = reply(c, 0, 0, 0);
        p32(o, 8, s && s->owner ? s->owner->id : 0);
        return;
    }
    case 24: NEED(24); convert_selection(c, g32(r, 4), g32(r, 8), g32(r, 12), g32(r, 16), g32(r, 20)); return;
    case 25: {                                                                     // SendEvent
        NEED(44);
        u32 dst = g32(r, 4), mask = g32(r, 8);
        struct win *w = dst == 0 ? ptr_win : dst == 1 ? (focus_win ? focus_win : ptr_win) : win_of(dst);
        if (!w) { if (dst > 1) BADWIN(dst); return; }
        u8 ev[32];
        memcpy(ev, r + 12, 32);
        ev[0] |= 0x80;
        if (mask == 0) { if (w->owner) ev_to_client(w->owner, ev); return; }
        ev_to_win(w, mask, ev, -1, r[1]);
        return;
    }
    case 26: {                                                                     // GrabPointer
        NEED(24);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u8 *o = reply(c, 0, 0, 0);
        if (grab_ptr_win && !grab_ptr_implicit && grab_ptr_client != c) { o[1] = 1; return; }
        if (!win_viewable(w)) { o[1] = 3; return; }
        grab_ptr_win = w; grab_ptr_client = c; grab_ptr_implicit = 0; grab_ptr_owner_events = r[1]; grab_ptr_mask = g16(r, 8);
        return;
    }
    case 27: if (grab_ptr_client == c) { grab_ptr_win = 0; grab_ptr_client = 0; grab_ptr_implicit = 0; } return;
    case 28: case 29: case 30: return;                                             // passive button grabs: not supported
    case 31: {                                                                     // GrabKeyboard
        NEED(16);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u8 *o = reply(c, 0, 0, 0);
        if (grab_kb_win && grab_kb_client != c) { o[1] = 1; return; }
        if (!win_viewable(w)) { o[1] = 3; return; }
        grab_kb_win = w; grab_kb_client = c;
        return;
    }
    case 32: if (grab_kb_client == c) { grab_kb_win = 0; grab_kb_client = 0; } return;
    case 33: case 34: case 35: case 36: case 37: return;
    case 38: {                                                                     // QueryPointer
        NEED(8);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        u8 *o = reply(c, 1, 0, 0);
        int rx, ry;
        root_pos(w, &rx, &ry);
        p32(o, 8, XRoot);
        p16(o, 16, (u32)ptr_root_x); p16(o, 18, (u32)ptr_root_y);
        p16(o, 20, (u32)(ptr_root_x - rx)); p16(o, 22, (u32)(ptr_root_y - ry));
        p16(o, 24, ptr_state);
        return;
    }
    case 39: reply(c, 0, 0, 0); return;
    case 40: {                                                                     // TranslateCoordinates
        NEED(16);
        struct win *s = win_of(g32(r, 4)), *d = win_of(g32(r, 8));
        if (!s) BADWIN(g32(r, 4));
        if (!d) BADWIN(g32(r, 8));
        int sx, sy, dx, dy;
        root_pos(s, &sx, &sy); root_pos(d, &dx, &dy);
        u8 *o = reply(c, 1, 0, 0);
        p16(o, 12, (u32)(sx + gi16(r, 12) - dx)); p16(o, 14, (u32)(sy + gi16(r, 14) - dy));
        return;
    }
    case 41: return;
    case 42: {                                                                     // SetInputFocus
        NEED(16);
        u32 f = g32(r, 4);
        focus_win = f > 1 ? win_of(f) : 0;
        return;
    }
    case 43: { u8 *o = reply(c, 1, 0, 0); p32(o, 8, focus_win ? focus_win->id : 1); return; }
    case 44: reply(c, 0, 32, 0); return;
    case 45: {                                                                     // OpenFont
        NEED(12);
        u32 id = g32(r, 4), n = g16(r, 8);
        NEED(12 + n);
        if (!id_ok(c, id)) { send_error(c, ER_IDChoice, id, op, 0); return; }
        struct font *f = font_open(id, (const char*)r + 12, (int)n);
        if (!f) { send_error(c, ER_Name, 0, op, 0); return; }
        res_put(id, RES_FONT, f);
        return;
    }
    case 46: { NEED(8); struct font *f = (struct font*)res_get(g32(r, 4), RES_FONT); if (!f) { send_error(c, ER_Font, g32(r, 4), op, 0); return; } res_del(g32(r, 4)); font_unref(f); return; }
    case 47: {                                                                     // QueryFont
        NEED(8);
        struct font *f = fontable(g32(r, 4));
        if (!f) { send_error(c, ER_Font, g32(r, 4), op, 0); return; }
        u8 *o = reply(c, 0, 28, 0);
        font_info(o, f);
        return;
    }
    case 48: {                                                                     // QueryTextExtents
        NEED(8);
        struct font *f = fontable(g32(r, 4));
        if (!f) { send_error(c, ER_Font, g32(r, 4), op, 0); return; }
        int n = (int)(len - 8) / 2 - (r[1] ? 1 : 0);
        if (n < 0) n = 0;
        u8 *o = reply(c, 0, 0, 0);
        p16(o, 8, (u32)f->asc); p16(o, 10, (u32)f->desc); p16(o, 12, (u32)f->asc); p16(o, 14, (u32)f->desc);
        p32(o, 16, (u32)(n * f->cw)); p32(o, 20, 0); p32(o, 24, (u32)(n * f->cw));
        return;
    }
    case 49: {                                                                     // ListFonts
        NEED(8);
        u32 maxn = g16(r, 4), pl = g16(r, 6);
        NEED(8 + pl);
        static char names[64][100];
        int n = font_list((const char*)r + 8, (int)pl, names, (int)(maxn < 64 ? maxn : 64));
        u32 bytes = 0;
        for (int i = 0; i < n; i++) bytes += 1 + (u32)strlen(names[i]);
        u8 *o = reply(c, 0, bytes, 0);
        p16(o, 8, (u32)n);
        u8 *q = o + 32;
        for (int i = 0; i < n; i++) { u32 l = (u32)strlen(names[i]); *q++ = (u8)l; memcpy(q, names[i], l); q += l; }
        return;
    }
    case 50: {                                                                     // ListFontsWithInfo
        NEED(8);
        u32 maxn = g16(r, 4), pl = g16(r, 6);
        NEED(8 + pl);
        static char names[64][100];
        int n = font_list((const char*)r + 8, (int)pl, names, (int)(maxn < 64 ? maxn : 64));
        for (int i = 0; i < n; i++) {
            struct font *f = font_open(0, names[i], (int)strlen(names[i]));
            if (!f) continue;
            u32 l = (u32)strlen(names[i]);
            u8 *o = reply(c, (int)l, 28 + l, 0);
            font_info(o, f);
            p32(o, 56, (u32)(n - 1 - i));
            memcpy(o + 60, names[i], l);
            font_unref(f);
        }
        u8 *o = reply(c, 0, 28, 0);                                                // the last reply: name length 0
        (void)o;
        return;
    }
    case 51: return;
    case 52: reply(c, 0, 0, 0); return;
    case 53: {                                                                     // CreatePixmap
        NEED(16);
        u32 id = g32(r, 4);
        int w = (int)g16(r, 12), h = (int)g16(r, 14), depth = r[1];
        if (!id_ok(c, id)) { send_error(c, ER_IDChoice, id, op, 0); return; }
        if (w <= 0 || h <= 0 || (depth != 1 && depth != 24 && depth != 32 && depth != 8 && depth != 16 && depth != 15)) { send_error(c, ER_Value, 0, op, 0); return; }
        if ((u64)w * (u64)h * 4 > (64u << 20)) { send_error(c, ER_Alloc, 0, op, 0); return; }
        struct pix *p = (struct pix*)xcalloc(sizeof *p);
        p->id = id; p->w = w; p->h = h; p->depth = depth == 1 ? 1 : 24;
        p->px = (u32*)xcalloc((u64)w * (u64)h * 4);
        res_put(id, RES_PIXMAP, p);
        return;
    }
    case 54: {
        NEED(8);
        struct pix *p = (struct pix*)res_get(g32(r, 4), RES_PIXMAP);
        if (!p) { send_error(c, ER_Pixmap, g32(r, 4), op, 0); return; }
        res_del(p->id); xfree(p->px); xfree(p);
        return;
    }
    case 55: {                                                                     // CreateGC
        NEED(16);
        u32 id = g32(r, 4), mask = g32(r, 12);
        if (!id_ok(c, id)) { send_error(c, ER_IDChoice, id, op, 0); return; }
        NEED(16 + 4 * popcnt(mask));
        struct gc *g = (struct gc*)xmalloc(sizeof *g);
        gc_defaults(g);
        g->id = id; g->owner = c;
        gc_apply(g, mask, r + 16);
        res_put(id, RES_GC, g);
        return;
    }
    case 56: {
        NEED(12);
        struct gc *g = (struct gc*)res_get(g32(r, 4), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 4), op, 0); return; }
        NEED(12 + 4 * popcnt(g32(r, 8)));
        gc_apply(g, g32(r, 8), r + 12);
        return;
    }
    case 57: {                                                                     // CopyGC
        NEED(16);
        struct gc *s = (struct gc*)res_get(g32(r, 4), RES_GC), *d = (struct gc*)res_get(g32(r, 8), RES_GC);
        if (!s || !d) { send_error(c, ER_GC, g32(r, s ? 8 : 4), op, 0); return; }
        u32 m = g32(r, 12);
        if (m & 1) d->function = s->function;
        if (m & 2) d->plane_mask = s->plane_mask;
        if (m & 4) d->fg = s->fg;
        if (m & 8) d->bg = s->bg;
        if (m & 0x10) d->line_width = s->line_width;
        if (m & 0x20) d->line_style = s->line_style;
        if (m & 0x40) d->cap_style = s->cap_style;
        if (m & 0x80) d->join_style = s->join_style;
        if (m & 0x100) d->fill_style = s->fill_style;
        if (m & 0x200) d->fill_rule = s->fill_rule;
        if (m & 0x400) d->tile = s->tile;
        if (m & 0x800) d->stipple = s->stipple;
        if (m & 0x1000) d->ts_x = s->ts_x;
        if (m & 0x2000) d->ts_y = s->ts_y;
        if (m & 0x4000) d->font = s->font;
        if (m & 0x8000) d->subwindow_mode = s->subwindow_mode;
        if (m & 0x10000) d->graphics_exposures = s->graphics_exposures;
        if (m & 0x400000) d->arc_mode = s->arc_mode;
        return;
    }
    case 58: {                                                                     // SetDashes
        NEED(12);
        struct gc *g = (struct gc*)res_get(g32(r, 4), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 4), op, 0); return; }
        u32 n = g16(r, 10);
        NEED(12 + n);
        if (!n || n > 16) { send_error(c, ER_Value, n, op, 0); return; }
        g->dash_offset = (int)g16(r, 8); g->ndash = (int)n;
        for (u32 i = 0; i < n; i++) g->dashes[i] = r[12 + i];
        return;
    }
    case 59: {                                                                     // SetClipRectangles
        NEED(12);
        struct gc *g = (struct gc*)res_get(g32(r, 4), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 4), op, 0); return; }
        int n = (int)(len - 12) / 8;
        xfree(g->clip);
        g->clip = (Rect*)xmalloc((u64)(n ? n : 1) * sizeof(Rect));
        g->nclip = n; g->clip_x = gi16(r, 8); g->clip_y = gi16(r, 10);
        for (int i = 0; i < n; i++) { g->clip[i].x = gi16(r, 12 + 8 * i); g->clip[i].y = gi16(r, 14 + 8 * i); g->clip[i].w = (int)g16(r, 16 + 8 * i); g->clip[i].h = (int)g16(r, 18 + 8 * i); }
        return;
    }
    case 60: {
        NEED(8);
        struct gc *g = (struct gc*)res_get(g32(r, 4), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 4), op, 0); return; }
        res_del(g->id); xfree(g->clip); xfree(g);
        return;
    }
    case 61: {                                                                     // ClearArea
        NEED(16);
        struct win *w = win_of(g32(r, 4));
        if (!w) BADWIN(g32(r, 4));
        expose_clear(w, gi16(r, 8), gi16(r, 10), (int)g16(r, 12), (int)g16(r, 14), r[1]);
        return;
    }
    case 62: case 63: {                                                            // CopyArea, CopyPlane
        NEED(28);
        u32 sid = g32(r, 4), did = g32(r, 8);
        struct gc *g = (struct gc*)res_get(g32(r, 12), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 12), op, 0); return; }
        struct dstate s, d;
        if (!draw_open(sid, g, &s)) { send_error(c, ER_Drawable, sid, op, 0); return; }
        if (!draw_open(did, g, &d)) { send_error(c, ER_Drawable, did, op, 0); return; }
        int sx = gi16(r, 16), sy = gi16(r, 18), dx = gi16(r, 20), dy = gi16(r, 22), w = (int)g16(r, 24), h = (int)g16(r, 26);
        // the source of a window copy shows what is on the screen: use the whole top-level buffer
        if (op == 62) draw_copy(&d, &s, sx, sy, w, h, dx, dy);
        else { NEED(32); draw_copy_plane(&d, &s, sx, sy, w, h, dx, dy, g32(r, 28)); }
        draw_close(&d);
        if (g->graphics_exposures) {
            u8 *e = out_reserve(c, 32);
            e[0] = EV_NoExpose; p16(e, 2, c->seq); p32(e, 4, did); p16(e, 8, 0); e[10] = (u8)op;
        }
        return;
    }
    case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: {    // shapes
        int hdr = op == 69 ? 16 : 12;
        NEED(hdr);
        u32 did = g32(r, 4);
        struct gc *g = (struct gc*)res_get(g32(r, 8), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 8), op, 0); return; }
        struct dstate d;
        if (!draw_open(did, g, &d)) { send_error(c, ER_Drawable, did, op, 0); return; }
        const u8 *q = r + hdr;
        u32 left = len - (u32)hdr;
        switch (op) {
        case 64: draw_points(&d, q, (int)(left / 4), r[1]); break;
        case 65: draw_polyline(&d, q, (int)(left / 4), r[1]); break;
        case 66: draw_segments(&d, q, (int)(left / 8)); break;
        case 67: for (u32 i = 0; i + 8 <= left; i += 8) draw_rect_outline(&d, gi16(q, (int)i), gi16(q, (int)i + 2), (int)g16(q, (int)i + 4), (int)g16(q, (int)i + 6)); break;
        case 68: case 71:
            for (u32 i = 0; i + 12 <= left; i += 12)
                draw_arc(&d, gi16(q, (int)i), gi16(q, (int)i + 2), (int)g16(q, (int)i + 4), (int)g16(q, (int)i + 6), gi16(q, (int)i + 8), gi16(q, (int)i + 10), op == 71);
            break;
        case 69: draw_fillpoly(&d, q, (int)(left / 4), r[12], r[13]); break;
        case 70: for (u32 i = 0; i + 8 <= left; i += 8) draw_rect(&d, gi16(q, (int)i), gi16(q, (int)i + 2), (int)g16(q, (int)i + 4), (int)g16(q, (int)i + 6)); break;
        }
        draw_close(&d);
        return;
    }
    case 72: {                                                                     // PutImage
        NEED(24);
        u32 did = g32(r, 4);
        struct gc *g = (struct gc*)res_get(g32(r, 8), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 8), op, 0); return; }
        struct dstate d;
        if (!draw_open(did, g, &d)) { send_error(c, ER_Drawable, did, op, 0); return; }
        draw_put_image(&d, r[1], r[21], (int)g16(r, 12), (int)g16(r, 14), gi16(r, 16), gi16(r, 18), r[20], r + 24, len - 24);
        draw_close(&d);
        return;
    }
    case 73: {                                                                     // GetImage
        NEED(20);
        u32 did = g32(r, 4);
        struct dstate d;
        if (!draw_open(did, 0, &d)) { send_error(c, ER_Drawable, did, op, 0); return; }
        int x = gi16(r, 8), y = gi16(r, 10), w = (int)g16(r, 12), h = (int)g16(r, 14);
        if (!d.px || w <= 0 || h <= 0 || (u64)w * (u64)h * 4 > (64u << 20)) { send_error(c, ER_Match, did, op, 0); return; }
        u32 bytes = d.depth == 1 ? (u32)(((w + 31) / 32) * 4 * h) : (u32)w * (u32)h * 4;
        u8 *o = reply(c, d.depth, bytes, 0);
        p32(o, 8, win_of(did) ? XVisual : 0);
        draw_get_image(&d, x, y, w, h, g32(r, 16), r[1], o + 32);
        return;
    }
    case 74: case 75: case 76: case 77: {                                          // text
        int image = op >= 76, wide = (op == 75 || op == 77);
        NEED(16);
        u32 did = g32(r, 4);
        struct gc *g = (struct gc*)res_get(g32(r, 8), RES_GC);
        if (!g) { send_error(c, ER_GC, g32(r, 8), op, 0); return; }
        struct dstate d;
        if (!draw_open(did, g, &d)) { send_error(c, ER_Drawable, did, op, 0); return; }
        int x = gi16(r, 12), y = gi16(r, 14);
        struct font *f = g->font ? (struct font*)res_get(g->font, RES_FONT) : 0;
        if (image) {
            int n = r[1];
            if ((u32)(16 + n * (wide ? 2 : 1)) <= len) draw_text(&d, f, x, y, r + 16, n, 1, wide);
        } else {
            u32 i = 16;
            while (i + 2 <= len) {
                int n = r[i];
                if (n == 0) break;
                if (n == 255) {
                    if (i + 5 > len) break;
                    u32 fid = ((u32)r[i + 1] << 24) | ((u32)r[i + 2] << 16) | ((u32)r[i + 3] << 8) | r[i + 4];
                    g->font = fid; f = (struct font*)res_get(fid, RES_FONT);
                    i += 5;
                    continue;
                }
                x += (i8)r[i + 1];
                u32 bytes = (u32)n * (wide ? 2 : 1);
                if (i + 2 + bytes > len) break;
                struct font *use = f ? f : font_default();
                draw_text(&d, use, x, y, r + i + 2, n, 0, wide);
                x += n * use->cw;
                i += 2 + bytes;
            }
        }
        draw_close(&d);
        return;
    }
    case 78: case 79: case 80: case 81: case 82: return;
    case 83: { u8 *o = reply(c, 0, 4, 0); p16(o, 8, 1); p32(o, 32, XColormap); return; }
    case 84: {                                                                     // AllocColor
        NEED(16);
        u32 rr = g16(r, 8) >> 8, gg = g16(r, 10) >> 8, bb = g16(r, 12) >> 8;
        u8 *o = reply(c, 0, 0, 0);
        p16(o, 8, rr * 257); p16(o, 10, gg * 257); p16(o, 12, bb * 257); p32(o, 16, (rr << 16) | (gg << 8) | bb);
        return;
    }
    case 85: case 92: {                                                            // AllocNamedColor, LookupColor
        NEED(12);
        u32 n = g16(r, 8), cr, cg, cb;
        NEED(12 + n);
        if (!color_by_name((const char*)r + 12, (int)n, &cr, &cg, &cb)) { send_error(c, ER_Name, 0, op, 0); return; }
        u8 *o = reply(c, 0, 0, 0);
        u32 er = (cr >> 8) * 257, eg = (cg >> 8) * 257, eb = (cb >> 8) * 257;
        if (op == 85) { p32(o, 8, pixel_rgb16(cr, cg, cb)); p16(o, 12, cr); p16(o, 14, cg); p16(o, 16, cb); p16(o, 18, er); p16(o, 20, eg); p16(o, 22, eb); }
        else { p16(o, 8, cr); p16(o, 10, cg); p16(o, 12, cb); p16(o, 14, er); p16(o, 16, eg); p16(o, 18, eb); }
        return;
    }
    case 86: case 87: send_error(c, ER_Alloc, 0, op, 0); return;
    case 88: case 89: case 90: return;
    case 91: {                                                                     // QueryColors
        NEED(8);
        int n = (int)(len - 8) / 4;
        u8 *o = reply(c, 0, (u32)n * 8, 0);
        p16(o, 8, (u32)n);
        for (int i = 0; i < n; i++) {
            u32 px = g32(r, 8 + 4 * i);
            p16(o, 32 + 8 * i, ((px >> 16) & 255) * 257); p16(o, 34 + 8 * i, ((px >> 8) & 255) * 257); p16(o, 36 + 8 * i, (px & 255) * 257);
        }
        return;
    }
    case 93: case 94: {                                                            // CreateCursor, CreateGlyphCursor
        NEED(8);
        u32 id = g32(r, 4);
        if (!id_ok(c, id)) { send_error(c, ER_IDChoice, id, op, 0); return; }
        res_put(id, RES_CURSOR, xcalloc(8));
        return;
    }
    case 95: { NEED(8); void *p = res_get(g32(r, 4), RES_CURSOR); if (p) { res_del(g32(r, 4)); xfree(p); } return; }
    case 96: return;
    case 97: { NEED(12); u8 *o = reply(c, 0, 0, 0); p16(o, 8, g16(r, 8)); p16(o, 10, g16(r, 10)); return; }
    case 98: {                                                                     // QueryExtension: none
        NEED(8);
        reply(c, 0, 0, 0);
        return;
    }
    case 99: reply(c, 0, 0, 0); return;
    case 100: return;
    case 101: {                                                                    // GetKeyboardMapping
        NEED(8);
        int first = r[4], count = r[5];
        u8 *o = reply(c, 2, (u32)count * 8, 0);
        for (int i = 0; i < count; i++) { p32(o, 32 + 8 * i, keysym_for(first + i, 0)); p32(o, 36 + 8 * i, keysym_for(first + i, 1)); }
        return;
    }
    case 102: return;
    case 103: { u8 *o = reply(c, 1, 20, 0); p32(o, 4, 5); o[13] = 50; p16(o, 14, 400); p16(o, 16, 100); for (int i = 0; i < 32; i++) o[20 + i] = 0xFF; return; }
    case 104: case 105: return;
    case 106: { u8 *o = reply(c, 0, 0, 0); p16(o, 8, 2); p16(o, 10, 1); p16(o, 12, 4); return; }
    case 107: return;
    case 108: reply(c, 0, 0, 0); return;
    case 109: return;
    case 110: reply(c, 0, 0, 0); return;
    case 111: case 112: return;
    case 113: {
        NEED(8);
        u32 id = g32(r, 4);
        int idx = (int)(id >> 21);
        if (idx > 0 && idx < MAXC && clients[idx]) clients[idx]->dead = 1;
        return;
    }
    case 114: case 115: return;
    case 116: reply(c, 0, 0, 0); return;
    case 117: { u8 *o = reply(c, 5, 8, 0); for (int i = 0; i < 5; i++) o[32 + i] = (u8)(i + 1); return; }
    case 118: reply(c, 0, 0, 0); return;
    case 119: {                                                                    // GetModifierMapping: shift, lock, control, mod1 .. mod5 (2 keycodes each)
        u8 *o = reply(c, 2, 16, 0);
        static const u8 m[16] = { 50, 62, 66, 0, 37, 105, 64, 108, 0, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(o + 32, m, 16);
        return;
    }
    case 127: return;
    default:
        send_error(c, ER_Request, 0, op, 0);
        return;
    }
}

void req_client_gone(struct client *c) { (void)c; }
