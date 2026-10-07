/*
 * okai JS — kernel glue (DOM bridge + script runner)
 *
 * "tinyjs ok edition (tm)": substantially rewritten C port of TinyJS
 * (gfwilliams/tiny-js + MarcoLizza/tiny-js) adapted to the freestanding
 * okernel. This file is the kernel-facing layer: it owns the global engine
 * instance, registers built-in functions and exposes a `document` object
 * bound to the active okai tab's DOM (src/web/wdom.h): getElementById,
 * querySelector(All), createElement, body, and element methods setText,
 * setStyle, setAttribute, getAttribute, appendChild, innerHTML. Mutations
 * mark the page for restyle + relayout (okai_dom_changed).
 *
 * Page scripts are NOT run by okai (tinyjs is not a spec runtime; a real
 * site's bundle wedges it) — js_run / js_dom_run_page are for the shell.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js.h"
#include "serial.h"
#include "../okai.h"
#include "../web/wdom.h"
#include <string.h>
#include <stdint.h>

/* The single shared browser engine instance (lazily created by js_run). */
js_tiny *g_js_engine = 0;

/* Current page context for DOM mutations. */
static char g_page_url[OKAI_URL_LEN] = {0};
static int g_current_okai = -1;
static int g_current_tab = -1;
static int g_rerender_needed = 0;

void js_set_page_url(const char *url) {
    if (!url) { g_page_url[0] = 0; return; }
    int i = 0;
    while (url[i] && i < OKAI_URL_LEN - 1) { g_page_url[i] = url[i]; i++; }
    g_page_url[i] = 0;
}

void js_set_current_tab(int okai_id, int tab_idx) {
    g_current_okai = okai_id;
    g_current_tab = tab_idx;
}

void js_dom_request_rerender(void) {
    g_rerender_needed = 1;
}

int js_dom_is_rerender_needed(void) {
    int r = g_rerender_needed;
    g_rerender_needed = 0;
    return r;
}

static int cur_okai(void) { return g_current_okai >= 0 ? g_current_okai : 0; }
static struct wdom *cur_dom(void) { return okai_active_dom(cur_okai()); }
static void dom_changed(void) { okai_dom_changed(cur_okai()); }

/* ---- print() / console.log() ------------------------------------------- */
static void scPrint(js_var *c, void *userdata) {
    (void)userdata;
    char buf[4096];
    js_var_get_string(js_var_get_parameter(c, "jsCode"), buf, sizeof(buf));
    serial_printf("[js] %s\n", buf);
}

/* ---- element objects ------------------------------------------------------ */
/* One JS object per DOM node, memoized on the root as "_n<node>". The node
 * index rides in userCustomData (node + 1; 0 = none). */

static int self_node(js_var *c) {
    js_var *self = js_var_get_parameter(c, "this");
    if (!self || !self->userCustomData) return -1;
    struct wdom *d = cur_dom();
    int n = (int)(intptr_t)self->userCustomData - 1;
    if (!d || n < 0 || n >= d->nn) return -1;
    return n;
}

static void scSetText(js_var *c, void *userdata);
static void scSetStyle(js_var *c, void *userdata);
static void scSetAttribute(js_var *c, void *userdata);
static void scGetAttribute(js_var *c, void *userdata);
static void scAppendChild(js_var *c, void *userdata);
static void scAddEventListener(js_var *c, void *userdata);
static void scInnerHTML(js_var *c, void *userdata);

static void add_method(js_var *el, const char *name, js_callback cb, const char *a1, const char *a2) {
    js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
    m->jsCallback = cb;
    if (a1) js_var_add_child(m, a1, js_var_new_blank(JS_V_UNDEFINED));
    if (a2) js_var_add_child(m, a2, js_var_new_blank(JS_V_UNDEFINED));
    js_var_add_child(el, name, m);
}

static js_var *element_for(int node) {
    js_tiny *t = g_js_engine;
    struct wdom *d = cur_dom();
    if (!t || !d || node < 0 || node >= d->nn) return 0;
    char key[24];
    int k = 0, v = node;
    char rev[12];
    int rl = 0;
    key[k++] = '_'; key[k++] = 'n';
    if (!v) rev[rl++] = '0';
    while (v > 0) { rev[rl++] = (char)('0' + v % 10); v /= 10; }
    while (rl) key[k++] = rev[--rl];
    key[k] = 0;
    js_var *el = js_var_get_child(t->root, key);
    if (el) return el;
    el = js_var_new_blank(JS_V_OBJECT);
    js_var_add_child(t->root, key, el);
    el->userCustomData = (void *)(intptr_t)(node + 1);

    static char buf[1024];
    int n = wdom_text_content(d, node, buf, sizeof buf);
    if (n < 0) n = 0;
    buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0;
    js_var_add_child(el, "textContent", js_var_new_string(buf));
    js_var_add_child(el, "innerHTML", js_var_new_string(""));
    {
        char tag[32];
        int tl = 0;
        const char *tn = wdom_tag_name(d, node, &tl);
        int i = 0;
        for (; tn && i < tl && i < 31; i++) tag[i] = (tn[i] >= 'a' && tn[i] <= 'z') ? tn[i] - 32 : tn[i];
        tag[i] = 0;
        js_var_add_child(el, "tagName", js_var_new_string(tag));
    }
    n = wdom_attr_copy(d, node, A_class, buf, sizeof buf);
    js_var_add_child(el, "className", js_var_new_string(n > 0 ? buf : ""));
    n = wdom_attr_copy(d, node, A_id, buf, sizeof buf);
    js_var_add_child(el, "id", js_var_new_string(n > 0 ? buf : ""));
    js_var_add_child(el, "style", js_var_new_blank(JS_V_OBJECT));

    add_method(el, "setText", scSetText, "text", 0);
    add_method(el, "setStyle", scSetStyle, "prop", "value");
    add_method(el, "setAttribute", scSetAttribute, "name", "value");
    add_method(el, "getAttribute", scGetAttribute, "name", 0);
    add_method(el, "appendChild", scAppendChild, "child", 0);
    add_method(el, "addEventListener", scAddEventListener, "type", "fn");
    add_method(el, "setInnerHTML", scInnerHTML, "value", 0);
    return el;
}

static void return_element(js_var *c, int node) {
    js_var *el = node >= 0 ? element_for(node) : 0;
    if (el) js_var_copy_value(js_var_get_return_var(c), el);
    else js_var_set_undefined(js_var_get_return_var(c));
}

/* ---- simple selectors: #id, .class, tag, tag.class ------------------------ */
static int sel_match(struct wdom *d, int n, const char *sel) {
    if (d->n[n].type != WN_ELEM) return 0;
    if (sel[0] == '#') {
        int l;
        const char *v = wdom_attr(d, n, A_id, &l);
        return v && l == (int)strlen(sel + 1) && !memcmp(v, sel + 1, l);
    }
    const char *dot = sel;
    while (*dot && *dot != '.') dot++;
    int tl = (int)(dot - sel);
    if (tl > 0) {
        int nl = 0;
        const char *tn = wdom_tag_name(d, n, &nl);
        if (!tn || nl != tl) return 0;
        for (int i = 0; i < tl; i++) {
            char a = tn[i], b = sel[i];
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
        }
    }
    if (*dot == '.') {
        int ca = watom_find(&d->atoms, dot + 1, (int)strlen(dot + 1));
        return ca && wdom_has_class(d, n, ca);
    }
    return 1;
}

static void scGetElementById(js_var *c, void *userdata) {
    (void)userdata;
    char id[256];
    js_var_get_string(js_var_get_parameter(c, "id"), id, sizeof(id));
    struct wdom *d = cur_dom();
    return_element(c, d ? wdom_find_id(d, id) : -1);
}

static void scQuerySelector(js_var *c, void *userdata) {
    (void)userdata;
    char sel[256];
    js_var_get_string(js_var_get_parameter(c, "selector"), sel, sizeof(sel));
    struct wdom *d = cur_dom();
    int found = -1;
    if (d && sel[0])
        for (int n = d->n[0].first; n >= 0; n = wdom_next(d, n, 0))
            if (sel_match(d, n, sel)) { found = n; break; }
    return_element(c, found);
}

static void scQuerySelectorAll(js_var *c, void *userdata) {
    (void)userdata;
    char sel[256];
    js_var_get_string(js_var_get_parameter(c, "selector"), sel, sizeof(sel));
    js_var *arr = js_var_new_blank(JS_V_ARRAY);
    struct wdom *d = cur_dom();
    int idx = 0;
    if (d && sel[0])
        for (int n = d->n[0].first; n >= 0 && idx < 1000; n = wdom_next(d, n, 0)) {
            if (!sel_match(d, n, sel)) continue;
            js_var *el = element_for(n);
            if (!el) continue;
            char iname[12];
            int k = 0, v = idx;
            char rev[12];
            int rl = 0;
            if (!v) rev[rl++] = '0';
            while (v > 0) { rev[rl++] = (char)('0' + v % 10); v /= 10; }
            while (rl) iname[k++] = rev[--rl];
            iname[k] = 0;
            js_var_add_child(arr, iname, el);
            idx++;
        }
    js_var_copy_value(js_var_get_return_var(c), arr);
}

static void scCreateElement(js_var *c, void *userdata) {
    (void)userdata;
    char tag[64];
    js_var_get_string(js_var_get_parameter(c, "tag"), tag, sizeof(tag));
    struct wdom *d = cur_dom();
    int n = -1;
    if (d && tag[0]) {
        for (int i = 0; tag[i]; i++) if (tag[i] >= 'A' && tag[i] <= 'Z') tag[i] += 32;
        int at = watom_intern(&d->atoms, tag, (int)strlen(tag));
        if (at) n = wdom_create_element(d, NS_HTML, at);
    }
    return_element(c, n);
}

static void scSetText(js_var *c, void *userdata) {
    (void)userdata;
    int n = self_node(c);
    static char text[4096];
    js_var_get_string(js_var_get_parameter(c, "text"), text, sizeof(text));
    if (n < 0) return;
    wdom_set_text_content(cur_dom(), n, text, (int)strlen(text));
    js_var *tc = js_var_get_child(js_var_get_parameter(c, "this"), "textContent");
    if (tc) js_var_set_string(tc, text);
    serial_printf("[js] setText(node %d)\n", n);
    dom_changed();
    js_dom_request_rerender();
}

static void scSetStyle(js_var *c, void *userdata) {
    (void)userdata;
    int n = self_node(c);
    char prop[64], value[256];
    js_var_get_string(js_var_get_parameter(c, "prop"), prop, sizeof(prop));
    js_var_get_string(js_var_get_parameter(c, "value"), value, sizeof(value));
    if (n < 0 || !prop[0]) return;
    struct wdom *d = cur_dom();
    /* append "prop:value" — later declarations win in the cascade */
    static char st[2048];
    int l = wdom_attr_copy(d, n, A_style, st, sizeof st);
    if (l < 0) l = 0;
    if (l + (int)strlen(prop) + (int)strlen(value) + 3 < (int)sizeof st) {
        if (l > 0 && st[l - 1] != ';') st[l++] = ';';
        for (int i = 0; prop[i]; i++) st[l++] = prop[i];
        st[l++] = ':';
        for (int i = 0; value[i]; i++) st[l++] = value[i];
        wdom_set_attr(d, n, A_style, st, l);
    }
    js_var *style = js_var_get_child(js_var_get_parameter(c, "this"), "style");
    if (style) {
        js_var *sv = js_var_get_child(style, prop);
        if (sv) js_var_set_string(sv, value);
        else js_var_add_child(style, prop, js_var_new_string(value));
    }
    serial_printf("[js] setStyle(node %d, %s:%s)\n", n, prop, value);
    dom_changed();
    js_dom_request_rerender();
}

static void scSetAttribute(js_var *c, void *userdata) {
    (void)userdata;
    int n = self_node(c);
    char name[64], value[512];
    js_var_get_string(js_var_get_parameter(c, "name"), name, sizeof(name));
    js_var_get_string(js_var_get_parameter(c, "value"), value, sizeof(value));
    if (n < 0 || !name[0]) return;
    struct wdom *d = cur_dom();
    for (int i = 0; name[i]; i++) if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 32;
    int at = watom_intern(&d->atoms, name, (int)strlen(name));
    if (at) wdom_set_attr(d, n, at, value, (int)strlen(value));
    js_var *self = js_var_get_parameter(c, "this");
    js_var *sv = js_var_get_child(self, name);
    if (sv) js_var_set_string(sv, value);
    dom_changed();
    js_dom_request_rerender();
}

static void scGetAttribute(js_var *c, void *userdata) {
    (void)userdata;
    int n = self_node(c);
    char name[64];
    js_var_get_string(js_var_get_parameter(c, "name"), name, sizeof(name));
    struct wdom *d = cur_dom();
    static char buf[1024];
    int l = -1;
    if (n >= 0 && name[0]) {
        for (int i = 0; name[i]; i++) if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 32;
        int at = watom_find(&d->atoms, name, (int)strlen(name));
        if (at) l = wdom_attr_copy(d, n, at, buf, sizeof buf);
    }
    if (l >= 0) js_var_set_string(js_var_get_return_var(c), buf);
    else js_var_set_undefined(js_var_get_return_var(c));
}

static void scAppendChild(js_var *c, void *userdata) {
    (void)userdata;
    int n = self_node(c);
    js_var *child = js_var_get_parameter(c, "child");
    struct wdom *d = cur_dom();
    int cn = (child && child->userCustomData) ? (int)(intptr_t)child->userCustomData - 1 : -1;
    if (n >= 0 && d && cn >= 0 && cn < d->nn && cn != n) {
        /* refuse cycles: n must not be inside cn */
        int bad = 0;
        for (int p = n; p >= 0; p = d->n[p].parent) if (p == cn) { bad = 1; break; }
        if (!bad) {
            if (d->n[cn].parent >= 0) wdom_remove(d, cn);
            wdom_append(d, n, cn);
            dom_changed();
            js_dom_request_rerender();
        }
    }
    if (child) js_var_copy_value(js_var_get_return_var(c), child);
}

static void scAddEventListener(js_var *c, void *userdata) {
    (void)userdata;
    char type[32];
    js_var_get_string(js_var_get_parameter(c, "type"), type, sizeof(type));
    serial_printf("[js] addEventListener(node %d, %s) — not dispatched\n", self_node(c), type);
}

static void scInnerHTML(js_var *c, void *userdata) {
    (void)userdata;
    int n = self_node(c);
    static char html[8192];
    js_var_get_string(js_var_get_parameter(c, "value"), html, sizeof(html));
    if (n < 0) return;
    struct wdom *d = cur_dom();
    while (d->n[n].first >= 0) wdom_remove(d, d->n[n].first);
    whtml_parse_fragment(d, n, html, (int)strlen(html));
    js_var *hv = js_var_get_child(js_var_get_parameter(c, "this"), "innerHTML");
    if (hv) js_var_set_string(hv, html);
    dom_changed();
    js_dom_request_rerender();
}

static void scDocumentBody(js_var *c, void *userdata) {
    (void)userdata;
    struct wdom *d = cur_dom();
    return_element(c, d ? d->body : -1);
}

/* ---- engine lifecycle -------------------------------------------------- */
void js_init(void) {
    if (g_js_engine) return;
    g_js_engine = js_create();
    registerFunctions(g_js_engine);
    registerMathFunctions(g_js_engine);

    js_add_native(g_js_engine, "function print(jsCode)", scPrint, 0);
    js_add_native(g_js_engine, "function console.log(jsCode)", scPrint, 0);
    js_add_native(g_js_engine, "function document.getElementById(id)", scGetElementById, 0);
    js_add_native(g_js_engine, "function document.querySelector(selector)", scQuerySelector, 0);
    js_add_native(g_js_engine, "function document.querySelectorAll(selector)", scQuerySelectorAll, 0);
    js_add_native(g_js_engine, "function document.createElement(tag)", scCreateElement, 0);
    js_add_native(g_js_engine, "function document.getBody()", scDocumentBody, 0);
}

/* Run a script; surface any error to the serial console. */
void js_run(const char *code) {
    if (!g_js_engine) js_init();
    js_execute(g_js_engine, code);
    if (js_has_error(g_js_engine)) {
        serial_printf("[js] ERROR: %s\n", g_js_engine->error);
    }
}

/* Run the inline <script> blocks of the active page's DOM. */
void js_dom_run_page(const char *html, int html_len) {
    (void)html; (void)html_len;
    struct wdom *d = cur_dom();
    if (!d) return;
    static char buf[16384];
    for (int n = d->n[0].first; n >= 0; n = wdom_next(d, n, 0)) {
        if (!wdom_is(d, n, T_script) || wdom_has_attr(d, n, A_src)) continue;
        int l = wdom_text_content(d, n, buf, sizeof buf);
        if (l <= 0 || l >= (int)sizeof buf - 1) continue;
        buf[l] = 0;
        js_run(buf);
    }
}
