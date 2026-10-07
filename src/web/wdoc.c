// Document controller: DOM + CSS + images + layout + paint.
#include "wdoc.h"
#include "wdom.h"
#include "css.h"
#include "layout.h"
#include "paint.h"
#include "image.h"
#include "wurl.h"
#include "wcommon.h"
#include "css_int.h"

enum { RES_CSS, RES_IMG };
enum { RS_PENDING, RS_INFLIGHT, RS_DONE, RS_FAILED };

#define MAX_IMAGES 160
#define MAX_IMG_DIM 2048

struct wres {
    char url[512];
    uint8_t kind, state;
    int32_t order;          // CSS cascade order key
    char media[96];
    int img;                // RES_IMG: image slot
};

struct wdimg {
    int w, h;
    int svg_scale;          // rendered at N x the intrinsic size (crisper)
    uint32_t* px;
    int state;              // 0 pending, 1 ok, 2 failed
    int res;
    char url[512];
};

struct wdoc {
    char url[512];
    char base[512];
    struct wdom* d;
    struct wstyleset* ss;
    struct wlayout* L;
    int vw, vh;
    int style_dirty, layout_dirty;
    struct wres* res;
    int nres, capres;
    struct wdimg* img;
    int nimg, capimg;
    int32_t* sheet_order;   // order keys of the sheets in ss (same order)
    int nsheet, capsheet;
    int32_t* node_img;      // DOM node -> image slot (-1)
    struct wlay_images li;
    struct wpaint_env env;
};

// ---- data: URLs --------------------------------------------------------------------

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int pct_hex(const char* s, int i, int len) {
    return i + 2 < len && w_ishex((unsigned char)s[i + 1]) && w_ishex((unsigned char)s[i + 2]);
}

char* wdoc_data_url(const char* url, int len, int* out_len, char* mime, int mime_cap) {
    if (len < 5 || !w_ieq_prefix(url, len, "data:")) return 0;
    int comma = -1;
    for (int i = 5; i < len; i++) if (url[i] == ',') { comma = i; break; }
    if (comma < 0) return 0;
    int b64 = 0;
    int mi = 0;
    for (int i = 5; i < comma; i++) {
        if (url[i] == ';') {
            if (w_ieq_prefix(url + i + 1, comma - i - 1, "base64")) b64 = 1;
            break;
        }
        if (mime && mi < mime_cap - 1) mime[mi++] = (char)w_lower((unsigned char)url[i]);
    }
    if (mime && mime_cap > 0) mime[mi] = 0;
    const char* p = url + comma + 1;
    int pl = len - comma - 1;
    char* out = (char*)w_malloc(pl + 1);
    if (!out) return 0;
    int o = 0;
    if (b64) {
        uint32_t acc = 0;
        int bits = 0;
        for (int i = 0; i < pl; i++) {
            int c = (unsigned char)p[i];
            if (c == '%' && pct_hex(p, i, pl)) { c = w_hexval((unsigned char)p[i + 1]) * 16 + w_hexval((unsigned char)p[i + 2]); i += 2; }
            int v = b64v(c);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out[o++] = (char)(acc >> bits); }
        }
    } else {
        for (int i = 0; i < pl; i++) {
            if (p[i] == '%' && pct_hex(p, i, pl)) {
                out[o++] = (char)(w_hexval((unsigned char)p[i + 1]) * 16 + w_hexval((unsigned char)p[i + 2]));
                i += 2;
            } else out[o++] = p[i];
        }
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

// ---- resources ---------------------------------------------------------------------

static int res_add(struct wdoc* d, const char* url, int kind, int32_t order, const char* media, int mlen) {
    for (int i = 0; i < d->nres; i++)
        if (d->res[i].kind == kind && kind == RES_IMG && !strcmp(d->res[i].url, url)) return i;
    if (d->nres >= d->capres) {
        int nc = d->capres ? d->capres * 2 : 32;
        struct wres* n = (struct wres*)w_realloc(d->res, nc * sizeof(struct wres));
        if (!n) return -1;
        d->res = n;
        d->capres = nc;
    }
    struct wres* r = &d->res[d->nres];
    memset(r, 0, sizeof *r);
    int n = (int)strlen(url);
    if (n > 511) n = 511;
    memcpy(r->url, url, n);
    r->url[n] = 0;
    r->kind = (uint8_t)kind;
    r->order = order;
    r->img = -1;
    if (media && mlen > 0) {
        int m = mlen < 95 ? mlen : 95;
        memcpy(r->media, media, m);
        r->media[m] = 0;
    }
    return d->nres++;
}

// ---- SVG documents as images ------------------------------------------------------

static int looks_svg(const char* b, int len) {
    int n = len < 1024 ? len : 1024;
    for (int i = 0; i + 4 <= n; i++)
        if (b[i] == '<' && (b[i + 1] == 's' || b[i + 1] == 'S') && w_ieq_prefix(b + i + 1, n - i - 1, "svg")) return 1;
    return 0;
}

static int32_t svg_len_attr(struct wdom* dom, int el, int atom) {
    int vl;
    const char* v = wdom_attr(dom, el, atom, &vl);
    if (!v) return -1;
    int32_t m; int u;
    if (!cv_number(v, vl, &m, &u) || m <= 0) return -1;
    if (u < vl && v[u] == '%') return -1;
    return m / 1000;
}

// Rasterize an SVG file into straight ARGB (alpha recovered by rendering on
// black and on white).
static int svg_to_image(const char* bytes, int len, int* W, int* H, uint32_t** out) {
    struct wdom* dom = whtml_parse(bytes, len, "utf-8");
    if (!dom) return 0;
    int root = -1;
    for (int i = dom->n[0].first; i >= 0; i = wdom_next(dom, i, 0))
        if (dom->n[i].type == WN_ELEM && dom->n[i].ns == NS_SVG && dom->n[i].tag == T_svg) { root = i; break; }
    if (root < 0) { wdom_free(dom); return 0; }
    int32_t w = svg_len_attr(dom, root, A_width), h = svg_len_attr(dom, root, A_height);
    int vl;
    const char* vb = wdom_attr(dom, root, A_viewbox, &vl);
    int32_t vw = 0, vh = 0;
    if (vb) {
        int32_t v[4]; int k = 0, pos = 0;
        while (k < 4 && pos < vl) {
            while (pos < vl && (vb[pos] == ' ' || vb[pos] == ',')) pos++;
            int32_t m; int u;
            if (!cv_number(vb + pos, vl - pos, &m, &u)) break;
            v[k++] = m;
            pos += u;
        }
        if (k == 4) { vw = v[2] / 1000; vh = v[3] / 1000; }
    }
    if (w <= 0 && h <= 0) { w = vw > 0 ? vw : 300; h = vh > 0 ? vh : 150; }
    else if (w <= 0) w = (vw > 0 && vh > 0) ? h * vw / vh : h;
    else if (h <= 0) h = (vw > 0 && vh > 0) ? w * vh / vw : w;
    // render small icons at 2x for crisper downscaling, cap large ones
    int scale = (w < 128 && h < 128) ? 2 : 1;
    int RW = w * scale, RH = h * scale;
    while (RW > 1024 || RH > 1024) { RW /= 2; RH /= 2; }
    if (RW < 1) RW = 1;
    if (RH < 1) RH = 1;
    struct wstyleset* ss = css_set_new(dom);
    if (!ss) { wdom_free(dom); return 0; }
    // <style> elements inside the SVG
    for (int i = dom->n[0].first; i >= 0; i = wdom_next(dom, i, 0)) {
        if (dom->n[i].type != WN_ELEM || dom->n[i].tag != T_style) continue;
        char buf[16384];
        int n = wdom_text_content(dom, i, buf, sizeof buf);
        css_set_add_sheet(ss, buf, n, "", -1);
    }
    css_compute_all(ss, RW, RH);
    uint32_t* black = (uint32_t*)w_malloc((size_t)RW * RH * 4);
    uint32_t* white = (uint32_t*)w_malloc((size_t)RW * RH * 4);
    if (!black || !white) { w_free(black); w_free(white); css_set_free(ss); wdom_free(dom); return 0; }
    struct wsurf sf;
    sf.w = RW; sf.h = RH; sf.stride = RW;
    sf.px = black;
    ws_reset_clip(&sf);
    for (int i = 0; i < RW * RH; i++) black[i] = 0;
    wsvg_paint(&sf, dom, ss, root, 0, 0, RW, RH, 0xFF000000, 255);
    sf.px = white;
    ws_reset_clip(&sf);
    for (int i = 0; i < RW * RH; i++) white[i] = 0xFFFFFF;
    wsvg_paint(&sf, dom, ss, root, 0, 0, RW, RH, 0xFF000000, 255);
    for (int i = 0; i < RW * RH; i++) {
        uint32_t b = black[i], wh = white[i];
        int a = 255 - (int)(((wh >> 8) & 255) - ((b >> 8) & 255));
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        uint32_t px = 0;
        if (a) {
            int r = ((b >> 16) & 255) * 255 / a, g = ((b >> 8) & 255) * 255 / a, bl = (b & 255) * 255 / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (bl > 255) bl = 255;
            px = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        }
        black[i] = px;
    }
    w_free(white);
    css_set_free(ss);
    wdom_free(dom);
    *W = RW / scale > 0 ? RW : RW;
    *H = RH;
    *out = black;
    return scale;
}

static void img_decode_into(struct wdoc* d, int slot, const char* bytes, int len) {
    struct wdimg* im = &d->img[slot];
    int w = 0, h = 0;
    uint32_t* px = 0;
    if (len > 0 && looks_svg(bytes, len)) {
        int sc = svg_to_image(bytes, len, &w, &h, &px);
        if (sc) {
            im->px = px;
            im->w = w;
            im->h = h;
            im->svg_scale = sc;
            im->state = 1;
        } else im->state = 2;
        return;
    }
    if (len > 0 && wimage_decode((const uint8_t*)bytes, len, MAX_IMG_DIM, &w, &h, &px)) {
        im->w = w;
        im->h = h;
        im->px = px;
        im->state = 1;
    } else {
        im->state = 2;
    }
}

// Find or register an image by absolute URL; returns slot.
static int img_slot(struct wdoc* d, const char* url) {
    for (int i = 0; i < d->nimg; i++) if (!strcmp(d->img[i].url, url)) return i;
    if (d->nimg >= MAX_IMAGES) return -1;
    if (d->nimg >= d->capimg) {
        int nc = d->capimg ? d->capimg * 2 : 32;
        struct wdimg* n = (struct wdimg*)w_realloc(d->img, nc * sizeof(struct wdimg));
        if (!n) return -1;
        d->img = n;
        d->capimg = nc;
    }
    struct wdimg* im = &d->img[d->nimg];
    memset(im, 0, sizeof *im);
    int n = (int)strlen(url);
    if (n > 511) n = 511;
    memcpy(im->url, url, n);
    im->url[n] = 0;
    im->res = -1;
    int slot = d->nimg++;
    if (w_ieq_prefix(url, n, "data:")) {
        int bl;
        char mime[64];
        char* bytes = wdoc_data_url(url, (int)strlen(url), &bl, mime, sizeof mime);
        if (bytes) {
            img_decode_into(d, slot, bytes, bl);
            w_free(bytes);
        } else im->state = 2;
    } else if (w_ieq_prefix(url, n, "http:") || w_ieq_prefix(url, n, "https:")) {
        int r = res_add(d, url, RES_IMG, 0, 0, 0);
        if (r >= 0) { d->res[r].img = slot; d->img[slot].res = r; }
    } else im->state = 2;
    return slot;
}

// choose the image URL for an <img> (src / srcset / lazy-load data-src)
static int pick_img_url(struct wdoc* d, int el, char* out, int cap) {
    struct wdom* dom = d->d;
    int sl;
    const char* src = wdom_attr(dom, el, A_src, &sl);
    int lazy_l;
    const char* lazy = wdom_attr_s(dom, el, "data-src", &lazy_l);
    if (!lazy) lazy = wdom_attr_s(dom, el, "data-lazy-src", &lazy_l);
    if (lazy && (!src || (sl > 5 && w_ieq_prefix(src, sl, "data:")))) { src = lazy; sl = lazy_l; }
    char tmp[1024];
    if (!src || !sl) {
        int ssl;
        const char* ss = wdom_attr(dom, el, A_srcset, &ssl);
        if (!ss) ss = wdom_attr_s(dom, el, "data-srcset", &ssl);
        if (!ss) return 0;
        int i = 0;
        while (i < ssl && w_isspace((unsigned char)ss[i])) i++;
        int st = i;
        while (i < ssl && !w_isspace((unsigned char)ss[i]) && ss[i] != ',') i++;
        int n = i - st < 1023 ? i - st : 1023;
        memcpy(tmp, ss + st, n);
        tmp[n] = 0;
        src = tmp;
        sl = n;
    }
    if (sl <= 0) return 0;
    if (w_ieq_prefix(src, sl, "data:")) {
        int n = sl < cap - 1 ? sl : cap - 1;
        memcpy(out, src, n);
        out[n] = 0;
        return n;
    }
    return wurl_resolve(d->base, src, sl, out, cap) > 0;
}

static int lay_img_node(void* ctx, int node, int* w, int* h) {
    struct wdoc* d = (struct wdoc*)ctx;
    *w = *h = 0;
    if (node < 0 || node >= d->d->nn || !d->node_img) return -1;
    int slot = d->node_img[node];
    if (slot < 0) return -1;
    if (d->img[slot].state == 1) {
        int sc = d->img[slot].svg_scale > 0 ? d->img[slot].svg_scale : 1;
        *w = d->img[slot].w / sc;
        *h = d->img[slot].h / sc;
    }
    return slot;
}

static int lay_img_url(void* ctx, const char* url, int* w, int* h) {
    struct wdoc* d = (struct wdoc*)ctx;
    *w = *h = 0;
    int slot = img_slot(d, url);
    if (slot < 0) return -1;
    if (d->img[slot].state == 1) {
        int sc = d->img[slot].svg_scale > 0 ? d->img[slot].svg_scale : 1;
        *w = d->img[slot].w / sc;
        *h = d->img[slot].h / sc;
    }
    return slot;
}

static const struct wimg* paint_img(void* ctx, int id) {
    struct wdoc* d = (struct wdoc*)ctx;
    static struct wimg tmp;
    if (id < 0 || id >= d->nimg || d->img[id].state != 1) return 0;
    tmp.w = d->img[id].w;
    tmp.h = d->img[id].h;
    tmp.px = d->img[id].px;
    return &tmp;
}

// ---- lifecycle -----------------------------------------------------------------------

struct wdoc* wdoc_new(void) {
    struct wdoc* d = (struct wdoc*)w_calloc(1, sizeof(struct wdoc));
    if (!d) return 0;
    d->vw = 1024;
    d->vh = 768;
    d->li.ctx = d;
    d->li.for_node = lay_img_node;
    d->li.for_url = lay_img_url;
    return d;
}

static void doc_clear(struct wdoc* d) {
    if (d->L) wlay_free(d->L);
    if (d->ss) css_set_free(d->ss);
    if (d->d) wdom_free(d->d);
    for (int i = 0; i < d->nimg; i++) w_free(d->img[i].px);
    w_free(d->img);
    w_free(d->res);
    w_free(d->sheet_order);
    w_free(d->node_img);
    d->L = 0; d->ss = 0; d->d = 0; d->img = 0; d->res = 0; d->sheet_order = 0; d->node_img = 0;
    d->nimg = d->capimg = d->nres = d->capres = d->nsheet = d->capsheet = 0;
}

void wdoc_free(struct wdoc* d) {
    if (!d) return;
    doc_clear(d);
    w_free(d);
}

static void add_sheet_ordered(struct wdoc* d, const char* text, int len, const char* base, int32_t order,
                              const char* media, int mlen) {
    int pos = 0;
    while (pos < d->nsheet && d->sheet_order[pos] <= order) pos++;
    if (d->nsheet >= d->capsheet) {
        int nc = d->capsheet ? d->capsheet * 2 : 16;
        int32_t* n = (int32_t*)w_realloc(d->sheet_order, nc * sizeof(int32_t));
        if (!n) return;
        d->sheet_order = n;
        d->capsheet = nc;
    }
    int at = css_set_add_sheet_ex(d->ss, text, len, base, pos, media, mlen);
    if (at < 0) return;
    for (int i = d->nsheet; i > at; i--) d->sheet_order[i] = d->sheet_order[i - 1];
    d->sheet_order[at] = order;
    d->nsheet++;
    d->style_dirty = 1;
    // newly discovered @imports: fetch them, cascading just before this sheet
    char urls[16][256];
    int before[16];
    int n = css_set_pending_imports(d->ss, urls, before, 16);
    for (int k = 0; k < n; k++) {
        css_set_mark_import_done(d->ss, urls[k]);
        int32_t o = d->sheet_order[before[k] < d->nsheet ? before[k] : d->nsheet - 1] - 256 + k;
        res_add(d, urls[k], RES_CSS, o, 0, 0);
    }
}

static int rel_has(const char* rel, int rl, const char* word) {
    int wl = (int)strlen(word);
    for (int i = 0; i + wl <= rl; i++)
        if ((i == 0 || w_isspace((unsigned char)rel[i - 1])) && w_ieq_prefix(rel + i, rl - i, word) &&
            (i + wl == rl || w_isspace((unsigned char)rel[i + wl])))
            return 1;
    return 0;
}

int wdoc_load(struct wdoc* d, const char* url, const char* html, int len, const char* charset) {
    doc_clear(d);
    int n = (int)strlen(url);
    if (n > 511) n = 511;
    memcpy(d->url, url, n);
    d->url[n] = 0;
    memcpy(d->base, d->url, n + 1);
    d->d = whtml_parse(html, len, charset);
    if (!d->d) return -1;
    struct wdom* dom = d->d;
    // <base href>
    int be = wdom_first_tag(dom, T_base);
    if (be >= 0) {
        int hl;
        const char* h = wdom_attr(dom, be, A_href, &hl);
        if (h) wurl_resolve(d->url, h, hl, d->base, sizeof d->base);
    }
    d->ss = css_set_new(dom);
    if (!d->ss) return -1;
    css_set_doc_base(d->ss, d->base);
    d->node_img = (int32_t*)w_malloc((dom->nn + 1) * sizeof(int32_t));
    if (d->node_img) for (int i = 0; i <= dom->nn; i++) d->node_img[i] = -1;
    int32_t idx = 0;
    char abs[1024];
    for (int el = dom->n[0].first; el >= 0; el = wdom_next(dom, el, 0)) {
        if (dom->n[el].type != WN_ELEM || dom->n[el].ns != NS_HTML) continue;
        int tag = dom->n[el].tag;
        idx++;
        if (tag == T_style) {
            int tl = 0;
            for (int c = dom->n[el].first; c >= 0; c = dom->n[c].next) if (dom->n[c].type == WN_TEXT) tl += (int)dom->n[c].tlen;
            char* buf = (char*)w_malloc(tl + 1);
            if (!buf) continue;
            int got = wdom_text_content(dom, el, buf, tl + 1);
            int ml;
            const char* media = wdom_attr(dom, el, A_media, &ml);
            add_sheet_ordered(d, buf, got, d->base, idx * 1024, media, media ? ml : 0);
            w_free(buf);
        } else if (tag == T_link) {
            int rl, hl;
            const char* rel = wdom_attr(dom, el, A_rel, &rl);
            const char* href = wdom_attr(dom, el, A_href, &hl);
            if (!rel || !href || !rel_has(rel, rl, "stylesheet") || rel_has(rel, rl, "alternate")) continue;
            if (wdom_has_attr(dom, el, A_disabled)) continue;
            if (wurl_resolve(d->base, href, hl, abs, sizeof abs) <= 0) continue;
            int ml;
            const char* media = wdom_attr(dom, el, A_media, &ml);
            if (media && (w_ieq(media, ml, "print") || w_ieq(media, ml, "speech"))) continue;
            res_add(d, abs, RES_CSS, idx * 1024, media, media ? ml : 0);
        } else if (tag == T_img || (tag == T_input && wdom_attr(dom, el, A_type, &(int){0}) &&
                                    w_ieq(wdom_attr(dom, el, A_type, &(int){0}), 5, "image"))) {
            if (pick_img_url(d, el, abs, sizeof abs) > 0) {
                int slot = img_slot(d, abs);
                if (d->node_img && slot >= 0) d->node_img[el] = slot;
            }
        } else if (tag == T_video) {
            int pl;
            const char* poster = wdom_attr_s(dom, el, "poster", &pl);
            if (poster && wurl_resolve(d->base, poster, pl, abs, sizeof abs) > 0) {
                int slot = img_slot(d, abs);
                if (d->node_img && slot >= 0) d->node_img[el] = slot;
            }
        }
    }
    d->style_dirty = 1;
    d->layout_dirty = 1;
    return 0;
}

int wdoc_next_fetch(struct wdoc* d, char* url, int cap) {
    // stylesheets first (they gate the first meaningful paint), then images
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < d->nres; i++) {
            struct wres* r = &d->res[i];
            if (r->state != RS_PENDING || (pass == 0) != (r->kind == RES_CSS)) continue;
            r->state = RS_INFLIGHT;
            int n = (int)strlen(r->url);
            if (n > cap - 1) n = cap - 1;
            memcpy(url, r->url, n);
            url[n] = 0;
            return i;
        }
    return -1;
}

int wdoc_is_css(struct wdoc* d, int id) { return id >= 0 && id < d->nres && d->res[id].kind == RES_CSS; }

int wdoc_pending(struct wdoc* d) {
    int n = 0;
    for (int i = 0; i < d->nres; i++) if (d->res[i].state <= RS_INFLIGHT) n++;
    return n;
}

void wdoc_fetch_done(struct wdoc* d, int id, const char* bytes, int len, const char* ctype) {
    if (id < 0 || id >= d->nres) return;
    struct wres* r = &d->res[id];
    if (len < 0 || !bytes) {
        r->state = RS_FAILED;
        if (r->kind == RES_IMG && r->img >= 0) d->img[r->img].state = 2;
        return;
    }
    r->state = RS_DONE;
    (void)ctype;
    if (r->kind == RES_CSS) {
        add_sheet_ordered(d, bytes, len, r->url, r->order, r->media[0] ? r->media : 0, (int)strlen(r->media));
        d->layout_dirty = 1;
    } else if (r->img >= 0) {
        img_decode_into(d, r->img, bytes, len);
        d->layout_dirty = 1;
    }
}

void wdoc_set_viewport(struct wdoc* d, int w, int h) {
    if (w != d->vw) { d->style_dirty = 1; d->layout_dirty = 1; }
    if (h != d->vh) { d->style_dirty = 1; d->layout_dirty = 1; }
    d->vw = w;
    d->vh = h;
}

void wdoc_invalidate(struct wdoc* d) { d->style_dirty = 1; d->layout_dirty = 1; }

int wdoc_update(struct wdoc* d) {
    if (!d->d || !d->ss) return 0;
    if (!d->style_dirty && !d->layout_dirty && d->L) return 0;
    if (d->style_dirty) css_compute_all(d->ss, d->vw, d->vh);
    if (d->L) wlay_free(d->L);
    d->L = wlay_run(d->d, d->ss, d->vw, d->vh, &d->li);
    d->style_dirty = 0;
    d->layout_dirty = 0;
    return 1;
}

int wdoc_height(struct wdoc* d) { wdoc_update(d); return d->L ? wlay_doc_height(d->L) : 0; }
int wdoc_width(struct wdoc* d) { wdoc_update(d); return d->L ? wlay_doc_width(d->L) : 0; }
uint32_t wdoc_background(struct wdoc* d) { wdoc_update(d); return d->L ? wlay_canvas_bg(d->L) : 0xFFFFFFFF; }
const char* wdoc_title(struct wdoc* d) { return d->d ? d->d->title : ""; }
const char* wdoc_url(struct wdoc* d) { return d->url; }
struct wdom* wdoc_dom(struct wdoc* d) { return d->d; }

void wdoc_paint(struct wdoc* d, struct wsurf* s, int scroll_y) {
    wdoc_update(d);
    if (!d->L) { ws_fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFFFF); return; }
    d->env.ss = d->ss;
    d->env.d = d->d;
    d->env.image = paint_img;
    d->env.ctx = d;
    wpaint(d->L, s, scroll_y, &d->env);
}

int wdoc_hit(struct wdoc* d, int x, int y, int scroll_y, struct wdoc_hit* out) {
    out->kind = WDOC_HIT_NONE;
    out->node = -1;
    out->href[0] = 0;
    wdoc_update(d);
    if (!d->L) return 0;
    const struct dhit* h;
    int n = wlay_hits(d->L, &h);
    int32_t X = PX(x), Y = PX(y), Yd = PX(y + scroll_y);
    for (int i = n - 1; i >= 0; i--) {
        int32_t yy = h[i].fixed ? Y : Yd;
        if (X < h[i].x || X >= h[i].x + h[i].w || yy < h[i].y || yy >= h[i].y + h[i].h) continue;
        out->node = h[i].node;
        out->kind = h[i].kind == HIT_LINK ? WDOC_HIT_LINK : h[i].kind == HIT_FIELD ? WDOC_HIT_FIELD : WDOC_HIT_BUTTON;
        if (out->kind == WDOC_HIT_LINK) {
            int hl;
            const char* href = wdom_attr(d->d, h[i].node, A_href, &hl);
            if (href) wurl_resolve(d->base, href, hl, out->href, sizeof out->href);
        }
        return out->kind;
    }
    return 0;
}

int wdoc_anchor_y(struct wdoc* d, const char* frag) {
    if (!d->d || !frag || !frag[0]) return -1;
    wdoc_update(d);
    int el = wdom_find_id(d->d, frag);
    if (el < 0) {
        for (int i = d->d->n[0].first; i >= 0; i = wdom_next(d->d, i, 0)) {
            if (!wdom_is(d->d, i, T_a)) continue;
            int nl;
            const char* nm = wdom_attr(d->d, i, A_name, &nl);
            if (nm && nl == (int)strlen(frag) && !memcmp(nm, frag, nl)) { el = i; break; }
        }
    }
    int32_t x, y, w, h;
    if (el < 0 || !d->L || !wlay_node_rect(d->L, el, &x, &y, &w, &h)) return -1;
    return LU_FLOOR(y);
}

void wdoc_stats(struct wdoc* d, char* out, int cap) {
    int boxes = 0, items = 0, lines = 0, rules = 0, styles = 0, sheets = 0;
    if (d->L) wlay_stats(d->L, &boxes, &items, &lines);
    if (d->ss) css_stats(d->ss, &rules, &styles, &sheets);
    int ok = 0;
    for (int i = 0; i < d->nimg; i++) if (d->img[i].state == 1) ok++;
    int n = 0;
    const char* parts[1] = { 0 };
    (void)parts;
    // tiny formatter (no snprintf in the kernel)
    char tmp[256];
    int vals[8] = { d->d ? d->d->nn : 0, sheets, rules, styles, boxes, items, lines, ok };
    const char* names[8] = { "nodes=", " sheets=", " rules=", " styles=", " boxes=", " items=", " lines=", " imgs=" };
    for (int k = 0; k < 8; k++) {
        for (const char* p = names[k]; *p && n < 250; p++) tmp[n++] = *p;
        char num[12]; int nl = 0; int v = vals[k];
        if (!v) num[nl++] = '0';
        while (v > 0 && nl < 11) { num[nl++] = (char)('0' + v % 10); v /= 10; }
        while (nl && n < 250) tmp[n++] = num[--nl];
    }
    tmp[n] = 0;
    int c = n < cap - 1 ? n : cap - 1;
    memcpy(out, tmp, c);
    out[c] = 0;
}

int wdoc_pending_css(struct wdoc* d) {
    int n = 0;
    for (int i = 0; i < d->nres; i++)
        if (d->res[i].kind == RES_CSS && d->res[i].state <= RS_INFLIGHT) n++;
    return n;
}

int wdoc_has_fixed(struct wdoc* d) {
    wdoc_update(d);
    if (!d->L) return 0;
    const struct ditem* it;
    int n = wlay_items(d->L, &it);
    for (int i = 0; i < n; i++) if (it[i].fixed) return 1;
    return 0;
}

int wdoc_node_rect(struct wdoc* d, int node, int* x, int* y, int* w, int* h) {
    wdoc_update(d);
    int32_t X, Y, W, H;
    if (!d->L || node < 0 || !wlay_node_rect(d->L, node, &X, &Y, &W, &H)) return 0;
    *x = LU_FLOOR(X); *y = LU_FLOOR(Y); *w = LU_ROUND(W); *h = LU_ROUND(H);
    return 1;
}

int wdoc_hit_count(struct wdoc* d) {
    wdoc_update(d);
    if (!d->L) return 0;
    const struct dhit* h;
    return wlay_hits(d->L, &h);
}

int wdoc_hit_get(struct wdoc* d, int i, struct wdoc_region* out) {
    if (!d->L) return 0;
    const struct dhit* h;
    int n = wlay_hits(d->L, &h);
    if (i < 0 || i >= n) return 0;
    out->kind = h[i].kind == HIT_LINK ? WDOC_HIT_LINK : h[i].kind == HIT_FIELD ? WDOC_HIT_FIELD : WDOC_HIT_BUTTON;
    out->node = h[i].node;
    out->x = LU_FLOOR(h[i].x);
    out->y = LU_FLOOR(h[i].y);
    out->w = LU_ROUND(h[i].w);
    out->h = LU_ROUND(h[i].h);
    out->fixed = h[i].fixed;
    out->href[0] = 0;
    if (out->kind == WDOC_HIT_LINK) {
        int hl;
        const char* href = wdom_attr(d->d, h[i].node, A_href, &hl);
        if (href) wurl_resolve(d->base, href, hl, out->href, sizeof out->href);
    }
    return 1;
}
