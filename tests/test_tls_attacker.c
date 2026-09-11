// Interop driver: our TLS client against an INDEPENDENT server
// (openssl s_server, python ssl, or a raw TLS 1.3 implementation).
// Usage: test_tls_attacker [port] [twice] [root.der]
//   twice = run two sequential connections; if the server accepts the
//   ticket from round 1, round 2 abbreviates (no Certificate flight).
// Serves as: full handshake with a real third-party flight (cert/CV/
// Finished built by someone else's code), hostname+chain verify against
// our test PKI via the host-only trust hook, then GET + body.
// Env: TLSA_PORT (default 4443), TLSA_ROOT (root override),
//   TLSA_SLEEP=n (pause n seconds before round 2, for ticket-age tests).
// NOTE: the trust hook holds ONE root per process — pick per chain.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tls_client.h"
#include "x509.h"
#include "certverify.h"

static int sock_fd = -1;

static int tlsa_send(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    uint32_t sent = 0;
    while (sent < len) {
        ssize_t n = write(sock_fd, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (uint32_t)n;
    }
    return 0;
}

static int tlsa_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)user;
    // Overall deadline: this synchronous driver must never spin forever —
    // cap total wait at ~25s, then fail closed (return -1 = transport error).
    static uint64_t waited_total = 0;
    uint32_t total = 0, waited = 0;
    while (total < cap) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(sock_fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 100 * 1000;
        int r = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) return -1;
        if (r == 0) {
            waited += 100;
            waited_total += 100;
            if (waited_total > 25000) return -1; // overall deadline
            if (waited >= timeout_ms) break;
            continue;
        }
        ssize_t n = read(sock_fd, buf + total, cap - total);
        if (n < 0) return -1;
        if (n == 0) return total > 0 ? (int)total : -1;
        total += (uint32_t)n;
        if (total >= cap) break;
        // Small extra drain window for coalesced flight records.
        tv.tv_sec = 0; tv.tv_usec = 50 * 1000;
        FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
        if (select(sock_fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
    }
    return (int)total;
}

static int load(const char* path, uint8_t* out, uint32_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(out, 1, cap, f);
    fclose(f);
    return n;
}

// Fresh TCP connection per round (resumption test needs sequential
// connections sharing only the in-process ticket store).
static int new_conn(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(0x7F000001);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    sock_fd = fd;
    return fd;
}

// Local compare: the kernel's src/string.h shadows libc <string.h> under
// -Isrc, so strcmp has no declaration here (same reason memcpy/memset come
// from the kernel header). Avoid the implicit-declaration trap entirely.
static int arg_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int port = 4443;
    if (argc > 1) port = atoi(argv[1]);
    if (getenv("TLSA_PORT")) port = atoi(getenv("TLSA_PORT"));

    // Fail-closed clock from wall time.
    {
        time_t tt = time(NULL);
        struct tm* g = gmtime(&tt);
        x509_time now = { 1900 + g->tm_year, g->tm_mon + 1, g->tm_mday,
                          g->tm_hour, g->tm_min, g->tm_sec };
        x509_set_now(&now);
        printf("[tlsa] clock: %04d-%02d-%02d\n", now.year, now.month, now.day);
    }
    // Trust a test root (host-only hook — never in production). The hook
    // holds ONE SPKI hash, so one root per process: argv[3] (or TLSA_ROOT)
    // selects it, defaulting to the P-256 chain root. Run P-384 chains as
    // ./t_tlsa 4443 twice tests/adversarial/at_p384_root.der
    const char* troot = "tests/adversarial/at_root.der";
    if (argc > 3) troot = argv[3];
    if (getenv("TLSA_ROOT")) troot = getenv("TLSA_ROOT");
    static uint8_t rder[4096];
    int rn = load(troot, rder, sizeof(rder));
    if (rn <= 0) { printf("[tlsa] no root fixture %s\n", troot); return 2; }
    x509_cert rc;
    if (x509_parse(rder, (uint32_t)rn, &rc) != 0) {
        printf("[tlsa] root parse failed %s\n", troot);
        return 2;
    }
    cert_verify_trust_extra(rc.spki.p, rc.spki.len);

    static const char req[] =
        "GET / HTTP/1.1\r\nHost: evil.example.com\r\nConnection: close\r\n\r\n";
    static uint8_t out[262144];
    struct tls_client_io io = { .send = tlsa_send, .recv = tlsa_recv, .user = 0 };
    int rounds = (argc > 2 && arg_eq(argv[2], "twice")) ? 2 : 1;
    int dro = 0;
    for (int round = 0; round < rounds; round++) {
        if (round == 1 && getenv("TLSA_SLEEP")) {
            int s = atoi(getenv("TLSA_SLEEP"));
            if (s > 0 && s < 30) { printf("[tlsa] sleeping %ds before round 2\n", s); sleep((unsigned)s); }
        }
        sock_fd = new_conn(port);
        if (sock_fd < 0) { perror("[tlsa] connect"); return 2; }
        printf("[tlsa] TCP connected to 127.0.0.1:%d (round %d)\n", port, round + 1);
        int n = tls_client_run("evil.example.com", (uint16_t)port,
                               (const uint8_t*)req, (uint32_t)sizeof(req) - 1,
                               out, sizeof(out), &io);
        int fr = tls_last_fail_reason();
        printf("[tlsa] run done: n=%d fail_reason=%d\n", n, fr);
        if (n > 0) {
            printf("[tlsa] first bytes: %.60s\n", out);
            printf("[tlsa] ticket cached: %s\n",
                   tls_ticket_have("evil.example.com") ? "yes" : "no");
        }
        close(sock_fd); sock_fd = -1;
        if (n < 0) {
            printf("TLSA-INTEROP: FAIL (round %d)\n", round + 1);
            dro = 1; break;
        }
    }
    if (!dro) printf("TLSA-INTEROP: PASS%s\n", rounds == 2 ? " x2" : "");
    return dro;
}
