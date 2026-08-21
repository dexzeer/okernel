// From-scratch CSS engine host test (no QEMU): parser, cascade, color quantize,
// inline override, and <style> extraction from HTML.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "css.h"
#include "html.h"

static int fails = 0;

static void ok(const char* name, int cond) {
    printf("%-40s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main(void) {
    struct css_rule rules[CSS_MAX_RULES];

    // ---- parser: selector kinds + specificity ----
    const char* css1 =
        "p { color: red; }\n"
        ".lead { color: blue; }\n"
        "#main { color: green; }\n"
        "div.box { color: white; }\n";
    int n = css_parse(css1, (int)strlen(css1), rules, CSS_MAX_RULES);
    ok("parse 4 rules", n == 4);
    ok("specificity tag=1",    rules[0].specificity == 1);
    ok("specificity class=10", rules[1].specificity == 10);
    ok("specificity id=100",    rules[2].specificity == 100);
    ok("specificity tag.class=11", rules[3].specificity == 11);
    ok("tag.class subject is tag", rules[3].sel.kind == CSS_SEL_TAG && strcmp(rules[3].sel.value, "div") == 0);
    ok("tag.class extra class", strcmp(rules[3].sel_class, "box") == 0);

    // ---- cascade: higher specificity wins regardless of order ----
    struct css_style s;
    css_compute(rules, n, "p", "lead", NULL, NULL, &s);
    ok("class beats tag (.lead blue->1)", s.has_fg && s.fg == 1); // blue=1

    // ---- cascade: equal specificity, later wins ----
    const char* css2 = ".a { color: blue; }\n.a { color: red; }\n";
    struct css_rule r2[CSS_MAX_RULES];
    int n2 = css_parse(css2, (int)strlen(css2), r2, CSS_MAX_RULES);
    struct css_style s2;
    css_compute(r2, n2, NULL, "a", NULL, NULL, &s2);
    ok("equal-spec later wins (red)", s2.has_fg && s2.fg == 4);

    // ---- cascade: id beats class ----
    struct css_style s3;
    css_compute(rules, n, "p", "lead", "main", NULL, &s3);
    ok("id beats class (green)", s3.has_fg && s3.fg == 2); // green=2

    // ---- inline style overrides rules ----
    struct css_style s4;
    css_compute(rules, n, "p", "lead", NULL, "color: green;", &s4);
    ok("inline overrides rule (green)", s4.has_fg && s4.fg == 2);

    // ---- color quantization to 16-color palette ----
    const char* css3 = "a { color: #ff0000; }\nb { color: #00ff00; }\n"
                       "c { color: #ffffff; }\nd { color: black; }\n";
    struct css_rule r3[CSS_MAX_RULES];
    int n3 = css_parse(css3, (int)strlen(css3), r3, CSS_MAX_RULES);
    struct css_style sc[4];
    for (int i = 0; i < 4; i++) css_compute(r3, n3, (const char*[]){"a","b","c","d"}[i], NULL, NULL, NULL, &sc[i]);
    ok("#ff0000 -> red(4)",   sc[0].has_fg && sc[0].fg == 4);
    ok("#00ff00 -> green(2)", sc[1].has_fg && sc[1].fg == 2);
    ok("#ffffff -> white(15)",sc[2].has_fg && sc[2].fg == 15);
    ok("named black -> 0",    sc[3].has_fg && sc[3].fg == 0);

    // ---- background, align, display, margin ----
    const char* css4 = ".hdr { background: yellow; text-align: center; display: block; margin: 10px; }\n";
    struct css_rule r4[CSS_MAX_RULES];
    int n4 = css_parse(css4, (int)strlen(css4), r4, CSS_MAX_RULES);
    struct css_style s5;
    css_compute(r4, n4, NULL, "hdr", NULL, NULL, &s5);
    ok("bg yellow(14)", s5.has_bg && s5.bg == 14);
    ok("align center",  s5.has_align && s5.align == CSS_ALIGN_CENTER);
    ok("display block", s5.has_display && s5.display == CSS_DISPLAY_BLOCK);
    ok("margin top",    s5.has_mt && s5.margin_top == 10);
    ok("margin bottom (shorthand)", s5.has_mb && s5.margin_bottom == 10);

    // ---- display:none hides element ----
    const char* css5 = ".hidden { display: none; }\n";
    struct css_rule r5[CSS_MAX_RULES];
    int n5 = css_parse(css5, (int)strlen(css5), r5, CSS_MAX_RULES);
    struct css_style s6;
    css_compute(r5, n5, NULL, "hidden", NULL, NULL, &s6);
    ok("display none", s6.has_display && s6.display == CSS_DISPLAY_NONE);

    // ---- html_extract_css pulls <style> blocks ----
    const char* page =
        "<html><head><style> p { color: red; } </style></head>"
        "<body><style> a { color: blue; } </style></body>";
    char cssbuf[256];
    int cn = html_extract_css(page, (int)strlen(page), cssbuf, sizeof(cssbuf));
    ok("extract non-empty", cn > 0);
    ok("extract p rule", strstr(cssbuf, "color: red") != NULL);
    ok("extract a rule", strstr(cssbuf, "color: blue") != NULL);

    // ---- css_merge_base: body-level inheritance (centered text, T18) ----
    // The flat renderer has no DOM tree, so it folds <body> styling into every
    // element that doesn't set the property itself.
    const char* css6 =
        "body { text-align: center; color: #ff0000; }\n"
        "h1 { color: blue; }\n";
    struct css_rule r6[CSS_MAX_RULES];
    int n6 = css_parse(css6, (int)strlen(css6), r6, CSS_MAX_RULES);
    struct css_style body_style;
    css_compute(r6, n6, "body", 0, 0, 0, &body_style);
    ok("body: align center", body_style.has_align && body_style.align == CSS_ALIGN_CENTER);
    ok("body: color set", body_style.has_fg);

    // An element matching no rule inherits both color and alignment from body.
    struct css_style s7;
    css_compute(r6, n6, "div", 0, 0, 0, &s7);
    css_merge_base(&s7, &body_style);
    ok("div inherits body align", s7.has_align && s7.align == CSS_ALIGN_CENTER);
    ok("div inherits body color", s7.has_fg);

    // An element with its own rule keeps it, but still inherits unset fields.
    struct css_style s8;
    css_compute(r6, n6, "h1", 0, 0, 0, &s8);
    css_merge_base(&s8, &body_style);
    ok("h1 keeps own color (blue=1)", s8.has_fg && s8.fg == 1);
    ok("h1 inherits body align", s8.has_align && s8.align == CSS_ALIGN_CENTER);

    printf("\n%s: %d failures\n", fails == 0 ? "ALL PASS" : "FAILURES", fails);
    return fails ? 1 : 0;
}
