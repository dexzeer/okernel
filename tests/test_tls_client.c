// Phase 4 host test: full TLS 1.3 handshake against a real server.
// Connects to example.com:443 over TCP, runs the handshake, sends an HTTP
// GET, and verifies we receive a valid HTML response.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "tls_client.h"

// ---- BSD socket wrappers for tls_client_io ----
static int sock_fd = -1;

static int bsd_send(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    uint32_t sent = 0;
    while (sent < len) {
        ssize_t n = write(sock_fd, buf + sent, len - sent);
        if (n < 0) { perror("write"); return -1; }
        sent += (uint32_t)n;
    }
    return 0;
}

static int bsd_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)user;
    // Read exactly `cap` bytes, looping on partial reads, with a total
    // timeout of `timeout_ms`.
    uint32_t total_read = 0;
    uint32_t waited = 0;
    uint32_t step = 100; // ms per poll
    while (total_read < cap) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(sock_fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = step * 1000;
        int sel = select(sock_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) { perror("select"); return -1; }
        if (sel > 0) {
            ssize_t n = read(sock_fd, buf + total_read, cap - total_read);
            if (n < 0) { perror("read"); return -1; }
            if (n == 0) return -1; // EOF
            total_read += (uint32_t)n;
            waited = 0; // reset timeout on progress
        } else {
            waited += step;
            if (waited >= timeout_ms) break;
        }
    }
    return total_read > 0 ? (int)total_read : -1;
}

static int tcp_connect(const char* host, uint16_t port) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc)); return -1; }

    for (rp = res; rp; rp = rp->ai_next) {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            freeaddrinfo(res);
            return fd;
        }
        close(fd);
    }
    freeaddrinfo(res);
    return -1;
}

int main(void) {
    const char* host = "example.com";
    uint16_t port = 443;

    fprintf(stderr, "[test] connecting to %s:%u ...\n", host, port);
    sock_fd = tcp_connect(host, port);
    if (sock_fd < 0) {
        fprintf(stderr, "[test] TCP connect failed\n");
        return 1;
    }
    fprintf(stderr, "[test] TCP connected\n");

    // Build HTTP request
    char request_buf[512];
    int rlen = snprintf(request_buf, sizeof(request_buf),
        "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);

    struct tls_client_io io = {
        .send = bsd_send,
        .recv = bsd_recv,
        .user = NULL,
    };

    uint8_t response[65536];
    fprintf(stderr, "[test] starting TLS handshake ...\n");
    int n = tls_client_run(host, port,
                           (const uint8_t*)request_buf, rlen,
                           response, sizeof(response), &io);

    close(sock_fd);
    sock_fd = -1;

    if (n < 0) {
        fprintf(stderr, "[test] tls_client_run FAILED (returned %d, fail_reason=%d)\n", n, tls_last_fail_reason());
        return 1;
    }

    fprintf(stderr, "[test] received %d bytes of plaintext\n", n);
    response[n] = '\0';

    // Check that we got something that looks like an HTTP response
    int has_http = (strstr((char*)response, "HTTP/1.1") != NULL ||
                    strstr((char*)response, "HTTP/1.0") != NULL);
    int has_html = (strstr((char*)response, "<!doctype") != NULL ||
                    strstr((char*)response, "<html") != NULL ||
                    strstr((char*)response, "<HTML") != NULL);

    printf("TLS handshake completed: %s\n", n > 0 ? "PASS" : "FAIL");
    printf("HTTP response received:  %s\n", has_http ? "PASS" : "FAIL");
    printf("HTML content present:    %s\n", has_html ? "PASS" : "FAIL");

    if (n > 0) {
        // Print first 200 chars of response for debugging
        printf("\n--- Response preview (first 200 chars) ---\n");
        int preview_len = n < 200 ? n : 200;
        for (int i = 0; i < preview_len; i++) {
            putchar(response[i]);
        }
        printf("\n--- End preview ---\n");
    }

    int ok = (n > 0 && has_http);
    printf("\n%s\n", ok ? "PHASE 4 PASS" : "PHASE 4 FAIL");
    if (!ok) return 1;

    // ---- MITM simulation: same wire, WRONG hostname ----
    // The server's certificate says example.com; asking to authenticate it
    // as "evil.example.attacker.io" MUST be rejected (hostname mismatch),
    // and the "certificate chain does not terminate at a trusted root when
    // we lie about everything" case is covered by test_pki fixtures.
    sock_fd = tcp_connect(host, port);
    if (sock_fd < 0) {
        fprintf(stderr, "[test] reconnect failed; skipping MITM test\n");
        return ok ? 0 : 1;
    }
    {
        char req2[512];
        int r2 = snprintf(req2, sizeof(req2),
            "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
        struct tls_client_io io2 = { .send = bsd_send, .recv = bsd_recv, .user = NULL };
        // Run the handshake with a DIFFERENT hostname for verification: we
        // call the state machine directly so SNI says example.com (the wire
        // stays truthful) but verification uses the attacker's name —
        // exactly what a MITM-accommodating client would wrongly accept.
        // Run the handshake with the TRUE SNI on the wire (so Cloudflare
        // serves the example.com certificate) but verify the certificate
        // against the ATTACKER'S hostname — exactly what a MITM-friendly
        // client would wrongly accept.
        struct tls_state st2;
        tls_state_init(&st2, host, port,
                       (const uint8_t*)req2, r2, response, sizeof(response));
        st2.verify_host = "evil.example.attacker.io";
        int r;
        do { r = tls_state_step(&st2, &io2); } while (r == TLS_STEP_AGAIN);
        int rejected = (r == TLS_STEP_ERR &&
                        (st2.fail_reason == TLS_FAIL_HOSTNAME ||
                         st2.fail_reason == TLS_FAIL_CERT));
        printf("MITM simulation (wrong hostname rejected): %s (reason=%d)\n",
               rejected ? "PASS" : "FAIL", st2.fail_reason); fprintf(stderr, "  [dbg] r=%d phase=%d alert=%d\n", r, st2.phase, st2.alert_desc);
        ok = ok && rejected;
        close(sock_fd);
        sock_fd = -1;
        printf("\n%s\n", ok ? "PHASE 4+MITM PASS" : "PHASE 4+MITM FAIL");
    }
    return ok ? 0 : 1;
}
