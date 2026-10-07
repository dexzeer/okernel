// TLS compatibility scanner: runs the KERNEL's TLS 1.3 client (tls_state
// machine + certificate verification) against real sites over host sockets
// and reports why each one fails. Mirrors the browser fetch: SNI, the same
// request headers, a 2MB response buffer.
//
//   gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/tls_scan tests/tls_scan.c $(HOST_CRYPTO_SRC)
//   ./build-host/tls_scan example.com wikipedia.org ...      (or -f hosts.txt)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "tls_client.h"
#include "x509.h"

static int fd = -1;

static int s_send(const uint8_t* b, uint32_t n, void* u) {
    (void)u;
    uint32_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) return -1;
        off += (uint32_t)w;
    }
    return 0;
}

static int s_recv(uint8_t* b, uint32_t cap, uint32_t timeout_ms, void* u) {
    (void)u;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = { (long)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000 };
    int s = select(fd + 1, &r, 0, 0, &tv);
    if (s <= 0) return 0;
    ssize_t n = read(fd, b, cap);
    if (n <= 0) return -1;
    return (int)n;
}

static int connect_host(const char* host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res)) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 10, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, res->ai_addr, res->ai_addrlen)) { close(s); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);
    return s;
}

static const char* reason(int r) {
    static const char* n[] = { "none", "close_notify", "ALERT", "MAC", "CERT", "HOSTNAME", "PROTO", "RNG", "OVERFLOW" };
    return r >= 0 && r <= 8 ? n[r] : "?";
}
static const char* cvname(int c) {
    static const char* n[] = { "-", "parse", "chain", "untrusted-root", "expired", "not-yet-valid", "hostname",
                               "ca-flags", "no-cert", "key-usage/strength", "pin-changed", "ocsp", "revoked", "preload-pin" };
    return c >= 0 && c <= 13 ? n[c] : "?";
}

static uint8_t resp[2 * 1024 * 1024 + 1];

static void scan(const char* host) {
    char h[256];
    snprintf(h, sizeof h, "%s", host);
    fd = connect_host(h, 443);
    if (fd < 0) { printf("%-28s TCP-FAIL\n", h); fflush(stdout); return; }
    char req[1024];
    int rl = snprintf(req, sizeof req,
                      "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: okernel/0.4\r\nAccept: */*\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n", h);
    static struct tls_state st;
    tls_state_init(&st, h, 443, (const uint8_t*)req, (uint32_t)rl, resp, sizeof resp - 1);
    struct tls_client_io io = { s_send, s_recv, 0 };
    time_t t0 = time(0);
    int r;
    for (;;) {
        r = tls_state_step(&st, &io);
        if (r != TLS_STEP_AGAIN) break;
        if (time(0) - t0 > 25) { r = -99; break; }
    }
    close(fd);
    fd = -1;
    if (r == TLS_STEP_DONE || (st.out_len > 0 && tls_response_complete(resp, st.out_len) == TLS_RESP_COMPLETE)) {
        resp[st.out_len] = 0;
        char line[80];
        int k = 0;
        while (k < 79 && resp[k] && resp[k] != '\r' && resp[k] != '\n') { line[k] = (char)resp[k]; k++; }
        line[k] = 0;
        printf("%-28s OK   %-24s %u bytes%s\n", h, line, st.out_len, st.psk_accepted ? " (resumed)" : "");
    } else if (r == -99) {
        printf("%-28s TIMEOUT phase=%d out=%u\n", h, st.phase, st.out_len);
    } else {
        printf("%-28s FAIL %s", h, reason(st.fail_reason));
        if (st.fail_reason == TLS_FAIL_ALERT) printf(" alert=%d", st.alert_desc);
        if (st.cert_detail) printf(" cert=%s", cvname(st.cert_detail));
        printf(" phase=%d out=%u\n", st.phase, st.out_len);
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    time_t tt = time(0);
    struct tm* g = gmtime(&tt);
    x509_time now = { 1900 + g->tm_year, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec };
    x509_set_now(&now);
    if (argc > 2 && !strcmp(argv[1], "-f")) {
        FILE* f = fopen(argv[2], "r");
        char line[256];
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n #")] = 0;
            if (line[0]) scan(line);
        }
        if (f) fclose(f);
        return 0;
    }
    for (int i = 1; i < argc; i++) scan(argv[i]);
    return 0;
}
