#ifndef WEB_WDOC_H
#define WEB_WDOC_H

#include <stdint.h>
#include "surface.h"

// A loaded web document: DOM + stylesheets + images + layout. The browser
// shell feeds it bytes and fetch results and asks it to paint.
//
//   d = wdoc_new();
//   wdoc_load(d, url, html, len, charset);
//   while ((id = wdoc_next_fetch(d, url, sizeof url)) >= 0) {
//       ... fetch url ...
//       wdoc_fetch_done(d, id, bytes, len, content_type);   // len < 0: failed
//   }
//   wdoc_set_viewport(d, w, h);
//   wdoc_paint(d, surf, scroll_y);   // relayouts lazily

struct wdoc;

#define WDOC_HIT_NONE   0
#define WDOC_HIT_LINK   1
#define WDOC_HIT_FIELD  2
#define WDOC_HIT_BUTTON 3

struct wdoc_hit {
    int kind;
    int node;          // DOM node of the link / control
    char href[1024];   // absolute URL (links)
};

struct wdoc* wdoc_new(void);
void wdoc_free(struct wdoc* d);
// Parse a document. charset: from the HTTP Content-Type (may be NULL).
int  wdoc_load(struct wdoc* d, const char* url, const char* html, int len, const char* charset);
// Next resource to fetch (absolute URL); returns its id or -1 when none.
int  wdoc_next_fetch(struct wdoc* d, char* url, int cap);
// Deliver a fetched resource (len < 0 = failed).
void wdoc_fetch_done(struct wdoc* d, int id, const char* bytes, int len, const char* ctype);
int  wdoc_pending(struct wdoc* d);       // resources not yet delivered
int  wdoc_is_css(struct wdoc* d, int id);
void wdoc_set_viewport(struct wdoc* d, int w, int h);
// Recompute styles/layout if needed; returns 1 if layout changed.
int  wdoc_update(struct wdoc* d);
int  wdoc_height(struct wdoc* d);        // document height px
int  wdoc_width(struct wdoc* d);
void wdoc_paint(struct wdoc* d, struct wsurf* s, int scroll_y);
// Hit test at viewport (x, y) with the given scroll. Returns kind.
int  wdoc_hit(struct wdoc* d, int x, int y, int scroll_y, struct wdoc_hit* out);
const char* wdoc_title(struct wdoc* d);
const char* wdoc_url(struct wdoc* d);
uint32_t wdoc_background(struct wdoc* d);
struct wdom* wdoc_dom(struct wdoc* d);
// Mark the DOM changed (form typing, scripts): restyle + relayout next update.
void wdoc_invalidate(struct wdoc* d);
// Document y (px) of an element with id/name (fragment navigation), -1 if none.
int  wdoc_anchor_y(struct wdoc* d, const char* frag);
// Diagnostics.
void wdoc_stats(struct wdoc* d, char* out, int cap);

// Base64 / data: URL helper (returns heap bytes + length, or NULL).
char* wdoc_data_url(const char* url, int len, int* out_len, char* mime, int mime_cap);

#endif
