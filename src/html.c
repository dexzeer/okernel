#include "html.h"
#include "serial.h"

static int add_token(struct html_token* tokens, int* count, int max,
                     uint8_t type, const char* text, const char* href) {
    if (*count >= max) return -1;
    struct html_token* t = &tokens[*count];
    t->type = type;
    t->text[0] = 0;
    t->href[0] = 0;
    int i = 0;
    if (text) {
        while (text[i] && i < HTML_MAX_TEXT - 1) {
            t->text[i] = text[i];
            i++;
        }
        t->text[i] = 0;
    }
    if (href) {
        i = 0;
        while (href[i] && i < 255) {
            t->href[i] = href[i];
            i++;
        }
        t->href[i] = 0;
    }
    (*count)++;
    return 0;
}

static int add_char_to_text(char* text, int* len, int max, char c) {
    if (*len < max - 1) {
        text[*len] = c;
        (*len)++;
    }
    return *len;
}

// Compare tag name (case-insensitive, stops at whitespace or '>')
static int tag_match(const char* html, int pos, const char* tag) {
    int i = 0;
    while (tag[i]) {
        char c = html[pos + i];
        if (c >= 'A' && c <= 'Z') c += 32; // tolower
        if (c != tag[i]) return 0;
        i++;
    }
    // Check next char is whitespace, '>', or '/'
    char next = html[pos + i];
    return (next == ' ' || next == '>' || next == '/' || next == '\t' || next == '\n');
}

// Skip until closing tag
static int skip_to_close(const char* html, int pos, int len, const char* tag) {
    int tag_len = 0;
    while (tag[tag_len]) tag_len++;
    while (pos < len - tag_len) {
        if (html[pos] == '<' && html[pos + 1] == '/') {
            int match = 1;
            for (int i = 0; i < tag_len; i++) {
                char c = html[pos + 2 + i];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != tag[i]) { match = 0; break; }
            }
            if (match) {
                // Skip past the closing tag
                pos += 2 + tag_len;
                while (pos < len && html[pos] != '>') pos++;
                if (pos < len) pos++; // skip '>'
                return pos;
            }
        }
        pos++;
    }
    return len;
}

// Extract attribute value: attr="value" or attr='value'
static int extract_attr(const char* html, int pos, int len,
                        const char* attr_name, char* value, int max_val) {
    int alen = 0;
    while (attr_name[alen]) alen++;

    while (pos < len) {
        // Look for attribute name
        if (html[pos] == attr_name[0]) {
            int match = 1;
            for (int i = 1; i < alen; i++) {
                if (pos + i >= len) { match = 0; break; }
                char c = html[pos + i];
                if (c >= 'A' && c <= 'Z') c += 32;
                if (c != attr_name[i]) { match = 0; break; }
            }
            if (match) {
                int at = pos + alen;
                while (at < len && html[at] == ' ') at++;
                if (at < len && html[at] == '=') {
                    at++;
                    while (at < len && html[at] == ' ') at++;
                    if (at < len && (html[at] == '"' || html[at] == '\'')) {
                        char quote = html[at];
                        at++;
                        int vi = 0;
                        while (at < len && html[at] != quote && vi < max_val - 1) {
                            value[vi++] = html[at++];
                        }
                        value[vi] = 0;
                        return 1;
                    }
                }
                return 0;
            }
        }
        pos++;
    }
    return 0;
}

int html_get_title(const char* html, int html_len, char* title, int max_len) {
    title[0] = 0;
    int pos = 0;
    while (pos < html_len - 6) {
        if (html[pos] == '<' && tag_match(html, pos + 1, "title")) {
            pos += 6; // past "title"
            // skip attributes to the tag's '>' (none for plain <title>)
            while (pos < html_len && html[pos] != '>') pos++;
            pos++; // skip '>'
            int ti = 0;
            while (pos < html_len && html[pos] != '<' && ti < max_len - 1) {
                title[ti++] = html[pos++];
            }
            title[ti] = 0;
            return ti;
        }
        pos++;
    }
    return 0;
}

int html_parse(const char* html, int html_len, struct html_token* tokens, int max_tokens) {
    int count = 0;
    int pos = 0;
    int in_head = 0;
    int in_script = 0;
    int in_style = 0;

    // Skip HTTP headers (everything before \r\n\r\n)
    for (int i = 0; i < html_len - 3; i++) {
        if (html[i] == '\r' && html[i+1] == '\n' && html[i+2] == '\r' && html[i+3] == '\n') {
            pos = i + 4;
            break;
        }
    }

    while (pos < html_len && count < max_tokens) {
        if (html[pos] == '<') {
            // Skip to end of tag
            int tag_start = pos;
            pos++;
            int is_close = 0;
            if (pos < html_len && html[pos] == '/') {
                is_close = 1;
                pos++;
            }

            // Read tag name
            char tag_name[32];
            int tn_len = 0;
            while (pos < html_len && html[pos] != ' ' && html[pos] != '>' &&
                   html[pos] != '/' && tn_len < 31) {
                tag_name[tn_len++] = html[pos++];
                if (tag_name[tn_len-1] >= 'A' && tag_name[tn_len-1] <= 'Z')
                    tag_name[tn_len-1] += 32;
            }
            tag_name[tn_len] = 0;

            // Check for <head>, <script>, <style>
            if (!is_close) {
                if (tag_match(html, tag_start + 1, "head")) in_head = 1;
                if (tag_match(html, tag_start + 1, "script")) in_script = 1;
                if (tag_match(html, tag_start + 1, "style")) in_style = 1;
            } else {
                if (tag_match(html, tag_start + 2, "script")) in_script = 0;
                if (tag_match(html, tag_start + 2, "style")) in_style = 0;
                if (tag_match(html, tag_start + 2, "head")) in_head = 0;
            }

            // Skip if inside head/script/style
            if (in_head || in_script || in_style) {
                // Skip to end of this opening tag
                while (pos < html_len && html[pos] != '>') pos++;
                if (pos < html_len) pos++;
                // If it was an opening tag, skip to closing tag
                if (!is_close && (in_script || in_style)) {
                    pos = skip_to_close(html, pos, html_len, tag_name);
                    // skip_to_close consumed </script> or </style> — the
                    // main-loop closer branch below never sees it. Without
                    // resetting here, every byte after the block is skipped
                    // forever (example.com: <style> before any content =
                    // zero tokens, blank page)
                    in_script = 0;
                    in_style = 0;
                }
                continue;
            }

            // Skip to end of tag
            while (pos < html_len && html[pos] != '>') pos++;
            if (pos < html_len) pos++; // skip '>'
            // For other tags (div, span, etc), continue to read text after them

            // If it's a void element or closing tag, skip
            if (is_close) {
                // Add end-para token for structural elements
                if (tag_match(html, tag_start + 2, "p") ||
                    tag_match(html, tag_start + 2, "div") ||
                    tag_match(html, tag_start + 2, "li") ||
                    tag_match(html, tag_start + 2, "tr") ||
                    tag_match(html, tag_start + 2, "td")) {
                    add_token(tokens, &count, max_tokens, HTML_END_PARA, "", "");
                }
                continue;
            }

            // Handle specific tags
            if (tag_match(html, tag_start + 1, "br")) {
                add_token(tokens, &count, max_tokens, HTML_LINE_BREAK, "", "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "hr")) {
                add_token(tokens, &count, max_tokens, HTML_BLOCK, "", "");
                continue;
            }

            // For content tags, extract text until closing tag
            if (tag_match(html, tag_start + 1, "h1")) {
                // Extract text
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H1, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h2")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H2, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h3")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H3, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h4")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H4, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h5")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H5, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h6")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H6, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "li")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_LIST_ITEM, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "pre")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                pos = skip_to_close(html, pos, html_len, "pre");
                // Re-scan from tag_end to get text
                int scan = tag_start;
                while (scan < html_len && html[scan] != '>') scan++;
                scan++;
                while (scan < pos - 4 && html[scan] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[scan++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_PRE, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "title")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_TITLE, text, "");
                continue;
            }

            // Handle <a href="...">text</a>
            if (tag_match(html, tag_start + 1, "a")) {
                char href[256] = {0};
                extract_attr(html, tag_start + 1, html_len, "href", href, 256);
                // Extract link text until </a>
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len) {
                    if (html[pos] == '<') {
                        if (pos + 1 < html_len && html[pos + 1] == '/') {
                            // Check if it's </a>
                            if (pos + 2 < html_len && html[pos + 2] == 'a') {
                                pos += 3;
                                while (pos < html_len && html[pos] != '>') pos++;
                                if (pos < html_len) pos++;
                                break;
                            }
                        }
                        // Skip nested tags
                        pos++;
                        while (pos < html_len && html[pos] != '>') pos++;
                        if (pos < html_len) pos++;
                    } else {
                        add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                    }
                }
                text[tl] = 0;
                if (tl > 0 || href[0]) {
                    add_token(tokens, &count, max_tokens, HTML_LINK, text, href);
                }
                continue;
            }

            // For other tags (div, span, etc), continue to read text after them
        }

        // Plain text
        if (!in_head && !in_script && !in_style) {
            char text[HTML_MAX_TEXT]; int tl = 0;
            while (pos < html_len && html[pos] != '<') {
                // Decode HTML entities
                if (html[pos] == '&' && pos + 1 < html_len) {
                    if (html[pos+1] == '#') {
                        // Numeric entity: &#NNN;
                        int val = 0;
                        int ep = pos + 2;
                        while (ep < html_len && html[ep] >= '0' && html[ep] <= '9') {
                            val = val * 10 + (html[ep] - '0');
                            ep++;
                        }
                        if (ep < html_len && html[ep] == ';') {
                            if (val >= 32 && val < 127) {
                                add_char_to_text(text, &tl, HTML_MAX_TEXT, (char)val);
                            }
                            pos = ep + 1;
                            continue;
                        }
                    } else {
                        // Named entity
                        if (tag_match(html, pos + 1, "amp")) {
                            add_char_to_text(text, &tl, HTML_MAX_TEXT, '&');
                            pos += 5; // &amp;
                            continue;
                        }
                        if (tag_match(html, pos + 1, "lt")) {
                            add_char_to_text(text, &tl, HTML_MAX_TEXT, '<');
                            pos += 4; // &lt;
                            continue;
                        }
                        if (tag_match(html, pos + 1, "gt")) {
                            add_char_to_text(text, &tl, HTML_MAX_TEXT, '>');
                            pos += 4; // &gt;
                            continue;
                        }
                        if (tag_match(html, pos + 1, "quot")) {
                            add_char_to_text(text, &tl, HTML_MAX_TEXT, '"');
                            pos += 6; // &quot;
                            continue;
                        }
                        if (tag_match(html, pos + 1, "nbsp")) {
                            add_char_to_text(text, &tl, HTML_MAX_TEXT, ' ');
                            pos += 5; // &nbsp;
                            continue;
                        }
                    }
                }
                add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
            }
            text[tl] = 0;
            if (tl > 0) {
                add_token(tokens, &count, max_tokens, HTML_TEXT, text, "");
            }
        } else {
            pos++;
        }
    }

    return count;
}
