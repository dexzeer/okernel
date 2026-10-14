// Preload scanner, see pscan.h.
#include "wcommon.h"
#include "pscan.h"
#include "wurl.h"

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// case-insensitive: does s[0..n) start with the lowercase literal?
static int starts_ci(const char* s, int n, const char* lit) {
    int i = 0;
    for (; lit[i]; i++) if (i >= n || lc((unsigned char)s[i]) != lit[i]) return 0;
    return 1;
}

// Find lowercase literal `lit` (case-insensitive) in s[from..n); returns its index or -1.
static int find_ci(const char* s, int n, int from, const char* lit) {
    int ll = (int)strlen(lit);
    for (int i = from; i + ll <= n; i++)
        if (starts_ci(s + i, n - i, lit)) return i;
    return -1;
}

struct attrs {
    const char* v[8];            // value pointers (into the html)
    int vl[8];
    int seen;                    // bit per attribute below
};
enum { A_HREF, A_SRC, A_REL, A_AS, A_MEDIA, A_LOADING, A_TYPE, A_NOMODULE };
static const char* const ANAMES[8] = { "href", "src", "rel", "as", "media", "loading", "type", "nomodule" };

// Parse the attributes of the tag whose name ends at i (s[i..e) up to '>'). Returns 1.
static void parse_attrs(const char* s, int i, int e, struct attrs* a) {
    memset(a, 0, sizeof *a);
    while (i < e) {
        while (i < e && (is_space((unsigned char)s[i]) || s[i] == '/')) i++;
        int ns = i;
        while (i < e && !is_space((unsigned char)s[i]) && s[i] != '=' && s[i] != '/' && s[i] != '>') i++;
        int nl = i - ns;
        if (nl <= 0) { i++; continue; }
        while (i < e && is_space((unsigned char)s[i])) i++;
        const char* vp = s + i;
        int vl = 0;
        if (i < e && s[i] == '=') {
            i++;
            while (i < e && is_space((unsigned char)s[i])) i++;
            if (i < e && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                vp = s + i;
                while (i < e && s[i] != q) i++;
                vl = (int)(s + i - vp);
                if (i < e) i++;
            } else {
                vp = s + i;
                while (i < e && !is_space((unsigned char)s[i]) && s[i] != '>') i++;
                vl = (int)(s + i - vp);
            }
        } else vp = s + ns + nl;
        for (int k = 0; k < 8; k++) {
            int al = (int)strlen(ANAMES[k]);
            if (al == nl && starts_ci(s + ns, nl, ANAMES[k]) && !(a->seen & (1 << k))) {
                a->seen |= 1 << k;
                a->v[k] = vp;
                a->vl[k] = vl;
            }
        }
    }
}

// Find the end of the tag starting at s[i] == '<': index of its '>' outside quotes, or -1 when the buffer ends first.
static int tag_end(const char* s, int n, int i) {
    char q = 0;
    for (int k = i + 1; k < n; k++) {
        char c = s[k];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') {
            // a quote only opens a value right after '=' (the apostrophe in `don't` inside a tag is rare, but be strict)
            int p = k - 1;
            while (p > i && is_space((unsigned char)s[p])) p--;
            if (s[p] == '=') q = c;
            continue;
        }
        if (c == '>') return k;
    }
    return -1;
}

// Token (space separated) present in a value?
static int has_token(const char* v, int vl, const char* tok) {
    int tl = (int)strlen(tok);
    for (int i = 0; i < vl;) {
        while (i < vl && is_space((unsigned char)v[i])) i++;
        int s = i;
        while (i < vl && !is_space((unsigned char)v[i])) i++;
        if (i - s == tl && starts_ci(v + s, tl, tok)) return 1;
    }
    return 0;
}

// &amp; -> & in a copy of the value; returns the length. Other entities are left alone.
static int unescape_url(const char* v, int vl, char* out, int cap) {
    int n = 0;
    for (int i = 0; i < vl && n < cap - 1; i++) {
        if (v[i] == '&' && starts_ci(v + i, vl - i, "&amp;")) { out[n++] = '&'; i += 4; continue; }
        out[n++] = v[i];
    }
    out[n] = 0;
    return n;
}

int pscan_html(const char* s, int n, int from, const char* base, pscan_emit_fn emit, void* ctx, int* next_from) {
    int i = from, emitted = 0;
    *next_from = from;
    while (i < n) {
        while (i < n && s[i] != '<') i++;
        if (i >= n) { *next_from = n; return emitted; }
        if (i + 1 >= n) { *next_from = i; return emitted; }
        if (s[i + 1] == '!' || s[i + 1] == '?') {
            if (starts_ci(s + i, n - i, "<!--")) {
                int e = find_ci(s, n, i + 4, "-->");
                if (e < 0) { *next_from = i; return emitted; }
                i = e + 3;
            } else {
                int e = tag_end(s, n, i);
                if (e < 0) { *next_from = i; return emitted; }
                i = e + 1;
            }
            *next_from = i;
            continue;
        }
        if (s[i + 1] == '/') {
            int e = tag_end(s, n, i);
            if (e < 0) { *next_from = i; return emitted; }
            i = e + 1;
            *next_from = i;
            continue;
        }
        int ns = i + 1, ne = ns;
        while (ne < n && !is_space((unsigned char)s[ne]) && s[ne] != '>' && s[ne] != '/') ne++;
        if (ne >= n) { *next_from = i; return emitted; }
        int e = tag_end(s, n, i);
        if (e < 0) { *next_from = i; return emitted; }
        int nl = ne - ns;
        struct attrs a;
        int kind = -1;
        const char* uv = 0;
        int ul = 0;
        if (nl == 4 && starts_ci(s + ns, nl, "base")) {
            return -1;
        } else if (nl == 4 && starts_ci(s + ns, nl, "link")) {
            parse_attrs(s, ne, e, &a);
            if ((a.seen & (1 << A_HREF)) && (a.seen & (1 << A_REL)) && a.vl[A_HREF] > 0) {
                int css = has_token(a.v[A_REL], a.vl[A_REL], "stylesheet") && !has_token(a.v[A_REL], a.vl[A_REL], "alternate");
                int pre = has_token(a.v[A_REL], a.vl[A_REL], "preload");
                int mpre = has_token(a.v[A_REL], a.vl[A_REL], "modulepreload");
                int media_print = (a.seen & (1 << A_MEDIA)) && (starts_ci(a.v[A_MEDIA], a.vl[A_MEDIA], "print") || starts_ci(a.v[A_MEDIA], a.vl[A_MEDIA], "speech"));
                uv = a.v[A_HREF]; ul = a.vl[A_HREF];
                if (css && !media_print) kind = PSCAN_CSS;
                else if (mpre) kind = PSCAN_SCRIPT;
                else if (pre && (a.seen & (1 << A_AS))) {
                    if (a.vl[A_AS] == 5 && starts_ci(a.v[A_AS], 5, "style")) kind = PSCAN_CSS;
                    else if (a.vl[A_AS] == 6 && starts_ci(a.v[A_AS], 6, "script")) kind = PSCAN_SCRIPT;
                    else if (a.vl[A_AS] == 5 && starts_ci(a.v[A_AS], 5, "image")) kind = PSCAN_IMG;
                }
            }
        } else if (nl == 6 && starts_ci(s + ns, nl, "script")) {
            parse_attrs(s, ne, e, &a);
            if ((a.seen & (1 << A_SRC)) && a.vl[A_SRC] > 0 && !(a.seen & (1 << A_NOMODULE))) { kind = PSCAN_SCRIPT; uv = a.v[A_SRC]; ul = a.vl[A_SRC]; }
            // the body of a script (or an empty one) must not be scanned as markup
            int close = find_ci(s, n, e + 1, "</script");
            if (close < 0) { *next_from = i; return emitted; }       // wait for the whole element
            int ce = tag_end(s, n, close);
            if (ce < 0) { *next_from = i; return emitted; }
            e = ce;
        } else if (nl == 5 && starts_ci(s + ns, nl, "style")) {
            int close = find_ci(s, n, e + 1, "</style");
            if (close < 0) { *next_from = i; return emitted; }
            int ce = tag_end(s, n, close);
            if (ce < 0) { *next_from = i; return emitted; }
            e = ce;
        } else if ((nl == 8 && starts_ci(s + ns, nl, "textarea")) || (nl == 5 && starts_ci(s + ns, nl, "title"))) {
            const char* lit = nl == 8 ? "</textarea" : "</title";
            int close = find_ci(s, n, e + 1, lit);
            if (close < 0) { *next_from = i; return emitted; }
            int ce = tag_end(s, n, close);
            if (ce < 0) { *next_from = i; return emitted; }
            e = ce;
        } else if (nl == 3 && starts_ci(s + ns, nl, "img")) {
            parse_attrs(s, ne, e, &a);
            if ((a.seen & (1 << A_SRC)) && a.vl[A_SRC] > 0 &&
                !((a.seen & (1 << A_LOADING)) && starts_ci(a.v[A_LOADING], a.vl[A_LOADING], "lazy")) &&
                !starts_ci(a.v[A_SRC], a.vl[A_SRC], "data:"))
                { kind = PSCAN_IMG; uv = a.v[A_SRC]; ul = a.vl[A_SRC]; }
        }
        if (kind >= 0) {
            char raw[1100], abs[1100];
            int rl = unescape_url(uv, ul < 1000 ? ul : 1000, raw, sizeof raw);
            if (rl > 0 && wurl_resolve(base, raw, rl, abs, sizeof abs) > 0) {
                emitted++;
                if (emit(ctx, abs, kind)) { *next_from = i; return emitted; }     // not taken: this tag is looked at again
            }
        }
        i = e + 1;
        *next_from = i;
    }
    return emitted;
}
