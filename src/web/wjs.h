#ifndef WEB_WJS_H
#define WEB_WJS_H

// Page scripting (one ojs realm per document). Owned by wdoc; the shell
// talks to it through the wdoc_js_* API in wdoc.h.

#include <stdint.h>

struct wdoc;
struct wdom;
struct wjs;

// Flags returned by wjs_run / wjs_event (OR-ed).
#define WJS_DIRTY   0x01   // DOM/styles changed: re-render
#define WJS_NAV     0x02   // navigation requested (wdoc_js_take_nav)
#define WJS_SCROLL  0x04   // scroll requested (wdoc_js_take_scroll)
#define WJS_TITLE   0x08   // document.title changed
#define WJS_FETCH   0x10   // new resources queued
#define WJS_URL     0x20   // history.pushState/replaceState changed the URL
#define WJS_FOCUS   0x40   // focus moved by script

// Host services (set by the shell; defaults are host-test friendly).
struct wjs_host {
    void (*random)(uint8_t* out, int n);    // crypto.getRandomValues (CPRNG)
    int  (*now_ms)(void);                   // monotonic ms
    void (*log)(const char* s);             // console output sink
    void (*yield)(void);                    // called now and then while a script runs (>= ~20 ms apart): keep the machine alive
    int  budget_scale;                      // time budgets x this (a slow CPU needs longer; 0 = 1)
};
extern struct wjs_host wjs_host;

int  wjs_now(void);                          // ms (host clock)
void wjs_debug_eval(struct wjs* js, const char* src);   // host builds only
char* wjs_debug_eval_str(struct wjs* js, const char* src);   // host builds only: the result as a malloc'd string (NULL on an exception)
struct wjs* wjs_new(struct wdoc* doc);
void wjs_free(struct wjs* js);
int  wjs_ok(struct wjs* js);                 // realm alive (not aborted/OOM)
// After wdoc parsed the document: register parser-inserted scripts.
void wjs_scan_scripts(struct wjs* js);
// A script / request resource finished (status 0 = network failure).
void wjs_resource_done(struct wjs* js, int res, int status, const char* headers,
                       const char* body, int len, const char* final_url);
// Streaming fetch(): the head of a response (status, raw header lines) and then pieces of its body; the end comes
// through wjs_resource_done with len = -3. Both return WJS_* flags.
int  wjs_fetch_head(struct wjs* js, int res, int status, const char* headers, const char* url);
int  wjs_fetch_chunk(struct wjs* js, int res, const char* data, int len);
// An image resource finished: fire load/error on <img> elements using it.
void wjs_image_done(struct wjs* js, int slot, int ok);
// Run what is due (scripts in order, timers, animation frames, lifecycle
// events) within roughly budget_ms. Returns WJS_* flags.
int  wjs_run(struct wjs* js, int budget_ms);
// ms until something wants to run (0 = now, -1 = idle)
int  wjs_next_due(struct wjs* js);
// Dispatch a UI event at node (-1 = window). Returns WJS_* flags; *prevented
// is set when a listener called preventDefault().
int  wjs_event(struct wjs* js, int node, const char* type, int x, int y, int button, int key,
               int* prevented);
// Scripts still loading/running (gates the load event / busy indicator).
int  wjs_busy(struct wjs* js);
// Navigation / scroll / focus requests from script.
int  wjs_take_nav(struct wjs* js, char* url, int cap, int* replace);
int  wjs_take_scroll(struct wjs* js, int* y);
int  wjs_take_focus(struct wjs* js, int* node);
// Tab keeps a history count for history.length/back(); -1/+1 requests.
int  wjs_take_history(struct wjs* js, int* delta);
// Media (<video>/<audio>): advance every playing session and publish new video frames. Meant
// for the shell's main loop, outside any script run. Returns 1 when a video changed size (the
// document needs a new layout); frames that only changed pixels come back as rectangles in page
// px (unscrolled; fixed = pinned to the viewport), at most `max`, count in *nout.
struct wjs_rect { int x, y, w, h, fixed; };
int  wjs_media_poll(struct wjs* js, struct wjs_rect* out, int max, int* nout);
void wjs_media_tick(struct wjs* js);         // advance the sessions (decode, audio feed) from inside a running script
int  wjs_media_active(struct wjs* js);       // something is playing: keep the main loop running
int  wjs_media_any(struct wjs* js);          // sessions exist (loading, paused...): poll them now and then

// Frames: window.postMessage across <iframe> documents. A script's outgoing message is taken by the shell
// (kind 0 = to the parent, 1 = to the top document, 2 = to the child frame whose <iframe> element is `node`),
// delivered to the other realm, and shown there as a MessageEvent whose source is the sending frame.
int  wjs_take_message(struct wjs* js, int* kind, int* node, char* origin, int ocap, char** data, int* len);   // data: w_malloc'ed (w_free it)
// src_kind: 0 = the message comes from the parent/top, 2 = from the child frame at <iframe> node src_node.
int  wjs_deliver_message(struct wjs* js, const char* data, int len, const char* from_origin, const char* target_origin, int src_kind, int src_node);

// Stats line for diagnostics ("scripts=.. errors=.. heap=..KB").
void wjs_stats(struct wjs* js, char* out, int cap);

#endif
