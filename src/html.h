#ifndef HTML_H
#define HTML_H

#include <stdint.h>

#define HTML_MAX_TOKENS 256
#define HTML_MAX_TEXT 128

// Token types
#define HTML_TEXT       0
#define HTML_H1        1
#define HTML_H2        2
#define HTML_H3        3
#define HTML_H4        4
#define HTML_H5        5
#define HTML_H6        6
#define HTML_PARA      7
#define HTML_LINK      8
#define HTML_LIST_ITEM 9
#define HTML_PRE       10
#define HTML_LINE_BREAK 11
#define HTML_TITLE     12
#define HTML_DIV       13
#define HTML_SPAN      14
#define HTML_BOLD      15
#define HTML_ITALIC    16
#define HTML_BLOCK     17  // <hr> or blockquote
#define HTML_END_PARA  18  // closing </p>, </div>, etc

struct html_token {
    uint8_t type;
    char text[HTML_MAX_TEXT];
    char href[64];  // For links
};

int html_parse(const char* html, int html_len, struct html_token* tokens, int max_tokens);
int html_get_title(const char* html, int html_len, char* title, int max_len);

#endif
