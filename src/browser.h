#ifndef BROWSER_H
#define BROWSER_H

#include <stdint.h>
#include "html.h"

#define BROWSER_MAX_HISTORY 4
#define BROWSER_URL_LEN 128
#define MAX_BROWSERS 2

struct browser {
    int win_id;
    char url[BROWSER_URL_LEN];
    char title[64];
    int scroll_y;
    int content_height; // total lines of rendered content
    struct html_token tokens[HTML_MAX_TOKENS];
    int token_count;
    int addr_bar_focused; // 1 = typing in address bar, 0 = scrolling
    char addr_input[BROWSER_URL_LEN];
    int addr_input_len;
    // History
    char history[BROWSER_MAX_HISTORY][BROWSER_URL_LEN];
    int history_count;
    int history_pos;
};

void browser_init(void);
int browser_open(const char* url);
void browser_close(int id);
void browser_handle_key(int id, char c);
void browser_handle_mouse_scroll(int id, int dy);
void browser_draw(int id);
void render_content(int id);
int browser_find_by_win(int win_id);
struct browser* browser_get(int id);

#endif
