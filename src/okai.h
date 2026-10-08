#ifndef OKAI_H
#define OKAI_H

#include <stdint.h>

// okai — the web browser shell. Pages are parsed, styled, laid out and
// painted by the web engine in src/web/ (struct wdoc); okai owns the
// window, the chrome (tabs, toolbar, address bar), navigation/history,
// the network fetch driver and input routing. The page is painted into a
// per-window pixel surface that the window system blits (window_set_pixels).

#define OKAI_MAX_HISTORY 8
#define OKAI_URL_LEN 2048
#define MAX_OKAIS 2            // one browser window (+ spare); tabs live inside
#define OKAI_MAX_TABS 4        // tabs per browser window
#define OKAI_MAX_REDIRECTS 8
#define OKAI_MAX_CSS_FETCH 24  // external stylesheets fetched per page
#define OKAI_MAX_IMG_FETCH 32  // images fetched per page (rest stay blank)
#define OKAI_MAX_JS_FETCH  160 // script/module sources fetched per page
#define OKAI_MAX_REQ_FETCH 64  // fetch()/XHR requests per page

// Where the Home button, the '+' new tab, and `okai` with no argument go.
// okai:home is an INTERNAL page (no network).
#define OKAI_HOME_URL "okai:home"

struct wdoc;

// One page: everything that a tab switch must preserve so switching back
// re-renders instantly (no refetch).
struct okai_tab {
    char url[OKAI_URL_LEN];
    char title[64];
    struct wdoc* doc;     // parsed page (NULL until the first load)
    int scroll_y;         // px
    int content_height;   // document height, px
    int load_state;       // 0 = needs fetch, 1 = loaded, -1 = error page shown
    int is_https;         // 1 if URL used the https:// scheme
    int https_fell_back;  // 1 once we've already retried a failed HTTPS fetch over HTTP
    int conn_retries;     // resumption/full-handshake retries this navigation
                          // (max OKAI_CONN_MAX_RETRIES, same origin, not a
                          // downgrade) against flaky backends
#define OKAI_CONN_MAX_RETRIES 2
    int cert_failed;      // 1 = HTTPS failed certificate verification: render a
                          // distinct security error and NEVER fall back to HTTP
    int cert_detail;      // CV_ERR_* code for the message above (0 = generic)
    int conn_failed;      // 1 = HTTPS failed for NON-cert reasons (protocol /
                          // transport / too-large): render a connection error,
                          // NEVER a certificate warning, NEVER fall back
    int conn_kind;        // OKAI_CONN_* below (meaningful when conn_failed)
#define OKAI_CONN_PROTO 1    // handshake/protocol/transport failure
#define OKAI_CONN_TOOLARGE 2 // response exceeded the fetch buffer
    int truncated;        // 1 = secure response proven TRUNCATED (EOF without
                          // close_notify and short of Content-Length framing):
                          // render a distinct warning, NEVER fall back
    int focused_node;     // DOM node of the focused form control, or -1
    // History
    char history[OKAI_MAX_HISTORY][OKAI_URL_LEN];
    int history_count;
    int history_pos;
    int redirect_count;   // HTTP 3xx hops followed on this page (anti-loop)
    // Sub-resources (stylesheets, scripts, images, script requests) of the
    // loaded page, fetched in parallel after the main document.
    int sub_inflight;     // resource requests in flight for this tab
    int sub_css, sub_img; // fetches issued this page (caps above)
    int sub_landed;       // resources applied since the last render
    uint32_t load_tick;   // tick of the main document parse
    uint32_t last_render_tick;
    int render_pending;   // relayout+repaint requested (coalesced in okai_poll)
    char pending_frag[128]; // #fragment to scroll to after the load
    int render_due;       // tick at which a pending render fires
    int has_fixed;        // layout has position:fixed content (scroll repaints fully)
    int first_paint;      // 1 once the loaded document has been painted
    // Animation: current rendered tab width (px) and close-in-progress flag.
    int anim_w;           // eased toward the target width for open/close animation
    int closing;          // 1 while the tab is shrinking before removal
    // Page scripts
    int sub_js, sub_req;  // script / request fetches issued this page (caps above)
    uint32_t js_next_tick;// next realm pump (tick); 0 = poll wjs_next_due
    int js_scroll_evt;    // a scroll happened: dispatch `scroll` on the next pump
    uint32_t links_hash;  // hash of the last logged link/field regions
};

struct okai {
    int win_id;
    int active_tab;       // index into tabs[]
    int tab_count;        // live tabs (tabs[0..tab_count-1])
    struct okai_tab tabs[OKAI_MAX_TABS];
    int addr_bar_focused; // 1 = typing in address bar, 0 = scrolling
    char addr_input[OKAI_URL_LEN];
    int addr_input_len;
    int show_security;    // HTTPS lock popup open (toggled by clicking the lock icon)
    int chrome_dirty;     // overlay state changed without a window repaint
                          // (addr typing, security popup) — forces a repaint
    // Page surface: the visible viewport of the active tab, painted by the
    // engine and blitted by the window system below the chrome band.
    uint32_t* page_px;
    int page_w, page_h;   // surface size (px)
    int page_cap;         // allocated pixels
};

void okai_init(void);
int okai_open(const char* url);
void okai_close(int id);
// Active tab of a okai (the page being displayed / fetched).
struct okai_tab* okai_tab_of(struct okai* b);
int  okai_window_count(void); // live browser windows (one-window policy)
// Tab management: switch re-renders the cached page (no refetch); new_tab
// navigates `url` in a fresh tab; close_tab removes it (closing the last tab
// closes the window). Returns tab index / 0 / -1 on limits.
int  okai_switch_tab(int id, int tab);
int  okai_new_tab(int id, const char* url);
void okai_close_tab(int id, int tab);
// Tab-strip hit test: tab index for (mx,my), or -1. *on_close is set to 1 if
// the click landed on that tab's close box.
int  okai_tab_hit(int id, int mx, int my, int* on_close);
void okai_navigate(int id, const char* url);
void okai_handle_key(int id, char c);
void okai_handle_mouse_scroll(int id, int dy);
// Full render: relayout (if the document changed) + repaint the page surface.
void okai_render_content(int id);
// Repaint the window from the existing page surface (no engine work).
void okai_blit_content(int id);
// Non-mutating tab-animation check (okai_anim_step advances state; this only
// reads) so the desktop loop can throttle overlay repaints to when the chrome
// can actually change.
int okai_is_animating(int id);
int okai_wants_cpu(void);   // loading / render pending / animating anywhere
// Hover target / loading spinner changed since the chrome was last drawn.
int okai_ui_needs_paint(int id);
int okai_find_by_win(int win_id);
struct okai* okai_get(int id);
// A window with requests in flight (any), or -1.
extern int okai_fetch_owner;
// Drive fetches (main document, then sub-resources) and coalesced renders.
// Called once per desktop main-loop iteration.
void okai_poll(void);

// Toolbar nav actions, returned by okai_check_nav_click(). The browser's
// back/forward/reload/home buttons are drawn in okai_draw_chrome(); this is
// their mouse hit-test (same geometry) so desktop.c can route clicks to them.
#define NAV_NONE   0
#define NAV_BACK   1
#define NAV_FWD    2
#define NAV_RELOAD 3
#define NAV_HOME    4
#define NAV_NEWTAB 5  // the '+' box in the tab strip

int  okai_check_nav_click(int id, int mx, int my);
int  okai_lock_hit(int id, int mx, int my); // click hit-test for the address-bar lock icon
int  okai_addr_bar_hit(int id, int mx, int my); // click-to-focus the address bar
// Click in the page area at screen (mx, my): follows links, focuses fields,
// activates buttons/checkboxes. Returns 1 if the click did something.
int  okai_content_click(int id, int mx, int my);
void okai_nav_back(int id);
void okai_nav_fwd(int id);
void okai_nav_reload(int id);
void okai_nav_home(int id);
// Chrome overlay for ONE window — desktop.c paints it right after the
// window itself in z-order.
void okai_paint_overlays(int id);
// Clip the overlay to the given sub-rects (okai window minus higher-z windows)
// so it never paints over a covering window.
void okai_paint_overlays_rects(int id, int rects[][4], int nr);

// Page scripts on/off for newly loaded pages (the J key toggles + reloads).
extern int okai_scripts_on;

#endif
