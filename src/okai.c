#define BLITGEO 1 // TEMP
#include "okai.h"
#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "net/network.h"
#include "net/tls_net.h"
#include "crypto/certverify.h" // CV_ERR_* for the warning-page reason line
#include "crypto/tls_client.h" // tls_ticket_drop() for the resumption fallback
#include "filesystem.h"
#include "serial.h"
#include "js/js_dom.h"
#include "layout.h"
#include "cjk.h"
#include <stdint.h>
#include <string.h>

// Wall-clock baseline for the tab open/close animation. okai_anim_step eases
// each tab's width toward its target once per 10ms tick (the PIT runs at 100Hz),
// so the animation duration is tied to real time, not the main-loop spin rate.
extern uint32_t tick_count;
static uint32_t okai_last_tick = 0;

// Tab chrome sizing / animation constants
#define OKAI_XBOX       16   // close-× square outline box, px
#define OKAI_NB         32   // new-tab "+" button box, px (matches the 32px glyph)
#define OKAI_ANIM_FACTOR 18  // % of remaining width eased per 10ms tick (exponential
                             // ease-out; ~150ms to fully open/close a tab)

static struct okai okais[MAX_OKAIS];
static int okai_count = 0;

// The network stack services a SINGLE global TCP connection + response buffer,
// so only one okai fetch may be in flight at a time. okai_fetch_owner is the id
// of the window that currently owns that in-flight fetch (-1 when idle). The
// desktop response loop starts pending windows (token_count == 0) in turn and
// attributes each completed response to its owner, which is the only correct
// behavior the single-connection architecture can give (concurrent/mixed
// fetches would corrupt the shared TLS/TCP state and mis-route responses).
int okai_fetch_owner = -1;

// External: save filename for browse command
extern void net_set_browse_save(const char* filename);

static void okai_tab_reset(struct okai_tab* T);

void okai_init(void) {
    for (int i = 0; i < MAX_OKAIS; i++) {
        okais[i].win_id = -1;
        okais[i].active_tab = 0;
        okais[i].tab_count = 0;
        okais[i].show_security = 0;
        for (int t = 0; t < OKAI_MAX_TABS; t++) {
            okai_tab_reset(&okais[i].tabs[t]);
            okais[i].tabs[t].anim_w = 0;
            okais[i].tabs[t].closing = 0;
        }
    }
    okai_count = 0;
    okai_fetch_owner = -1;
}

// ---- Tabs --------------------------------------------------------------------

struct okai_tab* okai_tab_of(struct okai* b) { return &b->tabs[b->active_tab]; }

int okai_window_count(void) { return okai_count; }

static void okai_tab_reset(struct okai_tab* T) {
    T->url[0] = 0;
    T->title[0] = 0;
    T->scroll_y = 0;
    T->content_height = 0;
    T->token_count = 0;
    T->last_resp_len = 0;
    T->is_https = 0;
    T->https_fell_back = 0;
    T->conn_retries = 0;
    T->cert_failed = 0;
    T->cert_detail = 0;
    T->conn_failed = 0;
    T->conn_kind = 0;
    T->truncated = 0;
    T->css_n = 0;
    T->link_count = 0;
    T->field_count = 0;
    T->focused_input = -1;
    T->history_count = 0;
    T->history_pos = 0;
    T->redirect_count = 0;
    T->sub_res_phase = 0;
    T->sub_res_idx = 0;
    T->sub_res_count = 0;
    T->sub_res_css_changed = 0;
    memset(T->sub_res_type, 0, OKAI_MAX_SUBRES);
    memset(T->sub_res_urls, 0, sizeof(T->sub_res_urls));
}

// Switching re-renders the tab's cached page — no refetch.
int okai_switch_tab(int id, int tab) {
    if (id < 0 || id >= okai_count) return -1;
    struct okai* b = &okais[id];
    if (tab < 0 || tab >= b->tab_count || tab == b->active_tab) return -1;
    b->active_tab = tab;
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
    okai_tab_reset(&b->tabs[tab]);
    b->tabs[tab].anim_w = 0;   // open animation: grow from zero width
    b->tabs[tab].closing = 0;
    okai_last_tick = tick_count; // start the ease-out cleanly from width 0
    okai_navigate(id, url);
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
    if (tab == b->active_tab) {
        // switch to a neighbour so the visible page updates immediately
        int nb = (tab + 1 < b->tab_count) ? tab + 1 : tab - 1;
        b->active_tab = nb;
        okai_render_content(id);
    }
    if (b->win_id >= 0) window_set_dirty(b->win_id);
    serial_printf("[okai] closing tab %d, active=%d\n", tab, b->active_tab);
}

// ---- Internal homepage --------------------------------------------------------

// Rendered through the normal pipeline (html_parse + CSS engine) — headings,
// styles and clickable links work like any page. No network.
static const char OKAI_HOME_HTML[] =
"<!DOCTYPE html><html><head><title>Homepage</title>"
"<style>body{background:#1a2e4d;color:#ffffff}"
"h1{color:#7db4f5;text-align:center}a{color:#55c1ff}"
".sub{text-align:center;color:#9aa7b8}</style></head><body>"
"<h1>okai</h1>"
"<p class=sub>okernel web browser</p>"
"<hr>"
"<h3>Quick links</h3>"
"<ul>"
"<li><a href=\"http://example.com/\">example.com — test page</a></li>"
"<li><a href=\"https://notdexy.ru/\">notdexy.ru</a></li>"
"<li><a href=\"https://www.wikipedia.org/\">Wikipedia</a></li>"
"<li><a href=\"http://info.cern.ch/\">info.cern.ch — the first website</a></li>"
"</ul>"
"<hr>"
"<p class=sub>Type a URL below or press g to focus the address bar.</p>"
"</body></html>";

int okai_is_home(const char* url) {
    return url[0] == 'o' && url[1] == 'k' && url[2] == 'a' && url[3] == 'i' &&
           url[4] == ':' && url[5] == 'h' && url[6] == 'o' && url[7] == 'm' &&
           url[8] == 'e' && url[9] == 0;
}

// Parse + render the internal homepage into the active tab.
static void okai_load_home(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    {   // local copy (net_copy_str is static to network.c)
        int i = 0; while (OKAI_HOME_URL[i] && i < OKAI_URL_LEN - 1) { T->url[i] = OKAI_HOME_URL[i]; i++; }
        T->url[i] = 0;
        i = 0; while ("okai Homepage"[i] && i < 63) { T->title[i] = "okai Homepage"[i]; i++; }
        T->title[i] = 0;
    }
    int len = 0; while (OKAI_HOME_HTML[len]) len++;
    static char home_css[INLINE_CSS_SCRATCH];
    int css_len = html_extract_css(OKAI_HOME_HTML, len, home_css, INLINE_CSS_SCRATCH);
    T->css_n = css_parse(home_css, css_len, T->css_rules, CSS_MAX_RULES);
    int count = html_parse(OKAI_HOME_HTML, len, T->tokens, OKAI_TAB_TOKENS);
    dom_build(&T->dom, OKAI_HOME_HTML, len);
    T->token_count = count > 0 ? count : -1;
    T->scroll_y = 0;
    T->history_count = 0; // home is the root of its own history
    T->history_pos = 0;
    okai_render_content(id);
    window_set_title(b->win_id, T->title);
    serial_puts("[br] home rendered\n");
}

static void parse_url(const char* url, char* host, char* path, int* port) {
    host[0] = 0;
    path[0] = 0;
    if (port) *port = 0;
    int i = 0;
    // Skip http:// (7) or https:// (8)
    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
        url[4] == ':' && url[5] == '/' && url[6] == '/') {
        i = 7;
    } else if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
               url[4] == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/') {
        i = 8;
    }
    // Extract host
    int hi = 0;
    while (url[i] && url[i] != '/' && url[i] != ':' && hi < 127) {
        host[hi++] = url[i++];
    }
    host[hi] = 0;
    // Capture port if present
    if (url[i] == ':') {
        i++;
        int p = 0;
        while (url[i] >= '0' && url[i] <= '9') { p = p * 10 + (url[i] - '0'); i++; }
        if (port) *port = p;
    }
    // Extract path
    if (url[i] == '/') {
        int pi = 0;
        while (url[i] && pi < 127) {
            path[pi++] = url[i++];
        }
        path[pi] = 0;
    } else {
        path[0] = '/';
        path[1] = 0;
    }
}

// Resolve a link href against the current page into an absolute URL.
static void okai_resolve_href(struct okai* b, const char* href, char* out, int outlen) {
    struct okai_tab* T = okai_tab_of(b);
    out[0] = 0;
    if (!href || !href[0]) return;

    // Already absolute (http:// or https://)
    if ((href[0]=='h'&&href[1]=='t'&&href[2]=='t'&&href[3]=='p'&&href[4]==':'&&href[5]=='/') ||
        (href[0]=='h'&&href[1]=='t'&&href[2]=='t'&&href[3]=='p'&&href[4]=='s'&&href[5]==':'&&href[6]=='/')) {
        int i = 0;
        while (href[i] && i < outlen - 1) { out[i] = href[i]; i++; }
        out[i] = 0;
        return;
    }

    // Protocol-relative //host/path
    if (href[0] == '/' && href[1] == '/') {
        const char* scheme = T->is_https ? "https:" : "http:";
        int i = 0;
        while (scheme[i] && i < outlen - 1) { out[i] = scheme[i]; i++; }
        int j = 0;
        while (href[j] && i < outlen - 1) { out[i] = href[j]; i++; j++; }
        out[i] = 0;
        return;
    }

    // Fragment only -> stay on the current page
    if (href[0] == '#') {
        int i = 0;
        while (T->url[i] && i < outlen - 1) { out[i] = T->url[i]; i++; }
        out[i] = 0;
        return;
    }

    // Otherwise resolve relative to the current host + directory.
    const char* p = T->url;
    if (p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]==':'&&p[5]=='/'&&p[6]=='/') p += 7;
    else if (p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'&&p[4]=='s'&&p[5]==':'&&p[6]=='/'&&p[7]=='/') p += 8;
    char host[128]; int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 127) host[hi++] = *p++;
    host[hi] = 0;
    // Preserve a numeric :port suffix (bisected 2026-09-10: relative hrefs
    // on http://host:port/ dropped the port and fetched :80 instead).
    char port[8]; int pi = 0;
    if (*p == ':') {
        p++;
        while (*p >= '0' && *p <= '9' && pi < 7) port[pi++] = *p++;
    }
    port[pi] = 0;

    const char* scheme = T->is_https ? "https://" : "http://";
    int i = 0;
    while (scheme[i] && i < outlen - 1) { out[i] = scheme[i]; i++; }
    int j = 0;
    while (host[j] && i < outlen - 1) { out[i] = host[j]; i++; j++; }
    if (pi > 0) {
        if (i < outlen - 1) out[i++] = ':';
        int q = 0;
        while (port[q] && i < outlen - 1) { out[i] = port[q]; i++; q++; }
    }

    if (href[0] == '/') {
        int k = 0;
        while (href[k] && i < outlen - 1) { out[i] = href[k]; i++; k++; }
    } else {
        int lastslash = -1;
        for (int x = 0; p[x]; x++) if (p[x] == '/') lastslash = x;
        int x = 0;
        while (x <= lastslash && i < outlen - 1) { out[i] = p[x]; i++; x++; }
        int k = 0;
        while (href[k] && i < outlen - 1) { out[i] = href[k]; i++; k++; }
    }
    out[i] = 0;
}

// ---- Virtual document (scroll model) ----
// The whole page is laid out ONCE into this offscreen grid, then the visible
// slice [scroll_y, scroll_y+view_h) is blitted into the window content
// buffer. This makes scrolling LINE-based and fixes two bugs of the old
// token-granular skip: (1) content_height was re-measured from the scrolled
// render, so every scroll step shrank the max-scroll bound (~2 lines per 1
// line scrolled) and j/k stalled after a few presses; (2) skipping one token
// skipped 2-3 rendered lines, so the page jumped and misaligned.
//
// Layout is produced by src/layout.c into a display list (g_lay); the renderer
// blits the visible slice into the window content buffer. Scrolling is a pure
// document-row offset, so content_height never shifts under the scroll bound.
#define OKAI_TEXT_PAD  3   // left/right page margin in character columns

// The most recent render's display list for the ACTIVE tab (heading overlay
// reads it after window_draw). One shared per-window slot is not needed: the
// overlay runs immediately after each okai window paints, and only the
// active tab of each window is ever laid out.
static struct layout g_lay[MAX_OKAIS];
static uint32_t g_page_fg, g_page_bg; // page text/bg colors, for the heading pixel pass

// Map a byte to a printable glyph: control bytes become spaces; bytes
// 0x80-0xFF are font slots (Cyrillic + symbols) and pass through. Block
// slots 0x01/0x02 (KAnarchy logo) pass through too — they render blank
// nowhere; the layout splitter already treats them as word chars.
static char doc_sanitize(char ch) {
    if (ch == 1 || ch == 2) return ch;
    if ((unsigned char)ch >= 128) return ch;
    if (ch < 32) return ' ';
    if (ch == 127) return '?';
    return ch;
}

// ---- Layout pass only: DOM -> display list (no window writes except the
// page background). Scrolling a page re-blits the existing layout — it must
// NOT pay for layout_run again (on a wikipedia-size page that pass alone is
// most of a frame). Callers that change content/DOM/CSS call
// okai_render_content() (relayout + blit); pure scroll offset changes call
// okai_blit_content() (blit only).
void okai_relayout(int ed_id) {
    struct okai* b = &okais[ed_id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;
    if (!w->content) return;
    if (T->token_count == -1 && T->last_resp_len == 0) return; // error pages: no DOM

    int view_w = w->content_w;
    int view_h = w->content_h - CHROME_ROWS; // Reserve top rows for pixel chrome
    if (view_h < 0) view_h = 0;

    // ---- Compute the page's base colors from the <body> rules ----
    const struct css_rule* ua = 0;
    int ua_n = css_ua_rules(&ua);

    struct css_style body_style;
    css_compute(T->css_rules, T->css_n, "body", 0, 0, 0, &body_style);
    // Merge the UA body defaults (and any page body rules) so the page
    // background and default text color are correct even un-styled.
    if (ua_n > 0) {
        struct css_style ub;
        css_compute(ua, ua_n, "body", 0, 0, 0, &ub);
        css_merge_base(&body_style, &ub);
    }
    uint32_t page_bg = body_style.has_bg_rgb ? body_style.bg_rgb : WIN_BG_RGB;
    uint32_t default_fg;
    if (body_style.has_fg_rgb) default_fg = body_style.fg_rgb;
    else if (window_rgb_is_light(page_bg)) default_fg = 0x000000;
    else default_fg = 0xFFFFFF;
    g_page_fg = default_fg; g_page_bg = page_bg;

    int page_left = OKAI_TEXT_PAD;
    int page_w = view_w - 2 * OKAI_TEXT_PAD;
    if (page_w < 20) page_w = 20;

    // ---- Layout pass: DOM -> display list (authored CSS + UA sheet) ----
    // Author rules and the UA sheet share one table so inheritance and
    // specificity resolve in a single cascade. UA rules carry a big negative
    // specificity base, so any author rule wins a tie.
    // Static (not stack): ~336 rules is ~200KB, which would blow the 256KB
    // kernel stack once the renderer's own frames are added.
    static struct css_rule combined[CSS_MAX_RULES + CSS_UA_MAX_RULES];
    int combined_n = 0;
    for (int i = 0; i < ua_n && combined_n < CSS_MAX_RULES + CSS_UA_MAX_RULES; i++)
        combined[combined_n++] = ua[i];
    for (int i = 0; i < T->css_n && combined_n < CSS_MAX_RULES + CSS_UA_MAX_RULES; i++)
        combined[combined_n++] = T->css_rules[i];

    struct layout_opts lo;
    lo.width_cols = page_w;
    lo.page_left = page_left;
    lo.page_bg = page_bg;
    lo.page_fg = default_fg;
    struct layout* layp = &g_lay[ed_id];
    layout_run(&T->dom, combined, combined_n, &lo, layp);
    serial_printf("[lay] id=%d tab=%d/nt=%d toks=%d items=%d h=%d bg=%x\n", ed_id,
                  b->active_tab, b->tab_count, T->token_count,
                  layp->n_items, layp->height, layp->page_bg);

    window_set_content_bg_rgb(b->win_id, layp->page_bg);

    T->content_height = layp->height;

    // ---- Clamp scroll against the real document height ----
    {
        int max_scroll = layp->height - view_h;
        if (max_scroll < 0) max_scroll = 0;
        if (T->scroll_y > max_scroll) T->scroll_y = max_scroll;
        if (T->scroll_y < 0) T->scroll_y = 0;
    }
}

// Blit core: writes the visible slice of the EXISTING layout into the window
// (no layout_run). When log_links==0 the link/field records still update
// (clicks must hit-test post-scroll rows) but the per-link serial dump is
// skipped — a wikipedia page logs hundreds of lines per scroll step otherwise.
// Linear RGB interpolation for gradient bands (t = 0..255).
static uint32_t okai_lerp_rgb(uint32_t c0, uint32_t c1, int t) {
    int r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF;
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r = r0 + (r1 - r0) * t / 255;
    int g = g0 + (g1 - g0) * t / 255;
    int b = b0 + (b1 - b0) * t / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void okai_blit_inner(int ed_id, int log_links) {
    struct okai* b = &okais[ed_id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;
    if (!w->content) return;


    T->link_count = 0;
    T->field_count = 0;

    int view_w = w->content_w;
    int view_h = w->content_h - CHROME_ROWS; // Reserve top rows for pixel chrome
    if (view_h < 0) view_h = 0;

    // Failed fetch (DNS failure, unreachable host, timeout): show a readable
    // error instead of a black page. token_count == -1 with no response bytes
    // means nothing ever arrived; a response that parsed to zero tokens is a
    // different (benign) case.
    if (T->token_count == -1 && T->last_resp_len == 0) {
        // Drop the previous page's layout: the heading overlay reads g_lay
        // live, and without this the old page's H1/H2 keeps floating over
        // the error text. (Link/field regions are reset above already.)
        g_lay[ed_id].n_items = 0;
        g_lay[ed_id].height = 0;
        uint32_t page_bg = WIN_BG_RGB;
        uint32_t default_fg = 0xFFFFFF;
    g_page_fg = default_fg; g_page_bg = page_bg;
        window_set_content_bg_rgb(b->win_id, page_bg);
        window_clear(b->win_id);
        window_set_cursor(b->win_id, CHROME_ROWS, OKAI_TEXT_PAD);

        if (T->truncated) {
            window_set_text_color_rgb(b->win_id, 0xFF5555, 0x000000);
            window_puts(b->win_id, "\n RESPONSE TRUNCATED\n\n");
            window_set_text_color_rgb(b->win_id, 0xAAAAAA, 0x000000);
            window_puts(b->win_id, " The secure connection ended\n");
            window_puts(b->win_id, " mid-response. The page may have\n");
            window_puts(b->win_id, " been cut by an attacker.\n\n");
            window_puts(b->win_id, " Nothing was loaded and no HTTP\n");
            window_puts(b->win_id, " fallback was attempted.\n");
            window_set_text_color_rgb(b->win_id, default_fg, page_bg);
            T->content_height = 10;
            w->dirty = 1;
            return;
        }
        if (T->cert_failed) {
            window_puts(b->win_id, "\n SECURITY WARNING\n\n");
            window_set_text_color_rgb(b->win_id, 0xAAAAAA, 0x000000);
            window_puts(b->win_id, " The certificate for\n ");
            {
                int i = 0;
                while (T->url[i] && i < view_w - 2) {
                    char ch = T->url[i];
                    if (ch < 32 || (unsigned char)ch >= 127) ch = '?';
                    window_put_char(b->win_id, ch);
                    i++;
                }
            }
            window_puts(b->win_id, "\n failed verification.\n\n");
            {
                const char* why = NULL;
                if (T->cert_detail == CV_ERR_PINCHANGED)
                    why = " Reason: site key changed since first visit.\n";
                else if (T->cert_detail == CV_ERR_EXPIRED)
                    why = " Reason: certificate expired (or no clock).\n";
                else if (T->cert_detail == CV_ERR_HOSTNAME)
                    why = " Reason: name does not match certificate.\n";
                else if (T->cert_detail == CV_ERR_KEYUSE)
                    why = " Reason: key not valid for this use.\n";
                else if (T->cert_detail == CV_ERR_ROOT)
                    why = " Reason: unknown issuer (not in store).\n";
                else if (T->cert_detail == CV_ERR_CHAIN)
                    why = " Reason: chain signature invalid.\n";
                else if (T->cert_detail == CV_ERR_REVOKED)
                    why = " Reason: certificate revoked (local blocklist).\n";
                else if (T->cert_detail == CV_ERR_PRELOAD)
                    why = " Reason: site key differs from pinned key.\n";
                if (why) window_puts(b->win_id, why);
            }
            window_puts(b->win_id, " The connection may be intercepted,\n");
            window_puts(b->win_id, " the site's certificate expired, or\n");
            window_puts(b->win_id, " the identity does not match.\n\n");
            window_puts(b->win_id, " Nothing was loaded and no HTTP\n");
            window_puts(b->win_id, " fallback was attempted.\n");
            window_puts(b->win_id, " Revocation: stapled OCSP is enforced\n");
            window_puts(b->win_id, " when sent; absent staples are not\n");
            window_puts(b->win_id, " fetched (soft-fail).\n");
            window_set_text_color_rgb(b->win_id, default_fg, page_bg);
            T->content_height = 14;
            w->dirty = 1;
            return;
        }
        if (T->conn_failed) {
            if (T->conn_kind == OKAI_CONN_TOOLARGE)
                window_puts(b->win_id, "\n PAGE TOO LARGE\n\n");
            else
                window_puts(b->win_id, "\n CONNECTION ERROR\n\n");
            window_set_text_color_rgb(b->win_id, 0xAAAAAA, 0x000000);
            window_puts(b->win_id, " Could not load:\n ");
            {
                int i = 0;
                while (T->url[i] && i < view_w - 2) {
                    char ch = T->url[i];
                    if (ch < 32 || (unsigned char)ch >= 127) ch = '?';
                    window_put_char(b->win_id, ch);
                    i++;
                }
            }
            if (T->conn_kind == OKAI_CONN_TOOLARGE) {
                window_puts(b->win_id, "\n The page is larger than the\n");
                window_puts(b->win_id, " 2MB fetch buffer and was not\n");
                window_puts(b->win_id, " loaded (nothing rendered).\n\n");
            } else {
                window_puts(b->win_id, "\n The secure connection broke\n");
                window_puts(b->win_id, " before the page arrived.\n");
                window_puts(b->win_id, " This is NOT a certificate\n");
                window_puts(b->win_id, " problem - try reloading.\n\n");
            }
            window_puts(b->win_id, " Nothing was loaded and no HTTP\n");
            window_puts(b->win_id, " fallback was attempted.\n");
            window_set_text_color_rgb(b->win_id, default_fg, page_bg);
            T->content_height = 14;
            w->dirty = 1;
            return;
        }
        window_puts(b->win_id, "\n Unable to load page\n\n");
        window_set_text_color_rgb(b->win_id, 0xAAAAAA, 0x000000);
        window_puts(b->win_id, " The okai could not fetch:\n ");
        {
            int i = 0;
            while (T->url[i] && i < view_w - 2) {
                char ch = T->url[i];
                if (ch < 32 || (unsigned char)ch >= 127) ch = '?';
                window_put_char(b->win_id, ch);
                i++;
            }
        }
        window_puts(b->win_id, "\n\n Check the address, or the site may\n");
        window_puts(b->win_id, " be unreachable.\n");
        window_set_text_color_rgb(b->win_id, default_fg, page_bg);
        T->content_height = 8;
        w->dirty = 1;
        return;
    }

    window_clear(b->win_id);
    window_set_cursor(b->win_id, CHROME_ROWS, OKAI_TEXT_PAD);

    struct layout* layp = &g_lay[ed_id];
    uint32_t default_fg = g_page_fg; // stored by okai_relayout
#ifdef BLITGEO
    if (layp->n_items < 20)
        serial_printf("[blitgeo] ch=%d cw=%d view_h=%d scroll=%d items=%d height=%d wx=%d wy=%d ww=%d wh=%d\n",
                      w->content_h, w->content_w, view_h, T->scroll_y,
                      layp->n_items, layp->height, w->x, w->y, w->w, w->h);
#endif
    // A resize may have changed the view since layout: re-clamp cheaply
    // (no layout_run here — that is the whole point of the blit split).
    {
        int max_scroll = layp->height - view_h;
        if (max_scroll < 0) max_scroll = 0;
        if (T->scroll_y > max_scroll) T->scroll_y = max_scroll;
        if (T->scroll_y < 0) T->scroll_y = 0;
    }


// ---- Blit the visible slice ----
    // Item-driven: one pass over bands, one fill, one pass over lines. The old
    // code scanned ALL items per visible row (O(view * items) per scroll
    // step — brutal on wikipedia). Bands and lines are keyed by DOCUMENT row,
    // so scroll stays a pure offset and every cell gets the same final value.
    struct window* win = w;
    (void)win;
    int doc_first = T->scroll_y;
    int doc_last = T->scroll_y + view_h - 1;
    // 2) default page background (FIRST: bands and runs overpaint it)
    for (int r = 0; r < view_h; r++) {
        int brow = CHROME_ROWS + r;
        if (brow >= w->content_h) break;
        for (int c = 0; c < view_w; c++)
            window_write_cell_rgb(b->win_id, brow, c, ' ', default_fg, layp->page_bg);
    }
    // 3+4) bands then lines, in two phases: normal flow first, out-of-flow
    // (absolute/fixed) boxes on top — positioned content paints above
    // scrolled content (dropdown menus, fixed headers). Fixed rows carry
    // viewport rows (mapped without the scroll offset).
    for (int oof_phase = 0; oof_phase < 2; oof_phase++) {
        for (int bi = 0; bi < layp->n_items; bi++) {
            struct layout_item* it = &layp->items[bi];
            if (it->kind != LOUT_BAND || !it->draw_box) continue;
            if (!!it->is_oof != oof_phase) continue;
            int r0 = it->row, r1 = it->row + it->height - 1;
            if (!it->is_fixed && (r1 < doc_first || r0 > doc_last)) continue;
            int c0 = it->box_left, c1 = it->box_left + it->box_width;
            int rr0 = r0, rr1 = r1;
            if (!it->is_fixed) {
                if (rr0 < doc_first) rr0 = doc_first;
                if (rr1 > doc_last) rr1 = doc_last;
            }
            int is_grad = (it->draw_box >= 2);
            int grad_vert = (it->draw_box == 2);
            for (int dr = rr0; dr <= rr1; dr++) {
            int brow = it->is_fixed ? CHROME_ROWS + dr
                                    : CHROME_ROWS + (dr - T->scroll_y);
            if (brow < CHROME_ROWS || brow >= w->content_h) continue;
                uint32_t row_bg = it->box_bg;
                if (is_grad && grad_vert && it->height > 1)
                    row_bg = okai_lerp_rgb(it->box_bg, it->box_c1,
                                           (dr - r0) * 255 / (it->height - 1));
                for (int c = c0; c < c1 && c < view_w; c++) {
                    if (c < 0) continue;
                    if (it->box_border && (dr == it->row || dr == it->row + it->height - 1 ||
                                           c < c0 + 1 || c >= c1 - 1))
                        window_write_cell_rgb(b->win_id, brow, c, ' ', it->box_border, it->box_border);
                    else {
                        uint32_t bg = row_bg;
                        if (is_grad && !grad_vert && it->box_width > 1)
                            bg = okai_lerp_rgb(it->box_bg, it->box_c1,
                                               (c - c0) * 255 / (it->box_width - 1));
                        window_write_cell_rgb(b->win_id, brow, c, ' ', default_fg, bg);
                    }
                }
            }
        }
        // 4) line runs in the view (same phase: bands under their own lines)
        for (int li = 0; li < layp->n_items; li++) {
            struct layout_item* it = &layp->items[li];
            if (it->kind != LOUT_LINE) continue;
            if (!!it->is_oof != oof_phase) continue;
        {
            int dr = it->row;
            if (!it->is_fixed && (dr < doc_first || dr > doc_last)) continue;
            int brow = it->is_fixed ? CHROME_ROWS + dr
                                    : CHROME_ROWS + (dr - T->scroll_y);
            if (brow < CHROME_ROWS || brow >= w->content_h) continue;
                // Scaled headings (H1=3x, H2=2x) are painted by the pixel overlay
                // (okai_draw_heading_pixels). Their runs carry columns in SCALED
                // units, so blitting them into the 1-column grid here leaves
                // garbage in the gaps between glyphs. Leave the white fill and let
                // the overlay draw them.
                if (it->heading && (it->heading == 1 || it->heading == 2)) continue;
                int first = it->run_start, last = it->run_start + it->run_count - 1;
                for (int ri = first; ri <= last; ri++) {
                    struct layout_run* run = &layp->runs[ri];
                    int hidden = (run->flags & LAYOUT_FLAG_HIDE) != 0;
                    int run_cjk = !hidden && (run->flags & LAYOUT_FLAG_CJK) != 0;
                    // Runs without an explicit bg inherit the band beneath (else
                    // every glyph would punch a page-colored hole in boxes).
                    int use_band = (!hidden && run->bg == layp->page_bg);
                    if (run_cjk) {
                        // CJK run: text holds raw UTF-8 while text_len counts
                        // display CHARACTERS. Decode each char and draw from
                        // the CJK bitmap table (table miss '?', same as an
                        // unmapped codepoint today). Decode advances even for
                        // off-view cells so the byte cursor never desyncs.
                        // Band/focus logic mirrors the byte path below exactly.
                        int boff = 0;
                        for (int kk = 0; kk < run->text_len; kk++) {
                            int bl = 1;
                            uint32_t cp = utf8_decode_char(
                                layp->text + run->text_off + boff,
                                LAYOUT_MAX_TEXT - (int)run->text_off - boff, &bl);
                            if (bl <= 0) bl = 1;
                            boff += bl;
                            int c = run->col + kk;
                            if (c < 0 || c >= view_w) continue;
                            uint32_t bg = run->bg;
                            if (use_band) {
                                // topmost covering band wins (bands paint in order).
                                // Fixed and scrolling rows live in different spaces —
                                // never inherit across the boundary.
                                for (int bi = 0; bi < layp->n_items; bi++) {
                                    struct layout_item* bd = &layp->items[bi];
                                    if (bd->kind != LOUT_BAND || !bd->draw_box) continue;
                                    if (!!bd->is_fixed != !!it->is_fixed) continue;
                                    int bc1 = bd->box_left + bd->box_width;
                                    if (dr < bd->row || dr >= bd->row + bd->height) continue;
                                    if (c < bd->box_left || c >= bc1) continue;
                                    if (bd->draw_box >= 2 && bd->height > 1 && bd->box_width > 1) {
                                        if (bd->draw_box == 2)
                                            bg = okai_lerp_rgb(bd->box_bg, bd->box_c1,
                                                               (dr - bd->row) * 255 / (bd->height - 1));
                                        else
                                            bg = okai_lerp_rgb(bd->box_bg, bd->box_c1,
                                                               (c - bd->box_left) * 255 / (bd->box_width - 1));
                                    } else {
                                        bg = bd->box_bg;
                                    }
                                }
                            }
                            uint32_t pfg = run->fg, pbg = bg;
                            if (T->focused_input >= 0 &&
                                run->node == T->focused_input) {
                                pfg = bg; pbg = run->fg;
                            }
                            const uint8_t* g = cjk_glyph_for(cp);
                            if (g) window_write_cjk_cell(b->win_id, brow, c, cp,
                                                         pfg, pbg, run->flags);
                            else {
                                window_write_cell_rgb(b->win_id, brow, c, '?',
                                                      pfg, pbg);
                                uint8_t attr = run->flags & 3;
                                if (attr) window_write_cell_attr(b->win_id, brow, c, attr);
                            }
                        }
                        continue;
                    }
                    for (int k = 0; k < run->text_len; k++) {
                        int c = run->col + k;
                        if (c < 0 || c >= view_w) continue;
                        char ch = hidden ? ' ' : layp->text[run->text_off + k];
                        // Block slots 0x01/0x02 render real glyphs (KAnarchy
                        // logo); every other control byte is a space here.
                        if ((unsigned char)ch < 32 && ch != 1 && ch != 2) ch = ' ';
                        uint32_t bg = run->bg;
                        if (use_band) {
                            // topmost covering band wins (bands paint in order).
                            // Fixed and scrolling rows live in different spaces —
                            // never inherit across the boundary.
                            for (int bi = 0; bi < layp->n_items; bi++) {
                                struct layout_item* bd = &layp->items[bi];
                                if (bd->kind != LOUT_BAND || !bd->draw_box) continue;
                                if (!!bd->is_fixed != !!it->is_fixed) continue;
                                int bc1 = bd->box_left + bd->box_width;
                                if (dr < bd->row || dr >= bd->row + bd->height) continue;
                                if (c < bd->box_left || c >= bc1) continue;
                                if (bd->draw_box >= 2 && bd->height > 1 && bd->box_width > 1) {
                                    if (bd->draw_box == 2)
                                        bg = okai_lerp_rgb(bd->box_bg, bd->box_c1,
                                                           (dr - bd->row) * 255 / (bd->height - 1));
                                    else
                                        bg = okai_lerp_rgb(bd->box_bg, bd->box_c1,
                                                           (c - bd->box_left) * 255 / (bd->box_width - 1));
                                } else {
                                    bg = bd->box_bg;
                                }
                            }
                        }
                        // Keyboard/mouse focus indicator: invert the focused
                        // field's cells so Tab/click focus is visible with
                        // no layout change (paint-only).
                        uint32_t pfg = run->fg, pbg = bg;
                        if (!hidden && T->focused_input >= 0 &&
                            run->node == T->focused_input) {
                            pfg = bg; pbg = run->fg;
                        }
                        window_write_cell_rgb(b->win_id, brow, c, ch, pfg, pbg);
                        uint8_t attr = hidden ? 0 : run->flags;
                        if (attr) window_write_cell_attr(b->win_id, brow, c, attr);
                    }
                }
            }
        }
    } // end oof_phase (normal flow, then positioned boxes on top)

    // ---- Record clickable link regions (buffer rows) ----
    for (int li = 0; li < layp->n_items && T->link_count < OKAI_MAX_LINKS; li++) {
        struct layout_item* it = &layp->items[li];
        if (it->kind != LOUT_LINE) continue;
        int row = it->is_fixed ? it->row + CHROME_ROWS
                               : it->row - T->scroll_y + CHROME_ROWS;
        if (row < CHROME_ROWS || row >= w->content_h) continue;
        int first = it->run_start, last = it->run_start + it->run_count - 1;
        for (int ri = first; ri <= last; ri++) {
            struct layout_run* run = &layp->runs[ri];
            if (!run->is_link || run->node >= T->dom.node_count) continue;
            if (run->flags & LAYOUT_FLAG_HIDE) continue; // invisible: not clickable
            char href[192];
            if (dom_attr_get(&T->dom, run->node, "href", href, sizeof(href)) < 0) continue;
            int col0 = run->col, col1 = run->col + run->text_len - 1;
            // Extend an existing region on the same row if it is the same link.
            int merged = 0;
            for (int k = 0; k < T->link_count; k++) {
                if (T->links[k].row == row && T->links[k].end_row == row &&
                    !strcmp(T->links[k].href, "") ) { merged = 0; }
            }
            (void)merged;
            int idx = T->link_count;
            T->links[idx].row = row;
            T->links[idx].end_row = row;
            T->links[idx].col0 = col0;
            T->links[idx].col1 = col1;
            okai_resolve_href(b, href, T->links[idx].href, OKAI_URL_LEN);
            T->link_count++;
            if (log_links)
                serial_printf("[okai] link[%d] row=%d col0=%d col1=%d href=%s endrow=%d\n",
                              idx, row, col0, col1, T->links[idx].href, row);
            if (T->link_count >= OKAI_MAX_LINKS) break;
        }
    }

    // ---- Record form control regions ----
    if (T->focused_input >= T->dom.node_count) T->focused_input = -1;
    for (int li = 0; li < layp->n_items && T->field_count < OKAI_MAX_LINKS; li++) {
        struct layout_item* it = &layp->items[li];
        if (it->kind != LOUT_LINE) continue;
        int row = it->is_fixed ? it->row + CHROME_ROWS
                               : it->row - T->scroll_y + CHROME_ROWS;
        if (row < CHROME_ROWS || row >= w->content_h) continue;
        int first = it->run_start, last = it->run_start + it->run_count - 1;
        for (int ri = first; ri <= last; ri++) {
            struct layout_run* run = &layp->runs[ri];
            if (!run->is_field || run->node >= T->dom.node_count) continue;
            if (run->flags & LAYOUT_FLAG_HIDE) continue;
            int is_btn = dom_tag_is(&T->dom, run->node, "button");
            int idx = T->field_count++;
            T->fields[idx].row = row;
            T->fields[idx].col0 = run->col;
            T->fields[idx].col1 = run->col + run->text_len - 1;
            T->fields[idx].node = run->node; // now a DOM node index
            T->fields[idx].is_button = is_btn;
            if (log_links)
                serial_printf("[okai] field[%d] row=%d col0=%d col1=%d btn=%d node=%d\n",
                              idx, row, T->fields[idx].col0, T->fields[idx].col1,
                              is_btn, run->node);
        }
    }

    T->content_height = layp->height;
    w->dirty = 1;
}

// Full render: re-layout then blit (content/DOM/CSS changed). Scroll offset
// changes use okai_blit_content() instead — layout is scroll-invariant.
void okai_render_content(int ed_id) {
    okai_relayout(ed_id);
    okai_blit_inner(ed_id, 1);
}

// Scroll-path blit: no layout_run, no link serial dump. Records still update
// so clicks hit-test the post-scroll rows.
void okai_blit_content(int ed_id) {
    okai_blit_inner(ed_id, 0);
}

// ---- Sub-resource fetch queue (<link rel=stylesheet>, <script src>) ----------

// Extract external CSS and JS URLs from the raw HTML and queue them for
// sequential fetching. Called after the main page is parsed and rendered.
void okai_queue_sub_resources(int id, const char* html, int html_len) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    T->sub_res_count = 0;
    T->sub_res_idx = 0;
    T->sub_res_phase = 0;
    T->sub_res_css_changed = 0;
    if (!html || html_len <= 0) return;

    // Extract <link rel="stylesheet" href="..."> URLs.
    char css_urls[OKAI_SUBRES_BUF];
    int n_css = html_extract_link_css(html, html_len, css_urls, OKAI_SUBRES_BUF);

    // Extract <script src="..."> URLs. External scripts are FETCHED but
    // deliberately NOT executed (HANDOFF #27): tinyjs is not a
    // spec-compliant runtime and running a real site's minified bundle
    // wedges the single-threaded kernel. The fetch keeps parity with the
    // documented behavior; the bytes are discarded in okai_sub_res_done
    // ('j' arm). Dropping them at queue time silently broke scripted pages.
    char js_urls[OKAI_SUBRES_BUF];
    int n_js = html_extract_script_src(html, html_len, js_urls, OKAI_SUBRES_BUF);

    if (n_css == 0 && n_js == 0) return;

    // Resolve all URLs against the current page URL and queue them
    int idx = 0;
    const char* p = css_urls;
    for (int i = 0; i < n_css && idx < OKAI_MAX_SUBRES; i++) {
        char abs_url[OKAI_URL_LEN];
        okai_resolve_href(b, p, abs_url, OKAI_URL_LEN);
        if (abs_url[0]) {
            int k = 0;
            while (abs_url[k] && k < OKAI_URL_LEN - 1) {
                T->sub_res_urls[idx][k] = abs_url[k]; k++;
            }
            T->sub_res_urls[idx][k] = 0;
            T->sub_res_type[idx] = 'c'; // CSS
            idx++;
        }
        while (*p) p++; p++; // skip to next null-terminated URL
    }

    p = js_urls;
    for (int i = 0; i < n_js && idx < OKAI_MAX_SUBRES; i++) {
        char abs_url[OKAI_URL_LEN];
        okai_resolve_href(b, p, abs_url, OKAI_URL_LEN);
        if (abs_url[0]) {
            int k = 0;
            while (abs_url[k] && k < OKAI_URL_LEN - 1) {
                T->sub_res_urls[idx][k] = abs_url[k]; k++;
            }
            T->sub_res_urls[idx][k] = 0;
            T->sub_res_type[idx] = 'j'; // JS (fetched, never executed)
            idx++;
        }
        while (*p) p++; p++;
    }

    T->sub_res_count = idx;
    if (idx > 0) {
        T->sub_res_phase = 1; // start fetching CSS links first
        T->sub_res_idx = 0;
        serial_printf("[okai] sub-res: %d resources queued (%d css, %d js)\n",
                      idx, n_css, n_js);
    }
}

// Start fetching the next sub-resource in the queue. Returns 0 if a fetch
// was started, 1 if all done, -1 on error.
int okai_start_sub_res_fetch(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);

    // Find the next resource to fetch
    while (T->sub_res_idx < T->sub_res_count) {
        int i = T->sub_res_idx;
        char type = T->sub_res_type[i];
        char* url = T->sub_res_urls[i];

        // Skip CSS during JS phase
        if (T->sub_res_phase == 2 && type == 'c') { T->sub_res_idx++; continue; }
        // Skip JS during CSS phase
        if (T->sub_res_phase == 1 && type == 'j') { T->sub_res_idx++; continue; }

        if (!url[0]) { T->sub_res_idx++; continue; }

        // Parse the URL and start the fetch
        char host[128], path[128];
        int url_port = 0;
        parse_url(url, host, path, &url_port);
        if (!host[0]) { T->sub_res_idx++; continue; }

        int is_https = (url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p' &&
                        url[4]=='s' && url[5]==':');
        serial_printf("[okai] sub-res fetch: %s %s\n", type == 'c' ? "CSS" : "JS", url);
        if (is_https) https_get_port(host, path, (uint16_t)url_port);
        else { http_reset_conn_attempts(); http_get_port(host, path, (uint16_t)url_port); }
        return 0;
    }

    // All resources in current phase done; advance to next phase
    if (T->sub_res_phase == 1) {
        T->sub_res_phase = 2; // switch to JS phase
        T->sub_res_idx = 0;
        return okai_start_sub_res_fetch(id); // recurse to start JS fetches
    }

    // All phases done
    T->sub_res_phase = 0;
    return 1;
}

// Called by desktop.c when a sub-resource fetch completes. Processes the
// response and advances to the next resource. Returns 1 if all done.
int okai_sub_res_done(int id, const char* resp, int resp_len) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (T->sub_res_phase == 0 || T->sub_res_idx >= T->sub_res_count) return 1;

    int i = T->sub_res_idx;
    char type = T->sub_res_type[i];

    if (type == 'c' && resp && resp_len > 0) {
        // Dechunk and strip HTTP headers
        int total = http_dechunk(resp, resp_len);
        // Find the body (after \r\n\r\n)
        char* body = resp;
        int body_len = total;
        for (int k = 0; k < total - 3; k++) {
            if (resp[k] == '\r' && resp[k+1] == '\n' && resp[k+2] == '\r' && resp[k+3] == '\n') {
                body = resp + k + 4;
                body_len = total - k - 4;
                break;
            }
        }
        // External CSS is raw CSS text (not wrapped in <style> tags).
        // Parse it directly into the table tail: sheets append in fetch
        // order (= cascade order), no concat buffer, no full re-parse.
        // Oversized sheets fill the remaining slots from the start rather
        // than dropping everything, so leading rules still style the page.
        int room_rules = CSS_MAX_RULES - T->css_n;
        int added = 0;
        if (room_rules > 0 && body_len > 0)
            added = css_parse(body, body_len,
                              T->css_rules + T->css_n, room_rules);
        if (added < 0) added = 0;
        T->css_n += added;
        T->sub_res_css_changed = 1; // external styling landed: re-render matters
        serial_printf("[okai] sub-res CSS: %d bytes, +%d rules (%d total)\n",
                      body_len, added, T->css_n);
    } else if (type == 'j' && resp && resp_len > 0) {
        // External scripts are intentionally not executed — see the note in
        // okai_queue_sub_resources(). A real site's bundle wedges tinyjs.
        serial_printf("[okai] sub-res JS: skipped %d bytes (not executed)\n", resp_len);
    }

    T->sub_res_idx++;
    // Re-render with updated CSS after each external stylesheet
    if (type == 'c') okai_render_content(id);

    return 0; // not done yet
}

// Begin the network fetch for window `id` using its current URL. Called by the
// desktop response loop (never more than one at a time — the owner model
// guarantees it). Detects scheme and kicks off http_get / https_get.
// Returns 0 if a fetch was started, -1 if refused (bad URL / empty host).
int okai_start_fetch(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return -1;
    if (okai_is_home(T->url)) { okai_load_home(id); return 0; }
    char host[128], path[128];
    int url_port = 0;
    parse_url(T->url, host, path, &url_port);
    if (!host[0]) {
        // No hostname (e.g. a malformed address-bar entry like "/path" or
        // "example.com page"). Never fire the request — a DNS query for an
        // empty host wedged the fetch owner until timeout.
        serial_puts("[okai] refusing fetch: empty host in '");
        serial_puts(T->url);
        serial_puts("'\n");
        T->token_count = -1;
        T->last_resp_len = 0;
        okai_render_content(id);
        return -1;
    }
    T->is_https = (T->url[0] == 'h' && T->url[1] == 't' && T->url[2] == 't' &&
                   T->url[3] == 'p' && T->url[4] == 's' && T->url[5] == ':');
    if (T->is_https) https_get_port(host, path, (uint16_t)url_port);
    else { http_reset_conn_attempts(); http_get_port(host, path, (uint16_t)url_port); }
    return 0;
}

// Called by the desktop response loop when an HTTPS fetch has definitively
// failed. Rewrites the tab URL to http:// and re-issues the fetch exactly once
// so http-only hosts still load. Returns okai_start_fetch()'s result, or -1 if
// we've already fallen back (so the caller can give up cleanly).
static void okai_rewrite_scheme_http(char* url); // defined later in this file
int okai_fallback_http(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return -1;
    if (T->https_fell_back) return -1;
    T->https_fell_back = 1;
    okai_rewrite_scheme_http(T->url); // https:// -> http://, in place
    serial_printf("[okai] https failed, retrying http: %s\n", T->url);
    T->token_count = 0;
    T->last_resp_len = 0;
    return okai_start_fetch(id);
}

// Resumption fallback (mirror of the http fallback above, but same-origin):
// some backends abort a resumed handshake (FIN instead of negotiating)
// while accepting a fresh full handshake for the same host. Only when the
// failed connection actually offered an UNACCEPTED psk (transport-class
// failure, never cert/hostname — those still fail closed), and only once
// per navigation (conn_retries guards the loop).
int okai_resumption_fallback(int id, int fail_reason) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return -1;
    if (T->conn_retries >= OKAI_CONN_MAX_RETRIES) return -1;
    // Only resumption-abort signatures retry: a full handshake can't fix
    // overflow (page still too big), RNG failure, or cert/hostname (which
    // must fail closed, never retry into anything).
    if (fail_reason != TLS_FAIL_PROTO && fail_reason != TLS_FAIL_ALERT &&
        fail_reason != TLS_FAIL_MAC)
        return -1;
    T->conn_retries++;
    // If we offered an unaccepted PSK, its ticket is poison for this
    // backend — drop it so the retry fully handshakes. A failed full
    // handshake retries identically (transient blips).
    if (tls_last_offer_unaccepted()) {
        char host[128], path[128];
        int url_port = 0;
        parse_url(T->url, host, path, &url_port);
        if (!host[0]) return -1;
        tls_ticket_drop(host);
        serial_printf("[okai] resumption aborted, retrying full handshake: %s\n", T->url);
    } else {
        serial_printf("[okai] fetch failed, retrying (%d/%d): %s\n",
                      T->conn_retries, OKAI_CONN_MAX_RETRIES, T->url);
    }
    T->token_count = 0;
    T->last_resp_len = 0;
    return okai_start_fetch(id);
}

// Default a web URL to HTTPS unless the user typed an explicit scheme.
// An explicit `https://` is kept; an explicit `http://` is RESPECTED as plain
// HTTP (typing the scheme is a deliberate choice — auto-upgrading it would make
// a plain-HTTP host unreachable, e.g. a local dev server a user named by hand);
// a bare host with no scheme defaults to HTTPS. The home/internal scheme and
// any other non-http scheme are left untouched.
static void okai_normalize_https(const char* in, char* out, int outlen) {
    int has_scheme = 0;
    for (int i = 0; in[i]; i++) {
        if (in[i] == ':') { has_scheme = 1; break; }
    }
    if (strncmp(in, "https://", 8) == 0 ||
        strncmp(in, "http://", 7) == 0 ||
        strncmp(in, "http:", 5) == 0 ||
        has_scheme) {
        // Explicit scheme (https, http, okai:home, ftp://, ...): use as-is.
        int j = 0; while (in[j] && j < outlen - 1) { out[j] = in[j]; j++; } out[j] = 0;
    } else {
        // Bare host (no scheme): default to https://.
        int j = 0; const char* s = "https://";
        while (s[j] && j < outlen - 1) { out[j] = s[j]; j++; }
        int i = 0; while (in[i] && j < outlen - 1) { out[j] = in[i]; j++; i++; }
        out[j] = 0;
    }
}

// Rewrite an in-place URL from https:// to http:// (used for the one-time
// HTTP fallback after an HTTPS fetch definitively fails).
static void okai_rewrite_scheme_http(char* url) {
    if (strncmp(url, "https://", 8) == 0) {
        int n = 0; while (url[n]) n++;
        for (int i = 4; i < n; i++) url[i] = url[i + 1]; // drop the 's' after "http"
    } else if (strncmp(url, "https:", 6) == 0) {
        int n = 0; while (url[n]) n++;
        for (int i = 4; i < n; i++) url[i] = url[i + 1];
    }
}

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
    okai_tab_reset(&b->tabs[0]);

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

    // Seed the tab's URL and history. Default every web URL to HTTPS
    // (http:// or bare host -> https://); the home/internal scheme is left alone.
    {
        char norm[OKAI_URL_LEN];
        okai_normalize_https(url, norm, OKAI_URL_LEN);
        int ui = 0;
        while (norm[ui] && ui < OKAI_URL_LEN - 1) { b->tabs[0].url[ui] = norm[ui]; ui++; }
        b->tabs[0].url[ui] = 0;
        for (int i = 0; i < ui + 1; i++) b->tabs[0].history[0][i] = b->tabs[0].url[i];
        b->tabs[0].history_count = 1;
        b->tabs[0].is_https = (strncmp(b->tabs[0].url, "https", 5) == 0);
    }
    if (okai_is_home(b->tabs[0].url)) {
        okai_load_home(id);
        return id;
    }
    // Defer the actual request: the desktop response loop starts it once the
    // previous fetch (if any) finishes — see okai_fetch_owner.
    b->tabs[0].token_count = 0;
    return id;
}

void okai_close(int id) {
    if (id < 0 || id >= okai_count) return;
    if (okais[id].win_id >= 0) {
        window_destroy(okais[id].win_id);
        okais[id].win_id = -1;
    }
    // Compact so the one-window policy (`okai` reuses slot 0) never targets
    // a dead slot (okai_get would return NULL mid-click-handler).
    for (int i = id; i < okai_count - 1; i++) okais[i] = okais[i + 1];
    okai_count--;
    if (okai_fetch_owner == id) okai_fetch_owner = -1;
    else if (okai_fetch_owner > id) okai_fetch_owner--;
}

void okai_navigate(int id, const char* url) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;

    // If this window owns the single in-flight fetch, abort it so the new URL
    // actually loads (otherwise the old response completes and fills the window,
    // discarding this navigation). The response loop will start the new fetch.
    if (okai_fetch_owner == id) okai_fetch_owner = -1;

    // Internal homepage: parse + render immediately, no network.
    if (okai_is_home(url)) {
        okai_tab_reset(T);
        okai_load_home(id);
        return;
    }

    // Set URL (default every web URL to HTTPS; home/internal scheme left alone).
    char norm[OKAI_URL_LEN];
    okai_normalize_https(url, norm, OKAI_URL_LEN);
    int ui = 0;
    while (norm[ui] && ui < OKAI_URL_LEN - 1) {
        T->url[ui] = norm[ui];
        ui++;
    }
    T->url[ui] = 0;

    // Add to history
    if (T->history_count < OKAI_MAX_HISTORY) {
        for (int i = 0; i < ui + 1; i++)
            T->history[T->history_count][i] = T->url[i];
        T->history_count++;
        T->history_pos = T->history_count - 1;
    }

    T->scroll_y = 0;
    T->token_count = 0;
    T->title[0] = 0;
    T->redirect_count = 0;
    T->https_fell_back = 0;
    T->conn_retries = 0;
    T->focused_input = -1; // stale DOM node from the old page must not persist
    T->cert_failed = 0;
    T->cert_detail = 0;
    T->conn_failed = 0;
    T->conn_kind = 0;
    T->truncated = 0;

    // Defer the request to the desktop response loop (single-connection owner
    // model); okai_start_fetch() derives scheme/host/path from T->url when it
    // actually fires, so nothing is fetched concurrently here.

    okai_render_content(id);
}

// ---- Toolbar nav buttons ---------------------------------------------------
// These mirror the back/forward/reload/home buttons drawn in okai_draw_chrome().
// They are wired so the (already-drawn) controls actually navigate.

// Hit-test the address bar. Geometry MUST mirror okai_draw_chrome(). Used by
// desktop.c so clicking the address bar focuses it for typing, while clicks
// elsewhere in the chrome band just focus/drag the window (the old blanket
// "any top-band click focuses + clears the address bar" wiped the URL display
// whenever a click missed a nav button).
int okai_addr_bar_hit(int id, int mx, int my) {
    if (id < 0 || id >= MAX_OKAIS) return 0;
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return 0;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible) return 0;
    int cx0 = w->x + WIN_BORDER;
    int topoff = w->no_titlebar ? 0 : WIN_TITLE_H;
    int tool_y = w->y + WIN_BORDER + topoff + CHROME_TAB_H;
    int btn = 30, gap = 8;
    int addr_x = cx0 + 8 + 4 * (btn + gap) + 10; // after the 4 nav buttons
    int ctrl_space = w->no_titlebar ? (WIN_CTRL_BTN + 10) : 8;
    int addr_w = (w->w - 2 * WIN_BORDER) - (addr_x - cx0) - ctrl_space;
    int ay = tool_y + 4, ah = CHROME_TOOL_H - 8;
    return addr_w > 24 && mx >= addr_x && mx < addr_x + addr_w &&
           my >= ay && my < ay + ah;
}

int okai_check_nav_click(int id, int mx, int my) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return NAV_NONE;
    struct window* w = window_get(b->win_id);
    if (!w) return NAV_NONE;
    // Geometry MUST match okai_draw_chrome(): button bar in the toolbar row.
    int cx0 = w->x + WIN_BORDER;
    int cy0 = w->y + WIN_BORDER + (w->no_titlebar ? 0 : WIN_TITLE_H);
    int tool_y = cy0 + CHROME_TAB_H;
    int btn = 30, gap = 8;
    int bx = cx0 + 8;
    int by = tool_y + (CHROME_TOOL_H - btn) / 2;
    if (my >= by && my <= by + btn) {
        for (int i = 0; i < 4; i++) {
            if (mx >= bx && mx <= bx + btn)
                return i == 0 ? NAV_BACK : i == 1 ? NAV_FWD :
                       i == 2 ? NAV_RELOAD : NAV_HOME;
            bx += btn + gap;
        }
    }
    // New-tab '+' box in the tab strip: after the LAST tab (mirrors the draw)
    {
        int n = b->tab_count; if (n < 1) n = 1;
        int cwp = w->w - 2 * WIN_BORDER;
        int tw = (cwp - 40) / n; if (tw > 300) tw = 300; if (tw < 60) tw = 60;
        int nb = OKAI_NB;
        int nbx = cx0 + 4 + n * (tw + 2);
        int nby = cy0 + (CHROME_TAB_H - nb) / 2;
        if (mx >= nbx && mx <= nbx + nb && my >= nby && my <= nby + nb)
            return NAV_NEWTAB;
    }
    return NAV_NONE;
}

// Tab-strip hit test: which tab (or its close box) is under (mx,my)?
int okai_tab_hit(int id, int mx, int my, int* on_close) {
    if (on_close) *on_close = 0;
    if (id < 0 || id >= okai_count) return -1;
    struct okai* b = &okais[id];
    if (b->win_id < 0) return -1;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible) return -1;
    int cx0 = w->x + WIN_BORDER;
    int cy0 = w->y + WIN_BORDER + (w->no_titlebar ? 0 : WIN_TITLE_H);
    if (my < cy0 + 2 || my >= cy0 + CHROME_TAB_H) return -1;
    int n = b->tab_count; if (n < 1) return -1;
    int cwp = w->w - 2 * WIN_BORDER;
    int tw = (cwp - 40) / n; if (tw > 300) tw = 300; if (tw < 60) tw = 60;
    for (int ti = 0; ti < n; ti++) {
        int tx = cx0 + 4 + ti * (tw + 2);
        if (mx >= tx && mx < tx + tw) {
            // close × box: white outline at the tab's right edge (every tab)
            int xx = tx + tw - OKAI_XBOX - 2, xy = cy0 + (CHROME_TAB_H - OKAI_XBOX) / 2;
            if (on_close && mx >= xx && mx <= xx + OKAI_XBOX && my >= xy && my <= xy + OKAI_XBOX)
                *on_close = 1;
            return ti;
        }
    }
    return -1;
}

static void okai_goto_history(int id, int pos) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (pos < 0 || pos >= T->history_count) return;
    int pi = 0;
    while (T->history[pos][pi] && pi < OKAI_URL_LEN - 1) {
        T->url[pi] = T->history[pos][pi]; pi++;
    }
    T->url[pi] = 0;
    T->history_pos = pos;
    okai_navigate(id, T->url);
}

void okai_nav_back(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    if (T->history_pos > 0) okai_goto_history(id, T->history_pos - 1);
}

void okai_nav_fwd(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    if (T->history_pos + 1 < T->history_count) okai_goto_history(id, T->history_pos + 1);
}

void okai_nav_reload(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    okai_navigate(id, T->url); // re-request the current URL
}

void okai_nav_home(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    okai_navigate(id, OKAI_HOME_URL); // a real Home: pushes history
}

// Detect an HTTP 3xx response with a Location header and follow it by re-issuing
// the request against the resolved target. Handles absolute/relative/protocol-
// relative targets and http<->https switches. Returns 1 if a redirect fired.
int okai_check_redirect(int id, const char* resp, int len) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0 || !resp || len <= 0) return 0;

    // Must be an HTTP response...
    if (len < 12) return 0;
    if (!(resp[0]=='H'&&resp[1]=='T'&&resp[2]=='T'&&resp[3]=='P'&&resp[4]=='/')) return 0;
    // ...with a 3xx status code.
    int sp = 0; while (sp < len && resp[sp] != ' ') sp++;
    if (sp >= len) return 0;
    int code = sp + 1;
    if (code + 1 >= len || resp[code] != '3') return 0;

    // Locate the "Location:" header (case-insensitive) within the headers.
    int header_end = len;
    for (int p = 0; p < len - 3; p++) {
        if (resp[p]=='\r' && resp[p+1]=='\n' && resp[p+2]=='\r' && resp[p+3]=='\n') { header_end = p; break; }
        if (resp[p]=='\n' && resp[p+1]=='\n') { header_end = p; break; }
    }
    int loc = -1;
    for (int p = 0; p < header_end - 8; p++) {
        int ok = 1;
        const char* w = "location";
        for (int k = 0; k < 8; k++) {
            char c = resp[p+k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != w[k]) { ok = 0; break; }
        }
        if (ok && resp[p+8] == ':') { loc = p + 9; break; }
    }
    if (loc < 0) return 0;
    while (loc < len && (resp[loc]==' ' || resp[loc]=='\t')) loc++;
    int end = loc;
    while (end < len && resp[end] != '\r' && resp[end] != '\n') end++;
    if (end <= loc) return 0;

    char target[256]; int ti = 0;
    for (int p = loc; p < end && ti < 255; p++) target[ti++] = resp[p];
    target[ti] = 0;

    if (T->redirect_count >= OKAI_MAX_REDIRECTS) return 0;
    T->redirect_count++;

    // Resolve the target against the current page URL, adopt it, re-request.
    char abs[OKAI_URL_LEN];
    okai_resolve_href(b, target, abs, OKAI_URL_LEN);
    if (abs[0] == 0) return 0;

    // Default the redirect target to HTTPS too (home/internal left alone).
    char norm[OKAI_URL_LEN];
    okai_normalize_https(abs, norm, OKAI_URL_LEN);

    int ui = 0;
    while (norm[ui] && ui < OKAI_URL_LEN - 1) { T->url[ui] = norm[ui]; ui++; }
    T->url[ui] = 0;
    if (T->history_count < OKAI_MAX_HISTORY) {
        for (int k = 0; k <= ui; k++) T->history[T->history_count][k] = T->url[k];
        T->history_count++;
        T->history_pos = T->history_count - 1;
    }
    T->scroll_y = 0;
    T->token_count = 0;
    T->title[0] = 0;

    char host[128], path[128];
    int url_port = 0;
    parse_url(T->url, host, path, &url_port);
    T->is_https = (abs[0]=='h'&&abs[1]=='t'&&abs[2]=='t'&&abs[3]=='p'&&
                   abs[4]=='s'&&abs[5]==':');
    if (T->is_https) https_get_port(host, path, (uint16_t)url_port);
    else { http_reset_conn_attempts(); http_get_port(host, path, (uint16_t)url_port); }

    serial_printf("[okai] redirect %d -> %s\n", T->redirect_count, abs);
    return 1;
}

// Clamp scroll_y against the true document height (content_height) and the
// visible slice height. content_height is measured by the layout pass over
// the WHOLE page, so it no longer shrinks as you scroll (the old bug that
// stalled scrolling after a few lines).
static void okai_scroll_clamp(struct okai* b, struct window* w) {
    struct okai_tab* T = okai_tab_of(b);
    int view_h = w->content_h - CHROME_ROWS;
    if (view_h < 1) view_h = 1;
    int max_scroll = T->content_height - view_h;
    if (max_scroll < 0) max_scroll = 0;
    if (T->scroll_y > max_scroll) T->scroll_y = max_scroll;
    if (T->scroll_y < 0) T->scroll_y = 0;
}

// Submit a form: gather all <input> values of the submitting control's form
// (form_idx), URL-encode them as a GET query, and navigate to
// <form action>?<query>. Used by Enter-in-field and button clicks.
void okai_submit_form(int id, int input_idx) {
    // input_idx is a DOM node index (clicked field run or focused input).
    // Walk up to the enclosing <form>, gather its named controls' values
    // from the DOM (user edits included — they live in value attributes),
    // and navigate to action?query. <button type="button"> never submits.
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    if (input_idx < 0 || input_idx >= T->dom.node_count) return;
    if (dom_tag_is(&T->dom, input_idx, "button")) {
        char btype[16] = {0};
        dom_attr_get(&T->dom, input_idx, "type", btype, sizeof(btype));
        if (btype[0] == 'b' && btype[1] == 'u') return; // type=button
    } else if (!dom_tag_is(&T->dom, input_idx, "input")) {
        return;
    } else {
        char itype[16] = {0};
        dom_attr_get(&T->dom, input_idx, "type", itype, sizeof(itype));
        if ((itype[0] == 'b' && itype[1] == 'u') || // button
            (itype[0] == 'c' && itype[1] == 'h') || // checkbox (v1: no toggle UI)
            (itype[0] == 'r' && itype[1] == 'a') || // radio
            (itype[0] == 'h' && itype[1] == 'i'))   // hidden
            return;
    }
    // Enclosing form (or none: submit bare inputs against the page URL).
    int form = -1;
    for (int p = T->dom.nodes[input_idx].parent;
         p >= 0 && p < T->dom.node_count;
         p = T->dom.nodes[p].parent) {
        if (dom_tag_is(&T->dom, p, "form")) { form = p; break; }
    }
    char query[1024]; int ql = 0; query[0] = 0;
    static const char hex[] = "0123456789ABCDEF";
    // Gather named, submittable controls: the form's subtree, or just the
    // one control when there is no form. Unchecked boxes/radios skip.
    int self_only = (form < 0);
    for (int pass = 0; pass < 2 && ql < 1023; pass++) {
        // pass 0: the clicked control first (matches legacy order-ish);
        // pass 1: the rest of the form in DOM order.
        for (int n = (pass == 0 ? input_idx : 0);
             n < T->dom.node_count && ql < 1023;
             n = (pass == 0 ? T->dom.node_count : n + 1)) {
            if (pass == 1 && n == input_idx) continue;
            if (!dom_tag_is(&T->dom, n, "input") &&
                !dom_tag_is(&T->dom, n, "button"))
                continue;
            if (!self_only) {
                // Must be inside the same form.
                int inf = 0;
                for (int p = n; p >= 0 && p < T->dom.node_count;
                     p = T->dom.nodes[p].parent) {
                    if (p == form) { inf = 1; break; }
                }
                if (!inf) continue;
            } else if (n != input_idx) {
                continue;
            }
            char nm[64] = {0}, vv[160] = {0}, tp[16] = {0};
            int is_btn = dom_tag_is(&T->dom, n, "button");
            dom_attr_get(&T->dom, n, "name", nm, sizeof(nm));
            if (nm[0] == 0) continue; // unnamed control: skip
            dom_attr_get(&T->dom, n, "type", tp, sizeof(tp));
            if (is_btn) {
                if (!(tp[0] == 's' && tp[1] == 'u')) continue; // only submit
            } else if ((tp[0] == 'b' && tp[1] == 'u') ||
                       (tp[0] == 'h' && tp[1] == 'i') ||
                       (tp[0] == 'c' && tp[1] == 'h') ||
                       (tp[0] == 'r' && tp[1] == 'a')) {
                continue;
            }
            dom_attr_get(&T->dom, n, "value", vv, sizeof(vv));
            if (ql > 0 && ql < 1023) query[ql++] = '&';
            int ni = 0; while (nm[ni] && ql < 1023) query[ql++] = nm[ni++];
            if (ql < 1023) query[ql++] = '=';
            for (int vi = 0; vv[vi] && ql < 1023; vi++) {
                unsigned char ch = (unsigned char)vv[vi];
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                    ch == '.' || ch == '~') {
                    query[ql++] = (char)ch;
                } else if (ch == ' ') {
                    query[ql++] = '+';
                } else {
                    query[ql++] = '%';
                    query[ql++] = hex[ch >> 4];
                    query[ql++] = hex[ch & 0xF];
                }
            }
        }
    }
    query[ql] = 0;
    char action[OKAI_URL_LEN]; action[0] = 0;
    if (form >= 0) dom_attr_get(&T->dom, form, "action", action, sizeof(action));
    char url[OKAI_URL_LEN]; int ul = 0;
    if (action[0]) {
        okai_resolve_href(b, action, url, OKAI_URL_LEN);
    } else {
        int ui = 0;
        while (T->url[ui] && ui < OKAI_URL_LEN - 1) { url[ul++] = T->url[ui++]; }
        url[ul] = 0;
    }
    {
        int ul2 = 0; while (url[ul2]) ul2++;
        ul = ul2;
        if (ql > 0 && ul < OKAI_URL_LEN - 1) url[ul++] = '?';
        int qi = 0; while (query[qi] && ul < OKAI_URL_LEN - 1) url[ul++] = query[qi++];
        url[ul] = 0;
    }
    T->focused_input = -1;
    serial_printf("[okai] submit form -> %s\n", url);
    okai_navigate(id, url);
}

// Content hit-test for form controls (inputs/buttons). Returns 1 and performs
// the action (focus a field / submit a button) if a control was clicked, else 0.
// `row`/`col` are buffer-space coords already transformed by the caller
// (desktop.c uses the same transform for links, so fields line up exactly).
int okai_check_content_click(int id, int row, int col) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return 0;
    if (row < CHROME_ROWS) return 0; // chrome, not content
    for (int li = 0; li < T->field_count; li++) {
        struct okai_field* f = &T->fields[li];
        if (f->row < 0) continue;
        if (row == f->row && col >= f->col0 && col <= f->col1) {
            if (f->is_button) {
                okai_submit_form(id, f->node);
            } else {
                T->focused_input = f->node;
                okai_render_content(id);
                serial_printf("[okai] focus input node=%d\n", f->node);
            }
            return 1;
        }
    }
    return 0;
}

void okai_handle_key(int id, char c) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    b->show_security = 0; // any keypress dismisses the security popup
    b->chrome_dirty = 1;  // addr text / focus / popup may have changed

    struct window* w = window_get(b->win_id);
    if (!w) return;

    if (b->addr_bar_focused) {
        // Address bar input mode
        if (c == '\n') {
            // Navigate
            b->addr_bar_focused = 0;
            if (b->addr_input_len > 0) {
                okai_navigate(id, b->addr_input);
            }
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
        } else if (c == '\b') {
            if (b->addr_input_len > 0) {
                b->addr_input_len--;
                b->addr_input[b->addr_input_len] = 0;
            }
        } else if (c == 27) { // Escape
            b->addr_bar_focused = 0;
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
        } else if (c >= 32 && c < 127 && b->addr_input_len < OKAI_URL_LEN - 1) {
            b->addr_input[b->addr_input_len++] = c;
            b->addr_input[b->addr_input_len] = 0;
        }
        // No re-render: the address text is drawn by the pixel overlay
        // (okai_draw_chrome reads addr_input live), which repaints on its own
        // throttle. A full layout per keystroke is what made URL typing crawl
        // on large pages — the DOM/CSS cannot change from chrome input.
    } else if (T->focused_input >= 0 && T->focused_input < T->dom.node_count &&
               dom_tag_is(&T->dom, T->focused_input, "input")) {
        // Form field input mode: keys edit the focused control's value
        // attribute (in the DOM, so re-render displays it and submit reads
        // it) instead of scrolling; Enter submits the form, Esc blurs.
        // focused_input is a DOM node index (set by clicking a field run).
        int fn = T->focused_input;
        // Non-textual controls aren't editable: submit/image submit on
        // Enter, everything else ignores keystrokes here. Typeless inputs
        // default to text (editable); hidden/checkbox/radio are skipped.
        char ftype[16] = {0};
        dom_attr_get(&T->dom, fn, "type", ftype, sizeof(ftype));
        int is_btn = dom_tag_is(&T->dom, fn, "button") ||
            (ftype[0] == 's' && ftype[1] == 'u') || // submit
            (ftype[0] == 'i' && ftype[1] == 'm');   // image
        int editable = !is_btn &&
            !(ftype[0] == 'h' && ftype[1] == 'i') && // hidden
            !(ftype[0] == 'b' && ftype[1] == 'u') && // button
            !(ftype[0] == 'c' && ftype[1] == 'h') && // checkbox
            !(ftype[0] == 'r' && ftype[1] == 'a');   // radio
        if (c == '\n') {
            okai_submit_form(id, fn);
            return;
        }
        if (!editable) {
            if (c == 27) { T->focused_input = -1; okai_render_content(id); }
            return;
        }
        if (c == '\b') {
            char cur[160];
            if (dom_attr_get(&T->dom, fn, "value", cur, sizeof(cur)) > 0) {
                int vl = 0; while (cur[vl]) vl++;
                if (vl > 0) {
                    cur[--vl] = 0;
                    dom_attr_set(&T->dom, fn, "value", cur);
                }
            }
            okai_render_content(id);
        } else if (c == 27) { // Escape blurs the field
            T->focused_input = -1;
            okai_render_content(id);
        } else if (c >= 32 && c < 127) {
            char cur[160];
            int gl = dom_attr_get(&T->dom, fn, "value", cur, sizeof(cur));
            if (gl < 0) { cur[0] = 0; gl = 0; }
            int vl = 0; while (cur[vl]) vl++;
            if (vl < 159) {
                cur[vl++] = c; cur[vl] = 0;
                dom_attr_set(&T->dom, fn, "value", cur);
            }
            okai_render_content(id);
        }
        // other keys ignored while typing in a field
    } else {
        // Content scroll mode
        if (c == 'g' || c == 'G') {
            // Go to address bar
            b->addr_bar_focused = 1;
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
            okai_render_content(id);
        } else if (c == '\t') {
            // Tab: cycle keyboard focus through form fields (mouseless
            // form access). Buttons focus like inputs; Enter activates
            // whatever is focused. Esc blurs.
            if (T->field_count > 0) {
                int at = -1;
                for (int k = 0; k < T->field_count; k++)
                    if (T->fields[k].node == T->focused_input) { at = k; break; }
                int nx = (at + 1) % T->field_count;
                T->focused_input = T->fields[nx].node;
                serial_printf("[okai] tab focus field=%d node=%d\n",
                              nx, T->focused_input);
                // Scroll the field into view when needed (field rows are
                // buffer rows at the current scroll offset).
                int fr = T->fields[nx].row;
                int vh = w->content_h - CHROME_ROWS;
                if (vh < 1) vh = 1;
                if (fr < CHROME_ROWS || fr >= CHROME_ROWS + vh) {
                    T->scroll_y += fr - CHROME_ROWS;
                    okai_scroll_clamp(b, w);
                    okai_blit_content(id);
                }
                okai_render_content(id);
            }
        } else if (c == 'j' || c == '\n') {
            // Scroll down one line
            int old = T->scroll_y;
            T->scroll_y++;
            okai_scroll_clamp(b, w);
            if (T->scroll_y != old) okai_blit_content(id);
        } else if (c == 'k') {
            // Scroll up one line
            int old = T->scroll_y;
            T->scroll_y--;
            okai_scroll_clamp(b, w);
            if (T->scroll_y != old) okai_blit_content(id);
        } else if (c == 'l') {
            // Scroll right (no-op for now)
        } else if (c == 'h') {
            // Scroll left (no-op for now)
        } else if (c == 'r') {
            // Refresh
            okai_navigate(id, T->url);
        } else if (c == 'b') {
            // Back
            if (T->history_pos > 0) {
                T->history_pos--;
                int pi = 0;
                while (T->history[T->history_pos][pi] && pi < OKAI_URL_LEN - 1) {
                    T->url[pi] = T->history[T->history_pos][pi];
                    pi++;
                }
                T->url[pi] = 0;
                okai_navigate(id, T->url);
            }
        }
    }
}

void okai_handle_mouse_scroll(int id, int dy) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;

    int old = T->scroll_y;
    serial_printf("[okai] wheel dy=%d old_scroll=%d\n", dy, T->scroll_y);
    T->scroll_y += dy * 3; // dy positive = wheel down; 3 lines per detent
    okai_scroll_clamp(b, w);
    if (T->scroll_y != old) okai_blit_content(id);
}

// ---- Pixel chrome (Option B): tab strip + toolbar drawn with graphics
// primitives over the window's content area. The content buffer's top
// CHROME_ROWS rows are left blank and overdrawn by this. ----

// 12-point unit circle for icon arcs (no trig in-kernel), scaled by r/8.
static const int OKAI_CIRC_X[12] = {8,7,4,0,-4,-7,-8,-7,-4,0,4,7};
static const int OKAI_CIRC_Y[12] = {0,4,7,8,7,4,0,-4,-7,-8,-7,-4};

static void okai_poly_ring(int cx, int cy, int r, int start, int count, uint32_t fg) {
    int px = cx + OKAI_CIRC_X[start] * r / 8;
    int py = cy + OKAI_CIRC_Y[start] * r / 8;
    for (int k = 1; k <= count; k++) {
        int idx = (start + k) % 12;
        int nx = cx + OKAI_CIRC_X[idx] * r / 8;
        int ny = cy + OKAI_CIRC_Y[idx] * r / 8;
        line(px, py, nx, ny, fg);
        px = nx; py = ny;
    }
}

// Navigation icons drawn as bold, filled glyphs so they read clearly on the
// toolbar gradient (white on the dark nav chips).
static void okai_fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    int miny = y0; if (y1 < miny) miny = y1; if (y2 < miny) miny = y2;
    int maxy = y0; if (y1 > maxy) maxy = y1; if (y2 > maxy) maxy = y2;
    for (int y = miny; y <= maxy; y++) {
        int xs[4]; int n = 0;
        int ex[3] = {x0, x1, x2}; int ey[3] = {y0, y1, y2};
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            int ya = ey[i], yb = ey[j], xa = ex[i], xb = ex[j];
            if ((y >= ya && y < yb) || (y >= yb && y < ya)) {
                int xx = xa + (y - ya) * (xb - xa) / (yb - ya);
                if (n < 4) xs[n++] = xx;
            }
        }
        if (n >= 2) {
            int a = xs[0], b = xs[1];
            if (a > b) { int t = a; a = b; b = t; }
            hline(a, y, b - a + 1, c);
        }
    }
}

static void okai_icon_back(int x, int y, int s, uint32_t fg) {
    int cy = y + s / 2;
    okai_fill_tri(x + 5, cy, x + 19, y + 5, x + 19, y + s - 5, fg); // left-pointing head
    rect_fill(x + 19, cy - 3, 6, 6, fg);                            // stem
}

static void okai_icon_fwd(int x, int y, int s, uint32_t fg) {
    int cy = y + s / 2;
    okai_fill_tri(x + s - 5, cy, x + s - 19, y + 5, x + s - 19, y + s - 5, fg); // right-pointing head
    rect_fill(x + s - 25, cy - 3, 6, 6, fg);                            // stem
}

static void okai_icon_reload(int x, int y, int s, uint32_t fg) {
    int cx = x + s / 2, cy = y + s / 2, r = s / 2 - 3;
    okai_poly_ring(cx, cy, r,     1, 9, fg);   // open ring (gap at top)
    okai_poly_ring(cx, cy, r - 1, 1, 9, fg);   // 2px-thick ring
    okai_poly_ring(cx, cy, r - 2, 1, 9, fg);   // 3px-thick ring
    // bold arrowhead at the ring's open end (top, index 10)
    int hx = cx + OKAI_CIRC_X[10] * r / 8, hy = cy + OKAI_CIRC_Y[10] * r / 8;
    okai_fill_tri(hx - 4, hy + 5, hx + 4, hy + 5, hx, hy - 4, fg);
}

static void okai_icon_home(int x, int y, int s, uint32_t fg) {
    int mid = x + s / 2;
    int half = (s - 8) / 2;
    okai_fill_tri(mid, y + 4, x + 4, y + 5 + half, x + s - 4, y + 5 + half, fg); // roof
    int body_y = y + 5 + half;
    int body_h = (y + s - 4) - body_y;
    if (body_h < 4) return;
    rect_outline(mid - half, body_y, 2 * half + 1, body_h, fg, 1);   // walls
    if (body_h >= 7) rect_fill(mid - 1, body_y + body_h - 5, 3, 5, fg); // door
}

static void okai_icon_lock(int x, int y, int s, uint32_t fg) {
    rect_fill(x + 2, y + s / 2, s - 4, s / 2 - 1, fg);   // body
    okai_poly_ring(x + s / 2, y + s / 2, s / 2 - 3, 8, 4, fg); // shackle (top arc)
}

// Click hit-test for the address-bar lock icon. Geometry mirrors the draw in
// okai_draw_chrome: 4 nav buttons (30px + 8px gap) from cx0+8, then the address
// bar starts 10px later; the lock sits at addr_x+6, 12px square, in the toolbar.
int okai_lock_hit(int id, int mx, int my) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return 0;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return 0;
    int topoff = w->no_titlebar ? 0 : WIN_TITLE_H;
    int cy0 = w->y + WIN_BORDER + topoff;
    int cx0 = w->x + WIN_BORDER;
    int tool_y = cy0 + CHROME_TAB_H;
    int addr_x = cx0 + 8 + 4 * (30 + 8) + 10;
    // Forgiving hit area centered on the 12px lock icon (20x20px) so it is
    // easy to click even when the mouse settles a few px off.
    int lx0 = addr_x + 2, lx1 = addr_x + 22;
    int ly0 = tool_y + 8, ly1 = tool_y + 28;
    return (mx >= lx0 && mx < lx1 && my >= ly0 && my < ly1);
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
    int n = b->tab_count; if (n < 1) n = 1;
    int cwp = w->w - 2 * WIN_BORDER;
    int tw = (cwp - 40) / n; if (tw > 300) tw = 300; if (tw < 60) tw = 60;
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
    int n = b->tab_count; if (n < 1) n = 1;
    int cwp = w->w - 2 * WIN_BORDER;
    int tw = (cwp - 40) / n; if (tw > 300) tw = 300; if (tw < 60) tw = 60;

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
            for (int j = i; j < b->tab_count - 1; j++) b->tabs[j] = b->tabs[j + 1];
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

static void okai_draw_fetch_status(int id); // defined below (progress pill)
void okai_draw_chrome(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return;

    int cx0 = w->x + WIN_BORDER;
    int topoff = w->no_titlebar ? 0 : WIN_TITLE_H;
    int cy0 = w->y + WIN_BORDER + topoff;
    int cwp = w->w - 2 * WIN_BORDER;
    if (cwp <= 0) return;
    int tool_y = cy0 + CHROME_TAB_H;

    // Tab strip (gradient) with one tab per open page -------------------------
    gradient_fill(cx0, cy0, cwp, CHROME_TAB_H, CHROME_TAB_TOP, CHROME_TAB_BOT, 1);
    {
        int n = b->tab_count; if (n < 1) n = 1;
        int x = cx0 + 4;
        for (int ti = 0; ti < n; ti++) {
            int tw = b->tabs[ti].anim_w;
            if (tw < 2) { x += 2; continue; } // fully-collapsed (closing) tab
            int tx = x;
            int active = (ti == b->active_tab);
            uint32_t fill = active ? CHROME_TAB_ACTIVE : 0x004A648F;
            rect_fill(tx, cy0 + 2, tw, CHROME_TAB_H - 2, fill);
            if (active) { // round the active tab's top corners
                int r = 5;
                for (int i = 0; i < r && i < tw; i++) {
                    int cl = r - i;
                    rect_fill(tx, cy0 + 2 + i, cl, 1, CHROME_TAB_TOP);
                    rect_fill(tx + tw - cl, cy0 + 2 + i, cl, 1, CHROME_TAB_TOP);
                }
            }
            struct okai_tab* TT = &b->tabs[ti];
            const char* t = TT->title[0] ? TT->title : TT->url;
            int max_ch = (tw - (OKAI_XBOX + 8)) / FONT_W; // reserve room for × box
            if (max_ch > 0)
                for (int i = 0; t[i] && i < max_ch; i++)
                    draw_char_1x(tx + 8 + i * FONT_W, cy0 + (CHROME_TAB_H - FONT_H) / 2, t[i],
                                 active ? CHROME_TAB_TEXT : CHROME_TAB_INACT, fill);
            // Close × in a white square outline — present on every tab. The
            // box is filled (like the + button) so the white outline stays
            // visible even on the near-white active tab.
            if (tw >= OKAI_XBOX + 8) {
                int xx = tx + tw - OKAI_XBOX - 2, xy = cy0 + (CHROME_TAB_H - OKAI_XBOX) / 2;
                rect_fill(xx, xy, OKAI_XBOX, OKAI_XBOX, CHROME_TAB_TOP);
                rect_outline(xx, xy, OKAI_XBOX, OKAI_XBOX, 0x00FFFFFF, 1);
                line(xx + 3, xy + 3, xx + OKAI_XBOX - 3, xy + OKAI_XBOX - 3, 0x00FFFFFF);
                line(xx + OKAI_XBOX - 3, xy + 3, xx + 3, xy + OKAI_XBOX - 3, 0x00FFFFFF);
            }
            x += tw + 2;
        }
        // New-tab "+" button right after the last tab
        int nb = OKAI_NB;
        int nbx = x, nby = cy0 + (CHROME_TAB_H - nb) / 2;
        rect_fill(nbx, nby, nb, nb, CHROME_TAB_TOP);   // uniform fill (no dark gradient patch)
        rect_outline(nbx, nby, nb, nb, 0x00FFFFFF, 1);  // white outline
        draw_string_1x(nbx + (nb - FONT_W) / 2, nby, "+", 0x00FFFFFF, CHROME_TAB_TOP);
    }

    // Toolbar (gradient) ----------------------------------------------------
    gradient_fill(cx0, tool_y, cwp, CHROME_TOOL_H, CHROME_TOOL_TOP, CHROME_TOOL_BOT, 1);
    hline(cx0, tool_y - 1, cwp, 0x001A2E4D); // crisp 2px divider between tab strip and toolbar
    hline(cx0, tool_y, cwp, 0x001A2E4D);

    // Nav buttons: rounded dark chips with the glyph centered inside
    int btn = 30, gap = 8;
    int by = tool_y + (CHROME_TOOL_H - btn) / 2;
    int bx = cx0 + 8;
    for (int i = 0; i < 4; i++) {
        round_rect_fill(bx, by, btn, btn, NAVBTN_BG, NAVBTN_R);
        hline(bx + NAVBTN_R, by, btn - 2 * NAVBTN_R, NAVBTN_HI);
        bx += btn + gap;
    }
    bx = cx0 + 8;
    okai_icon_back(bx, by, btn, CHROME_BTN_FG);   bx += btn + gap;
    okai_icon_fwd(bx, by, btn, CHROME_BTN_FG);    bx += btn + gap;
    okai_icon_reload(bx, by, btn, CHROME_BTN_FG); bx += btn + gap;
    okai_icon_home(bx, by, btn, CHROME_BTN_FG);   bx += btn + gap;

    // Address bar -----------------------------------------------------------
    int addr_x = bx + 10;
    // Reserve room for the top-right close control so the bar never overlaps it.
    int ctrl_space = w->no_titlebar ? (WIN_CTRL_BTN + 10) : 8;
    int addr_w = cwp - (addr_x - cx0) - ctrl_space;
    if (addr_w > 24) {
        int ay = tool_y + 4, ah = CHROME_TOOL_H - 8;
        rect_fill(addr_x, ay, addr_w, ah, ADDR_BG);
        rect_outline(addr_x, ay, addr_w, ah, ADDR_BORDER, 1);
        okai_icon_lock(addr_x + 6, ay + (ah - 12) / 2, 12,
                       T->is_https ? LOCK_OK : 0x006A6A6A);
        const char* u = b->addr_bar_focused ? b->addr_input : T->url;
        int max_ch = (addr_w - 24 - 8) / FONT_W; // stay inside the bar
        for (int i = 0; u[i] && i < max_ch; i++)
            draw_char_1x(addr_x + 24 + i * FONT_W, ay + (ah - FONT_H) / 2,
                         u[i], ADDR_TEXT, ADDR_BG);
    }

    // HTTPS lock popup: a security card anchored under the address bar. Toggled
    // by clicking the lock icon (okai_lock_hit); dismissed on any click elsewhere
    // or keypress (okai_handle_key clears show_security).
    if (b->show_security) {
        int px = addr_x;
        int py = tool_y + CHROME_TOOL_H + 3;
        int pw = 300, ph = 150;
        round_rect_fill(px, py, pw, ph, 0x00FFFFFF, 6);
        rect_outline(px, py, pw, ph, ADDR_BORDER, 1);
        int card_bg = 0x00FFFFFF;
        const char* head = T->is_https ? "Connection is secure" : "Not secure";
        uint32_t hcol = T->is_https ? LOCK_OK : 0x002020C0;
        draw_string_1x(px + 14, py + 14, head, hcol, card_bg);
        if (T->is_https) {
            draw_string_1x(px + 14, py + 48, "Protocol: TLS 1.3", 0x00202020, card_bg);
            char host[OKAI_URL_LEN]; int hi = 0; const char* hp = T->url;
            if (hp[0]=='h'&&hp[1]=='t'&&hp[2]=='t'&&hp[3]=='p'&&hp[4]=='s'&&hp[5]==':') hp += 8;
            else if (hp[0]=='h'&&hp[1]=='t'&&hp[2]=='t'&&hp[3]=='p'&&hp[4]==':') hp += 7;
            while (*hp && *hp!='/' && *hp!=':' && hi < OKAI_URL_LEN-1) host[hi++] = *hp++;
            host[hi] = 0;
            char line[64]; int li = 0; const char* h2 = "Host: ";
            while (*h2 && li < 60) line[li++] = *h2++;
            for (int k = 0; host[k] && li < 60; k++) line[li++] = host[k];
            line[li] = 0;
            draw_string_1x(px + 14, py + 82, line, 0x00202020, card_bg);
            draw_string_1x(px + 14, py + 116, "Certificate: valid", 0x00202020, card_bg);
        } else {
            draw_string_1x(px + 14, py + 48, "This page is not encrypted.", 0x00202020, card_bg);
            draw_string_1x(px + 14, py + 82, "Info you send may be visible.", 0x00202020, card_bg);
        }
    }

    // Close control at the top-right (replaces the title-bar × in title-less
    // mode). Its hit area is defined by window_check_close_click().
    if (w->no_titlebar) {
        int cs = WIN_CTRL_BTN;
        int cbx = w->x + w->w - WIN_BORDER - 4 - cs;
        int cby = w->y + WIN_BORDER;
        uint32_t xc = 0x00C8C8C8; // light gray ×
        int m = 5, e = cs - 5;
        line(cbx + m, cby + m, cbx + e, cby + e, xc);
        line(cbx + e, cby + m, cbx + m, cby + e, xc);
    }
    okai_draw_fetch_status(id);
}

// Fetch progress pill, bottom-left of the content area ("Loading 123K" in
// small text). Pixel overlay like the chrome: painted every frame while a
// fetch is in flight for this window. When the fetch ends, desktop.c fires
// one content blit (see the erase hook) to wipe its pixels.
static void okai_draw_fetch_status(int id) {
    if (okai_fetch_owner != id) return;
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    long rxb = -1;
    if (T->is_https) {
        if (tls_is_active() && !tls_is_done())
            rxb = tls_get_progress_len();
    } else if (!http_is_done() &&
               (http_is_pending() || http_is_retry_pending())) {
        rxb = http_get_response_len();
    }
    if (rxb < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return;
    char msg[20];
    int mi = 0;
    const char* pre = "Loading ";
    while (pre[mi] && mi < 18) { msg[mi] = pre[mi]; mi++; }
    long kb = rxb / 1024;
    char num[12]; int nl = 0; long v = kb;
    if (v == 0) num[nl++] = '0';
    else {
        char rev[12]; int rl = 0;
        while (v > 0 && rl < 11) { rev[rl++] = (char)('0' + v % 10); v /= 10; }
        while (rl > 0) num[nl++] = rev[--rl];
    }
    for (int k = 0; k < nl && mi < 18; k++) msg[mi++] = num[k];
    if (mi < 18) msg[mi++] = 'K';
    msg[mi] = 0;
    int cw = 8, chh = 16; // small text: half-size glyphs
    int tw = mi * cw;
    int cell_h = CONTENT_GH * w->font_scale;
    int cx = w->x + WIN_BORDER;
    int cytop = w->y + WIN_BORDER + (w->no_titlebar ? 0 : WIN_TITLE_H);
    int cbot = cytop + w->content_h * cell_h;
    int winbot = w->y + w->h - WIN_BORDER;
    if (cbot > winbot) cbot = winbot;
    int x = cx + 4, y = cbot - chh - 4;
    if (y < cytop || tw <= 0) return;
    rect_fill(x - 4, y - 2, tw + 8, chh + 4, 0x000000);
    for (int k = 0; k < mi; k++)
        draw_char_sized(x + k * cw, y, msg[k], 0x00FFFFFF, 0x000000, cw, chh);
}

static void okai_draw_heading_pixels(int id); // defined just below

// Chrome + heading overlays for one window. desktop.c paints these right
// after the window itself (in z-order) — the old draw-after-all-windows pass
// let a LOWER okai's chrome paint over a HIGHER overlapping window ("text
// leaking through").
// The security popup card (okai_draw_chrome) must sit ON TOP of page content,
// so the heading pixels are painted first and the chrome/card last.
void okai_paint_overlays(int id) {
    // This *is* the live render path (desktop.c calls it every main-loop
    // iteration for each visible okai window). Stepping the animation here
    // grows/shrinks each tab's anim_w; the chrome drawn below is flushed every
    // frame by graphics_flush(), so no full window re-render is needed during
    // the animation (the page body only re-renders when its content changes).
    okai_anim_step(id);
    okai_draw_heading_pixels(id);
    okai_draw_chrome(id);
}

// Draw okai's chrome/heading overlay clipped to the given sub-rects (okai's
// window minus any higher-z overlapping windows). This keeps the overlay from
// painting over a covering window, so that window does NOT need to be force-
// repainted every frame — which was tanking FPS when a window sat over okai.
// rects[i] = {x, y, w, h} in screen coords; nr may be 0 (fully covered).
void okai_paint_overlays_rects(int id, int rects[][4], int nr) {
    okai_anim_step(id);
    for (int i = 0; i < nr; i++) {
        graphics_set_clip(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        okai_draw_heading_pixels(id);
        okai_draw_chrome(id);
    }
    graphics_clip_reset();
}

void okai_draw_chrome_all(void) {
    for (int i = 0; i < MAX_OKAIS; i++) {
        if (okais[i].win_id >= 0) {
            okai_draw_chrome(i);
            okai_draw_heading_pixels(i);
        }
    }
}

// Paint heading lines (flagged in the layout pass) as raw scaled glyphs over
// the window's content region. The window char grid is one uniform size, so
// headings can't be bigger there; this runs after window_draw_all() and draws
// them directly into the backbuffer (kind 1 = 2x, kind 2 = 3x body size),
// matching the body text color.
static void okai_draw_heading_pixels(int id) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w || !w->visible || w->minimized) return;

    int cell_w = CONTENT_GW * w->font_scale;
    int cell_h = CONTENT_GH * w->font_scale;
    int cx = w->x + WIN_BORDER;
    int cy = w->y + WIN_BORDER + (w->no_titlebar ? 0 : WIN_TITLE_H);
    uint32_t fg = g_page_fg;
    uint32_t bg = g_page_bg;

    const struct layout* lay = &g_lay[id];
    int view_h = w->content_h - CHROME_ROWS;
    if (view_h < 0) view_h = 0;
    for (int li = 0; li < lay->n_items; li++) {
        const struct layout_item* it = &lay->items[li];
        if (it->kind != LOUT_LINE || !it->heading) continue;
        // H1 = 3x, H2 = 2x, H3-H6 = body size (heading but no scaling).
        int level = it->heading;
        int hscale = level <= 1 ? 3 : level == 2 ? 2 : 1;
        if (hscale <= 1) continue; // H3+ renders in the grid normally
        int cell_row = it->is_fixed ? it->row + CHROME_ROWS
                                    : it->row - T->scroll_y + CHROME_ROWS;
        if (cell_row < CHROME_ROWS || cell_row >= w->content_h) continue;
        int py = cy + cell_row * cell_h;
        for (int ri = it->run_start; ri < it->run_start + it->run_count; ri++) {
            const struct layout_run* run = &lay->runs[ri];
            for (int k = 0; k < run->text_len; k++) {
                int c = run->col + k * hscale; // heading runs are scale-columns apart
                if (c < 0 || c >= w->content_w) continue;
                int px = cx + c * cell_w;
                if ((run->flags & LAYOUT_FLAG_CJK) != 0) {
                    // CJK heading: decode the kth char and draw the bitmap
                    // scaled like the Latin overlay (square box, vertically
                    // centered). Table miss: skip (the grid '?' shows through).
                    int boff = 0;
                    uint32_t cjk = 0;
                    for (int q = 0; q <= k; q++) {
                        int bl = 1;
                        cjk = utf8_decode_char(
                            lay->text + run->text_off + boff,
                            LAYOUT_MAX_TEXT - (int)run->text_off - boff, &bl);
                        if (bl <= 0) bl = 1;
                        boff += bl;
                    }
                    const uint8_t* g = cjk_glyph_for(cjk);
                    if (!g) continue;
                    int side = CONTENT_GW * hscale;
                    draw_cjk_box(px, py + (cell_h - side) / 2, g,
                                 run->fg, lay->page_bg, side, side);
                    continue;
                }
                char ch = lay->text[run->text_off + k];
                if (ch == ' ' || (unsigned char)ch < 33) continue;
                draw_char_cell(px, py, doc_sanitize(ch), run->fg, lay->page_bg,
                               CONTENT_GW * hscale, CONTENT_GH * hscale, CELL_BOLD);
            }
        }
    }
}

// NOTE: okai_draw() is dead — the desktop main loop paints okai via
// okai_paint_overlays() (which steps the animation). Kept for API symmetry.
void okai_draw(int id) {
    if (id < 0 || id >= MAX_OKAIS) return;
    okai_draw_chrome(id);
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

// ---- Host preview support (tests/okai_preview.c) ---------------------------
// Exposes the laid-out virtual document so the host tool can render the page
// to a PNG with the real font/colors — a QEMU-free preview of okai output.
#ifdef HOST_PREVIEW
// The preview paints the layout display list directly (same source as the
// kernel renderer). Exposed as a borrowed pointer valid until the next
// okai_render_content_for_preview call.
static struct layout g_preview_lay;
const struct layout* preview_layout(void) { return &g_preview_lay; }
struct okai*    okai_get_for_preview(void) { return &okais[0]; }
void            okai_render_content_for_preview(void) {
    struct okai* b = &okais[0];
    struct okai_tab* T = okai_tab_of(b);
    // Rebuild the layout exactly as okai_render_content does (minus the
    // window blit, which the preview does not have a real window for).
    const struct css_rule* ua = 0;
    int ua_n = css_ua_rules(&ua);
    struct css_style body_style;
    css_compute(T->css_rules, T->css_n, "body", 0, 0, 0, &body_style);
    if (ua_n > 0) {
        struct css_style ub;
        css_compute(ua, ua_n, "body", 0, 0, 0, &ub);
        css_merge_base(&body_style, &ub);
    }
    uint32_t page_bg = body_style.has_bg_rgb ? body_style.bg_rgb : 0xFFFFFF;
    uint32_t default_fg = body_style.has_fg_rgb ? body_style.fg_rgb
                          : (window_rgb_is_light(page_bg) ? 0x000000 : 0xFFFFFF);
    g_page_fg = default_fg; g_page_bg = page_bg;

    static struct css_rule combined[CSS_MAX_RULES + CSS_UA_MAX_RULES];
    int cn = 0;
    for (int i = 0; i < ua_n && cn < CSS_MAX_RULES + CSS_UA_MAX_RULES; i++) combined[cn++] = ua[i];
    for (int i = 0; i < T->css_n && cn < CSS_MAX_RULES + CSS_UA_MAX_RULES; i++) combined[cn++] = T->css_rules[i];

    int view_w = 80;
    struct window* w = window_get(b->win_id);
    if (w && w->content_w > 0) view_w = w->content_w;
    int page_left = OKAI_TEXT_PAD;
    int page_w = view_w - 2 * OKAI_TEXT_PAD;
    if (page_w < 20) page_w = 20;

    struct layout_opts lo;
    lo.width_cols = page_w;
    lo.page_left = page_left;
    lo.page_bg = page_bg;
    lo.page_fg = default_fg;
    layout_run(&T->dom, combined, cn, &lo, &g_preview_lay);
    T->content_height = g_preview_lay.height;
}
#endif
