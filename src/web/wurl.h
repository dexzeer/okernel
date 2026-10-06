#ifndef WEB_WURL_H
#define WEB_WURL_H

// URL resolution (RFC 3986 section 5.2: reference resolution + dot-segment
// removal) for http/https/data/about-style URLs. Strings are byte strings;
// characters outside the URL code points are passed through unchanged.

// Resolve `rel` (rlen bytes, surrounding whitespace ignored) against the
// absolute URL `base`. Writes a NUL-terminated absolute URL; returns its
// length, or -1 if the result does not fit / base is unusable.
int wurl_resolve(const char* base, const char* rel, int rlen, char* out, int cap);

// Split helpers (all return lengths, 0 when absent).
int wurl_scheme(const char* url, char* out, int cap);   // "https"
int wurl_host(const char* url, char* out, int cap);     // "example.com"
int wurl_port(const char* url);                         // explicit port or 0
int wurl_path(const char* url, char* out, int cap);     // "/a/b?q" (no fragment)

#endif
