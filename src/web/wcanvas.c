// Software 2D canvas (see wcanvas.h).

#include "wcanvas.h"
#include "wcommon.h"
#include "raster.h"
#include "font.h"
#include "surface.h"

#if defined(KERNEL) && KERNEL
double sqrt(double);
double sin(double);
double cos(double);
double atan2(double, double);
double floor(double);
double ceil(double);
double fabs(double);
double exp(double);
#else
#include <math.h>
#endif

#define PI 3.14159265358979323846
#define MAX_CLIP_STACK 64

struct clipmask { uint8_t* m; int ref; };

struct wcv {
    int w, h;
    uint32_t* px;
    struct clipmask* clip;                  // current (NULL = everything)
    struct clipmask* stack[MAX_CLIP_STACK];
    int nstack;
    unsigned serial;
    struct wraster ras;
};

// ---------------------------------------------------------------------------------------------
// lifecycle

struct wcv* wcv_new(int w, int h) {
    if (w < 0 || h < 0 || w > WCV_MAX_DIM || h > WCV_MAX_DIM || (int64_t)w * h > WCV_MAX_PIXELS) return 0;
    struct wcv* c = (struct wcv*)w_calloc(1, sizeof *c);
    if (!c) return 0;
    c->w = w; c->h = h;
    c->px = (uint32_t*)w_calloc((size_t)(w ? w : 1) * (h ? h : 1), 4);
    if (!c->px) { w_free(c); return 0; }
    wr_init(&c->ras);
    return c;
}

static void clip_unref(struct clipmask* m) {
    if (m && --m->ref <= 0) { w_free(m->m); w_free(m); }
}

void wcv_reset_clip(struct wcv* c) {
    clip_unref(c->clip);
    c->clip = 0;
    while (c->nstack) clip_unref(c->stack[--c->nstack]);
}

void wcv_free(struct wcv* c) {
    if (!c) return;
    wcv_reset_clip(c);
    wr_free(&c->ras);
    w_free(c->px);
    w_free(c);
}

int wcv_resize(struct wcv* c, int w, int h) {
    if (w < 0 || h < 0 || w > WCV_MAX_DIM || h > WCV_MAX_DIM || (int64_t)w * h > WCV_MAX_PIXELS) return -1;
    uint32_t* n = (uint32_t*)w_calloc((size_t)(w ? w : 1) * (h ? h : 1), 4);
    if (!n) return -1;
    w_free(c->px);
    c->px = n;
    c->w = w; c->h = h;
    wcv_reset_clip(c);
    c->serial++;
    return 0;
}

int wcv_width(const struct wcv* c) { return c->w; }
int wcv_height(const struct wcv* c) { return c->h; }
uint32_t* wcv_pixels(struct wcv* c) { return c->px; }
unsigned wcv_serial(const struct wcv* c) { return c->serial; }
void wcv_touch(struct wcv* c) { c->serial++; }

void wcv_save(struct wcv* c) {
    if (c->nstack >= MAX_CLIP_STACK) return;                // deeper nesting: the clip simply is not restored
    if (c->clip) c->clip->ref++;
    c->stack[c->nstack++] = c->clip;
}

void wcv_restore(struct wcv* c) {
    if (!c->nstack) return;
    clip_unref(c->clip);
    c->clip = c->stack[--c->nstack];
}

// ---------------------------------------------------------------------------------------------
// small helpers

static inline unsigned mul255(unsigned a, unsigned b) {
    unsigned t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static inline int32_t to_fix(double v) {
    v *= 256.0;
    if (v > 2.0e9) v = 2.0e9;
    if (v < -2.0e9) v = -2.0e9;
    return (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
}

#define FIXLIM (1 << 22)                                   // 16384 px: beyond this the geometry is far off canvas anyway
static inline int32_t to_fix_clamped(double v) {
    double s = v * 256.0;
    if (s > FIXLIM * 4.0) s = FIXLIM * 4.0;
    if (s < -FIXLIM * 4.0) s = -FIXLIM * 4.0;
    return (int32_t)(s >= 0 ? s + 0.5 : s - 0.5);
}

// ---------------------------------------------------------------------------------------------
// path -> raster (fill) and flattening (stroke)

static int path_to_raster(struct wraster* r, const double* p, int n) {
    int i = 0, any = 0;
    wr_reset(r);
    while (i < n) {
        int cmd = (int)p[i++];
        switch (cmd) {
        case 0: if (i + 2 > n) return any; wr_move(r, to_fix_clamped(p[i]), to_fix_clamped(p[i + 1])); i += 2; break;
        case 1: if (i + 2 > n) return any; wr_line(r, to_fix_clamped(p[i]), to_fix_clamped(p[i + 1])); i += 2; any = 1; break;
        case 2: if (i + 4 > n) return any;
            wr_quad(r, to_fix_clamped(p[i]), to_fix_clamped(p[i + 1]), to_fix_clamped(p[i + 2]), to_fix_clamped(p[i + 3])); i += 4; any = 1; break;
        case 3: if (i + 6 > n) return any;
            wr_cubic(r, to_fix_clamped(p[i]), to_fix_clamped(p[i + 1]), to_fix_clamped(p[i + 2]), to_fix_clamped(p[i + 3]),
                     to_fix_clamped(p[i + 4]), to_fix_clamped(p[i + 5])); i += 6; any = 1; break;
        case 4: wr_close(r); break;
        default: return any;
        }
    }
    return any;
}

struct pt { double x, y; };
struct sub { int start, n; int closed; };
struct poly {
    struct pt* pts;
    int npts, cap;
    struct sub* subs;
    int nsubs, subcap;
};

static void poly_free(struct poly* p) { w_free(p->pts); w_free(p->subs); memset(p, 0, sizeof *p); }

static int poly_add(struct poly* p, double x, double y) {
    if (p->npts == p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        struct pt* n = (struct pt*)w_realloc(p->pts, (size_t)nc * sizeof *n);
        if (!n) return -1;
        p->pts = n; p->cap = nc;
    }
    p->pts[p->npts].x = x; p->pts[p->npts].y = y;
    p->npts++;
    return 0;
}

static int poly_begin(struct poly* p) {
    if (p->nsubs == p->subcap) {
        int nc = p->subcap ? p->subcap * 2 : 16;
        struct sub* n = (struct sub*)w_realloc(p->subs, (size_t)nc * sizeof *n);
        if (!n) return -1;
        p->subs = n; p->subcap = nc;
    }
    p->subs[p->nsubs].start = p->npts;
    p->subs[p->nsubs].n = 0;
    p->subs[p->nsubs].closed = 0;
    p->nsubs++;
    return 0;
}

static void poly_end(struct poly* p) {
    if (p->nsubs) p->subs[p->nsubs - 1].n = p->npts - p->subs[p->nsubs - 1].start;
}

#define FLAT_TOL 0.1

static void flat_cubic(struct poly* o, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3) {
    // deviation of the control points from the chord decides the segment count
    double dx = x3 - x0, dy = y3 - y0, len = sqrt(dx * dx + dy * dy), dev;
    if (len < 1e-9) {
        double a = sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)), b = sqrt((x2 - x0) * (x2 - x0) + (y2 - y0) * (y2 - y0));
        dev = a > b ? a : b;
    } else {
        double d1 = fabs((x1 - x0) * dy - (y1 - y0) * dx) / len, d2 = fabs((x2 - x0) * dy - (y2 - y0) * dx) / len;
        dev = d1 > d2 ? d1 : d2;
    }
    int n = (int)ceil(sqrt(0.75 * dev / FLAT_TOL)) + 1;
    if (n < 1) n = 1;
    if (n > 128) n = 128;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        poly_add(o, a * x0 + b * x1 + c * x2 + d * x3, a * y0 + b * y1 + c * y2 + d * y3);
    }
}

static int flatten(const double* p, int n, struct poly* o) {
    int i = 0, open = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    memset(o, 0, sizeof *o);
    while (i < n) {
        int cmd = (int)p[i++];
        switch (cmd) {
        case 0:
            if (i + 2 > n) goto done;
            if (open) poly_end(o);
            if (poly_begin(o) < 0) goto fail;
            open = 1;
            cx = sx = p[i]; cy = sy = p[i + 1]; i += 2;
            poly_add(o, cx, cy);
            break;
        case 1:
            if (i + 2 > n) goto done;
            if (!open) { if (poly_begin(o) < 0) goto fail; open = 1; sx = cx = p[i]; sy = cy = p[i + 1]; poly_add(o, cx, cy); i += 2; break; }
            cx = p[i]; cy = p[i + 1]; i += 2;
            poly_add(o, cx, cy);
            break;
        case 2: {
            if (i + 4 > n) goto done;
            if (!open) { if (poly_begin(o) < 0) goto fail; open = 1; sx = cx = p[i]; sy = cy = p[i + 1]; poly_add(o, cx, cy); }
            double qx = p[i], qy = p[i + 1], x = p[i + 2], y = p[i + 3]; i += 4;
            flat_cubic(o, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y), x, y);
            cx = x; cy = y;
            break;
        }
        case 3: {
            if (i + 6 > n) goto done;
            if (!open) { if (poly_begin(o) < 0) goto fail; open = 1; sx = cx = p[i]; sy = cy = p[i + 1]; poly_add(o, cx, cy); }
            flat_cubic(o, cx, cy, p[i], p[i + 1], p[i + 2], p[i + 3], p[i + 4], p[i + 5]);
            cx = p[i + 4]; cy = p[i + 5]; i += 6;
            break;
        }
        case 4:
            if (open) {
                poly_end(o);
                o->subs[o->nsubs - 1].closed = 1;
                open = 0;
                cx = sx; cy = sy;
                // a following drawing command starts a new subpath at the start point
                if (i < n && (int)p[i] != 0) {
                    if (poly_begin(o) < 0) goto fail;
                    open = 1;
                    poly_add(o, cx, cy);
                }
            }
            break;
        default: goto done;
        }
    }
done:
    if (open) poly_end(o);
    return 0;
fail:
    poly_free(o);
    return -1;
}

// ---------------------------------------------------------------------------------------------
// stroking: every segment, join and cap becomes a convex polygon of the same orientation, filled
// together with the nonzero rule

struct stroker {
    struct wraster* r;
    double hw;
    int cap, join;
    double miter;
};

static void emit_poly(struct stroker* s, const struct pt* v, int n) {
    double area = 0;
    for (int i = 0; i < n; i++) { const struct pt* a = &v[i]; const struct pt* b = &v[(i + 1) % n]; area += a->x * b->y - b->x * a->y; }
    if (area == 0) return;
    if (area > 0) {
        wr_move(s->r, to_fix_clamped(v[0].x), to_fix_clamped(v[0].y));
        for (int i = 1; i < n; i++) wr_line(s->r, to_fix_clamped(v[i].x), to_fix_clamped(v[i].y));
    } else {
        wr_move(s->r, to_fix_clamped(v[n - 1].x), to_fix_clamped(v[n - 1].y));
        for (int i = n - 2; i >= 0; i--) wr_line(s->r, to_fix_clamped(v[i].x), to_fix_clamped(v[i].y));
    }
    wr_close(s->r);
}

static void emit_circle(struct stroker* s, double cx, double cy) {
    double r = s->hw;
    int n = 8;
    if (r > 0.5) n = (int)ceil(PI / sqrt(2 * FLAT_TOL / r));          // chord error <= FLAT_TOL
    if (n < 8) n = 8;
    if (n > 96) n = 96;
    struct pt v[96];
    for (int i = 0; i < n; i++) { double a = 2 * PI * i / n; v[i].x = cx + r * cos(a); v[i].y = cy + r * sin(a); }
    emit_poly(s, v, n);
}

static void emit_cap(struct stroker* s, struct pt p, double dx, double dy) {
    // (dx, dy): unit direction pointing out of the stroke at this end
    if (s->cap == 1) { emit_circle(s, p.x, p.y); return; }
    if (s->cap == 2) {
        double nx = -dy * s->hw, ny = dx * s->hw, ex = dx * s->hw, ey = dy * s->hw;
        struct pt v[4] = { { p.x + nx, p.y + ny }, { p.x + nx + ex, p.y + ny + ey }, { p.x - nx + ex, p.y - ny + ey }, { p.x - nx, p.y - ny } };
        emit_poly(s, v, 4);
    }
}

static void emit_join(struct stroker* s, struct pt p, double d0x, double d0y, double d1x, double d1y) {
    double cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
    if (fabs(cross) < 1e-9 && dot > 0) return;                         // straight on
    if (s->join == 1) { emit_circle(s, p.x, p.y); return; }
    double side = cross > 0 ? -1.0 : 1.0;                              // outer side of the turn (left normal * side)
    double o0x = -d0y * s->hw * side, o0y = d0x * s->hw * side;
    double o1x = -d1y * s->hw * side, o1y = d1x * s->hw * side;
    if (s->join == 0 && dot > -0.9999) {
        double ratio = sqrt(2.0 / (1.0 + dot));                        // miter length / line width
        if (ratio <= s->miter) {
            double k = 1.0 / (1.0 + dot);
            struct pt v[4] = { p, { p.x + o0x, p.y + o0y }, { p.x + (o0x + o1x) * k, p.y + (o0y + o1y) * k }, { p.x + o1x, p.y + o1y } };
            emit_poly(s, v, 4);
            return;
        }
    }
    struct pt v[3] = { p, { p.x + o0x, p.y + o0y }, { p.x + o1x, p.y + o1y } };
    emit_poly(s, v, 3);
}

// One polyline (already without duplicate points when n >= 2).
static void stroke_line(struct stroker* s, const struct pt* a, int n, int closed) {
    if (n == 1) {
        if (s->cap == 1) emit_circle(s, a[0].x, a[0].y);
        else if (s->cap == 2) emit_cap(s, a[0], 1, 0), emit_cap(s, a[0], -1, 0);
        return;
    }
    int nseg = closed ? n : n - 1;
    double hw = s->hw;
    double pdx = 0, pdy = 0;
    double fdx = 0, fdy = 0;
    for (int i = 0; i < nseg; i++) {
        struct pt p0 = a[i], p1 = a[(i + 1) % n];
        double dx = p1.x - p0.x, dy = p1.y - p0.y, len = sqrt(dx * dx + dy * dy);
        dx /= len; dy /= len;
        double nx = -dy * hw, ny = dx * hw;
        struct pt v[4] = { { p0.x + nx, p0.y + ny }, { p1.x + nx, p1.y + ny }, { p1.x - nx, p1.y - ny }, { p0.x - nx, p0.y - ny } };
        emit_poly(s, v, 4);
        if (i == 0) { fdx = dx; fdy = dy; }
        else emit_join(s, p0, pdx, pdy, dx, dy);
        pdx = dx; pdy = dy;
    }
    if (closed) emit_join(s, a[0], pdx, pdy, fdx, fdy);
    else {
        emit_cap(s, a[0], -fdx, -fdy);
        emit_cap(s, a[n - 1], pdx, pdy);
    }
}

// remove consecutive duplicates (and the repeated start of a closed path); returns the new count
static int dedupe(struct pt* a, int n, int closed) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (w && fabs(a[i].x - a[w - 1].x) < 1e-9 && fabs(a[i].y - a[w - 1].y) < 1e-9) continue;
        a[w++] = a[i];
    }
    if (closed && w > 1 && fabs(a[0].x - a[w - 1].x) < 1e-9 && fabs(a[0].y - a[w - 1].y) < 1e-9) w--;
    return w;
}

static void flush_piece(struct stroker* s, struct pt* piece, int np) {
    int m = dedupe(piece, np, 0);
    if (m >= 1) stroke_line(s, piece, m, 0);
}

static void stroke_dashed(struct stroker* s, struct pt* a, int n, int closed, const double* dash, int nd, double offset) {
    double total = 0;
    for (int i = 0; i < nd; i++) total += dash[i];
    if (total <= 0) { stroke_line(s, a, n, closed); return; }
    offset -= total * floor(offset / total);                           // 0 <= offset < total
    int idx = 0, guard = 0;
    while (offset >= dash[idx] && guard++ < 4 * nd) { offset -= dash[idx]; idx = (idx + 1) % nd; }
    int on = (idx % 2) == 0;
    double rem = dash[idx] - offset;
    struct pt* piece = (struct pt*)w_malloc(sizeof(struct pt) * (size_t)(n + 3));
    if (!piece) return;
    int np = 0, nseg = closed ? n : n - 1, steps = 0;
    if (on) piece[np++] = a[0];
    for (int i = 0; i < nseg; i++) {
        struct pt p0 = a[i], p1 = a[(i + 1) % n];
        double dx = p1.x - p0.x, dy = p1.y - p0.y, len = sqrt(dx * dx + dy * dy), pos = 0;
        while (len - pos > rem && steps++ < 200000) {
            pos += rem;
            struct pt q = { p0.x + dx * pos / len, p0.y + dy * pos / len };
            if (on) { piece[np++] = q; flush_piece(s, piece, np); np = 0; }
            on = !on;
            idx = (idx + 1) % nd;
            rem = dash[idx];
            np = 0;
            if (on) piece[np++] = q;
        }
        rem -= len - pos;
        if (on) piece[np++] = p1;
    }
    if (on && np > 0) flush_piece(s, piece, np);
    w_free(piece);
}

static void build_stroke(struct wcv* c, const double* path, int n, const struct wcv_stroke* st) {
    struct poly po;
    struct stroker s;
    s.r = &c->ras;
    s.hw = st->width * 0.5;
    s.cap = st->cap;
    s.join = st->join;
    s.miter = st->miter_limit > 1 ? st->miter_limit : 1;
    wr_reset(&c->ras);
    if (flatten(path, n, &po) < 0) return;
    for (int i = 0; i < po.nsubs; i++) {
        struct sub* sb = &po.subs[i];
        if (sb->n < 1) continue;
        struct pt* a = po.pts + sb->start;
        int m = dedupe(a, sb->n, sb->closed);
        if (m < 1) continue;
        if (m == 1 && sb->n == 1 && !sb->closed && st->cap == 0) continue;
        if (st->ndash >= 2 && m >= 2) stroke_dashed(&s, a, m, sb->closed && m > 2, st->dash, st->ndash, st->dash_offset);
        else stroke_line(&s, a, m, sb->closed && m > 2);
    }
    poly_free(&po);
}

// ---------------------------------------------------------------------------------------------
// compositing

static inline unsigned chan_blend(int op, unsigned cb, unsigned cs) {
    switch (op) {
    case WCV_MULTIPLY: return mul255(cb, cs);
    case WCV_SCREEN: return cb + cs - mul255(cb, cs);
    case WCV_OVERLAY: return cb <= 127 ? mul255(cs, 2 * cb) : (cs + (2 * cb - 255) - mul255(cs, 2 * cb - 255));
    case WCV_DARKEN: return cb < cs ? cb : cs;
    case WCV_LIGHTEN: return cb > cs ? cb : cs;
    case WCV_COLOR_DODGE: if (cb == 0) return 0; if (cs >= 255) return 255; { unsigned v = cb * 255 / (255 - cs); return v > 255 ? 255 : v; }
    case WCV_COLOR_BURN: if (cb >= 255) return 255; if (cs == 0) return 0; { unsigned v = (255 - cb) * 255 / cs; return v > 255 ? 0 : 255 - v; }
    case WCV_HARD_LIGHT: return cs <= 127 ? mul255(cb, 2 * cs) : (cb + (2 * cs - 255) - mul255(cb, 2 * cs - 255));
    case WCV_SOFT_LIGHT: {
        double b = cb / 255.0, s = cs / 255.0, r;
        if (s <= 0.5) r = b - (1 - 2 * s) * b * (1 - b);
        else {
            double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : sqrt(b);
            r = b + (2 * s - 1) * (d - b);
        }
        return (unsigned)clampi((int)(r * 255 + 0.5), 0, 255);
    }
    case WCV_DIFFERENCE: return cb > cs ? cb - cs : cs - cb;
    case WCV_EXCLUSION: return cb + cs - 2 * mul255(cb, cs);
    }
    return cs;
}

static double lum3(double r, double g, double b) { return 0.3 * r + 0.59 * g + 0.11 * b; }
static void clip_color(double* r, double* g, double* b) {
    double l = lum3(*r, *g, *b), n = *r < *g ? (*r < *b ? *r : *b) : (*g < *b ? *g : *b), x = *r > *g ? (*r > *b ? *r : *b) : (*g > *b ? *g : *b);
    if (n < 0) { double k = l / (l - n); *r = l + (*r - l) * k; *g = l + (*g - l) * k; *b = l + (*b - l) * k; }
    if (x > 1) { double k = (1 - l) / (x - l); *r = l + (*r - l) * k; *g = l + (*g - l) * k; *b = l + (*b - l) * k; }
}
static void set_lum(double* r, double* g, double* b, double l) {
    double d = l - lum3(*r, *g, *b);
    *r += d; *g += d; *b += d;
    clip_color(r, g, b);
}
static double sat3(double r, double g, double b) {
    double x = r > g ? (r > b ? r : b) : (g > b ? g : b), n = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return x - n;
}
static void set_sat(double* r, double* g, double* b, double s) {
    double* v[3] = { r, g, b };
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++) if (*v[j] > *v[j + 1]) { double* t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; }
    double mn = *v[0], mx = *v[2];
    if (mx > mn) { *v[1] = (*v[1] - mn) * s / (mx - mn); *v[2] = s; }
    else { *v[1] = 0; *v[2] = 0; }
    *v[0] = 0;
}

static void blend_nonsep(int op, unsigned dr, unsigned dg, unsigned db, unsigned* sr, unsigned* sg, unsigned* sb) {
    double bR = dr / 255.0, bG = dg / 255.0, bB = db / 255.0, sR = *sr / 255.0, sG = *sg / 255.0, sB = *sb / 255.0, r, g, b;
    switch (op) {
    case WCV_HUE: r = sR; g = sG; b = sB; set_sat(&r, &g, &b, sat3(bR, bG, bB)); set_lum(&r, &g, &b, lum3(bR, bG, bB)); break;
    case WCV_SATURATION: r = bR; g = bG; b = bB; set_sat(&r, &g, &b, sat3(sR, sG, sB)); set_lum(&r, &g, &b, lum3(bR, bG, bB)); break;
    case WCV_COLOR: r = sR; g = sG; b = sB; set_lum(&r, &g, &b, lum3(bR, bG, bB)); break;
    default: r = bR; g = bG; b = bB; set_lum(&r, &g, &b, lum3(sR, sG, sB)); break;     // luminosity
    }
    *sr = (unsigned)clampi((int)(r * 255 + 0.5), 0, 255);
    *sg = (unsigned)clampi((int)(g * 255 + 0.5), 0, 255);
    *sb = (unsigned)clampi((int)(b * 255 + 0.5), 0, 255);
}

// d: destination (straight ARGB), s: source colour (rgb used, alpha ignored), sa: source alpha 0..255
static uint32_t comp_px(uint32_t d, uint32_t s, unsigned sa, int op) {
    unsigned da = d >> 24;
    unsigned sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
    unsigned dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    if (op == WCV_SOURCE_OVER) {
        if (sa == 255) return 0xFF000000u | (s & 0xFFFFFF);
        if (sa == 0) return d;
    }
    if (op >= WCV_MULTIPLY && op <= WCV_LUMINOSITY) {
        if (da && sa) {
            unsigned br, bg, bb;
            if (op >= WCV_HUE) { br = sr; bg = sg; bb = sb; blend_nonsep(op, dr, dg, db, &br, &bg, &bb); }
            else { br = chan_blend(op, dr, sr); bg = chan_blend(op, dg, sg); bb = chan_blend(op, db, sb); }
            // Cs' = (1 - ab) Cs + ab B(Cb, Cs)
            sr = mul255(255 - da, sr) + mul255(da, br);
            sg = mul255(255 - da, sg) + mul255(da, bg);
            sb = mul255(255 - da, sb) + mul255(da, bb);
        }
        op = WCV_SOURCE_OVER;
    }
    unsigned Fa, Fb;
    switch (op) {
    case WCV_SOURCE_OVER: Fa = 255; Fb = 255 - sa; break;
    case WCV_SOURCE_IN: Fa = da; Fb = 0; break;
    case WCV_SOURCE_OUT: Fa = 255 - da; Fb = 0; break;
    case WCV_SOURCE_ATOP: Fa = da; Fb = 255 - sa; break;
    case WCV_DEST_OVER: Fa = 255 - da; Fb = 255; break;
    case WCV_DEST_IN: Fa = 0; Fb = sa; break;
    case WCV_DEST_OUT: Fa = 0; Fb = 255 - sa; break;
    case WCV_DEST_ATOP: Fa = 255 - da; Fb = sa; break;
    case WCV_LIGHTER: Fa = 255; Fb = 255; break;
    case WCV_COPY: Fa = 255; Fb = 0; break;
    case WCV_XOR: Fa = 255 - da; Fb = 255 - sa; break;
    case WCV_CLEAR: Fa = 0; Fb = 255 - sa; break;                      // sa = coverage of the cleared area
    default: Fa = 255; Fb = 255 - sa; break;
    }
    unsigned oa = mul255(sa, Fa) + mul255(da, Fb);
    unsigned pr = mul255(mul255(sr, sa), Fa) + mul255(mul255(dr, da), Fb);
    unsigned pg = mul255(mul255(sg, sa), Fa) + mul255(mul255(dg, da), Fb);
    unsigned pb = mul255(mul255(sb, sa), Fa) + mul255(mul255(db, da), Fb);
    if (oa > 255) oa = 255;
    if (!oa) return 0;
    if (pr > oa) pr = oa;
    if (pg > oa) pg = oa;
    if (pb > oa) pb = oa;
    unsigned rr = (pr * 255 + oa / 2) / oa, gg = (pg * 255 + oa / 2) / oa, bb2 = (pb * 255 + oa / 2) / oa;
    return oa << 24 | (rr > 255 ? 255 : rr) << 16 | (gg > 255 ? 255 : gg) << 8 | (bb2 > 255 ? 255 : bb2);
}

// operators whose result differs from the destination even where the source is transparent
static int op_affects_outside(int op) {
    return op == WCV_SOURCE_IN || op == WCV_SOURCE_OUT || op == WCV_DEST_IN || op == WCV_DEST_ATOP || op == WCV_COPY;
}

// ---------------------------------------------------------------------------------------------
// paint sources

struct shade {
    struct wcv* c;
    const struct wcv_paint* p;
    const struct wcv_mask* mask;
    int op;
    unsigned galpha;
    uint32_t lut[1024];
    int have_lut;
    // linear
    double lx, ly, lk;
};

static void build_lut(struct shade* s) {
    const struct wcv_paint* p = s->p;
    s->have_lut = 1;
    if (p->nstops <= 0) { for (int i = 0; i < 1024; i++) s->lut[i] = 0; return; }
    int k = 0;
    for (int i = 0; i < 1024; i++) {
        double t = i / 1023.0;
        while (k < p->nstops && p->stops[k].t <= t) k++;
        if (k == 0) { s->lut[i] = p->stops[0].argb; continue; }
        if (k == p->nstops) { s->lut[i] = p->stops[p->nstops - 1].argb; continue; }
        const struct wcv_stop* a = &p->stops[k - 1];
        const struct wcv_stop* b = &p->stops[k];
        double f = b->t > a->t ? (t - a->t) / (b->t - a->t) : 1.0;
        unsigned v = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            double ca = (a->argb >> sh) & 255, cb = (b->argb >> sh) & 255;
            v |= (unsigned)clampi((int)(ca + (cb - ca) * f + 0.5), 0, 255) << sh;
        }
        s->lut[i] = v;
    }
}

static inline uint32_t lut_at(const struct shade* s, double t) {
    int i = (int)(clampd(t, 0.0, 1.0) * 1023.0 + 0.5);
    return s->lut[i];
}

static uint32_t texel(const struct wcv_paint* p, int u, int v) {
    // returns the pixel at image coordinates (u, v) of the source rect with the extension rules, 0 = transparent
    if (u < 0 || u >= p->sw) {
        if (p->repeat_x) { u %= p->sw; if (u < 0) u += p->sw; }
        else if (p->clamp) u = u < 0 ? 0 : p->sw - 1;
        else return 0;
    }
    if (v < 0 || v >= p->sh) {
        if (p->repeat_y) { v %= p->sh; if (v < 0) v += p->sh; }
        else if (p->clamp) v = v < 0 ? 0 : p->sh - 1;
        else return 0;
    }
    return p->img[(size_t)(p->sy + v) * p->stride + p->sx + u];
}

static uint32_t sample_image(const struct wcv_paint* p, double u, double v) {
    if (p->sw <= 0 || p->sh <= 0) return 0;
    if (!p->smooth) return texel(p, (int)floor(u), (int)floor(v));
    u -= 0.5; v -= 0.5;
    double fu = floor(u), fv = floor(v);
    int iu = (int)fu, iv = (int)fv;
    unsigned wx = (unsigned)((u - fu) * 256 + 0.5), wy = (unsigned)((v - fv) * 256 + 0.5);
    uint32_t t00 = texel(p, iu, iv), t10 = texel(p, iu + 1, iv), t01 = texel(p, iu, iv + 1), t11 = texel(p, iu + 1, iv + 1);
    unsigned w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;   // sum 65536
    uint32_t ts[4] = { t00, t10, t01, t11 };
    unsigned ws[4] = { w00, w10, w01, w11 };
    uint64_t A = 0, R = 0, G = 0, B = 0;
    for (int i = 0; i < 4; i++) {
        unsigned a = ts[i] >> 24;
        uint64_t wa = (uint64_t)ws[i] * a;
        A += (uint64_t)ws[i] * a;
        R += wa * ((ts[i] >> 16) & 255);
        G += wa * ((ts[i] >> 8) & 255);
        B += wa * (ts[i] & 255);
    }
    if (!A) return 0;
    unsigned a8 = (unsigned)((A + 32768) >> 16);
    if (a8 > 255) a8 = 255;
    unsigned r = (unsigned)((R + A / 2) / A), g = (unsigned)((G + A / 2) / A), b = (unsigned)((B + A / 2) / A);
    return a8 << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static uint32_t paint_color(struct shade* s, int x, int y) {
    const struct wcv_paint* p = s->p;
    double dx = x + 0.5, dy = y + 0.5;
    double ux = p->inv[0] * dx + p->inv[2] * dy + p->inv[4];
    double uy = p->inv[1] * dx + p->inv[3] * dy + p->inv[5];
    switch (p->kind) {
    case WCV_LINEAR: {
        double t = ((ux - p->g[0]) * s->lx + (uy - p->g[1]) * s->ly) * s->lk;
        return lut_at(s, t);
    }
    case WCV_RADIAL: {
        double cdx = p->g[3] - p->g[0], cdy = p->g[4] - p->g[1], dr = p->g[5] - p->g[2];
        double pdx = ux - p->g[0], pdy = uy - p->g[1];
        double a = cdx * cdx + cdy * cdy - dr * dr;
        double b = pdx * cdx + pdy * cdy + p->g[2] * dr;
        double cc = pdx * pdx + pdy * pdy - p->g[2] * p->g[2];
        double t;
        if (fabs(a) < 1e-12) {
            if (fabs(b) < 1e-12) return 0;
            t = cc / (2 * b);
            if (p->g[2] + t * dr < 0) return 0;
        } else {
            double disc = b * b - a * cc;
            if (disc < 0) return 0;
            double sq = sqrt(disc), t1 = (b + sq) / a, t2 = (b - sq) / a;
            if (t1 < t2) { double tmp = t1; t1 = t2; t2 = tmp; }
            if (p->g[2] + t1 * dr >= 0) t = t1;
            else if (p->g[2] + t2 * dr >= 0) t = t2;
            else return 0;
        }
        return lut_at(s, t);
    }
    case WCV_CONIC: {
        double ang = atan2(uy - p->g[2], ux - p->g[1]) - p->g[0];
        double t = ang / (2 * PI);
        t -= floor(t);
        return lut_at(s, t);
    }
    case WCV_IMAGE: return sample_image(p, ux, uy);
    }
    return p->color;
}

static inline unsigned mask_at(const struct wcv_mask* m, int x, int y) {
    double dx = x + 0.5, dy = y + 0.5;
    double u = m->inv[0] * dx + m->inv[2] * dy + m->inv[4];
    double v = m->inv[1] * dx + m->inv[3] * dy + m->inv[5];
    if (!m->smooth) {
        int iu = (int)floor(u), iv = (int)floor(v);
        if (iu < 0 || iv < 0 || iu >= m->w || iv >= m->h) return 0;
        return m->m[(size_t)iv * m->w + iu];
    }
    u -= 0.5; v -= 0.5;
    double fu = floor(u), fv = floor(v);
    int iu = (int)fu, iv = (int)fv;
    unsigned wx = (unsigned)((u - fu) * 256 + 0.5), wy = (unsigned)((v - fv) * 256 + 0.5);
    unsigned t[4];
    for (int k = 0; k < 4; k++) {
        int uu = iu + (k & 1), vv = iv + (k >> 1);
        t[k] = (uu < 0 || vv < 0 || uu >= m->w || vv >= m->h) ? 0 : m->m[(size_t)vv * m->w + uu];
    }
    unsigned top = t[0] * (256 - wx) + t[1] * wx, bot = t[2] * (256 - wx) + t[3] * wx;
    return (top * (256 - wy) + bot * wy + 32768) >> 16;
}

// ---------------------------------------------------------------------------------------------
// shading a rasterised path

static void shade_init(struct shade* s, struct wcv* c, const struct wcv_paint* p, const struct wcv_comp* comp, const struct wcv_mask* mask) {
    s->c = c;
    s->p = p;
    s->mask = mask;
    s->op = comp->op;
    s->galpha = (unsigned)clampi((int)(comp->alpha * 255.0 + 0.5), 0, 255);
    s->have_lut = 0;
    if (p->kind == WCV_LINEAR) {
        s->lx = p->g[2] - p->g[0]; s->ly = p->g[3] - p->g[1];
        double l2 = s->lx * s->lx + s->ly * s->ly;
        s->lk = l2 > 0 ? 1.0 / l2 : 0;
    }
    if (p->kind >= WCV_LINEAR && p->kind <= WCV_CONIC) build_lut(s);
}

struct spanctx { struct shade* sh; };

static void span_direct(void* ctx, int y, int x, int len, const uint8_t* cov) {
    struct shade* s = ((struct spanctx*)ctx)->sh;
    struct wcv* c = s->c;
    uint32_t* row = c->px + (size_t)y * c->w;
    const uint8_t* clipm = c->clip ? c->clip->m + (size_t)y * c->w : 0;
    int solid = s->p->kind == WCV_SOLID && !s->mask;
    uint32_t sc = s->p->color;
    unsigned base_a = s->p->kind == WCV_SOLID ? mul255(sc >> 24, s->galpha) : s->galpha;
    for (int i = 0; i < len; i++) {
        unsigned cv = cov[i];
        if (!cv) continue;
        int px = x + i;
        if (clipm) { cv = mul255(cv, clipm[px]); if (!cv) continue; }
        if (solid) {
            unsigned sa = mul255(cv, base_a);
            if (sa || s->op != WCV_SOURCE_OVER) row[px] = comp_px(row[px], sc, sa, s->op);
            continue;
        }
        if (s->mask) { cv = mul255(cv, mask_at(s->mask, px, y)); if (!cv) continue; }
        uint32_t col = paint_color(s, px, y);
        unsigned sa = mul255(mul255(col >> 24, cv), s->galpha);
        if (sa || s->op != WCV_SOURCE_OVER) row[px] = comp_px(row[px], col, sa, s->op);
    }
}

struct covctx { uint8_t* buf; int w, x0, y0, bw, bh; const struct wcv_mask* mask; };

static void span_cov(void* ctx, int y, int x, int len, const uint8_t* cov) {
    struct covctx* k = (struct covctx*)ctx;
    int ry = y - k->y0;
    if (ry < 0 || ry >= k->bh) return;
    for (int i = 0; i < len; i++) {
        int rx = x + i - k->x0;
        if (rx < 0 || rx >= k->bw) continue;
        unsigned cv = cov[i];
        if (k->mask && cv) cv = mul255(cv, mask_at(k->mask, x + i, y));
        uint8_t* d = &k->buf[(size_t)ry * k->bw + rx];
        unsigned t = *d + cv;                      // overlapping subpaths of a stroke accumulate (clamped)
        *d = (uint8_t)(t > 255 ? 255 : t);
    }
}

static void box_blur_h(uint8_t* b, int w, int h, int r, uint8_t* tmp) {
    int n = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        uint8_t* row = b + (size_t)y * w;
        unsigned sum = 0;
        for (int i = -r; i <= r; i++) sum += (i >= 0 && i < w) ? row[i] : 0;
        for (int x = 0; x < w; x++) {
            tmp[x] = (uint8_t)((sum + n / 2) / n);
            int add = x + r + 1, sub = x - r;
            sum += (add < w ? row[add] : 0);
            sum -= (sub >= 0 ? row[sub] : 0);
        }
        memcpy(row, tmp, (size_t)w);
    }
}

static void box_blur_v(uint8_t* b, int w, int h, int r, uint8_t* tmp) {
    int n = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        unsigned sum = 0;
        for (int i = -r; i <= r; i++) sum += (i >= 0 && i < h) ? b[(size_t)i * w + x] : 0;
        for (int y = 0; y < h; y++) {
            tmp[y] = (uint8_t)((sum + n / 2) / n);
            int add = y + r + 1, sub = y - r;
            sum += (add < h ? b[(size_t)add * w + x] : 0);
            sum -= (sub >= 0 ? b[(size_t)sub * w + x] : 0);
        }
        for (int y = 0; y < h; y++) b[(size_t)y * w + x] = tmp[y];
    }
}

static void gaussian_blur(uint8_t* b, int w, int h, double sigma) {
    if (sigma < 0.3) return;
    // three box blurs approximate a gaussian
    int bw = (int)floor(sqrt(12.0 * sigma * sigma / 3 + 1));
    int r = (bw - 1) / 2;
    if (r < 1) r = 1;
    int mx = w > h ? w : h;
    uint8_t* tmp = (uint8_t*)w_malloc((size_t)mx + 1);
    if (!tmp) return;
    for (int pass = 0; pass < 3; pass++) { box_blur_h(b, w, h, r, tmp); box_blur_v(b, w, h, r, tmp); }
    w_free(tmp);
}

static void draw_shadow(struct wcv* c, int evenodd, const struct wcv_paint* p, const struct wcv_comp* comp, const struct wcv_mask* mask) {
    const struct wcv_shadow* sh = &comp->shadow;
    if ((sh->color >> 24) == 0 || (sh->dx == 0 && sh->dy == 0 && sh->blur <= 0)) return;
    int bx0, by0, bx1, by1;
    if (!wr_bounds(&c->ras, &bx0, &by0, &bx1, &by1)) return;
    double sigma = sh->blur * 0.5;
    int margin = (int)ceil(sigma * 3) + 1;
    int dxi = (int)floor(sh->dx + 0.5), dyi = (int)floor(sh->dy + 0.5);
    // region of the shape (with the blur margin) whose shadow lands inside the canvas
    int rx0 = bx0 - margin, ry0 = by0 - margin, rx1 = bx1 + margin, ry1 = by1 + margin;
    if (rx0 < -dxi - margin) rx0 = -dxi - margin;
    if (ry0 < -dyi - margin) ry0 = -dyi - margin;
    if (rx1 > c->w - dxi + margin) rx1 = c->w - dxi + margin;
    if (ry1 > c->h - dyi + margin) ry1 = c->h - dyi + margin;
    int bw = rx1 - rx0, bh = ry1 - ry0;
    if (bw <= 0 || bh <= 0 || (int64_t)bw * bh > 16 * 1024 * 1024) return;
    uint8_t* buf = (uint8_t*)w_calloc((size_t)bw * bh, 1);
    if (!buf) return;
    struct covctx k = { buf, c->w, rx0, ry0, bw, bh, mask };
    wr_fill(&c->ras, rx0, ry0, rx1, ry1, evenodd, span_cov, &k);
    gaussian_blur(buf, bw, bh, sigma);
    unsigned sa_base = sh->color >> 24;
    unsigned galpha = (unsigned)clampi((int)(comp->alpha * 255.0 + 0.5), 0, 255);
    if (p->kind == WCV_SOLID) galpha = mul255(galpha, p->color >> 24);   // a translucent shape casts a lighter shadow
    sa_base = mul255(sa_base, galpha);
    for (int y = 0; y < bh; y++) {
        int cy = y + ry0 + dyi;
        if (cy < 0 || cy >= c->h) continue;
        uint32_t* row = c->px + (size_t)cy * c->w;
        const uint8_t* clipm = c->clip ? c->clip->m + (size_t)cy * c->w : 0;
        for (int x = 0; x < bw; x++) {
            unsigned cv = buf[(size_t)y * bw + x];
            if (!cv) continue;
            int cx = x + rx0 + dxi;
            if (cx < 0 || cx >= c->w) continue;
            if (clipm) { cv = mul255(cv, clipm[cx]); if (!cv) continue; }
            unsigned sa = mul255(cv, sa_base);
            if (sa) row[cx] = comp_px(row[cx], sh->color, sa, comp->op);
        }
    }
    w_free(buf);
}

// Composite the path currently in c->ras
static void shade_raster(struct wcv* c, int evenodd, const struct wcv_paint* p, const struct wcv_comp* comp, const struct wcv_mask* mask) {
    if (c->w <= 0 || c->h <= 0) return;
    int opx = comp->op;
    int outside = op_affects_outside(opx);
    int bx0, by0, bx1, by1;
    int have = wr_bounds(&c->ras, &bx0, &by0, &bx1, &by1);
    if (!have && !outside) return;
    if (have) {
        draw_shadow(c, evenodd, p, comp, mask);
    }
    struct shade* s = (struct shade*)w_malloc(sizeof *s);
    if (!s) return;
    shade_init(s, c, p, comp, mask);
    c->serial++;
    if (!outside) {
        struct spanctx sc = { s };
        wr_fill(&c->ras, 0, 0, c->w, c->h, evenodd, span_direct, &sc);
        w_free(s);
        return;
    }
    // operators that also change what is outside the shape: coverage of the whole canvas
    uint8_t* cov = (uint8_t*)w_calloc((size_t)c->w * c->h, 1);
    if (!cov) { w_free(s); return; }
    struct covctx k = { cov, c->w, 0, 0, c->w, c->h, mask };
    if (have) wr_fill(&c->ras, 0, 0, c->w, c->h, evenodd, span_cov, &k);
    int solid = p->kind == WCV_SOLID;
    for (int y = 0; y < c->h; y++) {
        uint32_t* row = c->px + (size_t)y * c->w;
        const uint8_t* clipm = c->clip ? c->clip->m + (size_t)y * c->w : 0;
        for (int x = 0; x < c->w; x++) {
            unsigned cv = cov[(size_t)y * c->w + x];
            uint32_t col = 0;
            unsigned sa = 0;
            if (cv) {
                col = solid ? p->color : paint_color(s, x, y);
                sa = mul255(mul255(col >> 24, cv), s->galpha);
            }
            uint32_t res = comp_px(row[x], col, sa, opx);
            if (clipm && clipm[x] != 255) {
                // partially clipped: blend old and new by the clip coverage
                unsigned cm = clipm[x];
                uint32_t o = row[x];
                unsigned a = (o >> 24) * (255 - cm) + (res >> 24) * cm;
                a = (a + 127) / 255;
                unsigned rr = ((o >> 16 & 255) * (255 - cm) + (res >> 16 & 255) * cm + 127) / 255;
                unsigned gg = ((o >> 8 & 255) * (255 - cm) + (res >> 8 & 255) * cm + 127) / 255;
                unsigned bb = ((o & 255) * (255 - cm) + (res & 255) * cm + 127) / 255;
                res = a << 24 | rr << 16 | gg << 8 | bb;
            }
            row[x] = res;
        }
    }
    w_free(cov);
    w_free(s);
}

void wcv_fill(struct wcv* c, const double* path, int n, int evenodd, const struct wcv_paint* p,
              const struct wcv_comp* comp, const struct wcv_mask* mask) {
    path_to_raster(&c->ras, path, n);
    shade_raster(c, evenodd, p, comp, mask);
}

void wcv_stroke(struct wcv* c, const double* path, int n, const struct wcv_stroke* st, const struct wcv_paint* p,
                const struct wcv_comp* comp) {
    if (st->width <= 0) return;
    build_stroke(c, path, n, st);
    shade_raster(c, 0, p, comp, 0);
}

static void span_clipmask(void* ctx, int y, int x, int len, const uint8_t* cov) {
    struct covctx* k = (struct covctx*)ctx;
    memcpy(k->buf + (size_t)y * k->w + x, cov, (size_t)len);
}

void wcv_clip(struct wcv* c, const double* path, int n, int evenodd) {
    struct clipmask* m = (struct clipmask*)w_malloc(sizeof *m);
    if (!m) return;
    m->m = (uint8_t*)w_calloc((size_t)(c->w ? c->w : 1) * (c->h ? c->h : 1), 1);
    m->ref = 1;
    if (!m->m) { w_free(m); return; }
    if (path_to_raster(&c->ras, path, n) && c->w > 0 && c->h > 0) {
        struct covctx k = { m->m, c->w, 0, 0, c->w, c->h, 0 };
        wr_fill(&c->ras, 0, 0, c->w, c->h, evenodd, span_clipmask, &k);
    }
    if (c->clip) {
        size_t total = (size_t)c->w * c->h;
        for (size_t i = 0; i < total; i++) m->m[i] = (uint8_t)mul255(m->m[i], c->clip->m[i]);
    }
    clip_unref(c->clip);
    c->clip = m;
}

// ---------------------------------------------------------------------------------------------
// pixel access

void wcv_get_image(const struct wcv* c, int x, int y, int w, int h, uint8_t* rgba) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            uint8_t* o = rgba + ((size_t)j * w + i) * 4;
            if (sx < 0 || sy < 0 || sx >= c->w || sy >= c->h) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            uint32_t p = c->px[(size_t)sy * c->w + sx];
            o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; o[3] = (uint8_t)(p >> 24);
        }
    }
}

// Copy the rect (dx, dy, dw, dh) of an RGBA image of width w to the canvas at (x + dx, y + dy).
void wcv_put_image(struct wcv* c, int x, int y, int w, int h, const uint8_t* rgba, int dx, int dy, int dw, int dh) {
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > w) dw = w - dx;
    if (dy + dh > h) dh = h - dy;
    for (int j = dy; j < dy + dh; j++) {
        int cy = y + j;
        if (cy < 0 || cy >= c->h) continue;
        for (int i = dx; i < dx + dw; i++) {
            int cx = x + i;
            if (cx < 0 || cx >= c->w) continue;
            const uint8_t* s = rgba + ((size_t)j * w + i) * 4;
            c->px[(size_t)cy * c->w + cx] = (uint32_t)s[3] << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
        }
    }
    c->serial++;
}

// ---------------------------------------------------------------------------------------------
// hit testing

int wcv_point_in_path(const double* path, int n, double x, double y, int evenodd) {
    struct poly po;
    if (flatten(path, n, &po) < 0) return 0;
    int wind = 0;
    for (int i = 0; i < po.nsubs; i++) {
        const struct pt* a = po.pts + po.subs[i].start;
        int m = po.subs[i].n;
        for (int k = 0; k < m; k++) {
            const struct pt* p0 = &a[k];
            const struct pt* p1 = &a[(k + 1) % m];           // every subpath is implicitly closed for filling
            if ((p0->y <= y) != (p1->y <= y)) {
                double t = (y - p0->y) / (p1->y - p0->y);
                if (x < p0->x + t * (p1->x - p0->x)) wind += p1->y > p0->y ? 1 : -1;
            }
        }
    }
    poly_free(&po);
    return evenodd ? (wind & 1) : wind != 0;
}

int wcv_point_in_stroke(const double* path, int n, double x, double y, const struct wcv_stroke* st) {
    struct poly po;
    if (flatten(path, n, &po) < 0) return 0;
    double hw = st->width * 0.5;
    int hit = 0;
    for (int i = 0; i < po.nsubs && !hit; i++) {
        const struct pt* a = po.pts + po.subs[i].start;
        int m = po.subs[i].n;
        int nseg = po.subs[i].closed ? m : m - 1;
        if (m == 1) { double dx = x - a[0].x, dy = y - a[0].y; hit = st->cap == 1 && dx * dx + dy * dy <= hw * hw; }
        for (int k = 0; k < nseg && !hit; k++) {
            const struct pt* p0 = &a[k];
            const struct pt* p1 = &a[(k + 1) % m];
            double dx = p1->x - p0->x, dy = p1->y - p0->y, l2 = dx * dx + dy * dy;
            double t = l2 > 0 ? ((x - p0->x) * dx + (y - p0->y) * dy) / l2 : 0;
            t = clampd(t, 0, 1);
            double qx = p0->x + t * dx - x, qy = p0->y + t * dy - y;
            if (qx * qx + qy * qy <= hw * hw) hit = 1;
        }
    }
    poly_free(&po);
    return hit;
}

// ---------------------------------------------------------------------------------------------
// PNG (stored deflate blocks: no compression, always valid)

struct obuf { uint8_t* p; uint32_t n, cap; int fail; };

static void ob_put(struct obuf* o, const void* d, uint32_t len) {
    if (o->fail) return;
    if (o->n + len > o->cap) {
        uint32_t nc = o->cap ? o->cap : 4096;
        while (nc < o->n + len) nc *= 2;
        uint8_t* np = (uint8_t*)w_realloc(o->p, nc);
        if (!np) { o->fail = 1; return; }
        o->p = np; o->cap = nc;
    }
    memcpy(o->p + o->n, d, len);
    o->n += len;
}
static void ob_u32(struct obuf* o, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; ob_put(o, b, 4); }

static uint32_t crc_table[256];
static int crc_init;
static uint32_t crc32_update(uint32_t crc, const uint8_t* d, uint32_t n) {
    if (!crc_init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
        crc_init = 1;
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *d++) & 255] ^ (crc >> 8);
    return ~crc;
}

static void png_chunk(struct obuf* o, const char* type, const uint8_t* d, uint32_t len) {
    ob_u32(o, len);
    uint32_t start = o->n;
    ob_put(o, type, 4);
    if (len) ob_put(o, d, len);
    if (!o->fail) ob_u32(o, crc32_update(0, o->p + start, len + 4));
}

uint8_t* wcv_png(const struct wcv* c, uint32_t* len) {
    if (c->w <= 0 || c->h <= 0) return 0;
    struct obuf o = { 0, 0, 0, 0 };
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    ob_put(&o, sig, 8);
    uint8_t ihdr[13] = { (uint8_t)(c->w >> 24), (uint8_t)(c->w >> 16), (uint8_t)(c->w >> 8), (uint8_t)c->w,
                         (uint8_t)(c->h >> 24), (uint8_t)(c->h >> 16), (uint8_t)(c->h >> 8), (uint8_t)c->h, 8, 6, 0, 0, 0 };
    png_chunk(&o, "IHDR", ihdr, 13);
    uint32_t rowb = (uint32_t)c->w * 4 + 1, total = rowb * (uint32_t)c->h;
    uint8_t* raw = (uint8_t*)w_malloc(total);
    if (!raw) { w_free(o.p); return 0; }
    uint32_t adler_a = 1, adler_b = 0;
    for (int y = 0; y < c->h; y++) {
        uint8_t* r = raw + (size_t)y * rowb;
        r[0] = 0;
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[(size_t)y * c->w + x];
            r[1 + x * 4] = (uint8_t)(p >> 16); r[2 + x * 4] = (uint8_t)(p >> 8); r[3 + x * 4] = (uint8_t)p; r[4 + x * 4] = (uint8_t)(p >> 24);
        }
    }
    for (uint32_t i = 0; i < total; i++) { adler_a = (adler_a + raw[i]) % 65521; adler_b = (adler_b + adler_a) % 65521; }
    // IDAT: zlib header, stored blocks, adler32
    uint32_t nblocks = (total + 65534) / 65535;
    if (!nblocks) nblocks = 1;
    uint32_t idat_len = 2 + total + nblocks * 5 + 4;
    uint8_t* idat = (uint8_t*)w_malloc(idat_len);
    if (!idat) { w_free(raw); w_free(o.p); return 0; }
    uint32_t q = 0;
    idat[q++] = 0x78; idat[q++] = 0x01;
    for (uint32_t pos = 0; pos < total || (pos == 0 && total == 0); ) {
        uint32_t n = total - pos > 65535 ? 65535 : total - pos;
        idat[q++] = pos + n >= total ? 1 : 0;
        idat[q++] = (uint8_t)n; idat[q++] = (uint8_t)(n >> 8);
        idat[q++] = (uint8_t)~n; idat[q++] = (uint8_t)(~n >> 8);
        memcpy(idat + q, raw + pos, n);
        q += n; pos += n;
        if (!n) break;
    }
    uint32_t ad = adler_b << 16 | adler_a;
    idat[q++] = (uint8_t)(ad >> 24); idat[q++] = (uint8_t)(ad >> 16); idat[q++] = (uint8_t)(ad >> 8); idat[q++] = (uint8_t)ad;
    png_chunk(&o, "IDAT", idat, q);
    png_chunk(&o, "IEND", 0, 0);
    w_free(idat);
    w_free(raw);
    if (o.fail) { w_free(o.p); return 0; }
    *len = o.n;
    return o.p;
}

// ---------------------------------------------------------------------------------------------
// text

static void fill_wfont(const struct wcv_font* f, struct wfont* w) {
    w->family = (uint8_t)(f->family < 0 || f->family > 2 ? 0 : f->family);
    w->bold = (uint8_t)(f->bold != 0);
    w->italic = (uint8_t)(f->italic != 0);
    int px = (int)(f->px + 0.5);
    if (px < 1) px = 1;
    if (px > 512) px = 512;
    w->px = (uint16_t)px;
}

double wcv_text_width(const char* str, int len, const struct wcv_font* f) {
    struct wfont w;
    fill_wfont(f, &w);
    wfont_init();
    return wfont_measure(&w, str, len) / (double)LU;
}

void wcv_font_metrics(const struct wcv_font* f, double* ascent, double* descent) {
    struct wfont w;
    struct wfmetrics m;
    fill_wfont(f, &w);
    wfont_init();
    wfont_metrics(&w, &m);
    *ascent = m.ascent / (double)LU;
    *descent = m.descent / (double)LU;
}

uint8_t* wcv_text_mask(const char* str, int len, const struct wcv_font* f, double fx, double fy, int* w, int* h, int* ox, int* oy) {
    struct wfont wf;
    struct wfmetrics m;
    fill_wfont(f, &wf);
    wfont_init();
    wfont_metrics(&wf, &m);
    int32_t width = wfont_measure(&wf, str, len);
    int mw = (int)(width / LU) + 1 + 6, asc = (int)((m.ascent + LU - 1) / LU), desc = (int)((m.descent + LU - 1) / LU);
    int mh = asc + desc + 6;
    if (mw <= 6 || mw > 8192 || mh > 2048 || (int64_t)mw * mh > 16 * 1024 * 1024) return 0;
    uint32_t* px = (uint32_t*)w_calloc((size_t)mw * mh, 4);
    uint8_t* out = (uint8_t*)w_malloc((size_t)mw * mh);
    if (!px || !out) { w_free(px); w_free(out); return 0; }
    struct wsurf s;
    s.px = px; s.w = mw; s.h = mh; s.stride = mw;
    ws_reset_clip(&s);
    wfont_draw(&s, &wf, PX(3) + (int32_t)(fx * LU + 0.5), PX(3 + asc) + (int32_t)(fy * LU + 0.5), 0xFFFFFFFFu, str, len, 0, 0);
    for (int i = 0; i < mw * mh; i++) out[i] = (uint8_t)(px[i] & 0xFF);
    w_free(px);
    *w = mw; *h = mh; *ox = 3; *oy = 3 + asc;
    return out;
}
