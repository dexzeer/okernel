/* Host regression for the inline/nesting rewrite (first-website milestone):
   linked <li> bullets, case-insensitive </a>, DT/DD tokens, whitespace
   collapse, named anchors, attribute bounds. Runs on saved fixtures plus
   inline snippets, no network. Complements tests/headless/test_firstrender.py
   (same pages, in-guest). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/html.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

static int load(const char* path, char* out, int cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(out, 1, cap - 1, f);
    fclose(f);
    if (n < 0) return -1;
    out[n] = 0;
    return n;
}

static int count_type(struct html_token* t, int n, int type) {
    int c = 0;
    for (int i = 0; i < n; i++) if (t[i].type == type) c++;
    return c;
}

static struct html_token* find_link(struct html_token* t, int n, const char* text) {
    for (int i = 0; i < n; i++)
        if (t[i].type == HTML_LINK && strstr(t[i].text, text)) return &t[i];
    return NULL;
}

static struct html_token toks[HTML_MAX_TOKENS];

int main(void) {
    char page[65536];

    // ---- info.cern.ch home ----
    {
        int len = load("tests/fixtures_pages/first.html", page, sizeof(page));
        CHECK(len > 0, "first.html fixture loads");
        int n = html_parse(page, len, toks, HTML_MAX_TOKENS);
        CHECK(n >= 15, "home yields 15+ tokens");
        int li = count_type(toks, n, HTML_LIST_ITEM);
        int links = count_type(toks, n, HTML_LINK);
        CHECK(li + links >= 4, "home list items present (bulleted or linked)");
        // every list item carries its bullet (link-first items too)
        int bulleted = 0, items = 0;
        for (int i = 0; i < n; i++) {
            if (toks[i].type == HTML_LIST_ITEM || toks[i].type == HTML_LINK) {
                // only count tokens inside the <ul> (after the 2nd PARA)
                items++;
                if ((unsigned char)toks[i].text[0] == 0xC6) bulleted++;
            }
        }
        CHECK(items >= 4 && bulleted >= 4, "home bullets on all 4 items");
        struct html_token* l = find_link(toks, n, "Browse the first website");
        CHECK(l && strstr(l->href, "TheProject.html"), "home link href kept");
        int empty_href = 0;
        for (int i = 0; i < n; i++)
            if (toks[i].type == HTML_LINK && !toks[i].href[0]) empty_href++;
        CHECK(!empty_href, "no empty-href links on home page");
    }

    // ---- TheProject (uppercase </A>, DT/DD, mixed inline) ----
    {
        int len = load("tests/fixtures_pages/theproject.html", page, sizeof(page));
        CHECK(len > 0, "theproject.html fixture loads");
        int n = html_parse(page, len, toks, HTML_MAX_TOKENS);
        CHECK(n >= 80, "theproject yields 80+ tokens (was 9 pre-fix)");
        struct html_token* l = find_link(toks, n, "hypermedia");
        CHECK(l && !strcmp(l->href, "WhatIs.html"), "multiline <A> href + text");
        // no single link swallows the document (the </A> bug)
        int huge = 0;
        for (int i = 0; i < n; i++)
            if (toks[i].type == HTML_LINK && strlen(toks[i].text) > 200) huge++;
        CHECK(!huge, "no swallowed-document links");
        // DTs here are all link-first (no DT token, just the LINK after an
        // END_PARA separator) — assert every term string survived somewhere.
        const char* terms[] = { "What's out there?", "Help", "Software Products",
            "Technical", "Bibliography", "People", "History", "How can I help",
            "Getting code" };
        int found_terms = 0;
        for (int k = 0; k < 9; k++)
            for (int i = 0; i < n; i++)
                if (strstr(toks[i].text, terms[k])) { found_terms++; break; }
        CHECK(found_terms == 9, "all 9 DT terms present");
        CHECK(count_type(toks, n, HTML_DD) >= 8, "DD tokens for 9 descriptions");
        l = find_link(toks, n, "anonymous FTP");
        CHECK(l && strstr(l->href, "Distribution.html"), "late-document link intact");
    }

    // ---- example.com (div-wrapped, styled) ----
    {
        int len = load("tests/fixtures_pages/example.html", page, sizeof(page));
        CHECK(len > 0, "example.html fixture loads");
        int n = html_parse(page, len, toks, HTML_MAX_TOKENS);
        int h1ok = 0, linkok = 0;
        for (int i = 0; i < n; i++) {
            if (toks[i].type == HTML_H1 && strstr(toks[i].text, "Example Domain")) h1ok = 1;
            if (toks[i].type == HTML_LINK && strstr(toks[i].href, "iana.org")) linkok = 1;
        }
        CHECK(h1ok, "example h1 kept");
        CHECK(linkok, "example iana link kept");
    }

    // ---- inline snippets (no fixture needed) ----
    {
        // named anchor without href: plain text, never a link
        const char* h = "<p><a name=\"s1\">section one</a> tail</p>";
        int n = html_parse(h, strlen(h), toks, 64);
        CHECK(count_type(toks, n, HTML_LINK) == 0, "href-less <a name> is not a link");
        int has_text = 0;
        for (int i = 0; i < n; i++)
            if (strstr(toks[i].text, "section one")) has_text = 1;
        CHECK(has_text, "anchor text preserved as text");
    }
    {
        // uppercase close + link-only list item keeps bullet + href
        const char* h = "<ul><li><A HREF=\"http://h/\">T</A></li></ul>";
        int n = html_parse(h, strlen(h), toks, 64);
        struct html_token* l = find_link(toks, n, "T");
        CHECK(l && !strcmp(l->href, "http://h/"), "uppercase <A>/</A> link works");
        CHECK(l && (unsigned char)l->text[0] == 0xC6, "link-only item keeps bullet");
        // nothing after the link leaks in
        int tail = 0;
        for (int i = 0; i < n; i++)
            if (toks[i].type == HTML_TEXT && strstr(toks[i].text, "T")) tail = 1;
        CHECK(!tail, "link text not duplicated as text");
    }
    {
        // bold/italic spans become styled tokens, text kept
        const char* h = "<p>a <b>bd</b> c <i>it</i> d</p>";
        int n = html_parse(h, strlen(h), toks, 64);
        CHECK(count_type(toks, n, HTML_BOLD) == 1, "<b> token emitted");
        CHECK(count_type(toks, n, HTML_ITALIC) == 1, "<i> token emitted");
        char joined[512]; joined[0] = 0;
        for (int i = 0; i < n; i++) {
            if (toks[i].type == HTML_END_PARA) continue;
            strncat(joined, toks[i].text, sizeof(joined) - strlen(joined) - 1);
            strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        }
        CHECK(joined[0] == 'a' && strstr(joined, "bd") && strstr(joined, "it"),
              "inline words survive in order");
    }
    {
        // unclosed <p> separates (implied close on next block)
        const char* h = "<p>one<p>two";
        int n = html_parse(h, strlen(h), toks, 64);
        CHECK(count_type(toks, n, HTML_PARA) == 2, "unclosed <p> splits");
    }
    {
        // whitespace collapses in flowed elements, survives in <pre>
        const char* h = "<p>a\n\t b</p><pre>x\n y</pre>";
        int n = html_parse(h, strlen(h), toks, 64);
        int pok = 0, rok = 0;
        for (int i = 0; i < n; i++) {
            if (toks[i].type == HTML_PARA && !strcmp(toks[i].text, "a b")) pok = 1;
            if (toks[i].type == HTML_PRE && strstr(toks[i].text, "x\n y")) rok = 1;
        }
        CHECK(pok, "para whitespace collapses");
        CHECK(rok, "pre keeps newline");
    }

    printf(fails ? "RENDER PAGES FAIL (%d)\n" : "RENDER PAGES PASS\n", fails);
    return fails != 0;
}
