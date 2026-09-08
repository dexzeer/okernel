/*
 * okai JS — kernel glue API (declarations only).
 * Include this from freestanding kernel files (desktop.c) that only need to
 * call the JS engine, without pulling in the host libc shim in js_os.h.
 *
 * "tinyjs ok edition (tm)": substantially rewritten C port of TinyJS.
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#ifndef JS_DOM_H
#define JS_DOM_H

typedef struct js_tiny js_tiny;

extern js_tiny *g_js_engine;
void js_init(void);
void js_run(const char *code);
void js_dom_run_page(const char *html, int html_len);

// Set the current page URL for the JS engine (used by resolve_href in DOM).
void js_set_page_url(const char *url);
// Set the current okai window/tab index so DOM mutations trigger re-render.
void js_set_current_tab(int okai_id, int tab_idx);
// Called after DOM mutations to re-render the current tab.
void js_dom_request_rerender(void);
// Check and clear the rerender flag (called from desktop.c main loop).
int js_dom_is_rerender_needed(void);

#endif /* JS_DOM_H */
