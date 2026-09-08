/* Host test for external resource extraction: <link rel=stylesheet> and
   <script src="..."> URL extraction from HTML. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/html.h"
#include "../src/okai.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

int main(void) {
    // Test 1: extract <link rel="stylesheet" href="..."> URLs
    {
        const char* html =
            "<html><head>"
            "<link rel=\"stylesheet\" href=\"/css/main.css\">"
            "<link rel=\"stylesheet\" href=\"https://cdn.example.com/style.css\">"
            "<link rel=\"icon\" href=\"/favicon.ico\">"
            "</head><body></body></html>";
        char buf[4096];
        int n = html_extract_link_css(html, strlen(html), buf, sizeof(buf));
        CHECK(n == 2, "extract_link_css: found 2 stylesheet links");
        CHECK(strcmp(buf, "/css/main.css") == 0, "extract_link_css: first URL");
        CHECK(strcmp(buf + strlen(buf) + 1, "https://cdn.example.com/style.css") == 0,
              "extract_link_css: second URL");
    }

    // Test 2: extract <script src="..."> URLs
    {
        const char* html =
            "<html><body>"
            "<script src=\"/js/app.js\"></script>"
            "<script src=\"https://cdn.example.com/lib.min.js\"></script>"
            "<script>var x = 1;</script>"
            "</body></html>";
        char buf[4096];
        int n = html_extract_script_src(html, strlen(html), buf, sizeof(buf));
        CHECK(n == 2, "extract_script_src: found 2 external scripts");
        CHECK(strcmp(buf, "/js/app.js") == 0, "extract_script_src: first URL");
        CHECK(strcmp(buf + strlen(buf) + 1, "https://cdn.example.com/lib.min.js") == 0,
              "extract_script_src: second URL");
    }

    // Test 3: no external resources
    {
        const char* html = "<html><body><p>Hello</p></body></html>";
        char buf[4096];
        int n = html_extract_link_css(html, strlen(html), buf, sizeof(buf));
        CHECK(n == 0, "extract_link_css: no links returns 0");
        n = html_extract_script_src(html, strlen(html), buf, sizeof(buf));
        CHECK(n == 0, "extract_script_src: no scripts returns 0");
    }

    // Test 4: mixed case attributes
    {
        const char* html =
            "<LINK REL=\"Stylesheet\" HREF=\"/style1.css\">"
            "<SCRIPT SRC=\"/app1.js\"></SCRIPT>";
        char buf[4096];
        int n = html_extract_link_css(html, strlen(html), buf, sizeof(buf));
        CHECK(n == 1, "extract_link_css: case-insensitive REL");
        CHECK(strcmp(buf, "/style1.css") == 0, "extract_link_css: mixed case URL");
        n = html_extract_script_src(html, strlen(html), buf, sizeof(buf));
        CHECK(n == 1, "extract_script_src: case-insensitive SRC");
        CHECK(strcmp(buf, "/app1.js") == 0, "extract_script_src: mixed case URL");
    }

    // Test 5: existing css_parse still works
    {
        const char* css = "body { color: #333; } .box { background: #fff; }";
        struct css_rule rules[16];
        int n = css_parse(css, strlen(css), rules, 16);
        CHECK(n == 2, "css_parse: existing parser still works");
    }

    // Test 6: html_extract_css still works
    {
        const char* html = "<style>body{color:red;}</style><p>text</p>";
        char buf[4096];
        int n = html_extract_css(html, strlen(html), buf, sizeof(buf));
        CHECK(n > 0, "html_extract_css: still extracts inline styles");
        CHECK(strstr(buf, "body{color:red;}") != NULL, "html_extract_css: correct content");
    }

    printf("\n%d failures\n", fails);
    return fails ? 1 : 0;
}
