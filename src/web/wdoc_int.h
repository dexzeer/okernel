#ifndef WEB_WDOC_INT_H
#define WEB_WDOC_INT_H

// Engine-internal wdoc services for the script realm (wjs.c). Not part of
// the shell-facing API in wdoc.h.

#include "wdoc.h"
#include "css.h"

// What a script mutation touched (wdoc_dom_changed)
#define WDC_LAYOUT 0x01   // tree/attributes/text: restyle + relayout
#define WDC_STYLE  0x02   // <style>/<link rel=stylesheet> set changed: rebuild sheets
#define WDC_IMG    0x04   // <img>/<source>/poster src changed: rescan images

struct wdom* wdoc_dom(struct wdoc* d);
const char* wdoc_base(struct wdoc* d);
void wdoc_set_url(struct wdoc* d, const char* url);   // history.pushState: same document
void wdoc_dom_changed(struct wdoc* d, int what);
// Queue a script fetch / a script-initiated request; returns the resource id
// (the completion arrives through wjs_resource_done), -1 if not fetchable.
int  wdoc_res_script(struct wdoc* d, const char* url);
int  wdoc_res_request(struct wdoc* d, const char* url, const char* method, const char* headers,
                      const char* body, int blen);
// Layout queries for scripts (may relayout; throttled while scripts run).
int  wdoc_layout_rect(struct wdoc* d, int node, int* x, int* y, int* w, int* h);
const struct wstyle* wdoc_style_of(struct wdoc* d, int node);
struct wstyleset* wdoc_styleset(struct wdoc* d);
void wdoc_viewport_get(struct wdoc* d, int* vw, int* vh, int* scroll, int* docw, int* doch);
// <img> natural size + state (0 loading, 1 ok, 2 failed); 0 if no image
int  wdoc_img_info(struct wdoc* d, int node, int* w, int* h, int* state);
int  wdoc_img_node_slot(struct wdoc* d, int node);
int  wdoc_hit_element(struct wdoc* d, int x, int y);   // topmost element at viewport point
int  wdoc_focus_node(struct wdoc* d);
int  wdoc_pending_load(struct wdoc* d);                 // CSS/images still loading

#endif
