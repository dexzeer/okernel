// HTTP/2 client session, see h2.h.
#include "h2.h"
#include "hpack.h"

#ifdef H2_HOST
#include <stdlib.h>
#include <string.h>
#define h2_alloc(n) malloc(n)
#define h2_realloc(p, n) realloc((p), (n))
#define h2_free_mem(p) free(p)
#else
#include "../memory.h"
#include "../string.h"
#define h2_alloc(n) kmalloc(n)
#define h2_realloc(p, n) krealloc((p), (n))
#define h2_free_mem(p) kfree(p)
#endif

#define F_DATA 0
#define F_HEADERS 1
#define F_PRIORITY 2
#define F_RST_STREAM 3
#define F_SETTINGS 4
#define F_PUSH_PROMISE 5
#define F_PING 6
#define F_GOAWAY 7
#define F_WINDOW_UPDATE 8
#define F_CONTINUATION 9

#define FL_END_STREAM 0x1
#define FL_ACK 0x1
#define FL_END_HEADERS 0x4
#define FL_PADDED 0x8
#define FL_PRIORITY 0x20

#define E_NO_ERROR 0
#define E_PROTOCOL 1
#define E_INTERNAL 2
#define E_FLOW_CONTROL 3
#define E_STREAM_CLOSED 5
#define E_FRAME_SIZE 6
#define E_REFUSED_STREAM 7
#define E_CANCEL 8
#define E_COMPRESSION 9

#define H2_MAX_STREAMS 64
#define H2_OUR_MAX_CONC 32                  // streams we keep open at once at most
#define H2_RECV_STREAM_WIN (1u << 20)       // window we give each stream
#define H2_RECV_CONN_WIN (16u << 20)        // ...and the connection
#define H2_MAX_FRAME_IN 16384               // the default SETTINGS_MAX_FRAME_SIZE, which we never raise
#define H2_MAX_HDR_BLOCK 65536
#define H2_REQ_BLOCK 32768                  // largest HPACK-encoded request header block
#define H2_HEAD_RESERVE 64                  // room in front of the header lines for the status line

struct h2_stream {
    int32_t id;                  // 0 = unused
    void* user;
    uint8_t head_done, remote_closed, local_closed;
    int32_t send_win;
    uint32_t recv_consumed;
    uint8_t* body;
    uint32_t body_len, body_off;
};

struct h2 {
    struct h2_cb cb;
    struct hpack_dec dec;
    uint8_t* out; uint32_t out_len, out_cap, out_off;
    uint8_t* in; uint32_t in_len, in_cap;
    struct h2_stream st[H2_MAX_STREAMS];
    int32_t next_id;
    int32_t conn_send_win;
    uint32_t conn_recv_consumed;
    uint32_t peer_max_frame, peer_max_conc;
    int32_t peer_init_win;
    int settings_seen, got_first_frame, goaway, dead;
    uint32_t goaway_last;
    int nactive;
    // header block being collected across HEADERS / CONTINUATION
    uint8_t* hb; uint32_t hb_len, hb_cap;
    int32_t hb_stream;
    uint8_t hb_end_stream;
    // response head being built
    char* head; uint32_t head_len, head_cap;
    int head_status, head_overflow;
};

// ---- buffers ------------------------------------------------------------------------------------------------------

static int grow(uint8_t** buf, uint32_t* cap, uint32_t need) {
    if (need <= *cap && *buf) return 0;
    uint32_t nc = *cap ? *cap : 1024;
    while (nc < need) nc *= 2;
    uint8_t* nb = (uint8_t*)h2_realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Append a frame to the output queue.
static int put_frame(struct h2* h, uint8_t type, uint8_t flags, uint32_t sid, const uint8_t* payload, uint32_t len) {
    if (grow(&h->out, &h->out_cap, h->out_len + 9 + len)) { h->dead = 1; return -1; }
    uint8_t* p = h->out + h->out_len;
    p[0] = (uint8_t)(len >> 16); p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)len;
    p[3] = type; p[4] = flags;
    put32(p + 5, sid & 0x7FFFFFFFu);
    if (len) memcpy(p + 9, payload, len);
    h->out_len += 9 + len;
    return 0;
}

static void put_window_update(struct h2* h, uint32_t sid, uint32_t inc) {
    uint8_t b[4];
    put32(b, inc & 0x7FFFFFFFu);
    put_frame(h, F_WINDOW_UPDATE, 0, sid, b, 4);
}

// ---- streams ------------------------------------------------------------------------------------------------------

static struct h2_stream* find(struct h2* h, uint32_t id) {
    for (int i = 0; i < H2_MAX_STREAMS; i++) if (h->st[i].id == (int32_t)id && id) return &h->st[i];
    return 0;
}

static void stream_free(struct h2* h, struct h2_stream* s) {
    h2_free_mem(s->body);
    memset(s, 0, sizeof *s);
    h->nactive--;
}

static void stream_end(struct h2* h, struct h2_stream* s, int how, uint32_t code) {
    void* user = s->user;
    stream_free(h, s);
    if (h->cb.on_end) h->cb.on_end(user, how, code);
}

static void kill_all(struct h2* h, int how, uint32_t code) {
    for (int i = 0; i < H2_MAX_STREAMS; i++)
        if (h->st[i].id) stream_end(h, &h->st[i], how, code);
}

// A connection error: tell the peer, fail every stream, stop.
static int conn_error(struct h2* h, uint32_t code) {
    if (!h->dead) {
        uint8_t b[8];
        put32(b, 0);
        put32(b + 4, code);
        put_frame(h, F_GOAWAY, 0, 0, b, 8);
        h->dead = 1;
        kill_all(h, H2_END_BROKEN, code);
    }
    return -(int)code - 1;
}

static void stream_rst(struct h2* h, uint32_t sid, uint32_t code) {
    uint8_t b[4];
    put32(b, code);
    put_frame(h, F_RST_STREAM, 0, sid, b, 4);
}

// ---- sending request bodies -----------------------------------------------------------------------------------------

static void pump(struct h2* h) {
    for (int i = 0; i < H2_MAX_STREAMS && !h->dead; i++) {
        struct h2_stream* s = &h->st[i];
        if (!s->id || !s->body || s->body_off >= s->body_len) continue;
        while (s->body_off < s->body_len && h->conn_send_win > 0 && s->send_win > 0) {
            uint32_t n = s->body_len - s->body_off;
            if (n > h->peer_max_frame) n = h->peer_max_frame;
            if (n > (uint32_t)h->conn_send_win) n = (uint32_t)h->conn_send_win;
            if (n > (uint32_t)s->send_win) n = (uint32_t)s->send_win;
            int last = s->body_off + n == s->body_len;
            if (put_frame(h, F_DATA, last ? FL_END_STREAM : 0, (uint32_t)s->id, s->body + s->body_off, n)) return;
            s->body_off += n;
            h->conn_send_win -= (int32_t)n;
            s->send_win -= (int32_t)n;
            if (last) { s->local_closed = 1; h2_free_mem(s->body); s->body = 0; break; }
        }
    }
}

// ---- opening a stream -----------------------------------------------------------------------------------------------

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_is(const char* n, int nl, const char* lit) {
    int ll = (int)strlen(lit);
    if (nl != ll) return 0;
    for (int i = 0; i < nl; i++) if (lc((unsigned char)n[i]) != lit[i]) return 0;
    return 1;
}

int h2_can_open(const struct h2* h) {
    uint32_t cap = h->peer_max_conc < H2_OUR_MAX_CONC ? h->peer_max_conc : H2_OUR_MAX_CONC;
    return !h->dead && !h->goaway && h->nactive < (int)cap && h->nactive < H2_MAX_STREAMS && h->next_id < 0x7FFF0000;
}

int h2_open(struct h2* h, const uint8_t* req, uint32_t len, void* user) {
    if (!h2_can_open(h)) return -1;
    // request line
    uint32_t i = 0;
    const char* method = (const char*)req;
    while (i < len && req[i] != ' ') i++;
    uint32_t ml = i;
    if (ml == 0 || i >= len) return -1;
    i++;
    const char* path = (const char*)req + i;
    while (i < len && req[i] != ' ') i++;
    uint32_t pl = (uint32_t)((const char*)req + i - path);
    if (pl == 0 || i >= len) return -1;
    while (i < len && req[i] != '\n') i++;
    if (i >= len) return -1;
    i++;
    // headers
    uint8_t* blk = (uint8_t*)h2_alloc(H2_REQ_BLOCK);
    if (!blk) return -1;
    uint32_t o = 0, c;
    const char* authority = 0;
    uint32_t al = 0;
    uint32_t hs = i;
    // find Host first (it becomes :authority, which must precede the others)
    for (uint32_t p = hs; p < len;) {
        uint32_t e = p;
        while (e < len && req[e] != '\n') e++;
        uint32_t le = e;
        if (le > p && req[le - 1] == '\r') le--;
        if (le == p) break;
        if (le - p > 5 && name_is((const char*)req + p, 4, "host") && req[p + 4] == ':') {
            uint32_t v = p + 5;
            while (v < le && (req[v] == ' ' || req[v] == '\t')) v++;
            authority = (const char*)req + v;
            al = le - v;
        }
        p = e + 1;
    }
    if (!authority || !al) { h2_free_mem(blk); return -1; }
    #define ENC(n, nlen, v, vlen, sens) do { c = hpack_encode_field(blk + o, H2_REQ_BLOCK - o, (n), (nlen), (v), (vlen), (sens)); if (!c) { h2_free_mem(blk); return -1; } o += c; } while (0)
    ENC(":method", 7, method, ml, 0);
    ENC(":scheme", 7, "https", 5, 0);
    ENC(":authority", 10, authority, al, 0);
    ENC(":path", 5, path, pl, 0);
    uint32_t body_at = len;
    for (uint32_t p = hs; p < len;) {
        uint32_t e = p;
        while (e < len && req[e] != '\n') e++;
        uint32_t le = e;
        if (le > p && req[le - 1] == '\r') le--;
        if (le == p) { body_at = e + 1 < len ? e + 1 : len; break; }
        uint32_t colon = p;
        while (colon < le && req[colon] != ':') colon++;
        if (colon < le && colon > p) {
            const char* n = (const char*)req + p;
            int nl = (int)(colon - p);
            uint32_t v = colon + 1;
            while (v < le && (req[v] == ' ' || req[v] == '\t')) v++;
            // connection-specific headers do not exist in HTTP/2 (RFC 9113 8.2.2)
            int skip = name_is(n, nl, "host") || name_is(n, nl, "connection") || name_is(n, nl, "keep-alive") ||
                       name_is(n, nl, "proxy-connection") || name_is(n, nl, "transfer-encoding") || name_is(n, nl, "upgrade") ||
                       name_is(n, nl, "te") || name_is(n, nl, "expect");
            if (!skip) {
                char ln[128];
                if (nl > (int)sizeof ln) { h2_free_mem(blk); return -1; }
                for (int k = 0; k < nl; k++) ln[k] = (char)lc((unsigned char)n[k]);
                int sens = name_is(n, nl, "cookie") || name_is(n, nl, "authorization");
                ENC(ln, (uint32_t)nl, (const char*)req + v, le - v, sens);
            }
        }
        p = e + 1;
    }
    #undef ENC
    // a free stream slot
    struct h2_stream* s = 0;
    for (int k = 0; k < H2_MAX_STREAMS; k++) if (!h->st[k].id) { s = &h->st[k]; break; }
    if (!s) { h2_free_mem(blk); return -1; }
    memset(s, 0, sizeof *s);
    uint32_t blen = len > body_at ? len - body_at : 0;
    if (blen) {
        s->body = (uint8_t*)h2_alloc(blen);
        if (!s->body) { h2_free_mem(blk); return -1; }
        memcpy(s->body, req + body_at, blen);
        s->body_len = blen;
    }
    s->id = h->next_id;
    h->next_id += 2;
    s->user = user;
    s->send_win = h->peer_init_win;
    h->nactive++;
    // HEADERS, split into CONTINUATIONs when the block is larger than a frame
    uint32_t sent = 0, first = 1;
    while (sent < o || first) {
        uint32_t n = o - sent;
        if (n > h->peer_max_frame) n = h->peer_max_frame;
        int end_headers = sent + n == o;
        uint8_t flags = end_headers ? FL_END_HEADERS : 0;
        if (first && !blen) flags |= FL_END_STREAM;
        put_frame(h, first ? F_HEADERS : F_CONTINUATION, flags, (uint32_t)s->id, blk + sent, n);
        sent += n;
        first = 0;
    }
    if (!blen) s->local_closed = 1;
    h2_free_mem(blk);
    pump(h);
    return s->id;
}

void h2_cancel(struct h2* h, int stream_id) {
    struct h2_stream* s = find(h, (uint32_t)stream_id);
    if (!s) return;
    if (!s->remote_closed) stream_rst(h, (uint32_t)stream_id, E_CANCEL);
    stream_free(h, s);
}

// ---- receiving ------------------------------------------------------------------------------------------------------

static const char* reason(int st) {
    switch (st) {
    case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted"; case 204: return "No Content";
    case 206: return "Partial Content"; case 301: return "Moved Permanently"; case 302: return "Found";
    case 303: return "See Other"; case 304: return "Not Modified"; case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 410: return "Gone"; case 429: return "Too Many Requests"; case 500: return "Internal Server Error";
    case 502: return "Bad Gateway"; case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
    default: return "";
    }
}

static int head_append(struct h2* h, const char* s, uint32_t n) {
    if (h->head_len + n > h->head_cap) {
        if (h->head_len + n > H2_MAX_HDR_BLOCK * 2) { h->head_overflow = 1; return -1; }
        uint32_t nc = h->head_cap ? h->head_cap : 4096;
        while (nc < h->head_len + n) nc *= 2;
        char* nb = (char*)h2_realloc(h->head, nc);
        if (!nb) { h->head_overflow = 1; return -1; }
        h->head = nb;
        h->head_cap = nc;
    }
    memcpy(h->head + h->head_len, s, n);
    h->head_len += n;
    return 0;
}

static int emit_header(void* ctx, const char* n, int nl, const char* v, int vl) {
    struct h2* h = (struct h2*)ctx;
    if (nl > 0 && n[0] == ':') {
        if (nl == 7 && !memcmp(n, ":status", 7)) {
            int st = 0;
            if (vl != 3) return 1;
            for (int i = 0; i < 3; i++) { if (v[i] < '0' || v[i] > '9') return 1; st = st * 10 + (v[i] - '0'); }
            h->head_status = st;
        }
        return 0;                                  // other pseudo-headers do not belong in a response
    }
    if (head_append(h, n, (uint32_t)nl) || head_append(h, ": ", 2) || head_append(h, v, (uint32_t)vl) || head_append(h, "\r\n", 2)) return 1;
    return 0;
}

// A complete header block (HEADERS + CONTINUATIONs) for stream sid.
static int header_block(struct h2* h, uint32_t sid, const uint8_t* blk, uint32_t len, int end_stream) {
    h->head_len = H2_HEAD_RESERVE;
    h->head_status = 0;
    h->head_overflow = 0;
    if (h->head_cap < H2_HEAD_RESERVE) {
        uint8_t* nb = (uint8_t*)h->head;
        if (grow(&nb, &h->head_cap, 4096)) return conn_error(h, E_INTERNAL);
        h->head = (char*)nb;
    }
    int r = hpack_decode(&h->dec, blk, len, emit_header, h);        // always decode: the dynamic table must stay in step
    if (r == -1) return conn_error(h, E_COMPRESSION);
    if (r == -2 || h->head_overflow) {
        struct h2_stream* s = find(h, sid);
        if (s) { stream_rst(h, sid, E_PROTOCOL); stream_end(h, s, H2_END_RESET, E_PROTOCOL); }
        return 0;
    }
    struct h2_stream* s = find(h, sid);
    if (!s) {
        if (sid >= (uint32_t)h->next_id || (sid & 1) == 0) return conn_error(h, E_PROTOCOL);
        return 0;                                                    // a stream we already closed or cancelled
    }
    if (s->head_done) {                                              // trailers: nothing to hand on
        if (!end_stream) { stream_rst(h, sid, E_PROTOCOL); stream_end(h, s, H2_END_RESET, E_PROTOCOL); return 0; }
    } else {
        if (h->head_status == 0) { stream_rst(h, sid, E_PROTOCOL); stream_end(h, s, H2_END_RESET, E_PROTOCOL); return 0; }
        if (h->head_status >= 100 && h->head_status < 200 && h->head_status != 101) return 0;   // informational: wait for the real one
        char line[H2_HEAD_RESERVE];
        int ll = 0;
        const char* pre = "HTTP/1.1 ";
        while (*pre) line[ll++] = *pre++;
        line[ll++] = (char)('0' + h->head_status / 100);
        line[ll++] = (char)('0' + h->head_status / 10 % 10);
        line[ll++] = (char)('0' + h->head_status % 10);
        line[ll++] = ' ';
        for (const char* q = reason(h->head_status); *q; q++) line[ll++] = *q;
        line[ll++] = '\r'; line[ll++] = '\n';
        if (head_append(h, "\r\n", 2)) return conn_error(h, E_INTERNAL);
        uint32_t start = H2_HEAD_RESERVE - (uint32_t)ll;
        memcpy(h->head + start, line, (size_t)ll);
        s->head_done = 1;
        if (h->cb.on_head) h->cb.on_head(s->user, h->head + start, h->head_len - start);
        s = find(h, sid);                                            // the callback may have cancelled it
        if (!s) return 0;
    }
    if (end_stream) {
        s->remote_closed = 1;
        stream_end(h, s, H2_END_OK, 0);
    }
    return 0;
}

static int handle_settings(struct h2* h, const uint8_t* p, uint32_t len) {
    for (uint32_t i = 0; i + 6 <= len; i += 6) {
        uint32_t id = ((uint32_t)p[i] << 8) | p[i + 1], v = get32(p + i + 2);
        switch (id) {
        case 2: if (v > 1) return conn_error(h, E_PROTOCOL); break;                       // ENABLE_PUSH
        case 3: h->peer_max_conc = v; break;                                              // MAX_CONCURRENT_STREAMS
        case 4: {                                                                         // INITIAL_WINDOW_SIZE
            if (v > 0x7FFFFFFFu) return conn_error(h, E_FLOW_CONTROL);
            int32_t delta = (int32_t)v - h->peer_init_win;
            h->peer_init_win = (int32_t)v;
            for (int k = 0; k < H2_MAX_STREAMS; k++)
                if (h->st[k].id) {
                    if ((delta > 0 && h->st[k].send_win > 0x7FFFFFFF - delta)) return conn_error(h, E_FLOW_CONTROL);
                    h->st[k].send_win += delta;
                }
            break;
        }
        case 5: if (v < 16384 || v > 16777215) return conn_error(h, E_PROTOCOL); h->peer_max_frame = v; break;   // MAX_FRAME_SIZE
        default: break;                                                                   // table size (we never index), header list size, unknown
        }
    }
    put_frame(h, F_SETTINGS, FL_ACK, 0, 0, 0);
    h->settings_seen = 1;
    pump(h);
    return 0;
}

static int frame(struct h2* h, uint8_t type, uint8_t flags, uint32_t sid, const uint8_t* p, uint32_t len) {
    if (!h->got_first_frame) {
        if (type != F_SETTINGS || (flags & FL_ACK)) return conn_error(h, E_PROTOCOL);   // the server preface is a SETTINGS frame
        h->got_first_frame = 1;
    }
    if (h->hb_stream && (type != F_CONTINUATION || sid != (uint32_t)h->hb_stream)) return conn_error(h, E_PROTOCOL);
    switch (type) {
    case F_DATA: {
        if (sid == 0) return conn_error(h, E_PROTOCOL);
        uint32_t total = len, pad = 0;
        if (flags & FL_PADDED) {
            if (len < 1) return conn_error(h, E_FRAME_SIZE);
            pad = p[0];
            if (pad + 1u > len) return conn_error(h, E_PROTOCOL);
            p++; len -= 1 + pad;
        }
        h->conn_recv_consumed += total;
        if (h->conn_recv_consumed >= 32768) { put_window_update(h, 0, h->conn_recv_consumed); h->conn_recv_consumed = 0; }
        struct h2_stream* s = find(h, sid);
        if (!s) {
            if ((sid & 1) == 0 || sid >= (uint32_t)h->next_id) return conn_error(h, E_PROTOCOL);
            return 0;                                              // closed or cancelled: the bytes were counted above
        }
        if (!s->head_done) { stream_rst(h, sid, E_PROTOCOL); stream_end(h, s, H2_END_RESET, E_PROTOCOL); return 0; }
        s->recv_consumed += total;
        if (s->recv_consumed >= H2_RECV_STREAM_WIN / 4 && !(flags & FL_END_STREAM)) {
            put_window_update(h, sid, s->recv_consumed);
            s->recv_consumed = 0;
        }
        if (len && h->cb.on_data) h->cb.on_data(s->user, p, len);
        if (flags & FL_END_STREAM) {
            s = find(h, sid);
            if (s) { s->remote_closed = 1; stream_end(h, s, H2_END_OK, 0); }
        }
        return 0;
    }
    case F_HEADERS: {
        if (sid == 0 || (sid & 1) == 0) return conn_error(h, E_PROTOCOL);        // push (even ids) is off
        uint32_t pad = 0;
        if (flags & FL_PADDED) {
            if (len < 1) return conn_error(h, E_FRAME_SIZE);
            pad = p[0]; p++; len--;
        }
        if (flags & FL_PRIORITY) {
            if (len < 5) return conn_error(h, E_FRAME_SIZE);
            p += 5; len -= 5;
        }
        if (pad > len) return conn_error(h, E_PROTOCOL);
        len -= pad;
        if (flags & FL_END_HEADERS) return header_block(h, sid, p, len, (flags & FL_END_STREAM) != 0);
        h->hb_len = 0;
        if (len > H2_MAX_HDR_BLOCK || grow(&h->hb, &h->hb_cap, len)) return conn_error(h, E_INTERNAL);
        memcpy(h->hb, p, len);
        h->hb_len = len;
        h->hb_stream = (int32_t)sid;
        h->hb_end_stream = (uint8_t)((flags & FL_END_STREAM) != 0);
        return 0;
    }
    case F_CONTINUATION: {
        if (!h->hb_stream || sid != (uint32_t)h->hb_stream) return conn_error(h, E_PROTOCOL);
        if (h->hb_len + len > H2_MAX_HDR_BLOCK || grow(&h->hb, &h->hb_cap, h->hb_len + len)) return conn_error(h, E_INTERNAL);
        memcpy(h->hb + h->hb_len, p, len);
        h->hb_len += len;
        if (flags & FL_END_HEADERS) {
            int es = h->hb_end_stream;
            uint32_t s2 = (uint32_t)h->hb_stream;
            h->hb_stream = 0;
            return header_block(h, s2, h->hb, h->hb_len, es);
        }
        return 0;
    }
    case F_PRIORITY:
        if (sid == 0) return conn_error(h, E_PROTOCOL);
        if (len != 5) return conn_error(h, E_FRAME_SIZE);
        return 0;
    case F_RST_STREAM: {
        if (sid == 0) return conn_error(h, E_PROTOCOL);
        if (len != 4) return conn_error(h, E_FRAME_SIZE);
        struct h2_stream* s = find(h, sid);
        if (!s) {
            if (sid >= (uint32_t)h->next_id && (sid & 1)) return conn_error(h, E_PROTOCOL);
            return 0;
        }
        uint32_t code = get32(p);
        stream_end(h, s, (code == E_REFUSED_STREAM && !s->head_done) ? H2_END_RETRY : H2_END_RESET, code);
        return 0;
    }
    case F_SETTINGS:
        if (sid != 0) return conn_error(h, E_PROTOCOL);
        if (flags & FL_ACK) return len == 0 ? 0 : conn_error(h, E_FRAME_SIZE);
        if (len % 6) return conn_error(h, E_FRAME_SIZE);
        return handle_settings(h, p, len);
    case F_PUSH_PROMISE:
        return conn_error(h, E_PROTOCOL);
    case F_PING:
        if (sid != 0) return conn_error(h, E_PROTOCOL);
        if (len != 8) return conn_error(h, E_FRAME_SIZE);
        if (!(flags & FL_ACK)) put_frame(h, F_PING, FL_ACK, 0, p, 8);
        return 0;
    case F_GOAWAY: {
        if (sid != 0) return conn_error(h, E_PROTOCOL);
        if (len < 8) return conn_error(h, E_FRAME_SIZE);
        h->goaway = 1;
        h->goaway_last = get32(p) & 0x7FFFFFFFu;
        for (int i = 0; i < H2_MAX_STREAMS; i++)
            if (h->st[i].id && (uint32_t)h->st[i].id > h->goaway_last) stream_end(h, &h->st[i], H2_END_RETRY, 0);
        return 0;
    }
    case F_WINDOW_UPDATE: {
        if (len != 4) return conn_error(h, E_FRAME_SIZE);
        uint32_t inc = get32(p) & 0x7FFFFFFFu;
        if (sid == 0) {
            if (inc == 0) return conn_error(h, E_PROTOCOL);
            if (h->conn_send_win > 0 && inc > (uint32_t)(0x7FFFFFFF - h->conn_send_win)) return conn_error(h, E_FLOW_CONTROL);
            h->conn_send_win += (int32_t)inc;
        } else {
            struct h2_stream* s = find(h, sid);
            if (inc == 0) {
                if (s) { stream_rst(h, sid, E_PROTOCOL); stream_end(h, s, H2_END_RESET, E_PROTOCOL); }
                return 0;
            }
            if (s) {
                if (s->send_win > 0 && inc > (uint32_t)(0x7FFFFFFF - s->send_win)) { stream_rst(h, sid, E_FLOW_CONTROL); stream_end(h, s, H2_END_RESET, E_FLOW_CONTROL); return 0; }
                s->send_win += (int32_t)inc;
            } else if ((sid & 1) == 0 || sid >= (uint32_t)h->next_id) return conn_error(h, E_PROTOCOL);
        }
        pump(h);
        return 0;
    }
    default:
        return 0;                                                       // unknown frame types are ignored (RFC 9113 4.1)
    }
}

int h2_input(struct h2* h, const uint8_t* data, uint32_t len) {
    if (h->dead) return -1;
    if (len && (grow(&h->in, &h->in_cap, h->in_len + len))) return conn_error(h, E_INTERNAL);
    if (len) { memcpy(h->in + h->in_len, data, len); h->in_len += len; }
    uint32_t pos = 0;
    int rc = 0;
    while (h->in_len - pos >= 9) {
        const uint8_t* f = h->in + pos;
        uint32_t flen = ((uint32_t)f[0] << 16) | ((uint32_t)f[1] << 8) | f[2];
        if (flen > H2_MAX_FRAME_IN) { rc = conn_error(h, E_FRAME_SIZE); break; }
        if (h->in_len - pos < 9 + flen) break;
        uint8_t type = f[3], flags = f[4];
        uint32_t sid = get32(f + 5) & 0x7FFFFFFFu;
        // the payload stays where it is: callbacks may not call back into h2_input, so the buffer does not move under them
        rc = frame(h, type, flags, sid, f + 9, flen);
        pos += 9 + flen;
        if (rc || h->dead) break;
    }
    if (pos) {
        memmove(h->in, h->in + pos, h->in_len - pos);
        h->in_len -= pos;
    }
    if (h->dead && !rc) rc = -1;
    return rc;
}

// ---- output queue, lifecycle -------------------------------------------------------------------------------------------

const uint8_t* h2_out(struct h2* h, uint32_t* len) {
    *len = h->out_len - h->out_off;
    return h->out + h->out_off;
}

void h2_out_done(struct h2* h, uint32_t n) {
    h->out_off += n;
    if (h->out_off >= h->out_len) h->out_off = h->out_len = 0;
}

int h2_dead(const struct h2* h) { return h->dead; }
int h2_active(const struct h2* h) { return h->nactive; }
int h2_draining(const struct h2* h) { return h->goaway; }
int h2_peer_settings_seen(const struct h2* h) { return h->settings_seen; }

struct h2* h2_new(const struct h2_cb* cb) {
    struct h2* h = (struct h2*)h2_alloc(sizeof *h);
    if (!h) return 0;
    memset(h, 0, sizeof *h);
    h->cb = *cb;
    if (hpack_dec_init(&h->dec, 4096)) { h2_free_mem(h); return 0; }
    h->next_id = 1;
    h->conn_send_win = 65535;
    h->peer_init_win = 65535;
    h->peer_max_frame = 16384;
    h->peer_max_conc = 100;
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    grow(&h->out, &h->out_cap, 4096);
    if (!h->out) { hpack_dec_free(&h->dec); h2_free_mem(h); return 0; }
    memcpy(h->out, preface, 24);
    h->out_len = 24;
    uint8_t s[18];
    s[0] = 0; s[1] = 2; put32(s + 2, 0);                       // ENABLE_PUSH = 0
    s[6] = 0; s[7] = 4; put32(s + 8, H2_RECV_STREAM_WIN);      // INITIAL_WINDOW_SIZE
    s[12] = 0; s[13] = 6; put32(s + 14, H2_MAX_HDR_BLOCK);     // MAX_HEADER_LIST_SIZE
    put_frame(h, F_SETTINGS, 0, 0, s, 18);
    put_window_update(h, 0, H2_RECV_CONN_WIN - 65535);         // a big connection window; per-stream windows do the throttling
    return h;
}

void h2_free(struct h2* h) {
    if (!h) return;
    for (int i = 0; i < H2_MAX_STREAMS; i++) h2_free_mem(h->st[i].body);
    hpack_dec_free(&h->dec);
    h2_free_mem(h->out);
    h2_free_mem(h->in);
    h2_free_mem(h->hb);
    h2_free_mem(h->head);
    h2_free_mem(h);
}
