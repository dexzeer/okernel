/*
 * okai JS — kernel glue (DOM bridge + script runner)
 *
 * "tinyjs ok edition (tm)": substantially rewritten C port of TinyJS
 * (gfwilliams/tiny-js + MarcoLizza/tiny-js) adapted to the freestanding
 * okernel. This file is the kernel-facing layer: it owns the global engine
 * instance, registers built-in functions, exposes a working `document` object
 * with getElementById/querySelector/createElement/body/setText/setStyle, and
 * extracts <script> blocks from fetched HTML.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js.h"
#include "serial.h"
#include "../html.h"
#include "../okai.h"
#include <string.h>

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

/* ---- print() / console.log() ------------------------------------------- */
static void scPrint(js_var *c, void *userdata) {
    (void)userdata;
    char buf[4096];
    js_var_get_string(js_var_get_parameter(c, "jsCode"), buf, sizeof(buf));
    serial_printf("[js] %s\n", buf);
}
static void scConsoleLog(js_var *c, void *userdata) {
    (void)userdata;
    char buf[4096];
    js_var_get_string(js_var_get_parameter(c, "jsCode"), buf, sizeof(buf));
    serial_printf("[js] %s\n", buf);
}

/* ---- Element helpers ---------------------------------------------------- */

/* Find a token by id in the current tab. Returns index or -1. */
static int dom_find_token_by_id(const char *id) {
    if (g_current_okai < 0 || g_current_tab < 0) return -1;
    struct okai *ok = okai_get(g_current_okai);
    if (!ok) return -1;
    struct okai_tab *T = okai_tab_of(ok);
    if (!T) return -1;
    for (int i = 0; i < T->token_count; i++) {
        if (T->tokens[i].id[0] && js_strcmp(T->tokens[i].id, id) == 0)
            return i;
    }
    return -1;
}

/* Find a token by tag name (first match). Returns index or -1. */
static int dom_find_token_by_tag(const char *tag) {
    if (g_current_okai < 0 || g_current_tab < 0) return -1;
    struct okai *ok = okai_get(g_current_okai);
    if (!ok) return -1;
    struct okai_tab *T = okai_tab_of(ok);
    if (!T) return -1;
    for (int i = 0; i < T->token_count; i++) {
        if (js_strcmp(T->tokens[i].tag, tag) == 0)
            return i;
    }
    return -1;
}

/* ---- document.getElementById(id) -> element object --------------------- */
static js_var *js_dom_get_element(const char *id) {
    js_tiny *t = g_js_engine;
    /* Use a namespaced key to avoid collisions with JS vars */
    char key[256];
    key[0] = '_'; key[1] = 'e'; key[2] = 'l'; key[3] = '_';
    int i = 4;
    while (id[i-4] && i < (int)sizeof(key) - 1) { key[i] = id[i-4]; i++; }
    key[i] = 0;

    js_var *el = js_var_get_child(t->root, key);
    if (el) return el;

    el = js_var_new_blank(JS_V_OBJECT);
    js_var_add_child(t->root, key, el);

    /* Store the element id as custom data for lookups */
    char *id_copy = (char *)js_malloc(strlen(id) + 1);
    js_strcpy(id_copy, id);
    el->userCustomData = id_copy;

    /* element.textContent (string) */
    js_var_add_child(el, "textContent", js_var_new_string(""));
    /* element.innerHTML (string) */
    js_var_add_child(el, "innerHTML", js_var_new_string(""));
    /* element.tagName (string, uppercase) */
    {
        char tag_upper[16] = {0};
        int ti = 0;
        int tidx = dom_find_token_by_id(id);
        if (tidx >= 0 && g_current_okai >= 0) {
            struct okai *ok = okai_get(g_current_okai);
            if (ok) {
                struct okai_tab *T = okai_tab_of(ok);
                if (T) {
                    const char *src = T->tokens[tidx].tag;
                    while (src[ti] && ti < 15) { tag_upper[ti] = src[ti]; ti++; }
                }
            }
        }
        tag_upper[ti] = 0;
        js_var_add_child(el, "tagName", js_var_new_string(tag_upper));
    }
    /* element.className (string) */
    js_var_add_child(el, "className", js_var_new_string(""));
    /* element.style (object) */
    js_var *style = js_var_new_blank(JS_V_OBJECT);
    js_var_add_child(el, "style", style);
    /* element.id (string) */
    js_var_add_child(el, "id", js_var_new_string(id));

    return el;
}

static void scGetElementById(js_var *c, void *userdata) {
    (void)userdata;
    char id[256];
    js_var_get_string(js_var_get_parameter(c, "id"), id, sizeof(id));
    js_var *el = js_dom_get_element(id);
    js_var_copy_value(js_var_get_return_var(c), el);
}

/* ---- document.querySelector(selector) ---------------------------------- */
/* Supports: "#id", ".class", "tag" selectors. Returns first match. */
static void scQuerySelector(js_var *c, void *userdata) {
    (void)userdata;
    char sel[256];
    js_var_get_string(js_var_get_parameter(c, "selector"), sel, sizeof(sel));
    if (g_current_okai < 0 || g_current_tab < 0) {
        js_var_set_undefined(js_var_get_return_var(c));
        return;
    }
    struct okai *ok = okai_get(g_current_okai);
    if (!ok) { js_var_set_undefined(js_var_get_return_var(c)); return; }
    struct okai_tab *T = okai_tab_of(ok);
    if (!T) { js_var_set_undefined(js_var_get_return_var(c)); return; }

    int found = -1;
    if (sel[0] == '#') {
        /* ID selector */
        found = dom_find_token_by_id(sel + 1);
    } else if (sel[0] == '.') {
        /* Class selector — find first token with matching class */
        for (int i = 0; i < T->token_count; i++) {
            if (T->tokens[i].cls[0] && js_strcmp(T->tokens[i].cls, sel + 1) == 0) {
                found = i; break;
            }
        }
    } else {
        /* Tag selector */
        found = dom_find_token_by_tag(sel);
    }

    if (found >= 0 && T->tokens[found].id[0]) {
        js_var *el = js_dom_get_element(T->tokens[found].id);
        js_var_copy_value(js_var_get_return_var(c), el);
    } else if (found >= 0) {
        /* Token has no id — create a temporary element with a generated id */
        char gen_id[32];
        gen_id[0] = '_'; gen_id[1] = 'q'; gen_id[2] = 's';
        int n = 3;
        int v = found;
        if (v >= 100) { gen_id[n++] = '0' + v / 100; v %= 100; }
        if (v >= 10) { gen_id[n++] = '0' + v / 10; v %= 10; }
        gen_id[n++] = '0' + v;
        gen_id[n] = 0;
        js_var *el = js_dom_get_element(gen_id);
        js_var_copy_value(js_var_get_return_var(c), el);
    } else {
        js_var_set_undefined(js_var_get_return_var(c));
    }
}

/* ---- document.querySelectorAll(selector) ------------------------------- */
/* Returns an array of matching elements. */
static void scQuerySelectorAll(js_var *c, void *userdata) {
    (void)userdata;
    char sel[256];
    js_var_get_string(js_var_get_parameter(c, "selector"), sel, sizeof(sel));
    js_var *arr = js_var_new_blank(JS_V_ARRAY);
    if (g_current_okai < 0 || g_current_tab < 0) {
        js_var_copy_value(js_var_get_return_var(c), arr);
        return;
    }
    struct okai *ok = okai_get(g_current_okai);
    if (!ok) { js_var_copy_value(js_var_get_return_var(c), arr); return; }
    struct okai_tab *T = okai_tab_of(ok);
    if (!T) { js_var_copy_value(js_var_get_return_var(c), arr); return; }

    int idx = 0;
    for (int i = 0; i < T->token_count; i++) {
        int match = 0;
        if (sel[0] == '#' && T->tokens[i].id[0] &&
            js_strcmp(T->tokens[i].id, sel + 1) == 0) match = 1;
        else if (sel[0] == '.' && T->tokens[i].cls[0] &&
                 js_strcmp(T->tokens[i].cls, sel + 1) == 0) match = 1;
        else if (sel[0] != '#' && sel[0] != '.' &&
                 js_strcmp(T->tokens[i].tag, sel) == 0) match = 1;

        if (match) {
            char id_buf[32];
            if (T->tokens[i].id[0]) {
                js_strcpy(id_buf, T->tokens[i].id);
            } else {
                id_buf[0] = '_'; id_buf[1] = 'q'; id_buf[2] = 'a';
                int n = 3, v = i;
                if (v >= 100) { id_buf[n++] = '0' + v / 100; v %= 100; }
                if (v >= 10) { id_buf[n++] = '0' + v / 10; v %= 10; }
                id_buf[n++] = '0' + v;
                id_buf[n] = 0;
            }
            js_var *el = js_dom_get_element(id_buf);
            char iname[16];
            iname[0] = 0;
            /* Convert index to string for array key */
            int tmp = idx, digits = 0, t2 = tmp;
            while (t2) { digits++; t2 /= 10; }
            if (digits == 0) digits = 1;
            for (int d = digits - 1; d >= 0; d--) {
                iname[d] = '0' + (tmp % 10);
                tmp /= 10;
            }
            iname[digits] = 0;
            js_var_add_child(arr, iname, el);
            idx++;
        }
    }
    js_var_copy_value(js_var_get_return_var(c), arr);
}

/* ---- document.createElement(tag) --------------------------------------- */
static void scCreateElement(js_var *c, void *userdata) {
    (void)userdata;
    char tag[64];
    js_var_get_string(js_var_get_parameter(c, "tag"), tag, sizeof(tag));
    /* Create a generic element with a generated id */
    char gen_id[32];
    gen_id[0] = '_'; gen_id[1] = 'c'; gen_id[2] = 'e';
    static int ce_counter = 0;
    int v = ce_counter++;
    int n = 3;
    if (v >= 1000) { gen_id[n++] = '0' + v / 1000; v %= 1000; }
    if (v >= 100) { gen_id[n++] = '0' + v / 100; v %= 100; }
    if (v >= 10) { gen_id[n++] = '0' + v / 10; v %= 10; }
    gen_id[n++] = '0' + v;
    gen_id[n] = 0;

    js_var *el = js_dom_get_element(gen_id);
    /* Update the tagName */
    js_var *tn = js_var_get_child(el, "tagName");
    if (tn) {
        char upper[16] = {0};
        int i = 0;
        while (tag[i] && i < 15) {
            char ch = tag[i];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            upper[i] = ch; i++;
        }
        upper[i] = 0;
        js_var_set_string(tn, upper);
    }
    js_var_copy_value(js_var_get_return_var(c), el);
}

/* ---- document.body ----------------------------------------------------- */
static void scDocumentBody(js_var *c, void *userdata) {
    (void)userdata;
    js_var *el = js_dom_get_element("body");
    js_var_copy_value(js_var_get_return_var(c), el);
}

/* ---- element.setText(text) — set textContent + re-render ---------------- */
static void scSetText(js_var *c, void *userdata) {
    (void)userdata;
    js_var *self = js_var_get_parameter(c, "this");
    if (!self) return;
    char *eid = (char *)self->userCustomData;
    if (!eid) return;

    char text[HTML_MAX_TEXT];
    js_var_get_string(js_var_get_parameter(c, "text"), text, sizeof(text));

    /* Update the token's text */
    int tidx = dom_find_token_by_id(eid);
    if (tidx >= 0 && g_current_okai >= 0) {
        struct okai *ok = okai_get(g_current_okai);
        if (ok) {
            struct okai_tab *T = okai_tab_of(ok);
            if (T) {
                int i = 0;
                while (text[i] && i < HTML_MAX_TEXT - 1) {
                    T->tokens[tidx].text[i] = text[i]; i++;
                }
                T->tokens[tidx].text[i] = 0;
                js_dom_request_rerender();
                serial_printf("[js] setText(%s) -> token[%d]\n", eid, tidx);
            }
        }
    }

    /* Also update the JS textContent property */
    js_var *tc = js_var_get_child(self, "textContent");
    if (tc) js_var_set_string(tc, text);
}

/* ---- element.setStyle(prop, value) — set inline style + re-render ------ */
static void scSetStyle(js_var *c, void *userdata) {
    (void)userdata;
    js_var *self = js_var_get_parameter(c, "this");
    if (!self) return;
    char *eid = (char *)self->userCustomData;
    if (!eid) return;

    char prop[64], value[128];
    js_var_get_string(js_var_get_parameter(c, "prop"), prop, sizeof(prop));
    js_var_get_string(js_var_get_parameter(c, "value"), value, sizeof(value));

    /* Update the token's inline style */
    int tidx = dom_find_token_by_id(eid);
    if (tidx >= 0 && g_current_okai >= 0) {
        struct okai *ok = okai_get(g_current_okai);
        if (ok) {
            struct okai_tab *T = okai_tab_of(ok);
            if (T) {
                /* Append or update the style property in the token's style string */
                char *st = T->tokens[tidx].style;
                int slen = 0;
                while (st[slen]) slen++;

                /* Check if property already exists in the style string */
                int found = 0;
                for (int k = 0; k < slen; k++) {
                    int match = 1;
                    for (int m = 0; prop[m]; m++) {
                        if (k + m >= slen || st[k + m] != prop[m]) { match = 0; break; }
                    }
                    if (match && k + (int)strlen(prop) < slen &&
                        st[k + strlen(prop)] == ':') {
                        /* Replace the value */
                        int vstart = k + strlen(prop) + 1;
                        while (vstart < slen && st[vstart] == ' ') vstart++;
                        int vend = vstart;
                        while (vend < slen && st[vend] != ';' && st[vend] != ' ') vend++;
                        /* Build new style: before + prop:value + after */
                        char new_style[128];
                        int ns = 0;
                        for (int j = 0; j < k; j++) new_style[ns++] = st[j];
                        for (int j = 0; prop[j]; j++) new_style[ns++] = prop[j];
                        new_style[ns++] = ':';
                        for (int j = 0; value[j]; j++) new_style[ns++] = value[j];
                        for (int j = vend; j < slen && ns < 127; j++)
                            new_style[ns++] = st[j];
                        new_style[ns] = 0;
                        int ci = 0;
                        while (new_style[ci] && ci < 127) {
                            T->tokens[tidx].style[ci] = new_style[ci]; ci++;
                        }
                        T->tokens[tidx].style[ci] = 0;
                        found = 1;
                        break;
                    }
                }
                if (!found && slen + (int)strlen(prop) + (int)strlen(value) + 3 < 128) {
                    if (slen > 0 && st[slen-1] != ';') {
                        st[slen++] = ';';
                    }
                    for (int j = 0; prop[j]; j++) st[slen++] = prop[j];
                    st[slen++] = ':';
                    for (int j = 0; value[j]; j++) st[slen++] = value[j];
                    st[slen] = 0;
                }
                js_dom_request_rerender();
                serial_printf("[js] setStyle(%s, %s:%s) -> token[%d]\n",
                              eid, prop, value, tidx);
            }
        }
    }

    /* Also update the JS style object */
    js_var *style = js_var_get_child(self, "style");
    if (style) {
        js_var *sv = js_var_get_child(style, prop);
        if (sv) js_var_set_string(sv, value);
        else js_var_add_child(style, prop, js_var_new_string(value));
    }
}

/* ---- element.setAttribute(name, value) --------------------------------- */
static void scSetAttribute(js_var *c, void *userdata) {
    (void)userdata;
    js_var *self = js_var_get_parameter(c, "this");
    if (!self) return;
    char *eid = (char *)self->userCustomData;
    if (!eid) return;

    char name[64], value[256];
    js_var_get_string(js_var_get_parameter(c, "name"), name, sizeof(name));
    js_var_get_string(js_var_get_parameter(c, "value"), value, sizeof(value));

    /* Map common attributes to token fields */
    int tidx = dom_find_token_by_id(eid);
    if (tidx >= 0 && g_current_okai >= 0) {
        struct okai *ok = okai_get(g_current_okai);
        if (ok) {
            struct okai_tab *T = okai_tab_of(ok);
            if (T) {
                struct html_token *tok = &T->tokens[tidx];
                if (js_strcmp(name, "class") == 0) {
                    int i = 0;
                    while (value[i] && i < 31) { tok->cls[i] = value[i]; i++; }
                    tok->cls[i] = 0;
                } else if (js_strcmp(name, "id") == 0) {
                    int i = 0;
                    while (value[i] && i < 31) { tok->id[i] = value[i]; i++; }
                    tok->id[i] = 0;
                } else if (js_strcmp(name, "style") == 0) {
                    int i = 0;
                    while (value[i] && i < 127) { tok->style[i] = value[i]; i++; }
                    tok->style[i] = 0;
                } else if (js_strcmp(name, "href") == 0) {
                    int i = 0;
                    while (value[i] && i < 63) { tok->href[i] = value[i]; i++; }
                    tok->href[i] = 0;
                }
                js_dom_request_rerender();
            }
        }
    }

    /* Also store on the JS object */
    js_var *sv = js_var_get_child(self, name);
    if (sv) js_var_set_string(sv, value);
    else js_var_add_child(self, name, js_var_new_string(value));
}

/* ---- element.getAttribute(name) ---------------------------------------- */
static void scGetAttribute(js_var *c, void *userdata) {
    (void)userdata;
    js_var *self = js_var_get_parameter(c, "this");
    if (!self) { js_var_set_string(js_var_get_return_var(c), ""); return; }

    char name[64];
    js_var_get_string(js_var_get_parameter(c, "name"), name, sizeof(name));

    char *eid = (char *)self->userCustomData;
    if (eid && g_current_okai >= 0) {
        struct okai *ok = okai_get(g_current_okai);
        if (ok) {
            struct okai_tab *T = okai_tab_of(ok);
            if (T) {
                int tidx = dom_find_token_by_id(eid);
                if (tidx >= 0) {
                    struct html_token *tok = &T->tokens[tidx];
                    if (js_strcmp(name, "class") == 0) {
                        js_var_set_string(js_var_get_return_var(c), tok->cls);
                        return;
                    } else if (js_strcmp(name, "id") == 0) {
                        js_var_set_string(js_var_get_return_var(c), tok->id);
                        return;
                    } else if (js_strcmp(name, "style") == 0) {
                        js_var_set_string(js_var_get_return_var(c), tok->style);
                        return;
                    } else if (js_strcmp(name, "href") == 0) {
                        js_var_set_string(js_var_get_return_var(c), tok->href);
                        return;
                    }
                }
            }
        }
    }

    /* Fallback: read from JS object */
    js_var *sv = js_var_get_child(self, name);
    if (sv) {
        char buf[256];
        js_var_get_string(sv, buf, sizeof(buf));
        js_var_set_string(js_var_get_return_var(c), buf);
    } else {
        js_var_set_string(js_var_get_return_var(c), "");
    }
}

/* ---- element.appendChild(child) — no-op (stub) ------------------------- */
static void scAppendChild(js_var *c, void *userdata) {
    (void)userdata;
    /* Stub: log and ignore */
    js_var *self = js_var_get_parameter(c, "this");
    js_var *child = js_var_get_parameter(c, "child");
    if (self && child) {
        char id_buf[64] = {0};
        char *eid = (char *)self->userCustomData;
        if (eid) { int i = 0; while (eid[i] && i < 63) { id_buf[i] = eid[i]; i++; } }
        serial_printf("[js] appendChild(%s) — stub\n", id_buf);
    }
    /* Return the child (standard DOM API) */
    if (child) js_var_copy_value(js_var_get_return_var(c), child);
}

/* ---- element.addEventListener(type, fn) — stores callback (stub) ------- */
static void scAddEventListener(js_var *c, void *userdata) {
    (void)userdata;
    js_var *self = js_var_get_parameter(c, "this");
    char type[32];
    js_var_get_string(js_var_get_parameter(c, "type"), type, sizeof(type));
    if (self) {
        char *eid = (char *)self->userCustomData;
        serial_printf("[js] addEventListener(%s, %s) — stub\n",
                      eid ? eid : "?", type);
    }
}

/* ---- element.innerHTML (getter/setter stub) ---------------------------- */
static void scInnerHTML(js_var *c, void *userdata) {
    (void)userdata;
    js_var *self = js_var_get_parameter(c, "this");
    if (!self) return;
    char *eid = (char *)self->userCustomData;
    if (!eid) return;

    /* Check if called as setter (with parameter "value") */
    js_var *val_param = js_var_get_parameter(c, "value");
    if (val_param) {
        char html_str[1024];
        js_var_get_string(val_param, html_str, sizeof(html_str));
        /* Simple: put the HTML text into the token's text field (strip tags) */
        int tidx = dom_find_token_by_id(eid);
        if (tidx >= 0 && g_current_okai >= 0) {
            struct okai *ok = okai_get(g_current_okai);
            if (ok) {
                struct okai_tab *T = okai_tab_of(ok);
                if (T) {
                    /* Strip HTML tags for a simple text extraction */
                    char plain[HTML_MAX_TEXT];
                    int pi = 0;
                    int in_tag = 0;
                    for (int k = 0; html_str[k] && pi < HTML_MAX_TEXT - 1; k++) {
                        if (html_str[k] == '<') in_tag = 1;
                        else if (html_str[k] == '>') in_tag = 0;
                        else if (!in_tag) plain[pi++] = html_str[k];
                    }
                    plain[pi] = 0;
                    int i = 0;
                    while (plain[i] && i < HTML_MAX_TEXT - 1) {
                        T->tokens[tidx].text[i] = plain[i]; i++;
                    }
                    T->tokens[tidx].text[i] = 0;
                    js_dom_request_rerender();
                }
            }
        }
        /* Update JS property */
        js_var *hv = js_var_get_child(self, "innerHTML");
        if (hv) js_var_set_string(hv, html_str);
        return;
    }

    /* Getter: return innerHTML value */
    js_var *hv = js_var_get_child(self, "innerHTML");
    if (hv) {
        char buf[1024];
        js_var_get_string(hv, buf, sizeof(buf));
        js_var_set_string(js_var_get_return_var(c), buf);
    } else {
        js_var_set_string(js_var_get_return_var(c), "");
    }
}

/* ---- document.getElementById with setText/setStyle methods -------------- */
/* This wraps scGetElementById and attaches native methods to the element. */
static void scGetElementByIdWithMethods(js_var *c, void *userdata) {
    scGetElementById(c, userdata);
    js_var *el = js_var_get_return_var(c);
    if (!el || (el->flags & JS_V_TYPEMASK) == JS_V_UNDEFINED) return;

    /* Only attach methods once (check if setText already exists) */
    if (js_var_get_child(el, "setText")) return;

    /* Attach methods directly as native function vars — no js_add_native,
     * no cloning from root.el. Just create the js_var with the callback
     * and parameter children directly. */
    {
        js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
        m->jsCallback = scSetText;
        js_var_add_child(m, "text", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(el, "setText", m);
    }
    {
        js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
        m->jsCallback = scSetStyle;
        js_var_add_child(m, "prop", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(m, "value", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(el, "setStyle", m);
    }
    {
        js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
        m->jsCallback = scSetAttribute;
        js_var_add_child(m, "name", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(m, "value", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(el, "setAttribute", m);
    }
    {
        js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
        m->jsCallback = scGetAttribute;
        js_var_add_child(m, "name", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(el, "getAttribute", m);
    }
    {
        js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
        m->jsCallback = scAppendChild;
        js_var_add_child(m, "child", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(el, "appendChild", m);
    }
    {
        js_var *m = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
        m->jsCallback = scAddEventListener;
        js_var_add_child(m, "type", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(m, "fn", js_var_new_blank(JS_V_UNDEFINED));
        js_var_add_child(el, "addEventListener", m);
    }
}

/* ---- engine lifecycle -------------------------------------------------- */
void js_init(void) {
    if (g_js_engine) return;
    g_js_engine = js_create();
    registerFunctions(g_js_engine);
    registerMathFunctions(g_js_engine);

    /* Core builtins */
    js_add_native(g_js_engine, "function print(jsCode)", scPrint, 0);
    js_add_native(g_js_engine, "function console.log(jsCode)", scConsoleLog, 0);

    /* document methods */
    js_add_native(g_js_engine, "function document.getElementById(id)",
                   scGetElementByIdWithMethods, 0);
    js_add_native(g_js_engine, "function document.querySelector(selector)",
                   scQuerySelector, 0);
    js_add_native(g_js_engine, "function document.querySelectorAll(selector)",
                   scQuerySelectorAll, 0);
    js_add_native(g_js_engine, "function document.createElement(tag)",
                   scCreateElement, 0);

    /* document.body — a special getter using the native callback pattern */
    /* We'll handle this via getElementById("body") with a pre-memoized element */
    js_dom_get_element("body");
}

/* Run a script; surface any error to the serial console. */
void js_run(const char *code) {
    if (!g_js_engine) js_init();
    js_execute(g_js_engine, code);
    if (js_has_error(g_js_engine)) {
        serial_printf("[js] ERROR: %s\n", g_js_engine->error);
    }
}

/* Extract <script>...</script> blocks from raw HTML and execute them. */
void js_dom_run_page(const char *html, int html_len) {
    if (html_len <= 0 || !html) return;
    const char *p = html;
    const char *end = html + html_len;
    while (p < end) {
        /* find "<script" (case-insensitive), then skip to '>' */
        if (p + 6 <= end &&
            p[0] == '<' &&
            ((p[1] == 's' || p[1] == 'S') &&
             (p[2] == 'c' || p[2] == 'C') &&
             (p[3] == 'r' || p[3] == 'R') &&
             (p[4] == 'i' || p[4] == 'I') &&
             (p[5] == 'p' || p[5] == 'P') &&
             (p[6] == 't' || p[6] == 'T'))) {
            const char *tag = p + 7;
            while (tag < end && *tag != '>') tag++;
            if (tag >= end) break;

            /* Check for src= attribute — skip external scripts (handled by sub-res queue) */
            int has_src = 0;
            {
                const char *a = p + 7;
                while (a < tag) {
                    if ((a[0] == 's' || a[0] == 'S') && (a[1] == 'r' || a[1] == 'R') &&
                        (a[2] == 'c' || a[2] == 'C') && a[3] == '=') {
                        has_src = 1; break;
                    }
                    a++;
                }
            }
            if (has_src) {
                p = tag + 1;
                continue;
            }

            const char *body = tag + 1;
            /* find "</script>" (case-insensitive) */
            const char *close = body;
            while (close + 8 <= end) {
                if (close[0] == '<' && close[1] == '/' &&
                    (close[2] == 's' || close[2] == 'S') &&
                    (close[3] == 'c' || close[3] == 'C') &&
                    (close[4] == 'r' || close[4] == 'R') &&
                    (close[5] == 'i' || close[5] == 'I') &&
                    (close[6] == 'p' || close[6] == 'P') &&
                    (close[7] == 't' || close[7] == 'T')) break;
                close++;
            }
            int body_len = (int)(close - body);
            if (body_len > 0) {
                char *script = (char *)js_malloc(body_len + 1);
                int i;
                for (i = 0; i < body_len; i++) script[i] = body[i];
                script[body_len] = 0;
                js_run(script);
                js_free(script);
            }
            p = close + 8;
        } else {
            p++;
        }
    }
}
