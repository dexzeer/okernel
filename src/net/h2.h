#ifndef NET_H2_H
#define NET_H2_H

#include <stdint.h>

// HTTP/2 client session (RFC 9113): framing, streams, flow control, HPACK. It does no I/O of its own: the caller feeds it the
// bytes that arrive on the TLS connection (h2_input) and sends whatever it has queued (h2_out / h2_out_done). Requests are
// handed in as the HTTP/1.1 message the rest of the browser builds (request line, headers, body) and responses come back
// as an HTTP/1.1 head ("HTTP/1.1 200 OK", headers, blank line) followed by the body bytes, so the code above does not
// have to know which protocol carried them.
//
// Not supported (and not needed by a client that never asks for it): server push (disabled in SETTINGS; a PUSH_PROMISE is a
// protocol error), priorities (accepted and ignored), the extended CONNECT / websocket settings, Alt-Svc / ORIGIN frames
// (ignored like any unknown frame type).

#define H2_END_OK 0
#define H2_END_RESET 1        // RST_STREAM from the peer (code in the `code` argument)
#define H2_END_RETRY 2        // the peer never processed it (GOAWAY below the stream, REFUSED_STREAM): safe to send again
#define H2_END_BROKEN 3       // the connection failed

struct h2;

struct h2_cb {
    // the final response head of a stream (1xx are swallowed), see above
    void (*on_head)(void* user, const char* head, uint32_t len);
    void (*on_data)(void* user, const uint8_t* data, uint32_t len);
    // the stream is over: H2_END_*; for H2_END_OK the response is complete
    void (*on_end)(void* user, int how, uint32_t code);
};

struct h2* h2_new(const struct h2_cb* cb);
void       h2_free(struct h2* h);

// Bytes to send to the peer (valid until the next h2_* call); call h2_out_done(n) for what was actually sent.
const uint8_t* h2_out(struct h2* h, uint32_t* len);
void           h2_out_done(struct h2* h, uint32_t n);

// Feed bytes received from the peer. Returns 0, or a negative HTTP/2 error (the connection is dead: every open stream gets
// on_end(H2_END_BROKEN) and h2_dead() is true).
int  h2_input(struct h2* h, const uint8_t* data, uint32_t len);
int  h2_dead(const struct h2* h);

// Can another stream be opened now (peer's MAX_CONCURRENT_STREAMS, our own cap, no GOAWAY, id space left)?
int  h2_can_open(const struct h2* h);
// Open a stream for the HTTP/1.1 request message req[0..len). `user` comes back in the callbacks. Returns the stream id,
// or -1 (cannot open / unusable request).
int  h2_open(struct h2* h, const uint8_t* req, uint32_t len, void* user);
// Give up on a stream (the caller no longer wants the answer): RST_STREAM(CANCEL), no more callbacks for it.
void h2_cancel(struct h2* h, int stream_id);

int  h2_active(const struct h2* h);        // streams still open
int  h2_peer_settings_seen(const struct h2* h);

#endif
