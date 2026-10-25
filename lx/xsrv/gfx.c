// X11 drawing: regions, pixel writes with raster function / plane mask / fill style, shapes, text, images.
#include "xsrv.h"

struct fglyph *font_glyph(struct font *f, int c);

// ---- regions ------------------------------------------------------------------------------------------------------------------------
void rgn_set(Region *g, int x, int y, int w, int h) {
    g->n = 0;
    if (w > 0 && h > 0) { g->r[0].x = x; g->r[0].y = y; g->r[0].w = w; g->r[0].h = h; g->n = 1; }
}

void rgn_isect(Region *g, int x, int y, int w, int h) {
    int n = 0;
    for (int i = 0; i < g->n; i++) {
        Rect r = g->r[i];
        int x0 = imax(r.x, x), y0 = imax(r.y, y), x1 = imin(r.x + r.w, x + w), y1 = imin(r.y + r.h, y + h);
        if (x1 > x0 && y1 > y0) { g->r[n].x = x0; g->r[n].y = y0; g->r[n].w = x1 - x0; g->r[n].h = y1 - y0; n++; }
    }
    g->n = n;
}

void rgn_sub(Region *g, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    Rect out[RGN_MAX]; int n = 0;
    for (int i = 0; i < g->n; i++) {
        Rect r = g->r[i];
        int x0 = imax(r.x, x), y0 = imax(r.y, y), x1 = imin(r.x + r.w, x + w), y1 = imin(r.y + r.h, y + h);
        if (x1 <= x0 || y1 <= y0) { if (n < RGN_MAX) out[n++] = r; continue; }
        if (n + 4 > RGN_MAX - (g->n - i - 1)) { if (n < RGN_MAX) out[n++] = r; continue; }   // no room: leave it whole (overdraw)
        if (y0 > r.y) { out[n].x = r.x; out[n].y = r.y; out[n].w = r.w; out[n].h = y0 - r.y; n++; }
        if (x0 > r.x) { out[n].x = r.x; out[n].y = y0; out[n].w = x0 - r.x; out[n].h = y1 - y0; n++; }
        if (x1 < r.x + r.w) { out[n].x = x1; out[n].y = y0; out[n].w = r.x + r.w - x1; out[n].h = y1 - y0; n++; }
        if (y1 < r.y + r.h) { out[n].x = r.x; out[n].y = y1; out[n].w = r.w; out[n].h = r.y + r.h - y1; n++; }
    }
    for (int i = 0; i < n; i++) g->r[i] = out[i];
    g->n = n;
}

void rgn_bbox(const Region *g, Rect *r) {
    if (!g->n) { r->x = r->y = r->w = r->h = 0; return; }
    int x0 = g->r[0].x, y0 = g->r[0].y, x1 = x0 + g->r[0].w, y1 = y0 + g->r[0].h;
    for (int i = 1; i < g->n; i++) {
        x0 = imin(x0, g->r[i].x); y0 = imin(y0, g->r[i].y);
        x1 = imax(x1, g->r[i].x + g->r[i].w); y1 = imax(y1, g->r[i].y + g->r[i].h);
    }
    r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
}

// ---- opening a drawable -----------------------------------------------------------------------------------------------------------------
static struct font *gc_font(struct gc *gc) {
    struct font *f = gc && gc->font ? (struct font*)res_get(gc->font, RES_FONT) : 0;
    return f ? f : font_default();
}

int draw_open(u32 id, struct gc *gc, struct dstate *d) {
    memset(d, 0, sizeof *d);
    d->gc = gc;
    d->bx0 = d->by0 = 0x7FFFFFFF; d->bx1 = d->by1 = -0x7FFFFFFF;
    struct pix *p = (struct pix*)res_get(id, RES_PIXMAP);
    if (p) {
        d->px = p->px; d->sw = p->w; d->sh = p->h; d->depth = p->depth;
        rgn_set(&d->clip, 0, 0, p->w, p->h);
    } else {
        struct win *w = win_of(id);
        if (!w) return 0;
        d->depth = w->depth ? w->depth : 24;
        if (w == root || w->inputonly) { d->clip.n = 0; d->px = 0; return 1; }
        struct win *t = win_top(w);
        d->top = t;
        if (!t->px || !win_viewable(w)) { d->clip.n = 0; d->px = 0; return 1; }
        d->px = t->px; d->sw = t->bw; d->sh = t->bh;
        win_abs(w, &d->ox, &d->oy);
        win_visible(w, &d->clip, !gc || gc->subwindow_mode == 0);
        d->viewable = 1;
    }
    // GC clip rectangles (relative to the clip origin in drawable coordinates)
    if (gc && gc->nclip >= 0) {
        Region src = d->clip, res; res.n = 0;
        for (int i = 0; i < gc->nclip; i++) {
            Region t = src;
            rgn_isect(&t, d->ox + gc->clip_x + gc->clip[i].x, d->oy + gc->clip_y + gc->clip[i].y, gc->clip[i].w, gc->clip[i].h);
            for (int k = 0; k < t.n && res.n < RGN_MAX; k++) res.r[res.n++] = t.r[k];
        }
        d->clip = res;
    }
    return 1;
}

void draw_close(struct dstate *d) {
    if (d->top && d->bx1 > d->bx0 && d->by1 > d->by0) top_dirty(d->top, d->bx0, d->by0, d->bx1, d->by1);
}

// ---- pixels -----------------------------------------------------------------------------------------------------------------------------
static u32 combine(int fn, u32 s, u32 dd) {
    switch (fn) {
    case 0: return 0;
    case 1: return s & dd;
    case 2: return s & ~dd;
    case 3: return s;
    case 4: return ~s & dd;
    case 5: return dd;
    case 6: return s ^ dd;
    case 7: return s | dd;
    case 8: return ~(s | dd);
    case 9: return ~(s ^ dd);
    case 10: return ~dd;
    case 11: return s | ~dd;
    case 12: return ~s;
    case 13: return ~s | dd;
    case 14: return ~(s & dd);
    default: return 0xFFFFFFFFu;
    }
}

static inline u32 depth_mask(const struct dstate *d) { return d->depth == 1 ? 1u : 0xFFFFFFu; }

static inline void touch(struct dstate *d, int x0, int y0, int x1, int y1) {
    if (x0 < d->bx0) d->bx0 = x0;
    if (y0 < d->by0) d->by0 = y0;
    if (x1 > d->bx1) d->bx1 = x1;
    if (y1 > d->by1) d->by1 = y1;
}

static inline void store(struct dstate *d, int x, int y, u32 src) {
    struct gc *gc = d->gc;
    u32 *p = &d->px[y * d->sw + x];
    u32 m = depth_mask(d);
    if (!gc || (gc->function == 3 && (gc->plane_mask & m) == m)) { *p = src & m; return; }
    u32 old = *p, v = combine(gc->function, src, old);
    u32 pm = gc->plane_mask & m;
    *p = ((v & pm) | (old & ~pm)) & m;
}

// colour of the fill pattern at drawable coordinates (x, y); returns 0 when the pixel is not drawn
static int source_pixel(struct dstate *d, int x, int y, u32 *col) {
    struct gc *gc = d->gc;
    if (!gc || gc->fill_style == 0) { *col = gc ? gc->fg : 0; return 1; }
    int fs = gc->fill_style;
    if (fs == 1) {                                                             // tiled
        struct pix *t = (struct pix*)res_get(gc->tile, RES_PIXMAP);
        if (!t || !t->w || !t->h) { *col = gc->fg; return 1; }
        int tx = (x - gc->ts_x) % t->w, ty = (y - gc->ts_y) % t->h;
        if (tx < 0) tx += t->w;
        if (ty < 0) ty += t->h;
        *col = t->px[ty * t->w + tx];
        return 1;
    }
    struct pix *s = (struct pix*)res_get(gc->stipple, RES_PIXMAP);
    if (!s || !s->w || !s->h) { *col = gc->fg; return 1; }
    int sx = (x - gc->ts_x) % s->w, sy = (y - gc->ts_y) % s->h;
    if (sx < 0) sx += s->w;
    if (sy < 0) sy += s->h;
    if (s->px[sy * s->w + sx] & 1) { *col = gc->fg; return 1; }
    if (fs == 3) { *col = gc->bg; return 1; }                                  // opaque stippled
    return 0;
}

static void put(struct dstate *d, int x, int y) {                              // surface coordinates, clipped
    for (int i = 0; i < d->clip.n; i++) {
        Rect *r = &d->clip.r[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            u32 c;
            if (source_pixel(d, x - d->ox, y - d->oy, &c)) { store(d, x, y, c); touch(d, x, y, x + 1, y + 1); }
            return;
        }
    }
}

static void put_col(struct dstate *d, int x, int y, u32 c) {                   // a given colour, clipped
    for (int i = 0; i < d->clip.n; i++) {
        Rect *r = &d->clip.r[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) { store(d, x, y, c); touch(d, x, y, x + 1, y + 1); return; }
    }
}

static void span(struct dstate *d, int x0, int x1, int y) {                    // x0..x1 inclusive, surface coordinates
    if (x1 < x0) return;
    struct gc *gc = d->gc;
    int fast = gc && gc->fill_style == 0 && gc->function == 3 && (gc->plane_mask & depth_mask(d)) == depth_mask(d);
    for (int i = 0; i < d->clip.n; i++) {
        Rect *r = &d->clip.r[i];
        if (y < r->y || y >= r->y + r->h) continue;
        int lo = imax(x0, r->x), hi = imin(x1, r->x + r->w - 1);
        if (hi < lo) continue;
        u32 *row = &d->px[y * d->sw];
        if (fast) { u32 c = gc->fg & depth_mask(d); for (int x = lo; x <= hi; x++) row[x] = c; }
        else for (int x = lo; x <= hi; x++) { u32 c; if (source_pixel(d, x - d->ox, y - d->oy, &c)) store(d, x, y, c); }
        touch(d, lo, y, hi + 1, y + 1);
    }
}

void draw_rect(struct dstate *d, int x, int y, int w, int h) {
    if (!d->px || w <= 0 || h <= 0) return;
    int x0 = d->ox + x, y0 = d->oy + y;
    for (int j = 0; j < h; j++) span(d, x0, x0 + w - 1, y0 + j);
}

// ---- lines ----------------------------------------------------------------------------------------------------------------------------
static const u8 default_dash[2] = { 4, 4 };

static void thin_line(struct dstate *d, int x0, int y0, int x1, int y1, int *dpos) {
    struct gc *gc = d->gc;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int dashed = gc && gc->line_style != 0;
    const u8 *dash = gc && gc->ndash ? gc->dashes : default_dash;
    int nd = gc && gc->ndash ? gc->ndash : 2;
    for (;;) {
        if (!dashed) put(d, x0, y0);
        else {
            int tot = 0; for (int i = 0; i < nd; i++) tot += dash[i] ? dash[i] : 1;
            int pos = ((*dpos + gc->dash_offset) % tot + tot) % tot, i = 0;
            while (pos >= (dash[i] ? dash[i] : 1)) { pos -= dash[i] ? dash[i] : 1; i++; }
            if (!(i & 1)) put(d, x0, y0);
            else if (gc->line_style == 2) put_col(d, x0, y0, gc->bg);
            (*dpos)++;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

struct fpt { double x, y; };

static int ifloor(double v) { int i = (int)v; return (double)i > v ? i - 1 : i; }
static int iceil(double v) { int i = (int)v; return (double)i < v ? i + 1 : i; }

static void fill_poly_f(struct dstate *d, const struct fpt *p, int n, int winding);

static void line_seg(struct dstate *d, int x0, int y0, int x1, int y1, int *dpos) {      // drawable coordinates
    struct gc *gc = d->gc;
    int lw = gc ? gc->line_width : 0;
    if (lw <= 1) { thin_line(d, d->ox + x0, d->oy + y0, d->ox + x1, d->oy + y1, dpos); return; }
    double fx = (double)(x1 - x0), fy = (double)(y1 - y0), len = dsqrt(fx * fx + fy * fy);
    double hw = lw / 2.0, ux, uy;
    if (len < 0.5) { ux = 1; uy = 0; fx = fy = 0; } else { ux = fx / len; uy = fy / len; }
    double nx = -uy * hw, ny = ux * hw;
    double ex = 0, ey = 0;
    if (gc && gc->cap_style == 3) { ex = ux * hw; ey = uy * hw; }              // projecting
    struct fpt q[4] = {
        { d->ox + x0 + 0.5 - ex + nx, d->oy + y0 + 0.5 - ey + ny }, { d->ox + x1 + 0.5 + ex + nx, d->oy + y1 + 0.5 + ey + ny },
        { d->ox + x1 + 0.5 + ex - nx, d->oy + y1 + 0.5 + ey - ny }, { d->ox + x0 + 0.5 - ex - nx, d->oy + y0 + 0.5 - ey - ny } };
    fill_poly_f(d, q, 4, 1);
    if (gc && gc->cap_style == 2 && lw > 2) {                                  // round caps: a disc at each end
        for (int e = 0; e < 2; e++) {
            double cx = d->ox + (e ? x1 : x0) + 0.5, cy = d->oy + (e ? y1 : y0) + 0.5;
            struct fpt c[24];
            for (int k = 0; k < 24; k++) { c[k].x = cx + hw * dcos(k * 15.0); c[k].y = cy - hw * dsin(k * 15.0); }
            fill_poly_f(d, c, 24, 1);
        }
    }
}

static void joint(struct dstate *d, int x, int y) {                            // join of a wide polyline: a disc (round) or a square (miter approximation)
    struct gc *gc = d->gc;
    if (!gc || gc->line_width <= 2) return;
    double hw = gc->line_width / 2.0, cx = d->ox + x + 0.5, cy = d->oy + y + 0.5;
    struct fpt c[24];
    int n = 24;
    for (int k = 0; k < n; k++) { c[k].x = cx + hw * dcos(k * 15.0); c[k].y = cy - hw * dsin(k * 15.0); }
    fill_poly_f(d, c, n, 1);
}

void draw_points(struct dstate *d, const u8 *pts, int n, int rel) {
    if (!d->px) return;
    int x = 0, y = 0;
    for (int i = 0; i < n; i++) {
        int px = gi16(pts, i * 4), py = gi16(pts, i * 4 + 2);
        if (rel && i) { x += px; y += py; } else { x = px; y = py; }
        put(d, d->ox + x, d->oy + y);
    }
}

void draw_polyline(struct dstate *d, const u8 *pts, int n, int rel) {
    if (!d->px || n < 1) return;
    int x = gi16(pts, 0), y = gi16(pts, 2), dpos = 0;
    if (n == 1) { put(d, d->ox + x, d->oy + y); return; }
    for (int i = 1; i < n; i++) {
        int px = gi16(pts, i * 4), py = gi16(pts, i * 4 + 2);
        int nx = rel ? x + px : px, ny = rel ? y + py : py;
        line_seg(d, x, y, nx, ny, &dpos);
        if (i < n - 1 && d->gc && d->gc->line_width > 2) joint(d, nx, ny);
        x = nx; y = ny;
    }
}

void draw_segments(struct dstate *d, const u8 *seg, int n) {
    if (!d->px) return;
    for (int i = 0; i < n; i++) {
        int dpos = 0;
        line_seg(d, gi16(seg, i * 8), gi16(seg, i * 8 + 2), gi16(seg, i * 8 + 4), gi16(seg, i * 8 + 6), &dpos);
    }
}

void draw_rect_outline(struct dstate *d, int x, int y, int w, int h) {
    if (!d->px) return;
    int dpos = 0;
    line_seg(d, x, y, x + w, y, &dpos);
    line_seg(d, x + w, y, x + w, y + h, &dpos);
    line_seg(d, x + w, y + h, x, y + h, &dpos);
    line_seg(d, x, y + h, x, y, &dpos);
}

// ---- polygons ---------------------------------------------------------------------------------------------------------------------------
static void fill_poly_f(struct dstate *d, const struct fpt *p, int n, int winding) {
    if (n < 3 || !d->px) return;
    double ymin = p[0].y, ymax = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
    int y0 = (int)(ymin - 0.5) - 1, y1 = (int)(ymax + 0.5) + 1;
    Rect bb; rgn_bbox(&d->clip, &bb);
    if (y0 < bb.y) y0 = bb.y;
    if (y1 > bb.y + bb.h) y1 = bb.y + bb.h;
    double *xs = (double*)xmalloc((u64)n * sizeof(double));
    int *dir = (int*)xmalloc((u64)n * sizeof(int));
    for (int y = y0; y < y1; y++) {
        double cy = y + 0.5;
        int c = 0;
        for (int i = 0; i < n; i++) {
            const struct fpt *a = &p[i], *b = &p[(i + 1) % n];
            if (a->y == b->y) continue;
            int up = a->y < b->y;
            double ya = up ? a->y : b->y, yb = up ? b->y : a->y;
            if (cy < ya || cy >= yb) continue;
            double x = a->x + (cy - a->y) * (b->x - a->x) / (b->y - a->y);
            int k = c++;
            while (k > 0 && xs[k - 1] > x) { xs[k] = xs[k - 1]; dir[k] = dir[k - 1]; k--; }
            xs[k] = x; dir[k] = up ? 1 : -1;
        }
        int wind = 0;
        for (int i = 0; i + 1 < c || (winding && i < c); i++) {
            int inside;
            if (winding) { wind += dir[i]; inside = wind != 0; } else inside = (i & 1) == 0;
            if (!inside || i + 1 >= c) continue;
            int xa = iceil(xs[i] - 0.5), xb = iceil(xs[i + 1] - 0.5) - 1;       // pixels whose centre lies in [xs[i], xs[i+1])
            span(d, xa, xb, y);
        }
    }
    xfree(xs); xfree(dir);
}

void draw_fillpoly(struct dstate *d, const u8 *pts, int n, int shape, int rel) {
    (void)shape;
    if (!d->px || n < 3) return;
    struct fpt *f = (struct fpt*)xmalloc((u64)n * sizeof *f);
    int x = 0, y = 0;
    for (int i = 0; i < n; i++) {
        int px = gi16(pts, i * 4), py = gi16(pts, i * 4 + 2);
        if (rel && i) { x += px; y += py; } else { x = px; y = py; }
        f[i].x = d->ox + x; f[i].y = d->oy + y;
    }
    fill_poly_f(d, f, n, d->gc && d->gc->fill_rule == 1);
    xfree(f);
}

// ---- arcs ---------------------------------------------------------------------------------------------------------------------------------
void draw_arc(struct dstate *d, int x, int y, int w, int h, int a1, int a2, int fill) {
    if (!d->px || w <= 0 || h <= 0) return;
    if (a2 > 360 * 64) a2 = 360 * 64;
    if (a2 < -360 * 64) a2 = -360 * 64;
    int full = a2 >= 360 * 64 || a2 <= -360 * 64;
    double rx = w / 2.0, ry = h / 2.0, cx = d->ox + x + rx, cy = d->oy + y + ry;
    double start = a1 / 64.0, ext = a2 / 64.0;
    int steps = (int)((rx + ry) * (ext < 0 ? -ext : ext) / 360.0 * 1.2) + 8;
    if (steps > 720) steps = 720;
    struct fpt *p = (struct fpt*)xmalloc((u64)(steps + 2) * sizeof *p);
    int n = 0;
    for (int i = 0; i <= steps; i++) {
        double t = start + ext * i / steps;
        p[n].x = cx + rx * dcos(t); p[n].y = cy - ry * dsin(t); n++;
    }
    struct gc *gc = d->gc;
    if (fill) {
        if (!full && gc && gc->arc_mode == 1) { p[n].x = cx; p[n].y = cy; n++; }       // pie slice
        fill_poly_f(d, p, n, 0);
    } else {
        int dpos = 0;
        for (int i = 0; i + 1 < n; i++) {
            int ax = ifloor(p[i].x + 0.5) - d->ox, ay = ifloor(p[i].y + 0.5) - d->oy;
            int bx = ifloor(p[i + 1].x + 0.5) - d->ox, by = ifloor(p[i + 1].y + 0.5) - d->oy;
            line_seg(d, ax, ay, bx, by, &dpos);
        }
    }
    xfree(p);
}

// ---- copies ---------------------------------------------------------------------------------------------------------------------------------
static u32 *grab_rect(struct dstate *s, int sx, int sy, int w, int h, int *ok) {   // source rectangle (drawable coords) -> w*h pixels, ok = 0 where outside the surface
    u32 *t = (u32*)xmalloc((u64)w * (u64)h * 4);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int x = s->ox + sx + i, y = s->oy + sy + j;
            int in = x >= 0 && y >= 0 && x < s->sw && y < s->sh && s->px;
            t[j * w + i] = in ? s->px[y * s->sw + x] : 0;
            (void)ok;
        }
    return t;
}

void draw_copy(struct dstate *dst, struct dstate *src, int sx, int sy, int w, int h, int dx, int dy) {
    if (!dst->px || !src->px || w <= 0 || h <= 0) return;
    u32 *t = grab_rect(src, sx, sy, w, h, 0);
    u32 m = depth_mask(dst);
    int x0 = dst->ox + dx, y0 = dst->oy + dy;
    for (int i = 0; i < dst->clip.n; i++) {
        Rect *r = &dst->clip.r[i];
        int cx0 = imax(x0, r->x), cy0 = imax(y0, r->y), cx1 = imin(x0 + w, r->x + r->w), cy1 = imin(y0 + h, r->y + r->h);
        for (int y = cy0; y < cy1; y++)
            for (int x = cx0; x < cx1; x++) store(dst, x, y, t[(y - y0) * w + (x - x0)] & m);
        if (cx1 > cx0 && cy1 > cy0) touch(dst, cx0, cy0, cx1, cy1);
    }
    xfree(t);
}

void draw_copy_plane(struct dstate *dst, struct dstate *src, int sx, int sy, int w, int h, int dx, int dy, u32 plane) {
    if (!dst->px || !src->px || w <= 0 || h <= 0 || !dst->gc) return;
    u32 *t = grab_rect(src, sx, sy, w, h, 0);
    int x0 = dst->ox + dx, y0 = dst->oy + dy;
    for (int i = 0; i < dst->clip.n; i++) {
        Rect *r = &dst->clip.r[i];
        int cx0 = imax(x0, r->x), cy0 = imax(y0, r->y), cx1 = imin(x0 + w, r->x + r->w), cy1 = imin(y0 + h, r->y + r->h);
        for (int y = cy0; y < cy1; y++)
            for (int x = cx0; x < cx1; x++) store(dst, x, y, (t[(y - y0) * w + (x - x0)] & plane) ? dst->gc->fg : dst->gc->bg);
        if (cx1 > cx0 && cy1 > cy0) touch(dst, cx0, cy0, cx1, cy1);
    }
    xfree(t);
}

// ---- text -----------------------------------------------------------------------------------------------------------------------------------------
static void blend_glyph(struct dstate *d, struct fglyph *g, int gx, int gy, u32 fg) {      // gx, gy = surface position of the glyph mask's top-left
    for (int j = 0; j < g->h; j++) {
        int y = gy + j;
        if (y < 0 || y >= d->sh) continue;
        for (int i = 0; i < g->w; i++) {
            u32 a = g->m[j * g->w + i];
            if (!a) continue;
            int x = gx + i;
            if (x < 0 || x >= d->sw) continue;
            for (int k = 0; k < d->clip.n; k++) {
                Rect *r = &d->clip.r[k];
                if (x < r->x || x >= r->x + r->w || y < r->y || y >= r->y + r->h) continue;
                u32 *p = &d->px[y * d->sw + x];
                if (d->depth == 1) { if (a >= 128) store(d, x, y, fg); }
                else if (a >= 250 && d->gc->function == 3) store(d, x, y, fg);
                else if (d->gc->function != 3) { if (a >= 128) store(d, x, y, fg); }
                else {
                    u32 o = *p;
                    u32 rr = (((fg >> 16) & 255) * a + ((o >> 16) & 255) * (255 - a)) / 255;
                    u32 gg = (((fg >> 8) & 255) * a + ((o >> 8) & 255) * (255 - a)) / 255;
                    u32 bb = ((fg & 255) * a + (o & 255) * (255 - a)) / 255;
                    store(d, x, y, (rr << 16) | (gg << 8) | bb);
                }
                touch(d, x, y, x + 1, y + 1);
                break;
            }
        }
    }
}

void draw_text(struct dstate *d, struct font *f, int x, int y, const u8 *s, int n, int image, int wide) {
    if (!d->px || !d->gc) return;
    if (!f) f = gc_font(d->gc);
    int step = wide ? 2 : 1;
    int cnt = n;
    if (image) {                                                                // background cell rectangle first
        struct gc tmp = *d->gc;
        tmp.fill_style = 0; tmp.fg = d->gc->bg; tmp.function = 3; tmp.plane_mask = 0xFFFFFFFFu;
        struct gc *save = d->gc; d->gc = &tmp;
        draw_rect(d, x, y - f->asc, cnt * f->cw, f->asc + f->desc);
        d->gc = save;
    }
    u32 fg = d->gc->fg;
    for (int i = 0; i < cnt; i++) {
        int c = s[i * step];
        if (wide) { int hi = s[i * 2]; c = s[i * 2 + 1]; if (hi) c = '?'; }
        struct fglyph *g = font_glyph(f, c);
        if (g->m) blend_glyph(d, g, d->ox + x + i * f->cw, d->oy + y - g->asc, fg);
    }
}

// ---- images ----------------------------------------------------------------------------------------------------------------------------------------
void draw_put_image(struct dstate *d, int format, int depth, int w, int h, int dx, int dy, int left_pad, const u8 *data, u32 len) {
    if (!d->px || w <= 0 || h <= 0) return;
    int x0 = d->ox + dx, y0 = d->oy + dy;
    if (format == 2 && (depth == 24 || depth == 32)) {
        if ((u64)w * (u64)h * 4 > len) return;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) put_col(d, x0 + i, y0 + j, g32(data, (j * w + i) * 4) & 0xFFFFFF);
        return;
    }
    if (format == 0 || (format == 2 && depth == 1) || (format == 1 && depth == 1)) {       // bitmap rows of 32-bit units, LSB first
        int stride = ((w + left_pad + 31) / 32) * 4;
        if ((u64)stride * (u64)h > len) return;
        struct gc *gc = d->gc;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int bit = (data[j * stride + (i + left_pad) / 8] >> ((i + left_pad) & 7)) & 1;
                if (d->depth == 1 && format == 2) put_col(d, x0 + i, y0 + j, (u32)bit);
                else if (format == 0 && d->depth != 1) put_col(d, x0 + i, y0 + j, bit ? gc->fg : gc->bg);
                else put_col(d, x0 + i, y0 + j, (u32)bit);
            }
        return;
    }
    if (format == 1 && depth == 24) {                                           // XYPixmap: planes from the top (bit 23) down
        int stride = ((w + left_pad + 31) / 32) * 4, plane = stride * h;
        if ((u64)plane * 24 > len) return;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                u32 v = 0;
                for (int b = 0; b < 24; b++) {
                    const u8 *pl = data + plane * (23 - b);
                    v |= (u32)((pl[j * stride + (i + left_pad) / 8] >> ((i + left_pad) & 7)) & 1) << b;
                }
                put_col(d, x0 + i, y0 + j, v);
            }
    }
}

u32 draw_get_image(struct dstate *d, int x, int y, int w, int h, u32 plane_mask, int format, u8 *out) {
    if (!d->px) return 0;
    if (d->depth == 1) {
        int stride = ((w + 31) / 32) * 4;
        memset(out, 0, (u64)stride * (u64)h);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int sx = d->ox + x + i, sy = d->oy + y + j;
                u32 v = sx >= 0 && sy >= 0 && sx < d->sw && sy < d->sh ? d->px[sy * d->sw + sx] & 1 : 0;
                if (v) out[j * stride + i / 8] |= (u8)(1 << (i & 7));
            }
        return (u32)stride * (u32)h;
    }
    (void)format;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = d->ox + x + i, sy = d->oy + y + j;
            u32 v = sx >= 0 && sy >= 0 && sx < d->sw && sy < d->sh ? d->px[sy * d->sw + sx] & plane_mask & 0xFFFFFF : 0;
            p32(out, (j * w + i) * 4, v);
        }
    return (u32)w * (u32)h * 4;
}

// ---- window background ---------------------------------------------------------------------------------------------------------------------------
void draw_bg(struct win *w, const Region *rg, int ox, int oy, u32 *px, int sw, int sh) {
    struct win *b = w;
    int bx = ox, by = oy;
    while (b && b->bg_mode == 3 && b->parent && b->parent != root) { bx -= b->x + b->parent->border * 0; by -= b->y; b = b->parent; }
    if (!b || b->bg_mode == 0 || b->bg_mode == 3) return;
    struct pix *t = b->bg_mode == 2 ? (struct pix*)res_get(b->bg_pixmap, RES_PIXMAP) : 0;
    if (b->bg_mode == 2 && !t) return;
    for (int i = 0; i < rg->n; i++) {
        Rect r = rg->r[i];
        for (int y = imax(r.y, 0); y < imin(r.y + r.h, sh); y++)
            for (int x = imax(r.x, 0); x < imin(r.x + r.w, sw); x++) {
                u32 c = b->bg_pixel;
                if (t) {
                    int tx = (x - bx) % t->w, ty = (y - by) % t->h;
                    if (tx < 0) tx += t->w;
                    if (ty < 0) ty += t->h;
                    c = t->px[ty * t->w + tx];
                }
                px[y * sw + x] = c & 0xFFFFFF;
            }
    }
}
