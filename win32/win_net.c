// okai on Windows -- TCP and DNS over Winsock.
//
// fetch.c / tls_client.c / h2.c drive the kernel's tiny socket API
// (tcp_open/send/send_space/recv/rx_avail/state/failed/close/abort and
// dns_lookup, all declared in src/net/network.h). This file implements that
// API on real sockets, so the TLS 1.3 stack, HTTP/2 and the fetch engine are
// the same code the kernel runs -- only the wire is the OS's.
//
// Nothing here blocks the caller: connects are non-blocking and polled,
// sends go into a per-socket ring, DNS runs on its own thread.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "net/network.h"
#include "crypto/tls_client.h"     // struct http_framing / http_parse_framing
#include "win_plat.h"

#define NSOCK      40
#define SEND_CAP   (128u * 1024)
#define LINGER_MS  4000

struct wsock {
    SOCKET s;
    int used;
    int state;            // TCP_STATE_*
    int failed;
    int eof;
    int closing;          // tcp_close: draining the FIN handshake
    uint32_t close_t;
    uint8_t* sbuf;
    uint32_t s_head, s_tail;   // ring: [head, tail)
};

static struct wsock socks[NSOCK];
static int net_up;

static uint32_t now_ms(void) { return (uint32_t)GetTickCount64(); }

static struct wsock* S(int id) {
    if (id < 0 || id >= NSOCK || !socks[id].used) return 0;
    return &socks[id];
}

static int send_flush(struct wsock* k) {
    while (k->s_head != k->s_tail) {
        uint32_t n = k->s_tail - k->s_head;
        uint32_t first = SEND_CAP - (k->s_head % SEND_CAP);
        if (first > n) first = n;
        int sent = send(k->s, (const char*)k->sbuf + (k->s_head % SEND_CAP), (int)first, 0);
        if (sent > 0) { k->s_head += (uint32_t)sent; continue; }
        if (sent == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 0;
            k->failed = 1;
            return -1;
        }
        return 0;
    }
    return 0;
}

static void sock_release(struct wsock* k) {
    if (k->s) closesocket(k->s);
    if (k->sbuf) free(k->sbuf);
    memset(k, 0, sizeof *k);
}

static void sock_check_connect(struct wsock* k) {
    if (k->state != TCP_STATE_SYN_SENT) return;
    fd_set wf, ef;
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(k->s, &wf);
    FD_SET(k->s, &ef);
    struct timeval tv = { 0, 0 };
    int r = select(0, 0, &wf, &ef, &tv);
    if (r <= 0) return;
    if (FD_ISSET(k->s, &ef)) { k->failed = 1; return; }
    if (FD_ISSET(k->s, &wf)) {
        int err = 0, len = sizeof err;
        if (getsockopt(k->s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err) { k->failed = 1; return; }
        k->state = TCP_STATE_ESTABLISHED;
    }
}

// Poll every socket: finish connects, push queued bytes, notice a peer FIN.
void win_net_pump(void) {
    for (int i = 0; i < NSOCK; i++) {
        struct wsock* k = &socks[i];
        if (!k->used) continue;
        if (k->closing) {
            if ((int32_t)(now_ms() - k->close_t) > LINGER_MS) { sock_release(k); continue; }
            char tmp[512];
            int n = recv(k->s, tmp, sizeof tmp, 0);
            if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) { sock_release(k); continue; }
            continue;
        }
        if (k->failed) continue;
        sock_check_connect(k);
        if (k->state == TCP_STATE_ESTABLISHED && k->s_head != k->s_tail) send_flush(k);
    }
}

// The peer half-closed and nothing is left: the kernel's sockets turn that
// into "closed" once the buffered bytes are read.
static void sock_check_eof(struct wsock* k) {
    if (k->eof || k->state != TCP_STATE_ESTABLISHED) return;
    u_long avail = 0;
    if (ioctlsocket(k->s, FIONREAD, &avail) != 0) { k->failed = 1; return; }
    if (avail) return;
    char c;
    int n = recv(k->s, &c, 1, MSG_PEEK);
    if (n == 0) { k->eof = 1; k->state = TCP_STATE_CLOSED; return; }
    if (n < 0) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) k->failed = 1;
    }
}

int tcp_open(uint32_t dst_ip, uint16_t dst_port) {
    int id = -1;
    for (int i = 0; i < NSOCK; i++) if (!socks[i].used) { id = i; break; }
    if (id < 0) return -1;
    struct wsock* k = &socks[id];
    memset(k, 0, sizeof *k);
    k->sbuf = (uint8_t*)malloc(SEND_CAP);
    if (!k->sbuf) return -1;
    k->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (k->s == INVALID_SOCKET) { free(k->sbuf); memset(k, 0, sizeof *k); return -1; }
    u_long nb = 1;
    ioctlsocket(k->s, FIONBIO, &nb);
    int one = 1;
    setsockopt(k->s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    k->used = 1;
    k->state = TCP_STATE_SYN_SENT;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(dst_port);
    a.sin_addr.s_addr = htonl(dst_ip);
    if (connect(k->s, (struct sockaddr*)&a, sizeof a) == 0) k->state = TCP_STATE_ESTABLISHED;
    else if (WSAGetLastError() != WSAEWOULDBLOCK) k->failed = 1;
    return id;
}

int tcp_state(int id) {
    struct wsock* k = S(id);
    if (!k) return TCP_STATE_CLOSED;
    if (k->failed) return TCP_STATE_CLOSED;
    sock_check_connect(k);
    if (k->state != TCP_STATE_ESTABLISHED) return k->failed ? TCP_STATE_CLOSED : k->state;
    sock_check_eof(k);
    return k->state;
}

uint32_t tcp_send_space(int id) {
    struct wsock* k = S(id);
    if (!k || k->failed || k->state != TCP_STATE_ESTABLISHED) return 0;
    if (k->s_head != k->s_tail) send_flush(k);
    uint32_t used = k->s_tail - k->s_head;
    return used < SEND_CAP ? SEND_CAP - used : 0;
}

int tcp_send(int id, const uint8_t* data, uint32_t len) {
    struct wsock* k = S(id);
    if (!k || k->failed || k->state != TCP_STATE_ESTABLISHED || !len) return -1;
    if (k->s_tail - k->s_head + len > SEND_CAP) return -1;
    for (uint32_t i = 0; i < len; i++) k->sbuf[(k->s_tail + i) % SEND_CAP] = data[i];
    k->s_tail += len;
    if (send_flush(k) < 0) return -1;
    return 0;
}

int tcp_rx_avail(int id) {
    struct wsock* k = S(id);
    if (!k || k->failed) return 0;
    u_long avail = 0;
    if (ioctlsocket(k->s, FIONREAD, &avail) != 0) return 0;
    return (int)(avail > 0x7FFFFFFF ? 0x7FFFFFFF : avail);
}

int tcp_recv(int id, uint8_t* buf, uint32_t cap) {
    struct wsock* k = S(id);
    if (!k) return -1;
    if (k->failed) return -1;
    if (!cap) return 0;
    if (k->s_head != k->s_tail) send_flush(k);
    int n = recv(k->s, (char*)buf, (int)cap, 0);
    if (n > 0) return n;
    if (n == 0) { k->eof = 1; k->state = TCP_STATE_CLOSED; return -1; }
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return 0;
    k->failed = 1;
    return -1;
}

int tcp_failed(int id) {
    struct wsock* k = S(id);
    return k ? k->failed : 1;
}

void tcp_close(int id) {
    struct wsock* k = S(id);
    if (!k) return;
    if (k->s_head != k->s_tail) send_flush(k);
    shutdown(k->s, SD_SEND);
    k->state = TCP_STATE_CLOSED;
    k->closing = 1;
    k->close_t = now_ms();
    // The id is dead for the caller; the slot keeps draining the FIN
    // handshake, then frees itself (win_net_pump).
    k->used = 2;
}

void tcp_abort(int id) {
    struct wsock* k = S(id);
    if (!k) return;
    struct linger lg = { 1, 0 };
    setsockopt(k->s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof lg);
    sock_release(k);
}

// ---- DNS -------------------------------------------------------------------
//
// getaddrinfo runs on a worker thread: fetch.c polls dns_lookup() each pass and
// must never wait for a resolver.

#define DNS_SLOTS 8
#define DNS_CACHE 32
#define DNS_TTL_MS 60000

struct dnsreq {
    int busy;
    char host[256];
    uint32_t ip;
    int result;          // 0 pending, 1 ok, -1 failed
    uint32_t started;
};

static struct dnsreq dq[DNS_SLOTS];
static CRITICAL_SECTION dq_lock;
static HANDLE dq_event;
static int dq_init;

static struct { char host[256]; uint32_t ip; uint32_t expires; } dcache[DNS_CACHE];
static int dcache_next;

static uint32_t cache_get(const char* host) {
    uint32_t t = now_ms();
    for (int i = 0; i < DNS_CACHE; i++)
        if (dcache[i].expires && (int32_t)(dcache[i].expires - t) > 0 && !strcmp(dcache[i].host, host))
            return dcache[i].ip ? dcache[i].ip : 0;
    return 0;
}

static void cache_put(const char* host, uint32_t ip) {
    if (!ip) return;
    int i = dcache_next;
    dcache_next = (dcache_next + 1) % DNS_CACHE;
    snprintf(dcache[i].host, sizeof dcache[i].host, "%s", host);
    dcache[i].ip = ip;
    dcache[i].expires = now_ms() + DNS_TTL_MS;
}

static DWORD WINAPI dns_thread(void* arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(dq_event, INFINITE);
        for (;;) {
            int slot = -1;
            char host[256];
            EnterCriticalSection(&dq_lock);
            for (int i = 0; i < DNS_SLOTS; i++)
                if (dq[i].busy && dq[i].result == 0) { slot = i; break; }
            if (slot >= 0) snprintf(host, sizeof host, "%s", dq[slot].host);
            LeaveCriticalSection(&dq_lock);
            if (slot < 0) break;

            uint32_t ip = 0;
            struct addrinfo hints, *res = 0;
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, 0, &hints, &res) == 0 && res) {
                struct sockaddr_in* a = (struct sockaddr_in*)res->ai_addr;
                ip = ntohl(a->sin_addr.s_addr);   // network -> host order, like dns_lookup
                freeaddrinfo(res);
            }
            if (ip) cache_put(host, ip);
            EnterCriticalSection(&dq_lock);
            dq[slot].ip = ip;
            dq[slot].result = ip ? 1 : -1;
            LeaveCriticalSection(&dq_lock);
            if (ip) win_logf("[dns] %s -> %u.%u.%u.%u\n", host,
                             ip >> 24 & 0xFF, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
            else win_logf("[dns] %s -> FAILED\n", host);
        }
    }
    return 0;
}

void win_net_init(void) {
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { win_logf("[okai-win] WSAStartup failed\n"); return; }
    net_up = 1;
    InitializeCriticalSection(&dq_lock);
    dq_event = CreateSemaphoreW(0, 0, 64, 0);
    CreateThread(0, 128 * 1024, dns_thread, 0, 0, 0);
    win_logf("[okai-win] winsock ready\n");
}

void win_net_shutdown(void) {
    for (int i = 0; i < NSOCK; i++) if (socks[i].used) sock_release(&socks[i]);
    WSACleanup();
}

int net_parse_ip(const char* s, uint32_t* out) {
    uint32_t a, b, c, d;
    char trail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &trail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

int dns_lookup(const char* host, uint32_t* ip) {
    uint32_t lit;
    if (net_parse_ip(host, &lit)) { *ip = lit; return 1; }
    uint32_t c = cache_get(host);
    if (c) { *ip = c; return 1; }
    if (!net_up) { *ip = 0; return -1; }
    uint32_t t = now_ms();
    EnterCriticalSection(&dq_lock);
    for (int i = 0; i < DNS_SLOTS; i++) {
        if (!dq[i].busy || strcmp(dq[i].host, host)) continue;
        int r = dq[i].result;
        if (r == 0 && (int32_t)(t - dq[i].started) > 15000) { r = dq[i].result = -1; }
        if (r > 0) *ip = dq[i].ip;
        LeaveCriticalSection(&dq_lock);
        return r;
    }
    int slot = -1;
    for (int i = 0; i < DNS_SLOTS; i++) if (!dq[i].busy) { slot = i; break; }
    if (slot < 0) { LeaveCriticalSection(&dq_lock); *ip = 0; return -1; }
    dq[slot].busy = 1;
    dq[slot].result = 0;
    dq[slot].ip = 0;
    dq[slot].started = t;
    snprintf(dq[slot].host, sizeof dq[slot].host, "%s", host);
    LeaveCriticalSection(&dq_lock);
    ReleaseSemaphore(dq_event, 1, 0);
    *ip = 0;
    return 0;
}

int dns_resolve(const char* hostname) {
    uint32_t ip;
    int r = dns_lookup(hostname, &ip);
    if (r == 1) win_logf("[dns] Resolved: %u.%u.%u.%u\n", ip >> 24 & 0xFF, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
    return r;
}

// ---- HTTP de-framing (network.c's function, unchanged) ----------------------

int http_dechunk(char* buf, int len) {
    int body = -1;
    for (int i = 0; i < len - 3; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            body = i + 4;
            break;
        }
    }
    if (body < 0) return len;

    int chunked = 0;
    {
        struct http_framing fr;
        http_parse_framing((const uint8_t*)buf, (uint32_t)body - 2, &fr);
        chunked = !fr.malformed && fr.chunked;
    }

    int out = len - body;
    memmove(buf, buf + body, (size_t)out);
    len = out;
    if (!chunked) return len;

    int rd = 0, wr = 0;
    int first_chunk = 1;
    while (rd < len) {
        int size = 0, digits = 0;
        while (rd < len) {
            char c = buf[rd];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            if (size > 0x07FFFFFF) size = 0x7FFFFFFF;
            else size = size * 16 + v;
            digits++;
            rd++;
        }
        if (!digits && first_chunk) return len;
        if (!digits) break;
        first_chunk = 0;
        while (rd < len && buf[rd] != '\n') rd++;
        if (rd < len) rd++;
        if (size == 0) break;
        for (int i = 0; i < size && rd < len; i++) buf[wr++] = buf[rd++];
        if (rd < len && buf[rd] == '\r') rd++;
        if (rd < len && buf[rd] == '\n') rd++;
    }
    buf[wr] = 0;
    return wr;
}
