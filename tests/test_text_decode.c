/* Host test for the text pipeline: charset decode (UTF-8 / CP1251),
   entity decode to font slots, list markers, img alt, table cells. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/html.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

static const char* tok_text(struct html_token* t, int n, int type, int k) {
    for (int i = 0; i < n; i++)
        if (t[i].type == type && k-- == 0) return t[i].text;
    return NULL;
}

int main(void) {
    // UTF-8 Cyrillic: "Привет мир" = П(0x417) р(0x440) и(0x438) в(0x432) е(0x435) т(0x442)
    {
        const char* html = "<html><body><p>\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80</p></body></html>";
        struct html_token t[8];
        int n = html_parse(html, strlen(html), t, 8);
        const char* txt = tok_text(t, n, HTML_TEXT, 0);
        CHECK(txt && (unsigned char)txt[0] == 0x8F && (unsigned char)txt[1] == 0xB0,
              "UTF-8 'П' decodes to slot 0x8F");
        // П = U+041F -> 0x80 + 0xF = 0x8F
        CHECK(txt && strlen(txt) == 10, "decoded length 10 ('Привет мир')");
    }

    // Numeric entities for Cyrillic + symbols
    {
        char s[] = "&#1055;&#1088;&#1080;&#1074;&#1077;&#1090; &mdash; &copy;";
        html_decode_entities(s);
        CHECK((unsigned char)s[0] == 0x8F, "&#1055; (П) -> slot 0x8F");
        CHECK((unsigned char)s[7] == 0xC2, "&mdash; -> slot 0xC2");
        CHECK((unsigned char)s[9] == 0xCB, "&copy; -> slot 0xCB");
    }


    // Hex entities
    {
        char s[] = "i&#x27;m &#x414;&#x430;";
        html_decode_entities(s);
        CHECK(s[0] == 'i' && s[1] == '\'' && s[2] == 'm' && s[3] == ' ',
              "&#x27; -> apostrophe");
        CHECK((unsigned char)s[4] == 0x84 && (unsigned char)s[5] == 0xA0,
              "&#x414;&#x430; -> Д а slots");
    }
    // CP1251 page (charset declared)
    {
        const char* html = "Content-Type: text/html; charset=windows-1251\r\n\r\n"
                           "<html><body><p>\xCF\xF0\xE8\xE2\xE5\xF2</p></body></html>";
        struct html_token t[8];
        int n = html_parse(html, strlen(html), t, 8);
        const char* txt = tok_text(t, n, HTML_TEXT, 0);
        // \xCF = П (0xC0 base) -> slot 0x8F; \xE5 = е -> U+0435 -> 0x95
        CHECK(txt && (unsigned char)txt[0] == 0x8F, "CP1251 П -> slot 0x8F");
        CHECK(txt && (unsigned char)txt[4] == 0xA5, "CP1251 е -> slot 0xA5");
    }

    // UTF-8 sniff default: no charset, high bytes treated as UTF-8
    {
        const char* html = "<html><body><p>\xD0\xAD</p></body></html>"; // Э
        struct html_token t[8];
        int n = html_parse(html, strlen(html), t, 8);
        const char* txt = tok_text(t, n, HTML_TEXT, 0);
        CHECK(txt && (unsigned char)txt[0] == 0x9D, "UTF-8 Э -> slot 0x9D (no charset decl)");
    }

    // Unordered list: bullet slot prefixed
    {
        const char* html = "<ul><li>one</li><li>two</li></ul>";
        struct html_token t[8];
        int n = html_parse(html, strlen(html), t, 8);
        const char* li = tok_text(t, n, HTML_LIST_ITEM, 0);
        CHECK(li && (unsigned char)li[0] == 0xC6 && li[1] == ' ' && li[2] == 'o',
              "ul li prefixed with '• '");
    }

    // Ordered list: numbers
    {
        const char* html = "<ol><li>first</li><li>second</li></ol>";
        struct html_token t[8];
        int n = html_parse(html, strlen(html), t, 8);
        const char* l1 = tok_text(t, n, HTML_LIST_ITEM, 0);
        const char* l2 = tok_text(t, n, HTML_LIST_ITEM, 1);
        CHECK(l1 && l1[0] == '1' && l1[1] == '.' && l1[2] == ' ', "ol li #1 = '1. '");
        CHECK(l2 && l2[0] == '2' && l2[1] == '.' && l2[2] == ' ', "ol li #2 = '2. '");
    }

    // img alt placeholder
    {
        const char* html = "<body><img src=\"x.png\" alt=\"logo\"></body>";
        struct html_token t[8];
        int n = html_parse(html, strlen(html), t, 8);
        const char* txt = tok_text(t, n, HTML_TEXT, 0);
        CHECK(txt && !strncmp(txt, "[image: logo]", 13), "img alt -> '[image: logo]'");
    }

    // Table cells: tokens carry cell text; </tr> yields END_PARA
    {
        const char* html = "<table><tr><td>a1</td><td>b1</td></tr><tr><td>a2</td></tr></table>";
        struct html_token t[16];
        int n = html_parse(html, strlen(html), t, 16);
        const char* c0 = tok_text(t, n, HTML_TABLE_CELL, 0);
        const char* c1 = tok_text(t, n, HTML_TABLE_CELL, 1);
        const char* c2 = tok_text(t, n, HTML_TABLE_CELL, 2);
        int endparas = 0;
        for (int i = 0; i < n; i++) if (t[i].type == HTML_END_PARA) endparas++;
        CHECK(c0 && !strcmp(c0, "a1"), "table cell 0 = 'a1'");
        CHECK(c1 && !strcmp(c1, "b1"), "table cell 1 = 'b1'");
        CHECK(c2 && !strcmp(c2, "a2"), "table cell 2 = 'a2'");
        CHECK(endparas >= 2, "row closes emit END_PARA");
    }

    // hr / blockquote tokens
    {
        const char* html = "<p>x</p><hr><blockquote>quote</blockquote><p>y</p>";
        struct html_token t[16];
        int n = html_parse(html, strlen(html), t, 16);
        int hr = 0, brk = 0;
        for (int i = 0; i < n; i++) {
            if (t[i].type == HTML_BLOCK) hr++;
            if (t[i].type == HTML_LINE_BREAK) brk++;
        }
        CHECK(hr == 1, "hr -> HTML_BLOCK");
        CHECK(brk >= 1, "blockquote -> LINE_BREAK");
    }

    printf(fails ? "TEXT_DECODE FAIL (%d)\n" : "TEXT_DECODE PASS\n", fails);
    return fails ? 1 : 0;
}
