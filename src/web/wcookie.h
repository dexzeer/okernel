#ifndef WEB_WCOOKIE_H
#define WEB_WCOOKIE_H

// Cookie jar (RFC 6265 subset) shared by the HTTP fetcher (Set-Cookie /
// Cookie headers) and page scripts (document.cookie). One jar per boot,
// in memory. Domain/path/Secure/HttpOnly/Max-Age/Expires/SameSite are
// honored; third-party (cross-site) sub-resource requests only carry
// SameSite=None cookies.

// Current time in seconds since the epoch (expiry). The shell sets it;
// without it session-only semantics apply (only explicit deletions expire).
extern long long (*wcookie_now)(void);

// A Set-Cookie header value received for `url` (HTTP: may set HttpOnly).
void wcookie_set_http(const char* url, const char* value, int vlen);
// document.cookie = "..." from a page at `url` (never touches HttpOnly).
void wcookie_set_doc(const char* url, const char* value, int vlen);
// Cookie header value for a request to `url`; `top_url` is the page that
// caused it (NULL = top-level navigation). Returns the length written.
int  wcookie_header(const char* url, const char* top_url, char* out, int cap);
// document.cookie getter for a page at `url` (no HttpOnly cookies).
int  wcookie_doc(const char* url, char* out, int cap);
int  wcookie_count(void);

#endif
