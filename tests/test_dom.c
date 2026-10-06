/* Host test for the DOM tree builder (src/dom.c): tokenizer states, implied
   end tags, void/raw-text elements, attributes, charset/entity decoding.
   Run: gcc -DKERNEL=0 -I../src -include string.h test_dom.c ../src/dom.c
        ../src/html.c -o t && ./t */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/dom.h"
#include "../src/html.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

static struct dom doc;

static void dump_tree(const struct dom* d, int node, int depth) {
    if (depth > 12) return;
    for (int i = 0; i < depth; i++) printf("  ");
    const struct dom_node* n = &d->nodes[node];
    if (n->type == DOM_NODE_TEXT) {
        char buf[256];
        dom_text_copy(d, node, buf, sizeof(buf));
        printf("#text \"%s\"%s\n", buf, (n->flags & DOM_F_WS) ? " [ws]" : "");
    } else if (n->type == DOM_NODE_ELEMENT) {
        char tag[32];
        dom_tag_copy(d, node, tag, sizeof(tag));
        printf("<%s>", tag);
        for (int a = n->attr_head; a != DOM_NONE; a = d->attrs[a].next) {
            printf(" %.*s=%.*s", d->attrs[a].name_len,
                   d->names + d->attrs[a].name_off, d->attrs[a].val_len,
                   d->text + d->attrs[a].val_off);
        }
        printf("\n");
    } else {
        printf("#root\n");
    }
    for (int c = n->first_child; c != DOM_NONE; c = d->nodes[c].next_sib)
        dump_tree(d, c, depth + 1);
}

static int parse(const char* html) {
    int r = dom_build(&doc, html, (int)strlen(html));
    if (getenv("DOM_DUMP")) dump_tree(&doc, r, 0);
    return r;
}

static int child_tag(int parent, const char* tag) {
    for (int c = doc.nodes[parent].first_child; c != DOM_NONE; c = doc.nodes[c].next_sib)
        if (dom_tag_is(&doc, c, tag)) return c;
    return -1;
}

static int count_tag(const char* tag) {
    int n = 0;
    for (int i = 0; i < doc.node_count; i++)
        if (doc.nodes[i].type == DOM_NODE_ELEMENT && dom_tag_is(&doc, i, tag)) n++;
    return n;
}

int main(void) {
    // --- basic nesting + attrs ---
    {
        int r = parse("<div class=\"a b\" id=x><p>Hello <b>world</b>!</p></div>");
        CHECK(r >= 0, "basic parse returns root");
        int div = child_tag(r, "div");
        CHECK(div >= 0, "div element created");
        char buf[64];
        CHECK(dom_attr_get(&doc, div, "class", buf, sizeof(buf)) == 3 &&
              !strcmp(buf, "a b"), "class attribute decoded");
        CHECK(dom_attr_get(&doc, div, "id", buf, sizeof(buf)) == 1 &&
              !strcmp(buf, "x"), "unquoted attribute decoded");
        int p = child_tag(div, "p");
        int b = p >= 0 ? child_tag(p, "b") : -1;
        CHECK(b >= 0, "b nested under p");
        CHECK(count_tag("p") == 1, "one p");
    }

    // --- implied end tags ---
    {
        parse("<ul><li>one<li>two<li>three</ul>");
        CHECK(count_tag("li") == 3, "unclosed <li> implies close");
        parse("<p>one<p>two");
        CHECK(count_tag("p") == 2, "unclosed <p> implies close");
        parse("<table><tr><td>a<td>b<tr><td>c</table>");
        CHECK(count_tag("td") == 3 && count_tag("tr") == 2, "td/tr implied closes");
    }

    // --- void + raw text ---
    {
        parse("<p>a<br>b<img src=x alt=pic></p><script>if(1<2){x=\"</p>\"}</script>");
        int n = count_tag("br");
        CHECK(n == 1, "<br> is void and present");
        CHECK(count_tag("script") == 1, "script element present");
        int sc = -1;
        for (int i = 0; i < doc.node_count; i++)
            if (dom_tag_is(&doc, i, "script")) sc = i;
        int tl = 0;
        const char* t = dom_text(&doc, doc.nodes[sc].first_child, &tl);
        CHECK(t && tl > 0 && strstr(t, "</p>") != NULL,
              "script body kept raw (close-tag text inside)");
    }

    // --- attribute with '>' inside a quote ---
    {
        parse("<a href=\"a?x=1>2\" title='q'>t</a>");
        int a = dom_first_tag(&doc, "a");
        char buf[64];
        CHECK(a >= 0 && dom_attr_get(&doc, a, "href", buf, sizeof(buf)) >= 0 &&
              !strcmp(buf, "a?x=1>2"), "quoted attr keeps '>'");
    }

    // --- entities + charset ---
    {
        parse("<p>a &amp; b &#x41; &#66; &nbsp;c</p>");
        int p = dom_first_tag(&doc, "p");
        int tn = p >= 0 ? doc.nodes[p].first_child : -1;
        char buf[128];
        dom_text_copy(&doc, tn, buf, sizeof(buf));
        CHECK(!strcmp(buf, "a & b A B  c"), "entities decoded in text");
    }

    // --- comments, doctype, CDATA ---
    {
        parse("<!doctype html><!-- xx --><p>a<![CDATA[<b>]]>b</p>");
        CHECK(count_tag("b") == 0, "CDATA not parsed as markup");
        int p = dom_first_tag(&doc, "p");
        int tn = p >= 0 ? doc.nodes[p].first_child : -1;
        char buf[128];
        dom_text_copy(&doc, tn, buf, sizeof(buf));
        CHECK(!strcmp(buf, "a<b>b"), "CDATA text preserved");
    }

    // --- misnested formatting: text must not be lost ---
    {
        parse("<b>bold<p>para</b>tail</p>");
        int tl = 0; (void)tl;
        int found_bold = 0, found_para = 0, found_tail = 0;
        for (int i = 0; i < doc.node_count; i++) {
            if (doc.nodes[i].type != DOM_NODE_TEXT) continue;
            char buf[64];
            dom_text_copy(&doc, i, buf, sizeof(buf));
            if (strstr(buf, "bold")) found_bold = 1;
            if (strstr(buf, "para")) found_para = 1;
            if (strstr(buf, "tail")) found_tail = 1;
        }
        CHECK(found_bold && found_para && found_tail, "misnested tags lose no text");
    }

    // --- dom_attr_set (form-field editing) ---
    {
        parse("<div><input name=\"q\" value=\"hi\"></div>");
        int inp = -1;
        for (int i = 0; i < doc.node_count; i++) {
            char t[16];
            dom_tag_copy(&doc, i, t, sizeof(t));
            if (!strcmp(t, "input")) { inp = i; break; }
        }
        CHECK(inp >= 0, "input node found");
        char v[64];
        CHECK(dom_attr_get(&doc, inp, "value", v, sizeof(v)) == 2 && !strcmp(v, "hi"),
              "initial value reads");
        CHECK(dom_attr_set(&doc, inp, "value", "hello world") == 0,
              "overwrite value");
        CHECK(dom_attr_get(&doc, inp, "value", v, sizeof(v)) == 11 && !strcmp(v, "hello world"),
              "edited value reads back");
        CHECK(dom_attr_set(&doc, inp, "value", "") == 0 &&
              dom_attr_get(&doc, inp, "value", v, sizeof(v)) == 0,
              "clear value");
        CHECK(dom_attr_set(&doc, inp, "placeholder", "ph") == 0 &&
              dom_attr_get(&doc, inp, "placeholder", v, sizeof(v)) == 2,
              "add brand-new attribute");
        CHECK(dom_attr_set(&doc, -1, "value", "x") != 0, "bad node rejected");
        CHECK(dom_attr_set(&doc, inp, "value", v) == 0, "self-overwrite ok");
    }

    // --- fixture pages ---
    {
        FILE* f = fopen("fixtures_pages/theproject.html", "rb");
        if (!f) f = fopen("tests/fixtures_pages/theproject.html", "rb");
        if (f) {
            static char page[65536];
            int n = (int)fread(page, 1, sizeof(page) - 1, f);
            fclose(f);
            page[n] = 0;
            parse(page);
            CHECK(count_tag("a") >= 8, "theproject has links");
            CHECK(doc.truncated == 0, "theproject fits the arenas");
        } else {
            printf("SKIP: theproject fixture not found\n");
        }
    }

    printf(fails ? "DOM FAIL (%d)\n" : "DOM PASS\n", fails);
    return fails != 0;
}