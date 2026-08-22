#include "html.h"
#include "serial.h"
#include <string.h>

// Current element attributes, copied into every token emitted while it is open.
// Reset on each opening/closing tag; plain text inherits the enclosing element.
static char g_tag[16];
static char g_cls[32];
static char g_id[32];
static char g_style[96];

// Charset of the current page (sniffed in html_parse). 1 = windows-1251
// (legacy Russian pages); 0 = UTF-8 (also the safe default).
static int g_cp1251 = 0;

// Map a Unicode codepoint to a one-byte render slot. ASCII passes through;
// Cyrillic А-я live at slots 0x80-0xBF; common symbols at 0xC0+ (matching
// font8x16_ext in graphics.c). Unknown codepoints become '?'.
static char html_map_cp(int cp) {
    if (cp < 0x20) return ' '; // whitespace/controls collapse to space
    if (cp < 0x7F) return (char)cp;
    if (cp >= 0x0410 && cp < 0x0450) return (char)(cp - 0x0410 + 0x80);
    switch (cp) {
    case 0x401: return (char)0xC0;  // Ё
    case 0x451: return (char)0xC1;  // ё
    case 0x2014: return (char)0xC2; // —
    case 0x2013: return (char)0xC3; // –
    case 0x00AB: return (char)0xC4; // «
    case 0x00BB: return (char)0xC5; // »
    case 0x2022: return (char)0xC6; // •
    case 0x2116: return (char)0xC7; // №
    case 0x2026: return (char)0xC8; // …
    case 0x00B0: return (char)0xC9; // °
    case 0x20AC: return (char)0xCA; // €
    case 0x00A9: return (char)0xCB; // ©
    case 0x00AE: return (char)0xCC; // ®
    case 0x2122: return (char)0xCD; // ™
    case 0x2500: return (char)0xCE; // ─
    }
    return '?';
}

// Decode the page charset IN PLACE to render slots (output <= input).
// UTF-8 multibyte sequences and windows-1251 high bytes collapse to one
// slot byte each; malformed sequences become '?'.
static void html_decode_charset(char* s) {
    int r = 0, w = 0;
    while (s[r]) {
        unsigned char b = (unsigned char)s[r];
        if (g_cp1251) {
            int cp = b;
            if (b >= 0xC0 && b <= 0xFF) cp = 0x0410 + (b - 0xC0); // А-я
            else if (b == 0xA8) cp = 0x401;                       // Ё
            else if (b == 0xB8) cp = 0x451;                       // ё
            else if (b == 0x85) cp = 0x2026;                      // …
            else if (b == 0x96) cp = 0x2013;                      // –
            else if (b == 0x97) cp = 0x2014;                      // —
            else if (b == 0xAB) cp = 0x00AB;                      // «
            else if (b == 0xBB) cp = 0x00BB;                      // »
            else if (b == 0xA9) cp = 0x00A9;                      // ©
            else if (b == 0xAE) cp = 0x00AE;                      // ®
            else if (b >= 0x80) cp = '?';
            s[w++] = html_map_cp(cp);
            r++;
        } else {
            int cp = -1, len = 1;
            if (b < 0x80) cp = b;
            else if ((b & 0xE0) == 0xC0 &&
                     s[r+1] && ((unsigned char)s[r+1] & 0xC0) == 0x80) {
                cp = ((b & 0x1F) << 6) | ((unsigned char)s[r+1] & 0x3F); len = 2;
            } else if ((b & 0xF0) == 0xE0 && s[r+1] && s[r+2] &&
                       ((unsigned char)s[r+1] & 0xC0) == 0x80 &&
                       ((unsigned char)s[r+2] & 0xC0) == 0x80) {
                cp = ((b & 0x0F) << 12) | (((unsigned char)s[r+1] & 0x3F) << 6)
                   | ((unsigned char)s[r+2] & 0x3F);
                len = 3;
            }
            if (cp < 0) { s[w++] = '?'; r++; continue; }
            s[w++] = html_map_cp(cp);
            r += len;
        }
    }
    s[w] = 0;
}

// Decode HTML entities IN PLACE (output is never longer than input). Runs
// AFTER the charset pass, so raw multibyte bytes are already slots and only
// ASCII entity syntax remains. Numeric: &#NNN; maps through html_map_cp
// (Cyrillic and symbols render; everything unmapped becomes '?'). Named:
// the common set, with typographic entities mapped to their slots.
// (The old inline decoder matched entity names with tag_match(), whose
// terminator check requires space/>'/' — an entity's ';' never matched, so
// named entities were never actually decoded anywhere.)
void html_decode_entities(char* s) {
    if (!s) return;
    html_decode_charset(s);
    static const char* names[] = {
        "amp", "lt", "gt", "quot", "apos", "nbsp",
        "copy", "reg", "trade", "hellip", "mdash", "ndash",
        "laquo", "raquo", "bull", "middot", "times", "deg",
    };
    static const char vals[] = {
        '&',  '<',  '>',  '"',   '\'',  ' ',
        '\xCB','\xCC','\xCD','\xC8','\xC2','\xC3',
        '\xC4','\xC5','\xC6', '?',   '?',  '\xC9',
    };
    int r = 0, w = 0;
    while (s[r]) {
        if (s[r] == '&') {
            if (s[r + 1] == '#') {
                int val = 0, p, digits = 0;
                if (s[r + 2] == 'x' || s[r + 2] == 'X') { // &#xHH; hex
                    p = r + 3;
                    while (digits < 20) {
                        char c = s[p];
                        int nib;
                        if (c >= '0' && c <= '9') nib = c - '0';
                        else if (c >= 'a' && c <= 'f') nib = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') nib = c - 'A' + 10;
                        else break;
                        val = val * 16 + nib;
                        p++; digits++;
                    }
                } else { // &#DDD; decimal
                    p = r + 2;
                    while (s[p] >= '0' && s[p] <= '9') {
                        val = val * 10 + (s[p] - '0');
                        p++; digits++;
                    }
                }
                if (digits && s[p] == ';') {
                    s[w++] = html_map_cp(val);
                    r = p + 1;
                    continue;
                }
            } else {
                int n = (int)(sizeof(names) / sizeof(names[0]));
                int matched = 0;
                for (int k = 0; k < n; k++) {
                    int L = 0;
                    while (names[k][L]) L++;
                    int ok = 1;
                    for (int i = 0; i < L; i++)
                        if (s[r + 1 + i] != names[k][i]) { ok = 0; break; }
                    if (ok && s[r + 1 + L] == ';') {
                        s[w++] = vals[k];
                        r = r + 1 + L + 1;
                        matched = 1;
                        break;
                    }
                }
                if (matched) continue;
            }
        }
        s[w++] = s[r++];
    }
    s[w] = 0;
}

static void css_lc(char* s) {
    for (int i = 0; s[i]; i++)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
}

static int add_token(struct html_token* tokens, int* count, int max,
                     uint8_t type, const char* text, const char* href) {
    if (*count >= max) return -1;
    struct html_token* t = &tokens[*count];
    memset(t, 0, sizeof(*t));
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
        while (href[i] && i < 63) {
            t->href[i] = href[i];
            i++;
        }
        t->href[i] = 0;
    }
    strncpy(t->tag, g_tag, 15);   t->tag[15] = 0;
    strncpy(t->cls, g_cls, 31);   t->cls[31] = 0;
    strncpy(t->id, g_id, 31);     t->id[31] = 0;
    strncpy(t->style, g_style, 95); t->style[95] = 0;
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
                    } else if (at < len) {
                        // Unquoted value: class=box, id=x, style=...
                        int vi = 0;
                        while (at < len && html[at] != ' ' && html[at] != '>'
                               && html[at] != '/' && html[at] != '\n'
                               && vi < max_val - 1) {
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
    // List state: -1 = none (li gets a • bullet), 0 = <ul>, 1..N = <ol> item
    // number (li gets "N. ").
    int list_ordered = 0;
    int list_num = 0;

    // Sniff the charset before anything else: look for "charset=..." in the
    // headers / early markup (covers both Content-Type and <meta charset>).
    // Cyrillic legacy pages are windows-1251; everything else is treated as
    // UTF-8 (the safe default — malformed bytes decode to '?').
    g_cp1251 = 0;
    {
        int sniff_len = html_len < 2048 ? html_len : 2048;
        for (int i = 0; i + 8 < sniff_len; i++) {
            if (html[i] == 'c' || html[i] == 'C') {
                static const char cs[] = "harset";
                int ok = 1;
                for (int k = 0; k < 6; k++)
                    if (html[i+1+k] != cs[k] && html[i+1+k] != cs[k] - 32) { ok = 0; break; }
                if (ok) {
                    int p = i + 7;
                    while (p < sniff_len && (html[p] == ' ' || html[p] == '='
                           || html[p] == '"' || html[p] == '\'')) p++;
                    // "1251" anywhere in the next few bytes (windows-1251 / cp1251)
                    for (int k = 0; k < 12 && p + 3 < sniff_len; k++, p++)
                        if (html[p] == '1' && html[p+1] == '2' &&
                            html[p+2] == '5' && html[p+3] == '1')
                            g_cp1251 = 1;
                    break;
                }
            }
        }
    }

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

            // Capture element attributes for CSS. Bounded to this tag's '>'.
            {
                int close_pos = tag_start;
                while (close_pos < html_len && html[close_pos] != '>') close_pos++;
                if (is_close) {
                    g_tag[0] = g_cls[0] = g_id[0] = g_style[0] = 0;
                } else {
                    strncpy(g_tag, tag_name, 15); g_tag[15] = 0;
                    extract_attr(html, tag_start + 1, close_pos, "class", g_cls, 32);
                    extract_attr(html, tag_start + 1, close_pos, "id", g_id, 32);
                    extract_attr(html, tag_start + 1, close_pos, "style", g_style, 96);
                    css_lc(g_cls); css_lc(g_id); // case-insensitive class/id
                }
            }

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
                    tag_match(html, tag_start + 2, "blockquote") ||
                    tag_match(html, tag_start + 2, "ul") ||
                    tag_match(html, tag_start + 2, "ol")) {
                    add_token(tokens, &count, max_tokens, HTML_END_PARA, "", "");
                }
                if (tag_match(html, tag_start + 2, "ul") ||
                    tag_match(html, tag_start + 2, "ol")) list_ordered = 0;
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
            if (tag_match(html, tag_start + 1, "ul")) {
                list_ordered = 0;
                continue;
            }
            if (tag_match(html, tag_start + 1, "ol")) {
                list_ordered = 1;
                list_num = 0;
                continue;
            }
            if (tag_match(html, tag_start + 1, "blockquote")) {
                // separation + visual break; content flows as plain text
                add_token(tokens, &count, max_tokens, HTML_LINE_BREAK, "", "");
                continue;
            }
            // <img>: acknowledge with an inline [image: alt] placeholder
            if (tag_match(html, tag_start + 1, "img")) {
                char alt[128] = {0};
                extract_attr(html, tag_start + 1, html_len, "alt", alt, 128);
                html_decode_entities(alt);
                if (alt[0]) {
                    char text[HTML_MAX_TEXT]; int tl = 0;
                    const char* pre = "[image: ";
                    for (int k = 0; pre[k]; k++) text[tl++] = pre[k];
                    for (int k = 0; alt[k] && tl < HTML_MAX_TEXT - 3; k++) text[tl++] = alt[k];
                    text[tl++] = ']'; text[tl] = 0;
                    add_token(tokens, &count, max_tokens, HTML_TEXT, text, "");
                }
                continue;
            }
            // <td>/<th>: one cell until its closing tag; consecutive cells
            // join on a row (okai render), </tr> breaks the line.
            if (tag_match(html, tag_start + 1, "td") ||
                tag_match(html, tag_start + 1, "th")) {
                int close = skip_to_close(html, pos, html_len,
                                          tag_name[0] == 't' && tag_name[1] == 'd' ? "td" : "th");
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < close && tl < HTML_MAX_TEXT - 1) {
                    if (html[pos] == '<') { // strip nested markup inside the cell
                        while (pos < close && html[pos] != '>') pos++;
                        if (pos < close) pos++;
                        if (tl && text[tl-1] != ' ') text[tl++] = ' ';
                    } else {
                        text[tl++] = html[pos++];
                    }
                }
                text[tl] = 0;
                while (tl > 0 && text[tl-1] == ' ') text[--tl] = 0; // trim
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_TABLE_CELL, text, "");
                pos = close;
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
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H1, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h2")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H2, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h3")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H3, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h4")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H4, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h5")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H5, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "h6")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_H6, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "li")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) {
                    // List marker: "• " or "N. " — inserted AFTER decoding
                    // (the bullet is a font slot byte, not raw page bytes).
                    char pre[8]; int pl = 0;
                    list_num++;
                    if (list_ordered) {
                        if (list_num >= 10) pre[pl++] = (char)('0' + (list_num / 10) % 10);
                        pre[pl++] = (char)('0' + list_num % 10);
                        pre[pl++] = '.'; pre[pl++] = ' ';
                    } else {
                        pre[pl++] = (char)0xC6; pre[pl++] = ' '; // •
                    }
                    if (tl + pl < HTML_MAX_TEXT) {
                        for (int k = tl; k >= 0; k--) text[k + pl] = text[k];
                        for (int k = 0; k < pl; k++) text[k] = pre[k];
                        tl += pl;
                    }
                    add_token(tokens, &count, max_tokens, HTML_LIST_ITEM, text, "");
                }
                continue;
            }
            // <p> / <div> become block tokens (so they receive box-model
            // styling). Their direct text is captured like <li>/<h1> do; nested
            // inline elements (<a>, <span>) remain separate tokens after it
            // (v1 limitation — no nesting inside a bordered block).
            if (tag_match(html, tag_start + 1, "p") ||
                tag_match(html, tag_start + 1, "div")) {
                int ttype = (html[tag_start + 1] == 'p') ? HTML_PARA : HTML_DIV;
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, ttype, text, "");
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
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_PRE, text, "");
                continue;
            }
            if (tag_match(html, tag_start + 1, "title")) {
                char text[HTML_MAX_TEXT]; int tl = 0;
                while (pos < html_len && html[pos] != '<') {
                    add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
                }
                text[tl] = 0;
                html_decode_entities(text);
                if (tl > 0) add_token(tokens, &count, max_tokens, HTML_TITLE, text, "");
                continue;
            }

            // Handle <a href="...">text</a>
            if (tag_match(html, tag_start + 1, "a")) {
                char href[256] = {0};
                extract_attr(html, tag_start + 1, html_len, "href", href, 256);
                html_decode_entities(href); // &amp; inside URLs is common
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
                html_decode_entities(text);
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
                add_char_to_text(text, &tl, HTML_MAX_TEXT, html[pos++]);
            }
            text[tl] = 0;
            html_decode_entities(text);
            if (text[0]) {
                add_token(tokens, &count, max_tokens, HTML_TEXT, text, "");
            }
        } else {
            pos++;
        }
    }

    return count;
}

// Concatenate every <style>...</style> block's body into `out`.
int html_extract_css(const char* html, int html_len, char* out, int cap) {
    int total = 0;
    out[0] = 0;
    int pos = 0;
    while (pos < html_len) {
        if (html[pos] == '<' && (html[pos+1] == 's' || html[pos+1] == 'S') &&
            tag_match(html, pos + 1, "style")) {
            // Find end of opening <style ...> tag
            int i = pos;
            while (i < html_len && html[i] != '>') i++;
            if (i >= html_len) break;
            int start = i + 1;
            // Find </style>
            int j = start;
            while (j < html_len && !(html[j] == '<' && html[j+1] == '/' &&
                                    tag_match(html, j + 2, "style")))
                j++;
            int chunk = j - start;
            if (chunk > 0 && total + chunk + 1 < cap) {
                memcpy(out + total, html + start, chunk);
                total += chunk;
                out[total] = 0;
            }
            pos = j + 1;
            continue;
        }
        pos++;
    }
    return total;
}
