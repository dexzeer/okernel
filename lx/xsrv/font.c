// Core fonts of the X server: fixed-cell fonts ("fixed", "6x13", XLFD names), glyphs rasterised by the system font (OKW_TEXT).
#include "xsrv.h"

static const struct { int w, h; } sizes[] = { {5,7},{5,8},{6,9},{6,10},{6,12},{6,13},{7,13},{7,14},{8,13},{8,16},{9,15},{9,18},{10,20} };
#define NSIZES ((int)(sizeof sizes / sizeof sizes[0]))

static int num(const char *s, int n, int *used) {
    int v = 0, i = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    *used = i;
    return v;
}

static void set_cell(struct font *f, int cw, int ch, int bold) {
    if (cw < 3) cw = 3;
    if (ch < 6) ch = 6;
    f->cw = cw;
    f->desc = ch >= 10 ? imax(2, ch / 5) : 1;
    f->asc = ch - f->desc;
    f->size = imin(cw * 5 / 3, ch - 1);
    if (f->size < 6) f->size = 6;
    f->bold = bold;
}

struct font *font_open(u32 id, const char *name, int len) {
    char nm[100];
    if (len > 99) len = 99;
    memcpy(nm, name, (u64)len); nm[len] = 0;
    int cw = 0, ch = 0, bold = 0;
    if (!strcmp(nm, "fixed") || !strcmp(nm, "variable")) { cw = 6; ch = 13; }
    else if (!strcmp(nm, "cursor")) { cw = 16; ch = 16; }
    else {
        int u1, u2;
        int w = num(nm, len, &u1);
        if (u1 && nm[u1] == 'x') {
            int h = num(nm + u1 + 1, len - u1 - 1, &u2);
            if (u2 && !nm[u1 + 1 + u2]) { cw = w; ch = h; }
        }
    }
    if (!cw) {
        if (nm[0] != '-' && !(strlen(nm) > 1 && (nm[0] == '*'))) return 0;
        // XLFD: -foundry-family-weight-slant-setwidth-adstyle-pixel-point-resx-resy-spacing-avgwidth-registry-encoding
        char *fld[16]; int nf = 0; char *p = nm;
        while (nf < 16) { fld[nf++] = p; while (*p && *p != '-') p++; if (!*p) break; *p++ = 0; }
        int pix = 0, avg = 0, u;
        if (nf > 7 && fld[7][0] >= '0' && fld[7][0] <= '9') pix = num(fld[7], 10, &u);
        if (!pix && nf > 8 && fld[8][0] >= '0' && fld[8][0] <= '9') pix = num(fld[8], 10, &u) / 10 * 100 / 72;
        if (nf > 12 && fld[12][0] >= '0' && fld[12][0] <= '9') avg = num(fld[12], 10, &u);
        if (nf > 3 && (glob_match("*bold*", fld[3]) || glob_match("bold", fld[3]))) bold = 1;
        if (!pix) pix = 13;
        ch = pix;
        cw = avg ? (avg + 5) / 10 : (pix * 6 + 6) / 13;
        // keep to the sizes that exist when the family is the misc fixed one
        if (!avg) for (int i = 0; i < NSIZES; i++) if (sizes[i].h == pix) { cw = sizes[i].w; break; }
    }
    struct font *f = (struct font*)xcalloc(sizeof *f);
    f->id = id;
    memcpy(f->name, name, (u64)len);
    set_cell(f, cw, ch, bold);
    f->refs = 1;
    return f;
}

struct font *font_default(void) {
    static struct font *d;
    if (!d) d = font_open(0, "fixed", 5);
    return d;
}

void font_unref(struct font *f) {
    if (!f || --f->refs > 0) return;
    for (int i = 0; i < 256; i++) xfree(f->g[i].m);
    xfree(f);
}

// the glyph of byte `c` (Latin-1) as an 8-bit coverage mask, w x h, the baseline at row g->asc
struct fglyph *font_glyph(struct font *f, int c);
struct fglyph *font_glyph(struct font *f, int c) {
    struct fglyph *g = &f->g[c & 255];
    if (g->done) return g;
    g->done = 1;
    if (c < 32 || c == 127) return g;
    unsigned char s[2]; int n = 0;
    if (c < 128) s[n++] = (unsigned char)c; else { s[n++] = (unsigned char)(0xC0 | (c >> 6)); s[n++] = (unsigned char)(0x80 | (c & 63)); }
    int cw = f->cw * 2 + 2, chh = (f->asc + f->desc) * 2;
    if (cw > 256) cw = 256;
    if (chh > 256) chh = 256;
    u8 *m = (u8*)xcalloc((u64)cw * (u64)chh);
    struct okw_text t;
    memset(&t, 0, sizeof t);
    t.str = (unsigned long long)s; t.len = n; t.size = f->size; t.flags = f->bold ? 1 : 0; t.family = 2;
    t.mask = (unsigned long long)m; t.cap_w = cw; t.cap_h = chh;
    if (okw_ioctl(OKW_TEXT, &t) < 0) { xfree(m); return g; }
    g->m = m; g->w = cw; g->h = chh; g->asc = t.out_asc;
    return g;
}

int font_list(const char *pat, int len, char names[][100], int max) {
    char p[100];
    if (len > 99) len = 99;
    memcpy(p, pat, (u64)len); p[len] = 0;
    int n = 0;
    char tmp[100];
    #define OFFER(s) do { if (n < max && glob_match(p, s)) { int k = 0; while ((s)[k] && k < 99) { names[n][k] = (s)[k]; k++; } names[n][k] = 0; n++; } } while (0)
    OFFER("fixed"); OFFER("variable"); OFFER("cursor");
    for (int i = 0; i < NSIZES; i++) {
        char *o = tmp;
        int w = sizes[i].w, h = sizes[i].h;
        o = tmp;
        if (w >= 10) *o++ = (char)('0' + w / 10);
        *o++ = (char)('0' + w % 10); *o++ = 'x';
        if (h >= 10) *o++ = (char)('0' + h / 10);
        *o++ = (char)('0' + h % 10); *o = 0;
        OFFER(tmp);
        char x[100]; char *q = x;
        const char *a = "-misc-fixed-medium-r-normal--";
        while (*a) *q++ = *a++;
        if (h >= 10) *q++ = (char)('0' + h / 10);
        *q++ = (char)('0' + h % 10); *q++ = '-';
        int pt = h * 10;
        if (pt >= 100) *q++ = (char)('0' + pt / 100);
        *q++ = (char)('0' + pt / 10 % 10); *q++ = '0'; *q++ = '-';
        a = "75-75-c-";
        while (*a) *q++ = *a++;
        int aw = w * 10;
        if (aw >= 100) *q++ = (char)('0' + aw / 100);
        *q++ = (char)('0' + aw / 10 % 10); *q++ = '0'; *q++ = '-';
        a = "iso8859-1";
        while (*a) *q++ = *a++;
        *q = 0;
        OFFER(x);
    }
    return n;
}
