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
    static struct css_rule rules[CSS_MAX_RULES];

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
    ok("tag.class subject is tag", rules[3].chain[0].tag[0] &&
       strcmp(rules[3].chain[0].tag, "div") == 0);
    ok("tag.class extra class", rules[3].chain[0].ncls == 1 &&
       strcmp(rules[3].chain[0].cls[0], "box") == 0);

    // ---- cascade: higher specificity wins regardless of order ----
    struct css_style s;
    css_compute(rules, n, "p", "lead", NULL, NULL, &s);
    ok("class beats tag (.lead blue->1)", s.has_fg && s.fg == 1); // blue=1

    // ---- cascade: equal specificity, later wins ----
    const char* css2 = ".a { color: blue; }\n.a { color: red; }\n";
    static struct css_rule r2[CSS_MAX_RULES];
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
    static struct css_rule r3[CSS_MAX_RULES];
    int n3 = css_parse(css3, (int)strlen(css3), r3, CSS_MAX_RULES);
    struct css_style sc[4];
    for (int i = 0; i < 4; i++) css_compute(r3, n3, (const char*[]){"a","b","c","d"}[i], NULL, NULL, NULL, &sc[i]);
    ok("#ff0000 -> red(4)",   sc[0].has_fg && sc[0].fg == 4);
    ok("#00ff00 -> green(2)", sc[1].has_fg && sc[1].fg == 2);
    ok("#ffffff -> white(15)",sc[2].has_fg && sc[2].fg == 15);
    ok("named black -> 0",    sc[3].has_fg && sc[3].fg == 0);

    // ---- background, align, display, margin ----
    const char* css4 = ".hdr { background: yellow; text-align: center; display: block; margin: 10px; }\n";
    static struct css_rule r4[CSS_MAX_RULES];
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
    static struct css_rule r5[CSS_MAX_RULES];
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
    static struct css_rule r6[CSS_MAX_RULES];
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

    // ---- rgb()/rgba() functional colors ----
    const char* css7 = "p { color: rgb(255,0,0); } q { color: rgba(0,128,0,1); }\n"
                       "z { color: rgba(0,0,0,0); } w { color: rgb(100%,0%,0%); }\n";
    static struct css_rule r7[CSS_MAX_RULES];
    int n7 = css_parse(css7, (int)strlen(css7), r7, CSS_MAX_RULES);
    struct css_style s9, s10, s11, s12;
    css_compute(r7, n7, "p", 0, 0, 0, &s9);
    css_compute(r7, n7, "q", 0, 0, 0, &s10);
    css_compute(r7, n7, "z", 0, 0, 0, &s11);
    css_compute(r7, n7, "w", 0, 0, 0, &s12);
    ok("rgb() red", s9.has_fg && s9.fg == 4);
    ok("rgb() exact rgb", s9.has_fg_rgb && s9.fg_rgb == 0xFF0000);
    ok("rgba() opaque green", s10.has_fg && s10.fg == 2);
    ok("rgba() transparent dropped", !s11.has_fg);
    ok("rgb() percent red", s12.has_fg && s12.fg == 4);

    // ---- !important ----
    const char* css8 = "p { color: red !important; } p { color: blue; }\n"
                       "q { color: green; } q { color: yellow !important; }\n";
    static struct css_rule r8[CSS_MAX_RULES];
    int n8 = css_parse(css8, (int)strlen(css8), r8, CSS_MAX_RULES);
    struct css_style s13, s14;
    css_compute(r8, n8, "p", 0, 0, 0, &s13);
    css_compute(r8, n8, "q", 0, 0, 0, &s14);
    ok("!important beats later rule", s13.has_fg && s13.fg == 4);
    ok("later !important wins", s14.has_fg && s14.fg == 14);
    struct css_style s15;
    css_compute(r8, n8, "p", 0, 0, "color: green;", &s15);
    ok("!important beats inline", s15.has_fg && s15.fg == 4);

    // ---- relative units + keywords ----
    const char* css9 = "p { font-size: 1.5em; width: 50%; margin: -5px 2em; }\n"
                       "q { font-size: larger; border-width: thick; }\n";
    static struct css_rule r9[CSS_MAX_RULES];
    int n9 = css_parse(css9, (int)strlen(css9), r9, CSS_MAX_RULES);
    struct css_style s16, s17;
    css_compute(r9, n9, "p", 0, 0, 0, &s16);
    css_compute(r9, n9, "q", 0, 0, 0, &s17);
    ok("1.5em -> 24px", s16.has_size && s16.font_size == 24);
    ok("width 50% stashed", s16.has_wpct && s16.wpct == 50 && !s16.has_w);
    ok("negative margin kept", s16.has_mt && s16.margin_top == -5);
    ok("margin 2em -> 32", s16.has_mr && s16.margin_right == 32);
    ok("larger -> 20", s17.has_size && s17.font_size == 20);
    ok("border-width thick", s17.has_bw && s17.border_width == 5);

    // ---- margin: 0 auto + text-decoration ----
    const char* css10 = "div { margin: 0 auto; } a { text-decoration: underline; }\n"
                        "u { text-decoration: none; }\n";
    static struct css_rule r10[CSS_MAX_RULES];
    int n10 = css_parse(css10, (int)strlen(css10), r10, CSS_MAX_RULES);
    struct css_style s18, s19, s20;
    css_compute(r10, n10, "div", 0, 0, 0, &s18);
    css_compute(r10, n10, "a", 0, 0, 0, &s19);
    css_compute(r10, n10, "u", 0, 0, 0, &s20);
    ok("auto margins flagged", s18.ml_auto && s18.mr_auto);
    ok("auto zero top", s18.has_mt && s18.margin_top == 0);
    ok("underline parsed", s19.has_td && s19.td_ul);
    ok("none parsed", s20.has_td && !s20.td_ul);

    // ---- multiline last-decl without semicolon ----
    const char* css11 = "p {\n  color: red\n}";
    static struct css_rule r11[CSS_MAX_RULES];
    int n11 = css_parse(css11, (int)strlen(css11), r11, CSS_MAX_RULES);
    struct css_style s21;
    css_compute(r11, n11, "p", 0, 0, 0, &s21);
    ok("trailing-newline value", s21.has_fg && s21.fg == 4);

    // ---- position + offsets ----
    const char* css12 = "div { position: absolute; top: 32px; left: 4px; } "
                        "span { position: fixed; bottom: 10px; right: auto; } "
                        "em { position: relative; } i { position: sticky; } "
                        "b { position: static; }\n";
    static struct css_rule r12[CSS_MAX_RULES];
    int n12 = css_parse(css12, (int)strlen(css12), r12, CSS_MAX_RULES);
    struct css_style s22, s23, s24, s25, s26;
    css_compute(r12, n12, "div", 0, 0, 0, &s22);
    css_compute(r12, n12, "span", 0, 0, 0, &s23);
    css_compute(r12, n12, "em", 0, 0, 0, &s24);
    css_compute(r12, n12, "i", 0, 0, 0, &s25);
    css_compute(r12, n12, "b", 0, 0, 0, &s26);
    ok("position absolute", s22.has_pos && s22.pos_mode == 2);
    ok("top/left offsets", s22.has_top && s22.top == 32 && s22.has_left && s22.left == 4);
    ok("position fixed", s23.has_pos && s23.pos_mode == 3);
    ok("bottom kept, right:auto dropped", s23.has_bottom && s23.bottom == 10 && !s23.has_right);
    ok("position relative", s24.has_pos && s24.pos_mode == 1);
    ok("sticky degrades to relative", s25.has_pos && s25.pos_mode == 1);
    ok("static parses to mode 0", s26.pos_mode == 0);

    printf("\n%s: %d failures\n", fails == 0 ? "ALL PASS" : "FAILURES", fails);
    return fails ? 1 : 0;
}
