#ifndef WEB_PSCAN_H
#define WEB_PSCAN_H

// Preload scanner: finds the sub-resources a page is going to ask for in the first bytes of its HTML, before the document
// has finished downloading, so the shell can start fetching them early (the response lands in the HTTP cache and the real
// request, made after parsing, is answered from it or takes over the load in flight).
//
// Looks at <link rel=stylesheet|preload|modulepreload href>, <script src> and <img src> (not loading=lazy, not data:), skips
// everything inside <script>, <style>, <textarea>, <title> and comments, and gives up at a <base> element (URLs would need
// the new base). It never allocates, accepts partial input, and only consumes complete tags: `*next_from` is where the next
// call has to continue (a tag cut off by the end of the buffer is left for later).

#define PSCAN_CSS 0
#define PSCAN_SCRIPT 1
#define PSCAN_IMG 2

// emit gets each absolute URL; returns nonzero to stop scanning because it cannot take it right now (then *next_from is the
// start of that tag, so the next call offers the URL again).
typedef int (*pscan_emit_fn)(void* ctx, const char* abs_url, int kind);

// Scan html[from..len). base: the document URL. Returns the number of URLs emitted; -1 if a <base> element was seen (the
// caller must stop scanning this document).
int pscan_html(const char* html, int len, int from, const char* base, pscan_emit_fn emit, void* ctx, int* next_from);

#endif
