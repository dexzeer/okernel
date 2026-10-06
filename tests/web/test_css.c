// Host tests for the CSS engine: cascade unit tests + a full corpus pass.
//   test_css            unit tests
//   test_css <name>...  also compute styles for corpus pages
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "web/wdom.h"
#include "web/css.h"
#include "web/wurl.h"
#include "corpus.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct wdom* D;
static struct wstyleset* SS;

static void load(const char* html, const char* css) {
    if (SS) css_set_free(SS);
    if (D) wdom_free(D);
    D = whtml_parse(html, (int)strlen(html), "utf-8");
    SS = css_set_new(D);
    css_set_doc_base(SS, "https://example.com/dir/page.html");
    char buf[65536];
    int n = css_collect_inline(D, buf, sizeof buf);
    if (n) css_set_add_sheet(SS, buf, n, 0, -1);
    if (css) css_set_add_sheet(SS, css, (int)strlen(css), "https://example.com/css/", -1);
    css_compute_all(SS, 1280, 800);
}

static const struct wstyle* S(const char* id) {
    int el = wdom_find_id(D, id);
    if (el < 0) { printf("  (no element #%s)\n", id); return 0; }
    const struct wstyle* s = css_style_of(SS, el);
    if (!s) printf("  (no style for #%s)\n", id);
    return s;
}

#define PXV(lu) ((lu) / 64)

static void unit_tests(void) {
    const struct wstyle* s;
    // UA defaults
    load("<!doctype html><body id=b><h1 id=h>x</h1><p id=p>y</p><a id=a href=x>l</a>"
         "<ul><li id=li>i</li></ul><pre id=pre>p</pre><b id=bold>b</b>", 0);
    s = S("b"); CHECK(s && s->display == D_BLOCK && PXV(s->margin[0].px) == 8, "body block margin 8");
    s = S("h"); CHECK(s && PXV(s->font_size) == 32 && s->font_weight == 700, "h1 32px bold (got %d)", s ? PXV(s->font_size) : -1);
    CHECK(s && PXV(s->margin[0].px) == 21, "h1 margin .67em=21px (got %d)", s ? s->margin[0].px : -1);
    s = S("p"); CHECK(s && PXV(s->margin[0].px) == 16, "p margin 1em");
    s = S("a"); CHECK(s && s->color == 0xFF0000EE && (s->deco_line & DECO_UNDERLINE), "a:link blue underline (%08x)", s ? s->color : 0);
    s = S("li"); CHECK(s && s->display == D_LIST_ITEM && s->list_style_type == LS_DISC, "li list-item disc");
    s = S("pre"); CHECK(s && s->font_family == FAM_MONO && PXV(s->font_size) == 13 && s->white_space == WS_PRE, "pre mono 13px pre (fs %d)", s ? PXV(s->font_size) : -1);
    s = S("bold"); CHECK(s && s->font_weight == 700, "b bolder -> 700 (%d)", s ? s->font_weight : -1);

    // specificity, order, !important, inline
    load("<div id=x class='a b' style='color:#00ff00'>t</div><div id=y class=a>u</div>",
         "#x{color:red} .a.b{color:blue} div{color:black;background:#111}"
         ".a{background-color:rgb(1,2,3) !important} #y{background:#fff}"
         "#y{color:hsl(120, 100%, 25%)}");
    s = S("x"); CHECK(s && s->color == 0xFF00FF00, "inline beats id (%08x)", s ? s->color : 0);
    s = S("y"); CHECK(s && s->bg_color == 0xFF010203, "!important beats id (%08x)", s ? s->bg_color : 0);
    CHECK(s && s->color == 0xFF008000, "hsl color (%08x)", s ? s->color : 0);

    // inheritance, em/rem, percentages, calc, line-height number
    load("<html style='font-size:20px'><body><div id=o style='font-size:2em;line-height:1.5'>"
         "<span id=i style='font-size:50%;padding-left:calc(10px + 1em);width:calc(100% - 2rem)'>x</span></div>",
         0);
    s = S("o"); CHECK(s && PXV(s->font_size) == 40 && PXV(s->line_height) == 60, "2em of 20px = 40, lh 60 (%d %d)", s ? PXV(s->font_size) : -1, s ? PXV(s->line_height) : -1);
    s = S("i"); CHECK(s && PXV(s->font_size) == 20, "50%% of 40 = 20 (%d)", s ? PXV(s->font_size) : -1);
    CHECK(s && PXV(s->line_height) == 30, "lh number inherited: 1.5*20 = 30 (%d)", s ? PXV(s->line_height) : -1);
    CHECK(s && PXV(s->padding[3].px) == 30, "calc(10px+1em) = 30 (%d)", s ? PXV(s->padding[3].px) : -1);
    CHECK(s && s->width.pct == 10000 && PXV(s->width.px) == -40, "calc(100%% - 2rem) (%d %d)", s ? s->width.pct : -1, s ? s->width.px : -1);

    // custom properties
    load("<div id=r><p id=c>x</p><p id=d class=k>y</p></div>",
         ":root{--main:#123456;--gap: 12px;--m: 1px 2px 3px 4px}"
         "#r{--main:#abcdef} #c{color:var(--main);margin:var(--m)}"
         "#d{color:var(--nope, rebeccapurple);padding:var(--gap) 0;--x:var(--gap)} .k{border-left:var(--x) solid red}");
    s = S("c"); CHECK(s && s->color == 0xFFABCDEF, "var overridden on ancestor (%08x)", s ? s->color : 0);
    CHECK(s && PXV(s->margin[1].px) == 2 && PXV(s->margin[3].px) == 4, "margin shorthand via var");
    s = S("d"); CHECK(s && s->color == 0xFF663399, "var fallback (%08x)", s ? s->color : 0);
    CHECK(s && PXV(s->padding[0].px) == 12 && s->padding[1].px == 0, "padding: var() 0");
    CHECK(s && PXV(s->bw[3]) == 12 && s->bs[3] == BS_SOLID, "nested var in border (%d)", s ? s->bw[3] : -1);

    // shorthand vs later longhand ordering with var
    load("<p id=q>x</p>", "#q{margin-left:5px} #q{margin:var(--zz, 7px)} #q{margin-top:1px}");
    s = S("q"); CHECK(s && PXV(s->margin[3].px) == 7 && PXV(s->margin[0].px) == 1, "var shorthand overrides earlier longhand (%d %d)", s ? PXV(s->margin[3].px) : -1, s ? PXV(s->margin[0].px) : -1);

    // media queries
    // <link media> gating (sheet-level media condition)
    load("<p id=lm>x</p>", 0);
    css_set_add_sheet_ex(SS, "#lm{color:red}", 14, 0, -1, "(prefers-color-scheme: dark)", 28);
    css_set_add_sheet_ex(SS, "#lm{background:blue}", 20, 0, -1, "screen and (min-width: 100px)", 29);
    css_compute_all(SS, 1280, 800);
    s = S("lm"); CHECK(s && s->color == 0xFF000000 && s->bg_color == 0xFF0000FF, "sheet media gating (%08x %08x)", s ? s->color : 0, s ? s->bg_color : 0);

    load("<p id=m>x</p>", "@media (max-width: 600px){#m{color:red}} @media screen and (min-width:1000px){#m{color:blue}}"
         "@media print{#m{background:red}} @media (width >= 1200px) and (prefers-color-scheme: light){#m{font-weight:900}}"
         "@media not print{#m{font-style:italic}}");
    s = S("m"); CHECK(s && s->color == 0xFF0000FF && s->bg_color == 0 && s->font_weight == 900 && s->font_style == 1, "media queries");

    // structural selectors
    load("<ul><li id=l1>a</li><li id=l2>b</li><li id=l3 class=z>c</li><li id=l4>d</li></ul>"
         "<div id=e></div><div id=f data-k='foo bar' lang=en-US>q</div>",
         "li:first-child{color:red} li:nth-child(2n){color:blue} li:last-child{color:green}"
         "li:not(.z):nth-child(3){color:yellow} li:nth-child(1 of .z){font-weight:900}"
         "div:empty{display:none} [data-k~=bar]{color:#010101} [data-k^=fo][lang|=en]{font-style:italic}"
         "ul:has(> .z){background:#020202} li + li.z{text-decoration:underline} .z ~ li{color:#030303}");
    s = S("l1"); CHECK(s && s->color == 0xFFFF0000, "first-child");
    s = S("l2"); CHECK(s && s->color == 0xFF0000FF, "nth-child(2n)");
    s = S("l3"); CHECK(s && s->font_weight == 900 && (s->deco_line & DECO_UNDERLINE), "nth-child(of) + adjacent");
    s = S("l4"); CHECK(s && s->color == 0xFF030303, "general sibling beats last-child by order (%08x)", s ? s->color : 0);
    s = S("e"); CHECK(s && s->display == D_NONE, ":empty");
    s = S("f"); CHECK(s && s->color == 0xFF010101 && s->font_style == 1, "attribute selectors");
    {
        int ul = wdom_first_tag(D, T_ul);
        const struct wstyle* us = css_style_of(SS, ul);
        CHECK(us && us->bg_color == 0xFF020202, ":has(> .z)");
    }

    // nesting + tailwind escapes + :is/:where specificity
    load("<div class='card'><p id=n class='md:flex w-1/2'>x</p></div><p id=w class=t>y</p>",
         ".card{color:red; & p{color:blue} .w-1\\/2{font-weight:900}}"
         ".md\\:flex{display:flex} :where(#w){color:red} .t{color:#040404}");
    s = S("n"); CHECK(s && s->color == 0xFF0000FF && s->display == D_FLEX && s->font_weight == 900, "nesting + escaped classes");
    s = S("w"); CHECK(s && s->color == 0xFF040404, ":where has zero specificity");

    // display blockification, floats, flex items
    load("<div style='display:flex'><span id=fi>a</span></div><span id=fl style='float:left'>b</span>"
         "<span id=ab style='position:absolute;display:inline-flex'>c</span>", 0);
    s = S("fi"); CHECK(s && s->display == D_BLOCK, "flex item blockified");
    s = S("fl"); CHECK(s && s->display == D_BLOCK, "float blockified");
    s = S("ab"); CHECK(s && s->display == D_FLEX, "abspos inline-flex -> flex");

    // colors: oklch, color-mix, currentColor in border, transparent
    load("<p id=k style='color:oklch(62.8% 0.2577 29.23);border:2px solid'>x</p>"
         "<p id=k2 style='color:#ff000080;background:color-mix(in srgb, red 50%, blue)'>y</p>", 0);
    s = S("k"); CHECK(s && ((s->color >> 16) & 255) > 240 && ((s->color >> 8) & 255) < 20, "oklch red (%08x)", s ? s->color : 0);
    CHECK(s && s->bc[0] == s->color && PXV(s->bw[0]) == 2, "border currentColor");
    s = S("k2"); CHECK(s && (s->color >> 24) == 0x80 && ((s->bg_color >> 16) & 255) > 120 && (s->bg_color & 255) > 120, "alpha + color-mix (%08x %08x)", s ? s->color : 0, s ? s->bg_color : 0);

    // font shorthand + family classification
    load("<p id=fo style='font: italic bold 12px/30px Georgia, serif'>x</p>"
         "<p id=fo2 style='font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", sans-serif'>y</p>"
         "<code id=co style='font-family: SFMono-Regular, Consolas, monospace'>z</code>", 0);
    s = S("fo"); CHECK(s && s->font_style == 1 && s->font_weight == 700 && PXV(s->font_size) == 12 && PXV(s->line_height) == 30 && s->font_family == FAM_SERIF, "font shorthand");
    s = S("fo2"); CHECK(s && s->font_family == FAM_SANS, "system-ui stack -> sans");
    s = S("co"); CHECK(s && s->font_family == FAM_MONO, "mono stack");

    // presentational hints
    load("<table id=t width=500 bgcolor=#eeeeee cellpadding=7 border=1><tr><td id=td align=center valign=top>x</td></tr></table>"
         "<font id=fn size=5 color=red>f</font><img id=im width=40 height=30>", 0);
    s = S("t"); CHECK(s && PXV(s->width.px) == 500 && s->bg_color == 0xFFEEEEEE, "table width/bgcolor hints");
    s = S("td"); CHECK(s && s->text_align == TA_CENTER && s->vertical_align == VA_TOP && PXV(s->padding[0].px) == 7 && PXV(s->bw[0]) == 1, "td hints");
    s = S("fn"); CHECK(s && s->color == 0xFFFF0000 && PXV(s->font_size) == 24, "font element hints");
    s = S("im"); CHECK(s && PXV(s->width.px) == 40 && PXV(s->height.px) == 30, "img dims");

    // pseudo-elements, gradients, url resolution
    load("<p id=ps class=q>x</p><div id=g>y</div>",
         ".q::before{content:'\\2192  ' attr(id)} .q::after{content:none}"
         "#g{background:linear-gradient(to right, #fff 0%, rgba(0,0,0,.5) 100%), url(../img/a.png) no-repeat}");
    {
        int el = wdom_find_id(D, "ps");
        const struct wstyle* b = css_pseudo_of(SS, el, 1);
        const struct wstyle* a = css_pseudo_of(SS, el, 2);
        CHECK(b && b->content && !strncmp(css_str(SS, b->content, b->content_len), "\xE2\x86\x92 ps", 5), "::before content with escape + attr() ('%s')", b ? css_str(SS, b->content, b->content_len) : "");
        CHECK(!a || !a->content, "content:none -> no ::after");
    }
    s = S("g"); CHECK(s && s->bg_grad >= 0 && css_grad(SS, s->bg_grad)->nstops == 2 && css_grad(SS, s->bg_grad)->angle == 90000, "linear-gradient first layer");

    // url resolution
    {
        char out[512];
        wurl_resolve("https://a.com/x/y/z.html?q=1#f", "../img/b.png", 12, out, sizeof out);
        CHECK(!strcmp(out, "https://a.com/x/img/b.png"), "url ../ (%s)", out);
        wurl_resolve("https://a.com/x/y/z.html", "//cdn.b.org/s.css", 17, out, sizeof out);
        CHECK(!strcmp(out, "https://cdn.b.org/s.css"), "url // (%s)", out);
        wurl_resolve("https://a.com/x/y/z.html", "?p=2", 4, out, sizeof out);
        CHECK(!strcmp(out, "https://a.com/x/y/z.html?p=2"), "url ? (%s)", out);
        wurl_resolve("https://a.com", "foo/./bar/../baz", 16, out, sizeof out);
        CHECK(!strcmp(out, "https://a.com/foo/baz"), "url dots (%s)", out);
        wurl_resolve("http://a.com:8080/a/b", "/c", 2, out, sizeof out);
        CHECK(!strcmp(out, "http://a.com:8080/c"), "url port (%s)", out);
    }
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void corpus_run(const char* name) {
    struct corpus c;
    if (!corpus_open(&c, name)) { printf("corpus %s: missing\n", name); return; }
    int len;
    char* html = corpus_page(&c, &len);
    if (!html) return;
    double t0 = now_ms();
    struct wdom* d = whtml_parse(html, len, 0);
    double t1 = now_ms();
    struct wstyleset* ss = css_set_new(d);
    css_set_doc_base(ss, c.page_url);
    // sheets in document order: <style> and <link rel=stylesheet>
    int nsheet = 0, css_bytes = 0;
    for (int i = d->n[0].first; i >= 0; i = wdom_next(d, i, 0)) {
        if (wdom_is(d, i, T_style)) {
            static char buf[1 << 20];
            int n = wdom_text_content(d, i, buf, sizeof buf);
            double ts = now_ms();
            css_set_add_sheet(ss, buf, n, c.page_url, -1);
            if (getenv("SHEET_TIMES")) printf("  <style> %d bytes: %.2fms\n", n, now_ms() - ts);
            nsheet++; css_bytes += n;
        } else if (wdom_is(d, i, T_link)) {
            int rl; const char* rel = wdom_attr(d, i, A_rel, &rl);
            int hl; const char* href = wdom_attr(d, i, A_href, &hl);
            if (!rel || !href) continue;
            int sty = 0;
            for (int k = 0; k + 10 <= rl; k++) if (!strncasecmp(rel + k, "stylesheet", 10)) sty = 1;
            if (!sty) continue;
            char abs[1024];
            wurl_resolve(c.page_url, href, hl, abs, sizeof abs);
            int cl;
            char* css = corpus_get(&c, abs, &cl);
            if (!css) continue;
            double ts = now_ms();
            int ml; const char* media = wdom_attr(d, i, A_media, &ml);
            css_set_add_sheet_ex(ss, css, cl, abs, -1, media, media ? ml : 0);
            if (getenv("SHEET_TIMES")) printf("  <link> %s %d bytes: %.2fms\n", abs, cl, now_ms() - ts);
            nsheet++; css_bytes += cl;
            free(css);
        }
    }
    double t2 = now_ms();
    css_compute_all(ss, 1280, 800);
    double t3 = now_ms();
    int rules, styles, sheets;
    css_stats(ss, &rules, &styles, &sheets);
    const struct wstyle* bs = d->body >= 0 ? css_style_of(ss, d->body) : 0;
    printf("%-18s nodes=%6d sheets=%2d css=%7dB rules=%6d styles=%5d parse=%.1fms cssparse=%.1fms cascade=%.1fms body.bg=%08x color=%08x fs=%d fam=%d\n",
           name, d->nn, sheets, css_bytes, rules, styles, t1 - t0, t2 - t1, t3 - t2,
           bs ? bs->bg_color : 0, bs ? bs->color : 0, bs ? bs->font_size / 64 : 0, bs ? bs->font_family : -1);
    css_set_free(ss);
    wdom_free(d);
    free(html);
}

int main(int argc, char** argv) {
    unit_tests();
    printf("CSS unit tests: %d passed, %d failed\n", passes, fails);
    for (int i = 1; i < argc; i++) corpus_run(argv[i]);
    if (SS) css_set_free(SS);
    if (D) wdom_free(D);
    return fails != 0;
}
