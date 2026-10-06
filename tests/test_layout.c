/* Host test for the layout engine (src/layout.c): block separation, inline
   wrapping, list markers, headings, links as runs. Uses the real DOM + CSS
   (incl. the UA sheet). Run: make host-tests (t_layout) or
   cd tests && gcc -m32 -DKERNEL=0 -I../src -o t test_layout.c ../src/dom.c
   ../src/html.c ../src/css.c ../src/layout.c */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/dom.h"
#include "../src/layout.h"
#include "../src/css.h"
#include "../src/html.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

static struct dom doc;
static struct layout lay;
static struct css_rule rules[CSS_MAX_RULES];

static int build(const char* html) {
    int n = (int)strlen(html);
    dom_build(&doc, html, n);
    int cn = css_parse("", 0, rules, CSS_MAX_RULES);
    const struct css_rule* ua = 0;
    int uan = css_ua_rules(&ua);
    static struct css_rule comb[CSS_MAX_RULES + CSS_UA_MAX_RULES];
    int ccn = 0;
    for (int i = 0; i < uan; i++) comb[ccn++] = ua[i];
    for (int i = 0; i < cn; i++) comb[ccn++] = rules[i];
    struct layout_opts o;
    memset(&o, 0, sizeof(o));
    o.width_cols = 40; o.page_left = 2; o.page_bg = 0xFFFFFF; o.page_fg = 0x000000;
    layout_run(&doc, comb, ccn, &o, &lay);
    return lay.height;
}

static int item_runs_text(int item, char* out, int cap) {
    out[0] = 0;
    struct layout_item* it = &lay.items[item];
    int n = 0;
    for (int ri = it->run_start; ri < it->run_start + it->run_count; ri++) {
        for (int k = 0; k < lay.runs[ri].text_len && n < cap - 1; k++)
            out[n++] = lay.text[lay.runs[ri].text_off + k];
    }
    out[n] = 0;
    return n;
}

static int count_heading(int level) {
    int c = 0;
    for (int i = 0; i < lay.n_items; i++)
        if (lay.items[i].kind == LOUT_LINE && lay.items[i].heading == level) c++;
    return c;
}

static int count_link_runs(void) {
    int c = 0;
    for (int i = 0; i < lay.n_runs; i++) if (lay.runs[i].is_link) c++;
    return c;
}

int main(void) {
    { // block separation: two <p> occupy distinct document rows
        build("<p>alpha</p><p>beta</p>");
        char t[128];
        int row_a = -1, row_b = -1;
        for (int i = 0; i < lay.n_items; i++) {
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "alpha")) row_a = lay.items[i].row;
            if (strstr(t, "beta")) row_b = lay.items[i].row;
        }
        CHECK(row_a >= 0 && row_b >= 0 && row_b > row_a, "paragraphs on separate rows");
    }
    { // inline wrapping: a long paragraph wraps into multiple line items
        build("<p>the quick brown fox jumps over the lazy dog again and again</p>");
        int lines = 0;
        for (int i = 0; i < lay.n_items; i++)
            if (lay.items[i].kind == LOUT_LINE && lay.items[i].run_count) lines++;
        CHECK(lines >= 2, "long paragraph wraps to multiple lines");
        // no line exceeds the content width
        int over = 0;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            for (int ri = lay.items[i].run_start; ri < lay.items[i].run_start + lay.items[i].run_count; ri++)
                if (lay.runs[ri].col + lay.runs[ri].text_len > 2 + 40) over++;
        }
        CHECK(!over, "no run overflows the content width");
    }
    { // heading flagged and scaled
        build("<h1>Big</h1><h2>Medium</h2><h3>Small</h3>");
        CHECK(count_heading(1) == 1, "h1 flagged heading level 1");
        CHECK(count_heading(2) == 1, "h2 flagged heading level 2");
        CHECK(count_heading(3) == 1, "h3 flagged heading level 3");
    }
    { // unclosed list items still yield markers
        build("<ul><li>one<li>two<li>three</ul>");
        int bullets = 0;
        for (int i = 0; i < lay.n_runs; i++) {
            for (int k = 0; k < lay.runs[i].text_len; k++)
                if ((unsigned char)lay.text[lay.runs[i].text_off + k] == 0xC6) bullets++;
        }
        CHECK(bullets >= 3, "each <li> gets a bullet marker");
    }
    { // ordered list numbers
        build("<ol><li>a</li><li>b</li></ol>");
        int onedot = 0;
        for (int i = 0; i < lay.n_runs; i++)
            for (int k = 0; k + 1 < lay.runs[i].text_len; k++)
                if (lay.text[lay.runs[i].text_off + k] == '1' &&
                    lay.text[lay.runs[i].text_off + k + 1] == '.') onedot++;
        CHECK(onedot >= 1, "ordered list emits '1. ' marker");
    }
    { // links become link runs with a resolvable node
        build("<p>go <a href=\"http://x/\">here</a> now</p>");
        CHECK(count_link_runs() >= 1, "link text emitted as link runs");
        int found = 0;
        for (int i = 0; i < lay.n_runs; i++) {
            if (!lay.runs[i].is_link) continue;
            char href[64];
            if (dom_attr_get(&doc, lay.runs[i].node, "href", href, sizeof(href)) >= 0 &&
                !strcmp(href, "http://x/")) found = 1;
        }
        CHECK(found, "link run carries the source <a> node/href");
    }
    { // <pre> keeps literal newlines as separate rows
        build("<pre>a\nb\nc</pre>");
        int rows = 0;
        for (int i = 0; i < lay.n_items; i++)
            if (lay.items[i].kind == LOUT_LINE && lay.items[i].run_count) rows++;
        CHECK(rows >= 3, "preformatted newlines produce separate rows");
    }
    { // position:relative shifts paint rows, keeps flow height
        build("<p>one</p><p>two</p>");
        char t[128];
        int r0 = -1, h0 = lay.height;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "one")) r0 = lay.items[i].row;
        }
        build("<p style=\"position:relative;top:64px\">one</p><p>two</p>");
        int r1 = -1;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "one")) r1 = lay.items[i].row;
        }
        CHECK(r0 >= 0 && r1 == r0 + 2, "relative top:64px shifts row +2");
        CHECK(lay.height == h0, "relative keeps document height");
    }
    { // position:absolute takes no flow space; document grows to cover it
        build("<p style=\"position:absolute;top:160px\">one</p><p>two</p>");
        char t[128];
        int ra = -1, rs = -1;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "one")) ra = lay.items[i].row;
            if (strstr(t, "two")) rs = lay.items[i].row;
        }
        CHECK(ra >= 0 && rs >= 0, "absolute box and sibling both laid out");
        CHECK(rs < ra, "sibling not pushed by the absolute offset");
        CHECK(lay.height >= ra + 1, "document grows to cover abs box");
    }
    { // position:fixed pins to viewport rows, takes no space, flagged oof
        build("<p>body</p>");
        char t[128];
        int rb = -1, hb = lay.height;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "body")) rb = lay.items[i].row;
        }
        build("<div style=\"position:fixed;top:0px;left:0px\">hdr</div><p>body</p>");
        int ih = -1, rb2 = -1;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "hdr")) ih = i;
            if (strstr(t, "body")) rb2 = lay.items[i].row;
        }
        CHECK(ih >= 0 && lay.items[ih].row == 0 && lay.items[ih].is_fixed,
              "fixed top:0 pins to viewport row 0 and is flagged");
        CHECK(ih >= 0 && lay.items[ih].is_oof, "fixed paints out-of-flow (on top)");
        CHECK(rb2 == rb && lay.height == hb, "fixed takes no flow space");
        build("<p style=\"position:absolute;top:0px\">ab</p>");
        int ia = -1;
        for (int i = 0; i < lay.n_items; i++) {
            if (lay.items[i].kind != LOUT_LINE) continue;
            item_runs_text(i, t, sizeof(t));
            if (strstr(t, "ab")) ia = i;
        }
        CHECK(ia >= 0 && !lay.items[ia].is_fixed && lay.items[ia].is_oof,
              "absolute is out-of-flow but not viewport-pinned");
    }
    { // form controls flag clickable runs (mouse hit-testing needs is_field)
        build("<form action=\"/s\"><input name=\"q\" value=\"ab\"><input type=\"submit\" value=\"Go\"></form>");
        int fields = 0;
        for (int i = 0; i < lay.n_runs; i++)
            if (lay.runs[i].is_field) fields++;
        CHECK(fields >= 2, "input + submit produce field runs");
        build("<p>plain</p>");
        int leaked = 0;
        for (int i = 0; i < lay.n_runs; i++) if (lay.runs[i].is_field) leaked++;
        CHECK(leaked == 0, "plain text has no field runs");
    }
    { // empty document still yields a valid 1+ row layout
        struct dom empty;
        dom_reset(&empty);
        struct layout_opts o; memset(&o, 0, sizeof(o));
        o.width_cols = 30; o.page_bg = 0; o.page_fg = 0xFFFFFF;
        struct layout L;
        layout_run(&empty, 0, 0, &o, &L);
        CHECK(L.height >= 1, "empty document yields a 1-row layout");
    }
    printf(fails ? "LAYOUT FAIL (%d)\n" : "LAYOUT PASS\n", fails);
    return fails != 0;
}