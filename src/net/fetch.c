// HTTP/HTTPS fetch engine (see fetch.h).
//
// Slots are requests; connections are pooled sockets (+ a TLS session for
// https). A waiting slot takes an idle connection to its origin when one
// exists (keep-alive), else opens a new one within the per-host and total
// limits. A connection walks DNS -> TCP -> (TLS handshake, which sends the
// request) -> response; a response complete by HTTP framing on a
// keep-alive-capable connection leaves it idle in the pool.
//
// Every TLS session and socket is independent; the TLS client's scratch
// buffers are only used within one step, so steps of different sessions
// may interleave freely (fetch_poll runs them one at a time).

#include "fetch.h"
#include "network.h"
#include "../memory.h"
#include "../string.h"
#include "../serial.h"
#include "../crypto/tls_client.h"

extern uint32_t tick_count;   // 100 Hz

#define CONN_MAX       8          // pooled connections (sockets: TCP_MAX_SOCKS)
#define CONN_PER_HOST  6          // like browsers
#define RESP_INIT      (64 * 1024)
#define RESP_MAX       (8 * 1024 * 1024)
#define RESP_HEADROOM  (TLS_RECORD_MAX_PAYLOAD + 64)
#define IDLE_TICKS     3000       // keep-alive: close after 30s unused
#define CONNECT_TICKS  1500       // DNS + TCP connect bound (15s)
#define STALL_TICKS    2500       // nothing received for 25s -> fail
#define STEP_TICKS     3          // TLS work per connection per poll (~30ms)

enum { CN_FREE = 0, CN_DNS, CN_TCP, CN_RUN, CN_IDLE };
enum { SL_FREE = 0, SL_WAIT, SL_RUN, SL_DONE, SL_FAIL };

struct fconn {
    int st;
    int https;
    uint16_t port;
    char host[128];
    uint32_t ip;
    int sock;
    struct tls_state* tls;        // heap, kept across reuse of this pool entry
    struct tls_client_io io;
    int slot;                     // request being served, or -1
    uint32_t t0;                  // tick the current state began
    uint32_t t_progress;          // last tick bytes arrived / phase moved
    int served;                   // responses completed on this connection
};

struct fslot {
    int st;
    uint32_t seq;                 // begin order (FIFO assignment)
    int https;
    uint16_t port;
    char host[128];
    char what[100];               // host + path prefix, for logs
    uint8_t* req;
    uint32_t req_len;
    char* resp;
    uint32_t resp_len, resp_cap;
    int conn;
    int fail_reason, fail_detail, offer_unaccepted, saw_close;
    int reused, stale_retried;
    uint32_t t_start;
};

static struct fconn conns[CONN_MAX];
static struct fslot slots[FETCH_MAX];
static uint32_t slot_seq;

static int io_send(const uint8_t* buf, uint32_t len, void* user) {
    struct fconn* c = (struct fconn*)user;
    return tcp_send(c->sock, buf, len) == 0 ? 0 : -1;
}

static int io_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)timeout_ms;   // never blocks: 0 = nothing yet, -1 = end of stream
    struct fconn* c = (struct fconn*)user;
    return tcp_recv(c->sock, buf, cap);
}

static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int host_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

// ---- response buffer -------------------------------------------------------------

// Make room for `need` more bytes (+1 for the terminating NUL). Grows
// geometrically up to RESP_MAX; returns -1 when it cannot.
static int resp_reserve(struct fslot* s, uint32_t need) {
    if (s->resp && s->resp_cap - s->resp_len > need) return 0;
    uint32_t want = s->resp_cap ? s->resp_cap : RESP_INIT;
    while (want - s->resp_len <= need && want < RESP_MAX) want *= 2;
    if (want > RESP_MAX) want = RESP_MAX;
    if (want <= s->resp_cap) return -1;
    char* n = (char*)krealloc(s->resp, want);
    if (!n) return -1;
    s->resp = n;
    s->resp_cap = want;
    return s->resp_cap - s->resp_len > need ? 0 : -1;
}

// ---- HTTP message framing ----------------------------------------------------------

static int header_end(const char* r, uint32_t n) {
    for (uint32_t i = 0; i + 3 < n; i++)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') return (int)i + 4;
    return -1;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// Does header `name` (lowercase) carry `token` (lowercase) in its value?
static int header_has(const char* r, int he, const char* name, const char* token) {
    int nl = (int)strlen(name), tl = (int)strlen(token);
    for (int i = 0; i < he; i++) {
        if (i > 0 && r[i - 1] != '\n') continue;
        int k = 0;
        while (k < nl && i + k < he && lc(r[i + k]) == name[k]) k++;
        if (k != nl || i + k >= he || r[i + k] != ':') continue;
        int e = i + k + 1;
        while (e < he && r[e] != '\r' && r[e] != '\n') e++;
        for (int p = i + k + 1; p + tl <= e; p++) {
            int m = 0;
            while (m < tl && lc(r[p + m]) == token[m]) m++;
            if (m == tl) return 1;
        }
    }
    return 0;
}

// 1 = the message is complete (by Content-Length / chunked framing, or a
// body-less status); *ka = the connection may carry another request.
// 2 = close-delimited body (read to EOF, no reuse). 0 = need more bytes.
static int message_state(const char* r, uint32_t n, int* ka) {
    *ka = 0;
    int he = header_end(r, n);
    if (he < 0) return 0;
    int status = 0;
    if (n > 12 && r[0] == 'H' && r[8] == ' ')
        status = (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
    int http11 = n > 8 && r[5] == '1' && r[7] == '1';
    int body_less = status == 204 || status == 304;
    int v = body_less ? TLS_RESP_COMPLETE : tls_response_complete((const uint8_t*)r, n);
    if (v == TLS_RESP_UNKNOWN) return 2;
    if (v != TLS_RESP_COMPLETE) return 0;
    *ka = http11 && !header_has(r, he, "connection", "close");
    return 1;
}

// ---- connections ---------------------------------------------------------------------

static void conn_close(struct fconn* c, int abort_it) {
    if (c->st == CN_FREE) return;
    if (c->sock >= 0) {
        if (abort_it) tcp_abort(c->sock);
        else tcp_close(c->sock);
    }
    c->sock = -1;
    if (c->tls) tls_state_wipe(c->tls);
    c->st = CN_FREE;
    c->slot = -1;
}

void fetch_close_idle(void) {
    for (int i = 0; i < CONN_MAX; i++)
        if (conns[i].st == CN_IDLE) conn_close(&conns[i], 0);
}

static void slot_finish(struct fslot* s, struct fconn* c, int keep) {
    s->st = SL_DONE;
    if (s->resp) s->resp[s->resp_len] = 0;
    s->saw_close = (c->https && c->tls) ? c->tls->saw_close : 0;
    s->conn = -1;
    c->served++;
    if (c->https)
        serial_printf("[tls-net] received %u bytes (%s, %ums)%s\n", s->resp_len, s->what,
                      (tick_count - s->t_start) * 10, s->reused ? " [reused]" : "");
    else
        serial_printf("[http] received %u bytes (%s, %ums)%s\n", s->resp_len, s->what,
                      (tick_count - s->t_start) * 10, s->reused ? " [reused]" : "");
    if (keep && c->st == CN_RUN && !tcp_failed(c->sock) && tcp_state(c->sock) == TCP_STATE_ESTABLISHED) {
        c->st = CN_IDLE;
        c->slot = -1;
        c->t0 = tick_count;
    } else {
        conn_close(c, 0);
    }
}

// A reused connection that dies before the first response byte was closed
// by the server while idle (keep-alive timeout race): the request was
// never processed — run it again on a fresh connection.
static int slot_retry_stale(struct fslot* s, struct fconn* c) {
    if (!s->reused || s->stale_retried || s->resp_len != 0) return 0;
    serial_printf("[fetch] stale keep-alive connection for %s, retrying fresh\n", s->what);
    conn_close(c, 1);
    s->stale_retried = 1;
    s->reused = 0;
    s->conn = -1;
    s->st = SL_WAIT;
    return 1;
}

static void slot_fail(struct fslot* s, struct fconn* c, int reason, const char* why) {
    if (c && slot_retry_stale(s, c)) return;
    s->st = SL_FAIL;
    s->fail_reason = reason;
    if (c && c->https && c->tls && reason) {
        s->fail_detail = c->tls->cert_detail;
        s->offer_unaccepted = c->tls->offer_psk && !c->tls->psk_accepted;
        serial_printf("[tls-net] handshake/download FAILED (reason=%d detail=%d phase=%d) %s\n",
                      reason, s->fail_detail, c->tls->phase, s->what);
    } else {
        serial_printf("[%s] fetch failed: %s (%s)\n", s->https ? "tls-net" : "http", why, s->what);
    }
    s->conn = -1;
    if (c) conn_close(c, 1);
}

// TLS session for pool entry c (allocated once, reused).
static int conn_tls(struct fconn* c) {
    if (!c->tls) c->tls = (struct tls_state*)kmalloc(sizeof(struct tls_state));
    return c->tls ? 0 : -1;
}

// Point the TLS output at the slot's (possibly regrown) buffer.
static void tls_bind_out(struct fconn* c, struct fslot* s) {
    resp_reserve(s, RESP_HEADROOM);
    c->tls->out = (uint8_t*)s->resp;
    c->tls->out_cap = s->resp_cap ? s->resp_cap - 1 : 0;
}

static void conn_start_request(struct fconn* c, struct fslot* s) {
    if (c->https) {
        if (conn_tls(c) != 0 || resp_reserve(s, RESP_HEADROOM) != 0) {
            slot_fail(s, c, 0, "out of memory");
            return;
        }
        tls_state_init(c->tls, c->host, c->port, s->req, s->req_len,
                       (uint8_t*)s->resp, s->resp_cap - 1);
        tls_state_set_now_ms(c->tls, (uint64_t)tick_count * 10);
        serial_printf("[tls-net] TCP established, handshake... (%s)\n", s->what);
    } else {
        if (tcp_send(c->sock, s->req, s->req_len) != 0) { slot_fail(s, c, 0, "send failed"); return; }
        serial_printf("[http] sending GET to %s\n", s->what);
    }
    c->st = CN_RUN;
    c->t0 = c->t_progress = tick_count;
}

// Drive one running connection.
static void conn_run(struct fconn* c) {
    struct fslot* s = &slots[c->slot];
    uint32_t t0 = tick_count;
    if (c->https) {
        for (int iter = 0; iter < 512; iter++) {
            int avail = tcp_rx_avail(c->sock);
            int phase = c->tls->phase;
            uint32_t before = c->tls->out_len;
            tls_bind_out(c, s);
            int r = tls_state_step(c->tls, &c->io);
            s->resp_len = c->tls->out_len;
            if (r == TLS_STEP_ERR) {
                slot_fail(s, c, c->tls->fail_reason ? c->tls->fail_reason : TLS_FAIL_PROTO, "tls");
                return;
            }
            if (avail > 0 || c->tls->phase != phase) c->t_progress = tick_count;
            if (r == TLS_STEP_DONE) {
                // End of stream (EOF at a record boundary / close_notify).
                if (s->resp_len == 0) {
                    if (slot_retry_stale(s, c)) return;
                    slot_fail(s, c, 0, "connection closed before a response");
                    return;
                }
                slot_finish(s, c, 0);
                return;
            }
            if (c->tls->phase == TLS_PH_RECV_BODY && s->resp_len != before) {
                int ka = 0;
                int ms = message_state(s->resp, s->resp_len, &ka);
                if (ms == 1) { slot_finish(s, c, ka); return; }
            }
            if (avail == 0 && c->tls->phase == phase) break;   // waiting for bytes
            if ((uint32_t)(tick_count - t0) >= STEP_TICKS) break;
        }
    } else {
        for (int iter = 0; iter < 64; iter++) {
            if (resp_reserve(s, 16384) != 0) { slot_fail(s, c, 0, "response too large"); return; }
            int n = tcp_recv(c->sock, (uint8_t*)s->resp + s->resp_len, s->resp_cap - 1 - s->resp_len);
            if (n < 0) {
                if (s->resp_len == 0) {
                    if (slot_retry_stale(s, c)) return;
                    slot_fail(s, c, 0, "connection closed before a response");
                    return;
                }
                slot_finish(s, c, 0);   // close-delimited (or truncated: okai judges framing)
                return;
            }
            if (n == 0) break;
            s->resp_len += (uint32_t)n;
            c->t_progress = tick_count;
            int ka = 0;
            if (message_state(s->resp, s->resp_len, &ka) == 1) { slot_finish(s, c, ka); return; }
        }
    }
    if ((uint32_t)(tick_count - c->t_progress) > STALL_TICKS) slot_fail(s, c, 0, "timed out");
}

// An idle pooled connection: watch for the server closing it, absorb
// post-response records (NewSessionTicket), expire it.
static void conn_idle(struct fconn* c) {
    if (tcp_failed(c->sock) || tcp_state(c->sock) != TCP_STATE_ESTABLISHED ||
        (uint32_t)(tick_count - c->t0) > IDLE_TICKS) {
        conn_close(c, 0);
        return;
    }
    if (tcp_rx_avail(c->sock) <= 0) return;
    if (!c->https) { conn_close(c, 1); return; }   // unsolicited bytes: unusable
    static uint8_t sink[1];
    for (int iter = 0; iter < 8 && tcp_rx_avail(c->sock) > 0; iter++) {
        c->tls->out = sink;
        c->tls->out_cap = 0;
        c->tls->out_len = 0;
        int r = tls_state_step(c->tls, &c->io);
        if (r != TLS_STEP_AGAIN || c->tls->out_len) { conn_close(c, 1); return; }
    }
}

static void conn_step(struct fconn* c) {
    struct fslot* s = c->slot >= 0 ? &slots[c->slot] : 0;
    switch (c->st) {
    case CN_DNS: {
        uint32_t ip;
        int r = dns_lookup(c->host, &ip);
        if (r < 0) { slot_fail(s, c, 0, "DNS failed (no A record)"); return; }
        if (r > 0) {
            c->ip = ip;
            c->sock = tcp_open(ip, c->port);
            if (c->sock >= 0) { c->st = CN_TCP; return; }   // keep t0: one connect bound
        }
        if ((uint32_t)(tick_count - c->t0) > CONNECT_TICKS) slot_fail(s, c, 0, "DNS timed out");
        return;
    }
    case CN_TCP: {
        int st = tcp_state(c->sock);
        if (st == TCP_STATE_ESTABLISHED) { conn_start_request(c, s); return; }
        if (tcp_failed(c->sock) || st == TCP_STATE_CLOSED) { slot_fail(s, c, 0, "connect failed"); return; }
        if ((uint32_t)(tick_count - c->t0) > CONNECT_TICKS) slot_fail(s, c, 0, "connect timed out");
        return;
    }
    case CN_RUN:
        conn_run(c);
        return;
    case CN_IDLE:
        conn_idle(c);
        return;
    }
}

// ---- assignment ----------------------------------------------------------------------

static int conn_matches(const struct fconn* c, const struct fslot* s) {
    return c->https == s->https && c->port == s->port && host_eq(c->host, s->host);
}

// Give waiting slot s a connection. Returns 1 if it started.
static int slot_assign(int si) {
    struct fslot* s = &slots[si];
    // 1. an idle keep-alive connection to the same origin
    for (int i = 0; i < CONN_MAX; i++) {
        struct fconn* c = &conns[i];
        if (c->st != CN_IDLE || !conn_matches(c, s)) continue;
        int ok;
        if (c->https) {
            ok = resp_reserve(s, RESP_HEADROOM) == 0 &&
                 tls_state_reuse(c->tls, &c->io, s->req, s->req_len,
                                 (uint8_t*)s->resp, s->resp_cap - 1) == 0;
        } else {
            ok = tcp_send(c->sock, s->req, s->req_len) == 0;
        }
        if (!ok) { conn_close(c, 1); continue; }
        c->st = CN_RUN;
        c->slot = si;
        c->t0 = c->t_progress = tick_count;
        s->conn = i;
        s->st = SL_RUN;
        s->reused = 1;
        return 1;
    }
    // 2. a new connection, within the per-host and pool limits
    int per_host = 0, free_i = -1, idle_other = -1;
    for (int i = 0; i < CONN_MAX; i++) {
        struct fconn* c = &conns[i];
        if (c->st == CN_FREE) { if (free_i < 0) free_i = i; continue; }
        if (conn_matches(c, s)) per_host++;
        else if (c->st == CN_IDLE && (idle_other < 0 || (int32_t)(c->t0 - conns[idle_other].t0) < 0))
            idle_other = i;
    }
    if (per_host >= CONN_PER_HOST) return 0;
    if (free_i < 0 && idle_other >= 0) {   // evict the longest-idle other origin
        conn_close(&conns[idle_other], 0);
        free_i = idle_other;
    }
    if (free_i < 0) return 0;
    struct fconn* c = &conns[free_i];
    c->st = CN_DNS;
    c->https = s->https;
    c->port = s->port;
    scopy(c->host, s->host, sizeof c->host);
    c->sock = -1;
    c->slot = si;
    c->served = 0;
    c->t0 = c->t_progress = tick_count;
    c->io.send = io_send;
    c->io.recv = io_recv;
    c->io.user = c;
    s->conn = free_i;
    s->st = SL_RUN;
    s->reused = 0;
    return 1;
}

void fetch_poll(void) {
    // Assign waiting requests in the order they were made.
    for (;;) {
        int best = -1;
        for (int i = 0; i < FETCH_MAX; i++)
            if (slots[i].st == SL_WAIT && (best < 0 || (int32_t)(slots[i].seq - slots[best].seq) < 0))
                best = i;
        if (best < 0) break;
        if (!slot_assign(best)) {
            // Blocked (limits): let later requests to other hosts go ahead.
            int any = 0;
            for (int i = 0; i < FETCH_MAX; i++)
                if (i != best && slots[i].st == SL_WAIT && slot_assign(i)) any = 1;
            (void)any;
            break;
        }
    }
    for (int i = 0; i < CONN_MAX; i++)
        if (conns[i].st != CN_FREE) conn_step(&conns[i]);
}

// ---- public API ----------------------------------------------------------------------

static void put(uint8_t* b, uint32_t* n, uint32_t cap, const char* s) {
    while (*s && *n < cap) b[(*n)++] = (uint8_t)*s++;
}

int fetch_begin(const char* host, const char* path, uint16_t port, int https,
                const struct fetch_opts* o) {
    if (!host || !host[0]) return -1;
    int h = -1;
    for (int i = 0; i < FETCH_MAX; i++) if (slots[i].st == SL_FREE) { h = i; break; }
    if (h < 0) return -1;
    struct fslot* s = &slots[h];
    memset(s, 0, sizeof *s);
    s->https = https ? 1 : 0;
    s->port = port ? port : (uint16_t)(https ? 443 : 80);
    scopy(s->host, host, sizeof s->host);
    if (!path || !path[0]) path = "/";
    // Request: GET path HTTP/1.1, Host (with a non-default port), UA,
    // Accept, Accept-Encoding, extra headers. No "Connection: close" —
    // HTTP/1.1 keeps the connection for the next request.
    uint32_t cap = (uint32_t)strlen(path) + (uint32_t)strlen(host) + 512 +
                   (o && o->extra ? (uint32_t)strlen(o->extra) : 0);
    s->req = (uint8_t*)kmalloc(cap);
    if (!s->req) return -1;
    uint32_t n = 0;
    put(s->req, &n, cap, "GET ");
    put(s->req, &n, cap, path);
    put(s->req, &n, cap, " HTTP/1.1\r\nHost: ");
    put(s->req, &n, cap, host);
    if (s->port != (https ? 443 : 80)) {
        char pb[8];
        int k = 0, v = s->port;
        char t[6];
        int tn = 0;
        while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
        pb[k++] = ':';
        while (tn) pb[k++] = t[--tn];
        pb[k] = 0;
        put(s->req, &n, cap, pb);
    }
    put(s->req, &n, cap, "\r\nUser-Agent: okernel/0.4\r\n");
    put(s->req, &n, cap, o && o->accept_html ? NET_ACCEPT_HTML : "Accept: */*\r\n");
    if (o && o->gzip) put(s->req, &n, cap, "Accept-Encoding: gzip\r\n");
    if (o && o->extra) put(s->req, &n, cap, o->extra);
    put(s->req, &n, cap, "\r\n");
    s->req_len = n;
    {   // log name: host + path prefix
        int k = 0;
        for (const char* p = host; *p && k < 60; p++) s->what[k++] = *p;
        for (const char* p = path; *p && k < (int)sizeof s->what - 1; p++) s->what[k++] = *p;
        s->what[k] = 0;
    }
    s->conn = -1;
    s->seq = ++slot_seq;
    s->t_start = tick_count;
    s->st = SL_WAIT;
    if (https) serial_printf("[tls-net] queued HTTPS %s\n", s->what);
    slot_assign(h);
    return h;
}

static struct fslot* slot_get(int h) {
    if (h < 0 || h >= FETCH_MAX || slots[h].st == SL_FREE) return 0;
    return &slots[h];
}

int fetch_status(int h) {
    struct fslot* s = slot_get(h);
    if (!s) return FETCH_FAILED;
    return s->st == SL_DONE ? FETCH_DONE : s->st == SL_FAIL ? FETCH_FAILED : FETCH_RUNNING;
}

char* fetch_response(int h, int* len) {
    struct fslot* s = slot_get(h);
    if (!s || s->st != SL_DONE) { if (len) *len = 0; return 0; }
    if (len) *len = (int)s->resp_len;
    return s->resp;
}

int fetch_progress(int h) { struct fslot* s = slot_get(h); return s ? (int)s->resp_len : 0; }
int fetch_fail_reason(int h) { struct fslot* s = slot_get(h); return s ? s->fail_reason : 0; }
int fetch_fail_detail(int h) { struct fslot* s = slot_get(h); return s ? s->fail_detail : 0; }
int fetch_offer_unaccepted(int h) { struct fslot* s = slot_get(h); return s ? s->offer_unaccepted : 0; }
int fetch_saw_close(int h) { struct fslot* s = slot_get(h); return s ? s->saw_close : 0; }

void fetch_end(int h) {
    struct fslot* s = slot_get(h);
    if (!s) return;
    if (s->st == SL_RUN && s->conn >= 0) conn_close(&conns[s->conn], 1);
    if (s->req) kfree(s->req);
    if (s->resp) kfree(s->resp);
    memset(s, 0, sizeof *s);
    s->conn = -1;
}

int fetch_free_slots(void) {
    int n = 0;
    for (int i = 0; i < FETCH_MAX; i++) if (slots[i].st == SL_FREE) n++;
    return n;
}
