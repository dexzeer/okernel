// okai — the web browser shell over the src/web engine (see okai.h).
//
// Pipeline: the desktop loop calls okai_poll() every iteration. A tab that
// needs its document fetches it through the fetch engine (net/fetch.c:
// parallel requests over pooled keep-alive connections); the response is
// header-parsed, dechunked and gunzipped here, then handed to wdoc_load
// (HTML5 parse + stylesheet discovery). The page's stylesheets, scripts and
// images then stream in, up to OKAI_SUB_PAR at a time per tab
// (wdoc_next_fetch / wdoc_fetch_done). Rendering is
// coalesced: the first paint waits for the stylesheets (bounded), later
// image arrivals repaint at most every ~1.5s. The engine paints the visible
// viewport into a per-window pixel surface that the window system blits
// (window_set_pixels); okai paints only its chrome as an overlay.
//
// Page scripts (QuickJS, src/web/wjs*.c) run inside the document: okai
// fetches their sources and fetch()/XHR requests through the same queue,
// pumps the realm (wjs_run: scripts, timers, frames, lifecycle events) from
// okai_poll, dispatches DOM events for clicks/keys/forms/scroll before
// applying default actions, and honors script navigation/scroll/focus.
//
// All engine work runs on a dedicated 2MB stack (call_on_stack): real pages
// nest deeper than the 256KB boot stack allows, and QuickJS recursion is
// capped at 900KB of it.
#include "okai.h"
#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "net/network.h"
#include "net/fetch.h"
#include "crypto/certverify.h" // CV_ERR_* for the warning-page reason line
#include "crypto/tls_client.h" // TLS_FAIL_*, tls_ticket_drop(), tls_response_complete()
#include "crypto/rand.h"
#include "web/wdoc.h"
#include "web/wdoc_int.h"
#include "web/wjs.h"
#include "web/wcookie.h"
#include "web/wcommon.h"
#include "web/wdom.h"
#include "web/wurl.h"
#include "web/image.h"
#include "web/surface.h"
#include <stdint.h>

// Wall-clock baseline for the tab open/close animation. okai_anim_step eases
// each tab's width toward its target once per 10ms tick (the PIT runs at 100Hz),
// so the animation duration is tied to real time, not the main-loop spin rate.
extern uint32_t tick_count;
static uint32_t okai_last_tick = 0;

// Tab chrome sizing / animation constants
#define OKAI_ANIM_FACTOR 18  // % of remaining width eased per 10ms tick (exponential
                             // ease-out; ~150ms to fully open/close a tab)
#define CHROME_PX (CHROME_TAB_H + CHROME_TOOL_H) // chrome band above the page, px
#define SCROLL_STEP 48       // j/k/arrows
#define WHEEL_STEP  80       // per wheel notch
#define CSS_WAIT_TICKS 300   // first paint waits at most 3s for stylesheets
#define IMG_RENDER_TICKS 150 // image arrivals repaint at most every 1.5s
#define MAX_BODY (16u << 20) // decompressed document cap

static struct okai okais[MAX_OKAIS];
static int okai_count = 0;

// A window with requests in flight (drives the progress-pill erase), or -1.
int okai_fetch_owner = -1;
int okai_scripts_on = 1;

// Chrome drawing + hit tests live in okai_ui.c.
int  okai_ui_tab_width(struct okai* b);
void okai_ui_paint(int id);
void okai_ui_painted_full(int id);
void okai_ui_forget(int id);

static void okai_tab_reset(struct okai_tab* T);
static void load_home(int id, int tab);
static void show_error(int id, int tab);
static void request_render(struct okai_tab* T, int delay);

// ---- small string helpers -----------------------------------------------------

static void scopy(char* dst, const char* src, int cap) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int iprefix(const char* s, const char* lit) {
    for (int i = 0; lit[i]; i++)
        if (lower((unsigned char)s[i]) != lit[i]) return 0;
    return 1;
}

static int url_is_https(const char* u) { return iprefix(u, "https:"); }
static int url_is_http(const char* u) { return iprefix(u, "http:") || iprefix(u, "https:"); }

// ---- engine jobs on the private stack ----------------------------------------------

extern void call_on_stack(void (*fn)(void*), void* arg, void* stack_top);

#define ENGINE_STACK (2u << 20) // layout peaks ~20KB; QuickJS may use up to 900KB
#define STACK_FILL 0x5AC4F00Du
static uint8_t* engine_stack;
static int in_engine;

enum { JOB_LOAD, JOB_FETCH_DONE, JOB_PAINT, JOB_HIT, JOB_RECT, JOB_ANCHOR, JOB_INFLATE, JOB_LINKS,
       JOB_JS_RUN, JOB_JS_EVENT, JOB_ELEM_AT, JOB_FREE };

struct job {
    int op;
    struct wdoc* doc;
    const char* url;
    const char* bytes;
    int len;
    const char* charset;
    int id;
    struct wsurf* surf;
    int scroll, vw, vh;
    int x, y, w, h;
    struct wdoc_hit* hit;
    const char* frag;
    uint8_t* out;
    int out_len;
    int result;
    int status;               // JOB_FETCH_DONE: HTTP status (0 = derive)
    const char* headers;      // JOB_FETCH_DONE: raw response headers
    const char* final_url;    // JOB_FETCH_DONE: URL after redirects
    const char* type;         // JOB_JS_EVENT: event type
    int button, key;          // JOB_JS_EVENT
};

static void job_entry(void* p) {
    struct job* j = (struct job*)p;
    switch (j->op) {
    case JOB_LOAD:
        j->result = wdoc_load(j->doc, j->url, j->bytes, j->len, j->charset);
        break;
    case JOB_FETCH_DONE: {
        int st = j->status ? j->status : ((j->len >= 0 && j->bytes) ? 200 : 0);
        wdoc_fetch_done2(j->doc, j->id, j->bytes, j->len, j->charset, st, j->headers, j->final_url);
        break;
    }
    case JOB_JS_RUN: {
        struct wjs* js = wdoc_js(j->doc);
        j->result = js ? wjs_run(js, j->len) : 0;
        break;
    }
    case JOB_JS_EVENT: {
        struct wjs* js = wdoc_js(j->doc);
        int prevented = 0;
        j->result = js ? wjs_event(js, j->id, j->type, j->x, j->y, j->button, j->key, &prevented) : 0;
        j->h = prevented;
        break;
    }
    case JOB_ELEM_AT:
        j->result = wdoc_hit_element(j->doc, j->x, j->y);
        break;
    case JOB_FREE:
        wdoc_free(j->doc);
        break;
    case JOB_PAINT:
        // vw/vh > 0: full paint (viewport may have changed); else a band
        // repaint into the clip already set on the surface.
        if (j->vw > 0) wdoc_set_viewport(j->doc, j->vw, j->vh);
        j->result = wdoc_height(j->doc);
        if (j->vw > 0) {
            int maxs = j->result - j->vh;
            if (maxs < 0) maxs = 0;
            if (j->scroll > maxs) j->scroll = maxs;
            if (j->scroll < 0) j->scroll = 0;
            j->h = wdoc_has_fixed(j->doc);
        }
        wdoc_paint(j->doc, j->surf, j->scroll);
        break;
    case JOB_HIT:
        j->result = wdoc_hit(j->doc, j->x, j->y, j->scroll, j->hit);
        break;
    case JOB_RECT:
        j->result = wdoc_node_rect(j->doc, j->id, &j->x, &j->y, &j->w, &j->h);
        break;
    case JOB_ANCHOR:
        j->result = wdoc_anchor_y(j->doc, j->frag);
        break;
    case JOB_INFLATE:
        j->out = winflate_gzip((const uint8_t*)j->bytes, j->len, &j->out_len, (int)MAX_BODY);
        break;
    case JOB_LINKS: {
        // Log the link/control regions in the viewport (headless tests aim
        // clicks with these; page px relative to the page origin).
        // j->x = 1: only when they changed since hash j->result (scripts
        // move content around); the new hash is returned in j->result.
        int n = wdoc_hit_count(j->doc), shown = 0;
        static struct wdoc_region r;
        int seen[48];
        uint32_t hash = 2166136261u;
        for (int i = 0; i < n; i++) {
            if (!wdoc_hit_get(j->doc, i, &r)) continue;
            int y = r.fixed ? r.y : r.y - j->scroll;
            if (y + r.h <= 0 || y >= j->vh || r.w <= 0 || r.h <= 0) continue;
            int v[6] = { r.kind, r.x, y, r.w, r.h, r.node };
            for (int k = 0; k < 6; k++) { hash ^= (uint32_t)v[k]; hash *= 16777619u; }
        }
        if (j->x && (uint32_t)j->result == hash) break;
        j->result = (int)hash;
        for (int i = 0; i < n && shown < 48; i++) {
            if (!wdoc_hit_get(j->doc, i, &r)) continue;
            int y = r.fixed ? r.y : r.y - j->scroll;
            if (y + r.h <= 0 || y >= j->vh || r.w <= 0 || r.h <= 0) continue;
            int dup = 0; // one line per element (its first, outermost region)
            for (int k = 0; k < shown; k++) if (seen[k] == r.node) { dup = 1; break; }
            if (dup) continue;
            seen[shown] = r.node;
            if (r.kind == WDOC_HIT_LINK)
                serial_printf("[okai] link[%d] x=%d y=%d w=%d h=%d href=%s\n",
                              shown, r.x, y, r.w, r.h, r.href);
            else
                serial_printf("[okai] field[%d] x=%d y=%d w=%d h=%d node=%d btn=%d\n",
                              shown, r.x, y, r.w, r.h, r.node, r.kind == WDOC_HIT_BUTTON);
            shown++;
        }
        break;
    }
    }
}

static void run_job(struct job* j) {
    if (!engine_stack) {
        engine_stack = (uint8_t*)kmalloc(ENGINE_STACK);
        if (engine_stack) {
            uint32_t* w = (uint32_t*)engine_stack;
            for (uint32_t i = 0; i < ENGINE_STACK / 4; i++) w[i] = STACK_FILL;
        } else serial_puts("[okai] engine stack alloc failed — using the boot stack\n");
    }
    if (!engine_stack || in_engine) { job_entry(j); return; }
    in_engine = 1;
    call_on_stack(job_entry, j, engine_stack + ENGINE_STACK);
    in_engine = 0;
}

// Free a document on the engine stack (its script realm tears down there).
static void doc_free(struct wdoc* d) {
    if (!d) return;
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_FREE;
    j.doc = d;
    run_job(&j);
}

// Deepest engine stack use so far (KB), from the fill pattern.
static int engine_stack_hwm_kb(void) {
    if (!engine_stack) return 0;
    const uint32_t* w = (const uint32_t*)engine_stack;
    uint32_t i = 0;
    while (i < ENGINE_STACK / 4 && w[i] == STACK_FILL) i++;
    return (int)((ENGINE_STACK - i * 4) / 1024);
}

// ---- HTTP response handling -------------------------------------------------------

struct resp_info {
    int status;
    int gzip;
    char ctype[64];
    char charset[32];
    char location[OKAI_URL_LEN];
};

static int header_end(const char* r, int len) {
    for (int i = 0; i + 3 < len; i++)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') return i;
    for (int i = 0; i + 1 < len; i++)
        if (r[i] == '\n' && r[i + 1] == '\n') return i;
    return -1;
}

// Value of header `name` (lowercase) within r[0..hend), or -1.
static int header_get(const char* r, int hend, const char* name, const char** val) {
    int nl = (int)strlen(name);
    int p = 0;
    while (p < hend) {
        int e = p;
        while (e < hend && r[e] != '\n') e++;
        if (e - p > nl && r[p + nl] == ':') {
            int ok = 1;
            for (int k = 0; k < nl; k++) if (lower((unsigned char)r[p + k]) != name[k]) { ok = 0; break; }
            if (ok) {
                int v = p + nl + 1;
                while (v < e && (r[v] == ' ' || r[v] == '\t')) v++;
                int ve = e;
                while (ve > v && (r[ve - 1] == '\r' || r[ve - 1] == ' ' || r[ve - 1] == '\t')) ve--;
                *val = r + v;
                return ve - v;
            }
        }
        p = e + 1;
    }
    return -1;
}

static void parse_resp(const char* r, int len, struct resp_info* ri) {
    ri->status = 0;
    ri->gzip = 0;
    ri->ctype[0] = ri->charset[0] = ri->location[0] = 0;
    if (len < 12 || !(r[0] == 'H' && r[1] == 'T' && r[2] == 'T' && r[3] == 'P' && r[4] == '/')) return;
    int sp = 0;
    while (sp < len && r[sp] != ' ') sp++;
    for (int k = sp + 1; k < sp + 4 && k < len && r[k] >= '0' && r[k] <= '9'; k++)
        ri->status = ri->status * 10 + (r[k] - '0');
    int he = header_end(r, len);
    if (he < 0) he = len;
    const char* v;
    int vl = header_get(r, he, "content-type", &v);
    if (vl > 0) {
        int i = 0;
        while (i < vl && v[i] != ';' && i < 63) { ri->ctype[i] = (char)lower((unsigned char)v[i]); i++; }
        ri->ctype[i] = 0;
        for (int k = 0; k + 8 <= vl; k++) {
            if (iprefix(v + k, "charset=")) {
                int s = k + 8, n = 0;
                if (s < vl && (v[s] == '"' || v[s] == '\'')) s++;
                while (s < vl && n < 31 && v[s] != ';' && v[s] != '"' && v[s] != '\'' && v[s] != ' ')
                    ri->charset[n++] = v[s++];
                ri->charset[n] = 0;
                break;
            }
        }
    }
    vl = header_get(r, he, "content-encoding", &v);
    if (vl >= 4) for (int k = 0; k + 4 <= vl; k++) if (iprefix(v + k, "gzip")) { ri->gzip = 1; break; }
    vl = header_get(r, he, "location", &v);
    if (vl > 0) {
        if (vl > OKAI_URL_LEN - 1) vl = OKAI_URL_LEN - 1;
        memcpy(ri->location, v, vl);
        ri->location[vl] = 0;
    }
}

// Strip headers + dechunk in place, then gunzip when encoded. Returns the
// body; *heap = 1 if it is a kmalloc'd buffer the caller must free.
static char* resp_body(char* resp, int len, const struct resp_info* ri, int* blen, int* heap) {
    *heap = 0;
    int n = http_dechunk(resp, len);
    if (n < 0) n = 0;
    *blen = n;
    if (ri->gzip && n >= 18 && (uint8_t)resp[0] == 0x1F && (uint8_t)resp[1] == 0x8B) {
        struct job j;
        memset(&j, 0, sizeof j);
        j.op = JOB_INFLATE;
        j.bytes = resp;
        j.len = n;
        run_job(&j);
        if (j.out) {
            *heap = 1;
            *blen = j.out_len;
            return (char*)j.out;
        }
        serial_printf("[okai] gzip body failed to inflate (%d bytes)\n", n);
    }
    return resp;
}

// ---- requests -----------------------------------------------------------------------

// In-flight requests. The fetch engine (net/fetch.c) runs up to FETCH_MAX
// GETs at once over pooled keep-alive connections; each record ties a
// fetch handle to its tab and to what it is for: the tab's main document
// (sub_id -1) or one resource of the document `doc`.
#define OKAI_SUB_PAR 6         // resources in flight per tab
struct oreq {
    int used;
    int bi, tab;               // window + tab index (kept in sync on close)
    struct wdoc* doc;          // document a resource belongs to (NULL = main)
    int sub_id;                // wdoc resource id, -1 = the main document
    int https;
    int redirects, retried;
    int cors;                  // script request (fetch/XHR): send Origin cross-origin
    int h;                     // fetch handle, -1 = none
    char url[OKAI_URL_LEN];
};
static struct oreq reqs[FETCH_MAX];

// scheme://host[:port] of an absolute URL, lowercased ("" if none).
static void url_origin(const char* u, char* out, int cap) {
    int i = 0, slashes = 0;
    for (; u[i] && i < cap - 1; i++) {
        if (u[i] == '/' && ++slashes == 3) break;
        out[i] = (char)lower((unsigned char)u[i]);
    }
    out[i] = 0;
    if (slashes < 2) out[0] = 0;
}

static struct oreq* req_new(int bi, int tab, struct wdoc* doc, int sub_id, const char* url) {
    for (int i = 0; i < FETCH_MAX; i++) {
        struct oreq* q = &reqs[i];
        if (q->used) continue;
        q->used = 1;
        q->bi = bi;
        q->tab = tab;
        q->doc = doc;
        q->sub_id = sub_id;
        q->https = url_is_https(url);
        q->redirects = q->retried = 0;
        q->cors = 0;
        q->h = -1;
        scopy(q->url, url, OKAI_URL_LEN);
        return q;
    }
    return 0;
}

static void req_free(struct oreq* q) {
    if (q->h >= 0) fetch_end(q->h);
    q->h = -1;
    q->used = 0;
}

// Start the GET for q->url. top: the page a resource belongs to (cookie
// SameSite context), NULL for a top-level navigation. 0 = started, -1 = no
// host in the URL, -2 = the fetch engine is full (try again later).
static int req_fetch(struct oreq* q, const char* top) {
    char host[128];
    static char path[OKAI_URL_LEN];
    static char cookie_hdr[NET_EXTRA_MAX];
    if (wurl_host(q->url, host, sizeof host) <= 0) return -1;
    if (wurl_path(q->url, path, sizeof path) <= 0) { path[0] = '/'; path[1] = 0; }
    int port = wurl_port(q->url);
    if (port < 0) port = 0;
    struct fetch_opts o;
    o.gzip = 1;
    o.accept_html = top == 0;
    o.extra = 0;
    int n = 0;
    memcpy(cookie_hdr, "Cookie: ", 8);
    int cl = wcookie_header(q->url, top, cookie_hdr + 8, NET_EXTRA_MAX - 512);
    if (cl > 0) {
        n = 8 + cl;
        cookie_hdr[n++] = '\r';
        cookie_hdr[n++] = '\n';
    }
    // fetch()/XHR across origins carry Origin (CORS): servers answer
    // Access-Control-Allow-Origin from it — without it BBC's sign-in check
    // failed and its script bounced bbc.com <-> bbc.co.uk forever.
    if (q->cors && top) {
        char oa[256], ob[256];
        url_origin(top, oa, sizeof oa);
        url_origin(q->url, ob, sizeof ob);
        if (oa[0] && strcmp(oa, ob)) {
            const char* pre = "Origin: ";
            while (*pre) cookie_hdr[n++] = *pre++;
            for (int k = 0; oa[k] && n < NET_EXTRA_MAX - 3; k++) cookie_hdr[n++] = oa[k];
            cookie_hdr[n++] = '\r';
            cookie_hdr[n++] = '\n';
        }
    }
    cookie_hdr[n] = 0;
    if (n > 0) o.extra = cookie_hdr;
    q->https = url_is_https(q->url);
    int h = fetch_begin(host, path, (uint16_t)port, q->https, &o);
    if (h < 0) return -2;
    q->h = h;
    return 0;
}

static int req_has_main(int bi, int tab) {
    for (int i = 0; i < FETCH_MAX; i++)
        if (reqs[i].used && reqs[i].bi == bi && reqs[i].tab == tab && reqs[i].sub_id < 0) return 1;
    return 0;
}

// Cancel every request of a tab (navigation, close).
static void req_cancel_tab(int bi, int tab) {
    for (int i = 0; i < FETCH_MAX; i++)
        if (reqs[i].used && reqs[i].bi == bi && reqs[i].tab == tab) req_free(&reqs[i]);
    struct okai* b = (bi >= 0 && bi < MAX_OKAIS) ? &okais[bi] : 0;
    if (b && tab >= 0 && tab < OKAI_MAX_TABS) b->tabs[tab].sub_inflight = 0;
}

// A tab left the array: drop its requests, renumber the tabs after it.
static void req_tab_removed(int bi, int tab) {
    req_cancel_tab(bi, tab);
    for (int i = 0; i < FETCH_MAX; i++)
        if (reqs[i].used && reqs[i].bi == bi && reqs[i].tab > tab) reqs[i].tab--;
}

// A window left the array (windows compact): same for window ids.
static void req_window_removed(int bi) {
    for (int t = 0; t < OKAI_MAX_TABS; t++) req_cancel_tab(bi, t);
    for (int i = 0; i < FETCH_MAX; i++)
        if (reqs[i].used && reqs[i].bi > bi) reqs[i].bi--;
}

// Every Set-Cookie of a response (redirect hops included) into the jar.
static void store_cookies(const char* url, const char* r, int len) {
    int he = header_end(r, len);
    int i = 0;
    while (i < he) {
        int e = i;
        while (e < he && r[e] != '\n') e++;
        int ll = e - i;
        if (ll > 11 && (r[i] | 32) == 's' && w_ieq_n(r + i, "set-cookie:", 11)) {
            const char* v = r + i + 11;
            int vl = ll - 11;
            while (vl > 0 && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            while (vl > 0 && *v == ' ') { v++; vl--; }
            wcookie_set_http(url, v, vl);
        }
        i = e + 1;
    }
}

extern long long time(long long* t);   // qjs_libc: RTC + ticks
static long long okai_epoch(void) { return time(0); }

// Default a web URL to HTTPS unless the user typed an explicit scheme.
// An explicit `https://` is kept; an explicit `http://` is RESPECTED as plain
// HTTP (typing the scheme is a deliberate choice); a bare host with no scheme
// defaults to HTTPS. The home/internal scheme and other schemes are untouched.
static void okai_normalize_https(const char* in, char* out, int outlen) {
    while (*in == ' ') in++;
    int has_scheme = 0;
    for (int i = 0; in[i] && in[i] != '/'; i++) {
        if (in[i] == ':') { has_scheme = 1; break; }
    }
    // host:port (digits after the colon) is not a scheme
    if (has_scheme) {
        int c = 0;
        while (in[c] && in[c] != ':') c++;
        if (in[c + 1] >= '0' && in[c + 1] <= '9') has_scheme = 0;
    }
    int j = 0;
    if (!has_scheme) {
        // IP literals and explicit non-443 ports are local/dev servers:
        // plain HTTP (a TLS handshake against them fails closed, no fallback).
        int h = 0, digits_dots = 1, port = -1;
        while (in[h] && in[h] != '/' && in[h] != ':' && in[h] != '?' && in[h] != '#') {
            if (!((in[h] >= '0' && in[h] <= '9') || in[h] == '.')) digits_dots = 0;
            h++;
        }
        if (in[h] == ':') {
            port = 0;
            for (int k = h + 1; in[k] >= '0' && in[k] <= '9'; k++) port = port * 10 + (in[k] - '0');
        }
        const char* s = (digits_dots && h > 0) || (port > 0 && port != 443) ? "http://" : "https://";
        while (s[j] && j < outlen - 1) { out[j] = s[j]; j++; }
    }
    for (int i = 0; in[i] && j < outlen - 1; i++) out[j++] = in[i];
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = 0;
}

// Rewrite an in-place URL from https:// to http:// (used for the one-time
// HTTP fallback after an HTTPS fetch definitively fails).
static void okai_rewrite_scheme_http(char* url) {
    if (strncmp(url, "https:", 6) == 0) {
        int n = 0; while (url[n]) n++;
        for (int i = 4; i < n; i++) url[i] = url[i + 1]; // drop the 's' after "http"
    }
}

int okai_is_home(const char* url) { return strcmp(url, OKAI_HOME_URL) == 0; }

// ---- init / tabs -----------------------------------------------------------------

// ---- script realm host services --------------------------------------------------

static int okai_now_ms(void) { return (int)(tick_count * 10); }

static void okai_js_random(uint8_t* out, int n) {
    if (rand_bytes(out, (uint32_t)n)) return;
    // CPRNG not seeded yet (very early): never hand out zeros
    static uint32_t x = 0x6A09E667u;
    uint32_t t;
    __asm__ volatile("rdtsc" : "=a"(t) :: "edx");
    x ^= t;
    for (int i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; out[i] = (uint8_t)x; }
}

static void okai_js_log(const char* s) { serial_puts(s); }

void okai_init(void) {
    wjs_host.now_ms = okai_now_ms;
    wjs_host.random = okai_js_random;
    wjs_host.log = okai_js_log;
    wcookie_now = okai_epoch;
    for (int i = 0; i < MAX_OKAIS; i++) {
        okais[i].win_id = -1;
        okais[i].active_tab = 0;
        okais[i].tab_count = 0;
        okais[i].show_security = 0;
        okais[i].page_px = 0;
        okais[i].page_w = okais[i].page_h = okais[i].page_cap = 0;
        for (int t = 0; t < OKAI_MAX_TABS; t++) {
            okais[i].tabs[t].doc = 0;
            okai_tab_reset(&okais[i].tabs[t]);
            okais[i].tabs[t].anim_w = 0;
            okais[i].tabs[t].closing = 0;
        }
    }
    okai_count = 0;
    okai_fetch_owner = -1;
}

struct okai_tab* okai_tab_of(struct okai* b) { return &b->tabs[b->active_tab]; }

int okai_window_count(void) { return okai_count; }

// Per-load state (navigation keeps url/history/doc).
static void tab_reset_load(struct okai_tab* T) {
    T->load_state = 0;
    T->redirect_count = 0;
    T->https_fell_back = 0;
    T->conn_retries = 0;
    T->cert_failed = 0;
    T->cert_detail = 0;
    T->conn_failed = 0;
    T->conn_kind = 0;
    T->truncated = 0;
    T->sub_inflight = 0;
    T->sub_css = T->sub_img = 0;
    T->sub_landed = 0;
    T->render_pending = 0;
    T->render_due = 0;
    T->pending_frag[0] = 0;
    T->first_paint = 0;
}

static void okai_tab_reset(struct okai_tab* T) {
    if (T->doc) { doc_free(T->doc); T->doc = 0; }
    T->url[0] = 0;
    T->title[0] = 0;
    T->scroll_y = 0;
    T->content_height = 0;
    T->is_https = 0;
    T->focused_node = -1;
    T->history_count = 0;
    T->history_pos = 0;
    T->has_fixed = 0;
    T->last_render_tick = 0;
    T->load_tick = 0;
    tab_reset_load(T);
}

// A tab leaves the array (close animation finished / window closed).
static void tab_dispose(int id, int tab) {
    struct okai_tab* T = &okais[id].tabs[tab];
    if (T->doc) { doc_free(T->doc); T->doc = 0; }
    req_tab_removed(id, tab);
}

// Switching re-renders the tab's cached page — no refetch.
int okai_switch_tab(int id, int tab) {
    if (id < 0 || id >= okai_count) return -1;
    struct okai* b = &okais[id];
    if (tab < 0 || tab >= b->tab_count || tab == b->active_tab) return -1;
    b->active_tab = tab;
    b->chrome_dirty = 1;
    okai_render_content(id);
    serial_printf("[okai] switch tab -> %d (%s)\n", tab, b->tabs[tab].url);
    return tab;
}

int okai_new_tab(int id, const char* url) {
    if (id < 0 || id >= okai_count) return -1;
    struct okai* b = &okais[id];
    if (b->tab_count >= OKAI_MAX_TABS) {
        serial_puts("[okai] tab limit — reusing active tab\n");
        okai_navigate(id, url);
        return b->active_tab;
    }
    int tab = b->tab_count++;
    b->active_tab = tab;
    b->tabs[tab].doc = 0;
    okai_tab_reset(&b->tabs[tab]);
    b->tabs[tab].anim_w = 0;   // open animation: grow from zero width
    b->tabs[tab].closing = 0;
    okai_last_tick = tick_count; // start the ease-out cleanly from width 0
    okai_navigate(id, url);
    if (!b->tabs[tab].doc) okai_render_content(id); // blank until the page arrives
    return tab;
}

void okai_close_tab(int id, int tab) {
    if (id < 0 || id >= okai_count) return;
    struct okai* b = &okais[id];
    if (tab < 0 || tab >= b->tab_count) return;
    if (b->tab_count <= 1) { okai_close(id); return; } // last tab closes window
    // Animate: keep the tab in the array but flag it closing; okai_anim_step()
    // shrinks its width to zero and removes it once the animation finishes.
    b->tabs[tab].closing = 1;
    req_cancel_tab(id, tab);
    if (tab == b->active_tab) {
        // switch to a neighbour so the visible page updates immediately
        int nb = (tab + 1 < b->tab_count) ? tab + 1 : tab - 1;
        b->active_tab = nb;
        okai_render_content(id);
    }
    if (b->win_id >= 0) window_set_dirty(b->win_id);
    serial_printf("[okai] closing tab %d, active=%d\n", tab, b->active_tab);
}

// ---- built-in pages ----------------------------------------------------------------

static const char OKAI_HOME_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>okai</title><style>"
"html{background:linear-gradient(160deg,#0f2027 0%,#203a43 55%,#2c5364 100%);min-height:100%}"
"body{margin:0;font-family:'Noto Sans',sans-serif;color:#e8eef5}"
".hero{text-align:center;padding:72px 24px 24px}"
".logo{font-size:96px;font-weight:700;letter-spacing:-3px;color:#7db4f5;margin:0;line-height:1.1}"
".tag{color:#9fb3c8;font-size:21px;margin:10px 0 0}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:22px;"
"max-width:1180px;margin:36px auto;padding:0 28px}"
".card{display:block;background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.14);"
"border-radius:16px;padding:22px 24px;text-decoration:none;color:#e8eef5;"
"box-shadow:0 10px 28px rgba(0,0,0,.28)}"
".card b{display:block;font-size:21px;color:#fff;margin-bottom:6px}"
".card span{color:#9fb3c8;font-size:15px}"
".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:10px;"
"vertical-align:middle}"
".foot{text-align:center;color:#8aa0b6;font-size:15px;margin:44px 0 56px}"
"kbd{background:#16283a;border:1px solid #3a5068;border-radius:5px;padding:1px 7px;"
"font-family:'Noto Sans Mono',monospace;color:#cfe3ff}"
"</style></head><body>"
"<div class=hero><h1 class=logo>okai</h1>"
"<p class=tag>the okernel web browser</p></div>"
"<div class=grid>"
"<a class=card href=\"http://example.com/\"><b><i class=dot style=\"background:#7db4f5\"></i>example.com</b>"
"<span>The classic test page</span></a>"
"<a class=card href=\"https://en.wikipedia.org/wiki/Operating_system\"><b><i class=dot style=\"background:#f5f5f5\"></i>Wikipedia</b>"
"<span>Operating system &mdash; the free encyclopedia</span></a>"
"<a class=card href=\"https://news.ycombinator.com/\"><b><i class=dot style=\"background:#ff6600\"></i>Hacker News</b>"
"<span>news.ycombinator.com</span></a>"
"<a class=card href=\"http://info.cern.ch/\"><b><i class=dot style=\"background:#5fd38d\"></i>info.cern.ch</b>"
"<span>The first website</span></a>"
"<a class=card href=\"https://notdexy.ru/\"><b><i class=dot style=\"background:#c792ea\"></i>notdexy.ru</b>"
"<span>Home of KAnarchy</span></a>"
"<a class=card href=\"https://www.python.org/\"><b><i class=dot style=\"background:#ffd43b\"></i>python.org</b>"
"<span>Python programming language</span></a>"
"</div>"
"<p class=foot>Press <kbd>g</kbd> to type an address &nbsp;&middot;&nbsp; <kbd>j</kbd> <kbd>k</kbd> or the wheel to scroll"
" &nbsp;&middot;&nbsp; <kbd>Tab</kbd> cycles form fields</p>"
"</body></html>";

// HTML-escape src into out (returns new length).
static int html_esc(char* out, int at, int cap, const char* src) {
    for (int i = 0; src[i] && at < cap - 8; i++) {
        char c = src[i];
        const char* e = c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" :
                        c == '"' ? "&quot;" : c == '\'' ? "&#39;" : 0;
        if (e) while (*e) out[at++] = *e++;
        else out[at++] = ((unsigned char)c < 32) ? '?' : c;
    }
    out[at] = 0;
    return at;
}

static int html_put(char* out, int at, int cap, const char* s) {
    while (*s && at < cap - 1) out[at++] = *s++;
    out[at] = 0;
    return at;
}

// scripts: run page JavaScript (internal and error pages never do)
static void tab_load_html(int id, int tab, const char* html, int len, const char* charset, int scripts) {
    struct okai_tab* T = &okais[id].tabs[tab];
    if (!T->doc) T->doc = wdoc_new();
    if (!T->doc) { serial_puts("[okai] out of memory (wdoc_new)\n"); return; }
    wdoc_set_scripting(T->doc, scripts);
    T->js_next_tick = 0;
    T->js_scroll_evt = 0;
    T->sub_js = T->sub_req = 0;
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_LOAD;
    j.doc = T->doc;
    j.url = T->url;
    j.bytes = html;
    j.len = len;
    j.charset = charset;
    run_job(&j);
    T->scroll_y = 0;
    T->focused_node = -1;
    T->has_fixed = 0;
    T->first_paint = 0;
    const char* t = wdoc_title(T->doc);
    scopy(T->title, t && t[0] ? t : T->url, sizeof T->title);
    if (okais[id].active_tab == tab) {
        window_set_title(okais[id].win_id, T->title[0] ? T->title : "okai");
        okais[id].chrome_dirty = 1;
    }
}

static void load_home(int id, int tab) {
    struct okai_tab* T = &okais[id].tabs[tab];
    scopy(T->url, OKAI_HOME_URL, OKAI_URL_LEN);
    tab_reset_load(T);
    T->is_https = 0;
    tab_load_html(id, tab, OKAI_HOME_HTML, (int)sizeof(OKAI_HOME_HTML) - 1, "utf-8", 0);
    T->load_state = 1;
    request_render(T, 0);
    if (okais[id].active_tab == tab) okai_render_content(id);
    serial_puts("[br] home rendered\n");
}

// Error pages are ordinary documents (rendered by the engine like any page).
static void show_error(int id, int tab) {
    struct okai_tab* T = &okais[id].tabs[tab];
    static char h[8192];
    int n = 0;
    const char* title = "Unable to load page";
    const char* accent = "#ffb454";
    if (T->truncated) { title = "Response truncated"; accent = "#ff6b6b"; }
    else if (T->cert_failed) { title = "Security warning"; accent = "#ff6b6b"; }
    else if (T->conn_failed) title = T->conn_kind == OKAI_CONN_TOOLARGE ? "Page too large" : "Connection error";
    n = html_put(h, n, sizeof h, "<!DOCTYPE html><html><head><meta charset=utf-8><title>");
    n = html_put(h, n, sizeof h, title);
    n = html_put(h, n, sizeof h, "</title><style>"
        "html{background:#14181f}body{margin:0;font-family:'Noto Sans',sans-serif;color:#d6dde6}"
        ".box{max-width:760px;margin:72px auto;padding:36px 44px;background:#1d232d;border-radius:16px;"
        "border:1px solid #2c3542;box-shadow:0 14px 40px rgba(0,0,0,.45)}"
        "h1{margin:0 0 18px;font-size:34px}"
        ".u{font-family:'Noto Sans Mono',monospace;color:#ffd479;word-break:break-all;background:#151a22;"
        "padding:8px 12px;border-radius:8px;display:block;margin:10px 0 18px}"
        ".why{color:#ff9b9b}.dim{color:#8b97a6;font-size:15px}"
        "a{color:#7db4f5}</style></head><body><div class=box><h1 style=\"color:");
    n = html_put(h, n, sizeof h, accent);
    n = html_put(h, n, sizeof h, "\">");
    n = html_put(h, n, sizeof h, title);
    n = html_put(h, n, sizeof h, "</h1>");
    if (T->truncated) {
        n = html_put(h, n, sizeof h, "<p>The secure connection to</p><span class=u>");
        n = html_esc(h, n, sizeof h, T->url);
        n = html_put(h, n, sizeof h, "</span><p>ended mid-response. The page may have been cut by an attacker.</p>"
            "<p class=dim>Nothing was rendered and no HTTP fallback was attempted.</p>");
    } else if (T->cert_failed) {
        n = html_put(h, n, sizeof h, "<p>The certificate for</p><span class=u>");
        n = html_esc(h, n, sizeof h, T->url);
        n = html_put(h, n, sizeof h, "</span><p>failed verification.</p>");
        const char* why = 0;
        switch (T->cert_detail) {
        case CV_ERR_PINCHANGED: why = "site key changed since first visit"; break;
        case CV_ERR_EXPIRED: why = "certificate expired (or no clock)"; break;
        case CV_ERR_HOSTNAME: why = "name does not match certificate"; break;
        case CV_ERR_KEYUSE: why = "key not valid for this use"; break;
        case CV_ERR_ROOT: why = "unknown issuer (not in store)"; break;
        case CV_ERR_CHAIN: why = "chain signature invalid"; break;
        case CV_ERR_REVOKED: why = "certificate revoked (local blocklist)"; break;
        case CV_ERR_PRELOAD: why = "site key differs from pinned key"; break;
        }
        if (why) {
            n = html_put(h, n, sizeof h, "<p class=why>Reason: ");
            n = html_put(h, n, sizeof h, why);
            n = html_put(h, n, sizeof h, ".</p>");
        }
        n = html_put(h, n, sizeof h, "<p>The connection may be intercepted, the site's certificate expired, "
            "or the identity does not match.</p><p class=dim>Nothing was loaded and no HTTP fallback was "
            "attempted. Revocation: stapled OCSP is enforced when sent; absent staples are not fetched "
            "(soft-fail).</p>");
    } else if (T->conn_failed) {
        n = html_put(h, n, sizeof h, "<p>Could not load</p><span class=u>");
        n = html_esc(h, n, sizeof h, T->url);
        n = html_put(h, n, sizeof h, "</span>");
        if (T->conn_kind == OKAI_CONN_TOOLARGE)
            n = html_put(h, n, sizeof h, "<p>The page is larger than the fetch buffer and was not loaded.</p>");
        else
            n = html_put(h, n, sizeof h, "<p>The secure connection broke before the page arrived. This is "
                "<b>not</b> a certificate problem &mdash; try reloading.</p>");
        n = html_put(h, n, sizeof h, "<p class=dim>Nothing was loaded and no HTTP fallback was attempted.</p>");
    } else {
        n = html_put(h, n, sizeof h, "<p>okai could not fetch</p><span class=u>");
        n = html_esc(h, n, sizeof h, T->url);
        n = html_put(h, n, sizeof h, "</span><p>Check the address, or the site may be unreachable.</p>");
    }
    n = html_put(h, n, sizeof h, "<p class=dim><a href=\"");
    n = html_esc(h, n, sizeof h, T->url);
    n = html_put(h, n, sizeof h, "\">Try again</a> &nbsp;&middot;&nbsp; <a href=\"okai:home\">Home</a></p></div></body></html>");
    serial_printf("[okai] error page: %s for %s\n", title, T->url);
    tab_load_html(id, tab, h, n, "utf-8", 0);
    T->load_state = -1;
    T->title[0] = 0;
    scopy(T->title, title, sizeof T->title);
    if (okais[id].active_tab == tab) window_set_title(okais[id].win_id, T->title);
    request_render(T, 0);
    if (okais[id].active_tab == tab) okai_render_content(id);
}

// ---- rendering -------------------------------------------------------------------

static void request_render(struct okai_tab* T, int delay) {
    int due = (int)tick_count + delay;
    if (!T->render_pending || due - T->render_due < 0) T->render_due = due;
    T->render_pending = 1;
}

// Page viewport of window id: surface size and its screen origin.
static int page_geom(struct okai* b, int* sx, int* sy, int* pw, int* ph) {
    struct window* w = window_get(b->win_id);
    if (!w) return 0;
    int title_off = w->no_titlebar ? 0 : WIN_TITLE_H;
    int cw = w->w - 2 * WIN_BORDER;
    int ch = w->h - title_off - 2 * WIN_BORDER - CHROME_PX;
    if (cw < 16 || ch < 16) return 0;
    if (sx) *sx = w->x + WIN_BORDER;
    if (sy) *sy = w->y + WIN_BORDER + title_off + CHROME_PX;
    *pw = cw;
    *ph = ch;
    return 1;
}

static int ensure_surface(struct okai* b, int pw, int ph) {
    if (pw * ph > b->page_cap) {
        if (b->page_px) kfree(b->page_px);
        b->page_px = (uint32_t*)kmalloc((uint32_t)pw * (uint32_t)ph * 4);
        b->page_cap = b->page_px ? pw * ph : 0;
        if (!b->page_px) {
            serial_printf("[okai] page surface alloc failed (%dx%d)\n", pw, ph);
            window_set_pixels(b->win_id, 0, 0, 0, 0, 0, 0);
            return 0;
        }
    }
    if (pw != b->page_w || ph != b->page_h) {
        b->page_w = pw;
        b->page_h = ph;
    }
    window_set_pixels(b->win_id, b->page_px, 0, CHROME_PX, pw, ph, pw);
    return 1;
}

static void surf_of(struct okai* b, struct wsurf* s) {
    s->px = b->page_px;
    s->w = b->page_w;
    s->h = b->page_h;
    s->stride = b->page_w;
    ws_reset_clip(s);
}

void okai_render_content(int id) {
    if (id < 0 || id >= MAX_OKAIS) return;
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    struct okai_tab* T = okai_tab_of(b);
    int pw, ph;
    T->render_pending = 0;
    if (!page_geom(b, 0, 0, &pw, &ph) || !ensure_surface(b, pw, ph)) return;
    struct wsurf s;
    surf_of(b, &s);
    uint32_t t0 = tick_count;
    if (!T->doc) {
        // nothing loaded yet: blank white page under the chrome
        for (int i = 0; i < pw * ph; i++) b->page_px[i] = 0xFFFFFF;
        T->content_height = 0;
        window_set_content_bg_rgb(b->win_id, 0xFFFFFF);
        window_set_dirty(b->win_id);
        return;
    }
    struct job j;
    memset(&j, 0, sizeof j);
    if (T->pending_frag[0] && T->load_state == 1) {
        j.op = JOB_ANCHOR;
        j.doc = T->doc;
        j.frag = T->pending_frag;
        run_job(&j);
        if (j.result >= 0) {
            T->scroll_y = j.result;
            T->pending_frag[0] = 0;
        }
        memset(&j, 0, sizeof j);
    }
    j.op = JOB_PAINT;
    j.doc = T->doc;
    j.surf = &s;
    j.scroll = T->scroll_y;
    j.vw = pw;
    j.vh = ph;
    run_job(&j);
    T->content_height = j.result;
    T->scroll_y = j.scroll;
    T->has_fixed = j.h;
    T->last_render_tick = tick_count;
    window_set_content_bg_rgb(b->win_id, 0xFFFFFF);
    window_set_dirty(b->win_id);
    b->chrome_dirty = 1;
    int first = !T->first_paint;
    T->first_paint = 1;
    if (first) {
        serial_printf("[okai] render tab=%d %dx%d doc_h=%d scroll=%d in %dms (page origin %d,%d) stack=%dKB\n",
                      b->active_tab, pw, ph, T->content_height, T->scroll_y,
                      (int)(tick_count - t0) * 10,
                      window_get(b->win_id)->x + WIN_BORDER,
                      window_get(b->win_id)->y + WIN_BORDER + CHROME_PX, engine_stack_hwm_kb());
        memset(&j, 0, sizeof j);
        j.op = JOB_LINKS;
        j.doc = T->doc;
        j.scroll = T->scroll_y;
        j.vh = ph;
        run_job(&j);
        T->links_hash = (uint32_t)j.result;
    } else {
        serial_printf("[okai] render tab=%d doc_h=%d scroll=%d in %dms\n", b->active_tab,
                      T->content_height, T->scroll_y, (int)(tick_count - t0) * 10);
        if (wdoc_js(T->doc)) {
            // scripts move content: re-log the regions when they changed
            memset(&j, 0, sizeof j);
            j.op = JOB_LINKS;
            j.doc = T->doc;
            j.scroll = T->scroll_y;
            j.vh = ph;
            j.x = 1;
            j.result = (int)T->links_hash;
            run_job(&j);
            T->links_hash = (uint32_t)j.result;
        }
    }
}

// Repaint the window from the existing page surface (no engine work).
void okai_blit_content(int id) {
    if (id < 0 || id >= MAX_OKAIS || okais[id].win_id < 0) return;
    window_set_dirty(okais[id].win_id);
}

// Scroll the active tab to document y = ny. Small moves shift the surface
// and repaint only the exposed band.
static uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void scroll_to(int id, int ny) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (!T->doc || !b->page_px) return;
    int pw = b->page_w, ph = b->page_h;
    int maxs = T->content_height - ph;
    if (maxs < 0) maxs = 0;
    if (ny > maxs) ny = maxs;
    if (ny < 0) ny = 0;
    int dy = ny - T->scroll_y;
    if (!dy) return;
    int ady = dy < 0 ? -dy : dy;
    uint64_t c0 = rdtsc64();
    T->js_scroll_evt = 1;
    if (T->has_fixed || ady >= ph * 3 / 4 || T->render_pending) {
        T->scroll_y = ny;
        okai_render_content(id);
        return;
    }
    if (dy > 0) memmove(b->page_px, b->page_px + dy * pw, (uint32_t)(ph - dy) * pw * 4);
    else memmove(b->page_px + ady * pw, b->page_px, (uint32_t)(ph - ady) * pw * 4);
    T->scroll_y = ny;
    struct wsurf s;
    surf_of(b, &s);
    if (dy > 0) { s.cy0 = ph - dy; s.cy1 = ph; }
    else { s.cy0 = 0; s.cy1 = ady; }
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_PAINT;
    j.doc = T->doc;
    j.surf = &s;
    j.scroll = ny;
    run_job(&j);
    window_set_dirty(b->win_id);
    serial_printf("[okai] scroll dy=%d in %dkcyc\n", dy, (int)((rdtsc64() - c0) >> 10));
}

// ---- fetch driver ------------------------------------------------------------------

// Start the main-document fetch for tab `tab`. Returns 0 if a request is in
// flight, 1 if the page was produced locally (home/error), -1 if refused,
// 2 if the fetch engine is full (try again on a later poll).
static int start_main_fetch(int id, int tab) {
    struct okai* b = &okais[id];
    struct okai_tab* T = &b->tabs[tab];
    if (okai_is_home(T->url)) { load_home(id, tab); return 1; }
    if (!url_is_http(T->url)) {
        serial_printf("[okai] unsupported scheme in '%s'\n", T->url);
        show_error(id, tab);
        return 1;
    }
    T->is_https = url_is_https(T->url);
    struct oreq* q = req_new(id, tab, 0, -1, T->url);
    if (!q) return 2;
    int r = req_fetch(q, 0);
    if (r == -2) { q->used = 0; return 2; }
    if (r < 0) {
        // No hostname (e.g. a malformed address-bar entry). Never fire the
        // request — a DNS query for an empty host wedged the old fetch owner.
        q->used = 0;
        serial_printf("[okai] refusing fetch: empty host in '%s'\n", T->url);
        show_error(id, tab);
        return -1;
    }
    serial_printf("[okai] fetch %s\n", T->url);
    return 0;
}

static const char* kind_name(int kind) {
    return kind == WDOC_RK_CSS ? "CSS" : kind == WDOC_RK_IMG ? "IMG" : kind == WDOC_RK_SCRIPT ? "JS" : "XHR";
}

// Deliver a failure for resource id (scripts/requests get their error events).
static void sub_fail(struct okai_tab* T, int id, const char* url) {
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_FETCH_DONE;
    j.doc = T->doc;
    j.id = id;
    j.len = -1;
    j.final_url = url;
    run_job(&j);
}

static int req_slot_free(void) {
    for (int i = 0; i < FETCH_MAX; i++) if (!reqs[i].used) return 1;
    return 0;
}

// Start the next resource of tab (bi, ti). Returns 1 if a request started,
// 0 when the document's queue is drained, 2 when the engine is full. One
// fetch slot always stays free for main documents (navigation must never
// queue behind another page's images).
static int start_next_sub(int bi, int ti) {
    struct okai_tab* T = &okais[bi].tabs[ti];
    if (!T->doc) return 0;
    static char url[OKAI_URL_LEN];
    for (;;) {
        if (fetch_free_slots() < 2 || !req_slot_free()) return 2;
        int id = wdoc_next_fetch(T->doc, url, sizeof url);
        if (id < 0) return 0;
        int kind = wdoc_res_kind(T->doc, id);
        const char *method = "GET", *hdrs = 0, *body = 0;
        int blen = 0;
        wdoc_fetch_info(T->doc, id, &method, &hdrs, &body, &blen);
        int capped = (kind == WDOC_RK_CSS && T->sub_css >= OKAI_MAX_CSS_FETCH) ||
                     (kind == WDOC_RK_IMG && T->sub_img >= OKAI_MAX_IMG_FETCH) ||
                     (kind == WDOC_RK_SCRIPT && T->sub_js >= OKAI_MAX_JS_FETCH) ||
                     (kind == WDOC_RK_REQ && T->sub_req >= OKAI_MAX_REQ_FETCH);
        // the network layer speaks GET only (POST bodies are not sent yet)
        int unsupported = kind == WDOC_RK_REQ && strcmp(method, "GET") && strcmp(method, "HEAD");
        if (capped || unsupported) {
            if (unsupported) serial_printf("[okai] %s request not supported: %s\n", method, url);
            sub_fail(T, id, url);
            continue;
        }
        if (iprefix(url, "data:")) {
            int bl = 0;
            char mime[64];
            char* bytes = wdoc_data_url(url, (int)strlen(url), &bl, mime, sizeof mime);
            struct job j;
            memset(&j, 0, sizeof j);
            j.op = JOB_FETCH_DONE;
            j.doc = T->doc;
            j.id = id;
            j.bytes = bytes;
            j.len = bytes ? bl : -1;
            j.charset = mime;
            j.status = bytes ? 200 : 0;
            j.final_url = url;
            run_job(&j);
            if (bytes) kfree(bytes);
            T->js_next_tick = 0;
            continue;
        }
        if (!url_is_http(url)) { sub_fail(T, id, url); continue; }
        if (kind == WDOC_RK_CSS) T->sub_css++;
        else if (kind == WDOC_RK_IMG) T->sub_img++;
        else if (kind == WDOC_RK_SCRIPT) T->sub_js++;
        else T->sub_req++;
        struct oreq* q = req_new(bi, ti, T->doc, id, url);
        if (q) q->cors = kind == WDOC_RK_REQ;
        serial_printf("[okai] sub-res fetch: %s %s\n", kind_name(kind), url);
        if (!q || req_fetch(q, T->url) != 0) {
            if (q) q->used = 0;
            sub_fail(T, id, url);
            continue;
        }
        T->sub_inflight++;
        return 1;
    }
}

// Follow a 3xx for the main document: T->url becomes the target. Returns 1
// if the caller should fetch it.
static int follow_redirect(int id, struct okai_tab* T, const struct resp_info* ri) {
    if (ri->status < 300 || ri->status > 399 || !ri->location[0]) return 0;
    if (T->redirect_count >= OKAI_MAX_REDIRECTS) return 0;
    char abs[OKAI_URL_LEN];
    if (wurl_resolve(T->url, ri->location, (int)strlen(ri->location), abs, sizeof abs) <= 0) return 0;
    if (!url_is_http(abs)) return 0;
    T->redirect_count++;
    scopy(T->url, abs, OKAI_URL_LEN);
    // a redirect replaces the history entry (it is not a new page)
    if (T->history_count > 0) scopy(T->history[T->history_pos], T->url, OKAI_URL_LEN);
    T->is_https = url_is_https(T->url);
    okais[id].chrome_dirty = 1;
    serial_printf("[okai] redirect %d -> %s\n", T->redirect_count, abs);
    return 1;
}

static void main_loaded(int id, int tab, char* resp, int len) {
    struct okai_tab* T = &okais[id].tabs[tab];
    struct resp_info ri;
    parse_resp(resp, len, &ri);
    int blen, heap;
    char* body = resp_body(resp, len, &ri, &blen, &heap);
    char* page = body;
    int plen = blen;
    int page_heap = 0;
    // Non-HTML documents: plain text in a <pre>, images in an <img>.
    if (ri.ctype[0] && !iprefix(ri.ctype, "text/html") && !iprefix(ri.ctype, "application/xhtml")) {
        int is_img = iprefix(ri.ctype, "image/");
        int cap = is_img ? OKAI_URL_LEN * 2 + 512 : blen * 5 + 512;
        char* w = (char*)kmalloc((uint32_t)cap);
        if (w) {
            int n = html_put(w, 0, cap, "<!DOCTYPE html><html><head><meta charset=utf-8></head>");
            if (is_img) {
                n = html_put(w, n, cap, "<body style=\"margin:0;background:#0e0e0e;min-height:100vh;display:flex;"
                                        "align-items:center;justify-content:center\"><img src=\"");
                n = html_esc(w, n, cap, T->url);
                n = html_put(w, n, cap, "\" style=\"max-width:100%\"></body></html>");
            } else {
                n = html_put(w, n, cap, "<body><pre style=\"white-space:pre-wrap;margin:16px;font-size:14px\">");
                for (int i = 0; i < blen && n < cap - 16; i++) {
                    char c = body[i];
                    const char* e = c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : 0;
                    if (e) while (*e) w[n++] = *e++;
                    else w[n++] = c;
                }
                n = html_put(w, n, cap, "</pre></body></html>");
            }
            page = w;
            plen = n;
            page_heap = 1;
        }
    }
    tab_load_html(id, tab, page, plen, ri.charset[0] ? ri.charset : 0, okai_scripts_on);
    struct wdom* dom = T->doc ? wdoc_dom(T->doc) : 0;
    serial_printf("[br] %sparse: count=%d len=%d dom_nodes=%d status=%d%s ctype=%s%s\n",
                  T->is_https ? "https " : "", dom ? dom->nn : 0, blen, dom ? dom->nn : 0,
                  ri.status, heap ? " gzip" : "", ri.ctype[0] ? ri.ctype : "-",
                  (T->doc && wdoc_js(T->doc)) ? " js" : "");
    if (page_heap) kfree(page);
    if (heap) kfree(body);
    T->load_state = dom ? 1 : -1;
    T->load_tick = tick_count;
    T->sub_landed = 0;
    // #fragment: scroll there once laid out
    {
        const char* hsh = 0;
        for (const char* p = T->url; *p; p++) if (*p == '#') { hsh = p + 1; break; }
        if (hsh && *hsh) scopy(T->pending_frag, hsh, sizeof T->pending_frag);
    }
    if (T->doc && wdoc_pending_css(T->doc) > 0) request_render(T, CSS_WAIT_TICKS);
    else request_render(T, 0);
}

// A resource finished (st: 1 done, -1 failed, -2 truncated). Delivers it to
// the document and returns 0 — or returns 1 when the same resource should
// be fetched again from q->url (redirect / one-shot transport retry; the
// caller re-issues after releasing the old handle).
static int sub_done(struct okai_tab* T, struct oreq* q, int st, char* resp, int len) {
    int rid = q->sub_id;
    int kind = wdoc_res_kind(T->doc, rid);
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_FETCH_DONE;
    j.doc = T->doc;
    j.id = rid;
    j.len = -1;
    j.final_url = q->url;
    int status = 0, blen = 0, heap = 0;
    char* body = 0;
    static char hdrs[8192];
    hdrs[0] = 0;
    if (st == 1 && resp && len > 0) {
        struct resp_info ri;
        parse_resp(resp, len, &ri);
        status = ri.status;
        store_cookies(q->url, resp, len);
        // raw header lines (after the status line) for scripts (XHR
        // getResponseHeader, CORS)
        if (kind == WDOC_RK_REQ || kind == WDOC_RK_SCRIPT) {
            int he = header_end(resp, len);
            int s0 = 0;
            while (s0 < he && resp[s0] != '\n') s0++;
            s0++;
            int n = he > s0 ? he - s0 : 0;
            if (n > (int)sizeof hdrs - 1) n = (int)sizeof hdrs - 1;
            if (n > 0) memcpy(hdrs, resp + s0, n);
            hdrs[n > 0 ? n : 0] = 0;
            j.headers = hdrs;
        }
        if (status >= 300 && status <= 399 && ri.location[0] && q->redirects < 4) {
            char abs[OKAI_URL_LEN];
            if (wurl_resolve(q->url, ri.location, (int)strlen(ri.location), abs, sizeof abs) > 0 &&
                url_is_http(abs)) {
                q->redirects++;
                scopy(q->url, abs, OKAI_URL_LEN);
                serial_printf("[okai] sub-res redirect -> %s\n", abs);
                return 1;
            }
        } else if ((status >= 200 && status <= 299) || (kind == WDOC_RK_REQ && status >= 200)) {
            // requests see error statuses too (XHR status 404 + body)
            body = resp_body(resp, len, &ri, &blen, &heap);
            j.bytes = body;
            j.len = blen;
            j.charset = ri.ctype;
            j.status = status;
        }
    }
    if (status == 0 && !q->retried) {
        // transport blip (reset, stale connection): retry once
        q->retried = 1;
        serial_printf("[okai] sub-res retry %s\n", q->url);
        return 1;
    }
    run_job(&j);
    if (heap) kfree(body);
    serial_printf("[okai] sub-res %s: status=%d %d bytes%s\n", kind_name(kind), status,
                  j.len, j.len < 0 ? " (failed)" : "");
    return 0;
}

// Bookkeeping once a resource is delivered (or given up).
static void sub_landed(struct okai_tab* T, int kind) {
    T->sub_landed++;
    if (kind == WDOC_RK_CSS) {
        if (wdoc_pending_css(T->doc) == 0) request_render(T, 0);
    } else if (kind == WDOC_RK_IMG) {
        if (T->first_paint || wdoc_pending_css(T->doc) == 0) request_render(T, IMG_RENDER_TICKS);
    } else {
        T->js_next_tick = 0;   // a script/response arrived: pump the realm now
    }
}

// ---- page scripts ------------------------------------------------------------------

static void set_focus(int id, struct okai_tab* T, int node);
static void submit_form_node(int id, struct okai_tab* T, int form, int submitter, int fire_event);
static void navigate_ex(int id, const char* url, int push);

// Apply what a realm entry asked for (WJS_* flags).
static void js_apply(int id, int tab, int flags) {
    struct okai* b = &okais[id];
    struct okai_tab* T = &b->tabs[tab];
    if (!T->doc) return;
    struct wjs* js = wdoc_js(T->doc);
    if (!js) return;
    int active = tab == b->active_tab;
    if (flags & WJS_TITLE) {
        const char* t = wdoc_title(T->doc);
        scopy(T->title, t && t[0] ? t : T->url, sizeof T->title);
        if (active) window_set_title(b->win_id, T->title);
        b->chrome_dirty = 1;
    }
    if (flags & WJS_URL) {
        scopy(T->url, wdoc_url(T->doc), OKAI_URL_LEN);
        if (T->history_count > 0) scopy(T->history[T->history_pos], T->url, OKAI_URL_LEN);
        b->chrome_dirty = 1;
        serial_printf("[okai] script URL -> %s\n", T->url);
    }
    if (flags & WJS_FOCUS) {
        int node;
        if (wjs_take_focus(js, &node) && active) set_focus(id, T, node);
    }
    if (flags & WJS_SCROLL) {
        int y;
        if (wjs_take_scroll(js, &y)) {
            if (active && T->first_paint) scroll_to(id, y);
            else T->scroll_y = y;
        }
    }
    if (flags & WJS_DIRTY) {
        if (T->first_paint) request_render(T, 15);
        else if (wdoc_pending_css(T->doc) == 0) request_render(T, 0);
    }
    if (flags & WJS_NAV) {
        static char u[OKAI_URL_LEN];
        int rep = 0;
        if (wjs_take_nav(js, u, sizeof u, &rep) && active && T->load_state == 1) {
            if (!strncmp(u, "#submit:", 8)) {
                int f = 0, btn = 0, neg = 0, k = 8;
                while (u[k] >= '0' && u[k] <= '9') f = f * 10 + (u[k++] - '0');
                if (u[k] == ':') k++;
                if (u[k] == '-') { neg = 1; k++; }
                while (u[k] >= '0' && u[k] <= '9') btn = btn * 10 + (u[k++] - '0');
                submit_form_node(id, T, f, neg ? -1 : btn, 0);
            } else if (!strncmp(u, "#hist:", 6)) {
                if (u[6] == '-') okai_nav_back(id);
                else okai_nav_fwd(id);
            } else {
                serial_printf("[okai] script navigation -> %s\n", u);
                navigate_ex(id, u, !rep);
            }
        }
    }
}

// Dispatch a UI event to the page (node -1 = window). Returns 1 when a
// listener called preventDefault().
static int js_event(int id, struct okai_tab* T, int node, const char* type, int x, int y, int button, int key) {
    if (!T->doc || !wdoc_js(T->doc) || T->load_state != 1) return 0;
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_JS_EVENT;
    j.doc = T->doc;
    j.id = node;
    j.type = type;
    j.x = x;
    j.y = y;
    j.button = button;
    j.key = key;
    run_job(&j);
    js_apply(id, (int)(T - okais[id].tabs), j.result);
    T->js_next_tick = 0;
    return j.h;
}

// Run due scripts/timers/frames for every loaded tab (background tabs at
// most once a second).
static void js_pump(void) {
    for (int bi = 0; bi < MAX_OKAIS; bi++) {
        struct okai* b = okai_get(bi);
        if (!b) continue;
        for (int ti = 0; ti < b->tab_count; ti++) {
            struct okai_tab* T = &b->tabs[ti];
            if (!T->doc || T->load_state != 1 || T->closing) continue;
            struct wjs* js = wdoc_js(T->doc);
            if (!js) continue;
            int active = ti == b->active_tab;
            if (T->js_next_tick && (int)(tick_count - T->js_next_tick) < 0) continue;
            if (T->js_scroll_evt && active) {
                T->js_scroll_evt = 0;
                js_event(bi, T, 0, "scroll", 0, 0, 0, 0);
            }
            int due = wjs_next_due(js);
            if (due < 0) { T->js_next_tick = 0; continue; }
            if (due > 0) {
                T->js_next_tick = tick_count + (uint32_t)((due + 9) / 10);
                if (!active && due < 1000) T->js_next_tick = tick_count + 100;
                continue;
            }
            struct job j;
            memset(&j, 0, sizeof j);
            j.op = JOB_JS_RUN;
            j.doc = T->doc;
            j.len = active ? 60 : 20;   // ms budget per pump
            uint32_t t0 = tick_count;
            run_job(&j);
            if (tick_count - t0 >= 10)
                serial_printf("[okai] js tab=%d ran %dms (stack %dKB)\n", ti, (int)(tick_count - t0) * 10,
                              engine_stack_hwm_kb());
            T->js_next_tick = active ? 0 : tick_count + 100;
            js_apply(bi, ti, j.result);
        }
    }
}

// Re-issue a request record for cur.url (redirect / retry / fallback).
// Returns 1 if it is in flight again.
static int req_reissue(const struct oreq* cur, const char* top) {
    struct oreq* q = req_new(cur->bi, cur->tab, cur->doc, cur->sub_id, cur->url);
    if (!q) return 0;
    q->redirects = cur->redirects;
    q->retried = cur->retried;
    q->cors = cur->cors;
    if (req_fetch(q, top) != 0) { q->used = 0; return 0; }
    return 1;
}

// A resource request finished. `cur` is a detached copy of its record (the
// table slot is already free: delivering may run scripts that navigate,
// and navigation cancels the tab's requests).
static void sub_complete(struct okai_tab* T, struct oreq* cur, int st, char* resp, int len) {
    int kind = wdoc_res_kind(T->doc, cur->sub_id);
    if (st == -2) serial_puts("[okai] TLS sub-resource truncated, skipping\n");
    int again = sub_done(T, cur, st, resp, len);
    fetch_end(cur->h);
    cur->h = -1;
    if (again) {
        if (req_reissue(cur, T->url)) return;   // still in flight (sub_inflight unchanged)
        sub_fail(T, cur->sub_id, cur->url);
    }
    T->sub_inflight--;
    sub_landed(T, kind);
    // a script/response landed: let the page run first so the scripts it
    // inserts are queued ahead of the remaining images
    if (kind == WDOC_RK_SCRIPT || kind == WDOC_RK_REQ) js_pump();
    if (T->doc && T->sub_inflight <= 0 && wdoc_pending(T->doc) == 0) {
        T->sub_inflight = 0;
        request_render(T, 0);
        serial_printf("[okai] sub-res done: %d css, %d img\n", T->sub_css, T->sub_img);
    }
}

// The main document request finished (cur: detached record).
static void main_complete(int bi, int tab, struct oreq* cur, int st, char* resp, int len) {
    struct okai* b = &okais[bi];
    struct okai_tab* T = &b->tabs[tab];
    if (st == -2) {
        fetch_end(cur->h);
        serial_printf("[okai] TLS response truncated for %s, no HTTP fallback\n", T->url);
        T->truncated = 1;
        show_error(bi, tab);
        return;
    }
    if (st == 1) {
        struct resp_info ri;
        parse_resp(resp, len, &ri);
        store_cookies(T->url, resp, len);
        if (follow_redirect(bi, T, &ri)) {
            fetch_end(cur->h);
            scopy(cur->url, T->url, OKAI_URL_LEN);
            if (!req_reissue(cur, 0)) show_error(bi, tab);
            return;
        }
        main_loaded(bi, tab, resp, len);   // resources start from okai_poll
        fetch_end(cur->h);
        return;
    }
    // st == -1: the fetch gave up.
    int fr = fetch_fail_reason(cur->h);
    int fdetail = fetch_fail_detail(cur->h);
    int unaccepted = fetch_offer_unaccepted(cur->h);
    fetch_end(cur->h);
    if (T->is_https) {
        // Classify: certificate failures AND secure-channel failures MUST NOT
        // fall back to plain HTTP — a MITM can force that downgrade by killing
        // the handshake. Only transport failures (timeout / unreachable / no A
        // record) downgrade. Cert problems render the SECURITY WARNING;
        // everything else renders a connection error — never the cert warning.
        int cert_fail = (fr == TLS_FAIL_CERT || fr == TLS_FAIL_HOSTNAME);
        int conn_fail = (fr == TLS_FAIL_PROTO || fr == TLS_FAIL_MAC || fr == TLS_FAIL_ALERT ||
                         fr == TLS_FAIL_RNG || fr == TLS_FAIL_OVERFLOW);
        if (cert_fail) {
            serial_printf("[okai] TLS cert failure (reason=%d), no HTTP fallback for %s\n", fr, T->url);
            T->cert_failed = 1;
            T->cert_detail = fdetail;
        } else if (conn_fail) {
            // Resumption fallback first: if the server aborted our PSK
            // resumption, retry with a full handshake (same origin, not a
            // downgrade). Only resumption-abort signatures retry.
            if (T->conn_retries < OKAI_CONN_MAX_RETRIES &&
                (fr == TLS_FAIL_PROTO || fr == TLS_FAIL_ALERT || fr == TLS_FAIL_MAC)) {
                T->conn_retries++;
                if (unaccepted) {
                    char host[128];
                    if (wurl_host(T->url, host, sizeof host) > 0) tls_ticket_drop(host);
                    serial_printf("[okai] resumption aborted, retrying full handshake: %s\n", T->url);
                } else {
                    serial_printf("[okai] fetch failed, retrying (%d/%d): %s\n",
                                  T->conn_retries, OKAI_CONN_MAX_RETRIES, T->url);
                }
                serial_printf("[okai] resumption retry in flight for %s\n", T->url);
                scopy(cur->url, T->url, OKAI_URL_LEN);
                if (req_reissue(cur, 0)) return;
            } else {
                serial_printf("[okai] TLS connection failure (reason=%d), no HTTP fallback for %s\n", fr, T->url);
                T->conn_failed = 1;
                T->conn_kind = (fr == TLS_FAIL_OVERFLOW) ? OKAI_CONN_TOOLARGE : OKAI_CONN_PROTO;
            }
        } else if (!T->https_fell_back) {
            // transport failure: one-time plain-HTTP retry (http-only hosts)
            T->https_fell_back = 1;
            okai_rewrite_scheme_http(T->url);
            if (T->history_count > 0) scopy(T->history[T->history_pos], T->url, OKAI_URL_LEN);
            T->is_https = 0;
            b->chrome_dirty = 1;
            serial_printf("[okai] https failed, retrying http: %s\n", T->url);
            serial_printf("[okai] http fallback in flight for %s\n", T->url);
            scopy(cur->url, T->url, OKAI_URL_LEN);
            if (req_reissue(cur, 0)) return;
        }
    }
    show_error(bi, tab);
}

// Check one request for completion.
static void req_poll(struct oreq* q) {
    int fst = fetch_status(q->h);
    if (fst == FETCH_RUNNING) return;
    struct oreq cur = *q;          // detach: the slot is free from here on
    q->used = 0;
    q->h = -1;
    struct okai* b = okai_get(cur.bi);
    if (!b || cur.tab < 0 || cur.tab >= b->tab_count) { fetch_end(cur.h); return; }
    struct okai_tab* T = &b->tabs[cur.tab];
    char* resp = 0;
    int len = 0, st = -1;          // 1 done, -1 failed, -2 truncated
    if (fst == FETCH_DONE) {
        resp = fetch_response(cur.h, &len);
        st = (resp && len > 0) ? 1 : -1;
        // Truncation integrity (cryptoholes #1): without an authenticated
        // close_notify, HTTP framing must prove the message complete.
        if (st == 1 && cur.https && !fetch_saw_close(cur.h) &&
            tls_response_complete((const uint8_t*)resp, (uint32_t)len) == TLS_RESP_SHORT)
            st = -2;
    }
    if (cur.sub_id >= 0) {
        if (T->doc != cur.doc || T->closing) { fetch_end(cur.h); return; }   // document replaced
        sub_complete(T, &cur, st, resp, len);
    } else {
        main_complete(cur.bi, cur.tab, &cur, st, resp, len);
    }
}

void okai_poll(void) {
    for (int i = 0; i < FETCH_MAX; i++)
        if (reqs[i].used && reqs[i].h >= 0) req_poll(&reqs[i]);
    // Main documents first (active tabs first).
    for (int pass = 0; pass < 2; pass++)
        for (int bi = 0; bi < MAX_OKAIS; bi++) {
            struct okai* b = okai_get(bi);
            if (!b) continue;
            for (int ti = 0; ti < b->tab_count; ti++) {
                if ((pass == 0) != (ti == b->active_tab)) continue;
                struct okai_tab* T = &b->tabs[ti];
                if (T->load_state != 0 || T->closing || req_has_main(bi, ti)) continue;
                start_main_fetch(bi, ti);
            }
        }
    // Resources (stylesheets, scripts, images, script requests): up to
    // OKAI_SUB_PAR per tab in parallel, active tabs first. Scripts insert
    // more resources after the initial queue drained — this picks them up.
    for (int pass = 0; pass < 2; pass++)
        for (int bi = 0; bi < MAX_OKAIS; bi++) {
            struct okai* b = okai_get(bi);
            if (!b) continue;
            for (int ti = 0; ti < b->tab_count; ti++) {
                if ((pass == 0) != (ti == b->active_tab)) continue;
                struct okai_tab* T = &b->tabs[ti];
                if (T->load_state != 1 || !T->doc || T->closing) continue;
                while (T->sub_inflight < OKAI_SUB_PAR && start_next_sub(bi, ti) == 1) {}
            }
        }
    js_pump();
    // Coalesced renders (active tabs only; others render on switch).
    for (int bi = 0; bi < MAX_OKAIS; bi++) {
        struct okai* b = okai_get(bi);
        if (!b) continue;
        struct okai_tab* T = okai_tab_of(b);
        if (T->render_pending && (int)tick_count - T->render_due >= 0) okai_render_content(bi);
    }
    // Fetch-pill erase: the progress pill is an overlay; when a window's
    // requests end repaint it once so its pixels go away.
    okai_fetch_owner = -1;
    for (int i = 0; i < FETCH_MAX; i++)
        if (reqs[i].used) { okai_fetch_owner = reqs[i].bi; break; }
    {
        static int pill_win = -1;
        if (okai_fetch_owner >= 0) pill_win = okai_fetch_owner;
        else if (pill_win >= 0) {
            if (okai_get(pill_win)) okai_blit_content(pill_win);
            pill_win = -1;
        }
    }
}

// ---- open / close / navigate ---------------------------------------------------------

int okai_open(const char* url) {
    if (okai_count >= MAX_OKAIS) return -1;
    int id = okai_count;
    struct okai* b = &okais[id];
    b->win_id = -1;
    b->active_tab = 0;
    b->tab_count = 1;
    b->addr_bar_focused = 0;
    b->addr_input_len = 0;
    b->addr_input[0] = 0;
    b->show_security = 0;
    b->chrome_dirty = 1;
    okai_tab_reset(&b->tabs[0]);
    b->tabs[0].anim_w = 0;
    b->tabs[0].closing = 0;

    // One browser window, maximized to the work area on open (NOT fullscreen:
    // keeps its border/chrome and stays draggable/resizable like any window).
    int win = window_create("okai", 0, 0, SCREEN_W, SCREEN_H - TASKBAR_H);
    if (win < 0) return -1;
    window_set_close_button(win, 1);
    window_set_minimize_button(win, 1);
    window_set_no_titlebar(win, 1); // browser draws its own chrome at the top
    window_set_hide_cursor(win, 1); // no blinking text cursor in the browser
    b->win_id = win;
    okai_count++;
    window_set_focus(win); // take focus so keyboard input (g/j/k) works immediately
    okai_last_tick = tick_count;
    okai_navigate(id, url);
    if (!okai_tab_of(b)->doc) okai_render_content(id); // blank until the page arrives
    return id;
}

void okai_close(int id) {
    if (id < 0 || id >= okai_count) return;
    struct okai* b = &okais[id];
    req_window_removed(id);
    for (int t = 0; t < OKAI_MAX_TABS; t++)
        if (b->tabs[t].doc) { wdoc_free(b->tabs[t].doc); b->tabs[t].doc = 0; }
    if (b->win_id >= 0) {
        window_destroy(b->win_id);
        b->win_id = -1;
    }
    if (b->page_px) { kfree(b->page_px); b->page_px = 0; }
    b->page_cap = b->page_w = b->page_h = 0;
    for (int i = 0; i < MAX_OKAIS; i++) okai_ui_forget(i); // slots shift below
    // Compact so the one-window policy (`okai` reuses slot 0) never targets
    // a dead slot (okai_get would return NULL mid-click-handler).
    for (int i = id; i < okai_count - 1; i++) okais[i] = okais[i + 1];
    okai_count--;
    okais[okai_count].win_id = -1;
    okais[okai_count].page_px = 0;
    okais[okai_count].page_cap = 0;
    for (int t = 0; t < OKAI_MAX_TABS; t++) okais[okai_count].tabs[t].doc = 0;
    okais[okai_count].tab_count = 0;
}

// Load `url` into the active tab. push = add a history entry.
static void navigate_ex(int id, const char* url, int push) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    // Cancel this tab's requests (old document's resources included) so
    // the new URL loads and nothing lands in the page being replaced.
    req_cancel_tab(id, b->active_tab);
    char norm[OKAI_URL_LEN];
    okai_normalize_https(url, norm, OKAI_URL_LEN);
    scopy(T->url, norm, OKAI_URL_LEN);
    if (push) {
        if (T->history_count > 0 && T->history_pos < T->history_count - 1)
            T->history_count = T->history_pos + 1;    // drop forward history
        if (T->history_count == OKAI_MAX_HISTORY) {
            for (int i = 1; i < OKAI_MAX_HISTORY; i++) scopy(T->history[i - 1], T->history[i], OKAI_URL_LEN);
            T->history_count--;
        }
        scopy(T->history[T->history_count], T->url, OKAI_URL_LEN);
        T->history_pos = T->history_count++;
    }
    tab_reset_load(T);
    T->is_https = url_is_https(T->url);
    if (T->focused_node >= 0 && T->doc && wdoc_dom(T->doc)) {
        struct wdom* d = wdoc_dom(T->doc);
        if (T->focused_node < d->nn) d->n[T->focused_node].flags &= ~(WNF_FOCUSED);
    }
    T->focused_node = -1;
    b->chrome_dirty = 1;
    serial_printf("[okai] navigate %s\n", T->url);
    if (okai_is_home(T->url)) load_home(id, b->active_tab);
    // otherwise okai_poll starts the fetch; the old page stays visible
    // until the new one arrives.
}

void okai_navigate(int id, const char* url) {
    if (id < 0 || id >= MAX_OKAIS) return;
    navigate_ex(id, url, 1);
}

static void okai_goto_history(int id, int pos) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (pos < 0 || pos >= T->history_count) return;
    T->history_pos = pos;
    char u[OKAI_URL_LEN];
    scopy(u, T->history[pos], sizeof u);
    navigate_ex(id, u, 0);
}

void okai_nav_back(int id) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    struct okai_tab* T = okai_tab_of(b);
    if (T->history_pos > 0) okai_goto_history(id, T->history_pos - 1);
}

void okai_nav_fwd(int id) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    struct okai_tab* T = okai_tab_of(b);
    if (T->history_pos + 1 < T->history_count) okai_goto_history(id, T->history_pos + 1);
}

void okai_nav_reload(int id) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    char u[OKAI_URL_LEN];
    scopy(u, okai_tab_of(b)->url, sizeof u);
    navigate_ex(id, u, 0);
}

void okai_nav_home(int id) {
    if (okais[id].win_id < 0) return;
    okai_navigate(id, OKAI_HOME_URL); // a real Home: pushes history
}

// ---- forms + input -----------------------------------------------------------------

static int is_tag(struct wdom* d, int n, int tag) {
    return n >= 0 && n < d->nn && d->n[n].type == WN_ELEM && d->n[n].tag == tag && d->n[n].ns == NS_HTML;
}

static int attr_ieq(struct wdom* d, int n, int atom, const char* lit) {
    int l;
    const char* v = wdom_attr(d, n, atom, &l);
    if (!v) return 0;
    int i = 0;
    for (; i < l && lit[i]; i++) if (lower((unsigned char)v[i]) != lit[i]) return 0;
    return i == l && !lit[i];
}

// input types that take typed text
static int is_text_input(struct wdom* d, int n) {
    if (is_tag(d, n, T_textarea)) return 1;
    if (!is_tag(d, n, T_input)) return 0;
    int l;
    const char* t = wdom_attr(d, n, A_type, &l);
    if (!t || !l) return 1;
    static const char* const txt[] = { "text", "search", "email", "url", "password", "tel", "number", 0 };
    for (int i = 0; txt[i]; i++) if (attr_ieq(d, n, A_type, txt[i])) return 1;
    return 0;
}

static int focusable(struct wdom* d, int n) {
    if (d->n[n].type != WN_ELEM || d->n[n].ns != NS_HTML) return 0;
    if (wdom_has_attr(d, n, A_disabled)) return 0;
    int t = d->n[n].tag;
    if (t == T_input) return !attr_ieq(d, n, A_type, "hidden");
    return t == T_select || t == T_textarea || t == T_button;
}

static void set_focus(int id, struct okai_tab* T, int node) {
    static int in_focus;   // focus handlers may move focus again: no recursion
    struct wdom* d = T->doc ? wdoc_dom(T->doc) : 0;
    if (!d) return;
    if (T->focused_node == node) return;
    if (node >= d->nn) node = -1;
    int old = T->focused_node;
    if (old >= 0 && old < d->nn) {
        d->n[old].flags &= ~WNF_FOCUSED;
        for (int p = old; p >= 0; p = d->n[p].parent) d->n[p].flags &= ~WNF_FOCUS_WITHIN;
    }
    T->focused_node = node;
    if (node >= 0) {
        d->n[node].flags |= WNF_FOCUSED;
        for (int p = node; p >= 0; p = d->n[p].parent) d->n[p].flags |= WNF_FOCUS_WITHIN;
    }
    wdoc_set_focus(T->doc, node);
    wdoc_invalidate(T->doc);
    request_render(T, 0);
    if (!in_focus && wdoc_js(T->doc)) {
        in_focus = 1;
        if (old >= 0 && old < d->nn) { js_event(id, T, old, "blur", 0, 0, 0, 0); js_event(id, T, old, "focusout", 0, 0, 0, 0); }
        if (node >= 0) { js_event(id, T, node, "focus", 0, 0, 0, 0); js_event(id, T, node, "focusin", 0, 0, 0, 0); }
        in_focus = 0;
    }
}

// URL-encode s[0..n) (application/x-www-form-urlencoded) onto out.
static int form_enc(char* out, int at, int cap, const char* s, int n) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < n && at < cap - 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '*') out[at++] = (char)c;
        else if (c == ' ') out[at++] = '+';
        else { out[at++] = '%'; out[at++] = hex[c >> 4]; out[at++] = hex[c & 15]; }
    }
    return at;
}

static int form_pair(char* q, int ql, int cap, const char* nm, int nl, const char* v, int vl) {
    if (ql > 0 && ql < cap - 1) q[ql++] = '&';
    ql = form_enc(q, ql, cap, nm, nl);
    if (ql < cap - 1) q[ql++] = '=';
    return form_enc(q, ql, cap, v, vl);
}

// Value of a control (value attr, or text for textarea / option), into buf.
static int control_value(struct wdom* d, int n, char* buf, int cap) {
    int l = wdom_attr_copy(d, n, A_value, buf, cap);
    if (l >= 0) return l;
    if (is_tag(d, n, T_textarea) || is_tag(d, n, T_option)) return wdom_text_content(d, n, buf, cap);
    return 0;
}

// Submit the form owning `submitter` (GET; POST forms are sent as GET).
static void submit_form(int id, struct okai_tab* T, int submitter) {
    struct wdom* d = T->doc ? wdoc_dom(T->doc) : 0;
    if (!d) return;
    int form = -1;
    for (int p = submitter; p >= 0; p = d->n[p].parent) if (is_tag(d, p, T_form)) { form = p; break; }
    if (form < 0) { serial_puts("[okai] submit: control is not in a form\n"); return; }
    submit_form_node(id, T, form, submitter, 1);
}

// Build the GET query of `form` (submitter: the clicked button or -1) and
// navigate. fire_event: dispatch `submit` first (a script may cancel it).
static void submit_form_node(int id, struct okai_tab* T, int form, int submitter, int fire_event) {
    struct wdom* d = T->doc ? wdoc_dom(T->doc) : 0;
    if (!d || form < 0 || form >= d->nn || !is_tag(d, form, T_form)) return;
    if (fire_event && js_event(id, T, form, "submit", 0, 0, 0, 0)) {
        serial_puts("[okai] submit prevented by script\n");
        return;
    }
    if (T->load_state != 1 || !T->doc || wdoc_dom(T->doc) != d) return;   // a handler navigated
    static char q[2048];
    static char val[1024];
    int ql = 0;
    q[0] = 0;
    for (int n = d->n[form].first; n >= 0; n = wdom_next(d, n, form)) {
        if (d->n[n].type != WN_ELEM || wdom_has_attr(d, n, A_disabled)) continue;
        int nl;
        const char* nm = wdom_attr(d, n, A_name, &nl);
        if (!nm || !nl) continue;
        if (is_tag(d, n, T_input)) {
            if (attr_ieq(d, n, A_type, "checkbox") || attr_ieq(d, n, A_type, "radio")) {
                if (!wdom_has_attr(d, n, A_checked)) continue;
                int vl = control_value(d, n, val, sizeof val);
                if (!wdom_has_attr(d, n, A_value)) { memcpy(val, "on", 2); vl = 2; }
                ql = form_pair(q, ql, sizeof q, nm, nl, val, vl);
            } else if (attr_ieq(d, n, A_type, "submit") || attr_ieq(d, n, A_type, "image") ||
                       attr_ieq(d, n, A_type, "button") || attr_ieq(d, n, A_type, "reset") ||
                       attr_ieq(d, n, A_type, "file")) {
                if (n != submitter || attr_ieq(d, n, A_type, "button") || attr_ieq(d, n, A_type, "reset")) continue;
                int vl = control_value(d, n, val, sizeof val);
                ql = form_pair(q, ql, sizeof q, nm, nl, val, vl);
            } else {
                int vl = control_value(d, n, val, sizeof val);
                ql = form_pair(q, ql, sizeof q, nm, nl, val, vl);
            }
        } else if (is_tag(d, n, T_textarea)) {
            int vl = control_value(d, n, val, sizeof val);
            ql = form_pair(q, ql, sizeof q, nm, nl, val, vl);
        } else if (is_tag(d, n, T_select)) {
            int first = -1, sel = -1;
            for (int c = d->n[n].first; c >= 0; c = wdom_next(d, c, n)) {
                if (!is_tag(d, c, T_option)) continue;
                if (first < 0) first = c;
                if (wdom_has_attr(d, c, A_selected)) { sel = c; break; }
            }
            if (sel < 0) sel = first;
            if (sel < 0) continue;
            int vl = control_value(d, sel, val, sizeof val);
            ql = form_pair(q, ql, sizeof q, nm, nl, val, vl);
        } else if (is_tag(d, n, T_button) && n == submitter && !attr_ieq(d, n, A_type, "button")) {
            int vl = control_value(d, n, val, sizeof val);
            ql = form_pair(q, ql, sizeof q, nm, nl, val, vl);
        }
    }
    q[ql] = 0;
    if (attr_ieq(d, form, A_method, "post")) serial_puts("[okai] POST form submitted as GET\n");
    static char action[OKAI_URL_LEN];
    char url[OKAI_URL_LEN];
    int al;
    const char* a = wdom_attr(d, form, A_action, &al);
    if (!a || !al || wurl_resolve(T->url, a, al, action, sizeof action) <= 0) scopy(action, T->url, sizeof action);
    // the query replaces the action's query + fragment
    int ul = 0;
    while (action[ul] && action[ul] != '?' && action[ul] != '#' && ul < OKAI_URL_LEN - 2) { url[ul] = action[ul]; ul++; }
    url[ul++] = '?';
    for (int i = 0; q[i] && ul < OKAI_URL_LEN - 1; i++) url[ul++] = q[i];
    url[ul] = 0;
    serial_printf("[okai] submit form -> %s\n", url);
    okai_navigate(id, url);
}

// Click / Enter on a non-text control.
static void activate_control(int id, struct okai_tab* T, int node) {
    struct wdom* d = wdoc_dom(T->doc);
    if (is_tag(d, node, T_input) && (attr_ieq(d, node, A_type, "checkbox") || attr_ieq(d, node, A_type, "radio"))) {
        int radio = attr_ieq(d, node, A_type, "radio");
        if (radio) {
            int nl;
            const char* nm = wdom_attr(d, node, A_name, &nl);
            if (nm) {
                for (int n = 0; n < d->nn; n++) {
                    if (n == node || !is_tag(d, n, T_input) || !attr_ieq(d, n, A_type, "radio")) continue;
                    int ol;
                    const char* on = wdom_attr(d, n, A_name, &ol);
                    if (on && ol == nl && !memcmp(on, nm, nl)) wdom_remove_attr(d, n, A_checked);
                }
            }
            wdom_set_attr(d, node, A_checked, "", 0);
        } else if (wdom_has_attr(d, node, A_checked)) wdom_remove_attr(d, node, A_checked);
        else wdom_set_attr(d, node, A_checked, "", 0);
        serial_printf("[okai] toggle node=%d checked=%d\n", node, wdom_has_attr(d, node, A_checked));
        set_focus(id, T, node);
        wdoc_invalidate(T->doc);
        request_render(T, 0);
        js_event(id, T, node, "input", 0, 0, 0, 0);
        js_event(id, T, node, "change", 0, 0, 0, 0);
        return;
    }
    if (is_tag(d, node, T_select)) {
        // cycle to the next option (no popup menu yet)
        int first = -1, sel = -1, next = -1;
        for (int c = d->n[node].first; c >= 0; c = wdom_next(d, c, node)) {
            if (!is_tag(d, c, T_option)) continue;
            if (first < 0) first = c;
            if (sel >= 0 && next < 0) next = c;
            if (sel < 0 && wdom_has_attr(d, c, A_selected)) sel = c;
        }
        if (sel < 0) { sel = first; next = -1; for (int c = first; c >= 0; c = wdom_next(d, c, node)) if (c != first && is_tag(d, c, T_option)) { next = c; break; } }
        if (next < 0) next = first;
        if (sel >= 0) wdom_remove_attr(d, sel, A_selected);
        if (next >= 0) wdom_set_attr(d, next, A_selected, "", 0);
        set_focus(id, T, node);
        wdoc_invalidate(T->doc);
        request_render(T, 0);
        js_event(id, T, node, "input", 0, 0, 0, 0);
        js_event(id, T, node, "change", 0, 0, 0, 0);
        return;
    }
    int submit = is_tag(d, node, T_button) ? !attr_ieq(d, node, A_type, "button") && !attr_ieq(d, node, A_type, "reset")
                                           : (attr_ieq(d, node, A_type, "submit") || attr_ieq(d, node, A_type, "image"));
    if (submit) submit_form(id, T, node);
    else set_focus(id, T, node);
}

static void scroll_node_into_view(int id, struct okai_tab* T, int node) {
    struct okai* b = &okais[id];
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_RECT;
    j.doc = T->doc;
    j.id = node;
    run_job(&j);
    if (!j.result) return;
    if (j.y < T->scroll_y + 8 || j.y + j.h > T->scroll_y + b->page_h - 8) {
        T->scroll_y = j.y - b->page_h / 3;
        if (T->scroll_y < 0) T->scroll_y = 0;
        request_render(T, 0);
    }
}

static void focus_next(int id, struct okai_tab* T) {
    struct wdom* d = T->doc ? wdoc_dom(T->doc) : 0;
    if (!d) return;
    int start = T->focused_node >= 0 ? T->focused_node : 0;
    int n = start;
    for (int k = 0; k < d->nn; k++) {
        n = wdom_next(d, n, 0);
        if (n < 0) n = d->n[0].first;
        if (n < 0) return;
        if (n != T->focused_node && focusable(d, n)) {
            set_focus(id, T, n);
            serial_printf("[okai] tab focus node=%d\n", n);
            scroll_node_into_view(id, T, n);
            return;
        }
    }
}

int okai_content_click(int id, int mx, int my) {
    if (id < 0 || id >= MAX_OKAIS) return 0;
    struct okai* b = &okais[id];
    if (b->win_id < 0) return 0;
    struct okai_tab* T = okai_tab_of(b);
    int sx, sy, pw, ph;
    if (!page_geom(b, &sx, &sy, &pw, &ph)) return 0;
    int x = mx - sx, y = my - sy;
    if (x < 0 || y < 0 || x >= pw || y >= ph || !T->doc) return 0;
    static struct wdoc_hit hit;
    struct job j;
    memset(&j, 0, sizeof j);
    j.op = JOB_HIT;
    j.doc = T->doc;
    j.x = x;
    j.y = y;
    j.scroll = T->scroll_y;
    j.hit = &hit;
    run_job(&j);
    serial_printf("[okai] click x=%d y=%d (mx=%d my=%d) hit=%d node=%d\n", x, y, mx, my, hit.kind, hit.node);
    if (wdoc_js(T->doc) && T->load_state == 1) {
        // scripts see the click first (the deepest element under the
        // pointer); preventDefault() cancels the default action below
        int target = hit.node;
        if (target < 0) {
            memset(&j, 0, sizeof j);
            j.op = JOB_ELEM_AT;
            j.doc = T->doc;
            j.x = x;
            j.y = y;
            run_job(&j);
            target = j.result;
        }
        if (target >= 0) {
            struct wdoc* before = T->doc;
            js_event(id, T, target, "pointerdown", x, y, 0, 0);
            js_event(id, T, target, "mousedown", x, y, 0, 0);
            js_event(id, T, target, "pointerup", x, y, 0, 0);
            js_event(id, T, target, "mouseup", x, y, 0, 0);
            int prevented = js_event(id, T, target, "click", x, y, 0, 0);
            if (T->doc != before || T->load_state != 1) return 1;   // a handler navigated
            if (prevented) {
                serial_printf("[okai] click default prevented by script (node=%d)\n", target);
                return 1;
            }
        }
    }
    if (hit.kind == WDOC_HIT_LINK) {
        if (!hit.href[0] || iprefix(hit.href, "javascript:")) return 1;
        serial_printf("[okai] LINK HIT -> %s\n", hit.href);
        // same-document fragment link: just scroll
        const char* hs = 0;
        for (const char* p = hit.href; *p; p++) if (*p == '#') { hs = p; break; }
        if (hs) {
            int n = (int)(hs - hit.href), m = 0;
            while (T->url[m] && T->url[m] != '#') m++;
            if (n == m && !strncmp(hit.href, T->url, n)) {
                memset(&j, 0, sizeof j);
                j.op = JOB_ANCHOR;
                j.doc = T->doc;
                j.frag = hs + 1;
                run_job(&j);
                if (j.result >= 0) scroll_to(id, j.result);
                return 1;
            }
        }
        okai_navigate(id, hit.href);
        return 1;
    }
    if (hit.kind == WDOC_HIT_FIELD) {
        struct wdom* d = wdoc_dom(T->doc);
        if (is_tag(d, hit.node, T_select)) activate_control(id, T, hit.node);
        else set_focus(id, T, hit.node);
        serial_printf("[okai] focus input node=%d\n", hit.node);
        return 1;
    }
    if (hit.kind == WDOC_HIT_BUTTON) {
        activate_control(id, T, hit.node);
        return 1;
    }
    if (T->focused_node >= 0) set_focus(id, T, -1);
    return 0;
}

static void edit_field(struct okai_tab* T, char c) {
    struct wdom* d = wdoc_dom(T->doc);
    int n = T->focused_node;
    static char cur[1024];
    int l = control_value(d, n, cur, sizeof cur);
    if (l < 0) l = 0;
    if (c == '\b') {
        if (l > 0) {
            l--;
            while (l > 0 && ((unsigned char)cur[l] & 0xC0) == 0x80) l--;
        }
    } else if (l < (int)sizeof cur - 1) cur[l++] = c;
    wdom_set_attr(d, n, A_value, cur, l);
    wdoc_invalidate(T->doc);
    request_render(T, 2);
}

void okai_handle_key(int id, char c) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    struct okai_tab* T = okai_tab_of(b);
    b->show_security = 0; // any keypress dismisses the security popup
    b->chrome_dirty = 1;  // addr text / focus / popup may have changed

    if (b->addr_bar_focused) {
        if (c == '\n') {
            b->addr_bar_focused = 0;
            if (b->addr_input_len > 0) okai_navigate(id, b->addr_input);
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
        } else if (c == '\b') {
            if (b->addr_input_len > 0) b->addr_input[--b->addr_input_len] = 0;
        } else if (c == 27) { // Escape
            b->addr_bar_focused = 0;
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
        } else if (c >= 32 && c < 127 && b->addr_input_len < OKAI_URL_LEN - 1) {
            b->addr_input[b->addr_input_len++] = c;
            b->addr_input[b->addr_input_len] = 0;
        }
        // The address text is drawn by the chrome overlay — no page work.
        return;
    }

    struct wdom* d = T->doc ? wdoc_dom(T->doc) : 0;
    int fn = T->focused_node;
    if (d && fn >= 0 && fn < d->nn) {
        if (c == 27) { set_focus(id, T, -1); return; }
        if (c == '\t') { focus_next(id, T); return; }
        if (is_text_input(d, fn)) {
            if (c == '\n') {
                if (js_event(id, T, fn, "keydown", 0, 0, 0, 13)) return;
                if (is_tag(d, fn, T_textarea)) { edit_field(T, '\n'); js_event(id, T, fn, "input", 0, 0, 0, 10); }
                else submit_form(id, T, fn);
                return;
            }
            if (c == '\b' || (c >= 32 && c < 127)) {
                int key = c == '\b' ? 8 : (unsigned char)c;
                if (js_event(id, T, fn, "keydown", 0, 0, 0, key)) return;
                if (T->focused_node != fn || !T->doc) return;
                edit_field(T, c);
                js_event(id, T, fn, "input", 0, 0, 0, key);
                js_event(id, T, fn, "keyup", 0, 0, 0, key);
                return;
            }
            // arrows fall through to scrolling
        } else if (c == '\n' || c == ' ') {
            activate_control(id, T, fn);
            return;
        }
    }

    if (c == 'g' || c == 'G') {
        b->addr_bar_focused = 1;
        b->addr_input_len = 0;
        b->addr_input[0] = 0;
    } else if (c == 'J') {
        okai_scripts_on = !okai_scripts_on;
        serial_printf("[okai] page scripts %s\n", okai_scripts_on ? "on" : "off");
        okai_nav_reload(id);
    } else if (c == '\t') {
        focus_next(id, T);
    } else if (c == 'j' || c == '\n' || c == '\x10') {
        scroll_to(id, T->scroll_y + SCROLL_STEP);
    } else if (c == 'k' || c == '\x11') {
        scroll_to(id, T->scroll_y - SCROLL_STEP);
    } else if (c == ' ' || c == '\x04') { // space / Page Down
        scroll_to(id, T->scroll_y + (b->page_h > 120 ? b->page_h - 80 : 40));
    } else if (c == '\x12') {             // Page Up
        scroll_to(id, T->scroll_y - (b->page_h > 120 ? b->page_h - 80 : 40));
    } else if (c == 'r') {
        okai_nav_reload(id);
    } else if (c == 'b') {
        okai_nav_back(id);
    } else if (c == 'f') {
        okai_nav_fwd(id);
    }
}

void okai_handle_mouse_scroll(int id, int dy) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    struct okai_tab* T = okai_tab_of(b);
    serial_printf("[okai] wheel dy=%d old_scroll=%d\n", dy, T->scroll_y);
    scroll_to(id, T->scroll_y + dy * WHEEL_STEP);
}


// Advance tab open/close animations. Eases every live tab's rendered width
// (anim_w) toward its target with a frame-rate-independent exponential ease-out
// (tied to the 100Hz tick clock), then removes any tab whose close finished.
// Returns 1 while still animating. The chrome is repainted every frame by
// okai_paint_overlays and flushed by graphics_flush(), so no full window
// re-render is needed here.
// Read-only tab-animation check: 1 while any tab's width still eases toward
// its target (mirrors okai_anim_step's targets without advancing state).
int okai_is_animating(int id) {
    if (id < 0 || id >= MAX_OKAIS) return 0;
    struct okai* b = &okais[id];
    if (b->win_id < 0) return 0;
    struct window* w = window_get(b->win_id);
    if (!w) return 0;
    int tw = okai_ui_tab_width(b);
    for (int ti = 0; ti < b->tab_count; ti++) {
        int target = b->tabs[ti].closing ? 0 : tw;
        if (b->tabs[ti].anim_w != target) return 1;
    }
    return 0;
}

static int okai_anim_step(int id) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return 0;
    struct window* w = window_get(b->win_id);
    if (!w) return 0;
    int tw = okai_ui_tab_width(b);

    // Real-time delta since the last step, in ticks (10ms each at 100Hz).
    int dt = (int)(tick_count - okai_last_tick);
    okai_last_tick = tick_count;
    if (dt < 0) dt = 0;
    if (dt > 10) dt = 10;   // clamp after a long stall / hidden window

    // Ease each tab's width toward its target (exponential ease-out).
    int animating = 0;
    for (int ti = 0; ti < b->tab_count; ti++) {
        int target = b->tabs[ti].closing ? 0 : tw;
        int cur = b->tabs[ti].anim_w;
        if (cur != target && dt > 0) {
            for (int k = 0; k < dt; k++) {
                int delta = target - cur;
                int step = (delta * OKAI_ANIM_FACTOR + 50) / 100;
                if (step == 0) step = (delta > 0 ? 1 : -1);
                cur += step;
                if ((delta > 0 && cur >= target) || (delta < 0 && cur <= target)) { cur = target; break; }
            }
        }
        if (cur != target) animating = 1;
        b->tabs[ti].anim_w = cur;
    }

    // Remove tabs whose close animation finished (width hit zero).
    for (int i = 0; i < b->tab_count; ) {
        if (b->tabs[i].closing && b->tabs[i].anim_w <= 0) {
            tab_dispose(id, i);
            for (int j = i; j < b->tab_count - 1; j++) b->tabs[j] = b->tabs[j + 1];
            b->tabs[b->tab_count - 1].doc = 0;
            b->tab_count--;
            if (b->active_tab >= b->tab_count) b->active_tab = b->tab_count - 1;
            else if (i < b->active_tab) b->active_tab--;
        } else {
            i++;
        }
    }
    if (b->tab_count == 0) { okai_close(id); return 0; } // last tab closed -> close window
    return animating;
}

// Loading state of a tab for the chrome spinner: 2 = main document in
// flight, 1 = resources in flight, 0 = idle.
int okai_tab_busy(int id, int tab) {
    if (id < 0 || id >= MAX_OKAIS || okais[id].win_id < 0) return 0;
    if (req_has_main(id, tab)) return 2;
    return okais[id].tabs[tab].sub_inflight > 0 ? 1 : 0;
}

// Bytes received so far for the active tab's fetches (main document alone
// while it loads, else the sum over its resources), -1 when none run.
long okai_fetch_bytes(int id, int* subs) {
    struct okai* b = &okais[id];
    long rxb = -1;
    *subs = 0;
    for (int i = 0; i < FETCH_MAX; i++) {
        struct oreq* q = &reqs[i];
        if (!q->used || q->bi != id || q->tab != b->active_tab || q->h < 0) continue;
        long p = fetch_progress(q->h);
        if (q->sub_id < 0) { *subs = 0; return p; }
        rxb = (rxb < 0 ? 0 : rxb) + p;
        (*subs)++;
    }
    return rxb;
}

// Scroll position indicator along the right edge of the page area.
static void okai_draw_scrollbar(int id) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return;
    struct okai_tab* T = okai_tab_of(b);
    int sx, sy, pw, ph;
    if (!page_geom(b, &sx, &sy, &pw, &ph)) return;
    int total = T->content_height;
    if (total <= ph || ph < 40) return;
    int track = ph - 8;
    int th = (int)((long)track * ph / total);
    if (th < 32) th = 32;
    int maxs = total - ph;
    int pos = T->scroll_y > maxs ? maxs : T->scroll_y;
    // 32-bit safe: (track - th) <= ~1100, pos scaled down for huge pages
    int den = maxs, num = pos;
    while (den > 1000000) { den >>= 4; num >>= 4; }
    int ty = sy + 4 + (den > 0 ? (track - th) * num / den : 0);
    round_rect_fill(sx + pw - 10, ty, 6, th, 0x008F8F9D, 3);
}

// Chrome overlay for one window. desktop.c paints it right after the window
// itself (in z-order). The security panel (okai_ui.c) must sit ON TOP of
// page content, so the page-area overlays go first and chrome last.
void okai_paint_overlays(int id) {
    // This *is* the live render path (desktop.c calls it every main-loop
    // iteration for each visible okai window). Stepping the animation here
    // grows/shrinks each tab's anim_w; the chrome drawn below is flushed every
    // frame by graphics_flush(), so no full window re-render is needed during
    // the animation (the page only repaints when its content changes).
    okai_anim_step(id);
    if (!okai_get(id)) return;
    okai_draw_scrollbar(id);
    okai_ui_paint(id);
}

// Draw okai's chrome overlay clipped to the given sub-rects (okai's window
// minus any higher-z overlapping windows). This keeps the overlay from
// painting over a covering window, so that window does NOT need to be force-
// repainted every frame — which was tanking FPS when a window sat over okai.
// rects[i] = {x, y, w, h} in screen coords; nr may be 0 (fully covered).
void okai_paint_overlays_rects(int id, int rects[][4], int nr) {
    okai_anim_step(id);
    if (!okai_get(id)) return;
    for (int i = 0; i < nr; i++) {
        graphics_set_clip(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        okai_draw_scrollbar(id);
        okai_ui_paint(id);
    }
    graphics_clip_reset();
    okai_ui_painted_full(id);
}

int okai_find_by_win(int win_id) {
    for (int i = 0; i < MAX_OKAIS; i++) {
        if (okais[i].win_id == win_id) return i;
    }
    return -1;
}

struct okai* okai_get(int id) {
    if (id < 0 || id >= MAX_OKAIS) return 0;
    if (okais[id].win_id < 0) return 0;
    return &okais[id];
}
