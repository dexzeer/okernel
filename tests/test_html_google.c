// Host-side: run the kernel's html_parse on a real captured page and dump
// what the okai would render (token types + text), to diagnose "garbled".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/html.h"

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <html-file> [max-tokens]\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    int max_tokens = argc > 2 ? atoi(argv[2]) : HTML_MAX_TOKENS;
    // Mimic the okai's HTTP response cap (128KB since the fix)
    if (sz > 131071) {
        printf("[note] input %ld bytes, truncating to 131071 (okai HTTP cap)\n", sz);
        sz = 131071;
        buf[sz] = 0;
    }

    static struct html_token tokens[HTML_MAX_TOKENS];
    int n = html_parse(buf, (int)sz, tokens, max_tokens);
    printf("parsed %d tokens\n", n);

    char title[64];
    html_get_title(buf, (int)sz, title, 64);
    printf("title: '%s'\n\n", title);

    for (int i = 0; i < n; i++) {
        struct html_token* t = &tokens[i];
        const char* tn = "?";
        switch (t->type) {
            case HTML_TEXT: tn = "TEXT"; break;
            case HTML_H1: tn = "H1"; break;
            case HTML_H2: tn = "H2"; break;
            case HTML_H3: tn = "H3"; break;
            case HTML_H4: tn = "H4"; break;
            case HTML_H5: tn = "H5"; break;
            case HTML_H6: tn = "H6"; break;
            case HTML_PARA: tn = "PARA"; break;
            case HTML_LINK: tn = "LINK"; break;
            case HTML_LIST_ITEM: tn = "LI"; break;
            case HTML_PRE: tn = "PRE"; break;
            case HTML_LINE_BREAK: tn = "BR"; break;
            case HTML_TITLE: tn = "TITLE"; break;
            case HTML_DIV: tn = "DIV"; break;
            case HTML_SPAN: tn = "SPAN"; break;
            case HTML_BLOCK: tn = "BLOCK"; break;
            case HTML_END_PARA: tn = "ENDP"; break;
        }
        // show control chars as . and high-bit chars as ~ to spot garbage
        char clean[HTML_MAX_TEXT + 1];
        int cl = 0;
        for (int j = 0; t->text[j] && j < HTML_MAX_TEXT; j++) {
            unsigned char c = (unsigned char)t->text[j];
            if (c >= 32 && c < 127) clean[cl++] = t->text[j];
            else if (c == '\n' || c == '\t' || c == '\r') clean[cl++] = ' ';
            else clean[cl++] = '~';
        }
        clean[cl] = 0;
        printf("[%3d] %-5s '%s'", i, tn, clean);
        if (t->type == HTML_LINK) printf("  href='%s'", t->href);
        printf("\n");
    }

    // Entity audit: what named entities does the page use?
    int counts[8] = {0};
    const char* names[8] = {"&amp;", "&lt;", "&gt;", "&quot;", "&nbsp;", "&copy;", "&laquo;", "&raquo;"};
    for (long i = 0; i + 6 < sz; i++) {
        if (buf[i] == '&') {
            for (int k = 0; k < 8; k++) {
                int L = strlen(names[k]);
                if (strncmp(buf + i, names[k], L) == 0) { counts[k]++; break; }
            }
        }
    }
    printf("\nentity usage: ");
    for (int k = 0; k < 8; k++) printf("%s=%d ", names[k], counts[k]);
    printf("\n");

    // numeric entities >= 127 (dropped by parser)
    int numhi = 0, numtotal = 0;
    for (long i = 0; i + 3 < sz; i++) {
        if (buf[i] == '&' && buf[i+1] == '#') {
            long j = i + 2; int val = 0; int digits = 0;
            while (j < sz && buf[j] >= '0' && buf[j] <= '9') { val = val * 10 + (buf[j] - '0'); j++; digits++; }
            if (digits && j < sz && buf[j] == ';') { numtotal++; if (val >= 127) numhi++; }
        }
    }
    printf("numeric entities: total=%d high(>=127, dropped)=%d\n", numtotal, numhi);
    return 0;
}
