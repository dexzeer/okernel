// Phase 4 host test: full TLS 1.3 handshake against a Python ssl server
// on 127.0.0.1. We assume the server is already running on PORT (default
// 4433) — the test runner is expected to start it before invoking this
// binary.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "tls_client.h"

static int fails = 0;
static void ok(const char* name, int cond) {
    printf("%-32s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

static int tcp_send(const uint8_t* buf, uint32_t len, void* user) {
    int fd = (int)(intptr_t)user;
    fprintf(stderr, "[send] %u bytes:", len);
    for (uint32_t i = 0; i < len && i < 64; i++) fprintf(stderr, " %02x", buf[i]);
    if (len > 64) fprintf(stderr, " ...");
    fprintf(stderr, "\n");
    uint32_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int tcp_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    int fd = (int)(intptr_t)user;
    (void)timeout_ms;
    ssize_t n = recv(fd, buf, cap, 0);
    if (n <= 0) {
        fprintf(stderr, "[recv] n=%zd errno=%d buf=", n, errno);
        if (n > 0) for (int i = 0; i < n; i++) fprintf(stderr, "%02x", buf[i]);
        fprintf(stderr, "\n");
        return -1;
    }
    return (int)n;
}

int main(void) {
    const char* host = "127.0.0.1";
    int port = 4433;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        // Mark skip rather than fail so we can run in CI without server
        printf("connect failed — server not running, skipping test\n");
        return 0;
    }

    const char* req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    uint8_t response[16384];
    struct tls_client_io io = {
        .send = tcp_send,
        .recv = tcp_recv,
        .user = (void*)(intptr_t)fd,
    };
    int n = tls_client_run("localhost", port,
                           (const uint8_t*)req, strlen(req),
                           response, sizeof(response), &io);
    close(fd);

    ok("handshake + request + response", n > 0);
    if (n > 0) {
        response[n] = 0;
        printf("--- response (first 256 bytes) ---\n%.*s\n---\n",
               n > 256 ? 256 : n, (char*)response);
        ok("response is HTTP", memcmp(response, "HTTP/", 5) == 0);
        ok("response contains 200", memmem(response, n, "200", 3) != NULL);
        ok("response contains body", memmem(response, n, "Hello TLS 1.3", 13) != NULL);
    }

    printf("\n%s\n", fails ? "PHASE 4 FAILED" : "PHASE 4 PASS");
    return fails != 0;
}