#ifndef HTML_H
#define HTML_H

#include <stdint.h>

#define HTML_MAX_TOKENS 1024
#define HTML_MAX_TEXT 384

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
#define HTML_TABLE_CELL 19 // <td>/<th> text; cells join on a row, </tr> breaks

struct html_token {
    uint8_t type;
    char text[HTML_MAX_TEXT];
    char href[64];  // For links
    char tag[16];   // element tag (lowercased), for CSS matching
    char cls[32];   // space-separated class list (lowercased)
    char id[32];    // element id (lowercased)
    char style[128]; // inline style="" attribute text
};

int html_parse(const char* html, int html_len, struct html_token* tokens, int max_tokens);
int html_get_title(const char* html, int html_len, char* title, int max_len);
// Make raw page text renderable, in place: decodes the page charset
// (UTF-8 multibyte / windows-1251 → one-byte font slots, Cyrillic included)
// and then HTML entities (&#NNN; and the common named ones). Unmapped
// codepoints become '?'.
void html_decode_entities(char* s);

// Concatenate the text of every <style>...</style> block into `out` (for CSS).
// Returns total bytes written (capped at cap-1).
int html_extract_css(const char* html, int html_len, char* out, int cap);

#endif
