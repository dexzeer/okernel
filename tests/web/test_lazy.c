// Host test: <img loading=lazy> is fetched only once its box comes within reach of the viewport (wdoc_set_view),
// is not counted as pending while it waits, and eager images are unaffected.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web/wdoc.h"
#include "web/wdoc_int.h"
#include "web/wdom.h"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

static int next(struct wdoc* d, char* url) { return wdoc_next_fetch(d, url, 512); }
// -Isrc makes <string.h> the kernel's tiny one: no strstr
static int has(const char* s, const char* sub) {
    for (; *s; s++) { const char *a = s, *b = sub; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
    return 0;
}

int main(void) {
    static const char html[] =
        "<html><body style='margin:0'>"
        "<img src='http://x.test/eager.png' width=100 height=100>"
        "<img src='http://x.test/near.png' loading=lazy width=100 height=100>"
        "<div style='height:6000px'></div>"
        "<img src='http://x.test/far.png' loading=lazy width=100 height=100>"
        "<img src='http://x.test/also-eager.png' loading=eager width=100 height=100>"
        "</body></html>";
    struct wdoc* d = wdoc_new();
    CHECK(wdoc_load(d, "http://x.test/", html, (int)strlen(html), "utf-8") == 0, "load");
    char url[512];
    int id = next(d, url);
    CHECK(id >= 0 && has(url, "eager.png"), "the eager image is fetched at once");
    wdoc_fetch_done(d, id, 0, -1, 0);
    id = next(d, url);
    CHECK(id >= 0 && has(url, "also-eager"), "loading=eager is eager too");
    wdoc_fetch_done(d, id, 0, -1, 0);
    CHECK(next(d, url) < 0, "lazy images wait before there is a layout");
    CHECK(wdoc_pending(d) == 0, "waiting lazy images are not pending");
    wdoc_set_viewport(d, 800, 600);
    wdoc_update(d);
    CHECK(wdoc_set_view(d, 0, 1500) == 1, "one lazy image is within reach of the top");
    id = next(d, url);
    CHECK(id >= 0 && has(url, "near.png"), "the near image is fetched");
    CHECK(wdoc_pending(d) == 1, "it counts as pending now");
    wdoc_fetch_done(d, id, 0, -1, 0);
    CHECK(next(d, url) < 0, "the far image still waits");
    CHECK(wdoc_set_view(d, 0, 1500) == 0, "nothing new at the same position");
    CHECK(wdoc_set_view(d, 5500, 7000) == 1, "scrolling down brings the far image in reach");
    id = next(d, url);
    CHECK(id >= 0 && has(url, "far.png"), "the far image is fetched");
    wdoc_free(d);

    // a rebuild of the sheet set (scripts touching <style>/<link>) reuses the parsed sheets and keeps their rules
    static const char styled[] =
        "<html><head><style>p { height: 50px; margin: 0 } #b { height: 70px }</style>"
        "<style media='print'>p { height: 5px }</style></head><body style='margin:0'><p id=a>x</p><p id=b>y</p></body></html>";
    d = wdoc_new();
    CHECK(wdoc_load(d, "http://x.test/", styled, (int)strlen(styled), "utf-8") == 0, "load styled page");
    wdoc_set_viewport(d, 800, 600);
    wdoc_update(d);
    int x, y, w, h, ha = -1, hb = -1;
    int na = wdom_find_id(wdoc_dom(d), "a"), nb = wdom_find_id(wdoc_dom(d), "b");
    if (wdoc_node_rect(d, na, &x, &y, &w, &h)) ha = h;
    if (wdoc_node_rect(d, nb, &x, &y, &w, &h)) hb = h;
    CHECK(ha == 50 && hb == 70, "styles apply");
    for (int round = 0; round < 3; round++) {
        wdoc_dom_changed(d, WDC_STYLE);
        wdoc_update(d);
        ha = hb = -1;
        if (wdoc_node_rect(d, na, &x, &y, &w, &h)) ha = h;
        if (wdoc_node_rect(d, nb, &x, &y, &w, &h)) hb = h;
        CHECK(ha == 50 && hb == 70, "styles survive a rebuild of the sheet set");
    }
    wdoc_free(d);
    printf(fails ? "LAZY TESTS FAIL\n" : "LAZY TESTS PASS\n");
    return fails != 0;
}
