#ifndef NET_FCACHE_H
#define NET_FCACHE_H

#include <stdint.h>

// In-memory HTTP response cache (fetch.c). Whole raw responses (status line + headers + body, as the fetch engine hands
// them out) are kept for as long as their headers say they are fresh; a repeat request for the same resource is answered
// from memory without touching the network. Nothing is revalidated: a stale entry is simply gone.
//
// What may be stored: a complete 200 response to a GET without Set-Cookie, no `no-store` / `no-cache`, a Vary that only
// names things the request does not change (Accept-Encoding, Accept, Origin, User-Agent), and a freshness lifetime from
// Cache-Control max-age (minus Age), Expires - Date, or 10% of (Date - Last-Modified); `immutable` is a day. Lifetimes are
// capped at a day. A prefetched response (speculative load of something the page will ask for in a moment) is kept for at
// least 30 s even without freshness headers, but never against `no-store`.
//
// Time is the kernel's 10 ms tick count; the only wall-clock arithmetic is Expires/Date/Last-Modified differences of
// server-supplied dates (32-bit seconds, valid until 2106).

// Freshness of a complete response: returns 1 and the lifetime in ticks when it may be stored.
int  fcache_freshness(const char* resp, uint32_t len, uint32_t* fresh_ticks, int prefetch);

// Key: two independent 32-bit hashes of everything that makes requests equivalent.
struct fcache_key { uint32_t h1, h2; };
void fcache_key_add(struct fcache_key* k, const char* s, uint32_t n);   // feed bytes (a 0 byte separates fields)
void fcache_key_init(struct fcache_key* k);

// Look up a fresh entry; on a hit returns 1 and points *data at the stored response (valid until the next store/clear).
int  fcache_lookup(const struct fcache_key* k, uint32_t now_ticks, const char** data, uint32_t* len);
// Store a complete response (copied). Does nothing if it is not storable. Returns 1 when stored.
int  fcache_store(const struct fcache_key* k, const char* resp, uint32_t len, uint32_t now_ticks, int prefetch);
void fcache_clear(void);
void fcache_stats(int* entries, uint32_t* bytes, uint32_t* hits, uint32_t* misses);

#endif
