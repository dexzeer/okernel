// t_evil_blocklist.c — end-to-end local-revocation probe.
// Plants a (issuer-SPKI-hash, serial) blocklist entry, then runs a full
// TLS handshake against the evil server. Usage:
//   t_evil_blocklist PORT ISSUER_DER LEAF_DER [wrongserial]
// With wrongserial=1 the planted serial is flipped (must PASS); otherwise
// the planted entry matches the served leaf (must FAIL with reason 4).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tls_client.h"
#include "x509.h"
#include "certverify.h"
#include "sha256.h"

static int sock_fd = -1;
static int tsend(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    uint32_t sent = 0;
    while (sent < len) {
        ssize_t n = write(sock_fd, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (uint32_t)n;
    }
    return 0;
}
static int trecv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)user;
    uint32_t total = 0, waited = 0;
    static uint64_t waited_total = 0;
    while (total < cap) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 100 * 1000;
        int r = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) return -1;
        if (r == 0) {
            waited += 100; waited_total += 100;
            if (waited_total > 25000) return -1;
            if (waited >= timeout_ms) break;
            continue;
        }
        ssize_t n = read(sock_fd, buf + total, cap - total);
        if (n < 0) return -1;
        if (n == 0) return total > 0 ? (int)total : -1;
        total += (uint32_t)n;
        if (total >= cap) break;
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
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) { printf("usage: %s PORT ISSUER_DER LEAF_DER [wrong]\n", argv[0]); return 2; }
    int port = atoi(argv[1]);
    static uint8_t ider[4096], lder[8192];
    int inl = load(argv[2], ider, sizeof(ider));
    int lnl = load(argv[3], lder, sizeof(lder));
    if (inl <= 0 || lnl <= 0) { printf("load failed\n"); return 2; }
    x509_cert ic, lc;
    if (x509_parse(ider, (uint32_t)inl, &ic) != 0 ||
        x509_parse(lder, (uint32_t)lnl, &lc) != 0) { printf("parse failed\n"); return 2; }
    uint8_t ih[32];
    sha256(ic.spki.p, ic.spki.len, ih);
    static uint8_t serial[64];
    if (lc.serial.len > sizeof(serial)) { printf("serial too long\n"); return 2; }
    memcpy(serial, lc.serial.p, lc.serial.len);
    int wrong = (argc > 4 && argv[4][0] == 'w');
    if (wrong) serial[lc.serial.len - 1] ^= 0xFF; // miss the entry
    if (tls_blocklist_add(ih, serial, lc.serial.len) != 0) { printf("blocklist add failed\n"); return 2; }
    printf("[bl] planted %s entry for issuer %.8s serial_len=%u\n",
           wrong ? "DECOY" : "MATCHING", argv[3], lc.serial.len);
    time_t tt = time(NULL);
    struct tm* g = gmtime(&tt);
    x509_time now = { 1900 + g->tm_year, g->tm_mon + 1, g->tm_mday,
                      g->tm_hour, g->tm_min, g->tm_sec };
    x509_set_now(&now);
    static uint8_t rder[4096];
    int rn = load("tests/adversarial/at_root.der", rder, sizeof(rder));
    x509_cert rc;
    if (x509_parse(rder, (uint32_t)rn, &rc) != 0) return 2;
    cert_verify_trust_extra(rc.spki.p, rc.spki.len);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(0x7F000001);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) { perror("connect"); return 2; }
    sock_fd = fd;
    static char req[256];
    snprintf(req, sizeof(req),
             "GET / HTTP/1.1\r\nHost: evil.example.com\r\nConnection: close\r\n\r\n");
    static uint8_t out[262144];
    struct tls_client_io io = { .send = tsend, .recv = trecv, .user = 0 };
    int n = tls_client_run("evil.example.com", (uint16_t)port,
                           (const uint8_t*)req, (uint32_t)strlen(req),
                           out, sizeof(out), &io);
    int fr = tls_last_fail_reason();
    printf("[bl] run done: n=%d fail_reason=%d\n", n, fr);
    close(fd);
    if (!wrong) {
        if (n < 0 && fr == 4) { printf("BLOCKLIST: PASS (revoked refused)\n"); return 0; }
        printf("BLOCKLIST: FAIL (revoked connection not refused!)\n");
        return 1;
    }
    if (n > 0) { printf("BLOCKLIST-DECOY: PASS (unlisted serial connects)\n"); return 0; }
    printf("BLOCKLIST-DECOY: FAIL (decoy blocked legit connection!)\n");
    return 1;
}
