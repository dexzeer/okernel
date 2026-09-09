#ifndef OKAI_H
#define OKAI_H

#include <stdint.h>
#include "html.h"
#include "css.h"

#define OKAI_MAX_HISTORY 4
#define OKAI_URL_LEN 128
#define MAX_OKAIS 2            // one browser window (+ spare); tabs live inside
#define OKAI_MAX_TABS 4        // tabs per browser window
#define OKAI_TAB_TOKENS 384    // per-tab parsed-HTML cap (memory: ~250KB/tab)
#define OKAI_CSS_TEXT 16384
#define OKAI_MAX_LINKS 64
#define OKAI_MAX_REDIRECTS 8
#define OKAI_MAX_SUBRES 8      // max external <link>/<script> per page
#define OKAI_SUBRES_BUF 4096   // buffer for extracted URLs (double-null-terminated)

// Where the Home button, the '+' new tab, and `okai` with no argument go.
// okai:home is an INTERNAL page rendered from OKAI_HOME_HTML (no network).
#define OKAI_HOME_URL "okai:home"

// A clickable link region in the window content buffer (buffer row/col space).
struct okai_link {
    int row;            // content buffer row
    int col0;           // first column (inclusive)
    int col1;           // last column (inclusive)
    char href[OKAI_URL_LEN]; // resolved, absolute URL
};

// A focusable form control (input/button) region, recorded during render for
// mouse hit-testing. token_index indexes T->tokens[]; is_button distinguishes
// submit targets from text fields (click focuses a field, activates a button).
struct okai_field {
    int row;            // content buffer row
    int col0;           // first column (inclusive)
    int col1;           // last column (inclusive)
    int token_index;    // index into T->tokens[]
    int is_button;      // 1 = submit button, 0 = text input
};

// One page: everything that a tab switch must preserve so switching back
// re-renders instantly (no refetch).
struct okai_tab {
    char url[OKAI_URL_LEN];
    char title[64];
    int scroll_y;
    int content_height; // total lines of rendered content
    struct html_token tokens[OKAI_TAB_TOKENS];
    int token_count;
    int last_resp_len;    // track HTTP response changes
    int is_https;         // 1 if URL used the https:// scheme
    int https_fell_back;  // 1 once we've already retried a failed HTTPS fetch over HTTP
    int cert_failed;      // 1 = HTTPS failed certificate verification: render a
                          // distinct security error and NEVER fall back to HTTP
    // CSS: parsed from <style> blocks of this page
    struct css_rule css_rules[CSS_MAX_RULES];
    int css_n;
    char css_text[OKAI_CSS_TEXT];
    // Clickable links recorded during render, for mouse hit-testing
    struct okai_link links[OKAI_MAX_LINKS];
    int link_count;
    // Form controls (inputs/buttons) recorded during render, for mouse focus/submit
    struct okai_field fields[OKAI_MAX_LINKS];
    int field_count;
    int focused_input;  // token index of the focused <input>, or -1
    // History
    char history[OKAI_MAX_HISTORY][OKAI_URL_LEN];
    int history_count;
    int history_pos;
    int redirect_count;    // HTTP 3xx hops followed on this page (anti-loop)
    // External resource fetch queue (<link rel=stylesheet>, <script src>)
    int sub_res_phase;     // 0=idle, 1=fetching CSS links, 2=fetching JS scripts
    int sub_res_idx;       // index into sub_res_urls for the current fetch
    int sub_res_count;     // number of URLs in sub_res_urls
    char sub_res_type[OKAI_MAX_SUBRES]; // 'c'=CSS link, 'j'=JS script
    char sub_res_urls[OKAI_MAX_SUBRES][OKAI_URL_LEN]; // resolved absolute URLs
    // Animation: current rendered tab width (px) and close-in-progress flag.
    int anim_w;            // eased toward the target width for open/close animation
    int closing;           // 1 while the tab is shrinking before removal
};

struct okai {
    int win_id;
    int active_tab;       // index into tabs[]
    int tab_count;        // live tabs (tabs[0..tab_count-1])
    struct okai_tab tabs[OKAI_MAX_TABS];
    int addr_bar_focused; // 1 = typing in address bar, 0 = scrolling
    char addr_input[OKAI_URL_LEN];
    int addr_input_len;
    int show_security;   // HTTPS lock popup open (toggled by clicking the lock icon)
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
// Begin the network fetch for window `id` from its current URL. Driven by the
// desktop response loop under the single-connection owner model (okai_fetch_owner).
// Returns 0 if a fetch was started, -1 if the URL was refused (empty host).
int okai_start_fetch(int id);
// Re-issue the current tab's fetch over plain HTTP after an HTTPS failure
// (one-time fallback so http-only hosts still load). Returns okai_start_fetch()'s
// result, or -1 if already fallen back this navigation.
int okai_fallback_http(int id);
void okai_navigate(int id, const char* url);
void okai_handle_key(int id, char c);
void okai_handle_mouse_scroll(int id, int dy);
void okai_draw(int id);
void okai_draw_chrome_all(void);
void okai_render_content(int id);
int okai_find_by_win(int win_id);
struct okai* okai_get(int id);
// Id of the window that currently owns the single in-flight okai fetch, or -1.
// The desktop response loop sets/clears this; declared here so desktop.c can
// read it without reaching into okai.c internals.
extern int okai_fetch_owner;
// Inspect an HTTP response for a 3xx + Location header; if found, resolve the
// target against the current URL and re-issue the request (http<->https aware).
// Returns 1 if a redirect was followed (caller should skip parsing this frame).
int okai_check_redirect(int id, const char* resp, int len);

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
// Content hit-test for form controls (inputs/buttons), given buffer-row/col
// already transformed by the caller (one coordinate space with links). Returns
// 1 and performs the action (focus the input / submit the form), else 0.
int  okai_check_content_click(int id, int row, int col);
// Build the form's GET query from its inputs and navigate (used on Enter / button
// click). `input_idx` is the submitting field's token index (input or button).
void okai_submit_form(int id, int input_idx);
void okai_nav_back(int id);
void okai_nav_fwd(int id);
void okai_nav_reload(int id);
void okai_nav_home(int id);
// Chrome + heading overlays for ONE window — desktop.c paints these right
// after the window itself in z-order (okai_draw_chrome_all drew them after
// ALL windows, so a lower okai's chrome painted over a higher window).
void okai_paint_overlays(int id);
// Clip the overlay to the given sub-rects (okai window minus higher-z windows)
// so it never paints over a covering window — avoids forcing that window to
// repaint every frame (FPS regression when a window sits over okai).
void okai_paint_overlays_rects(int id, int rects[][4], int nr);

// Sub-resource fetch queue (<link rel=stylesheet>, <script src>).
// After the main page is parsed, external CSS/JS are queued and fetched
// sequentially through the single TCP connection.
void okai_queue_sub_resources(int id, const char* html, int html_len);
int  okai_start_sub_res_fetch(int id);
int  okai_sub_res_done(int id, const char* resp, int resp_len);

#endif
