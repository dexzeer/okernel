#include "wurl.h"
#include "wcommon.h"

static int scheme_len(const char* s, int n) {
    if (n <= 0 || !w_isalpha((unsigned char)s[0])) return 0;
    for (int i = 1; i < n; i++) {
        char c = s[i];
        if (c == ':') return i;
        if (!(w_isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) return 0;
    }
    return 0;
}

// authority end (index after host[:port]) for a URL with "scheme://"
static int authority_end(const char* s, int n, int start) {
    int i = start;
    while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
    return i;
}

struct ob { char* p; int n, cap, bad; };
static void put(struct ob* o, const char* s, int n) {
    if (o->n + n >= o->cap) { o->bad = 1; return; }
    memcpy(o->p + o->n, s, n);
    o->n += n;
}

// remove_dot_segments over path p[0..n) appended to o
static void put_path(struct ob* o, const char* p, int n) {
    // work on a copy split into segments
    int start = o->n;
    int i = 0;
    while (i < n) {
        // "../" or "./" prefixes
        if (n - i >= 3 && p[i] == '.' && p[i + 1] == '.' && p[i + 2] == '/') { i += 3; continue; }
        if (n - i >= 2 && p[i] == '.' && p[i + 1] == '/') { i += 2; continue; }
        // "/./" or "/." at end
        if (n - i >= 3 && p[i] == '/' && p[i + 1] == '.' && p[i + 2] == '/') { i += 2; continue; }
        if (n - i == 2 && p[i] == '/' && p[i + 1] == '.') { put(o, "/", 1); i += 2; continue; }
        // "/../" or "/.." at end: pop last segment
        if ((n - i >= 4 && p[i] == '/' && p[i + 1] == '.' && p[i + 2] == '.' && p[i + 3] == '/') ||
            (n - i == 3 && p[i] == '/' && p[i + 1] == '.' && p[i + 2] == '.')) {
            int k = o->n;
            while (k > start && o->p[k - 1] != '/') k--;
            if (k > start) k--;
            o->n = k;
            if (n - i == 3) { put(o, "/", 1); i += 3; }
            else i += 3;
            continue;
        }
        if ((n - i == 1 && p[i] == '.') || (n - i == 2 && p[i] == '.' && p[i + 1] == '.')) { i = n; continue; }
        // copy one segment (leading '/' included)
        int j = i;
        if (p[j] == '/') j++;
        while (j < n && p[j] != '/') j++;
        put(o, p + i, j - i);
        i = j;
    }
}

int wurl_resolve(const char* base, const char* rel, int rlen, char* out, int cap) {
    while (rlen > 0 && w_isspace((unsigned char)rel[0])) { rel++; rlen--; }
    while (rlen > 0 && w_isspace((unsigned char)rel[rlen - 1])) rlen--;
    struct ob o = { out, 0, cap, 0 };
    int bn = base ? (int)strlen(base) : 0;
    int rs = scheme_len(rel, rlen);
    if (rs) {
        // absolute reference: still normalize the path for hierarchical URLs
        if (rs + 2 < rlen && rel[rs + 1] == '/' && rel[rs + 2] == '/') {
            int ae = authority_end(rel, rlen, rs + 3);
            for (int k = 0; k < rs; k++) { char c = (char)w_lower((unsigned char)rel[k]); put(&o, &c, 1); }
            put(&o, rel + rs, ae - rs);
            int qs = ae;
            while (qs < rlen && rel[qs] != '?' && rel[qs] != '#') qs++;
            if (ae == qs) put(&o, "/", 1);
            else put_path(&o, rel + ae, qs - ae);
            put(&o, rel + qs, rlen - qs);
        } else put(&o, rel, rlen);
        goto done;
    }
    int bs = scheme_len(base, bn);
    if (!bs) return -1;
    int has_auth = bs + 2 < bn && base[bs + 1] == '/' && base[bs + 2] == '/';
    int ae = has_auth ? authority_end(base, bn, bs + 3) : bs + 1;
    // base path range [ae, bq) and query
    int bq = ae;
    while (bq < bn && base[bq] != '?' && base[bq] != '#') bq++;
    int bf = bq;
    while (bf < bn && base[bf] != '#') bf++;
    if (rlen >= 2 && rel[0] == '/' && rel[1] == '/') {
        put(&o, base, bs + 1);
        int ae2 = authority_end(rel, rlen, 2);
        put(&o, rel, ae2);
        int qs = ae2;
        while (qs < rlen && rel[qs] != '?' && rel[qs] != '#') qs++;
        if (ae2 == qs) put(&o, "/", 1);
        else put_path(&o, rel + ae2, qs - ae2);
        put(&o, rel + qs, rlen - qs);
        goto done;
    }
    put(&o, base, ae);
    if (rlen == 0) { put(&o, base + ae, bf - ae); goto done; }
    if (rel[0] == '#') { put(&o, base + ae, bf - ae); put(&o, rel, rlen); goto done; }
    if (rel[0] == '?') {
        if (bq == ae && has_auth) put(&o, "/", 1);
        else put(&o, base + ae, bq - ae);
        put(&o, rel, rlen);
        goto done;
    }
    int qs = 0;
    while (qs < rlen && rel[qs] != '?' && rel[qs] != '#') qs++;
    if (rel[0] == '/') {
        put_path(&o, rel, qs);
    } else {
        // merge: base path up to the last '/', then rel path
        int ls = -1;
        for (int i = ae; i < bq; i++) if (base[i] == '/') ls = i;
        char tmp[2048];
        int tn = 0;
        if (ls < 0) tmp[tn++] = '/';
        else { int l = ls - ae + 1; if (l > 1024) l = 1024; memcpy(tmp, base + ae, l); tn = l; }
        int rl = qs < 1000 ? qs : 1000;
        memcpy(tmp + tn, rel, rl);
        tn += rl;
        put_path(&o, tmp, tn);
    }
    put(&o, rel + qs, rlen - qs);
done:
    if (o.bad || o.n >= cap) { if (cap > 0) out[0] = 0; return -1; }
    out[o.n] = 0;
    return o.n;
}

int wurl_scheme(const char* url, char* out, int cap) {
    int n = (int)strlen(url), s = scheme_len(url, n);
    if (!s || s >= cap) { if (cap) out[0] = 0; return 0; }
    for (int i = 0; i < s; i++) out[i] = (char)w_lower((unsigned char)url[i]);
    out[s] = 0;
    return s;
}

static int host_range(const char* url, int* hs, int* he) {
    int n = (int)strlen(url), s = scheme_len(url, n);
    if (!s || s + 2 >= n || url[s + 1] != '/' || url[s + 2] != '/') return 0;
    int a = s + 3, e = authority_end(url, n, a);
    for (int i = a; i < e; i++) if (url[i] == '@') a = i + 1; // userinfo
    int h = a;
    if (h < e && url[h] == '[') { while (h < e && url[h] != ']') h++; if (h < e) h++; }
    else while (h < e && url[h] != ':') h++;
    *hs = a;
    *he = h;
    return 1;
}

int wurl_host(const char* url, char* out, int cap) {
    int hs, he;
    if (!host_range(url, &hs, &he) || he - hs >= cap) { if (cap) out[0] = 0; return 0; }
    for (int i = hs; i < he; i++) out[i - hs] = (char)w_lower((unsigned char)url[i]);
    out[he - hs] = 0;
    return he - hs;
}

int wurl_port(const char* url) {
    int hs, he;
    if (!host_range(url, &hs, &he) || url[he] != ':') return 0;
    int p = 0;
    for (int i = he + 1; w_isdigit((unsigned char)url[i]); i++) p = p * 10 + (url[i] - '0');
    return p;
}

int wurl_path(const char* url, char* out, int cap) {
    int n = (int)strlen(url), s = scheme_len(url, n);
    int st = 0;
    if (s && s + 2 < n && url[s + 1] == '/' && url[s + 2] == '/') st = authority_end(url, n, s + 3);
    int e = st;
    while (e < n && url[e] != '#') e++;
    int o = 0;
    if (st == e || url[st] != '/') { if (o < cap - 1) out[o++] = '/'; }
    for (int i = st; i < e && o < cap - 1; i++) out[o++] = url[i];
    out[o] = 0;
    return o;
}
