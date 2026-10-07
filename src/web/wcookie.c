// Cookie jar (see wcookie.h).
#include "wcookie.h"
#include "wurl.h"
#include "wcommon.h"

#define MAX_COOKIES 400
#define MAX_NAME 256
#define MAX_VALUE 4096
#define MAX_HEADER_COOKIES 64

struct wck {
    char* name;
    char* value;
    char domain[128];     // lowercase, no leading dot
    char path[128];
    long long expires;    // epoch seconds, 0 = session
    uint8_t host_only, secure, http_only, same_site_none;
    uint32_t order;       // creation order (stable header ordering)
};

long long (*wcookie_now)(void);
static struct wck jar[MAX_COOKIES];
static int njar;
static uint32_t order_seq;

static long long now_s(void) { return wcookie_now ? wcookie_now() : 0; }

static void ck_free(struct wck* c) { w_free(c->name); w_free(c->value); c->name = c->value = 0; }

static void ck_del(int i) {
    ck_free(&jar[i]);
    jar[i] = jar[--njar];
}

static void lower_copy(char* out, int cap, const char* s, int n) {
    int k = 0;
    for (int i = 0; i < n && k < cap - 1; i++) out[k++] = (char)w_lower((unsigned char)s[i]);
    out[k] = 0;
}

// host domain-matches domain (RFC 6265 5.1.3)
static int domain_match(const char* host, const char* dom) {
    int hl = (int)strlen(host), dl = (int)strlen(dom);
    if (hl == dl) return !strcmp(host, dom);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, dom);
}

static int path_match(const char* req, const char* cp) {
    int cl = (int)strlen(cp);
    if (strncmp(req, cp, cl)) return 0;
    return req[cl] == 0 || cp[cl - 1] == '/' || req[cl] == '/' || req[cl] == '?';
}

// "site" = the last two labels (eTLD+1 approximation) for SameSite
static const char* site_of(const char* host) {
    const char* last = 0;
    const char* prev = 0;
    for (const char* p = host; *p; p++) if (*p == '.') { prev = last; last = p; }
    return prev ? prev + 1 : host;
}

static int is_ip(const char* h) {
    for (; *h; h++) if (!w_isdigit((unsigned char)*h) && *h != '.') return 0;
    return 1;
}

// "Thu, 01 Jan 1970 00:00:00 GMT" (and common variants) -> epoch, -1 bad
static long long parse_http_date(const char* s, int n) {
    static const char* const mon[12] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    int day = -1, month = -1, year = -1, hh = 0, mm = 0, ss = 0;
    int i = 0;
    while (i < n) {
        while (i < n && !w_isalnum((unsigned char)s[i])) i++;
        int st = i;
        while (i < n && (w_isalnum((unsigned char)s[i]) || s[i] == ':')) i++;
        int tl = i - st;
        if (!tl) break;
        const char* t = s + st;
        if (tl >= 5 && t[2] == ':') {
            hh = (t[0] - '0') * 10 + (t[1] - '0');
            mm = (t[3] - '0') * 10 + (t[4] - '0');
            if (tl >= 8 && t[5] == ':') ss = (t[6] - '0') * 10 + (t[7] - '0');
        } else if (w_isdigit((unsigned char)t[0])) {
            int v = 0;
            for (int k = 0; k < tl && w_isdigit((unsigned char)t[k]); k++) v = v * 10 + (t[k] - '0');
            if (tl >= 3) year = v; else if (day < 0) day = v; else year = v;
        } else if (tl >= 3) {
            for (int m = 0; m < 12; m++)
                if (w_lower((unsigned char)t[0]) == mon[m][0] && w_lower((unsigned char)t[1]) == mon[m][1] &&
                    w_lower((unsigned char)t[2]) == mon[m][2]) month = m;
        }
    }
    if (day < 1 || month < 0 || year < 0) return -1;
    if (year < 70) year += 2000; else if (year < 100) year += 1900;
    // days from civil
    long long y = year - (month < 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    int mp = (month + 9) % 12;
    long long doy = (153 * mp + 2) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

// host of a URL for cookie purposes (file: pages act as "localhost")
static int cookie_host(const char* url, char* host, int cap) {
    if (w_ieq_prefix(url, (int)strlen(url), "file:")) { memcpy(host, "localhost", 10); return 9; }
    return wurl_host(url, host, cap);
}

static void set_cookie(const char* url, const char* v, int vl, int from_http) {
    char host[128], path[512], scheme[16];
    if (cookie_host(url, host, sizeof host) <= 0) return;
    if (wurl_path(url, path, sizeof path) <= 0) { path[0] = '/'; path[1] = 0; }
    wurl_scheme(url, scheme, sizeof scheme);
    for (char* p = host; *p; p++) *p = (char)w_lower((unsigned char)*p);
    int secure_origin = !strcmp(scheme, "https") || !strcmp(host, "localhost") || is_ip(host);
    // name=value
    int semi = 0;
    while (semi < vl && v[semi] != ';') semi++;
    int eq = 0;
    while (eq < semi && v[eq] != '=') eq++;
    const char *name, *val;
    int nl, vlen;
    if (eq >= semi) { name = v; nl = 0; val = v; vlen = semi; }   // "value" alone: empty name
    else { name = v; nl = eq; val = v + eq + 1; vlen = semi - eq - 1; }
    while (nl > 0 && w_isspace((unsigned char)name[0])) { name++; nl--; }
    while (nl > 0 && w_isspace((unsigned char)name[nl - 1])) nl--;
    while (vlen > 0 && w_isspace((unsigned char)val[0])) { val++; vlen--; }
    while (vlen > 0 && w_isspace((unsigned char)val[vlen - 1])) vlen--;
    if (nl > MAX_NAME || vlen > MAX_VALUE || nl + vlen == 0) return;
    struct wck c;
    memset(&c, 0, sizeof c);
    c.host_only = 1;
    {
        int n = (int)strlen(host);
        if (n > 127) n = 127;
        memcpy(c.domain, host, n);
        c.domain[n] = 0;
    }
    // default path: the request path's directory
    {
        int last = 0;
        for (int i = 0; path[i] && path[i] != '?'; i++) if (path[i] == '/') last = i;
        int n = last > 0 ? last : 1;
        if (n > 127) n = 127;
        memcpy(c.path, path, n);
        c.path[n] = 0;
    }
    int max_age_set = 0, del = 0, same_site_set = 0;
    // attributes
    int i = semi;
    while (i < vl) {
        i++;
        int st = i;
        while (i < vl && v[i] != ';') i++;
        const char* a = v + st;
        int al = i - st;
        while (al > 0 && w_isspace((unsigned char)a[0])) { a++; al--; }
        while (al > 0 && w_isspace((unsigned char)a[al - 1])) al--;
        int ae = 0;
        while (ae < al && a[ae] != '=') ae++;
        const char* av = ae < al ? a + ae + 1 : a + al;
        int avl = ae < al ? al - ae - 1 : 0;
        while (avl > 0 && w_isspace((unsigned char)av[0])) { av++; avl--; }
        if (w_ieq(a, ae, "domain") && avl > 0) {
            if (av[0] == '.') { av++; avl--; }
            char d[128];
            lower_copy(d, sizeof d, av, avl);
            // must cover the request host and not be a bare TLD
            int dots = 0;
            for (char* p = d; *p; p++) if (*p == '.') dots++;
            if (!domain_match(host, d) || (!dots && strcmp(d, host))) return;
            memcpy(c.domain, d, strlen(d) + 1);
            c.host_only = 0;
        } else if (w_ieq(a, ae, "path") && avl > 0 && av[0] == '/') {
            int n = avl < 127 ? avl : 127;
            memcpy(c.path, av, n);
            c.path[n] = 0;
        } else if (w_ieq(a, ae, "max-age") && avl > 0) {
            int neg = av[0] == '-';
            long long s = 0;
            for (int k = neg; k < avl && w_isdigit((unsigned char)av[k]); k++) s = s * 10 + (av[k] - '0');
            max_age_set = 1;
            if (neg || s == 0) del = 1;
            else c.expires = now_s() ? now_s() + s : 0;
        } else if (w_ieq(a, ae, "expires") && avl > 0 && !max_age_set) {
            long long t = parse_http_date(av, avl);
            if (t >= 0) {
                if (now_s() ? t <= now_s() : t < 946684800LL) del = 1;   // past (or pre-2000 when no clock)
                else c.expires = t;
            }
        } else if (w_ieq(a, ae, "secure")) {
            c.secure = 1;
        } else if (w_ieq(a, ae, "httponly")) {
            c.http_only = 1;
        } else if (w_ieq(a, ae, "samesite")) {
            same_site_set = 1;
            c.same_site_none = w_ieq(av, avl, "none");
        }
    }
    (void)same_site_set;
    if (c.secure && !secure_origin) return;
    if (c.http_only && !from_http) return;
    // replace / delete an existing cookie with the same name+domain+path
    for (int k = 0; k < njar; k++) {
        struct wck* o = &jar[k];
        if ((int)strlen(o->name) == nl && !memcmp(o->name, name, nl) && !strcmp(o->domain, c.domain) &&
            !strcmp(o->path, c.path)) {
            if (o->http_only && !from_http) return;
            c.order = o->order;
            ck_del(k);
            break;
        }
    }
    if (del) return;
    if (njar >= MAX_COOKIES) {   // evict the oldest
        int old = 0;
        for (int k = 1; k < njar; k++) if (jar[k].order < jar[old].order) old = k;
        ck_del(old);
    }
    c.name = (char*)w_malloc(nl + 1);
    c.value = (char*)w_malloc(vlen + 1);
    if (!c.name || !c.value) { ck_free(&c); return; }
    memcpy(c.name, name, nl);
    c.name[nl] = 0;
    memcpy(c.value, val, vlen);
    c.value[vlen] = 0;
    if (!c.order) c.order = ++order_seq;
    jar[njar++] = c;
}

void wcookie_set_http(const char* url, const char* value, int vlen) { set_cookie(url, value, vlen, 1); }
void wcookie_set_doc(const char* url, const char* value, int vlen) { set_cookie(url, value, vlen, 0); }

static int collect(const char* url, const char* top_url, int for_http, char* out, int cap) {
    char host[128], path[512], scheme[16], top[128];
    if (cap > 0) out[0] = 0;
    if (cookie_host(url, host, sizeof host) <= 0) return 0;
    if (wurl_path(url, path, sizeof path) <= 0) { path[0] = '/'; path[1] = 0; }
    wurl_scheme(url, scheme, sizeof scheme);
    for (char* p = host; *p; p++) *p = (char)w_lower((unsigned char)*p);
    int https = !strcmp(scheme, "https");
    int cross_site = 0;
    if (top_url && wurl_host(top_url, top, sizeof top) > 0) {
        for (char* p = top; *p; p++) *p = (char)w_lower((unsigned char)*p);
        cross_site = strcmp(site_of(host), site_of(top)) != 0;
    }
    long long now = now_s();
    // expire
    for (int k = njar - 1; k >= 0; k--) if (jar[k].expires && now && jar[k].expires <= now) ck_del(k);
    // matching cookies, longer paths first, then creation order
    int pick[MAX_HEADER_COOKIES], np = 0;
    for (int k = 0; k < njar && np < MAX_HEADER_COOKIES; k++) {
        struct wck* c = &jar[k];
        if (c->host_only ? strcmp(host, c->domain) : !domain_match(host, c->domain)) continue;
        if (!path_match(path, c->path)) continue;
        if (c->secure && !https) continue;
        if (!for_http && c->http_only) continue;
        if (cross_site && !c->same_site_none) continue;
        pick[np++] = k;
    }
    for (int a = 1; a < np; a++) {
        int x = pick[a], b = a;
        while (b > 0) {
            struct wck* p = &jar[pick[b - 1]];
            struct wck* q = &jar[x];
            int pl = (int)strlen(p->path), ql = (int)strlen(q->path);
            if (pl > ql || (pl == ql && p->order <= q->order)) break;
            pick[b] = pick[b - 1];
            b--;
        }
        pick[b] = x;
    }
    int n = 0;
    for (int a = 0; a < np; a++) {
        struct wck* c = &jar[pick[a]];
        int nl = (int)strlen(c->name), vl = (int)strlen(c->value);
        int need = (n ? 2 : 0) + nl + (nl ? 1 : 0) + vl;
        if (n + need >= cap) break;
        if (n) { out[n++] = ';'; out[n++] = ' '; }
        memcpy(out + n, c->name, nl);
        n += nl;
        if (nl) out[n++] = '=';
        memcpy(out + n, c->value, vl);
        n += vl;
    }
    if (cap > 0) out[n] = 0;
    return n;
}

int wcookie_header(const char* url, const char* top_url, char* out, int cap) { return collect(url, top_url, 1, out, cap); }
int wcookie_doc(const char* url, char* out, int cap) { return collect(url, 0, 0, out, cap); }
int wcookie_count(void) { return njar; }
