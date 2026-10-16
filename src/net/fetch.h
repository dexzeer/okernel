#ifndef NET_FETCH_H
#define NET_FETCH_H

#include <stdint.h>

// HTTP/HTTPS fetch engine: up to FETCH_MAX requests in flight at once over
// a pool of connections (TCP sockets + TLS 1.3 sessions). Finished
// responses leave their connection idle for reuse (HTTP/1.1 keep-alive):
// the next request to the same origin skips DNS, the TCP handshake and the
// TLS handshake. Requests are GETs; the response is the raw HTTP message.
// Everything is driven by fetch_poll() from the main loop.

#define FETCH_MAX 16

#define FETCH_RUNNING 0
#define FETCH_DONE    1
#define FETCH_FAILED  -1

struct fetch_opts {
    int gzip;            // Accept-Encoding: gzip
    int accept_html;     // navigation Accept header (see NET_ACCEPT_HTML)
    const char* extra;   // extra header lines ("Name: value\r\n"...), or NULL
    const char* method;  // NULL = GET
    const uint8_t* body; // request body (copied), with body_len bytes; Content-Length is added
    uint32_t body_len;
    int own_accept;      // `extra` carries its own Accept header: do not send the default one
    int cache;           // a bodyless GET that may be answered from / stored in the response cache (sub-resources, not documents)
};

// Start a request (GET unless o->method says otherwise). port 0 = scheme default. Returns a handle, or -1 when every
// slot is busy (call again later) or the arguments are unusable.
int   fetch_begin(const char* host, const char* path, uint16_t port, int https,
                  const struct fetch_opts* o);
int   fetch_status(int h);                   // FETCH_RUNNING / DONE / FAILED
// Raw response (status line + headers + body, NUL-terminated); valid until
// fetch_end. The caller may modify it in place (dechunk/inflate).
char* fetch_response(int h, int* len);
int   fetch_progress(int h);                 // bytes received so far
// The response so far (status line + headers + body as received, NOT NUL-terminated, may be raw chunked) of a request still
// running; NULL otherwise. Valid until the next fetch_poll.
const char* fetch_peek(int h, int* len);
// Failure classification: TLS_FAIL_* for secure-channel/certificate
// failures, 0 for transport failures (DNS, connect, reset, timeout).
int   fetch_fail_reason(int h);
int   fetch_fail_detail(int h);              // CV_ERR_* for certificate failures
int   fetch_offer_unaccepted(int h);         // PSK offered, server aborted
int   fetch_saw_close(int h);                // authenticated close_notify seen
// Release the handle. A request still running is cancelled (its
// connection is aborted — a half-read response cannot be reused).
void  fetch_end(int h);
int   fetch_free_slots(void);
void  fetch_poll(void);
// Close every idle pooled connection (fresh handshakes from here on).
void  fetch_close_idle(void);
// Fire-and-forget load of something the page is about to ask for: the response lands in the cache (kept at least 30 s) and
// the slot frees itself. A later fetch_begin for the same request adopts a prefetch still in flight. Returns 1 if started,
// 0 if there is nothing to do (cached / already in flight), -1 if there is no spare capacity right now (try again later).
int   fetch_prefetch(const char* host, const char* path, uint16_t port, int https, const struct fetch_opts* o);
void  fetch_cache_clear(void);
extern int fetch_log_heads;           // debug: log request heads (shell `fetchhdr`)

#endif
