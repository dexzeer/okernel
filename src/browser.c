#include "browser.h"
#include "window.h"
#include "graphics.h"
#include "memory.h"
#include "net/network.h"
#include "filesystem.h"
#include "serial.h"
#include <stdint.h>

#define MAX_BROWSERS 4
static struct browser browsers[MAX_BROWSERS];
static int browser_count = 0;

// External: save filename for browse command
extern void net_set_browse_save(const char* filename);

void browser_init(void) {
    for (int i = 0; i < MAX_BROWSERS; i++) {
        browsers[i].win_id = -1;
    }
    browser_count = 0;
}

static void parse_url(const char* url, char* host, char* path) {
    host[0] = 0;
    path[0] = 0;
    int i = 0;
    // Skip http://
    if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
        url[4] == ':' && url[5] == '/' && url[6] == '/') {
        i = 7;
    }
    // Extract host
    int hi = 0;
    while (url[i] && url[i] != '/' && url[i] != ':' && hi < 127) {
        host[hi++] = url[i++];
    }
    host[hi] = 0;
    // Skip port if present
    if (url[i] == ':') {
        while (url[i] && url[i] != '/') i++;
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

// Render tokens into window content buffer
void render_content(int ed_id) {
    struct browser* b = &browsers[ed_id];
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;

    window_clear(b->win_id);

    int view_w = w->content_w;
    int view_h = w->content_h - 2; // Reserve 2 rows: toolbar + address bar
    int cx = 0; // cursor x in content buffer
    int cy = 0; // cursor y in content buffer
    int skip = b->scroll_y;

    // Draw toolbar
    window_set_text_color(b->win_id, 0, 7); // Black on grey
    window_puts(b->win_id, " ");
    window_puts(b->win_id, b->addr_bar_focused ? "[URL]" : "[URL]");
    window_puts(b->win_id, " ");
    window_puts(b->win_id, b->title[0] ? b->title : b->url);
    for (int i = 0; i < view_w - 30; i++) window_put_char(b->win_id, ' ');
    window_set_text_color(b->win_id, 15, 0); // White on black

    // Draw address bar
    window_set_text_color(b->win_id, 15, 1); // White on blue
    window_put_char(b->win_id, '>');
    if (b->addr_bar_focused) {
        for (int i = 0; i < view_w - 1 && i < b->addr_input_len; i++) {
            window_put_char(b->win_id, b->addr_input[i]);
        }
        // Cursor indicator
        if (b->addr_input_len < view_w - 1) {
            window_put_char(b->win_id, '_');
        }
    } else {
        for (int i = 0; i < view_w - 1 && b->url[i]; i++) {
            window_put_char(b->win_id, b->url[i]);
        }
    }
    for (int i = (b->addr_bar_focused ? b->addr_input_len + 1 : 0);
         i < view_w - 1; i++) {
        window_put_char(b->win_id, ' ');
    }
    window_set_text_color(b->win_id, 15, 0);

    // Render HTML tokens
    for (int i = 0; i < b->token_count; i++) {
        struct html_token* t = &b->tokens[i];

        switch (t->type) {
        case HTML_H1:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            window_set_text_color(b->win_id, 15, 0);
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            window_puts(b->win_id, "\n");
            cy += 2;
            break;

        case HTML_H2:
        case HTML_H3:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            window_puts(b->win_id, "\n");
            cy += 2;
            break;

        case HTML_H4:
        case HTML_H5:
        case HTML_H6:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            window_puts(b->win_id, "\n");
            cy += 2;
            break;

        case HTML_PARA:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            window_puts(b->win_id, "\n");
            cy += 2;
            break;

        case HTML_LINK:
            if (skip > 0) { skip--; break; }
            window_set_text_color(b->win_id, 11, 0); // Cyan
            window_put_char(b->win_id, '[');
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            window_put_char(b->win_id, ']');
            window_set_text_color(b->win_id, 15, 0);
            break;

        case HTML_LIST_ITEM:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n * ");
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            cy++;
            break;

        case HTML_PRE:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            window_puts(b->win_id, "\n");
            cy += 2;
            break;

        case HTML_LINE_BREAK:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            cy++;
            break;

        case HTML_BLOCK:
            if (skip > 0) { skip--; break; }
            window_puts(b->win_id, "\n");
            for (int j = 0; j < view_w; j++) window_put_char(b->win_id, '-');
            window_puts(b->win_id, "\n");
            cy += 2;
            break;

        case HTML_TEXT:
            if (skip > 0) { skip--; break; }
            for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
                window_put_char(b->win_id, t->text[j] >= 32 ? t->text[j] : ' ');
            }
            break;

        case HTML_TITLE:
            // Title already captured in url/title
            break;

        default:
            break;
        }
    }

    b->content_height = cy;
    w->dirty = 1;
}

int browser_open(const char* url) {
    if (browser_count >= MAX_BROWSERS) return -1;

    int id = browser_count;
    struct browser* b = &browsers[id];

    // Initialize
    b->scroll_y = 0;
    b->content_height = 0;
    b->token_count = 0;
    b->addr_bar_focused = 0;
    b->addr_input_len = 0;
    b->addr_input[0] = 0;
    b->title[0] = 0;
    b->history_count = 0;
    b->history_pos = 0;

    // Set URL
    int ui = 0;
    while (url[ui] && ui < BROWSER_URL_LEN - 1) {
        b->url[ui] = url[ui];
        ui++;
    }
    b->url[ui] = 0;

    // Add to history
    for (int i = 0; i < ui + 1; i++) b->history[0][i] = b->url[i];
    b->history_count = 1;

    // Create window
    int win = window_create("okai", 30, 20, 520, 400);
    if (win < 0) return -1;
    window_set_close_button(win, 1);
    window_set_minimize_button(win, 1);
    b->win_id = win;
    browser_count++;

    // Parse URL and start navigation
    char host[128], path[128];
    parse_url(url, host, path);

    // Start HTTP request
    http_get(host, path);

    // Render initial state
    render_content(id);

    serial_puts("[browser] opened: ");
    serial_puts(url);
    serial_putchar('\n');

    return id;
}

void browser_close(int id) {
    if (id < 0 || id >= MAX_BROWSERS) return;
    if (browsers[id].win_id >= 0) {
        window_destroy(browsers[id].win_id);
        browsers[id].win_id = -1;
    }
}

void browser_navigate(int id, const char* url) {
    struct browser* b = &browsers[id];
    if (b->win_id < 0) return;

    // Set URL
    int ui = 0;
    while (url[ui] && ui < BROWSER_URL_LEN - 1) {
        b->url[ui] = url[ui];
        ui++;
    }
    b->url[ui] = 0;

    // Add to history
    if (b->history_count < BROWSER_MAX_HISTORY) {
        for (int i = 0; i < ui + 1; i++)
            b->history[b->history_count][i] = b->url[i];
        b->history_count++;
        b->history_pos = b->history_count - 1;
    }

    b->scroll_y = 0;
    b->token_count = 0;
    b->title[0] = 0;

    char host[128], path[128];
    parse_url(url, host, path);
    http_get(host, path);

    render_content(id);
}

void browser_handle_key(int id, char c) {
    struct browser* b = &browsers[id];
    if (b->win_id < 0) return;

    struct window* w = window_get(b->win_id);
    if (!w) return;

    if (b->addr_bar_focused) {
        // Address bar input mode
        if (c == '\n') {
            // Navigate
            b->addr_bar_focused = 0;
            if (b->addr_input_len > 0) {
                browser_navigate(id, b->addr_input);
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
        } else if (c >= 32 && c < 127 && b->addr_input_len < BROWSER_URL_LEN - 1) {
            b->addr_input[b->addr_input_len++] = c;
            b->addr_input[b->addr_input_len] = 0;
        }
        render_content(id);
    } else {
        // Content scroll mode
        if (c == 'g' || c == 'G') {
            // Go to address bar
            b->addr_bar_focused = 1;
            b->addr_input_len = 0;
            b->addr_input[0] = 0;
            render_content(id);
        } else if (c == 'j' || c == '\n') {
            // Scroll down
            if (b->scroll_y < b->content_height - (w->content_h - 2)) {
                b->scroll_y++;
                render_content(id);
            }
        } else if (c == 'k') {
            // Scroll up
            if (b->scroll_y > 0) {
                b->scroll_y--;
                render_content(id);
            }
        } else if (c == 'l') {
            // Scroll right (no-op for now)
        } else if (c == 'h') {
            // Scroll left (no-op for now)
        } else if (c == 'r') {
            // Refresh
            browser_navigate(id, b->url);
        } else if (c == 'b') {
            // Back
            if (b->history_pos > 0) {
                b->history_pos--;
                int pi = 0;
                while (b->history[b->history_pos][pi] && pi < BROWSER_URL_LEN - 1) {
                    b->url[pi] = b->history[b->history_pos][pi];
                    pi++;
                }
                b->url[pi] = 0;
                b->scroll_y = 0;
                b->token_count = 0;
                b->title[0] = 0;
                char host[128], path[128];
                parse_url(b->url, host, path);
                http_get(host, path);
                render_content(id);
            }
        }
    }
}

void browser_handle_mouse_scroll(int id, int dy) {
    struct browser* b = &browsers[id];
    if (b->win_id < 0) return;
    struct window* w = window_get(b->win_id);
    if (!w) return;

    int view_h = w->content_h - 2;
    b->scroll_y -= dy; // dy positive = scroll up
    if (b->scroll_y < 0) b->scroll_y = 0;
    if (b->scroll_y > b->content_height - view_h)
        b->scroll_y = b->content_height - view_h;
    if (b->scroll_y < 0) b->scroll_y = 0;
    render_content(id);
}

void browser_draw(int id) {
    if (id < 0 || id >= MAX_BROWSERS) return;
    // Content is rendered in render_content, called on state changes
    // Window system handles the actual pixel drawing
}

int browser_find_by_win(int win_id) {
    for (int i = 0; i < MAX_BROWSERS; i++) {
        if (browsers[i].win_id == win_id) return i;
    }
    return -1;
}

struct browser* browser_get(int id) {
    if (id < 0 || id >= MAX_BROWSERS) return 0;
    if (browsers[id].win_id < 0) return 0;
    return &browsers[id];
}
