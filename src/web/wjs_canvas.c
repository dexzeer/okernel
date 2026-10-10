// wjs_canvas.c - <canvas> 2D natives for page scripts: the CanvasRenderingContext2D class in the
// prelude keeps the drawing state (transform, styles, path) and calls these to rasterise into the
// software canvases of wcanvas.c. A canvas bound to a <canvas> element is published into the
// element's image slot from wjs_canvas_poll() (called with the media poll in the main loop).

#include "wjs_int.h"
#include "wcanvas.h"
#include "css.h"

#if defined(KERNEL) && KERNEL
double sqrt(double);
double fabs(double);
double floor(double);
double ceil(double);
#else
#include <math.h>
#endif

#define CV_MAX 128

struct wcvs {
    struct wcv* cv;
    int node;                 // bound <canvas> element, -1
    unsigned published;       // serial last pushed to the document
    int published_w, published_h;
};

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)
#define UNUSED_THIS (void)this_val

static struct wcvs* find(struct wjs* js, ojs* ctx, int argc, ojsv* argv, int idx) {
    int32_t id = 0;
    if (argc <= idx || ojs_to_int32(ctx, &id, argv[idx]) < 0 || !js->canvas || id < 1 || id > CV_MAX) return 0;
    struct wcvs* s = &js->canvas[id - 1];
    return s->cv ? s : 0;
}

static double num(ojs* ctx, int argc, ojsv* argv, int i, double dflt) {
    double d = dflt;
    if (i < argc && ojs_to_number(ctx, &d, argv[i]) < 0) return dflt;
    if (d != d) return dflt;
    return d;
}

static const double* f64(ojs* ctx, ojsv v, size_t* n) {
    size_t bytes = 0;
    uint8_t* p = ojs_bytes(ctx, v, &bytes);
    if (!p) { *n = 0; return 0; }
    *n = bytes / sizeof(double);
    return (const double*)p;
}

// ---- matrices (a b c d e f) ----------------------------------------------------------------
static int mat_invert(const double* m, double* o) {
    double det = m[0] * m[3] - m[1] * m[2];
    if (det == 0 || det != det) return 0;
    double id = 1.0 / det;
    o[0] = m[3] * id; o[1] = -m[1] * id; o[2] = -m[2] * id; o[3] = m[0] * id;
    o[4] = (m[2] * m[5] - m[3] * m[4]) * id;
    o[5] = (m[1] * m[4] - m[0] * m[5]) * id;
    return 1;
}
static void mat_apply(const double* m, double x, double y, double* ox, double* oy) {
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}
// out = a * b (apply b first, then a)
static void mat_mul(const double* a, const double* b, double* o) {
    double r[6];
    r[0] = a[0] * b[0] + a[2] * b[1]; r[1] = a[1] * b[0] + a[3] * b[1];
    r[2] = a[0] * b[2] + a[2] * b[3]; r[3] = a[1] * b[2] + a[3] * b[3];
    r[4] = a[0] * b[4] + a[2] * b[5] + a[4]; r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    for (int i = 0; i < 6; i++) o[i] = r[i];
}

// ---- lifecycle -----------------------------------------------------------------------------

NATIVE(n_cv_new) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    int w = (int)num(ctx, argc, argv, 0, 300), h = (int)num(ctx, argc, argv, 1, 150);
    if (!js->canvas) js->canvas = (struct wcvs*)w_calloc(CV_MAX, sizeof(struct wcvs));
    if (!js->canvas) return ojs_int(0);
    for (int i = 0; i < CV_MAX; i++) {
        if (js->canvas[i].cv) continue;
        struct wcv* cv = wcv_new(w, h);
        if (!cv) return ojs_int(0);
        js->canvas[i].cv = cv;
        js->canvas[i].node = -1;
        js->canvas[i].published = 0;
        js->canvas[i].published_w = js->canvas[i].published_h = -1;
        return ojs_int(i + 1);
    }
    return ojs_int(0);
}

static void cv_release(struct wjs* js, struct wcvs* s) {
    if (s->cv && s->node >= 0 && js->doc) wdoc_canvas_set(js->doc, s->node, 0, 0, 0);
    wcv_free(s->cv);
    memset(s, 0, sizeof *s);
    s->node = -1;
}

NATIVE(n_cv_free) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (s) cv_release(js, s);
    return OJS_UNDEFINED;
}

NATIVE(n_cv_resize) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s) return ojs_bool(0);
    int w = (int)num(ctx, argc, argv, 1, 300), h = (int)num(ctx, argc, argv, 2, 150);
    return ojs_bool(wcv_resize(s->cv, w, h) == 0);
}

NATIVE(n_cv_bind) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s) return OJS_UNDEFINED;
    int32_t node = -1;
    if (argc > 1 && !ojs_is_null(argv[1]) && !ojs_is_undefined(argv[1])) node = wjs_node_of(js, argv[1]);
    if (s->node >= 0 && s->node != node && js->doc) wdoc_canvas_set(js->doc, s->node, 0, 0, 0);
    s->node = node;
    s->published = wcv_serial(s->cv) - 1;            // publish on the next poll
    s->published_w = s->published_h = -1;
    return OJS_UNDEFINED;
}

NATIVE(n_cv_size) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s) return ojs_int(0);
    return ojs_int(argc > 1 && num(ctx, argc, argv, 1, 0) ? wcv_height(s->cv) : wcv_width(s->cv));
}

// ---- paint / composite descriptors ---------------------------------------------------------

#define MAX_STOPS 64
struct paintbuf {
    struct wcv_paint p;
    struct wcv_stop stops[MAX_STOPS];
};

// pixels of an image-like source: 1 = <img> node, 2 = canvas id, 3 = media session (video frame)
static const uint32_t* source_pixels(struct wjs* js, int kind, int id, int* w, int* h, int* stride) {
    *stride = 0;
    if (kind == 1) {
        if (!js->doc) return 0;
        const uint32_t* px = wdoc_img_pixels(js->doc, id, w, h);
        *stride = *w;
        return px;
    }
    if (kind == 2) {
        if (!js->canvas || id < 1 || id > CV_MAX || !js->canvas[id - 1].cv) return 0;
        struct wcv* c = js->canvas[id - 1].cv;
        *w = wcv_width(c); *h = wcv_height(c); *stride = *w;
        return wcv_pixels(c);
    }
    if (kind == 3) {
        const uint32_t* px = wjs_media_frame(js, id, w, h);
        *stride = *w;
        return px;
    }
    return 0;
}

// a: [kind, color, g0..g5, inv0..inv5, nstops, (t, argb)...]   pattern (kind 4): [4, 0, srcKind, srcId, repeatX, repeatY, smooth, 0, inv0..inv5]
static int decode_paint(struct wjs* js, const double* a, size_t n, struct paintbuf* pb) {
    struct wcv_paint* p = &pb->p;
    memset(p, 0, sizeof *p);
    if (n < 14) { p->kind = WCV_SOLID; p->color = 0; p->inv[0] = p->inv[3] = 1; return 1; }
    int kind = (int)a[0];
    p->color = (uint32_t)a[1];
    for (int i = 0; i < 6; i++) { p->g[i] = a[2 + i]; p->inv[i] = a[8 + i]; }
    if (kind == 4) {
        int w = 0, h = 0, stride = 0;
        const uint32_t* px = source_pixels(js, (int)a[2], (int)a[3], &w, &h, &stride);
        if (!px || w <= 0 || h <= 0) { p->kind = WCV_SOLID; p->color = 0; return 1; }
        p->kind = WCV_IMAGE;
        p->img = px; p->stride = stride; p->sx = 0; p->sy = 0; p->sw = w; p->sh = h;
        p->repeat_x = a[4] != 0; p->repeat_y = a[5] != 0;
        p->smooth = a[6] != 0;
        return 1;
    }
    if (kind < 0 || kind > 3) kind = 0;
    p->kind = kind;
    if (kind != WCV_SOLID && n >= 15) {
        int ns = (int)a[14];
        if (ns > MAX_STOPS) ns = MAX_STOPS;
        if (ns < 0 || (size_t)(15 + 2 * ns) > n) ns = 0;
        for (int i = 0; i < ns; i++) { pb->stops[i].t = a[15 + 2 * i]; pb->stops[i].argb = (uint32_t)a[16 + 2 * i]; }
        p->stops = pb->stops;
        p->nstops = ns;
    }
    return 1;
}

static void decode_comp(const double* a, size_t n, struct wcv_comp* c) {
    memset(c, 0, sizeof *c);
    c->op = WCV_SOURCE_OVER;
    c->alpha = 1;
    if (n >= 2) { c->op = (int)a[0]; if (c->op < 0 || c->op >= WCV_NOPS) c->op = 0; c->alpha = a[1]; }
    if (n >= 6) { c->shadow.color = (uint32_t)a[2]; c->shadow.blur = a[3]; c->shadow.dx = a[4]; c->shadow.dy = a[5]; }
}

// ---- drawing -------------------------------------------------------------------------------

// cvFill(id, path, evenodd, paint, comp)
NATIVE(n_cv_fill) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 5) return OJS_UNDEFINED;
    size_t np, pn, cn;
    const double* path = f64(ctx, argv[1], &np);
    const double* pa = f64(ctx, argv[3], &pn);
    const double* ca = f64(ctx, argv[4], &cn);
    if (!path || !pa) return OJS_UNDEFINED;
    struct paintbuf* pb = (struct paintbuf*)w_malloc(sizeof *pb);
    if (!pb) return OJS_UNDEFINED;
    struct wcv_comp comp;
    decode_paint(js, pa, pn, pb);
    decode_comp(ca, cn, &comp);
    wcv_fill(s->cv, path, (int)np, (int)num(ctx, argc, argv, 2, 0) != 0, &pb->p, &comp, 0);
    w_free(pb);
    return OJS_UNDEFINED;
}

// cvStroke(id, path, stroke[width, cap, join, miter, dashOffset, dash...], paint, comp)
NATIVE(n_cv_stroke) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 5) return OJS_UNDEFINED;
    size_t np, sn, pn, cn;
    const double* path = f64(ctx, argv[1], &np);
    const double* sa = f64(ctx, argv[2], &sn);
    const double* pa = f64(ctx, argv[3], &pn);
    const double* ca = f64(ctx, argv[4], &cn);
    if (!path || !sa || sn < 5 || !pa) return OJS_UNDEFINED;
    struct wcv_stroke st;
    st.width = sa[0]; st.cap = (int)sa[1]; st.join = (int)sa[2]; st.miter_limit = sa[3]; st.dash_offset = sa[4];
    st.dash = sn > 5 ? sa + 5 : 0;
    st.ndash = (int)(sn - 5);
    if (st.ndash & 1) st.ndash = 0;
    struct paintbuf* pb = (struct paintbuf*)w_malloc(sizeof *pb);
    if (!pb) return OJS_UNDEFINED;
    struct wcv_comp comp;
    decode_paint(js, pa, pn, pb);
    decode_comp(ca, cn, &comp);
    wcv_stroke(s->cv, path, (int)np, &st, &pb->p, &comp);
    w_free(pb);
    return OJS_UNDEFINED;
}

NATIVE(n_cv_clip) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 2) return OJS_UNDEFINED;
    size_t np;
    const double* path = f64(ctx, argv[1], &np);
    if (path) wcv_clip(s->cv, path, (int)np, (int)num(ctx, argc, argv, 2, 0) != 0);
    return OJS_UNDEFINED;
}

NATIVE(n_cv_save) { struct wjs* js = JSX(ctx); UNUSED_THIS; struct wcvs* s = find(js, ctx, argc, argv, 0); if (s) wcv_save(s->cv); return OJS_UNDEFINED; }
NATIVE(n_cv_restore) { struct wjs* js = JSX(ctx); UNUSED_THIS; struct wcvs* s = find(js, ctx, argc, argv, 0); if (s) wcv_restore(s->cv); return OJS_UNDEFINED; }

// quad path of a rectangle (x, y, w, h) mapped through m
static int rect_path(double* path, const double* m, double x, double y, double w, double h) {
    double c[4][2] = { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        double dx, dy;
        mat_apply(m, c[i][0], c[i][1], &dx, &dy);
        path[n++] = i == 0 ? 0 : 1; path[n++] = dx; path[n++] = dy;
    }
    path[n++] = 4;
    return n;
}

// cvDrawImage(id, srcKind, srcId, sx, sy, sw, sh, dx, dy, dw, dh, ctm, comp, smooth)
NATIVE(n_cv_draw_image) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 14) return ojs_bool(0);
    int w = 0, h = 0, stride = 0;
    const uint32_t* px = source_pixels(js, (int)num(ctx, argc, argv, 1, 0), (int)num(ctx, argc, argv, 2, 0), &w, &h, &stride);
    if (!px || w <= 0 || h <= 0) return ojs_bool(0);
    double sx = num(ctx, argc, argv, 3, 0), sy = num(ctx, argc, argv, 4, 0), sw = num(ctx, argc, argv, 5, w), sh = num(ctx, argc, argv, 6, h);
    double dx = num(ctx, argc, argv, 7, 0), dy = num(ctx, argc, argv, 8, 0), dw = num(ctx, argc, argv, 9, w), dh = num(ctx, argc, argv, 10, h);
    size_t mn, cn;
    const double* ctm = f64(ctx, argv[11], &mn);
    const double* ca = f64(ctx, argv[12], &cn);
    if (!ctm || mn < 6) return ojs_bool(0);
    if (sw < 0) { sx += sw; sw = -sw; dx += dw; dw = -dw; }          // negative sizes flip
    if (sh < 0) { sy += sh; sh = -sh; dy += dh; dh = -dh; }
    // source rect clipped to the image (the destination shrinks with it)
    double cx0 = sx < 0 ? 0 : sx, cy0 = sy < 0 ? 0 : sy, cx1 = sx + sw > w ? w : sx + sw, cy1 = sy + sh > h ? h : sy + sh;
    if (cx1 <= cx0 || cy1 <= cy0 || dw == 0 || dh == 0 || sw == 0 || sh == 0) return ojs_bool(1);
    double kx = dw / sw, ky = dh / sh;
    double ddx = dx + (cx0 - sx) * kx, ddy = dy + (cy0 - sy) * ky, ddw = (cx1 - cx0) * kx, ddh = (cy1 - cy0) * ky;
    double inv[6];
    if (!mat_invert(ctm, inv)) return ojs_bool(1);
    // device -> user -> source pixel: u = cx0 + (user.x - ddx) / kx
    double toSrc[6] = { 1 / kx, 0, 0, 1 / ky, cx0 - ddx / kx, cy0 - ddy / ky };
    struct wcv_paint p;
    memset(&p, 0, sizeof p);
    p.kind = WCV_IMAGE;
    p.img = px; p.stride = stride;
    int isx = (int)floor(cx0 + 0.0001), isy = (int)floor(cy0 + 0.0001);
    int iex = (int)ceil(cx1 - 0.0001), iey = (int)ceil(cy1 - 0.0001);
    p.sx = isx; p.sy = isy; p.sw = iex - isx; p.sh = iey - isy;
    if (p.sw < 1) p.sw = 1;
    if (p.sh < 1) p.sh = 1;
    p.clamp = 1;
    p.smooth = num(ctx, argc, argv, 13, 1) != 0;
    // sample coordinates are relative to the sub-rectangle
    toSrc[4] -= isx; toSrc[5] -= isy;
    mat_mul(toSrc, inv, p.inv);
    struct wcv_comp comp;
    decode_comp(ca, cn, &comp);
    double path[16];
    int pn = rect_path(path, ctm, ddx, ddy, ddw, ddh);
    wcv_fill(s->cv, path, pn, 0, &p, &comp, 0);
    return ojs_bool(1);
}

// cvText(id, str, font[family, bold, italic, px], ctm, x, y, paint, comp) -> width in user units
NATIVE(n_cv_text) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 8) return OJS_UNDEFINED;
    size_t len = 0, fn, mn, pn, cn;
    char* str = ojs_to_cstring(ctx, argv[1], &len);
    if (!str) return OJS_UNDEFINED;
    const double* fa = f64(ctx, argv[2], &fn);
    const double* ctm = f64(ctx, argv[3], &mn);
    const double* pa = f64(ctx, argv[6], &pn);
    const double* ca = f64(ctx, argv[7], &cn);
    double x = num(ctx, argc, argv, 4, 0), y = num(ctx, argc, argv, 5, 0);
    if (!fa || fn < 4 || !ctm || mn < 6 || !pa) { ojs_free_cstring(ctx, str); return OJS_UNDEFINED; }
    double det = ctm[0] * ctm[3] - ctm[1] * ctm[2];
    double sc = sqrt(fabs(det));
    double inv[6];
    if (sc < 1e-6 || !mat_invert(ctm, inv)) { ojs_free_cstring(ctx, str); return OJS_UNDEFINED; }
    struct wcv_font f = { (int)fa[0], (int)fa[1], (int)fa[2], fa[3] * sc };
    int pure = ctm[1] == 0 && ctm[2] == 0 && ctm[0] == ctm[3] && ctm[0] > 0;      // translate + uniform scale
    double X0, Y0;
    mat_apply(ctm, x, y, &X0, &Y0);
    double fx = 0, fy = 0;
    if (pure) { fx = X0 - floor(X0); fy = Y0 - floor(Y0); }
    int mw, mh, ox, oy;
    uint8_t* mask = wcv_text_mask(str, (int)len, &f, fx, fy, &mw, &mh, &ox, &oy);
    ojs_free_cstring(ctx, str);
    if (!mask) return OJS_UNDEFINED;
    struct paintbuf* pb = (struct paintbuf*)w_malloc(sizeof *pb);
    if (!pb) { w_free(mask); return OJS_UNDEFINED; }
    struct wcv_comp comp;
    decode_paint(js, pa, pn, pb);
    decode_comp(ca, cn, &comp);
    struct wcv_mask m;
    m.m = mask; m.w = mw; m.h = mh;
    double path[16];
    int pcount;
    if (pure) {
        // device pixel (X, Y) <-> mask pixel (X - floor(X0) + ox, Y - floor(Y0) + oy), no resampling
        m.inv[0] = 1; m.inv[1] = 0; m.inv[2] = 0; m.inv[3] = 1;
        m.inv[4] = ox - floor(X0); m.inv[5] = oy - floor(Y0);
        m.smooth = 0;
        double ident[6] = { 1, 0, 0, 1, floor(X0) - ox, floor(Y0) - oy };
        pcount = rect_path(path, ident, 0, 0, mw, mh);
    } else {
        // mask = (user - (x, y)) * sc + (ox, oy); user = inv * device
        m.inv[0] = sc * inv[0]; m.inv[1] = sc * inv[1]; m.inv[2] = sc * inv[2]; m.inv[3] = sc * inv[3];
        m.inv[4] = sc * inv[4] + ox - sc * x; m.inv[5] = sc * inv[5] + oy - sc * y;
        m.smooth = 1;
        double toUser[6] = { 1 / sc, 0, 0, 1 / sc, x - ox / sc, y - oy / sc };
        double toDev[6];
        mat_mul(ctm, toUser, toDev);
        pcount = rect_path(path, toDev, 0, 0, mw, mh);
    }
    wcv_fill(s->cv, path, pcount, 0, &pb->p, &comp, &m);
    w_free(pb);
    w_free(mask);
    return OJS_UNDEFINED;
}

// cvMeasure(str, font[family, bold, italic, px]) -> [width, ascent, descent]
NATIVE(n_cv_measure) {
    UNUSED_THIS;
    size_t len = 0, fn;
    if (argc < 2) return OJS_UNDEFINED;
    char* str = ojs_to_cstring(ctx, argv[0], &len);
    if (!str) return OJS_UNDEFINED;
    const double* fa = f64(ctx, argv[1], &fn);
    ojsv out = ojs_array(ctx);
    if (fa && fn >= 4) {
        struct wcv_font f = { (int)fa[0], (int)fa[1], (int)fa[2], fa[3] };
        double asc, desc;
        wcv_font_metrics(&f, &asc, &desc);
        ojs_set_index(ctx, out, 0, ojs_number(wcv_text_width(str, (int)len, &f)));
        ojs_set_index(ctx, out, 1, ojs_number(asc));
        ojs_set_index(ctx, out, 2, ojs_number(desc));
    }
    ojs_free_cstring(ctx, str);
    return out;
}

// cvGet(id, x, y, w, h, uint8array)
NATIVE(n_cv_get) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 6) return ojs_bool(0);
    int x = (int)num(ctx, argc, argv, 1, 0), y = (int)num(ctx, argc, argv, 2, 0), w = (int)num(ctx, argc, argv, 3, 0), h = (int)num(ctx, argc, argv, 4, 0);
    size_t bytes = 0;
    uint8_t* dst = ojs_bytes(ctx, argv[5], &bytes);
    if (!dst || w <= 0 || h <= 0 || (size_t)w * h * 4 > bytes) return ojs_bool(0);
    wcv_get_image(s->cv, x, y, w, h, dst);
    return ojs_bool(1);
}

// cvPut(id, x, y, w, h, uint8array, dx, dy, dw, dh)
NATIVE(n_cv_put) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s || argc < 10) return ojs_bool(0);
    int x = (int)num(ctx, argc, argv, 1, 0), y = (int)num(ctx, argc, argv, 2, 0), w = (int)num(ctx, argc, argv, 3, 0), h = (int)num(ctx, argc, argv, 4, 0);
    size_t bytes = 0;
    const uint8_t* src = ojs_bytes(ctx, argv[5], &bytes);
    if (!src || w <= 0 || h <= 0 || (size_t)w * h * 4 > bytes) return ojs_bool(0);
    wcv_put_image(s->cv, x, y, w, h, src, (int)num(ctx, argc, argv, 6, 0), (int)num(ctx, argc, argv, 7, 0), (int)num(ctx, argc, argv, 8, w), (int)num(ctx, argc, argv, 9, h));
    return ojs_bool(1);
}

// cvPng(id) -> "data:image/png;base64,..."
NATIVE(n_cv_png) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wcvs* s = find(js, ctx, argc, argv, 0);
    if (!s) return ojs_string(ctx, "data:,");
    uint32_t len = 0;
    uint8_t* png = wcv_png(s->cv, &len);
    if (!png) return ojs_string(ctx, "data:,");
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char head[] = "data:image/png;base64,";
    size_t hl = sizeof head - 1, ol = hl + ((len + 2) / 3) * 4;
    char* out = (char*)w_malloc(ol + 1);
    if (!out) { w_free(png); return ojs_string(ctx, "data:,"); }
    memcpy(out, head, hl);
    size_t o = hl;
    for (uint32_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)png[i] << 16 | (i + 1 < len ? (uint32_t)png[i + 1] << 8 : 0) | (i + 2 < len ? png[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63]; out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < len ? b64[v >> 6 & 63] : '='; out[o++] = i + 2 < len ? b64[v & 63] : '=';
    }
    ojsv r = ojs_string_len(ctx, out, o);
    w_free(out);
    w_free(png);
    return r;
}

// cvPointInPath(path, x, y, evenodd) / cvPointInStroke(path, x, y, stroke)
NATIVE(n_cv_point_in_path) {
    UNUSED_THIS;
    size_t np;
    if (argc < 3) return ojs_bool(0);
    const double* path = f64(ctx, argv[0], &np);
    if (!path) return ojs_bool(0);
    return ojs_bool(wcv_point_in_path(path, (int)np, num(ctx, argc, argv, 1, 0), num(ctx, argc, argv, 2, 0), (int)num(ctx, argc, argv, 3, 0) != 0));
}

NATIVE(n_cv_point_in_stroke) {
    UNUSED_THIS;
    size_t np, sn;
    if (argc < 4) return ojs_bool(0);
    const double* path = f64(ctx, argv[0], &np);
    const double* sa = f64(ctx, argv[3], &sn);
    if (!path || !sa || sn < 5) return ojs_bool(0);
    struct wcv_stroke st;
    memset(&st, 0, sizeof st);
    st.width = sa[0]; st.cap = (int)sa[1]; st.join = (int)sa[2]; st.miter_limit = sa[3];
    return ojs_bool(wcv_point_in_stroke(path, (int)np, num(ctx, argc, argv, 1, 0), num(ctx, argc, argv, 2, 0), &st));
}

// cssColor(str) -> straight ARGB as a number, -1 when it is not a colour, -2 for currentcolor
NATIVE(n_css_color) {
    UNUSED_THIS;
    size_t len = 0;
    if (argc < 1) return ojs_number(-1);
    char* s = ojs_to_cstring(ctx, argv[0], &len);
    if (!s) return ojs_number(-1);
    uint32_t argb = 0;
    double r = -1;
    if (len == 12 && w_ieq(s, (int)len, "currentcolor")) r = -2;
    else if (css_parse_color(s, (int)len, &argb)) r = (double)argb;
    ojs_free_cstring(ctx, s);
    return ojs_number(r);
}

// nodeIndex(element) -> DOM node number (the handle the natives above take for <img> sources)
NATIVE(n_node_index) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    if (argc < 1) return ojs_int(-1);
    return ojs_int(wjs_node_of(js, argv[0]));
}

static const struct ojs_func_entry canvas_funcs[] = {
    { "nodeIndex", n_node_index, 1 },
    { "cvNew", n_cv_new, 2 }, { "cvFree", n_cv_free, 1 }, { "cvResize", n_cv_resize, 3 }, { "cvBind", n_cv_bind, 2 },
    { "cvSize", n_cv_size, 2 }, { "cvFill", n_cv_fill, 5 }, { "cvStroke", n_cv_stroke, 5 }, { "cvClip", n_cv_clip, 3 },
    { "cvSave", n_cv_save, 1 }, { "cvRestore", n_cv_restore, 1 }, { "cvDrawImage", n_cv_draw_image, 14 },
    { "cvText", n_cv_text, 8 }, { "cvMeasure", n_cv_measure, 2 }, { "cvGet", n_cv_get, 6 }, { "cvPut", n_cv_put, 10 },
    { "cvPng", n_cv_png, 1 }, { "cvPointInPath", n_cv_point_in_path, 4 }, { "cvPointInStroke", n_cv_point_in_stroke, 4 },
    { "cssColor", n_css_color, 1 },
};

void wjs_canvas_install(struct wjs* js, ojsv natives) {
    ojs_set_functions(js->J, natives, canvas_funcs, (int)(sizeof canvas_funcs / sizeof canvas_funcs[0]));
}

void wjs_canvas_free(struct wjs* js) {
    if (!js->canvas) return;
    for (int i = 0; i < CV_MAX; i++) if (js->canvas[i].cv) cv_release(js, &js->canvas[i]);
    w_free(js->canvas);
    js->canvas = 0;
}

// Publish canvases that changed; fills the repaint rectangles like the media poll. Returns 1 when a layout is needed.
int wjs_canvas_poll(struct wjs* js, struct wjs_rect* out, int max, int* n) {
    int relayout = 0;
    if (!js || js->dead || !js->canvas || !js->doc) return 0;
    for (int i = 0; i < CV_MAX; i++) {
        struct wcvs* s = &js->canvas[i];
        if (!s->cv || s->node < 0) continue;
        unsigned ser = wcv_serial(s->cv);
        int w = wcv_width(s->cv), h = wcv_height(s->cv);
        if (ser == s->published && w == s->published_w && h == s->published_h) continue;
        s->published = ser; s->published_w = w; s->published_h = h;
        if (wdoc_canvas_set(js->doc, s->node, wcv_pixels(s->cv), w, h)) { relayout = 1; continue; }
        int dx, dy, dw, dh, fixed;
        if (!wdoc_video_dest(js->doc, s->node, &dx, &dy, &dw, &dh, &fixed) || dw <= 0 || dh <= 0) continue;
        if (*n < max) { out[*n].x = dx; out[*n].y = dy; out[*n].w = dw; out[*n].h = dh; out[*n].fixed = fixed; (*n)++; }
    }
    return relayout;
}

int wjs_canvas_pending(struct wjs* js) {
    if (!js || js->dead || !js->canvas) return 0;
    for (int i = 0; i < CV_MAX; i++) {
        struct wcvs* s = &js->canvas[i];
        if (s->cv && s->node >= 0 && (wcv_serial(s->cv) != s->published || wcv_width(s->cv) != s->published_w || wcv_height(s->cv) != s->published_h)) return 1;
    }
    return 0;
}
