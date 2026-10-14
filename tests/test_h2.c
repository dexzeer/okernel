// Host test for the HTTP/2 session: a scripted server talks to it frame by frame (built here with the HPACK encoder), the
// bytes the client queues are parsed back and checked. ASan + UBSan.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define HPACK_HOST 1
#define H2_HOST 1
#include "../src/net/hpack.c"
#include "../src/net/hpack_tables.c"
#include "../src/net/h2.c"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

// ---- what the client reports ----
#define MAXS 8
static struct { char head[4096]; uint8_t data[200000]; uint32_t dlen; int ended, how; uint32_t code; int got_head; } R[MAXS];
static void on_head(void* u, const char* h, uint32_t l) { int i = (int)(intptr_t)u; memcpy(R[i].head, h, l); R[i].head[l] = 0; R[i].got_head = 1; }
static void on_data(void* u, const uint8_t* d, uint32_t l) { int i = (int)(intptr_t)u; if (R[i].dlen + l <= sizeof R[i].data) { memcpy(R[i].data + R[i].dlen, d, l); R[i].dlen += l; } }
static void on_end(void* u, int how, uint32_t code) { int i = (int)(intptr_t)u; R[i].ended = 1; R[i].how = how; R[i].code = code; }
static const struct h2_cb CB = { on_head, on_data, on_end };

// ---- frames the scripted server sends ----
static uint8_t sbuf[1 << 20];
static uint32_t slen;
static void sframe(uint8_t type, uint8_t flags, uint32_t sid, const uint8_t* p, uint32_t len) {
    sbuf[slen++] = (uint8_t)(len >> 16); sbuf[slen++] = (uint8_t)(len >> 8); sbuf[slen++] = (uint8_t)len;
    sbuf[slen++] = type; sbuf[slen++] = flags;
    sbuf[slen++] = (uint8_t)(sid >> 24); sbuf[slen++] = (uint8_t)(sid >> 16); sbuf[slen++] = (uint8_t)(sid >> 8); sbuf[slen++] = (uint8_t)sid;
    if (len) memcpy(sbuf + slen, p, len);
    slen += len;
}
static uint32_t henc(uint8_t* o, const char* n, const char* v) { return hpack_encode_field(o, 4096, n, (uint32_t)strlen(n), v, (uint32_t)strlen(v), 0); }
static void sresp(uint32_t sid, int end, const char* status, const char* ctype) {
    uint8_t b[512]; uint32_t o = 0;
    o += henc(b + o, ":status", status);
    if (ctype) o += henc(b + o, "content-type", ctype);
    sframe(1, (uint8_t)(4 | (end ? 1 : 0)), sid, b, o);
}
static int feed(struct h2* h) { int r = h2_input(h, sbuf, slen); slen = 0; return r; }
static int feed_bytewise(struct h2* h) { int r = 0; for (uint32_t i = 0; i < slen && !r; i++) r = h2_input(h, sbuf + i, 1); slen = 0; return r; }

// ---- parsing what the client sent ----
struct fr { uint8_t type, flags; uint32_t sid, len; const uint8_t* p; };
static int next_frame(const uint8_t** cur, const uint8_t* end, struct fr* f) {
    if (end - *cur < 9) return 0;
    const uint8_t* b = *cur;
    f->len = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    f->type = b[3]; f->flags = b[4];
    f->sid = (((uint32_t)b[5] << 24) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 8) | b[8]) & 0x7FFFFFFFu;
    if ((uint32_t)(end - b) < 9 + f->len) return 0;
    f->p = b + 9;
    *cur = b + 9 + f->len;
    return 1;
}
static char hdrs[4096];
static int collect(void* c, const char* n, int nl, const char* v, int vl) {
    (void)c; size_t o = strlen(hdrs);
    memcpy(hdrs + o, n, (size_t)nl); o += (size_t)nl; hdrs[o++] = '='; memcpy(hdrs + o, v, (size_t)vl); o += (size_t)vl; hdrs[o++] = ';'; hdrs[o] = 0; return 0;
}
// everything the client queued, drained
static uint8_t cbuf[1 << 20];
static uint32_t clen;
static void drain(struct h2* h) { uint32_t n; const uint8_t* p = h2_out(h, &n); memcpy(cbuf + clen, p, n); clen += n; h2_out_done(h, n); }
static int count_type(uint8_t type) {
    int n = 0; const uint8_t* c = cbuf; struct fr f;
    while (next_frame(&c, cbuf + clen, &f)) n += f.type == type;
    return n;
}

int main(void) {
    struct h2* h = h2_new(&CB);
    CHECK(h != 0, "new session");
    drain(h);
    CHECK(clen > 24 && !memcmp(cbuf, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24), "starts with the connection preface");
    {
        const uint8_t* c = cbuf + 24; struct fr f;
        int ok = next_frame(&c, cbuf + clen, &f) && f.type == 4 && f.sid == 0 && f.len == 18;
        int push_off = ok && f.p[1] == 2 && f.p[5] == 0;
        CHECK(ok && push_off, "then a SETTINGS frame that turns server push off");
        ok = next_frame(&c, cbuf + clen, &f) && f.type == 8 && f.sid == 0 && f.len == 4;
        CHECK(ok, "and a connection WINDOW_UPDATE");
    }
    clen = 0;

    // server SETTINGS -> we ACK
    sframe(4, 0, 0, 0, 0);
    CHECK(feed(h) == 0, "server SETTINGS accepted");
    drain(h);
    {
        const uint8_t* c = cbuf; struct fr f;
        CHECK(next_frame(&c, cbuf + clen, &f) && f.type == 4 && (f.flags & 1) && f.len == 0, "SETTINGS ACK queued");
    }
    clen = 0;

    // a GET
    static const char GET[] = "GET /a/b?c=d HTTP/1.1\r\nHost: x.test\r\nUser-Agent: okernel/0.4\r\nAccept: */*\r\nAccept-Encoding: gzip\r\n"
                              "Connection: keep-alive\r\nCookie: a=b\r\nX-Extra: yes\r\n\r\n";
    int s1 = h2_open(h, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)1);
    CHECK(s1 == 1, "first stream is id 1");
    drain(h);
    {
        const uint8_t* c = cbuf; struct fr f;
        int ok = next_frame(&c, cbuf + clen, &f) && f.type == 1 && f.sid == 1 && (f.flags & 4) && (f.flags & 1);
        CHECK(ok, "HEADERS with END_HEADERS | END_STREAM");
        struct hpack_dec d; hpack_dec_init(&d, 4096); hdrs[0] = 0;
        hpack_decode(&d, f.p, f.len, collect, 0);
        CHECK(!strncmp(hdrs, ":method=GET;:scheme=https;:authority=x.test;:path=/a/b?c=d;", 58), "pseudo-headers first, in the usual order");
        CHECK(strstr(hdrs, "user-agent=okernel/0.4;") && strstr(hdrs, "accept=*/*;") && strstr(hdrs, "accept-encoding=gzip;") &&
              strstr(hdrs, "cookie=a=b;") && strstr(hdrs, "x-extra=yes;"), "ordinary headers, lower-cased");
        CHECK(!strstr(hdrs, "host=") && !strstr(hdrs, "connection="), "Host and Connection are not sent as headers");
        hpack_dec_free(&d);
    }
    clen = 0;

    // 100-continue style interim response, then the real one and the body
    sresp(1, 0, "100", 0);
    sresp(1, 0, "200", "text/plain");
    sframe(0, 0, 1, (const uint8_t*)"hello ", 6);
    sframe(0, 1, 1, (const uint8_t*)"world", 5);
    CHECK(feed_bytewise(h) == 0, "response fed one byte at a time is reassembled");
    CHECK(R[1].got_head && !strncmp(R[1].head, "HTTP/1.1 200 OK\r\n", 17) && strstr(R[1].head, "content-type: text/plain\r\n") &&
          !strncmp(R[1].head + strlen(R[1].head) - 4, "\r\n\r\n", 4), "head is an HTTP/1.1 head (interim 1xx swallowed)");
    CHECK(R[1].dlen == 11 && !memcmp(R[1].data, "hello world", 11) && R[1].ended && R[1].how == H2_END_OK, "body and end");
    CHECK(h2_active(h) == 0, "stream closed");

    // CONTINUATION and a large body with flow-control updates
    {
        int s3 = h2_open(h, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)2);
        CHECK(s3 == 3, "next stream id is 3");
        drain(h); clen = 0;
        uint8_t b[600]; uint32_t o = 0;
        o += henc(b + o, ":status", "200");
        uint32_t half = o;
        o += henc(b + o, "x-a", "1");
        o += henc(b + o, "set-cookie", "k=v");
        sframe(1, 0, 3, b, half);                         // HEADERS without END_HEADERS
        sframe(9, 4, 3, b + half, o - half);              // CONTINUATION with END_HEADERS
        static uint8_t body[16384];
        memset(body, 'z', sizeof body);
        for (int i = 0; i < 6; i++) sframe(0, i == 5 ? 1 : 0, 3, body, sizeof body);       // 96 KB
        CHECK(feed(h) == 0, "HEADERS + CONTINUATION + 96 KB of DATA");
        CHECK(R[2].dlen == 6 * 16384 && R[2].how == H2_END_OK && strstr(R[2].head, "x-a: 1\r\n") && strstr(R[2].head, "set-cookie: k=v\r\n"), "all of it arrived");
        drain(h);
        CHECK(count_type(8) >= 2, "WINDOW_UPDATEs were sent for the connection and the stream");
        clen = 0;
    }

    // POST with a body bigger than the 65535 byte initial window
    {
        static uint8_t post[100000 + 200];
        int pl = snprintf((char*)post, 200, "POST /up HTTP/1.1\r\nHost: x.test\r\nContent-Length: 100000\r\nContent-Type: application/octet-stream\r\n\r\n");
        memset(post + pl, 'p', 100000);
        int s5 = h2_open(h, post, (uint32_t)pl + 100000, (void*)(intptr_t)3);
        CHECK(s5 == 5, "POST stream id 5");
        drain(h);
        uint32_t data = 0; int end_seen = 0, headers_end_stream = 0;
        const uint8_t* c = cbuf; struct fr f;
        while (next_frame(&c, cbuf + clen, &f)) {
            if (f.type == 1) headers_end_stream = f.flags & 1;
            if (f.type == 0) { data += f.len; if (f.flags & 1) end_seen = 1; if (f.len > 16384) { data = 0xFFFFFFF; } }
        }
        CHECK(!headers_end_stream && data == 65535 && !end_seen, "only 65535 bytes of body go out before the window is used up");
        clen = 0;
        uint8_t wu[4] = { 0, 1, 0, 0 };                    // +65536
        sframe(8, 0, 0, wu, 4);
        sframe(8, 0, 5, wu, 4);
        CHECK(feed(h) == 0, "WINDOW_UPDATEs accepted");
        drain(h);
        data = 0; end_seen = 0; c = cbuf;
        while (next_frame(&c, cbuf + clen, &f)) if (f.type == 0) { data += f.len; if (f.flags & 1) end_seen = 1; }
        CHECK(data == 100000 - 65535 && end_seen, "the rest of the body follows, the last DATA frame carries END_STREAM");
        clen = 0;
        sresp(5, 1, "204", 0);
        feed(h);
        CHECK(R[3].ended && R[3].how == H2_END_OK && !strncmp(R[3].head, "HTTP/1.1 204 No Content", 23), "204 answer ends the stream");
    }

    // PING, GOAWAY, RST_STREAM(REFUSED_STREAM)
    {
        uint8_t ping[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        sframe(6, 0, 0, ping, 8);
        feed(h); drain(h);
        const uint8_t* c = cbuf; struct fr f;
        CHECK(next_frame(&c, cbuf + clen, &f) && f.type == 6 && (f.flags & 1) && !memcmp(f.p, ping, 8), "PING is answered with the same payload and ACK");
        clen = 0;
        int a = h2_open(h, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)4);      // id 7
        int b = h2_open(h, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)5);      // id 9
        CHECK(a == 7 && b == 9, "ids 7 and 9");
        drain(h); clen = 0;
        uint8_t rst[4] = { 0, 0, 0, 7 };
        sframe(3, 0, 7, rst, 4);
        feed(h);
        CHECK(R[4].ended && R[4].how == H2_END_RETRY, "REFUSED_STREAM before any answer means: try again");
        uint8_t ga[8] = { 0, 0, 0, 5, 0, 0, 0, 0 };         // last stream id 5: 9 was not processed
        sframe(7, 0, 0, ga, 8);
        feed(h);
        CHECK(R[5].ended && R[5].how == H2_END_RETRY && !h2_can_open(h), "GOAWAY below a stream: retry it elsewhere, no new streams");
    }
    h2_free(h);

    // protocol errors
    {
        struct h2* e = h2_new(&CB);
        drain(e); clen = 0;
        sframe(0, 0, 1, (const uint8_t*)"x", 1);
        CHECK(feed(e) < 0 && h2_dead(e), "a first frame that is not SETTINGS kills the connection");
        drain(e);
        const uint8_t* c = cbuf; struct fr f;
        CHECK(next_frame(&c, cbuf + clen, &f) && f.type == 7, "...with a GOAWAY");
        clen = 0;
        h2_free(e);
        e = h2_new(&CB); drain(e); clen = 0;
        sframe(4, 0, 0, 0, 0);
        uint8_t pp[4] = { 0, 0, 0, 2 };
        sframe(5, 4, 1, pp, 4);
        CHECK(feed(e) < 0 && h2_dead(e), "PUSH_PROMISE is refused");
        h2_free(e);
        e = h2_new(&CB); drain(e); clen = 0;
        sframe(4, 0, 0, 0, 0);
        uint8_t big[1] = { 0 };
        sbuf[slen++] = 0x00; sbuf[slen++] = 0x40; sbuf[slen++] = 0x01; sbuf[slen++] = 0; sbuf[slen++] = 0; sbuf[slen++] = 0; sbuf[slen++] = 0; sbuf[slen++] = 0; sbuf[slen++] = 1; (void)big;
        CHECK(feed(e) < 0, "a frame larger than SETTINGS_MAX_FRAME_SIZE is refused");
        h2_free(e);
        e = h2_new(&CB); drain(e); clen = 0;
        sframe(4, 0, 0, 0, 0);
        int sx = h2_open(e, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)6);
        (void)sx;
        uint8_t garbage[] = { 0x88, 0xff, 0xff, 0xff, 0xff, 0xff };         // not a valid header block
        sframe(1, 5, 1, garbage, sizeof garbage);
        feed(e);
        CHECK(h2_dead(e) && R[6].ended && R[6].how == H2_END_BROKEN, "a broken header block is a connection error and fails the open streams");
        h2_free(e);
    }

    // cancel, and the stream limit
    {
        struct h2* e = h2_new(&CB); drain(e); clen = 0;
        sframe(4, 0, 0, 0, 0); feed(e); drain(e); clen = 0;
        int ids[40], n = 0;
        for (int i = 0; i < 40; i++) { ids[i] = h2_open(e, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)7); if (ids[i] > 0) n++; }
        CHECK(n == 32 && !h2_can_open(e), "at most 32 streams at a time");
        h2_cancel(e, ids[0]);
        CHECK(h2_can_open(e) && h2_active(e) == 31, "cancelling frees a place");
        drain(e);
        int rst = 0; const uint8_t* c = cbuf; struct fr f;
        while (next_frame(&c, cbuf + clen, &f)) rst += f.type == 3 && f.sid == (uint32_t)ids[0];
        CHECK(rst == 1, "...and sent RST_STREAM");
        clen = 0;
        // late data for the cancelled stream is dropped quietly but still counts for the connection window
        sresp((uint32_t)ids[0], 0, "200", 0);
        sframe(0, 1, (uint32_t)ids[0], (const uint8_t*)"late", 4);
        CHECK(feed(e) == 0 && !h2_dead(e), "data for a cancelled stream is ignored");
        h2_free(e);
    }

    // hostile input
    {
        uint32_t rs = 99;
        int survived = 0;
        for (int t = 0; t < 3000; t++) {
            struct h2* e = h2_new(&CB); drain(e); clen = 0;
            if (t % 2) { sframe(4, 0, 0, 0, 0); feed(e); h2_open(e, (const uint8_t*)GET, sizeof GET - 1, (void*)(intptr_t)7); }
            slen = 0;
            uint32_t n = 20 + (rs = rs * 1664525u + 1013904223u) % 400;
            for (uint32_t i = 0; i < n; i++) sbuf[slen++] = (uint8_t)((rs = rs * 1664525u + 1013904223u) >> 16);
            if (t % 3 == 0) {                              // plausible frame headers with random payloads
                slen = 0;
                for (int k = 0; k < 6; k++) {
                    uint8_t pl[40]; uint32_t l = (rs = rs * 1664525u + 1013904223u) % 30;
                    for (uint32_t i = 0; i < l; i++) pl[i] = (uint8_t)((rs = rs * 1664525u + 1013904223u) >> 16);
                    sframe((uint8_t)((rs = rs * 1664525u + 1013904223u) >> 8) % 11, (uint8_t)((rs = rs * 1664525u + 1013904223u) >> 8), 1 + (rs >> 4) % 5, pl, l);
                }
            }
            feed(e);
            drain(e); clen = 0;
            h2_free(e);
            survived++;
        }
        CHECK(survived == 3000, "3000 sessions fed random bytes and random frames survive (ASan/UBSan clean)");
    }
    printf(fails ? "H2 TESTS FAIL\n" : "H2 TESTS PASS\n");
    return fails != 0;
}
