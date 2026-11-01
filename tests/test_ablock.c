// tests/test_ablock.c — host unit tests for the built-in adblocker
// (src/ablock.c). Builds the same way the kernel does (freestanding subset)
// but on the host:  gcc -m32 -O2 -Isrc -o t_ablock tests/test_ablock.c
//                   src/ablock.c src/web/wdom.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#include "web/wcommon.h"      // w_free (the engine allocator on both builds)
#include "ablock.h"
#include "web/wdom.h"

static int fails = 0, checks = 0;

// the css engine's progress hook (wcommon.h): the kernel defines it in the
// engine; the unit test provides its own
void (*w_progress_hook)(void);
uint32_t w_progress_n;

// The :has-text() path verifies its base selector with the engine's
// css_selq (css_select.c) — linking the whole css engine into this unit
// test pulls in the image decoders, so the host build provides
// match-always stubs. ablock's own part (needle match in the element text,
// attribute tagging, rule emission) IS tested here; the selector half is
// covered by tests/headless/test_ablock.py with the real engine.
struct wselq;
struct wselq* css_selq_compile(struct wdom* d, const char* s, int len) {
    (void)d; (void)s;
    return (struct wselq*)1;
}
int css_selq_match(const struct wselq* q, const struct wdom* d, int el) {
    (void)q; (void)d; (void)el;
    return 1;
}
void css_selq_free(struct wselq* q) { (void)q; }

// <string.h> resolves to the kernel's freestanding string.h under -Isrc
// (no strstr there) — this test needs only this bounded variant.
static int has_needle(const char* h, const char* n) {
    for (int i = 0; h[i]; i++) {
        int k = 0;
        while (n[k] && h[i + k] == n[k]) k++;
        if (!n[k]) return 1;
    }
    return 0;
}

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void load_list(const char* text) {
    ablock_load(text, (uint32_t)strlen(text));
    while (ablock_parse_step(0)) {}
}

static int allows(const char* url, const char* top, int kind) {
    return ablock_allows(url, top, kind);
}

// a tiny hand-built document for the cosmetic tests
static struct wdom* make_doc(const char* tag, const char* id, const char* cls, const char* extra_attr, const char* extra_val) {
    struct wdom* d = wdom_new();
    if (!d) return 0;
    int el = wdom_create_element(d, NS_HTML, watom_intern(&d->atoms, tag, (int)strlen(tag)));
    if (el < 0) { wdom_free(d); return 0; }
    wdom_append(d, 0, el);
    if (id) wdom_set_attr(d, el, A_id, id, (int)strlen(id));
    if (cls) wdom_set_attr(d, el, A_class, cls, (int)strlen(cls));
    if (extra_attr) wdom_set_attr(d, el, watom_intern(&d->atoms, extra_attr, (int)strlen(extra_attr)), extra_val, (int)strlen(extra_val));
    return d;
}

static int css_has(const char* css, const char* needle) {
    if (!css) return 0;
    return has_needle(css, needle);
}

static void test_network(void) {
    load_list(
        "[Adblock Plus 2.0]\n"
        "! a comment\r\n"
        "||ads.example.com^\n"
        "||tracker.net^$script,third-party\n"
        "||mixed.net^\n"
        "@@||mixed.net^$script\n"
        "||imp.net^$important\n"
        "@@||imp.net^\n"
        "||cdn.net/track.js$domain=~ok.org|allowed.org\n"
        "||host-x.com^\n"
        "||blocked-doc.net^$document\n"
        "||bf.net^\n"
        "||bf.net^$badfilter\n"
        "||frame-e.com^\n"
        "@@||frame-e.com^$subdocument\n"
        "/adbanner.gif\n"
        "/ads/\n"
        "http://x.com/ban*er.png\n"
        "||sep.org^path^\n"
        "swf|\n"
        "||popup.net^$popup\n"
        "||wild.net^*\n"
        "upstream.example\n"
        "||regex.net/\\d+^\n"
    );

    // host set (||host^): exact host, subdomains, parent domains
    CHECK(allows("http://example.com/", 0, ABK_MAIN) == 1);
    CHECK(allows("http://ads.example.com/x.js", "http://example.com/", ABK_SCRIPT) == 0);
    CHECK(allows("https://track.ads.example.com/", "http://example.com/", ABK_IMG) == 0);
    CHECK(allows("http://notads.example.com/x", "http://example.com/", ABK_IMG) == 1);
    CHECK(allows("http://example.com:8080/x", "http://example.com/", ABK_SCRIPT) == 1);

    // options: script + third-party
    CHECK(allows("https://tracker.net/s.js", "http://example.com/", ABK_SCRIPT) == 0);
    CHECK(allows("https://tracker.net/s.js", "https://tracker.net/page", ABK_SCRIPT) == 1);   // first-party
    CHECK(allows("https://tracker.net/pix.gif", "http://example.com/", ABK_IMG) == 1);        // not $script
    CHECK(allows("https://tracker.net/x", 0, ABK_MAIN) == 1);                                 // untyped, but $script is typed... $script only

    // exceptions
    CHECK(allows("https://mixed.net/s.js", "http://example.com/", ABK_SCRIPT) == 1);          // @@ script
    CHECK(allows("https://mixed.net/pix.gif", "http://example.com/", ABK_IMG) == 0);          // block stands
    CHECK(allows("https://imp.net/s.js", "http://example.com/", ABK_SCRIPT) == 0);            // $important beats @@
    CHECK(allows("https://mixed.net/", 0, ABK_MAIN) == 1);                                    // navigations never untyped-blocked

    // $domain=
    CHECK(allows("https://cdn.net/track.js", "https://allowed.org/", ABK_SCRIPT) == 0);
    CHECK(allows("https://cdn.net/track.js", "https://sub.allowed.org/", ABK_SCRIPT) == 0);
    CHECK(allows("https://cdn.net/track.js", "https://ok.org/", ABK_SCRIPT) == 1);
    CHECK(allows("https://cdn.net/track.js", "https://example.com/", ABK_SCRIPT) == 1);

    // untyped vs main document / frames / subdocuments
    CHECK(allows("http://host-x.com/", 0, ABK_MAIN) == 1);
    CHECK(allows("http://host-x.com/", "http://embedder.com/", ABK_FRAME) == 0);
    CHECK(allows("http://host-x.com/img.png", "http://embedder.com/", ABK_IMG) == 0);
    CHECK(allows("http://blocked-doc.net/", 0, ABK_MAIN) == 0);                               // $document does block

    // frames: untyped blocks, @@$subdocument unblocks
    CHECK(allows("http://frame-e.com/x.js", "http://example.com/", ABK_SCRIPT) == 0);
    CHECK(allows("http://frame-e.com/", "http://example.com/", ABK_FRAME) == 1);

    // badfilter
    CHECK(allows("https://bf.net/x.js", "http://example.com/", ABK_SCRIPT) == 1);

    // plain substring (token), regex-literal unwrap, wildcard, separator, end anchor
    CHECK(allows("http://x.com/img/adbanner.gif", "http://example.com/", ABK_IMG) == 0);
    CHECK(allows("http://x.com/img/adbanner2.gif", "http://example.com/", ABK_IMG) == 1);     // token must be a full run (uBlock too)
    CHECK(allows("http://x.com/ads/foo.js", "http://example.com/", ABK_SCRIPT) == 0);         // /ads/ as regex == literal "ads"
    CHECK(allows("http://x.com/adverts/", "http://example.com/", ABK_SCRIPT) == 1);           // "adverts" does not contain "ads"
    CHECK(allows("http://x.com/banner.png", "http://example.com/", ABK_IMG) == 0);            // ban*er.png
    CHECK(allows("http://x.com/banXer.png", "http://example.com/", ABK_IMG) == 0);
    CHECK(allows("http://x.com/banner2.png", "http://example.com/", ABK_IMG) == 1);
    CHECK(allows("http://sep.org/path/x", "http://example.com/", ABK_IMG) == 0);              // ||sep.org^path^
    CHECK(allows("http://sep.org/pathology", "http://example.com/", ABK_IMG) == 1);
    CHECK(allows("http://a.swf.example.com/x", "http://example.com/", ABK_IMG) == 1);         // swf| needs the URL to END there
    CHECK(allows("http://x.com/movie.swf", "http://example.com/", ABK_IMG) == 0);
    CHECK(allows("http://x.com/movie.swf?r=1", "http://example.com/", ABK_IMG) == 1);

    // skipped lines: $popup and the \\d regex never load
    CHECK(allows("https://popup.net/x", "http://example.com/", ABK_SCRIPT) == 1);
    CHECK(allows("https://regex.net/123", "http://example.com/", ABK_SCRIPT) == 1);

    // plain-host line ("upstream.example") behaves like ||host^
    CHECK(allows("http://upstream.example/p", "http://example.com/", ABK_REQ) == 0);
    CHECK(allows("http://upstream.example/", 0, ABK_MAIN) == 1);

    // requests with no top page (main navigation) are first-party
    CHECK(allows("https://tracker.net/s.js", 0, ABK_SCRIPT) == 1);
    ablock_reset();
}

static void test_cosmetic(void) {
    load_list(
        "##.ad-banner\n"
        "##.wide-ad\n"
        "example.com##.promo\n"
        "sub.example.com##.deep\n"
        "ok.org,~bad.ok.org##.combo\n"
        "##.promo\n"                        // generic .promo too
        "example.com#@#.promo\n"            // ...unhidden on example.com
        "##topad\n"
        "##div[id^=\"ad-\"]\n"
        "##.x:has-text(cheap)\n"            // procedural: skipped
        "example.com##+js(nano-sib)\n"      // scriptlet: skipped
        "##^script:has-text(zz)\n"          // html filter: skipped
        "example.com#$#body { color: red }\n"  // style injection: skipped
        "~example.com##.not-here\n"
    );

    // class hit -> selector emitted
    struct wdom* d1 = make_doc("div", 0, "ad-banner footer", 0, 0);
    char* css = ablock_doc_css(d1, "www.news.com");
    CHECK(css && css_has(css, ".ad-banner{display:none!important}"));
    CHECK(css && !css_has(css, "promo"));
    CHECK(css && css_has(css, "not-here"));           // ~example.com: applies here
    w_free(css);
    wdom_free(d1);

    // nothing but the negative-only rule's match -> just that rule
    struct wdom* d2 = make_doc("div", 0, "content", 0, 0);
    css = ablock_doc_css(d2, "www.news.com");
    CHECK(css && css_has(css, ".not-here{display:none!important}"));
    CHECK(css && !css_has(css, "ad-banner"));
    w_free(css);
    wdom_free(d2);

    // domain-qualified, host match / mismatch, subdomains (domain= matches
    // subdomains, and so does an unhide)
    struct wdom* d3 = make_doc("div", 0, "promo deep", 0, 0);
    css = ablock_doc_css(d3, "example.com");
    CHECK(css == 0);                                  // .promo canceled by example.com#@#.promo, .deep is sub.example.com only
    wdom_free(d3);
    struct wdom* d3b = make_doc("div", 0, "promo deep", 0, 0);
    css = ablock_doc_css(d3b, "sub.example.com");
    CHECK(css && !css_has(css, ".promo"));            // the unhide's suffix match covers the subdomain too
    CHECK(css && css_has(css, ".deep{display:none!important}"));
    w_free(css);
    wdom_free(d3b);

    struct wdom* d4 = make_doc("div", 0, "deep", 0, 0);
    css = ablock_doc_css(d4, "sub.example.com");
    CHECK(css && css_has(css, ".deep{display:none!important}"));
    w_free(css);
    wdom_free(d4);

    // id hit
    struct wdom* d5 = make_doc("div", "topad", 0, 0, 0);
    css = ablock_doc_css(d5, "www.news.com");
    CHECK(css && css_has(css, "topad{display:none!important}"));
    w_free(css);
    wdom_free(d5);

    // attribute rule keyed on the attr name
    struct wdom* d6 = make_doc("div", "ad-160", 0, 0, 0);
    css = ablock_doc_css(d6, "www.news.com");
    CHECK(css && css_has(css, "div[id^=\"ad-\"]"));
    w_free(css);
    wdom_free(d6);

    // procedural rule never emitted
    struct wdom* d7 = make_doc("div", 0, "x", 0, 0);
    css = ablock_doc_css(d7, "www.news.com");
    CHECK(css == 0 || !css_has(css, "has-text"));
    w_free(css);
    wdom_free(d7);

    // ~example.com: hidden elsewhere, not on example.com
    struct wdom* d8 = make_doc("div", 0, "not-here", 0, 0);
    css = ablock_doc_css(d8, "example.com");
    CHECK(css == 0 || !css_has(css, "not-here"));
    w_free(css);
    struct wdom* d8b = make_doc("div", 0, "not-here", 0, 0);
    css = ablock_doc_css(d8b, "other.net");
    CHECK(css && css_has(css, "not-here"));
    w_free(css);
    wdom_free(d8);
    wdom_free(d8b);
    ablock_reset();
}

static void test_real_lists(void) {
    // the shipped lists must parse with sane counts (they are checked in)
    static char* buf;
    long total = 0;
    const char* files[2] = { "filters/easylist.txt", "filters/easyprivacy.txt" };
    for (int i = 0; i < 2; i++) {
        FILE* f = fopen(files[i], "rb");
        if (!f) { printf("SKIP %s (not found)\n", files[i]); continue; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* nb = (char*)realloc(buf, (size_t)(total + len + 1));
        if (!nb) { fclose(f); continue; }
        buf = nb;
        if (fread(buf + total, 1, (size_t)len, f) != (size_t)len) { fclose(f); continue; }
        total += len;
        fclose(f);
    }
    if (!total) { printf("SKIP real lists\n"); return; }
    buf[total] = 0;
    uint64_t t0 = __rdtsc();
    ablock_load(buf, (uint32_t)total);
    while (ablock_parse_step(0)) {}
    uint64_t t1 = __rdtsc();
    CHECK(ablock_ready());
    // smoke: well-known entries of both lists must actually block, and the
    // check must be fast
    CHECK(allows("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js", "https://news.com/", ABK_SCRIPT) == 0);
    CHECK(allows("https://www.googletagmanager.com/gtag/js?id=G-X", "https://news.com/", ABK_SCRIPT) == 0);
    CHECK(allows("https://www.google-analytics.com/analytics.js", "https://news.com/", ABK_SCRIPT) == 0);
    CHECK(allows("https://news.com/style.css", "https://news.com/", ABK_CSS) == 1);
    CHECK(allows("https://news.com/", 0, ABK_MAIN) == 1);
    uint64_t t2 = __rdtsc();
    for (int i = 0; i < 10000; i++)
        allows("https://cdn.news.com/img/story/2026/10/pic.jpg?v=3", "https://news.com/", ABK_IMG);
    uint64_t t3 = __rdtsc();
    printf("ablock: load %llu Mcycles (%.1f MB list), 10k checks in %llu Mcycles\n",
           (unsigned long long)(t1 - t0) >> 20, (double)total / 1048576.0,
           (unsigned long long)((t3 - t2) >> 20));
    ablock_reset();
    free(buf);
}

static void test_scriptlets_redirects_hastext(void) {
    load_list(
        "example.com##+js(set, window.__ablkOk, 1)\n"
        "##+js(acs, document.createElement, admiral)\n"
        "||ads.example.net/gtm.js$redirect=noopjs\n"
        "||ads.example.net/pix.gif$redirect=1x1.gif\n"
        "||ads.example.net/plain.js$redirect=noop.js\n"
        "##.ht-elem:has-text(FREE_IPHONE)\n"
        "##.plain-elem:has-text(x):has-text(y)\n"      // not trailing: skipped
        "##.re-elem:has-text(/foo\\d/)\n"              // regex needle: skipped for now
    );

    // scriptlets: parsed with name + args, domain-qualified
    {
        char* js = ablock_doc_scriptlets("example.com");
        CHECK(js && has_needle(js, "__ablk(["));
        CHECK(js && has_needle(js, "\"set\",\"window.__ablkOk\",\"1\""));
        CHECK(js && has_needle(js, "set-constant"));
        w_free(js);
    }
    {
        char* js = ablock_doc_scriptlets("other.org");
        CHECK(js && has_needle(js, "\"acs\",\"document.createElement\",\"admiral\""));
        CHECK(js && !has_needle(js, "window.__ablkOk"));            // example.com only
        w_free(js);
    }

    // redirects: a block that carries a built-in resource
    {
        int r = -1;
        CHECK(ablock_allows2("https://ads.example.net/gtm.js", "https://x.com/", ABK_SCRIPT, &r) == 0);
        CHECK(r >= 0);                                              // some noop resource
        const char* b; int bl; const char* ct;
        ablock_resource(r, &b, &bl, &ct);
        CHECK(ct && has_needle(ct, "javascript"));
        CHECK(ablock_allows2("https://ads.example.net/pix.gif", "https://x.com/", ABK_IMG, &r) == 0);
        ablock_resource(r, &b, &bl, &ct);
        CHECK(bl == 43 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F');
        CHECK(ablock_allows2("https://ads.example.net/plain.js", "https://x.com/", ABK_SCRIPT, &r) == 0);
        ablock_resource(r, &b, &bl, &ct);
        CHECK(has_needle(ct, "javascript"));
        CHECK(ablock_allows("https://ads.example.net/gtm.js", "https://x.com/", ABK_SCRIPT) == 0);
        CHECK(ablock_allows2("https://ads.example.net/gtm.js", 0, ABK_MAIN, &r) == 1 && r == -1);  // untyped: a navigation passes
    }

    // :has-text(): the tagged elements get an internal attribute rule, and
    // the base selector is never emitted as plain CSS
    {
        struct wdom* d = make_doc("div", 0, "ht-elem", 0, 0);
        // give it text
        int t = wdom_create_text(d, "win a FREE_IPHONE now", 21);
        wdom_append(d, 1, t);
        char* css = ablock_doc_css(d, "www.news.com");
        CHECK(css && has_needle(css, "[data-ablk-h=\"r"));
        CHECK(css && !has_needle(css, "ht-elem"));                  // base selector stays internal
        CHECK(css && has_needle(css, "display:none!important"));
        // the element was tagged
        const char* v = wdom_attr(d, 1, watom_intern(&d->atoms, "data-ablk-h", 11), &(int){0});
        CHECK(v != 0);
        w_free(css);
        wdom_free(d);
    }
    {
        // no text match -> no attribute rule at all
        struct wdom* d = make_doc("div", 0, "ht-elem", 0, 0);
        char* css = ablock_doc_css(d, "www.news.com");
        CHECK(css == 0);
        wdom_free(d);
    }
    ablock_reset();
}

int main(void) {
    test_network();
    test_cosmetic();
    test_scriptlets_redirects_hastext();
    test_real_lists();
    printf("t_ablock: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
