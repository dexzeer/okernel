#ifndef WEB_WJS_INT_H
#define WEB_WJS_INT_H

// Shared internals of the script realm: wjs.c (runtime, scripts, event
// loop, network, storage) and wjs_dom.c (DOM natives).

#include "wjs.h"
#include "wdom.h"
#include "wdoc_int.h"
#include "wcommon.h"
#include "quickjs.h"

// script record
enum { SK_CLASSIC, SK_MODULE };
enum { SM_BLOCK, SM_DEFER, SM_ASYNC };
enum { SS_FETCH, SS_READY, SS_DONE, SS_FAILED };

struct wjs_script {
    int node;            // <script> element (-1: none)
    uint8_t kind, mode, state, parser;
    int res;             // resource id while fetching
    char* url;           // absolute (external) or NULL (inline)
    char* text;          // source (NUL-terminated)
    int len;
};

// module map entry (static import graph, fetched ahead of evaluation)
struct wjs_mod {
    char* url;
    char* text;
    int len;
    uint8_t state;       // SS_*
    uint8_t scanned;
    int res;
};

// import() waiting for its module graph to be fetched
struct wjs_dyn {
    char* basename;
    char* spec;
    int mod;             // module map index
    JSValue resolve, reject, attrs;
};

// node flags
#define NF_STARTED 0x01  // script "already started"
#define NF_PARSER  0x02  // parser-inserted script
#define NF_IMGEVT  0x04  // load/error already fired for this <img>

#define WJS_LOG_MAX 400

struct wjs {
    struct wdoc* doc;
    struct wdom* d;
    JSRuntime* rt;
    JSContext* ctx;
    JSValue hooks;           // object returned by the prelude
    JSValue h_dispatch, h_ui, h_fetch, h_nextdue, h_rundue, h_frame, h_dcl, h_load,
            h_script, h_image, h_proto, h_report, h_mo, h_click, h_ready;
    int interactive;         // readyState "interactive" reached (parsing done)
    JSValue* node_obj;       // wrapper per node (JS_UNDEFINED = none yet)
    uint8_t* node_flags;
    int node_cap;
    JSValue* proto_html;     // cached prototype per HTML tag atom
    int proto_cap;
    JSValue proto_svg, proto_svgroot, proto_math, proto_text, proto_comment, proto_doc, proto_frag;
    int frag_atom;           // atom naming fragment pseudo-elements (unused: WN_FRAG)
    // scripts
    struct wjs_script* sc;
    int nsc, capsc;
    int next_block, next_defer;
    int current_script;      // node of the executing script, -1
    int current_index;       // index into sc[] of the executing parser script
    struct wjs_mod* mods;
    int nmod, capmod;
    char* importmap;         // raw "imports" JSON object text (or NULL)
    struct wjs_dyn* dyn;     // deferred dynamic imports
    int ndyn, capdyn;
    // template element -> content fragment
    int* tpl_map;            // pairs
    int ntpl, captpl;
    // lifecycle
    int dcl_fired, load_fired;
    int t0;                  // creation time (ms)
    int deadline;            // interrupt deadline (ms, 0 = none)
    int interrupted;
    int dead;                // realm aborted: never enter again
    int mo_active;           // a MutationObserver watches childList
    int flags;               // pending WJS_* for the shell
    int next_due;            // cached ms (absolute W.now() time) or -1
    // requests from script
    char* nav_url;
    int nav_replace;
    int scroll_req;          // -1 none
    int focus_req;           // node, -1 none
    int focus_set;
    int hist_delta;
    int submit_form, submit_btn;
    // stats
    int nlog, nerrors, nscripts_run;
    // abort recovery (kernel): __builtin_setjmp buffer
    void* jmp[5];
};

extern JSClassID wjs_class_id;

// wrappers
JSValue wjs_wrap(struct wjs* js, int node);      // new reference; JS_NULL for -1
int     wjs_node_of(struct wjs* js, JSValueConst v);
int     wjs_grow_nodes(struct wjs* js);
// DOM natives (wjs_dom.c): install on the natives object
void    wjs_dom_install(struct wjs* js, JSValue natives);
// mutation side effects (inserted subtree: scripts, styles, images)
void    wjs_inserted(struct wjs* js, int node);
void    wjs_removed(struct wjs* js, int parent, int node);
void    wjs_attr_changed(struct wjs* js, int el, int name_atom);
void    wjs_text_changed(struct wjs* js, int node);
// scripts (wjs.c)
void    wjs_dynamic_script(struct wjs* js, int node);
void    wjs_doc_write(struct wjs* js, const char* html, int len);
void    wjs_report_exception(struct wjs* js, const char* where);
void    wjs_drain_jobs(struct wjs* js);
int     wjs_now(void);
void    wjs_logf(struct wjs* js, int level, const char* s, int len);
// storage / cookies (wjs.c)
JSValue wjs_store_op(struct wjs* js, int area, int op, JSValueConst* argv, int argc);
JSValue wjs_cookie_op(struct wjs* js, JSValueConst setv, int set);

// small helpers
static inline int wjs_str_eq(const char* s, int n, const char* lit) {
    int i = 0;
    for (; i < n && lit[i]; i++) if (s[i] != lit[i]) return 0;
    return i == n && lit[i] == 0;
}

#endif
