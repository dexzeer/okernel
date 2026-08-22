#include "okai.h"
#include "window.h"
#include "graphics.h"
#include "theme.h"
#include "memory.h"
#include "net/network.h"
#include "net/tls_net.h"
#include "filesystem.h"
#include "serial.h"
#include <stdint.h>
#include <string.h>

// Set by okai_paint_overlays while a tab open/close animation runs; the desktop
// main loop reads it to keep the window re-rendering until the animation settles.
int okai_anim_win = -1;

// Tab chrome sizing / animation constants
#define OKAI_XBOX      16   // close-× square outline box, px
#define OKAI_NB        32   // new-tab "+" button box, px (matches the 32px glyph)
#define OKAI_ANIM_STEP 24   // px/frame tab-width easing for open/close animation

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
    T->css_n = 0;
    T->css_text[0] = 0;
    T->link_count = 0;
    T->history_count = 0;
    T->history_pos = 0;
    T->redirect_count = 0;
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
    int css_len = html_extract_css(OKAI_HOME_HTML, len, T->css_text, OKAI_CSS_TEXT);
    T->css_n = css_parse(T->css_text, css_len, T->css_rules, CSS_MAX_RULES);
    int count = html_parse(OKAI_HOME_HTML, len, T->tokens, OKAI_TAB_TOKENS);
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

    const char* scheme = T->is_https ? "https://" : "http://";
    int i = 0;
    while (scheme[i] && i < outlen - 1) { out[i] = scheme[i]; i++; }
    int j = 0;
    while (host[j] && i < outlen - 1) { out[i] = host[j]; i++; j++; }

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
// One shared scratch buffer is enough: it is only live inside
// okai_render_content; each window keeps its own blitted slice in w->content.
#define OKAI_DOC_LINES 1024
#define OKAI_DOC_COLS  256 // >= max content_w ((SCREEN_W - 2*WIN_BORDER)/CHAR_W) -> 239 at 1920px
#define OKAI_TEXT_PAD  3   // left/right page margin in character columns

static char    doc_chars[OKAI_DOC_LINES][OKAI_DOC_COLS];
static uint32_t doc_fg_rgb[OKAI_DOC_LINES][OKAI_DOC_COLS]; // exact 0xRRGGBB per cell
static uint32_t doc_bg_rgb[OKAI_DOC_LINES][OKAI_DOC_COLS];
static uint8_t doc_line_kind[OKAI_DOC_LINES]; // 0=normal, 1=heading 2x, 2=heading 3x
static int doc_lines, doc_cx, doc_cy, doc_w;
static int doc_left;          // left content margin (cols) for page padding
static uint32_t doc_fg, doc_bg;
static uint32_t g_page_fg, g_page_bg; // page text/bg colors, captured each render for the heading pixel pass

static void doc_putc(char c) {
    if (doc_cy >= OKAI_DOC_LINES) return; // document full — clamp (page truncates)
    if (c == '\n') { doc_cx = doc_left; doc_cy++; return; }
    if (doc_cx >= doc_w) {
        doc_cx = doc_left; doc_cy++;
        if (doc_cy >= OKAI_DOC_LINES) return;
    }
    if (doc_cy >= doc_lines) doc_lines = doc_cy + 1;
    doc_chars[doc_cy][doc_cx] = c;
    doc_fg_rgb[doc_cy][doc_cx] = doc_fg;
    doc_bg_rgb[doc_cy][doc_cx] = doc_bg;
    doc_cx++;
}

// Map a byte to a printable glyph: control bytes become spaces; bytes
// 0x80-0xFF are font slots (Cyrillic + symbols, produced by the HTML
// decoder) and pass straight through.
static char doc_sanitize(char ch) {
    if ((unsigned char)ch >= 128) return ch;
    if (ch < 32) return ' ';
    if (ch == 127) return '?';
    return ch;
}

static int doc_is_space(char c) { return c == ' ' || ((unsigned char)c < 33); }

// Emit one whitespace-delimited word, wrapping to a fresh line first if it
// doesn't fit on the current one. Words longer than a whole line hard-split.
static void doc_flow_word(const char* text, int s, int n) {
    if (n > doc_w) {
        for (int k = s; k < s + n; k++) doc_putc(doc_sanitize(text[k]));
        return;
    }
    // The +1 reserves room for the separating space emitted before the NEXT
    // word; without it "word" fills the line exactly and its separator wraps.
    if (doc_cx > doc_left && doc_cx + 1 + n > doc_w) doc_putc('\n');
    else if (doc_cx > doc_left) doc_putc(' ');
    for (int k = s; k < s + n; k++) doc_putc(doc_sanitize(text[k]));
}

// Flowing text with WORD WRAP: break at whitespace instead of splitting
// words across lines ("without ne / eding"). Runs of whitespace collapse
// to single separators.
static void doc_flow_text(const char* text) {
    int j = 0;
    while (text[j] && j < HTML_MAX_TEXT) {
        while (text[j] && doc_is_space(text[j])) j++;
        if (!text[j] || j >= HTML_MAX_TEXT) break;
        int n = 0;
        while (j + n < HTML_MAX_TEXT && text[j + n] && !doc_is_space(text[j + n])) n++;
        doc_flow_word(text, j, n);
        j += n;
    }
}

// Draw one line of block text honoring CSS text-align (within doc_w). The
// left gutter (doc_left) is left blank so the page has a margin.
static void doc_draw_block_text(const char* text, int view_w, int align) {
    (void)view_w; // wrapping uses the global doc_w; param kept for signature
    int tl = 0;
    while (text[tl] && tl < HTML_MAX_TEXT) tl++;
    if (tl <= doc_w) {
        int pad = 0;
        if (align == CSS_ALIGN_CENTER) pad = (doc_w - tl) / 2;
        else if (align == CSS_ALIGN_RIGHT) pad = doc_w - tl;
        doc_cx = doc_left + pad;
        for (int j = 0; text[j] && j < HTML_MAX_TEXT; j++)
            doc_putc(doc_sanitize(text[j]));
        return;
    }
    doc_flow_text(text);
}

// Draw a box-model outline around an already-laid-out block. The block spans
// document rows [top, bottom) and columns [box_left, box_right); its inner
// content area is [content_left, content_left+content_w) x [top+pt, bottom-pb).
// Fills the border ring with `bcolor` and the padding band with `bg`, leaving
// content cells (text) untouched. Box-model CSS: margin (outside, handled by
// caller via doc_left), padding (inside, filled here), border (ring).
static void doc_box_decorate(int top, int bottom, int box_left, int box_right,
                             int bw, int content_left, int content_w,
                             int pt, int pb, uint32_t bcolor, uint32_t bg) {
    if (bottom <= top || box_right <= box_left) return;
    if (bw < 1) return;
    int ctop = top + pt, cbot = bottom - pb;
    if (cbot < ctop) cbot = ctop;
    // Bottom border sits on [bottom, bottom+bw): the row just past the last
    // content line (the trailing newline), so it never overwrites text. The
    // caller advances doc_cy/doc_lines past it afterwards.
    for (int r = top; r < bottom + bw && r < OKAI_DOC_LINES; r++) {
        for (int c = box_left; c < box_right && c < OKAI_DOC_COLS; c++) {
            int in_border = (c < box_left + bw) || (c >= box_right - bw) ||
                            (r < top + bw) || (r >= bottom);
            int in_content = (c >= content_left && c < content_left + content_w &&
                              r >= ctop && r < cbot);
            if (in_border) {
                doc_chars[r][c] = ' ';
                doc_bg_rgb[r][c] = bcolor;
            } else if (!in_content) {
                doc_chars[r][c] = ' ';
                doc_bg_rgb[r][c] = bg;
            }
        }
    }
}

// Draw a heading scaled up on the uniform char grid: each glyph is placed
// every `hc` grid columns and the line is flagged in doc_line_kind; the blit
// blanks its grid cells and okai_draw_heading_pixels() paints it scaled after
// the window grid is drawn. Trailing blank grid rows reserve the extra
// vertical space the taller glyph needs (scale-1 spacers). Heading hierarchy:
// H1 = 3x (2 spacer rows), H2 = 2x (1 spacer row), H3-H6 = body size (plain
// grid text, no flag) so the three levels read distinctly.
static void doc_draw_heading(const char* text, int align, int level) {
    int hc, kind, spacers;
    if (level <= 1)      { hc = 3; kind = 2; spacers = 2; } // H1: 3x
    else if (level == 2) { hc = 2; kind = 1; spacers = 1; } // H2: 2x
    else {
        // H3-H6: body-size text with the normal heading separation above.
        doc_draw_block_text(text, doc_w, align);
        return;
    }
    int max_chars = doc_w / hc;
    if (max_chars < 1) max_chars = 1;
    int len = 0;
    while (text[len] && len < HTML_MAX_TEXT) len++;
    int pos = 0;
    while (pos < len) {
        int take = len - pos;
        if (take > max_chars) take = max_chars;
        int used = take * hc;
        int pad = 0;
        if (align == CSS_ALIGN_CENTER) pad = (doc_w - used) / 2;
        else if (align == CSS_ALIGN_RIGHT) pad = doc_w - used;
        doc_cx = doc_left + pad;
        for (int j = 0; j < take; j++) {
            doc_putc(doc_sanitize(text[pos + j]));
            for (int e = 1; e < hc; e++) doc_putc(' ');
        }
        if (doc_cy >= 0 && doc_cy < OKAI_DOC_LINES) doc_line_kind[doc_cy] = (uint8_t)kind;
        doc_putc('\n'); // advance past the heading line...
        for (int e = 0; e < spacers; e++) doc_putc('\n'); // ...taller glyph space
        pos += take;
    }
}

void okai_render_content(int ed_id) {
    struct okai* b = &okais[ed_id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;
    if (!w->content) return;

    T->link_count = 0;

    int view_w = w->content_w;
    int view_h = w->content_h - CHROME_ROWS; // Reserve top rows for pixel chrome
    if (view_h < 0) view_h = 0;

    // Base style from <body> rules, inherited by every element. The parser has
    // no DOM hierarchy, so we fold body-level styling (e.g. text-align:center,
    // color) into each token that doesn't set it itself.
    struct css_style body_style;
    css_compute(T->css_rules, T->css_n, "body", 0, 0, 0, &body_style);

    // The body's background is the PAGE background (painted across the whole
    // content area), not per-character. Pick a readable default text color:
    // honor an explicit body color, else choose black-on-light / white-on-dark.
    uint32_t page_bg = body_style.has_bg_rgb ? body_style.bg_rgb : WIN_BG_RGB;
    uint32_t default_fg;
    if (body_style.has_fg_rgb) default_fg = body_style.fg_rgb;
    else if (window_rgb_is_light(page_bg)) default_fg = 0x000000; // black
    else default_fg = 0xFFFFFF; // white
    g_page_fg = default_fg; g_page_bg = page_bg; // for the heading pixel pass
    window_set_content_bg_rgb(b->win_id, page_bg);
    // Clear AFTER setting the page background so blank cells carry the page
    // color (otherwise empty space chars repaint the content area black).
    window_clear(b->win_id);

    // The toolbar + tab strip + address bar are drawn as PIXEL chrome by
    // okai_draw_chrome() (gradient backgrounds, icon buttons, lock icon). The
    // content-buffer rows 0..CHROME_ROWS-1 stay blank and are overdrawn by it.
    window_set_cursor(b->win_id, CHROME_ROWS, OKAI_TEXT_PAD);

    // Failed fetch (DNS failure, unreachable host, timeout): show a readable
    // error instead of a black page. token_count == -1 with no response bytes
    // means nothing ever arrived; a response that parsed to zero tokens is a
    // different (benign) case.
    if (T->token_count == -1 && T->last_resp_len == 0) {
        window_set_text_color_rgb(b->win_id, 0xFF5555, 0x000000); // light red
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

    // ---- Layout pass: render the WHOLE page into the virtual document ----
    // Clear the whole grid first: cells the new page never writes would
    // otherwise blit stale glyphs from a previous page (ghost headings,
    // interleaved text from two sites).
    memset(doc_chars, ' ', sizeof(doc_chars));
    for (int r = 0; r < OKAI_DOC_LINES; r++) {
        for (int c = 0; c < OKAI_DOC_COLS; c++) {
            doc_fg_rgb[r][c] = default_fg;
            doc_bg_rgb[r][c] = page_bg;
        }
    }
    doc_lines = 0; doc_cx = OKAI_TEXT_PAD; doc_cy = 0;
    for (int i = 0; i < OKAI_DOC_LINES; i++) doc_line_kind[i] = 0;
    doc_left = OKAI_TEXT_PAD;
    doc_w = view_w - 2 * OKAI_TEXT_PAD;
    if (doc_w < 20) doc_w = 20;
    doc_fg = default_fg; doc_bg = page_bg;

    for (int i = 0; i < T->token_count; i++) {
        struct html_token* t = &T->tokens[i];

        // Compute CSS for this element (defaults + matched rules + inline)
        struct css_style st;
        css_compute(T->css_rules, T->css_n, t->tag, t->cls, t->id, t->style, &st);
        css_merge_base(&st, &body_style); // inherit body-level styling

        // display:none elements are not rendered
        if (st.has_display && st.display == CSS_DISPLAY_NONE) continue;

        uint32_t fg = st.has_fg_rgb ? st.fg_rgb : default_fg;
        uint32_t bg = st.has_bg_rgb ? st.bg_rgb : page_bg;
        if (st.has_mt && st.margin_top > 0) doc_putc('\n');
        doc_fg = fg; doc_bg = bg;

        // ---- Box model (block-level elements only) ----
        // px -> char cells. margin already applied via doc_left/dw before here
        // for top; left/right and padding/border are computed per block below.
        int is_block = (t->type == HTML_PARA || t->type == HTML_PRE ||
                        t->type == HTML_BLOCK || t->type == HTML_LIST_ITEM ||
                        t->type == HTML_DIV ||
                        (t->type >= HTML_H1 && t->type <= HTML_H6));
        int ml=0,mr=0,pl=0,pr=0,pt=0,pb=0,bw=0;
        uint32_t bcolor = 0x000000;
        if (is_block) {
            ml = st.has_ml ? st.margin_left  / CHAR_W : 0;
            mr = st.has_mr ? st.margin_right / CHAR_W : 0;
            pl = st.has_pl ? st.padding_left  / CHAR_W : 0;
            pr = st.has_pr ? st.padding_right / CHAR_W : 0;
            pt = st.has_pt ? st.padding_top   / CHAR_H : 0;
            pb = st.has_pb ? st.padding_bottom/ CHAR_H : 0;
            bw = (st.has_bw && st.border_width > 0) ? (st.border_width + CHAR_W - 1)/CHAR_W : 0;
            if (st.has_bw && st.border_width > 0 && bw < 1) bw = 1;
            bcolor = st.has_bc ? st.border_color : 0x000000;
        }
        int saved_left = doc_left, saved_w = doc_w;
        int block_top = doc_cy;
        int content_left = doc_left + ml + bw + pl;
        int content_w = doc_w - ml - mr - 2*bw - pl - pr;
        if (content_w < 10) content_w = 10;
        if (st.has_w && is_block) {
            int wcols = st.width / CHAR_W;
            if (wcols >= 10 && wcols < content_w) content_w = wcols;
        }
        for (int _r = 0; _r < pt; _r++) doc_putc('\n'); // padding-top space
        doc_left = content_left; doc_w = content_w;
        // Reserve (bw-1) extra rows above the content so the top border spans
        // [block_top, block_top+bw) and the content begins at block_top+bw. The
        // block case's own leading newline supplies the final (bw-th) top row.
        for (int _b = 0; bw > 1 && _b < bw - 1; _b++) doc_putc('\n');

        switch (t->type) {
        case HTML_H1:
        case HTML_H2:
        case HTML_H3:
        case HTML_H4:
        case HTML_H5:
        case HTML_H6:
            doc_putc('\n');                       // separation above
            doc_draw_heading(t->text, st.align, t->type); // H1=3x, H2=2x, rest=1x
            break;

        case HTML_PARA:
            doc_putc('\n');
            doc_draw_block_text(t->text, doc_w, st.align);
            doc_putc('\n');
            break;

        case HTML_LINK: {
            // Inline link: flow with surrounding text (no forced newline) so a
            // link sits inside its paragraph. Drawn in blue with no brackets,
            // like a real browser; its span is recorded (document coordinates,
            // converted to buffer coordinates below) so a click can open it.
            // Whole-link fit check FIRST: a link that wrapped mid-text used to
            // record its click region at the wrap point (a bogus 1-char span).
            {
                int tl = 0;
                while (t->text[tl] && tl < HTML_MAX_TEXT) tl++;
                int sep = doc_cx > doc_left ? 1 : 0;
                if (doc_cx + sep + tl > doc_left + doc_w && tl <= doc_w)
                    doc_putc('\n');
            }
            if (doc_cx > doc_left) doc_putc(' '); // separator from preceding word
            int lrow = doc_cy;
            int lcol0 = doc_cx;
            int n = 0;
            if (T->link_count < OKAI_MAX_LINKS) {
                doc_fg = st.has_fg_rgb ? st.fg_rgb : 0x0000EE; // honor CSS link color, else browser-default blue
                for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                    char ch = t->text[j];
                    if ((unsigned char)ch >= 128) { /* slot byte: keep */ }
                    else if (ch < 32) ch = ' ';
                    else if (ch == 127) ch = '?';
                    doc_putc(ch);
                    n++;
                }
                if (n > 0) {
                    int li = T->link_count++;
                    T->links[li].row = lrow;
                    T->links[li].col0 = lcol0;
                    T->links[li].col1 = doc_cx - 1;
                    if (T->links[li].col1 < lcol0) T->links[li].col1 = lcol0;
                    if (T->links[li].col1 >= view_w) T->links[li].col1 = view_w - 1;
                    okai_resolve_href(b, t->href, T->links[li].href, OKAI_URL_LEN);
                    serial_printf("[okai] link[%d] row=%d col0=%d col1=%d href=%s\n",
                                  li, T->links[li].row, T->links[li].col0,
                                  T->links[li].col1, T->links[li].href);
                }
            }
            doc_fg = fg; doc_bg = bg;
            break;
        }

        case HTML_LIST_ITEM:
            // The marker ("• " / "N. ") is already prefixed by the tokenizer.
            doc_putc('\n');
            doc_putc(' ');
            doc_draw_block_text(t->text,
                            doc_w > 3 ? doc_w - 3 : doc_w, st.align);
            break;

        case HTML_PRE:
            doc_putc('\n');
            doc_draw_block_text(t->text, doc_w, st.align);
            doc_putc('\n');
            break;

        case HTML_LINE_BREAK:
            doc_putc('\n');
            break;

        case HTML_BLOCK:
            doc_putc('\n');
            for (int j = 0; j < doc_w; j++) doc_putc((char)0xCE); // ─ rule
            doc_putc('\n');
            break;

        case HTML_TABLE_CELL:
            // Cells flow on one line with a light separator; </tr> emits
            // HTML_END_PARA which breaks the row.
            doc_flow_text(t->text);
            doc_putc((char)0xCF); // │ column separator
            doc_putc(' ');
            break;

        case HTML_TEXT:
            doc_flow_text(t->text);
            break;

        case HTML_TITLE:
            // Title already captured in url/title
            break;

        case HTML_END_PARA:
            // Closing </p>/</div>/</li>/</tr>/</td>: end the current line so
            // the next block starts fresh (browsers give these margins; our
            // flat tokenizer only sees the close). Collapses naturally — when
            // already at line start (nested closes, or after a link's own
            // trailing newline) it emits nothing.
            if (doc_cx > doc_left) doc_putc('\n');
            break;

        default:
            break;
        }

        // Restore page margins, emit padding-bottom, draw the border ring.
        doc_left = saved_left; doc_w = saved_w;
        for (int _r = 0; _r < pb; _r++) doc_putc('\n'); // padding-bottom space
        if (is_block && bw > 0) {
            int box_left = saved_left + ml;
            int box_right = box_left + 2*bw + pl + content_w + pr;
            doc_box_decorate(block_top, doc_cy, box_left, box_right, bw,
                             content_left, content_w, pt, pb, bcolor, bg);
            // Bottom border occupies [doc_cy, doc_cy+bw); advance past it and
            // extend doc_lines so the blit paints those rows.
            for (int _b = 0; _b < bw; _b++) { doc_cx = doc_left; doc_cy++; }
            if (doc_cy > doc_lines) doc_lines = doc_cy;
        }
        if (st.has_mb && st.margin_bottom > 0) doc_putc('\n');
    }
    if (doc_lines < 1) doc_lines = 1;
    T->content_height = doc_lines; // TRUE total height, independent of scroll


    // ---- Clamp scroll against the real document height ----
    {
        int max_scroll = doc_lines - view_h;
        if (max_scroll < 0) max_scroll = 0;
        if (T->scroll_y > max_scroll) T->scroll_y = max_scroll;
        if (T->scroll_y < 0) T->scroll_y = 0;
    }

    // ---- Blit the visible slice into the content buffer (below the chrome) ----
    for (int r = 0; r < view_h; r++) {
        int dr = T->scroll_y + r;
        for (int c = 0; c < view_w; c++) {
            // Left/right gutters and lines past the document end stay blank
            // with the page background (content is inset by doc_left).
            if (c < doc_left || c >= doc_left + doc_w || dr >= doc_lines) {
                window_write_cell_rgb(b->win_id, CHROME_ROWS + r, c, ' ',
                                      default_fg, page_bg);
            } else if (dr < OKAI_DOC_LINES && doc_line_kind[dr]) {
                // Heading line: leave the grid blank — a later pixel pass paints
                // it at 2x so the small uniform grid glyph won't show underneath.
                window_write_cell_rgb(b->win_id, CHROME_ROWS + r, c, ' ',
                                      default_fg, page_bg);
            } else {
                window_write_cell_rgb(b->win_id, CHROME_ROWS + r, c, doc_chars[dr][c],
                                      doc_fg_rgb[dr][c], doc_bg_rgb[dr][c]);
            }
        }
    }

    // Links were recorded in document rows; convert to content-BUFFER rows
    // (what desktop.c's click hit-test computes from the mouse position).
    // The blit places doc row (scroll_y + r) at buffer row CHROME_ROWS + r.
    // Links scrolled into the chrome area are invalidated so they can never
    // match a click (rows 0..CHROME_ROWS-1 are the pixel chrome).
    for (int li = 0; li < T->link_count; li++) {
        int buf_row = T->links[li].row - T->scroll_y + CHROME_ROWS;
        if (buf_row < CHROME_ROWS || buf_row >= w->content_h) buf_row = -1;
        T->links[li].row = buf_row;
    }

    w->dirty = 1;
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
    if (T->is_https) https_get(host, path);
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

// Upgrade an http:// or scheme-less URL to https://. Other schemes
// (okai:home, ftp://, ...) are left untouched. Used so every navigation
// defaults to TLS, with an HTTP fallback only when a host lacks HTTPS.
static void okai_normalize_https(const char* in, char* out, int outlen) {
    int has_scheme = 0;
    for (int i = 0; in[i]; i++) {
        if (in[i] == ':') { has_scheme = 1; break; }
    }
    if (strncmp(in, "https://", 8) == 0) {
        int j = 0; while (in[j] && j < outlen - 1) { out[j] = in[j]; j++; } out[j] = 0;
    } else if (strncmp(in, "http://", 7) == 0) {
        int j = 0; const char* s = "https://";
        while (s[j] && j < outlen - 1) { out[j] = s[j]; j++; }
        int i = 7; while (in[i] && j < outlen - 1) { out[j] = in[i]; j++; i++; }
        out[j] = 0;
    } else if (strncmp(in, "http:", 5) == 0) {
        int j = 0; const char* s = "https:";
        while (s[j] && j < outlen - 1) { out[j] = s[j]; j++; }
        int i = 5; while (in[i] && j < outlen - 1) { out[j] = in[i]; j++; i++; }
        out[j] = 0;
    } else if (has_scheme) {
        // A non-http scheme (okai:home, ftp://, ...): leave untouched.
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

    // One browser window, right of the default terminal (80,60,900x650).
    int win = window_create("okai", 1010, 60, 880, 650);
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
    if (T->is_https) https_get(host, path);
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

void okai_handle_key(int id, char c) {
    struct okai* b = &okais[id];
    struct okai_tab* T = okai_tab_of(b);
    if (b->win_id < 0) return;
    b->show_security = 0; // any keypress dismisses the security popup

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
        okai_render_content(id);
    } else {
        // Content scroll mode
        if (c == 'g' || c == 'G') {
            // Go to address bar
            b->addr_bar_focused = 1;
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
            okai_render_content(id);
        } else if (c == 'j' || c == '\n') {
            // Scroll down one line
            int old = T->scroll_y;
            T->scroll_y++;
            okai_scroll_clamp(b, w);
            if (T->scroll_y != old) okai_render_content(id);
        } else if (c == 'k') {
            // Scroll up one line
            int old = T->scroll_y;
            T->scroll_y--;
            okai_scroll_clamp(b, w);
            if (T->scroll_y != old) okai_render_content(id);
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
    if (T->scroll_y != old) okai_render_content(id);
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

// Navigation icons, 26px box, drawn with 2px-thick strokes so they stay legible
// on the toolbar gradient.
static void okai_icon_back(int x, int y, int s, uint32_t fg) {
    int cy = y + s / 2, tip = x + 5, tail = x + s - 5;
    rect_fill(tip + 2, cy - 1, tail - tip - 2, 3, fg);       // shaft
    line(tip, cy, tip + 6, cy - 6, fg);                      // head, 2px thick
    line(tip + 1, cy, tip + 7, cy - 6, fg);
    line(tip, cy, tip + 6, cy + 6, fg);
    line(tip + 1, cy, tip + 7, cy + 6, fg);
}

static void okai_icon_fwd(int x, int y, int s, uint32_t fg) {
    int cy = y + s / 2, tip = x + s - 5, tail = x + 5;
    rect_fill(tail, cy - 1, tip - tail - 2, 3, fg);          // shaft
    line(tip, cy, tip - 6, cy - 6, fg);
    line(tip - 1, cy, tip - 7, cy - 6, fg);
    line(tip, cy, tip - 6, cy + 6, fg);
    line(tip - 1, cy, tip - 7, cy + 6, fg);
}

static void okai_icon_reload(int x, int y, int s, uint32_t fg) {
    int cx = x + s / 2, cy = y + s / 2, r = s / 2 - 3;
    okai_poly_ring(cx, cy, r, 1, 9, fg);          // open ring (gap at top)
    okai_poly_ring(cx, cy, r - 1, 1, 9, fg);      // 2px-thick ring
    // arrowhead at the ring's open end (top, around index 10)
    int hx = cx + OKAI_CIRC_X[10] * r / 8, hy = cy + OKAI_CIRC_Y[10] * r / 8;
    line(hx - 3, hy + 2, hx, hy - 2, fg);
    line(hx + 3, hy + 2, hx, hy - 2, fg);
    line(hx - 3, hy + 3, hx, hy - 1, fg);
    line(hx + 3, hy + 3, hx, hy - 1, fg);
}

static void okai_icon_home(int x, int y, int s, uint32_t fg) {
    int mid = x + s / 2;
    int half = (s - 8) / 2;
    for (int i = 0; i <= half; i++) { line(mid - i, y + 4 + i, mid + i, y + 4 + i, fg); } // roof
    int half_w = half + 1;                       // roof base half-width
    int body_y = y + 5 + half;
    int body_h = (y + s - 5) - body_y + 1;
    if (body_h < 4) return;
    rect_outline(mid - half_w, body_y, 2 * half_w + 1, body_h, fg, 1);   // walls
    if (body_h >= 8) rect_fill(mid - 1, body_y + body_h - 5, 3, 5, fg);  // door
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
// (anim_w) toward its target; removes any tab whose close animation finished.
// Returns 1 while something is still animating (caller keeps the window dirty).
static int okai_anim_step(int id) {
    struct okai* b = &okais[id];
    if (b->win_id < 0) return 0;
    struct window* w = window_get(b->win_id);
    if (!w) return 0;
    int n = b->tab_count; if (n < 1) n = 1;
    int cwp = w->w - 2 * WIN_BORDER;
    int tw = (cwp - 40) / n; if (tw > 300) tw = 300; if (tw < 60) tw = 60;

    // Remove tabs whose close animation has finished (width reached zero).
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

    int animating = 0;
    for (int ti = 0; ti < b->tab_count; ti++) {
        int target = b->tabs[ti].closing ? 0 : tw;
        int cur = b->tabs[ti].anim_w;
        if (cur < target) { cur += OKAI_ANIM_STEP; if (cur > target) cur = target; animating = 1; }
        else if (cur > target) { cur -= OKAI_ANIM_STEP; if (cur < target) cur = target; animating = 1; }
        b->tabs[ti].anim_w = cur;
    }
    return animating;
}

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
}

static void okai_draw_heading_pixels(int id); // defined just below

// Chrome + heading overlays for one window. desktop.c paints these right
// after the window itself (in z-order) — the old draw-after-all-windows pass
// let a LOWER okai's chrome paint over a HIGHER overlapping window ("text
// leaking through").
// The security popup card (okai_draw_chrome) must sit ON TOP of page content,
// so the heading pixels are painted first and the chrome/card last.
void okai_paint_overlays(int id) {
    // Advance tab open/close animation. This *is* the live render path
    // (desktop.c calls it every iteration for each visible okai window), so
    // stepping here is what grows anim_w from 0 to target. Keep the okai window
    // re-marked dirty while animating so desktop.c re-renders it next iteration.
    if (okai_anim_step(id)) {
        struct okai* b = &okais[id];
        okai_anim_win = (b->win_id >= 0) ? b->win_id : -1;
    } else {
        okai_anim_win = -1;
    }
    okai_draw_heading_pixels(id);
    okai_draw_chrome(id);
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

    int view_h = w->content_h - CHROME_ROWS;
    if (view_h < 0) view_h = 0;
    for (int r = 0; r < view_h; r++) {
        int dr = T->scroll_y + r;
        if (dr < 0 || dr >= doc_lines || dr >= OKAI_DOC_LINES) continue;
        int kind = doc_line_kind[dr];
        if (!kind) continue;
        int glyph_rows = kind + 1;                   // 2x spans 2 grid rows, 3x spans 3
        int hscale = (kind + 1) * w->font_scale;     // 2x / 3x the body glyph size
        int cell_row = CHROME_ROWS + r;
        if (cell_row + glyph_rows - 1 >= w->content_h) continue;
        int py = cy + cell_row * cell_h;
        for (int c = doc_left; c < doc_left + doc_w && c < w->content_w; c++) {
            char ch = doc_chars[dr][c];
            if (ch == ' ' || (unsigned char)ch < 33) continue;
            int px = cx + c * cell_w;
            draw_char_sized(px, py, doc_sanitize(ch), fg, bg,
                            CONTENT_GW * hscale, CONTENT_GH * hscale);
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
const char*     preview_doc_chars(void) { return &doc_chars[0][0]; }
const uint32_t* preview_doc_fg_rgb(void) { return &doc_fg_rgb[0][0]; }
const uint32_t* preview_doc_bg_rgb(void) { return &doc_bg_rgb[0][0]; }
const uint8_t*  preview_doc_kinds(void) { return &doc_line_kind[0]; }
int             preview_doc_dims(int* left, int* width) { *left = doc_left; *width = doc_w; return doc_lines; }
void            preview_page_colors(uint32_t* fg, uint32_t* bg) { *fg = g_page_fg; *bg = g_page_bg; }
struct okai*    okai_get_for_preview(void) { return &okais[0]; }
void            okai_render_content_for_preview(void) { okai_render_content(0); }
int             preview_doc_cols(void) { return OKAI_DOC_COLS; }
#endif
