// Page scripting: one ojs realm (src/ojs, our own engine) per document.
//
//  * The Web API lives in JS (wjs_prelude.js, embedded) on top of the C
//    natives here and in wjs_dom.c.
//  * Script scheduling follows the HTML spec's shape on a pre-parsed tree:
//    parser-inserted classic scripts run in document order (fetching
//    external ones first), then defer scripts and module scripts in order,
//    then DOMContentLoaded; async/dynamic scripts run as they arrive; the
//    load event fires once scripts, stylesheets and images have settled.
//  * Module graphs are fetched ahead of evaluation (static imports are
//    found by compiling each fetched module) because loading is synchronous.
//  * Every entry from the shell sets a time budget (interrupt handler),
//    the x87 control word (53-bit doubles) and the collector's stack top
//    (ojs_enter in the entry function itself, so its locals are scanned).

#include "wjs_int.h"
#include "wurl.h"
#include "css.h"
#include "wcookie.h"

#if !(defined(KERNEL) && KERNEL)
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#endif

extern const char wjs_prelude_src[];
extern const uint32_t wjs_prelude_len;
void wjs_selq_forget(struct wdom* d);

#define SCRIPT_BUDGET_MS 4000   // one script evaluation (the whole OS waits: keep it short)
#define TASK_BUDGET_MS   1000   // one timer / event / callback
#define REALM_STACK_MAX  (900u << 10)
#define MAX_SCRIPTS      256
#define MAX_MODULES      512

struct wjs_host wjs_host;

// ---- host services ----------------------------------------------------------------

int wjs_now(void) {
    if (wjs_host.now_ms) return wjs_host.now_ms();
#if !(defined(KERNEL) && KERNEL)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#else
    return 0;
#endif
}

static void fallback_random(uint8_t* out, int n) {
    static uint32_t x = 0x9E3779B9u;
    x ^= (uint32_t)wjs_now() * 2654435761u;
    for (int i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = (uint8_t)(x >> 11);
    }
}

void wjs_logf(struct wjs* js, int level, const char* s, int len) {
    if (js) {
        if (js->nlog == WJS_LOG_MAX) {
            if (wjs_host.log) wjs_host.log("[js] (further console output suppressed)\n");
            else w_log("[js] (further console output suppressed)\n");
        }
        if (js->nlog++ >= WJS_LOG_MAX) return;
    }
    static const char* const tag[4] = { "[js] ", "[js] ", "[js] warn: ", "[js] error: " };
    char buf[600];
    const char* t = tag[level < 0 ? 0 : level > 3 ? 3 : level];
    int n = 0;
    while (*t && n < 40) buf[n++] = *t++;
    for (int i = 0; i < len && n < (int)sizeof buf - 2; i++) {
        unsigned char c = (unsigned char)s[i];
        buf[n++] = (c == '\n' && i + 1 < len) ? '\n' : (c < 32 && c != '\n') ? ' ' : (char)c;
    }
    if (len > (int)sizeof buf - 50) { buf[n - 3] = '.'; buf[n - 2] = '.'; buf[n - 1] = '.'; }
    buf[n++] = '\n';
    buf[n] = 0;
    if (wjs_host.log) wjs_host.log(buf);
    else w_log("%s", buf);
}

// ---- realm entry guard ------------------------------------------------------------------

static int enter_ok(struct wjs* js) {
    if (!js || js->dead || !js->J) return 0;
#if defined(__i386__)
    uint16_t cw = 0x027F;   // x87: 53-bit precision (IEEE doubles), exceptions masked
    __asm__ volatile("fldcw %0" :: "m"(cw));
#endif
    return 1;
}

// GUARD / UNGUARD bracket every entry from the shell: ojs_enter must expand
// in the entry function so the collector scans that function's locals
#define GUARD(js, ret)                       \
    do {                                     \
        if (!enter_ok(js)) return ret;       \
        ojs_enter((js)->J);                  \
    } while (0)
#define UNGUARD(js) ojs_leave((js)->J)

#if !(defined(KERNEL) && KERNEL)
// host diagnostics: OJS_JSPROF=1 samples the running JS function at every interrupt
// check (every few thousand loop iterations / calls) and prints the hottest at exit
#define PROF_N 4096
static struct { char where[120]; unsigned n; } prof[PROF_N];
static unsigned prof_total;
static void prof_dump(void) {
    for (int r = 0; r < 30; r++) {
        int best = -1;
        for (int i = 0; i < PROF_N; i++) if (prof[i].n && (best < 0 || prof[i].n > prof[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "[prof] %5.1f%% %s\n", 100.0 * prof[best].n / (prof_total ? prof_total : 1), prof[best].where);
        prof[best].n = 0;
    }
}
static void prof_sample(ojs* J) {
    char w[120];
    if (!ojs_where(J, w, sizeof w)) return;
    unsigned h = 5381;
    for (char* p = w; *p; p++) h = h * 33 + (unsigned char)*p;
    for (unsigned k = 0; k < PROF_N; k++) {
        unsigned i = (h + k) % PROF_N;
        if (!prof[i].n) { memcpy(prof[i].where, w, strlen(w) + 1); prof[i].n = 1; break; }
        if (!strcmp(prof[i].where, w)) { prof[i].n++; break; }
    }
    if (++prof_total == 1) atexit(prof_dump);
}
#endif

static int interrupt_cb(ojs* J, void* op) {
    (void)J;
    struct wjs* js = (struct wjs*)op;
#if !(defined(KERNEL) && KERNEL)
    static int prof_on = -1;
    if (prof_on < 0) prof_on = getenv("OJS_JSPROF") != 0;
    if (prof_on) prof_sample(J);
    static int nobudget = -1;   // host diagnostics: OJS_NOBUDGET=1 lets scripts run to completion
    if (nobudget < 0) nobudget = getenv("OJS_NOBUDGET") != 0;
    if (nobudget) return 0;
#endif
    if (js->deadline && wjs_now() > js->deadline) { js->interrupted = 1; return 1; }
    return 0;
}

void wjs_drain_jobs(struct wjs* js) {
    for (int i = 0; i < 100000; i++) {
        int r = ojs_run_job(js->J);
        if (r == 0) break;
        if (r < 0) wjs_report_exception(js, "promise job");
        if (js->interrupted) break;
    }
}

void wjs_report_exception(struct wjs* js, const char* where) {
    ojs* ctx = js->J;
    ojsv e = ojs_take_exception(ctx);
    js->nerrors++;
    if (js->interrupted) {
        char m[160];
        int n = 0;
        const char* a = "script interrupted: time budget exceeded in ";
        while (*a) m[n++] = *a++;
        for (const char* b = where; *b && n < 150; b++) m[n++] = *b;
        wjs_logf(js, 3, m, n);
        js->interrupted = 0;
        // where it was stopped (the error's stack: the busy loop is in the first frames)
        if (ojs_is_object(e)) {
            ojsv st = ojs_get(ctx, e, "stack");
            char* s = ojs_is_string(st) ? ojs_to_cstring(ctx, st, 0) : 0;
            if (s) {
                int sl = (int)strlen(s);
                wjs_logf(js, 3, s, sl > 600 ? 600 : sl);
                ojs_free_cstring(ctx, s);
            } else ojs_take_exception(ctx);
        }
        return;
    }
    ojsv args[2] = { e, ojs_string(ctx, where) };
    ojsv r = ojs_is_undefined(js->h_report) ? OJS_EXCEPTION : ojs_call(ctx, js->h_report, OJS_UNDEFINED, 2, args);
    if (ojs_is_exception(r)) {
        ojs_take_exception(ctx);
        char* s = ojs_describe(ctx, e);
        if (s) { wjs_logf(js, 3, s, (int)strlen(s)); ojs_free_cstring(ctx, s); }
    }
}

static void rejection_cb(ojs* ctx, ojsv promise, ojsv reason, int handled, void* op) {
    (void)promise;
    struct wjs* js = (struct wjs*)op;
    if (handled) return;
    char m[400];
    int n = 0;
    const char* a = "Uncaught (in promise) ";
    while (*a) m[n++] = *a++;
    char* s = ojs_describe(ctx, reason);   // errors: "Name: message" + stack
    if (s) { for (const char* b = s; *b && n < 398; b++) m[n++] = *b; ojs_free_cstring(ctx, s); }
    js->nerrors++;
    wjs_logf(js, 3, m, n);
}

// call a hook with the guard already held
static ojsv call_hook(struct wjs* js, ojsv fn, int argc, ojsv* argv, int budget) {
    if (budget) js->deadline = wjs_now() + budget;
    ojsv r = ojs_call(js->J, fn, OJS_UNDEFINED, argc, argv);
    if (ojs_is_exception(r)) wjs_report_exception(js, "callback");
    wjs_drain_jobs(js);
    js->deadline = 0;
    return r;
}

// ---- natives: environment ------------------------------------------------------------

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)

NATIVE(n_log) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    int32_t lvl = 1;
    if (argc > 0) ojs_to_int32(ctx, &lvl, argv[0]);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    if (lvl >= 3) js->nerrors++;
    wjs_logf(js, lvl, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    return OJS_UNDEFINED;
}
NATIVE(n_now) {
    (void)this_val; (void)argc; (void)argv;
    struct wjs* js = JSX(ctx);
    return ojs_int(wjs_now() - js->t0);
}
NATIVE(n_resolve) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    const char* base = wdoc_base(js->doc);
    const char* b2 = 0;
    if (argc > 1 && !ojs_is_null(argv[1]) && !ojs_is_undefined(argv[1])) {
        b2 = ojs_to_cstring(ctx, argv[1], 0);
        if (!b2) { ojs_free_cstring(ctx, (char*)s); return OJS_EXCEPTION; }
        base = b2;
    }
    char out[4096];
    // trim ASCII whitespace like the URL parser
    const char* p = s;
    int pl = (int)l;
    while (pl > 0 && (unsigned char)p[0] <= 32) { p++; pl--; }
    while (pl > 0 && (unsigned char)p[pl - 1] <= 32) pl--;
    ojsv r = OJS_NULL;
    if (w_ieq_prefix(p, pl, "data:") || w_ieq_prefix(p, pl, "blob:") || w_ieq_prefix(p, pl, "javascript:") ||
        w_ieq_prefix(p, pl, "about:") || w_ieq_prefix(p, pl, "mailto:") || w_ieq_prefix(p, pl, "tel:"))
        r = ojs_string_len(ctx, p, pl);
    else if (wurl_resolve(base, p, pl, out, sizeof out) > 0)
        r = ojs_string(ctx, out);
    if (b2) ojs_free_cstring(ctx, (char*)b2);
    ojs_free_cstring(ctx, (char*)s);
    return r;
}
NATIVE(n_doc_url) { (void)this_val; (void)argc; (void)argv; return ojs_string(ctx, wdoc_url(JSX(ctx)->doc)); }
NATIVE(n_base_url) { (void)this_val; (void)argc; (void)argv; return ojs_string(ctx, wdoc_base(JSX(ctx)->doc)); }
NATIVE(n_referrer) { (void)this_val; (void)argc; (void)argv; return ojs_string(ctx, ""); }
NATIVE(n_navigate) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, 0);
    if (!s) return OJS_EXCEPTION;
    w_free(js->nav_url);
    int n = (int)strlen(s);
    js->nav_url = (char*)w_malloc(n + 1);
    if (js->nav_url) memcpy(js->nav_url, s, n + 1);
    js->nav_replace = argc > 1 && ojs_to_bool(ctx, argv[1]);
    js->flags |= WJS_NAV;
    ojs_free_cstring(ctx, (char*)s);
    return OJS_UNDEFINED;
}
NATIVE(n_push_url) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, 0);
    if (!s) return OJS_EXCEPTION;
    wdoc_set_url(js->doc, s);
    js->flags |= WJS_URL;
    ojs_free_cstring(ctx, (char*)s);
    return OJS_UNDEFINED;
}
NATIVE(n_history_go) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    int32_t n = 0;
    if (argc > 0) ojs_to_int32(ctx, &n, argv[0]);
    js->hist_delta = n;
    js->flags |= WJS_NAV;
    return OJS_UNDEFINED;
}
NATIVE(n_fetch) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    const char* url = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, 0);
    const char* method = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, 0);
    const char* headers = ojs_to_cstring(ctx, argc > 2 ? argv[2] : OJS_UNDEFINED, 0);
    size_t bl = 0;
    const char* body = (argc > 3 && !ojs_is_null(argv[3]) && !ojs_is_undefined(argv[3])) ? ojs_to_cstring(ctx, argv[3], &bl) : 0;
    int id = -1;
    if (url && method && headers) id = wdoc_res_request(js->doc, url, method, headers, body, (int)bl);
    if (id >= 0) js->flags |= WJS_FETCH;
    if (url) ojs_free_cstring(ctx, (char*)url);
    if (method) ojs_free_cstring(ctx, (char*)method);
    if (headers) ojs_free_cstring(ctx, (char*)headers);
    if (body) ojs_free_cstring(ctx, (char*)body);
    return ojs_int(id);
}
NATIVE(n_store) {
    (void)this_val;
    struct wjs* js = JSX(ctx);
    int32_t area = 0, op = 0;
    if (argc > 0) ojs_to_int32(ctx, &area, argv[0]);
    if (argc > 1) ojs_to_int32(ctx, &op, argv[1]);
    return wjs_store_op(js, area, op, argv + 2, argc > 2 ? argc - 2 : 0);
}
NATIVE(n_cookie) {
    (void)this_val;
    return wjs_cookie_op(JSX(ctx), argc > 0 ? argv[0] : OJS_UNDEFINED, argc > 0);
}
NATIVE(n_random) {
    (void)this_val;
    if (argc < 1) return OJS_UNDEFINED;
    size_t len;
    uint8_t* p = ojs_bytes(ctx, argv[0], &len);
    if (p) {
        if (len > 65536) len = 65536;
        if (wjs_host.random) wjs_host.random(p, (int)len);
        else fallback_random(p, (int)len);
    }
    return OJS_UNDEFINED;
}
NATIVE(n_utf8enc) {
    (void)this_val;
    size_t l;
    char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    ojsv u8 = ojs_uint8array_copy(ctx, s, l);
    ojs_free_cstring(ctx, s);
    return u8;
}
NATIVE(n_utf8dec) {
    (void)this_val;
    if (argc < 1) return ojs_string(ctx, "");
    size_t len;
    const char* s = (const char*)ojs_bytes(ctx, argv[0], &len);
    if (!s) return ojs_string(ctx, "");
    if (len >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) { s += 3; len -= 3; }
    return ojs_string_len(ctx, s, len);
}

static const struct ojs_func_entry env_funcs[] = {
    { "log", n_log, 2 },
    { "now", n_now, 0 },
    { "resolve", n_resolve, 2 },
    { "docURL", n_doc_url, 0 },
    { "baseURL", n_base_url, 0 },
    { "referrer", n_referrer, 0 },
    { "navigate", n_navigate, 2 },
    { "pushURL", n_push_url, 2 },
    { "historyGo", n_history_go, 1 },
    { "fetch", n_fetch, 4 },
    { "store", n_store, 4 },
    { "cookie", n_cookie, 1 },
    { "random", n_random, 1 },
    { "utf8enc", n_utf8enc, 1 },
    { "utf8dec", n_utf8dec, 1 },
};

// ---- storage (per origin, shared by every tab of the session) ---------------------------

struct kv { char* origin; char* key; char* val; uint8_t area; };
static struct kv* kv_tab;
static int kv_n, kv_cap;
static int kv_bytes;
#define KV_MAX_BYTES (4 << 20)

static void origin_of(struct wjs* js, char* out, int cap) {
    const char* u = wdoc_url(js->doc);
    int i = 0, slashes = 0;
    for (; u[i] && i < cap - 1; i++) {
        if (u[i] == '/' && ++slashes == 3) break;
        out[i] = u[i];
    }
    out[i] = 0;
}

static char* dupn(const char* s, int n) {
    char* p = (char*)w_malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

static int kv_find(const char* origin, int area, const char* key) {
    for (int i = 0; i < kv_n; i++)
        if (kv_tab[i].area == area && !strcmp(kv_tab[i].origin, origin) && !strcmp(kv_tab[i].key, key)) return i;
    return -1;
}

static void kv_del(int i) {
    kv_bytes -= (int)(strlen(kv_tab[i].key) + strlen(kv_tab[i].val));
    w_free(kv_tab[i].origin); w_free(kv_tab[i].key); w_free(kv_tab[i].val);
    kv_tab[i] = kv_tab[--kv_n];
}

ojsv wjs_store_op(struct wjs* js, int area, int op, ojsv* argv, int argc) {
    ojs* ctx = js->J;
    char origin[256];
    origin_of(js, origin, sizeof origin);
    if (op == 5 || op == 4) {   // length / key(i)
        int count = 0;
        int32_t want = -1;
        if (op == 4 && argc > 0) ojs_to_int32(ctx, &want, argv[0]);
        for (int i = 0; i < kv_n; i++)
            if (kv_tab[i].area == area && !strcmp(kv_tab[i].origin, origin)) {
                if (op == 4 && count == want) return ojs_string(ctx, kv_tab[i].key);
                count++;
            }
        return op == 5 ? ojs_int(count) : OJS_NULL;
    }
    if (op == 3) {   // clear
        for (int i = kv_n - 1; i >= 0; i--)
            if (kv_tab[i].area == area && !strcmp(kv_tab[i].origin, origin)) kv_del(i);
        return OJS_UNDEFINED;
    }
    const char* key = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, 0);
    if (!key) return OJS_EXCEPTION;
    int i = kv_find(origin, area, key);
    ojsv r = OJS_UNDEFINED;
    if (op == 0) r = i >= 0 ? ojs_string(ctx, kv_tab[i].val) : OJS_NULL;
    else if (op == 2) { if (i >= 0) kv_del(i); }
    else if (op == 1) {
        size_t vl;
        const char* v = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &vl);
        if (!v) { ojs_free_cstring(ctx, (char*)key); return OJS_EXCEPTION; }
        int kl = (int)strlen(key);
        int grow = (int)vl + (i >= 0 ? -(int)strlen(kv_tab[i].val) : kl);
        if (kv_bytes + grow > KV_MAX_BYTES) {
            ojs_free_cstring(ctx, (char*)v);
            ojs_free_cstring(ctx, (char*)key);
            return ojs_throw_type_error(ctx, "QuotaExceededError: storage is full");
        }
        if (i >= 0) {
            char* nv = dupn(v, (int)vl);
            if (nv) { kv_bytes += grow; w_free(kv_tab[i].val); kv_tab[i].val = nv; }
        } else {
            if (kv_n >= kv_cap) {
                int nc = kv_cap ? kv_cap * 2 : 64;
                struct kv* t = (struct kv*)w_realloc(kv_tab, nc * sizeof(struct kv));
                if (t) { kv_tab = t; kv_cap = nc; }
            }
            if (kv_n < kv_cap) {
                struct kv* e = &kv_tab[kv_n];
                e->origin = dupn(origin, (int)strlen(origin));
                e->key = dupn(key, kl);
                e->val = dupn(v, (int)vl);
                e->area = (uint8_t)area;
                if (e->origin && e->key && e->val) { kv_n++; kv_bytes += grow; }
                else { w_free(e->origin); w_free(e->key); w_free(e->val); }
            }
        }
        ojs_free_cstring(ctx, (char*)v);
    }
    ojs_free_cstring(ctx, (char*)key);
    return r;
}

// document.cookie: the shared jar (src/web/wcookie.c) the HTTP fetcher
// also uses, so script-set cookies reach the server and vice versa.
ojsv wjs_cookie_op(struct wjs* js, ojsv setv, int set) {
    ojs* ctx = js->J;
    if (!set) {
        static char buf[8192];
        int n = wcookie_doc(wdoc_url(js->doc), buf, sizeof buf);
        return ojs_string_len(ctx, buf, n);
    }
    size_t l;
    const char* s = ojs_to_cstring(ctx, setv, &l);
    if (!s) return OJS_EXCEPTION;
    wcookie_set_doc(wdoc_url(js->doc), s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    return OJS_UNDEFINED;
}

// ---- modules -----------------------------------------------------------------------------

static int mod_find(struct wjs* js, const char* url) {
    for (int i = 0; i < js->nmod; i++) if (!strcmp(js->mods[i].url, url)) return i;
    return -1;
}

static int mod_add(struct wjs* js, const char* url, int dynamic_only) {
    int i = mod_find(js, url);
    if (i >= 0) {
        if (!dynamic_only && js->mods[i].scanned == 2) js->mods[i].scanned = 0;   // now a static dep
        return i;
    }
    if (js->nmod >= MAX_MODULES) return -1;
    if (js->nmod >= js->capmod) {
        int nc = js->capmod ? js->capmod * 2 : 16;
        struct wjs_mod* m = (struct wjs_mod*)w_realloc(js->mods, nc * sizeof(struct wjs_mod));
        if (!m) return -1;
        js->mods = m;
        js->capmod = nc;
    }
    struct wjs_mod* m = &js->mods[js->nmod];
    memset(m, 0, sizeof *m);
    m->url = dupn(url, (int)strlen(url));
    if (!m->url) return -1;
    m->state = SS_FETCH;
    m->scanned = dynamic_only ? 2 : 0;   // 2 = only reachable through import()
    m->res = wdoc_res_script(js->doc, url);
    if (m->res < 0) m->state = SS_FAILED;
    else js->flags |= WJS_FETCH;
    return js->nmod++;
}

// import map lookup ("imports": exact keys, then the longest "prefix/" key)
static int importmap_resolve(struct wjs* js, const char* spec, char* out, int cap) {
    if (!js->importmap || !js->J) return 0;
    ojs* ctx = js->J;
    ojsv m = ojs_parse_json(ctx, js->importmap, strlen(js->importmap));
    if (ojs_is_exception(m)) { ojs_take_exception(ctx); return 0; }
    if (!ojs_is_object(m)) return 0;
    ojsv imports = ojs_get(ctx, m, "imports");
    if (ojs_is_exception(imports)) { ojs_take_exception(ctx); return 0; }
    int ok = 0;
    if (ojs_is_object(imports)) {
        ojsv v = ojs_get(ctx, imports, spec);
        if (ojs_is_exception(v)) { ojs_take_exception(ctx); return 0; }
        if (ojs_is_string(v)) {
            char* t = ojs_to_cstring(ctx, v, 0);
            if (t) { ok = wurl_resolve(wdoc_base(js->doc), t, (int)strlen(t), out, cap) > 0; ojs_free_cstring(ctx, t); }
        } else {
            ojsv keys = ojs_own_keys(ctx, imports);
            int best = 0;
            ojsv nk = ojs_is_exception(keys) ? OJS_EXCEPTION : ojs_get(ctx, keys, "length");
            int32_t np = 0;
            if (!ojs_is_exception(nk)) ojs_to_int32(ctx, &np, nk);
            for (int32_t k = 0; k < np; k++) {
                ojsv kv = ojs_get_index(ctx, keys, (uint32_t)k);
                char* key = ojs_is_exception(kv) ? 0 : ojs_to_cstring(ctx, kv, 0);
                int kl = key ? (int)strlen(key) : 0;
                if (key && kl > best && key[kl - 1] == '/' && !strncmp(spec, key, kl)) {
                    ojsv pv = ojs_get(ctx, imports, key);
                    char* t = ojs_is_exception(pv) ? 0 : ojs_to_cstring(ctx, pv, 0);
                    if (t) {
                        char tmp[2048];
                        int tl = (int)strlen(t);
                        int rest = (int)strlen(spec) - kl;
                        if (tl + rest < (int)sizeof tmp) {
                            memcpy(tmp, t, tl);
                            memcpy(tmp + tl, spec + kl, rest);
                            tmp[tl + rest] = 0;
                            if (wurl_resolve(wdoc_base(js->doc), tmp, tl + rest, out, cap) > 0) { ok = 1; best = kl; }
                        }
                        ojs_free_cstring(ctx, t);
                    }
                }
                if (key) ojs_free_cstring(ctx, key);
            }
            if (ojs_has_exception(ctx)) ojs_take_exception(ctx);
        }
    }
    return ok;
}

static int resolve_spec(struct wjs* js, const char* base, const char* spec, int sl, char* out, int cap) {
    char tmp[2048];
    if (sl <= 0 || sl >= (int)sizeof tmp) return 0;
    memcpy(tmp, spec, sl);
    tmp[sl] = 0;
    if (importmap_resolve(js, tmp, out, cap)) return 1;
    int rel = tmp[0] == '/' || (tmp[0] == '.' && (tmp[1] == '/' || (tmp[1] == '.' && tmp[2] == '/')));
    int abs = w_ieq_prefix(tmp, sl, "http:") || w_ieq_prefix(tmp, sl, "https:");
    if (!rel && !abs) return 0;   // bare specifier without an import map entry
    return wurl_resolve(base, tmp, sl, out, cap) > 0;
}

// Light JS scanner: static `import ... from "x"`, `import "x"`,
// `export ... from "x"` and literal `import("x")`. Skips comments, strings,
// templates and (heuristically) regex literals.
typedef void (*import_cb)(struct wjs* js, const char* base, const char* spec, int len, int dynamic);

static int skip_ws_comments(const char* s, int n, int i) {
    for (;;) {
        while (i < n && (w_isspace((unsigned char)s[i]) || s[i] == '\v')) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        return i;
    }
}

static int skip_string(const char* s, int n, int i) {
    char q = s[i++];
    while (i < n && s[i] != q) { if (s[i] == '\\') i++; else if (s[i] == '\n' && q != '`') break; i++; }
    return i + 1;
}

static int is_id(int c) { return w_isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

static void scan_imports(struct wjs* js, const char* s, int n, const char* base, import_cb cb) {
    int i = 0;
    int regex_ok = 1;   // a '/' here starts a regex literal
    while (i < n) {
        int c = (unsigned char)s[i];
        if (c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) { i = skip_ws_comments(s, n, i); continue; }
        if (c == '\'' || c == '"' || c == '`') { i = skip_string(s, n, i); regex_ok = 0; continue; }
        if (c == '/') {
            if (regex_ok) {
                i++;
                int cls = 0;
                while (i < n && s[i] != '\n') {
                    if (s[i] == '\\') { i += 2; continue; }
                    if (s[i] == '[') cls = 1;
                    else if (s[i] == ']') cls = 0;
                    else if (s[i] == '/' && !cls) break;
                    i++;
                }
                i++;
                while (i < n && is_id((unsigned char)s[i])) i++;
                regex_ok = 0;
            } else { i++; regex_ok = 1; }
            continue;
        }
        if (is_id(c) && !w_isdigit(c)) {
            int st = i;
            while (i < n && is_id((unsigned char)s[i])) i++;
            int wl = i - st;
            const char* w = s + st;
            int dot_before = 0;
            for (int k = st - 1; k >= 0; k--) { if (w_isspace((unsigned char)s[k])) continue; dot_before = s[k] == '.'; break; }
            if (!dot_before && wl == 6 && !memcmp(w, "import", 6)) {
                int j = skip_ws_comments(s, n, i);
                if (j < n && s[j] == '(') {
                    j = skip_ws_comments(s, n, j + 1);
                    if (j < n && (s[j] == '"' || s[j] == '\'')) {
                        int e = skip_string(s, n, j);
                        cb(js, base, s + j + 1, e - j - 2, 1);
                    }
                } else if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    int e = skip_string(s, n, j);
                    cb(js, base, s + j + 1, e - j - 2, 0);
                    i = e;
                } else if (j < n && (s[j] == '*' || s[j] == '{' || (is_id((unsigned char)s[j]) && !w_isdigit((unsigned char)s[j])))) {
                    // import <bindings> from "x" (binding lists can be huge)
                    int k = j, lim = n;
                    while (k < lim) {
                        if (s[k] == ';') break;
                        if (s[k] == '"' || s[k] == '\'') {
                            int e = skip_string(s, n, k);
                            // preceded by `from`?
                            int b = k - 1;
                            while (b > j && w_isspace((unsigned char)s[b])) b--;
                            if (b >= j + 3 && !memcmp(s + b - 3, "from", 4)) { cb(js, base, s + k + 1, e - k - 2, 0); i = e; }
                            break;
                        }
                        k++;
                    }
                }
            } else if (!dot_before && wl == 6 && !memcmp(w, "export", 6)) {
                int j = skip_ws_comments(s, n, i);
                if (j < n && (s[j] == '*' || s[j] == '{')) {
                    int k = j, depth = 0, lim = n;
                    while (k < lim) {
                        if (s[k] == '{') depth++;
                        else if (s[k] == '}') depth--;
                        else if (s[k] == ';' && depth <= 0) break;
                        else if ((s[k] == '"' || s[k] == '\'') && depth <= 0) {
                            int e = skip_string(s, n, k);
                            int b = k - 1;
                            while (b > j && w_isspace((unsigned char)s[b])) b--;
                            if (b >= j + 3 && !memcmp(s + b - 3, "from", 4)) { cb(js, base, s + k + 1, e - k - 2, 0); i = e; }
                            break;
                        }
                        k++;
                    }
                }
            }
            // keywords after which a '/' starts a regex
            regex_ok = (wl == 6 && (!memcmp(w, "return", 6) || !memcmp(w, "typeof", 6) || !memcmp(w, "delete", 6))) ||
                       (wl == 4 && (!memcmp(w, "case", 4) || !memcmp(w, "void", 4))) ||
                       (wl == 2 && (!memcmp(w, "in", 2) || !memcmp(w, "do", 2))) ||
                       (wl == 5 && (!memcmp(w, "throw", 5) || !memcmp(w, "yield", 5) || !memcmp(w, "await", 5))) ||
                       (wl == 3 && !memcmp(w, "new", 3)) || (wl == 4 && !memcmp(w, "else", 4)) ||
                       (wl == 10 && !memcmp(w, "instanceof", 10));
            continue;
        }
        if (w_isdigit(c)) { while (i < n && (is_id((unsigned char)s[i]) || s[i] == '.')) i++; regex_ok = 0; continue; }
        if (!w_isspace(c)) regex_ok = !(c == ')' || c == ']' || c == '}');
        i++;
    }
}

static void on_import(struct wjs* js, const char* base, const char* spec, int len, int dynamic) {
    char url[2048];
    if (dynamic) return;   // import() targets load on demand (dyn_import_hook)
    if (resolve_spec(js, base, spec, len, url, sizeof url)) mod_add(js, url, 0);
}

static void set_import_meta(struct wjs* js, ojsv module, const char* url);

// Compile a fetched module (COMPILE_ONLY registers it by name, so linking
// later finds it through the loader) and queue the modules it requests:
// the compiler gives the exact static import list.
static void mod_compile_deps(struct wjs* js, int i) {
    ojs* ctx = js->J;
    js->mods[i].scanned = 1;
    char* url = dupn(js->mods[i].url, (int)strlen(js->mods[i].url));
    if (!url) return;
    ojsv md = ojs_find_module(ctx, url);
    if (ojs_is_undefined(md)) {
        js->deadline = wjs_now() + SCRIPT_BUDGET_MS;
        md = ojs_eval(ctx, js->mods[i].text, js->mods[i].len, url, OJS_EVAL_MODULE | OJS_EVAL_COMPILE_ONLY);
        js->deadline = 0;
        if (ojs_is_exception(md)) {
            wjs_report_exception(js, url);
            js->mods[i].state = SS_FAILED;
            w_free(url);
            return;
        }
        set_import_meta(js, md, url);
    }
    int n = ojs_module_request_count(ctx, md);
    for (int k = 0; k < n; k++) {
        char* spec = ojs_module_request(ctx, md, k);
        if (!spec) continue;
        char dep[2048];
        if (resolve_spec(js, url, spec, (int)strlen(spec), dep, sizeof dep)) mod_add(js, dep, 0);
        ojs_free_cstring(ctx, spec);
    }
    w_free(url);
}

// Discover the static deps of fetched modules (compile = 1: inside a realm
// entry). Returns the number of module fetches still outstanding
// (0 = every static graph is loadable).
static int mods_outstanding_ex(struct wjs* js, int compile) {
    int pending_scan = 0;
    for (int pass = 0; pass < 64; pass++) {
        int did = 0;
        for (int i = 0; i < js->nmod; i++) {
            if (js->mods[i].state != SS_READY || js->mods[i].scanned) continue;
            if (!compile) { pending_scan = 1; continue; }
            mod_compile_deps(js, i);
            did = 1;
        }
        if (!did) break;
    }
    int out = pending_scan;
    for (int i = 0; i < js->nmod; i++)
        if (js->mods[i].state == SS_FETCH && js->mods[i].scanned != 2) out++;
    return out;
}

static int mods_outstanding(struct wjs* js) { return mods_outstanding_ex(js, js->J && !js->dead); }

static int is_http(const char* s) {
    return s && (w_ieq_prefix(s, (int)strlen(s), "http:") || w_ieq_prefix(s, (int)strlen(s), "https:"));
}

// module hook: specifier -> module name (absolute URL)
static char* mod_resolve(ojs* ctx, const char* referrer, const char* name, void* op) {
    struct wjs* js = (struct wjs*)op;
    char url[2048];
    const char* base = is_http(referrer) ? referrer : wdoc_base(js->doc);
    if (!resolve_spec(js, base, name, (int)strlen(name), url, sizeof url)) {
        ojs_throw_type_error(ctx, "Failed to resolve module specifier \"%s\"", name);
        return 0;
    }
    return dupn(url, (int)strlen(url));
}

// import(): load now if the module (and every static dep) is fetched,
// otherwise fetch it and park the promise until wjs_run can finish it.
static int dyn_import_hook(ojs* ctx, const char* referrer, const char* spec, ojsv resolve, ojsv reject, void* op) {
    (void)ctx;
    struct wjs* js = (struct wjs*)op;
    char url[2048];
    const char* base = is_http(referrer) ? referrer : wdoc_base(js->doc);
    if (!resolve_spec(js, base, spec, (int)strlen(spec), url, sizeof url)) return 0;
    int m = mod_find(js, url);
    if (m >= 0 && js->mods[m].state != SS_FETCH && mods_outstanding(js) == 0) return 0;
    if (m < 0) m = mod_add(js, url, 0);
    if (m < 0) return 0;
    if (js->ndyn >= js->capdyn) {
        int nc = js->capdyn ? js->capdyn * 2 : 16;
        struct wjs_dyn* n = (struct wjs_dyn*)w_realloc(js->dyn, nc * sizeof(struct wjs_dyn));
        if (!n) return 0;
        js->dyn = n;
        js->capdyn = nc;
    }
    if (js->ndyn * 2 + 2 > js->capdynv) {
        int nc = js->capdynv ? js->capdynv * 2 : 32;
        if (!wjs_grow_roots(js, &js->dyn_vals, js->capdynv, nc)) return 0;
        js->capdynv = nc;
    }
    struct wjs_dyn* p = &js->dyn[js->ndyn];
    p->basename = dupn(referrer ? referrer : "", referrer ? (int)strlen(referrer) : 0);
    p->spec = dupn(spec, (int)strlen(spec));
    if (!p->basename || !p->spec) { w_free(p->basename); w_free(p->spec); return 0; }
    p->mod = m;
    p->slot = js->ndyn;
    js->dyn_vals[js->ndyn * 2] = resolve;
    js->dyn_vals[js->ndyn * 2 + 1] = reject;
    js->ndyn++;
    js->flags |= WJS_FETCH;
    return 1;
}

static void dyn_free(struct wjs_dyn* p) {
    w_free(p->basename);
    w_free(p->spec);
}

static int dyn_ready_ex(struct wjs* js, struct wjs_dyn* p, int compile) {
    return p->mod < js->nmod && js->mods[p->mod].state != SS_FETCH && mods_outstanding_ex(js, compile) == 0;
}
static int dyn_ready(struct wjs* js, struct wjs_dyn* p) { return dyn_ready_ex(js, p, 1); }

// finish parked import()s whose graphs are complete
static void dyn_run(struct wjs* js) {
    for (int i = 0; i < js->ndyn;) {
        struct wjs_dyn* p = &js->dyn[i];
        if (!dyn_ready(js, p)) { i++; continue; }
        struct wjs_dyn q = *p;
        ojsv res = js->dyn_vals[i * 2], rej = js->dyn_vals[i * 2 + 1];
        int last = --js->ndyn;
        js->dyn[i] = js->dyn[last];
        js->dyn_vals[i * 2] = js->dyn_vals[last * 2];
        js->dyn_vals[i * 2 + 1] = js->dyn_vals[last * 2 + 1];
        js->dyn_vals[last * 2] = js->dyn_vals[last * 2 + 1] = OJS_UNDEFINED;
        js->deadline = wjs_now() + SCRIPT_BUDGET_MS;
        ojs_finish_dynamic_import(js->J, q.basename, q.spec, res, rej);
        wjs_drain_jobs(js);
        js->deadline = 0;
        js->flags |= WJS_DIRTY;
        dyn_free(&q);
    }
}

static void set_import_meta(struct wjs* js, ojsv module, const char* url) {
    ojs* ctx = js->J;
    ojsv meta = ojs_module_meta(ctx, module);
    if (ojs_is_exception(meta)) { ojs_take_exception(ctx); return; }
    if (ojs_set(ctx, meta, "url", ojs_string(ctx, url)) < 0) ojs_take_exception(ctx);
}

// module hook: compile a module the graph needs (it was fetched ahead)
static ojsv mod_load(ojs* ctx, const char* name, void* op) {
    struct wjs* js = (struct wjs*)op;
    int i = mod_find(js, name);
    if (i < 0 || js->mods[i].state != SS_READY) {
        if (i < 0) mod_add(js, name, 1);   // fetch it for a later import() retry
        return ojs_throw_reference_error(ctx, "could not load module '%s' (not fetched)", name);
    }
    struct wjs_mod* m = &js->mods[i];
    ojsv md = ojs_eval(ctx, m->text, m->len, name, OJS_EVAL_MODULE | OJS_EVAL_COMPILE_ONLY);
    if (ojs_is_exception(md)) return md;
    set_import_meta(js, md, name);
    return md;
}

// evaluate a compiled module graph root; errors are reported
static void run_module(struct wjs* js, ojsv md, const char* fname) {
    ojs* ctx = js->J;
    ojsv p = ojs_run_compiled(ctx, md);
    if (ojs_is_exception(p)) { wjs_report_exception(js, fname); return; }
    wjs_drain_jobs(js);
    ojsv why;
    if (ojs_promise_state(ctx, p, &why) == 2) {
        ojs_throw(ctx, why);
        wjs_report_exception(js, fname);
    }
}

// ---- scripts --------------------------------------------------------------------------

static int js_mime(const char* t, int l) {
    while (l > 0 && w_isspace((unsigned char)t[0])) { t++; l--; }
    while (l > 0 && w_isspace((unsigned char)t[l - 1])) l--;
    int semi = 0;
    while (semi < l && t[semi] != ';') semi++;
    l = semi;
    static const char* const ok[] = { "text/javascript", "application/javascript", "text/ecmascript",
        "application/ecmascript", "application/x-javascript", "application/x-ecmascript", "text/jscript",
        "text/livescript", "text/x-javascript", "text/x-ecmascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4",
        "text/javascript1.5", "module", 0 };
    for (int i = 0; ok[i]; i++) if (w_ieq(t, l, ok[i])) return ok[i][0] == 'm' ? 2 : 1;
    return 0;
}

static int utf8_valid(const char* s, int n) {
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (k < 0 || i + k >= n + (k ? 0 : 1)) return 0;
        for (int j = 1; j <= k; j++) if (((unsigned char)s[i + j] >> 6) != 2) return 0;
        i += k + 1;
    }
    return 1;
}

// copy script bytes into a NUL-terminated UTF-8 buffer
static char* script_text(const char* b, int n, int* out_len) {
    if (n >= 3 && (uint8_t)b[0] == 0xEF && (uint8_t)b[1] == 0xBB && (uint8_t)b[2] == 0xBF) { b += 3; n -= 3; }
    char* t;
    int tl;
    if (utf8_valid(b, n)) {
        t = (char*)w_malloc(n + 1);
        if (!t) return 0;
        memcpy(t, b, n);
        tl = n;
    } else {
        char used[24];
        t = wcharset_decode(b, n, "windows-1252", &tl, used, sizeof used);
        if (!t) return 0;
        char* t2 = (char*)w_realloc(t, tl + 1);
        if (!t2) { w_free(t); return 0; }
        t = t2;
    }
    t[tl] = 0;
    *out_len = tl;
    return t;
}

static int script_new(struct wjs* js) {
    if (js->nsc >= MAX_SCRIPTS) return -1;
    if (js->nsc >= js->capsc) {
        int nc = js->capsc ? js->capsc * 2 : 32;
        struct wjs_script* s = (struct wjs_script*)w_realloc(js->sc, nc * sizeof(struct wjs_script));
        if (!s) return -1;
        js->sc = s;
        js->capsc = nc;
    }
    struct wjs_script* s = &js->sc[js->nsc];
    memset(s, 0, sizeof *s);
    s->node = -1;
    s->res = -1;
    return js->nsc++;
}

// Prepare a <script> element (parser or dynamic). Returns the record index.
static int prepare_script(struct wjs* js, int el, int parser, int insert_at) {
    struct wdom* d = js->d;
    if (!wjs_grow_nodes(js)) return -1;
    if (js->node_flags[el] & NF_STARTED) return -1;
    int tl;
    const char* type = wdom_attr(d, el, A_type, &tl);
    int kind;
    if (!type || !tl) {
        int ll;
        const char* lang = wdom_attr_s(d, el, "language", &ll);
        kind = (lang && ll && !w_ieq_prefix(lang, ll, "javascript") && !w_ieq_prefix(lang, ll, "jscript")) ? 0 : 1;
    } else if (w_ieq(type, tl, "importmap")) {
        js->node_flags[el] |= NF_STARTED;
        int n = 0;
        for (int c = d->n[el].first; c >= 0; c = d->n[c].next) if (d->n[c].type == WN_TEXT) n += (int)d->n[c].tlen;
        char* t = (char*)w_malloc(n + 1);
        if (t) {
            wdom_text_content(d, el, t, n + 1);
            w_free(js->importmap);
            js->importmap = t;
        }
        return -1;
    } else kind = js_mime(type, tl);
    if (!kind) return -1;                       // data block
    if (kind == 1 && wdom_attr_s(d, el, "nomodule", &tl)) return -1;
    js->node_flags[el] |= NF_STARTED | (parser ? NF_PARSER : 0);
    int idx = script_new(js);
    if (idx < 0) return -1;
    // insertion point (document.write scripts run right after the writer)
    if (insert_at >= 0 && insert_at < idx) {
        struct wjs_script tmp = js->sc[idx];
        memmove(&js->sc[insert_at + 1], &js->sc[insert_at], (idx - insert_at) * sizeof(struct wjs_script));
        js->sc[insert_at] = tmp;
        idx = insert_at;
    }
    struct wjs_script* s = &js->sc[idx];
    s->node = el;
    s->kind = kind == 2 ? SK_MODULE : SK_CLASSIC;
    s->parser = (uint8_t)parser;
    int sl;
    const char* src = wdom_attr(d, el, A_src, &sl);
    int async = wdom_attr_s(d, el, "async", &tl) != 0;
    int defer = wdom_attr_s(d, el, "defer", &tl) != 0;
    if (s->kind == SK_MODULE) s->mode = async ? SM_ASYNC : SM_DEFER;
    else if (!parser) s->mode = SM_ASYNC;
    else if (src && async) s->mode = SM_ASYNC;
    else if (src && defer) s->mode = SM_DEFER;
    else s->mode = SM_BLOCK;
    if (!parser && s->kind == SK_MODULE) s->mode = SM_ASYNC;
    if (src) {
        char url[2048];
        if (sl == 0 || wurl_resolve(wdoc_base(js->doc), src, sl, url, sizeof url) <= 0) {
            s->state = SS_FAILED;
            return idx;
        }
        s->url = dupn(url, (int)strlen(url));
        if (s->kind == SK_MODULE) {
            int m = mod_add(js, url, 0);
            s->state = SS_FETCH;
            s->res = m >= 0 ? -2 - m : -1;      // waits on the module map
            if (m < 0) s->state = SS_FAILED;
        } else {
            s->res = wdoc_res_script(js->doc, url);
            s->state = s->res >= 0 ? SS_FETCH : SS_FAILED;
            if (s->res >= 0) js->flags |= WJS_FETCH;
        }
    } else {
        int n = 0;
        for (int c = d->n[el].first; c >= 0; c = d->n[c].next) if (d->n[c].type == WN_TEXT) n += (int)d->n[c].tlen;
        s->text = (char*)w_malloc(n + 1);
        if (!s->text) { s->state = SS_FAILED; return idx; }
        s->len = wdom_text_content(d, el, s->text, n + 1);
        s->state = SS_READY;
        if (s->kind == SK_MODULE) scan_imports(js, s->text, s->len, wdoc_base(js->doc), on_import);
    }
    return idx;
}

void wjs_scan_scripts(struct wjs* js) {
    if (!js || js->dead) return;
    struct wdom* d = js->d;
    for (int el = d->n[0].first; el >= 0; el = wdom_next(d, el, 0))
        if (wdom_is(d, el, T_script)) prepare_script(js, el, 1, -1);
}

static void free_text(struct wjs_script* s) { w_free(s->text); s->text = 0; s->len = 0; }

static void fire_script_event(struct wjs* js, int node, int ok) {
    if (node < 0) return;
    ojsv a[2] = { wjs_wrap(js, node), ojs_bool(ok) };
    call_hook(js, js->h_script, 2, a, TASK_BUDGET_MS);
}

static int module_ready_ex(struct wjs* js, struct wjs_script* s, int compile) {
    if (s->kind != SK_MODULE || s->state == SS_FAILED) return 1;
    if (s->url) {
        int m = -2 - s->res;
        if (m < 0 || m >= js->nmod) return 1;
        if (js->mods[m].state == SS_FETCH) return 0;
        if (js->mods[m].state == SS_FAILED) { s->state = SS_FAILED; return 1; }
    }
    return mods_outstanding_ex(js, compile) == 0;
}
static int module_ready(struct wjs* js, struct wjs_script* s) { return module_ready_ex(js, s, 1); }

static void run_script(struct wjs* js, int idx) {
    struct wjs_script* s = &js->sc[idx];
    if (s->state == SS_DONE) return;
    if (s->state == SS_FAILED) {
        s->state = SS_DONE;
        if (s->url) {
            char m[300];
            int n = 0;
            const char* a = "failed to load script ";
            while (*a) m[n++] = *a++;
            for (const char* b = s->url; *b && n < 290; b++) m[n++] = *b;
            wjs_logf(js, 2, m, n);
        }
        fire_script_event(js, s->node, 0);
        return;
    }
    ojs* ctx = js->J;
    int node = s->node;
    s->state = SS_DONE;
    js->nscripts_run++;
    char name[600];
    const char* fname = s->url;
    if (!fname) {
        const char* u = wdoc_url(js->doc);
        int n = 0;
        while (u[n] && n < 500) { name[n] = u[n]; n++; }
        const char* sfx = s->kind == SK_MODULE ? "#inline-module-" : "#inline-script-";
        while (*sfx) name[n++] = *sfx++;
        int v = idx, d0 = n;
        do { name[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        for (int a = d0, b = n - 1; a < b; a++, b--) { char t = name[a]; name[a] = name[b]; name[b] = t; }
        name[n] = 0;
        fname = name;
    }
    js->deadline = wjs_now() + SCRIPT_BUDGET_MS;
    if (s->kind == SK_CLASSIC) {
        int prev = js->current_script, prev_i = js->current_index;
        js->current_script = node;
        js->current_index = idx;
        // the source buffer may be released by a nested write; keep our own
        char* text = s->text;
        int len = s->len;
        s->text = 0;
        ojsv r = text ? ojs_eval(ctx, text, len, fname, OJS_EVAL_SCRIPT) : OJS_UNDEFINED;
        if (ojs_is_exception(r)) wjs_report_exception(js, fname);
        w_free(text);
        js->current_script = prev;
        js->current_index = prev_i;
    } else {
        const char* text = s->text;
        int len = s->len;
        int m = s->url ? -2 - s->res : -1;
        if (m >= 0 && m < js->nmod) { text = js->mods[m].text; len = js->mods[m].len; }
        // an external root was compiled on arrival: link + run that module
        ojsv pre = s->url ? ojs_find_module(ctx, s->url) : OJS_UNDEFINED;
        if (!ojs_is_undefined(pre)) run_module(js, pre, fname);
        else if (text) {
            ojsv md = ojs_eval(ctx, text, len, fname, OJS_EVAL_MODULE | OJS_EVAL_COMPILE_ONLY);
            if (ojs_is_exception(md)) wjs_report_exception(js, fname);
            else {
                set_import_meta(js, md, s->url ? s->url : wdoc_url(js->doc));
                run_module(js, md, fname);
            }
        }
        free_text(&js->sc[idx]);
    }
    wjs_drain_jobs(js);
#if !(defined(KERNEL) && KERNEL)
    if (getenv("OJS_SCRIPTTIME"))   // host diagnostics: time per script (+ its microtasks)
        fprintf(stderr, "[time] %6ums %.150s\n", (unsigned)(wjs_now() - (js->deadline - SCRIPT_BUDGET_MS)), fname);
#endif
    js->deadline = 0;
    js->flags |= WJS_DIRTY;
    if (js->sc[idx].url) fire_script_event(js, node, 1);
}

void wjs_dynamic_script(struct wjs* js, int node) {
    int idx = prepare_script(js, node, 0, -1);
    if (idx < 0) return;
    struct wjs_script* s = &js->sc[idx];
    if (!s->url && s->state == SS_READY && s->kind == SK_CLASSIC) run_script(js, idx);   // inline: now
}

void wjs_doc_write(struct wjs* js, const char* html, int len) {
    struct wdom* d = js->d;
    int cur = js->current_script;
    if (cur < 0 || js->dcl_fired || d->n[cur].parent < 0 || !(js->node_flags[cur] & NF_PARSER)) {
        wjs_logf(js, 2, "document.write ignored (not during parsing)", 43);
        return;
    }
    int f = wdom_create_fragment(d);
    if (f < 0) return;
    int p = d->n[cur].parent;
    int ctx_tag = d->n[p].type == WN_ELEM && d->n[p].ns == NS_HTML ? d->n[p].tag : T_body;
    if (ctx_tag == T_head || ctx_tag == T_html) ctx_tag = T_body;
    whtml_parse_fragment_ctx(d, f, ctx_tag, html, len);
    // insert after the writing script (after earlier writes from it, too)
    int ref = d->n[cur].next;
    while (ref >= 0 && js->node_cap > ref && (js->node_flags[ref] & 0x80)) ref = d->n[ref].next;
    int insert_at = js->current_index + 1;
    while (d->n[f].first >= 0) {
        int k = d->n[f].first;
        wdom_insert_before(d, p, k, ref);
        if (wjs_grow_nodes(js)) js->node_flags[k] |= 0x80;   // written by this script
        // written scripts run right after the writer, in order (parser-inserted)
        for (int e = k; e >= 0; e = wdom_next(d, e, k))
            if (wdom_is(d, e, T_script)) {
                int i = prepare_script(js, e, 1, insert_at);
                if (i >= 0) {
                    insert_at = i + 1;
                    if (js->sc[i].mode == SM_DEFER) js->sc[i].mode = SM_ASYNC;
                }
            }
        wjs_inserted(js, k);   // styles/images (scripts are already started)
    }
    js->flags |= WJS_DIRTY;
    wdoc_dom_changed(js->doc, WDC_LAYOUT | WDC_STYLE | WDC_IMG);
}

// ---- mutation side effects -------------------------------------------------------------

void wjs_inserted(struct wjs* js, int node) {
    struct wdom* d = js->d;
    js->flags |= WJS_DIRTY;
    if (!wdom_is_connected(d, node)) { wdoc_dom_changed(js->doc, WDC_LAYOUT); return; }
    int what = WDC_LAYOUT;
    int scripts[32], ns = 0;
    for (int e = node; e >= 0; e = wdom_next(d, e, node)) {
        if (d->n[e].type != WN_ELEM) continue;
        if (d->n[e].ns == NS_HTML) {
            int t = d->n[e].tag;
            if (t == T_style || t == T_link) what |= WDC_STYLE;
            else if (t == T_img || t == T_source || t == T_video || t == T_picture || t == T_input) what |= WDC_IMG;
            else if (t == T_script && ns < 32 && !(e < js->node_cap && (js->node_flags[e] & NF_STARTED))) scripts[ns++] = e;
        }
        if (wdom_has_attr(d, e, A_style)) what |= WDC_IMG;   // background-image urls
    }
    wdoc_dom_changed(js->doc, what);
    for (int i = 0; i < ns; i++) wjs_dynamic_script(js, scripts[i]);
}

void wjs_removed(struct wjs* js, int parent, int node) {
    (void)parent;
    struct wdom* d = js->d;
    int what = WDC_LAYOUT;
    for (int e = node; e >= 0; e = wdom_next(d, e, node))
        if (wdom_is(d, e, T_style) || wdom_is(d, e, T_link)) what |= WDC_STYLE;
    js->flags |= WJS_DIRTY;
    wdoc_dom_changed(js->doc, what);
}

void wjs_attr_changed(struct wjs* js, int el, int a) {
    struct wdom* d = js->d;
    js->flags |= WJS_DIRTY;
    int what = WDC_LAYOUT;
    if (d->n[el].ns == NS_HTML) {
        int t = d->n[el].tag;
        if ((t == T_link && (a == A_href || a == A_rel || a == A_media || a == A_disabled)) ||
            (t == T_style && (a == A_media || a == A_disabled)))
            what |= WDC_STYLE;
        if (a == A_src || a == A_srcset || a == A_style || a == A_poster) what |= WDC_IMG;
        else {
            int nl;
            const char* nm = watom_name(&d->atoms, a, &nl);
            if (nl > 5 && !memcmp(nm, "data-", 5) && (t == T_img || t == T_source)) what |= WDC_IMG;
        }
        if (t == T_script && a == A_src && wdom_is_connected(d, el) &&
            !(el < js->node_cap && (js->node_flags[el] & NF_STARTED)))
            wjs_dynamic_script(js, el);
    }
    wdoc_dom_changed(js->doc, what);
}

void wjs_text_changed(struct wjs* js, int node) {
    struct wdom* d = js->d;
    js->flags |= WJS_DIRTY;
    int p = d->n[node].type == WN_ELEM ? node : d->n[node].parent;
    int what = WDC_LAYOUT;
    if (p >= 0 && wdom_is(d, p, T_style)) what |= WDC_STYLE;
    if (p >= 0 && wdom_is(d, p, T_title) && p == wdom_first_tag(d, T_title)) {
        int n = wdom_text_content(d, p, d->title, sizeof d->title);
        d->title[n < (int)sizeof d->title ? n : (int)sizeof d->title - 1] = 0;
        js->flags |= WJS_TITLE;
    }
    wdoc_dom_changed(js->doc, what);
    // inline script that gained its text after insertion runs now
    if (p >= 0 && wdom_is(d, p, T_script) && wdom_is_connected(d, p) &&
        !(p < js->node_cap && (js->node_flags[p] & NF_STARTED)) && d->n[p].first >= 0)
        wjs_dynamic_script(js, p);
}

// ---- lifecycle --------------------------------------------------------------------------

struct wjs* wjs_new(struct wdoc* doc) {
    struct wjs* js = (struct wjs*)w_calloc(1, sizeof(struct wjs));
    if (!js) return 0;
    js->doc = doc;
    js->d = wdoc_dom(doc);
    js->current_script = -1;
    js->current_index = -1;
    js->scroll_req = -1;
    js->focus_req = -1;
    js->submit_form = -1;
    js->submit_btn = -1;
    js->next_due = -1;
    js->t0 = wjs_now();
    js->hooks = OJS_UNDEFINED;
    ojsv* hv[] = { &js->hooks, &js->h_dispatch, &js->h_ui, &js->h_fetch, &js->h_nextdue, &js->h_rundue, &js->h_frame,
                   &js->h_dcl, &js->h_load, &js->h_script, &js->h_image, &js->h_proto, &js->h_report,
                   &js->h_mo, &js->h_click, &js->h_ready, &js->proto_svg, &js->proto_svgroot,
                   &js->proto_math, &js->proto_text, &js->proto_comment, &js->proto_doc, &js->proto_frag };
    for (unsigned i = 0; i < sizeof hv / sizeof hv[0]; i++) *hv[i] = OJS_UNDEFINED;
    if (!wjs_grow_nodes(js)) { w_free(js); return 0; }
#if defined(__i386__)
    uint16_t cw = 0x027F;
    __asm__ volatile("fldcw %0" :: "m"(cw));
#endif
    js->J = ojs_new();
    if (!js->J) { js->dead = 1; wjs_logf(0, 3, "script realm: out of memory", 27); return js; }
    ojs* ctx = js->J;
    ojs_enter(ctx);
    ojs_set_opaque(ctx, js);
    ojs_set_memory_limit(ctx, wjs_sys_realm_limit());
    ojs_set_memory_guard(ctx, wjs_sys_mem_ok, 0);
    ojs_set_stack_size(ctx, REALM_STACK_MAX);
    ojs_set_interrupt_handler(ctx, interrupt_cb, js);
    ojs_set_rejection_tracker(ctx, rejection_cb, js);
#if !(defined(KERNEL) && KERNEL)
    if (getenv("OJS_GCSTRESS")) ojs_set_gc_stress(ctx, (uint32_t)atoi(getenv("OJS_GCSTRESS")));   // host GC-root testing
#endif
    struct ojs_module_hooks mh = { mod_resolve, mod_load, dyn_import_hook, js };
    ojs_set_module_hooks(ctx, &mh);
    if (wjs_host.random) {
        uint64_t seed;
        wjs_host.random((uint8_t*)&seed, sizeof seed);
        ojs_set_random_seed(ctx, seed);
    }
    // roots: every value slot of struct wjs, and the arrays grown before the realm existed
    for (unsigned i = 0; i < sizeof hv / sizeof hv[0]; i++) ojs_add_root(ctx, hv[i]);
    ojs_add_root_range(ctx, js->node_obj, js->node_cap);
    int cls = ojs_host_class(ctx, "Node");
    if (!wjs_class_id) wjs_class_id = cls;
    ojsv natives = ojs_object(ctx);
    ojs_set_functions(ctx, natives, env_funcs, (int)(sizeof env_funcs / sizeof env_funcs[0]));
    wjs_dom_install(js, natives);
    js->deadline = wjs_now() + SCRIPT_BUDGET_MS;
    // the prelude evaluates to a function(W, global) that returns the hooks
    ojsv fn = ojs_eval(ctx, wjs_prelude_src, wjs_prelude_len, "<okai-prelude>", OJS_EVAL_SCRIPT);
    ojsv hooks = OJS_UNDEFINED;
    if (ojs_is_exception(fn)) wjs_report_exception(js, "prelude compile");
    else {
        ojsv args[2] = { natives, ojs_global(ctx) };
        hooks = ojs_call(ctx, fn, OJS_UNDEFINED, 2, args);
        if (ojs_is_exception(hooks)) { wjs_report_exception(js, "prelude"); hooks = OJS_UNDEFINED; }
    }
    wjs_drain_jobs(js);
    js->deadline = 0;
    if (!ojs_is_object(hooks)) {
        wjs_logf(js, 3, "prelude failed: scripts disabled", 32);
        ojs_leave(ctx);
        js->dead = 1;   // realm unusable; free it later without running anything
        return js;
    }
    js->hooks = hooks;
    struct { ojsv* slot; const char* name; } hk[] = {
        { &js->h_dispatch, "dispatch" }, { &js->h_ui, "uiEvent" }, { &js->h_fetch, "onFetch" },
        { &js->h_nextdue, "nextDue" }, { &js->h_rundue, "runDue" }, { &js->h_frame, "runFrame" },
        { &js->h_dcl, "fireDCL" }, { &js->h_load, "fireLoad" }, { &js->h_script, "scriptEvent" },
        { &js->h_image, "imageEvent" }, { &js->h_proto, "protoFor" }, { &js->h_report, "report" },
        { &js->h_mo, "moChildList" }, { &js->h_click, "dispatchClick" }, { &js->h_ready, "setReady" } };
    for (unsigned i = 0; i < sizeof hk / sizeof hk[0]; i++) {
        ojsv v = ojs_get(ctx, hooks, hk[i].name);
        if (ojs_is_exception(v)) { ojs_take_exception(ctx); v = OJS_UNDEFINED; }
        if (!ojs_is_undefined(v) || ojs_is_undefined(*hk[i].slot)) *hk[i].slot = v;   // setProtoFor may have filled one
    }
    ojs_leave(ctx);
    return js;
}

int wjs_ok(struct wjs* js) { return js && !js->dead; }

void wjs_free(struct wjs* js) {
    if (!js) return;
    wjs_selq_forget(js->d);
#if !(defined(KERNEL) && KERNEL)
    if (js->J && getenv("OJS_GCSTATS")) {   // host: where the realm's memory went
        ojs_gc(js->J);
        struct ojs_stats st;
        ojs_get_stats(js->J, &st);
        fprintf(stderr, "[gc] heap %zuKB live %zuKB collections %d (%.0fms) allocated %lluKB\n", st.heap_bytes >> 10, st.live_bytes >> 10, st.gc_count, st.gc_ms, st.total_alloc >> 10);
        const char* names = OJS_STATS_TYPES;
        for (int i = 0; i < 16 && *names; i++) {
            int l = 0;
            while (names[l] && names[l] != ' ') l++;
            if (st.live_by_type[i] >= 1024) fprintf(stderr, "  %.*s %zuKB\n", l, names, st.live_by_type[i] >> 10);
            names += l;
            while (*names == ' ') names++;
        }
    }
#endif
    // the realm owns every value; freeing it releases them all at once
    if (js->J) ojs_free(js->J);
    js->J = 0;
    for (int i = 0; i < js->ndyn; i++) dyn_free(&js->dyn[i]);
    for (int i = 0; i < js->nsc; i++) { w_free(js->sc[i].url); w_free(js->sc[i].text); }
    w_free(js->sc);
    for (int i = 0; i < js->nmod; i++) { w_free(js->mods[i].url); w_free(js->mods[i].text); }
    w_free(js->mods);
    w_free(js->importmap);
    w_free(js->dyn);
    w_free(js->dyn_vals);
    w_free(js->node_obj);
    w_free(js->node_flags);
    w_free(js->proto_html);
    w_free(js->tpl_map);
    w_free(js->nav_url);
    w_free(js);
}

void wjs_resource_done(struct wjs* js, int res, int status, const char* headers, const char* body, int len,
                       const char* final_url) {
    if (!js || js->dead) return;
    int ok = status >= 200 && status < 300 && body && len >= 0;
    for (int i = 0; i < js->nsc; i++) {
        struct wjs_script* s = &js->sc[i];
        if (s->res != res || s->state != SS_FETCH || s->kind != SK_CLASSIC) continue;
        s->res = -1;
        if (ok) {
            s->text = script_text(body, len, &s->len);
            s->state = s->text ? SS_READY : SS_FAILED;
        } else s->state = SS_FAILED;
        return;
    }
    for (int i = 0; i < js->nmod; i++) {
        struct wjs_mod* m = &js->mods[i];
        if (m->res != res || m->state != SS_FETCH) continue;
        m->res = -1;
        if (ok) {
            m->text = script_text(body, len, &m->len);
            m->state = m->text ? SS_READY : SS_FAILED;
        } else m->state = SS_FAILED;
        if (m->state == SS_FAILED) {
            char msg[300];
            int n = 0;
            const char* a = "failed to load module ";
            while (*a) msg[n++] = *a++;
            for (const char* b = m->url; *b && n < 290; b++) msg[n++] = *b;
            wjs_logf(js, 2, msg, n);
        }
        return;
    }
    // a fetch()/XHR request
    GUARD(js, );
    ojs* ctx = js->J;
    ojsv a[6];
    a[0] = ojs_int(res);
    a[1] = ojs_int(status);
    a[2] = ojs_string(ctx, status == 200 ? "OK" : "");
    a[3] = ojs_string(ctx, headers ? headers : "");
    a[4] = (body && len >= 0) ? ojs_arraybuffer_copy(ctx, body, (size_t)len) : OJS_NULL;
    a[5] = ojs_string(ctx, final_url ? final_url : "");
    call_hook(js, js->h_fetch, 6, a, TASK_BUDGET_MS);
    js->flags |= WJS_DIRTY;
    UNGUARD(js);
}

void wjs_image_done(struct wjs* js, int slot, int ok) {
    if (!js || js->dead || !js->dcl_fired) return;
    GUARD(js, );
    struct wdom* d = js->d;
    for (int el = d->n[0].first; el >= 0; el = wdom_next(d, el, 0)) {
        if (!wdom_is(d, el, T_img) || wdoc_img_node_slot(js->doc, el) != slot) continue;
        if (el >= js->node_cap || ojs_is_undefined(js->node_obj[el])) continue;   // no script holds it
        ojsv a[2] = { wjs_wrap(js, el), ojs_bool(ok) };
        call_hook(js, js->h_image, 2, a, TASK_BUDGET_MS);
    }
    UNGUARD(js);
}

static int any_script_pending(struct wjs* js) {
    for (int i = 0; i < js->nsc; i++)
        if (js->sc[i].state == SS_FETCH || js->sc[i].state == SS_READY) return 1;
    return 0;
}

static int ready_to_run(struct wjs* js, struct wjs_script* s) {
    if (s->state == SS_FAILED) return 1;
    if (s->kind == SK_MODULE) return (s->state == SS_READY || s->url) && module_ready(js, s);
    return s->state == SS_READY;
}

int wjs_run(struct wjs* js, int budget_ms) {
    if (!js || js->dead) return 0;
    GUARD(js, 0);
    int start = wjs_now();
    int over = 0;
    // 1. parser-blocking scripts, in document order
    while (js->next_block < js->nsc && !over) {
        struct wjs_script* s = &js->sc[js->next_block];
        if (!(s->parser && s->mode == SM_BLOCK) || s->state == SS_DONE) { js->next_block++; continue; }
        if (!ready_to_run(js, s)) break;
        run_script(js, js->next_block);
        js->next_block++;
        if (wjs_now() - start > budget_ms) over = 1;
    }
    int blocking_done = js->next_block >= js->nsc;
    // 2. parsing is over: readyState "interactive", then defer + module
    // scripts in order, then DOMContentLoaded
    if (blocking_done && !js->interactive && !over) {
        js->interactive = 1;
        ojsv a[1] = { ojs_string(js->J, "interactive") };
        call_hook(js, js->h_ready, 1, a, TASK_BUDGET_MS);
    }
    if (blocking_done) {
        while (js->next_defer < js->nsc && !over) {
            struct wjs_script* s = &js->sc[js->next_defer];
            if (!(s->parser && s->mode == SM_DEFER) || s->state == SS_DONE) { js->next_defer++; continue; }
            if (!ready_to_run(js, s)) break;
            run_script(js, js->next_defer);
            js->next_defer++;
            if (wjs_now() - start > budget_ms) over = 1;
        }
        if (js->next_defer >= js->nsc && !js->dcl_fired && !over) {
            js->dcl_fired = 1;
            call_hook(js, js->h_dcl, 0, 0, TASK_BUDGET_MS * 2);
            js->flags |= WJS_DIRTY;
        }
    }
    // 3. async and dynamic scripts, parked import()s: whenever ready
    if (js->ndyn && !over) dyn_run(js);
    for (int i = 0; i < js->nsc && !over; i++) {
        struct wjs_script* s = &js->sc[i];
        if (s->state == SS_DONE || (s->parser && s->mode != SM_ASYNC)) continue;
        if (!ready_to_run(js, s)) continue;
        run_script(js, i);
        if (wjs_now() - start > budget_ms) over = 1;
    }
    // 4. timers (one at a time, microtasks in between), animation frames
    if (js->dcl_fired || !any_script_pending(js)) {
        for (int k = 0; k < 256 && !over; k++) {
            ojsv a[1] = { ojs_int(wjs_now() - js->t0) };
            ojsv r = call_hook(js, js->h_rundue, 1, a, TASK_BUDGET_MS);
            int ran = ojs_to_bool(js->J, r);
            if (!ran) break;
            js->flags |= WJS_DIRTY;
            if (wjs_now() - start > budget_ms) over = 1;
        }
        if (!over) {
            ojsv a[1] = { ojs_int(wjs_now() - js->t0) };
            ojsv r = call_hook(js, js->h_frame, 1, a, TASK_BUDGET_MS);
            if (ojs_to_bool(js->J, r)) js->flags |= WJS_DIRTY;
        }
    }
    // 5. load
    if (js->dcl_fired && !js->load_fired && !over && !any_script_pending(js) && !wdoc_pending_load(js->doc)) {
        js->load_fired = 1;
        call_hook(js, js->h_load, 0, 0, TASK_BUDGET_MS * 2);
        js->flags |= WJS_DIRTY;
        char st[160], line[220];
        wjs_stats(js, st, sizeof st);
        int n = 0;
        const char* a = "[js] load event: ";
        while (*a) line[n++] = *a++;
        for (const char* b = st; *b && n < 200; b++) line[n++] = *b;
        line[n++] = '\n';
        line[n] = 0;
        if (wjs_host.log) wjs_host.log(line);
        else w_log("%s", line);
    }
    // cache the next wake-up
    {
        ojsv r = call_hook(js, js->h_nextdue, 0, 0, TASK_BUDGET_MS);
        int32_t due = -1;
        ojs_to_int32(js->J, &due, r);
        js->next_due = due;
        if (over || (js->nsc && js->next_block < js->nsc && ready_to_run(js, &js->sc[js->next_block])))
            js->next_due = 0;
    }
    UNGUARD(js);
    int f = js->flags;
    js->flags = 0;
    return f;
}

int wjs_next_due(struct wjs* js) {
    if (!js || js->dead) return -1;
    // fetched modules wait to be compiled (dependency discovery) in wjs_run
    for (int i = 0; i < js->nmod; i++) if (js->mods[i].state == SS_READY && !js->mods[i].scanned) return 0;
    // a script became ready (fetched) since the last run?
    for (int i = 0; i < js->nsc; i++) {
        struct wjs_script* s = &js->sc[i];
        if (s->state == SS_FAILED || (s->state == SS_READY && s->kind == SK_CLASSIC)) return 0;
        if (s->kind == SK_MODULE && s->state != SS_DONE && module_ready_ex(js, s, 0)) return 0;
    }
    for (int i = 0; i < js->ndyn; i++) if (dyn_ready_ex(js, &js->dyn[i], 0)) return 0;
    if (js->dcl_fired && !js->load_fired && !any_script_pending(js) && !wdoc_pending_load(js->doc)) return 0;
    if (!js->dcl_fired && js->next_block >= js->nsc && js->next_defer >= js->nsc) return 0;
    if (js->next_due < 0) return -1;
    int now = wjs_now() - js->t0;
    return js->next_due <= now ? 0 : js->next_due - now;
}

int wjs_event(struct wjs* js, int node, const char* type, int x, int y, int button, int key, int* prevented) {
    if (prevented) *prevented = 0;
    if (!js || js->dead) return 0;
    GUARD(js, 0);
    ojs* ctx = js->J;
    ojsv a[6] = { node >= 0 ? wjs_wrap(js, node) : OJS_NULL, ojs_string(ctx, type), ojs_int(x),
                     ojs_int(y), ojs_int(button), ojs_int(key) };
    ojsv r = call_hook(js, js->h_ui, 6, a, TASK_BUDGET_MS);
    if (prevented && ojs_is_bool(r) && !ojs_to_bool(ctx, r)) *prevented = 1;
    {
        ojsv r2 = call_hook(js, js->h_nextdue, 0, 0, TASK_BUDGET_MS);
        int32_t due = -1;
        ojs_to_int32(ctx, &due, r2);
        js->next_due = due;
    }
    UNGUARD(js);
    int f = js->flags;
    js->flags = 0;
    return f;
}

int wjs_busy(struct wjs* js) { return js && !js->dead && (any_script_pending(js) || !js->dcl_fired); }

int wjs_take_nav(struct wjs* js, char* url, int cap, int* replace) {
    if (!js) return 0;
    if (js->submit_form >= 0) {
        // encoded as "#submit:<form>:<button>" for the shell
        int f = js->submit_form, b = js->submit_btn;
        js->submit_form = -1;
        if (cap > 40) {
            char tmp[40];
            int n = 0;
            const char* p = "#submit:";
            while (*p) tmp[n++] = *p++;
            int vals[2] = { f, b };
            for (int k = 0; k < 2; k++) {
                int v = vals[k];
                if (v < 0) { tmp[n++] = '-'; v = 1; }
                char d[12]; int dl = 0;
                do { d[dl++] = (char)('0' + v % 10); v /= 10; } while (v);
                while (dl) tmp[n++] = d[--dl];
                if (!k) tmp[n++] = ':';
            }
            tmp[n] = 0;
            memcpy(url, tmp, n + 1);
            if (replace) *replace = 0;
            return 1;
        }
    }
    if (js->hist_delta) {
        int dlt = js->hist_delta;
        js->hist_delta = 0;
        if (cap > 16) {
            url[0] = '#'; url[1] = 'h'; url[2] = 'i'; url[3] = 's'; url[4] = 't'; url[5] = ':';
            int n = 6;
            if (dlt < 0) { url[n++] = '-'; dlt = -dlt; }
            char d[12]; int dl = 0;
            do { d[dl++] = (char)('0' + dlt % 10); dlt /= 10; } while (dlt);
            while (dl) url[n++] = d[--dl];
            url[n] = 0;
            if (replace) *replace = 0;
            return 1;
        }
    }
    if (!js->nav_url) return 0;
    int n = (int)strlen(js->nav_url);
    if (n > cap - 1) n = cap - 1;
    memcpy(url, js->nav_url, n);
    url[n] = 0;
    if (replace) *replace = js->nav_replace;
    w_free(js->nav_url);
    js->nav_url = 0;
    return 1;
}

int wjs_take_scroll(struct wjs* js, int* y) {
    if (!js || js->scroll_req < 0) return 0;
    *y = js->scroll_req;
    js->scroll_req = -1;
    return 1;
}

int wjs_take_focus(struct wjs* js, int* node) {
    if (!js || !js->focus_set) return 0;
    *node = js->focus_req;
    js->focus_set = 0;
    return 1;
}

int wjs_take_history(struct wjs* js, int* delta) {
    if (!js || !js->hist_delta) return 0;
    *delta = js->hist_delta;
    js->hist_delta = 0;
    return 1;
}

void wjs_stats(struct wjs* js, char* out, int cap) {
    if (!js) { if (cap) out[0] = 0; return; }
    // heap: memory the realm holds; live: what survived its last collection
    struct ojs_stats st;
    memset(&st, 0, sizeof st);
    if (js->J) ojs_get_stats(js->J, &st);
    int vals[7] = { js->nsc, js->nscripts_run, js->nerrors, js->nmod, (int)(st.heap_bytes >> 10), (int)(st.live_bytes >> 10), st.gc_count };
    const char* names[7] = { "scripts=", " ran=", " errors=", " modules=", " heap=", "KB live=", "KB gcs=" };
    int n = 0;
    for (int k = 0; k < 7 && n < cap - 16; k++) {
        for (const char* p = names[k]; *p && n < cap - 16; p++) out[n++] = *p;
        int v = vals[k];
        char d[12]; int dl = 0;
        do { d[dl++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (dl && n < cap - 4) out[n++] = d[--dl];
    }
    if (js->dead && n < cap - 8) { memcpy(out + n, " dead", 5); n += 5; }
    out[n] = 0;
}
