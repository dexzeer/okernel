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
    // CSS: parsed from <style> blocks of this page
    struct css_rule css_rules[CSS_MAX_RULES];
    int css_n;
    char css_text[OKAI_CSS_TEXT];
    // Clickable links recorded during render, for mouse hit-testing
    struct okai_link links[OKAI_MAX_LINKS];
    int link_count;
    // History
    char history[OKAI_MAX_HISTORY][OKAI_URL_LEN];
    int history_count;
    int history_pos;
    int redirect_count;    // HTTP 3xx hops followed on this page (anti-loop)
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
// Window id whose tab open/close animation is currently running, or -1.
// okai_draw sets this; the desktop main loop re-marks that window dirty each
// iteration so the animation keeps advancing (window_draw clears w->dirty after
// every render, so a per-render dirty flag alone can't self-sustain it).
extern int okai_anim_win;
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
void okai_nav_back(int id);
void okai_nav_fwd(int id);
void okai_nav_reload(int id);
void okai_nav_home(int id);
// Chrome + heading overlays for ONE window — desktop.c paints these right
// after the window itself in z-order (okai_draw_chrome_all drew them after
// ALL windows, so a lower okai's chrome painted over a higher window).
void okai_paint_overlays(int id);

#endif
