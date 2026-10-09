// DOM natives for the script realm: the `W` object the prelude builds the
// Web API on. Nodes are wdom indices wrapped in one JS object each (cached
// in js->node_obj, so identity is stable: el === el.parentNode.firstChild).
// Every mutation reports its side effects (wjs_inserted & co) so scripts
// run, stylesheets/images are picked up and the page re-renders.

#include "wjs_int.h"
#include "css.h"
#include "wurl.h"

int wjs_class_id;

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)
#define ARG_NODE(i, var)                                                          \
    int var = argc > (i) ? wjs_node_of(js, argv[i]) : -1;                         \
    if (var < 0) return ojs_throw_type_error(ctx, "parameter %d is not a Node", (i) + 1)
#define UNUSED_THIS (void)this_val

// ---- wrappers -----------------------------------------------------------------

// grow an array of values that is a GC root range from `cap` to `nc` slots (new ones
// undefined). Nothing allocates GC memory in between, so unregistering first is safe.
ojsv* wjs_grow_roots(struct wjs* js, ojsv** arr, int cap, int nc) {
    if (js->J && *arr) ojs_remove_root_range(js->J, *arr);
    ojsv* v = (ojsv*)w_realloc(*arr, nc * sizeof(ojsv));
    if (!v) {
        if (js->J && *arr) ojs_add_root_range(js->J, *arr, cap);
        return 0;
    }
    for (int i = cap; i < nc; i++) v[i] = OJS_UNDEFINED;
    *arr = v;
    if (js->J) ojs_add_root_range(js->J, v, nc);
    return v;
}

int wjs_grow_nodes(struct wjs* js) {
    int need = js->d->nn;
    if (need <= js->node_cap) return 1;
    int nc = need + 1024;
    if (!wjs_grow_roots(js, &js->node_obj, js->node_cap, nc)) return 0;
    uint8_t* f = (uint8_t*)w_realloc(js->node_flags, nc);
    if (!f) return 0;
    memset(f + js->node_cap, 0, nc - js->node_cap);
    js->node_flags = f;
    js->node_cap = nc;
    return 1;
}

static int node_type(const struct wdom* d, int n) {
    switch (d->n[n].type) {
    case WN_ELEM: return 1;
    case WN_TEXT: return 3;
    case WN_COMMENT: return 8;
    case WN_FRAG: return 11;
    default: return n == 0 ? 9 : 11;
    }
}

static ojsv proto_call(struct wjs* js, const char* name, int ns, int type) {
    ojsv a[3] = { ojs_string(js->J, name), ojs_int(ns), ojs_int(type) };
    ojsv r = ojs_call(js->J, js->h_proto, OJS_UNDEFINED, 3, a);
    if (ojs_is_exception(r)) { ojs_take_exception(js->J); return OJS_NULL; }
    return r;
}

static ojsv proto_for(struct wjs* js, int node) {
    struct wdom* d = js->d;
    int type = node_type(d, node);
    ojsv* slot = 0;
    char name[64];
    name[0] = 0;
    if (type == 1) {
        int nl;
        const char* nm = watom_name(&d->atoms, d->n[node].tag, &nl);
        if (nl > 63) nl = 63;
        memcpy(name, nm, nl);
        name[nl] = 0;
        int ns = d->n[node].ns;
        if (ns == NS_SVG) slot = wjs_str_eq(name, nl, "svg") ? &js->proto_svgroot : &js->proto_svg;
        else if (ns == NS_MATH) slot = &js->proto_math;
        else {
            int custom = 0;
            for (int i = 0; i < nl; i++) if (name[i] == '-') custom = 1;
            if (custom) return proto_call(js, name, 0, 1);
            int a = d->n[node].tag;
            if (a >= js->proto_cap) {
                int nc = a + 256;
                if (!wjs_grow_roots(js, &js->proto_html, js->proto_cap, nc)) return proto_call(js, name, 0, 1);
                js->proto_cap = nc;
            }
            slot = &js->proto_html[a];
        }
        if (ojs_is_undefined(*slot)) *slot = proto_call(js, name, ns, 1);
        return (*slot);
    }
    slot = type == 3 ? &js->proto_text : type == 8 ? &js->proto_comment : type == 9 ? &js->proto_doc : &js->proto_frag;
    if (ojs_is_undefined(*slot)) *slot = proto_call(js, "", 0, type);
    return (*slot);
}

ojsv wjs_wrap(struct wjs* js, int node) {
    if (node < 0 || node >= js->d->nn) return OJS_NULL;
    if (node >= js->node_cap && !wjs_grow_nodes(js)) return OJS_NULL;
    if (!ojs_is_undefined(js->node_obj[node])) return (js->node_obj[node]);
    ojsv proto = proto_for(js, node);
    ojsv o = ojs_host_object(js->J, wjs_class_id, ojs_is_object(proto) ? proto : OJS_UNDEFINED, (void*)(intptr_t)(node + 1));
    if (ojs_is_exception(o)) return o;
    js->node_obj[node] = o;
    return o;
}

int wjs_node_of(struct wjs* js, ojsv v) {
    void* p = ojs_host_opaque(js->J, v, wjs_class_id);
    if (!p) return -1;
    int n = (int)(intptr_t)p - 1;
    return n >= 0 && n < js->d->nn ? n : -1;
}

static ojsv str_or_null(ojs* ctx, const char* s, int len) {
    return s ? ojs_string_len(ctx, s, len) : OJS_NULL;
}

// ---- tree navigation ------------------------------------------------------------

NATIVE(n_ntype) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return ojs_int(node_type(js->d, n)); }
NATIVE(n_ns) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return ojs_int(js->d->n[n].ns); }
NATIVE(n_name) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    if (js->d->n[n].type != WN_ELEM) return ojs_string(ctx, "");
    int l;
    const char* s = watom_name(&js->d->atoms, js->d->n[n].tag, &l);
    return ojs_string_len(ctx, s, l);
}
NATIVE(n_parent) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    return wjs_wrap(js, js->d->n[n].parent);
}
NATIVE(n_first) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return wjs_wrap(js, js->d->n[n].first); }
NATIVE(n_last) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return wjs_wrap(js, js->d->n[n].last); }
NATIVE(n_next) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return wjs_wrap(js, js->d->n[n].next); }
NATIVE(n_prev) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return wjs_wrap(js, js->d->n[n].prev); }
NATIVE(n_children) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    ojsv a = ojs_array(ctx);
    uint32_t i = 0;
    for (int c = js->d->n[n].first; c >= 0; c = js->d->n[c].next)
        ojs_set_index(ctx, a, i++, wjs_wrap(js, c));
    return a;
}
NATIVE(n_connected) { struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n); return ojs_bool(wdom_is_connected(js->d, n)); }
NATIVE(n_doc) { struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv; return wjs_wrap(js, 0); }
NATIVE(n_root) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    for (int c = js->d->n[0].first; c >= 0; c = js->d->n[c].next)
        if (js->d->n[c].type == WN_ELEM) return wjs_wrap(js, c);
    return OJS_NULL;
}

// ---- character data / text ------------------------------------------------------

NATIVE(n_data) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    const struct wnode* x = &js->d->n[n];
    if (x->type != WN_TEXT && x->type != WN_COMMENT) return ojs_string(ctx, "");
    return ojs_string_len(ctx, js->d->text + x->text, x->tlen);
}
NATIVE(n_set_data) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    wdom_set_data(js->d, n, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    wjs_text_changed(js, n);
    return OJS_UNDEFINED;
}
NATIVE(n_text) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    struct wbuf b = { 0, 0, 0 };
    for (int c = js->d->n[n].first; c >= 0; c = wdom_next(js->d, c, n))
        if (js->d->n[c].type == WN_TEXT) wbuf_put(&b, js->d->text + js->d->n[c].text, (int)js->d->n[c].tlen);
    ojsv r = ojs_string_len(ctx, b.p ? b.p : "", b.len);
    wbuf_free(&b);
    return r;
}
NATIVE(n_set_text) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    struct wdom* d = js->d;
    while (d->n[n].first >= 0) {
        int c = d->n[n].first;
        wdom_remove(d, c);
        wjs_removed(js, n, c);
    }
    if (l) {
        int t = wdom_create_text(d, s, (int)l);
        if (t >= 0) {
            wdom_append(d, n, t);
            wjs_inserted(js, t);
        }
    }
    ojs_free_cstring(ctx, (char*)s);
    wjs_text_changed(js, n);
    return OJS_UNDEFINED;
}

// ---- attributes -----------------------------------------------------------------

NATIVE(n_get_attr) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    size_t l;
    const char* name = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &l);
    if (!name) return OJS_EXCEPTION;
    ojsv r = OJS_NULL;
    int a = watom_find(&js->d->atoms, name, (int)l);
    if (a) {
        int vl;
        const char* v = wdom_attr(js->d, n, a, &vl);
        r = str_or_null(ctx, v, vl);
    }
    ojs_free_cstring(ctx, (char*)name);
    return r;
}
NATIVE(n_set_attr) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    if (js->d->n[n].type != WN_ELEM) return OJS_UNDEFINED;
    size_t nl, vl;
    const char* name = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &nl);
    if (!name) return OJS_EXCEPTION;
    const char* val = ojs_to_cstring(ctx, argc > 2 ? argv[2] : OJS_UNDEFINED, &vl);
    if (!val) { ojs_free_cstring(ctx, (char*)name); return OJS_EXCEPTION; }
    int a = nl ? watom_intern(&js->d->atoms, name, (int)nl) : 0;
    if (a) {
        wdom_set_attr(js->d, n, a, val, (int)vl);
        wjs_attr_changed(js, n, a);
    }
    ojs_free_cstring(ctx, (char*)name);
    ojs_free_cstring(ctx, (char*)val);
    return OJS_UNDEFINED;
}
NATIVE(n_remove_attr) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    size_t nl;
    const char* name = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &nl);
    if (!name) return OJS_EXCEPTION;
    int a = watom_find(&js->d->atoms, name, (int)nl);
    if (a && wdom_has_attr(js->d, n, a)) {
        wdom_remove_attr(js->d, n, a);
        wjs_attr_changed(js, n, a);
    }
    ojs_free_cstring(ctx, (char*)name);
    return OJS_UNDEFINED;
}
NATIVE(n_attr_names) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    ojsv arr = ojs_array(ctx);
    uint32_t i = 0;
    if (js->d->n[n].type == WN_ELEM)
        for (int a = js->d->n[n].attr; a >= 0; a = js->d->a[a].next) {
            int l;
            const char* s = watom_name(&js->d->atoms, js->d->a[a].name, &l);
            ojs_set_index(ctx, arr, i++, ojs_string_len(ctx, s, l));
        }
    return arr;
}

// ---- tree mutation --------------------------------------------------------------

static int can_have_children(const struct wdom* d, int n) {
    int t = d->n[n].type;
    return t == WN_ELEM || t == WN_FRAG || t == WN_DOC;
}

static void mo_childlist(struct wjs* js, int parent, int added, int removed) {
    if (!js->mo_active) return;
    ojs* ctx = js->J;
    ojsv a = ojs_array(ctx), r = ojs_array(ctx);
    if (added >= 0) ojs_set_index(ctx, a, 0, wjs_wrap(js, added));
    if (removed >= 0) ojs_set_index(ctx, r, 0, wjs_wrap(js, removed));
    ojsv args[3] = { wjs_wrap(js, parent), a, r };
    ojsv res = ojs_call(ctx, js->h_mo, OJS_UNDEFINED, 3, args);
    if (ojs_is_exception(res)) wjs_report_exception(js, "MutationObserver");
}

NATIVE(n_insert) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    ARG_NODE(0, p);
    ARG_NODE(1, c);
    int ref = argc > 2 ? wjs_node_of(js, argv[2]) : -1;
    struct wdom* d = js->d;
    if (!can_have_children(d, p)) return ojs_throw_type_error(ctx, "HierarchyRequestError: parent cannot have children");
    if (c == 0) return ojs_throw_type_error(ctx, "HierarchyRequestError: cannot insert the document");
    for (int a = p; a >= 0; a = d->n[a].parent)
        if (a == c) return ojs_throw_type_error(ctx, "HierarchyRequestError: the new child contains the parent");
    if (ref >= 0 && d->n[ref].parent != p) ref = -1;
    if (d->n[c].type == WN_FRAG) {
        while (d->n[c].first >= 0) {
            int k = d->n[c].first;
            wdom_insert_before(d, p, k, ref);
            wjs_inserted(js, k);
            mo_childlist(js, p, k, -1);
        }
    } else {
        int old_parent = d->n[c].parent;
        if (old_parent >= 0) {
            wdom_remove(d, c);
            wjs_removed(js, old_parent, c);
            mo_childlist(js, old_parent, -1, c);
        }
        wdom_insert_before(d, p, c, ref);
        d->n[c].flags &= ~WNF_REMOVED;
        wjs_inserted(js, c);
        mo_childlist(js, p, c, -1);
    }
    return (argv[1]);
}

NATIVE(n_remove) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, c);
    int p = js->d->n[c].parent;
    if (p >= 0) {
        wdom_remove(js->d, c);
        wjs_removed(js, p, c);
        mo_childlist(js, p, -1, c);
    }
    return OJS_UNDEFINED;
}

NATIVE(n_create) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* name = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!name) return OJS_EXCEPTION;
    int32_t ns = 0;
    if (argc > 1) ojs_to_int32(ctx, &ns, argv[1]);
    int a = l ? watom_intern(&js->d->atoms, name, (int)l) : 0;
    ojs_free_cstring(ctx, (char*)name);
    if (!a) return ojs_throw_type_error(ctx, "too many names");
    int el = wdom_create_element(js->d, ns == 1 ? NS_SVG : ns == 2 ? NS_MATH : NS_HTML, a);
    if (el < 0) return ojs_throw_oom(ctx);
    return wjs_wrap(js, el);
}
NATIVE(n_create_text) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    int t = wdom_create_text(js->d, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    if (t < 0) return ojs_throw_oom(ctx);
    return wjs_wrap(js, t);
}
NATIVE(n_create_comment) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    int t = wdom_create_comment(js->d, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    if (t < 0) return ojs_throw_oom(ctx);
    return wjs_wrap(js, t);
}
NATIVE(n_create_fragment) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    int f = wdom_create_fragment(js->d);
    if (f < 0) return ojs_throw_oom(ctx);
    return wjs_wrap(js, f);
}

static void copy_started_flags(struct wjs* js, int src, int dst) {
    // cloned scripts keep "already started" (spec); walk both trees in step
    struct wdom* d = js->d;
    if (!wjs_grow_nodes(js)) return;
    int a = src, b = dst;
    while (a >= 0 && b >= 0) {
        if (d->n[a].type == WN_ELEM && wdom_is(d, a, T_script)) js->node_flags[b] |= js->node_flags[a] & NF_STARTED;
        a = wdom_next(d, a, src);
        b = wdom_next(d, b, dst);
    }
}

NATIVE(n_clone) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    int deep = argc > 1 && ojs_to_bool(ctx, argv[1]);
    int c;
    if (js->d->n[n].type == WN_FRAG || n == 0) {
        c = wdom_create_fragment(js->d);
        if (c >= 0 && deep)
            for (int k = js->d->n[n].first; k >= 0; k = js->d->n[k].next) {
                int kc = wdom_clone(js->d, k, 1);
                if (kc >= 0) wdom_append(js->d, c, kc);
            }
    } else c = wdom_clone(js->d, n, deep);
    if (c < 0) return ojs_throw_oom(ctx);
    copy_started_flags(js, n, c);
    return wjs_wrap(js, c);
}

// ---- HTML -----------------------------------------------------------------------

NATIVE(n_html) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    int outer = argc > 1 && ojs_to_bool(ctx, argv[1]);
    struct wbuf b = { 0, 0, 0 };
    wdom_serialize(js->d, n, outer, &b);
    ojsv r = ojs_string_len(ctx, b.p ? b.p : "", b.len);
    wbuf_free(&b);
    return r;
}

static void mark_scripts_started(struct wjs* js, int root) {
    struct wdom* d = js->d;
    if (!wjs_grow_nodes(js)) return;
    for (int c = root; c >= 0; c = wdom_next(d, c, root))
        if (wdom_is(d, c, T_script)) js->node_flags[c] |= NF_STARTED;
}

static int ctx_tag_of(struct wjs* js, int n) {
    struct wdom* d = js->d;
    if (n >= 0 && d->n[n].type == WN_ELEM && d->n[n].ns == NS_HTML) return d->n[n].tag;
    return T_body;
}

NATIVE(n_set_html) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    struct wdom* d = js->d;
    while (d->n[n].first >= 0) {
        int c = d->n[n].first;
        wdom_remove(d, c);
        wjs_removed(js, n, c);
    }
    int f = wdom_create_fragment(d);
    if (f >= 0) {
        whtml_parse_fragment_ctx(d, f, ctx_tag_of(js, n), s, (int)l);
        mark_scripts_started(js, f);
        while (d->n[f].first >= 0) {
            int k = d->n[f].first;
            wdom_append(d, n, k);
            wjs_inserted(js, k);
        }
    }
    ojs_free_cstring(ctx, (char*)s);
    wjs_text_changed(js, n);
    return OJS_UNDEFINED;
}

NATIVE(n_parse_html) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    int ctxn = argc > 1 ? wjs_node_of(js, argv[1]) : -1;
    int f = wdom_create_fragment(js->d);
    if (f >= 0) {
        whtml_parse_fragment_ctx(js->d, f, ctx_tag_of(js, ctxn), s, (int)l);
        mark_scripts_started(js, f);
    }
    ojs_free_cstring(ctx, (char*)s);
    if (f < 0) return ojs_throw_oom(ctx);
    return wjs_wrap(js, f);
}

NATIVE(n_template_content) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, t);
    for (int i = 0; i < js->ntpl; i++)
        if (js->tpl_map[i * 2] == t) return wjs_wrap(js, js->tpl_map[i * 2 + 1]);
    int f = wdom_create_fragment(js->d);
    if (f < 0) return ojs_throw_oom(ctx);
    // the parser keeps template contents as children: move them over
    while (js->d->n[t].first >= 0) wdom_append(js->d, f, js->d->n[t].first);
    if (js->ntpl >= js->captpl) {
        int nc = js->captpl ? js->captpl * 2 : 16;
        int* m = (int*)w_realloc(js->tpl_map, nc * 2 * sizeof(int));
        if (!m) return wjs_wrap(js, f);
        js->tpl_map = m;
        js->captpl = nc;
    }
    js->tpl_map[js->ntpl * 2] = t;
    js->tpl_map[js->ntpl * 2 + 1] = f;
    js->ntpl++;
    return wjs_wrap(js, f);
}

// ---- selectors ------------------------------------------------------------------

#define SELQ_CACHE 24
struct selq_ent { char* text; int len; struct wselq* q; uint32_t used; };
static struct selq_ent selq_cache[SELQ_CACHE];
static struct wdom* selq_dom;      // cache belongs to one document at a time
static uint32_t selq_clock;

static void selq_flush(void) {
    for (int i = 0; i < SELQ_CACHE; i++) {
        w_free(selq_cache[i].text);
        css_selq_free(selq_cache[i].q);
        memset(&selq_cache[i], 0, sizeof selq_cache[i]);
    }
}

void wjs_selq_forget(struct wdom* d) { if (selq_dom == d) { selq_flush(); selq_dom = 0; } }

static struct wselq* selq_get(struct wdom* d, const char* s, int len) {
    if (selq_dom != d) { selq_flush(); selq_dom = d; }
    int lru = 0;
    for (int i = 0; i < SELQ_CACHE; i++) {
        if (selq_cache[i].q && selq_cache[i].len == len && !memcmp(selq_cache[i].text, s, len)) {
            selq_cache[i].used = ++selq_clock;
            return selq_cache[i].q;
        }
        if (selq_cache[i].used < selq_cache[lru].used) lru = i;
    }
    struct wselq* q = css_selq_compile(d, s, len);
    if (!q) return 0;
    char* t = (char*)w_malloc(len + 1);
    if (!t) { css_selq_free(q); return 0; }
    memcpy(t, s, len);
    w_free(selq_cache[lru].text);
    css_selq_free(selq_cache[lru].q);
    selq_cache[lru].text = t;
    selq_cache[lru].len = len;
    selq_cache[lru].q = q;
    selq_cache[lru].used = ++selq_clock;
    return q;
}

// ":scope" -> a temporary marker attribute on the root
static int scope_rewrite(const char* s, int len, char* out, int cap) {
    static const char rep[] = "[data-okai-scope]";
    int o = 0, found = 0;
    for (int i = 0; i < len; i++) {
        if (i + 6 <= len && !memcmp(s + i, ":scope", 6)) {
            if (o + (int)sizeof rep >= cap) return -1;
            memcpy(out + o, rep, sizeof rep - 1);
            o += sizeof rep - 1;
            i += 5;
            found = 1;
            continue;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = s[i];
    }
    out[o] = 0;
    return found ? o : 0;
}

static ojsv do_query(struct wjs* js, ojs* ctx, int root, ojsv selv, int all, int match_only) {
    size_t l;
    const char* s = ojs_to_cstring(ctx, selv, &l);
    if (!s) return OJS_EXCEPTION;
    struct wdom* d = js->d;
    // leading combinator ("> a") is relative to the root
    const char* sel = s;
    int sl = (int)l;
    char* tmp = 0;
    int scope_atom = 0;
    int k = 0;
    while (k < sl && w_isspace((unsigned char)sel[k])) k++;
    if (k < sl && (sel[k] == '>' || sel[k] == '+' || sel[k] == '~') && !match_only) {
        tmp = (char*)w_malloc(sl + 32);
        if (tmp) {
            memcpy(tmp, ":scope ", 7);
            memcpy(tmp + 7, sel + k, sl - k);
            sel = tmp;
            sl = 7 + sl - k;
        }
    }
    char* rew = 0;
    for (int i = 0; i + 6 <= sl; i++)
        if (!memcmp(sel + i, ":scope", 6)) {
            rew = (char*)w_malloc(sl * 3 + 64);
            if (rew) {
                int rl = scope_rewrite(sel, sl, rew, sl * 3 + 64);
                if (rl > 0) { sel = rew; sl = rl; scope_atom = watom_intern(&d->atoms, "data-okai-scope", 15); }
            }
            break;
        }
    struct wselq* q = selq_get(d, sel, sl);
    ojsv r;
    if (!q) {
        char shown[204];
        int k2 = 0;
        while (s[k2] && k2 < 200) { shown[k2] = s[k2]; k2++; }
        shown[k2] = 0;
        r = ojs_throw_syntax_error(ctx, "'%s' is not a valid selector", shown);
    } else {
        int scoped_el = scope_atom && root >= 0 && d->n[root].type == WN_ELEM;
        if (scoped_el) wdom_set_attr(d, root, scope_atom, "", 0);
        if (match_only) {
            r = ojs_bool(css_selq_match(q, d, root));
        } else if (all) {
            r = ojs_array(ctx);
            uint32_t i = 0;
            for (int c = d->n[root].first; c >= 0; c = wdom_next(d, c, root))
                if (d->n[c].type == WN_ELEM && css_selq_match(q, d, c))
                    ojs_set_index(ctx, r, i++, wjs_wrap(js, c));
        } else {
            r = OJS_NULL;
            for (int c = d->n[root].first; c >= 0; c = wdom_next(d, c, root))
                if (d->n[c].type == WN_ELEM && css_selq_match(q, d, c)) { r = wjs_wrap(js, c); break; }
        }
        if (scoped_el) wdom_remove_attr(d, root, scope_atom);
    }
    w_free(tmp);
    w_free(rew);
    ojs_free_cstring(ctx, (char*)s);
    return r;
}

NATIVE(n_query) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, root);
    return do_query(js, ctx, root, argc > 1 ? argv[1] : OJS_UNDEFINED, argc > 2 && ojs_to_bool(ctx, argv[2]), 0);
}
NATIVE(n_matches) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, el);
    if (js->d->n[el].type != WN_ELEM) return OJS_FALSE;
    return do_query(js, ctx, el, argc > 1 ? argv[1] : OJS_UNDEFINED, 0, 1);
}
NATIVE(n_by_id) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, 0);
    if (!s) return OJS_EXCEPTION;
    int n = s[0] ? wdom_find_id(js->d, s) : -1;
    ojs_free_cstring(ctx, (char*)s);
    return wjs_wrap(js, n);
}
NATIVE(n_by_tag) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, root);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 1 ? argv[1] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    int a = watom_find(&js->d->atoms, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    ojsv r = ojs_array(ctx);
    if (!a) return r;
    uint32_t i = 0;
    for (int c = js->d->n[root].first; c >= 0; c = wdom_next(js->d, c, root))
        if (js->d->n[c].type == WN_ELEM && js->d->n[c].tag == a) ojs_set_index(ctx, r, i++, wjs_wrap(js, c));
    return r;
}

// ---- layout / style ---------------------------------------------------------------

NATIVE(n_rect) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    int x, y, w, h;
    if (js->d->n[n].type != WN_ELEM || !wdom_is_connected(js->d, n) ||
        !wdoc_layout_rect(js->doc, n, &x, &y, &w, &h))
        return OJS_NULL;
    ojsv a = ojs_array(ctx);
    ojs_set_index(ctx, a, 0, ojs_int(x));
    ojs_set_index(ctx, a, 1, ojs_int(y));
    ojs_set_index(ctx, a, 2, ojs_int(w));
    ojs_set_index(ctx, a, 3, ojs_int(h));
    return a;
}
NATIVE(n_view) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    int vw, vh, sy, dw, dh;
    wdoc_viewport_get(js->doc, &vw, &vh, &sy, &dw, &dh);
    if (js->scroll_req >= 0) sy = js->scroll_req;
    ojsv a = ojs_array(ctx);
    int v[5] = { vw, vh, sy, dw, dh };
    for (int i = 0; i < 5; i++) ojs_set_index(ctx, a, i, ojs_int(v[i]));
    return a;
}
NATIVE(n_scroll) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    int32_t y = 0;
    if (argc > 0) ojs_to_int32(ctx, &y, argv[0]);
    js->scroll_req = y < 0 ? 0 : y;
    js->flags |= WJS_SCROLL;
    return OJS_UNDEFINED;
}
NATIVE(n_hit) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    int32_t x = 0, y = 0;
    if (argc > 1) { ojs_to_int32(ctx, &x, argv[0]); ojs_to_int32(ctx, &y, argv[1]); }
    return wjs_wrap(js, wdoc_hit_element(js->doc, x, y));
}

static void put_len(ojs* ctx, ojsv o, const char* k, struct wlen l, int32_t resolved_lu, int have) {
    char b[32];
    int n = 0;
    if (have) {
        int px = LU_ROUND(resolved_lu);
        if (px < 0) { b[n++] = '-'; px = -px; }
        char t[12]; int tl = 0;
        do { t[tl++] = (char)('0' + px % 10); px /= 10; } while (px && tl < 11);
        while (tl) b[n++] = t[--tl];
        b[n++] = 'p'; b[n++] = 'x';
    } else if (l.t == WL_AUTO) { memcpy(b, "auto", 4); n = 4; }
    else if (l.t == WL_NONE) { memcpy(b, "none", 4); n = 4; }
    else { memcpy(b, "0px", 3); n = 3; }
    ojs_set(ctx, o, k, ojs_string_len(ctx, b, n));
}

static void put_px(ojs* ctx, ojsv o, const char* k, int32_t lu) {
    struct wlen l = { lu, 0, WL_LEN };
    put_len(ctx, o, k, l, lu, 1);
}

static void put_color(ojs* ctx, ojsv o, const char* k, uint32_t argb) {
    char b[48];
    int a = (int)(argb >> 24), r = (int)(argb >> 16) & 255, g = (int)(argb >> 8) & 255, bl = (int)argb & 255;
    int n = 0;
    const char* pre = a == 255 ? "rgb(" : "rgba(";
    for (const char* p = pre; *p; p++) b[n++] = *p;
    int vals[3] = { r, g, bl };
    for (int i = 0; i < 3; i++) {
        int v = vals[i];
        if (v >= 100) b[n++] = (char)('0' + v / 100);
        if (v >= 10) b[n++] = (char)('0' + v / 10 % 10);
        b[n++] = (char)('0' + v % 10);
        if (i < 2) { b[n++] = ','; b[n++] = ' '; }
    }
    if (a != 255) {
        // alpha with two decimals
        int c = a * 100 / 255;
        b[n++] = ','; b[n++] = ' '; b[n++] = '0'; b[n++] = '.';
        b[n++] = (char)('0' + c / 10); b[n++] = (char)('0' + c % 10);
    }
    b[n++] = ')';
    ojs_set(ctx, o, k, ojs_string_len(ctx, b, n));
}

static const char* const DISPLAY_NAMES[] = { "none", "inline", "block", "inline-block", "list-item", "flex",
    "inline-flex", "grid", "inline-grid", "table", "inline-table", "table-row-group", "table-header-group",
    "table-footer-group", "table-row", "table-cell", "table-column", "table-column-group", "table-caption",
    "contents", "flow-root" };
static const char* const POS_NAMES[] = { "static", "relative", "absolute", "fixed", "sticky" };
static const char* const OV_NAMES[] = { "visible", "hidden", "scroll", "auto", "clip" };

NATIVE(n_cstyle) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    ojsv o = ojs_object(ctx);
    const struct wstyle* s = js->d->n[n].type == WN_ELEM && wdom_is_connected(js->d, n) ? wdoc_style_of(js->doc, n) : 0;
    if (!s) {
        ojs_set(ctx, o, "display", ojs_string(ctx, "none"));
        return o;
    }
    ojs_set(ctx, o, "display", ojs_string(ctx, s->display < 21 ? DISPLAY_NAMES[s->display] : "block"));
    ojs_set(ctx, o, "position", ojs_string(ctx, s->position < 5 ? POS_NAMES[s->position] : "static"));
    ojs_set(ctx, o, "visibility", ojs_string(ctx, s->visibility ? "hidden" : "visible"));
    ojs_set(ctx, o, "float", ojs_string(ctx, s->float_ == FL_LEFT ? "left" : s->float_ == FL_RIGHT ? "right" : "none"));
    ojs_set(ctx, o, "overflow", ojs_string(ctx, s->overflow_x < 5 ? OV_NAMES[s->overflow_x] : "visible"));
    ojs_set(ctx, o, "overflow-x", ojs_string(ctx, s->overflow_x < 5 ? OV_NAMES[s->overflow_x] : "visible"));
    ojs_set(ctx, o, "overflow-y", ojs_string(ctx, s->overflow_y < 5 ? OV_NAMES[s->overflow_y] : "visible"));
    ojs_set(ctx, o, "box-sizing", ojs_string(ctx, s->box_sizing == BX_BORDER ? "border-box" : "content-box"));
    put_color(ctx, o, "color", s->color);
    put_color(ctx, o, "background-color", s->bg_color);
    {
        char b[8];
        int op = s->opacity * 100 / 255;
        int k = 0;
        if (op >= 100) { b[k++] = '1'; }
        else { b[k++] = '0'; b[k++] = '.'; b[k++] = (char)('0' + op / 10); if (op % 10) b[k++] = (char)('0' + op % 10); }
        ojs_set(ctx, o, "opacity", ojs_string_len(ctx, b, k));
    }
    put_px(ctx, o, "font-size", s->font_size);
    {
        char b[8];
        int fw = s->font_weight, k = 0;
        b[k++] = (char)('0' + fw / 100); b[k++] = '0'; b[k++] = '0';
        ojs_set(ctx, o, "font-weight", ojs_string_len(ctx, b, k));
    }
    ojs_set(ctx, o, "font-style", ojs_string(ctx, s->font_style ? "italic" : "normal"));
    ojs_set(ctx, o, "font-family", ojs_string(ctx, s->font_family == FAM_SERIF ? "serif" : s->font_family == FAM_MONO ? "monospace" : "sans-serif"));
    if (s->line_height) put_px(ctx, o, "line-height", s->line_height);
    else ojs_set(ctx, o, "line-height", ojs_string(ctx, "normal"));
    static const char* const side[4] = { "top", "right", "bottom", "left" };
    char k[32];
    for (int i = 0; i < 4; i++) {
        int sl = (int)strlen(side[i]);
        memcpy(k, "margin-", 7); memcpy(k + 7, side[i], sl + 1);
        put_len(ctx, o, k, s->margin[i], s->margin[i].px, s->margin[i].t == WL_LEN && !s->margin[i].pct);
        memcpy(k, "padding-", 8); memcpy(k + 8, side[i], sl + 1);
        put_len(ctx, o, k, s->padding[i], s->padding[i].px, s->padding[i].t == WL_LEN && !s->padding[i].pct);
        memcpy(k, "border-", 7); memcpy(k + 7, side[i], sl); memcpy(k + 7 + sl, "-width", 7);
        put_px(ctx, o, k, s->bw[i]);
        put_len(ctx, o, side[i], s->inset[i], s->inset[i].px, s->inset[i].t == WL_LEN && !s->inset[i].pct);
    }
    int x, y, w, h;
    if (wdoc_layout_rect(js->doc, n, &x, &y, &w, &h)) {
        put_px(ctx, o, "width", PX(w));
        put_px(ctx, o, "height", PX(h));
    } else {
        put_len(ctx, o, "width", s->width, s->width.px, 0);
        put_len(ctx, o, "height", s->height, s->height.px, 0);
    }
    {
        char b[16];
        int z = s->z_index, kk = 0;
        if (s->z_auto) { memcpy(b, "auto", 4); kk = 4; }
        else {
            if (z < 0) { b[kk++] = '-'; z = -z; }
            char t[12]; int tl = 0;
            do { t[tl++] = (char)('0' + z % 10); z /= 10; } while (z && tl < 11);
            while (tl) b[kk++] = t[--tl];
        }
        ojs_set(ctx, o, "z-index", ojs_string_len(ctx, b, kk));
    }
    static const char* const TA[] = { "start", "left", "right", "center", "justify", "end", "-webkit-center" };
    ojs_set(ctx, o, "text-align", ojs_string(ctx, s->text_align < 7 ? TA[s->text_align] : "start"));
    static const char* const WSN[] = { "normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces" };
    ojs_set(ctx, o, "white-space", ojs_string(ctx, s->white_space < 6 ? WSN[s->white_space] : "normal"));
    ojs_set(ctx, o, "pointer-events", ojs_string(ctx, "auto"));
    ojs_set(ctx, o, "cursor", ojs_string(ctx, "auto"));
    ojs_set(ctx, o, "transform", ojs_string(ctx, "none"));
    ojs_set(ctx, o, "transition-duration", ojs_string(ctx, "0s"));
    ojs_set(ctx, o, "animation-name", ojs_string(ctx, "none"));
    ojs_set(ctx, o, "animation-duration", ojs_string(ctx, "0s"));
    ojs_set(ctx, o, "content", ojs_string(ctx, "normal"));
    ojs_set(ctx, o, "direction", ojs_string(ctx, s->direction_rtl ? "rtl" : "ltr"));
    return o;
}

NATIVE(n_css_var) { (void)ctx; UNUSED_THIS; (void)argc; (void)argv; return ojs_string(ctx, ""); }

NATIVE(n_img_size) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    int w, h, st;
    if (!wdoc_img_info(js->doc, n, &w, &h, &st)) return OJS_NULL;
    ojsv a = ojs_array(ctx);
    ojs_set_index(ctx, a, 0, ojs_int(w));
    ojs_set_index(ctx, a, 1, ojs_int(h));
    ojs_set_index(ctx, a, 2, ojs_int(st));
    return a;
}

// ---- focus / forms ------------------------------------------------------------------

NATIVE(n_focus) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    int on = argc > 1 && ojs_to_bool(ctx, argv[1]);
    if (on) { js->focus_req = n; js->focus_set = 1; }
    else if (wdoc_focus_node(js->doc) == n) { js->focus_req = -1; js->focus_set = 1; }
    js->flags |= WJS_FOCUS;
    return OJS_UNDEFINED;
}
NATIVE(n_active) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    int f = js->focus_set ? js->focus_req : wdoc_focus_node(js->doc);
    return wjs_wrap(js, f);
}
NATIVE(n_submit) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, f);
    js->submit_form = f;
    js->submit_btn = argc > 1 ? wjs_node_of(js, argv[1]) : -1;
    js->flags |= WJS_NAV;
    return OJS_UNDEFINED;
}
NATIVE(n_parser_inserted) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; ARG_NODE(0, n);
    return ojs_bool(n < js->node_cap && (js->node_flags[n] & NF_PARSER));
}
NATIVE(n_current_script) {
    struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv;
    return wjs_wrap(js, js->current_script);
}
NATIVE(n_write) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    wjs_doc_write(js, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    return OJS_UNDEFINED;
}
NATIVE(n_title) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    struct wdom* d = js->d;
    if (argc < 1) return ojs_string(ctx, d->title);
    size_t l;
    const char* s = ojs_to_cstring(ctx, argv[0], &l);
    if (!s) return OJS_EXCEPTION;
    int t = wdom_first_tag(d, T_title);
    if (t < 0) {
        int head = d->head >= 0 ? d->head : wdom_first_tag(d, T_head);
        t = wdom_create_element(d, NS_HTML, T_title);
        if (t >= 0 && head >= 0) wdom_append(d, head, t);
    }
    if (t >= 0) wdom_set_text_content(d, t, s, (int)l);
    int n = (int)l < (int)sizeof d->title - 1 ? (int)l : (int)sizeof d->title - 1;
    memcpy(d->title, s, n);
    d->title[n] = 0;
    ojs_free_cstring(ctx, (char*)s);
    js->flags |= WJS_TITLE;
    return OJS_UNDEFINED;
}
NATIVE(n_quirks) { struct wjs* js = JSX(ctx); UNUSED_THIS; (void)argc; (void)argv; return ojs_bool(js->d->quirks); }
NATIVE(n_mq) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    int vw, vh, sy, dw, dh;
    wdoc_viewport_get(js->doc, &vw, &vh, &sy, &dw, &dh);
    int r = css_media_eval(s, (int)l, vw, vh, 1);
    ojs_free_cstring(ctx, (char*)s);
    return ojs_bool(r);
}
NATIVE(n_supports) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    size_t l;
    const char* s = ojs_to_cstring(ctx, argc > 0 ? argv[0] : OJS_UNDEFINED, &l);
    if (!s) return OJS_EXCEPTION;
    int r = css_supports(js->d, s, (int)l);
    ojs_free_cstring(ctx, (char*)s);
    return ojs_bool(r);
}
NATIVE(n_set_proto_for) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    if (argc < 1 || !ojs_is_function(ctx, argv[0])) return ojs_throw_type_error(ctx, "function expected");
    js->h_proto = (argv[0]);
    return OJS_UNDEFINED;
}
NATIVE(n_set_mo) {
    struct wjs* js = JSX(ctx); UNUSED_THIS;
    js->mo_active = argc > 0 && ojs_to_bool(ctx, argv[0]);
    return OJS_UNDEFINED;
}

// ---- install ------------------------------------------------------------------------

static const struct ojs_func_entry dom_funcs[] = {
    { "ntype", n_ntype, 1 },
    { "ns", n_ns, 1 },
    { "name", n_name, 1 },
    { "parent", n_parent, 1 },
    { "first", n_first, 1 },
    { "last", n_last, 1 },
    { "next", n_next, 1 },
    { "prev", n_prev, 1 },
    { "children", n_children, 1 },
    { "connected", n_connected, 1 },
    { "doc", n_doc, 0 },
    { "root", n_root, 0 },
    { "data", n_data, 1 },
    { "setData", n_set_data, 2 },
    { "text", n_text, 1 },
    { "setText", n_set_text, 2 },
    { "getAttr", n_get_attr, 2 },
    { "setAttr", n_set_attr, 3 },
    { "removeAttr", n_remove_attr, 2 },
    { "attrNames", n_attr_names, 1 },
    { "insert", n_insert, 3 },
    { "remove", n_remove, 1 },
    { "create", n_create, 2 },
    { "createText", n_create_text, 1 },
    { "createComment", n_create_comment, 1 },
    { "createFragment", n_create_fragment, 0 },
    { "clone", n_clone, 2 },
    { "html", n_html, 2 },
    { "setHTML", n_set_html, 2 },
    { "parseHTML", n_parse_html, 2 },
    { "templateContent", n_template_content, 1 },
    { "query", n_query, 3 },
    { "matches", n_matches, 2 },
    { "byId", n_by_id, 1 },
    { "byTag", n_by_tag, 2 },
    { "rect", n_rect, 1 },
    { "view", n_view, 0 },
    { "scroll", n_scroll, 1 },
    { "hit", n_hit, 2 },
    { "cstyle", n_cstyle, 1 },
    { "cssVar", n_css_var, 2 },
    { "imgSize", n_img_size, 1 },
    { "focus", n_focus, 2 },
    { "activeElement", n_active, 0 },
    { "submit", n_submit, 2 },
    { "parserInserted", n_parser_inserted, 1 },
    { "currentScript", n_current_script, 0 },
    { "write", n_write, 1 },
    { "title", n_title, 1 },
    { "quirks", n_quirks, 0 },
    { "mq", n_mq, 1 },
    { "supports", n_supports, 1 },
    { "setMutationHook", n_set_mo, 1 },
    { "setProtoFor", n_set_proto_for, 1 },
};

void wjs_dom_install(struct wjs* js, ojsv natives) {
    ojs_set_functions(js->J, natives, dom_funcs, (int)(sizeof dom_funcs / sizeof dom_funcs[0]));
}
