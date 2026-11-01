#ifndef ABLOCK_H
#define ABLOCK_H

// Built-in, always-on adblocker for okai. Speaks the EasyList/uBlock filter
// syntax; the lists are data (filters/, shipped as the "ablists.txt" boot
// module) and the engine is ours — no third-party code.
//
//   network : req_fetch() (okai.c) asks ablock_allows() before anything goes
//             out — before the cache, the prefetch path and fetch_begin; a
//             block is req_fetch's -3, which the existing callers turn into
//             natural page-visible failures (img error events, fetch()
//             rejection, error page for documents).
//   cosmetic: wdoc.c asks ablock_doc_css() for the per-document hide rules
//             ("sel{display:none!important}"), re-collected when the DOM
//             changes. The sheet is small and per-document on purpose: CSS
//             atoms are per-document, so one big shared sheet cannot be
//             reused across documents.
//
// There is no toggle by design. The lists are parsed in idle-path slices
// (ablock_parse_step) and the parse finishes synchronously on first use, so a
// check never sees a half-loaded engine. Everything runs on the main loop
// only; no locks.

#include <stdint.h>

struct wdom;

// Resource kinds (mapped from okai's WDOC_RK_* in okai.c).
#define ABK_MAIN    0   // top-level document navigation
#define ABK_FRAME   1   // iframe document
#define ABK_CSS     2
#define ABK_IMG     3
#define ABK_SCRIPT  4
#define ABK_REQ     5   // fetch()/XMLHttpRequest (media Range GETs included)
#define ABK_FONT    6

const char* ablock_kind_name(int kind);

// Kernel boot: find the ablists boot module and start the parse.
void ablock_init(void);
// Feed a filter-list buffer (kernel: the module memory, zero-copy; host
// tests: any text). Parsing runs in ablock_parse_step slices. 0 ok, -1 OOM.
int  ablock_load(const char* buf, uint32_t len);
// One idle slice. 1 = more to do, 0 = done. budget = TSC cycles (0 or a host
// build = unlimited).
int  ablock_parse_step(uint32_t budget_cycles);
int  ablock_ready(void);
void ablock_reset(void);    // tests only

// 1 = let it through, 0 = block. `top` = the page the request belongs to
// (NULL for a top-level navigation, which untyped filters never match).
int  ablock_allows(const char* url, const char* top, int kind);
// Same, and on a block reports a built-in $redirect= resource to serve
// instead (index >= 0 into ablock_resource; -1 = just block). The redirect
// only ever applies to page subresources, never to documents.
int  ablock_allows2(const char* url, const char* top, int kind, int* redirect);
// Bytes of a built-in redirect resource (noop.js, 1x1.gif, ...).
void ablock_resource(int idx, const char** bytes, int* len, const char** ctype);

// Hide CSS for one document from the list plus what its DOM actually uses.
// Engine-allocator buffer (w_free by the caller), 0 when there is nothing
// to hide.
char* ablock_doc_css(const struct wdom* dom, const char* host);
// Generated JS for this document's ##+js scriptlets (our own implementations
// of the uBlock scriptlet names). Engine-allocator buffer, 0 when none.
// Evaluate in the page realm BEFORE any page script runs.
char* ablock_doc_scriptlets(const char* host);

#endif
