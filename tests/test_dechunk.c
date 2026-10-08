// Host test for http_dechunk (src/net/network.c). network.c is kernel-only,
// so the Makefile extracts the function into build-host/dechunk_fn.c.
// Regression: an overflow cap that counted DIGITS broke Google's
// zero-padded chunk sizes ("00002a8f") -> gzip body left framed -> the
// homepage rendered as Google's no-JS fallback.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tls_client.h"

int http_dechunk(char* buf, int len);

static int fails, passes;
static void check(const char* name, const char* resp, const char* want, int want_len) {
    static char buf[4096];
    int n = (int)strlen(resp);
    memcpy(buf, resp, n + 1);
    int got = http_dechunk(buf, n);
    if (got == want_len && memcmp(buf, want, want_len) == 0) { passes++; return; }
    fails++;
    printf("FAIL %s: got %d bytes '%.*s', want %d '%s'\n", name, got, got > 0 ? got : 0, buf, want_len, want);
}

#define H "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
int main(void) {
    check("plain", H "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", "hello world", 11);
    check("zero-padded 8", H "00000005\r\nhello\r\n00000001\r\n!\r\n00000000\r\n\r\n", "hello!", 6);
    check("zero-padded 16", H "0000000000000003\r\nabc\r\n0\r\n\r\n", "abc", 3);
    check("upper hex", H "A\r\n0123456789\r\n0\r\n\r\n", "0123456789", 10);
    check("extension", H "3;foo=bar\r\nabc\r\n0\r\n\r\n", "abc", 3);
    // Absurd sizes saturate: the copy stays bounded by the buffer.
    check("huge size", H "FFFFFFFFFFFF\r\nabc", "abc", 3);
    check("overflow 32", H "100000003\r\nabc", "abc", 3);
    check("not chunked", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc", "abc", 3);
    // Real Google framing: many 1-byte padded chunks, then a big one.
    {
        static char r[2048], w[512];
        int n = sprintf(r, H), wn = 0;
        for (int i = 0; i < 20; i++) { n += sprintf(r + n, "00000001\r\n%c\r\n", 'a' + i); w[wn++] = (char)('a' + i); }
        n += sprintf(r + n, "00000010\r\n0123456789abcdef\r\n0\r\n\r\n");
        memcpy(w + wn, "0123456789abcdef", 16); wn += 16;
        w[wn] = 0;
        check("google framing", r, w, wn);
    }
    printf("dechunk: %d passed, %d failed\n", passes, fails);
    return fails != 0;
}
