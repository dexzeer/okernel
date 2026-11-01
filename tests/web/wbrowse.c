// Host browser harness: the full engine + page scripts against live sites
// (fetched with curl, cached in ~/.cache/wbrowse/) or local files.
//
//   wbrowse <url|file.html> [width] [height] [out.ppm] [seconds]
//
// Drives the same loop okai does: parse -> fetch resources (CSS, scripts,
// requests, images) one at a time -> run scripts/timers -> render. Prints
// the console, a summary line, and writes a PPM of the first screen.
// WB_NOJS=1 disables scripts (A/B comparison); WB_OFFLINE=1 only uses the
// cache; WB_QUIET=1 hides console output.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "web/wdoc.h"
#include "web/wdom.h"
#include "web/wjs.h"
#include "web/wcookie.h"
int strncasecmp(const char*, const char*, unsigned long);
long time(long*);
char* strstr(const char*, const char*);
char* strdup(const char*);
char* strchr(const char*, int);

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static char* slurp(const char* path, int* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    b[n] = 0;
    *len = (int)n;
    return b;
}

static unsigned long long fnv(const char* s) {
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

struct resp { char* body; int len; int status; char* headers; char final_url[2048]; char ctype[128]; };

// GET through curl (cached). Returns 0 on transport failure.
static const char* g_top_url;   // page URL for cookie SameSite context (NULL = navigation)
static int http_get(const char* url, const char* method, const char* req_headers, const char* body, int blen, struct resp* r) {
    static char cookie[4096];
    int cl = wcookie_header(url, g_top_url, cookie, sizeof cookie);
    if (cl && strchr(cookie, '\'')) { cl = 0; cookie[0] = 0; }   // keep the shell quoting simple
    memset(r, 0, sizeof *r);
    static char cdir[512];
    if (!cdir[0]) {
        const char* home = getenv("HOME");
        snprintf(cdir, sizeof cdir, "%s/.cache", home ? home : "/tmp");
        mkdir(cdir, 0755);
        snprintf(cdir, sizeof cdir, "%s/.cache/wbrowse", home ? home : "/tmp");
        mkdir(cdir, 0755);
    }
    char key[64], bpath[700], hpath[700], mpath[700];
    char kin[8192];
    int local = !strncmp(url, "http://127.0.0.1", 16) || !strncmp(url, "http://localhost", 16);
    // a local test server is never cached: files are private to this process (parallel runs fetch the same url all the time)
    snprintf(kin, sizeof kin, "o3 %s %s %s %s %d", method, url, cookie, g_top_url ? g_top_url : "", local ? (int)getpid() : 0);
    unsigned long long kh = fnv(kin);
    for (int bi = 0; body && bi < blen; bi++) { kh ^= (unsigned char)body[bi]; kh *= 1099511628211ULL; }
    for (const char* hp = req_headers; hp && *hp; hp++) { kh ^= (unsigned char)*hp; kh *= 1099511628211ULL; }   // Range etc. select a different answer
    snprintf(key, sizeof key, "%016llx", kh);
    snprintf(bpath, sizeof bpath, "%s/%s.body", cdir, key);
    snprintf(hpath, sizeof hpath, "%s/%s.hdr", cdir, key);
    snprintf(mpath, sizeof mpath, "%s/%s.meta", cdir, key);
    struct stat st;
    if (!strncmp(url, "http://127.0.0.1", 16) || !strncmp(url, "http://localhost", 16)) unlink(mpath);   // local test servers are never cached
    if (stat(mpath, &st) != 0) {
        if (getenv("WB_OFFLINE")) return 0;
        char cmd[12000];
        char hdrargs[8192] = "";
        int hn = cl ? snprintf(hdrargs, sizeof hdrargs, " -H 'Cookie: %s'", cookie) : 0;
        if (g_top_url) {    // a cross-origin request carries the page's Origin (servers answer Access-Control-Allow-Origin only for it)
            char to[300], uo[300];
            int tn = 0, un = 0, sl = 0;
            for (const char* q = g_top_url; *q && tn < 299; q++) { if (*q == '/' && ++sl == 3) break; to[tn++] = *q; }
            to[tn] = 0; sl = 0;
            for (const char* q = url; *q && un < 299; q++) { if (*q == '/' && ++sl == 3) break; uo[un++] = *q; }
            uo[un] = 0;
            if (strcmp(to, uo) && !strchr(to, '\'')) hn += snprintf(hdrargs + hn, sizeof hdrargs - hn, " -H 'Origin: %s'", to);
        }
        if (req_headers && req_headers[0]) {
            const char* p = req_headers;
            int n = hn;
            while (*p && n < 8000) {
                const char* e = strstr(p, "\r\n");
                int l = e ? (int)(e - p) : (int)strlen(p);
                if (l > 0 && !strchr(p, '\'')) n += snprintf(hdrargs + n, sizeof hdrargs - n, " -H '%.*s'", l, p);
                if (!e) break;
                p = e + 2;
            }
        }
        char bodyarg[700] = "";
        if (body && strcmp(method, "GET")) {
            char rq[600];
            snprintf(rq, sizeof rq, "%s/post_%s.bin", cdir, key);   // kept: the request bodies of a session can be inspected
            FILE* bf = fopen(rq, "wb");
            if (bf) { fwrite(body, 1, blen, bf); fclose(bf); }
            snprintf(bodyarg, sizeof bodyarg, " --data-binary @%s", rq);
        }
        snprintf(cmd, sizeof cmd,
                 "curl -s -L --max-time 30 --compressed -A 'Mozilla/5.0 (X11; okernel i686) okai/0.6' -X %s%s%s "
                 "-D %s -o %s -w '%%{http_code} %%{url_effective} %%{content_type}' '%s' > %s 2>/dev/null",
                 method, hdrargs, bodyarg, hpath, bpath, url, mpath);
        int rc = system(cmd);
        (void)rc;
    }
    int ml;
    char* meta = slurp(mpath, &ml);
    if (!meta) return 0;
    int status = 0;
    char fu[2048] = "", ct[128] = "";
    sscanf(meta, "%d %2047s %127[^\n]", &status, fu, ct);
    free(meta);
    if (!status) return 0;
    r->status = status;
    snprintf(r->final_url, sizeof r->final_url, "%s", fu[0] ? fu : url);
    snprintf(r->ctype, sizeof r->ctype, "%s", ct);
    r->body = slurp(bpath, &r->len);
    if (!r->body) { r->body = calloc(1, 1); r->len = 0; }
    if (local) { unlink(bpath); unlink(mpath); }
    int hl;
    char* h = slurp(hpath, &hl);
    if (local) unlink(hpath);
    // cookies from every header block (redirect hops included)
    if (h) {
        for (char* line = h; line && *line;) {
            char* e = strchr(line, '\n');
            int ll = e ? (int)(e - line) : (int)strlen(line);
            if (ll > 11 && !strncasecmp(line, "set-cookie:", 11)) {
                char* v = line + 11;
                int vl = ll - 11;
                while (vl > 0 && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
                while (vl > 0 && *v == ' ') { v++; vl--; }
                wcookie_set_http(r->final_url, v, vl);
            }
            line = e ? e + 1 : 0;
        }
    }
    // keep only the last header block (after redirects), drop the status line
    if (h) {
        char* last = h;
        for (char* p = h; (p = strstr(p, "HTTP/")); p++) if (p == h || p[-1] == '\n') last = p;
        char* nl = strchr(last, '\n');
        r->headers = strdup(nl ? nl + 1 : "");
        free(h);
    }
    return 1;
}

static void save_ppm(const char* path, struct wsurf* s) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    unsigned char* row = malloc(s->w * 3);
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            uint32_t p = s->px[y * s->stride + x];
            row[x * 3] = (unsigned char)(p >> 16);
            row[x * 3 + 1] = (unsigned char)(p >> 8);
            row[x * 3 + 2] = (unsigned char)p;
        }
        fwrite(row, 1, s->w * 3, f);
    }
    free(row);
    fclose(f);
}

static int quiet;
static void log_sink(const char* s) { if (!quiet) fputs(s, stdout); }
static int t_base_ms;
static int host_now(void) { return (int)now_ms() - t_base_ms; }
static long long host_epoch(void) { return (long long)time(0); }

// overridden by src/web/image.c when linked
__attribute__((weak)) int wimage_decode(const uint8_t* data, int len, int max_dim, int* w, int* h, uint32_t** px) {
    (void)data; (void)len; (void)max_dim; (void)w; (void)h; (void)px;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s url|file.html [w] [h] [out.ppm] [seconds]\n", argv[0]); return 2; }
    int vw = argc > 2 ? atoi(argv[2]) : 1280;
    int vh = argc > 3 ? atoi(argv[3]) : 800;
    const char* out = argc > 4 ? argv[4] : "tests/web/out/wbrowse.ppm";
    double seconds = argc > 5 ? atof(argv[5]) : 8;
    quiet = getenv("WB_QUIET") != 0;
    { extern int lay_memo_enabled; if (getenv("WB_NOMEMO")) lay_memo_enabled = 0; }
    t_base_ms = (int)now_ms();
    wjs_host.now_ms = host_now;
    wjs_host.log = log_sink;
    wcookie_now = host_epoch;

    char* html;
    int len;
    char url[2048];
    struct resp page;
    memset(&page, 0, sizeof page);
    // WB_FOLLOW=N: follow up to N script navigations (location.href = ...)
    int follow = getenv("WB_FOLLOW") ? atoi(getenv("WB_FOLLOW")) : 0;
    char next_url[2048];
    next_url[0] = 0;
    const char* start = argv[1];
again:
    g_top_url = 0;
    if (!strncmp(start, "http://", 7) || !strncmp(start, "https://", 8)) {
        if (!http_get(start, "GET", 0, 0, 0, &page) || !page.body) { fprintf(stderr, "fetch failed: %s\n", start); return 1; }
        html = page.body;
        len = page.len;
        if (getenv("WB_BODY")) {                       // WB_BODY=file.html: use this as the document served at the URL (a page captured from another network/region)
            int bl;
            char* bb = slurp(getenv("WB_BODY"), &bl);
            if (bb) { html = bb; len = bl; }
        }
        snprintf(url, sizeof url, "%s", page.final_url);
    } else {
        html = slurp(argv[1], &len);
        if (!html) { perror(argv[1]); return 1; }
        char cwd[1024];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(url, sizeof url, "file://%s/%s", cwd, argv[1]);
    }
    double t0 = now_ms();
    struct wdoc* d = wdoc_new();
    wdoc_set_viewport(d, vw, vh);
    wdoc_set_scripting(d, getenv("WB_NOJS") ? 0 : 1);
    wdoc_load(d, url, html, len, 0);
    double t1 = now_ms();
    int fetched = 0, failed = 0, js_runs = 0;
    double js_ms = 0, up_ms = 0;                 // time spent inside the engine: page scripts, and re-rendering after them
    double deadline = now_ms() + seconds * 1000;
    double mid_at = getenv("WB_MID_EVAL") ? now_ms() + (getenv("WB_MID_AFTER") ? atof(getenv("WB_MID_AFTER")) : 6000) : 0;   // WB_MID_EVAL=file.js: run it once, WB_MID_AFTER ms after the start, WHILE the load loop (timers, fetches) keeps running
    char rurl[4096];
    struct wjs* js = wdoc_js(d);
    g_top_url = url;
    for (;;) {
        double tf = now_ms();
        int id = wdoc_next_fetch(d, rurl, sizeof rurl);
        if (id >= 0) {
            const char *method, *hdrs, *body;
            int blen;
            wdoc_fetch_info(d, id, &method, &hdrs, &body, &blen);
            struct resp r;
            int ok;
            if (!strncmp(rurl, "data:", 5)) {
                char mime[64];
                int dl;
                char* bytes = wdoc_data_url(rurl, (int)strlen(rurl), &dl, mime, sizeof mime);
                memset(&r, 0, sizeof r);
                ok = bytes != 0;
                if (ok) { r.body = bytes; r.len = dl; r.status = 200; snprintf(r.final_url, sizeof r.final_url, "%s", rurl); }
            } else if (!strncmp(rurl, "file://", 7)) {
                memset(&r, 0, sizeof r);
                r.body = slurp(rurl + 7, &r.len);
                ok = r.body != 0;
                r.status = ok ? 200 : 0;
                snprintf(r.final_url, sizeof r.final_url, "%s", rurl);
            } else { tf = now_ms(); ok = http_get(rurl, method, hdrs, body, blen, &r); }
            double tg = now_ms();
            if (ok && r.body) {
                if (!quiet && getenv("WB_NET")) printf("[net] %d %s (%d bytes)\n", r.status, rurl, r.len);
                wdoc_fetch_done2(d, id, r.body, r.len, r.ctype, r.status, r.headers, r.final_url);
                if (getenv("WB_TRACE")) printf("[trace] get %.0fms deliver %.0fms %.100s\n", tg - tf, now_ms() - tg, rurl);
                fetched++;
            } else {
                if (!quiet) printf("[net] FAILED %s\n", rurl);
                wdoc_fetch_done2(d, id, 0, -1, "", 0, 0, rurl);
                failed++;
            }
            free(r.body);
            free(r.headers);
        }
        if (js && mid_at && now_ms() >= mid_at) {
            int el; char* es = slurp(getenv("WB_MID_EVAL"), &el);
            mid_at = 0;
            if (es) { es = realloc(es, (size_t)el + 1); es[el] = 0; wjs_debug_eval(js, es); free(es); deadline = now_ms() + (getenv("WB_MID_FOR") ? atof(getenv("WB_MID_FOR")) : 8000); }
        }
        if (js) {
            double tr = now_ms();
            int f = wjs_run(js, 500);
            js_ms += now_ms() - tr;
            {   // script focus(): applied like the shell does (the focused node and its ancestors get the :focus / :focus-within flags)
                int fnode;
                if ((f & WJS_FOCUS) && wjs_take_focus(js, &fnode)) {
                    struct wdom* dd = wdoc_dom(d);
                    static int cur_focus = -1;
                    if (cur_focus >= 0 && cur_focus < dd->nn) {
                        dd->n[cur_focus].flags &= ~WNF_FOCUSED;
                        for (int q = cur_focus; q >= 0; q = dd->n[q].parent) dd->n[q].flags &= ~WNF_FOCUS_WITHIN;
                    }
                    cur_focus = fnode >= 0 && fnode < dd->nn ? fnode : -1;
                    if (cur_focus >= 0) {
                        dd->n[cur_focus].flags |= WNF_FOCUSED;
                        for (int q = cur_focus; q >= 0; q = dd->n[q].parent) dd->n[q].flags |= WNF_FOCUS_WITHIN;
                    }
                    wdoc_set_focus(d, cur_focus);
                    wdoc_invalidate(d);
                    f |= WJS_DIRTY;
                }
            }
            if (f & WJS_DIRTY) { double tu = now_ms(); wdoc_update(d); up_ms += now_ms() - tu; }        // the shell re-renders after script changes (observers measure the new layout)
            js_runs++;
            if (getenv("WB_TRACE") && now_ms() - tr > 50) printf("[trace] wjs_run %.0fms flags=%x\n", now_ms() - tr, f);
            char nav[2048];
            int rep;
            if (wjs_take_nav(js, nav, sizeof nav, &rep)) {
                if (!quiet) printf("[wb] script navigation -> %s\n", nav);
                if (follow > 0 && nav[0] != '#') { snprintf(next_url, sizeof next_url, "%s", nav); break; }
            }
        }
        if (js) {                                   // <video>/<audio>: the shell's main loop drives playback
            struct wjs_rect rr[8];
            int nn;
            wjs_media_poll(js, rr, 8, &nn);
        }
        if (id >= 0) continue;
        wdoc_height(d);                             // lays out: images only a layout finds (CSS backgrounds) are queued now, like the shell does while it renders
        int due = js ? wjs_next_due(js) : -1;
        if (js && wjs_media_active(js)) due = 5;
        if (now_ms() > deadline) break;
        if (due < 0 && wdoc_pending(d) > 0) due = 1;    // a request queued by the script that just ran
        if (due < 0) break;
        if (due > 0) usleep((due > 50 ? 50 : due) * 1000);
    }
    double t2 = now_ms();
    if (js && getenv("WB_EVAL")) { int el; char* es = slurp(getenv("WB_EVAL"), &el); if (es) { es = realloc(es, (size_t)el + 1); es[el] = 0; wjs_debug_eval(js, es); free(es); } }   // WB_EVAL=file.js: run in the page after the load
    if (js && getenv("WB_EVAL") && getenv("WB_EVAL_WAIT")) {      // WB_EVAL_WAIT=ms: keep the timers running afterwards (what the snippet scheduled)
        double until = now_ms() + atof(getenv("WB_EVAL_WAIT"));
        while (now_ms() < until) { wjs_run(js, 500); usleep(5000); }
    }
    wdoc_update(d);
    if (getenv("WB_RESTYLE")) {                // WB_RESTYLE=N: restyle + relayout the loaded page N times (profiling the cascade)
        int n = atoi(getenv("WB_RESTYLE"));
        double r0 = now_ms();
        for (int i = 0; i < n; i++) { wdoc_invalidate(d); wdoc_update(d); }
        printf("WB restyle x%d: %.1f ms each\n", n,(now_ms() - r0) / (n ? n : 1));
    }
    int dh = wdoc_height(d);
    int hh = vh;
    struct wsurf s;
    s.w = vw;
    s.h = hh;
    s.stride = vw;
    s.px = calloc((size_t)vw * hh, 4);
    ws_reset_clip(&s);
    wdoc_paint(d, &s, 0);
    if (getenv("WB_PAINT")) {                  // WB_PAINT=N: paint the first viewport N more times (profiling the painter)
        int n = atoi(getenv("WB_PAINT"));
        double p0 = now_ms();
        for (int i = 0; i < n; i++) wdoc_paint(d, &s, 0);
        printf("WB paint x%d: %.1f ms each (%dx%d)\n", n, (now_ms() - p0) / (n ? n : 1), vw, hh);
    }
    double t3 = now_ms();
    save_ppm(out, &s);
    char stats[256], jst[512] = "";
    wdoc_stats(d, stats, sizeof stats);
    if (js) wjs_stats(js, jst, sizeof jst);
    printf("WB %s: %s doc_h=%d | %s | parse=%.0fms net+js=%.0fms (scripts %.0fms, updates %.0fms) paint=%.0fms fetched=%d failed=%d title=\"%s\"\n",
           argv[1], stats, dh, jst, t1 - t0, t2 - t1, js_ms, up_ms, t3 - t2, fetched, failed, wdoc_title(d));
    if (getenv("WB_DUMP")) { extern void wdoc_dump_layout(struct wdoc*); wdoc_dump_layout(d); }
    free(s.px);
    wdoc_free(d);
    if (html != page.body) free(html);
    free(page.body);
    free(page.headers);
    memset(&page, 0, sizeof page);
    if (next_url[0] && follow-- > 0) {
        static char hop[2048];
        snprintf(hop, sizeof hop, "%s", next_url);
        next_url[0] = 0;
        start = hop;
        printf("[wb] following -> %s (cookies: %d)\n", hop, wcookie_count());
        goto again;
    }
    return 0;
}
