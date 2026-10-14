// Host test for the preload scanner.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web/pscan.h"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

static char got[64][300];
static int gk[64], ng;
static int emit(void* c, const char* u, int k) { (void)c; if (ng < 64) { snprintf(got[ng], 300, "%s", u); gk[ng++] = k; } return 0; }
static int has(const char* u, int k) { for (int i = 0; i < ng; i++) if (!strcmp(got[i], u) && gk[i] == k) return 1; return 0; }

int main(void) {
    const char* B = "https://x.test/dir/page.html";
    static const char H[] =
        "<!doctype html><html><head><meta charset=utf-8><title>a <link rel=stylesheet href=/in-title.css> b</title>"
        "<link rel=\"stylesheet\" href=\"/a.css\"><link rel=stylesheet media=print href=/print.css>"
        "<link rel='preload' as=script href='pre.js'><link rel=preload as=font href=/f.woff2>"
        "<link rel=modulepreload href=\"/m.mjs\"><link rel=icon href=/fav.ico><link rel=\"alternate stylesheet\" href=/alt.css>"
        "<script src=\"/s1.js\"></script><script>var x = '<img src=/in-js.png>'; document.write('<link rel=stylesheet href=/js.css>');</script>"
        "<script nomodule src=/legacy.js></script><style>@import url(/imp.css); a::before { content: '<img src=/in-style.png>' }</style>"
        "<!-- <link rel=stylesheet href=/commented.css> --></head><body>"
        "<img src=\"i1.png\"><img src=/lazy.png loading=lazy><img src=\"data:image/gif;base64,AAAA\"><img alt='a>b' src=/i2.png>"
        "<img src=/amp.png?a=1&amp;b=2><textarea><img src=/in-ta.png></textarea><IMG SRC=/upper.png>"
        "<script async src=https://cdn.test/lib.js></script></body></html>";
    int nf;
    int n = pscan_html(H, (int)strlen(H), 0, B, emit, 0, &nf);
    CHECK(n == ng && nf == (int)strlen(H), "whole document scanned");
    CHECK(has("https://x.test/a.css", PSCAN_CSS), "stylesheet link");
    CHECK(!has("https://x.test/print.css", PSCAN_CSS), "print stylesheet skipped");
    CHECK(has("https://x.test/dir/pre.js", PSCAN_SCRIPT), "preload as=script, relative URL");
    CHECK(!has("https://x.test/f.woff2", PSCAN_IMG) && !has("https://x.test/f.woff2", PSCAN_CSS) && !has("https://x.test/f.woff2", PSCAN_SCRIPT), "preload as=font ignored");
    CHECK(has("https://x.test/m.mjs", PSCAN_SCRIPT), "modulepreload");
    CHECK(!has("https://x.test/fav.ico", PSCAN_IMG), "icons ignored");
    CHECK(!has("https://x.test/alt.css", PSCAN_CSS), "alternate stylesheets ignored");
    CHECK(has("https://x.test/s1.js", PSCAN_SCRIPT), "script src");
    CHECK(!has("https://x.test/legacy.js", PSCAN_SCRIPT), "nomodule script skipped");
    CHECK(!has("https://x.test/in-js.png", PSCAN_IMG) && !has("https://x.test/js.css", PSCAN_CSS), "nothing inside inline script");
    CHECK(!has("https://x.test/in-style.png", PSCAN_IMG), "nothing inside <style>");
    CHECK(!has("https://x.test/commented.css", PSCAN_CSS), "nothing inside comments");
    CHECK(!has("https://x.test/in-title.css", PSCAN_CSS), "nothing inside <title>");
    CHECK(!has("https://x.test/in-ta.png", PSCAN_IMG), "nothing inside <textarea>");
    CHECK(has("https://x.test/dir/i1.png", PSCAN_IMG), "img src");
    CHECK(!has("https://x.test/lazy.png", PSCAN_IMG), "loading=lazy skipped");
    CHECK(has("https://x.test/i2.png", PSCAN_IMG), "attribute value containing '>' does not end the tag");
    CHECK(has("https://x.test/amp.png?a=1&b=2", PSCAN_IMG), "&amp; in a URL");
    CHECK(has("https://x.test/upper.png", PSCAN_IMG), "upper case tag and attribute");
    CHECK(has("https://cdn.test/lib.js", PSCAN_SCRIPT), "absolute URL, async script");
    // incremental: cut the document at every position; the union over calls equals the single pass
    int bad = 0;
    for (int cut = 1; cut < (int)strlen(H); cut += 7) {
        ng = 0;
        int from = 0, len1 = cut;
        pscan_html(H, len1, from, B, emit, 0, &from);
        pscan_html(H, (int)strlen(H), from, B, emit, 0, &from);
        int total1 = ng;
        ng = 0;
        pscan_html(H, (int)strlen(H), 0, B, emit, 0, &nf);
        if (total1 != ng) { bad++; printf("  cut=%d: %d vs %d\n", cut, total1, ng); }
    }
    CHECK(bad == 0, "scanning in two pieces finds exactly what one pass finds");
    {
        static const char Hb[] = "<link rel=stylesheet href=/a.css><base href=/other/><link rel=stylesheet href=/b.css>";
        ng = 0;
        int r = pscan_html(Hb, (int)strlen(Hb), 0, B, emit, 0, &nf);
        CHECK(r == -1 && has("https://x.test/a.css", PSCAN_CSS) && !has("https://x.test/b.css", PSCAN_CSS), "stops at <base>");
    }
    // hostile input: truncated everywhere, no crash
    for (int cut = 0; cut <= (int)strlen(H); cut++) { ng = 0; pscan_html(H, cut, 0, B, emit, 0, &nf); }
    CHECK(1, "every truncation survives");
    printf(fails ? "PSCAN TESTS FAIL\n" : "PSCAN TESTS PASS\n");
    return fails != 0;
}
