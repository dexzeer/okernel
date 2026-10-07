// Host timing harness for TLS resumption: a full handshake, then resumed
// connections (PSK from the first one's tickets) to the same host, timing
// the ServerHello wait and the whole fetch. Diagnoses resumption-specific
// server delays.
//   gcc -m32 -O2 -Isrc/crypto -Isrc -o build-host/tls_resume_time tests/tls_resume_time.c $(HOST_CRYPTO_SRC)
//   ./build-host/tls_resume_time en.wikipedia.org [n]
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include "tls_client.h"
#include "x509.h"
#include <time.h>

static int fd = -1;
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static int s_send(const uint8_t* b, uint32_t n, void* u) {
    (void)u;
    for (uint32_t off = 0; off < n;) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) return -1;
        off += (uint32_t)w;
    }
    return 0;
}

static int s_recv(uint8_t* b, uint32_t cap, uint32_t timeout_ms, void* u) {
    (void)u; (void)timeout_ms;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = { 0, 20000 };
    if (select(fd + 1, &r, 0, 0, &tv) <= 0) return 0;
    ssize_t n = read(fd, b, cap);
    return n <= 0 ? -1 : (int)n;
}

int main(int argc, char** argv) {
    time_t tt = time(0);
    struct tm* g = gmtime(&tt);
    x509_time xt = { 1900 + g->tm_year, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec };
    x509_set_now(&xt);
    const char* host = argc > 1 ? argv[1] : "example.com";
    int n = argc > 2 ? atoi(argv[2]) : 3;
    static uint8_t out[4 << 20];
    static struct tls_state st;
    char req[512];
    int rl = snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: okernel/0.4\r\nConnection: close\r\n\r\n", host);
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "443", &hints, &res)) { printf("dns fail\n"); return 1; }
    for (int i = 0; i < n; i++) {
        double t0 = now();
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, res->ai_addr, res->ai_addrlen)) { printf("connect fail\n"); return 1; }
        double tc = now();
        tls_state_init(&st, host, 443, (const uint8_t*)req, (uint32_t)rl, out, sizeof out - 1);
        tls_state_set_now_ms(&st, (uint64_t)(now() * 1000));
        struct tls_client_io io = { s_send, s_recv, 0 };
        double tsh = 0;
        int r;
        for (;;) {
            r = tls_state_step(&st, &io);
            if (!tsh && st.phase >= TLS_PH_RECV_HS) tsh = now();
            if (r != TLS_STEP_AGAIN) break;
            if (now() - t0 > 30) { r = -99; break; }
        }
        close(fd);
        printf("conn %d: psk_offered=%d accepted=%d tcp=%.0fms sh_wait=%.0fms total=%.0fms out=%u r=%d fail=%d\n",
               i, st.offer_psk, st.psk_accepted, (tc - t0) * 1e3, tsh ? (tsh - tc) * 1e3 : -1,
               (now() - t0) * 1e3, st.out_len, r, st.fail_reason);
        fflush(stdout);
    }
    return 0;
}
