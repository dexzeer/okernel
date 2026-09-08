/*
 * okai JS — value type (CScriptVar port).
 *
 * Universal JS value: int | double | string | function | object | array | null,
 * with a doubly-linked child list and reference-counted GC.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js.h"
#include <string.h>

js_var *js_var_deep_copy(js_var *v);

static int js_is_numeric_str(const char *s) {
    if (!s || !*s) return 0;
    while (*s) { if (!(*s >= '0' && *s <= '9')) return 0; s++; }
    return 1;
}

/* ---------------- links ---------------- */

js_varlink *js_link_new(js_var *var, const char *name) {
    js_varlink *l = (js_varlink*)js_malloc(sizeof(js_varlink));
    l->name = js_strdup(name ? name : JS_TEMP_NAME);
    l->next = l->prev = 0;
    l->var = js_var_ref(var);
    l->owned = 0;
    return l;
}

void js_link_free(js_varlink *l) {
    if (!l) return;
    js_var_unref(l->var);
    js_free(l->name);
    js_free(l);
}

void js_link_replace_var(js_varlink *l, js_var *nv) {
    js_var *old = l->var;
    l->var = js_var_ref(nv);
    js_var_unref(old);
}

int js_link_get_int_name(js_varlink *l) {
    return (int)js_strtol(l->name, 0, 0);
}
void js_link_set_int_name(js_varlink *l, int n) {
    char s[64]; js_sprintf(s, sizeof(s), "%d", n);
    js_free(l->name);
    l->name = js_strdup(s);
}

/* ---------------- value lifecycle ---------------- */

static void js_var_init(js_var *v) {
    v->firstChild = v->lastChild = 0;
    v->flags = 0;
    v->jsCallback = 0;
    v->jsCallbackUserData = 0;
    v->data = js_strdup(JS_BLANK_DATA);
    v->intData = 0;
    v->doubleData = 0;
    v->userCustomData = 0;
    v->refs = 0;
}

void js_var_free(js_var *v) {
    js_var_remove_all_children(v);
    js_free(v->data);
    js_free(v);
}

js_var *js_var_ref(js_var *v) { if (v) v->refs++; return v; }

void js_var_unref(js_var *v) {
    if (!v) return;
    if (v->refs <= 0) { js_printf("JS: unref past zero!\n"); return; }
    if (--v->refs == 0) js_var_free(v);
}

js_var *js_var_new_undefined(void) {
    js_var *v = (js_var*)js_malloc(sizeof(js_var));
    js_var_init(v);
    v->flags = JS_V_UNDEFINED;
    return v;
}
js_var *js_var_new_int(long val) {
    js_var *v = (js_var*)js_malloc(sizeof(js_var));
    js_var_init(v);
    js_var_set_int(v, val);
    return v;
}
js_var *js_var_new_double(double val) {
    js_var *v = (js_var*)js_malloc(sizeof(js_var));
    js_var_init(v);
    js_var_set_double(v, val);
    return v;
}
js_var *js_var_new_string(const char *s) {
    js_var *v = (js_var*)js_malloc(sizeof(js_var));
    js_var_init(v);
    js_var_set_string(v, s);
    return v;
}
js_var *js_var_new_null(void) {
    js_var *v = (js_var*)js_malloc(sizeof(js_var));
    js_var_init(v);
    v->flags = JS_V_NULL;
    return v;
}
js_var *js_var_new_blank(int flags) {
    js_var *v = (js_var*)js_malloc(sizeof(js_var));
    js_var_init(v);
    v->flags = flags;
    return v;
}

/* ---------------- children ---------------- */

js_varlink *js_var_add_child(js_var *p, const char *name, js_var *child) {
    if (js_var_is_undefined(p)) p->flags = JS_V_OBJECT;
    if (!child) child = js_var_new_undefined();
    js_varlink *link = js_link_new(child, name);
    link->owned = 1;
    if (p->lastChild) {
        p->lastChild->next = link;
        link->prev = p->lastChild;
        p->lastChild = link;
    } else {
        p->firstChild = p->lastChild = link;
    }
    return link;
}

js_varlink *js_var_add_child_no_dup(js_var *p, const char *name, js_var *child) {
    if (!child) child = js_var_new_undefined();
    js_varlink *v = js_var_find_child(p, name);
    if (v) js_link_replace_var(v, child);
    else     v = js_var_add_child(p, name, child);
    return v;
}

js_varlink *js_var_find_child(js_var *p, const char *name) {
    js_varlink *v = p->firstChild;
    while (v) {
        if (js_strcmp(v->name, name) == 0) return v;
        v = v->next;
    }
    return 0;
}

js_var *js_var_get_child(js_var *p, const char *name) {
    js_varlink *l = js_var_find_child(p, name);
    return l ? l->var : 0;
}

js_varlink *js_var_find_child_or_create(js_var *p, const char *name, int flags) {
    js_varlink *l = js_var_find_child(p, name);
    if (l) return l;
    return js_var_add_child(p, name, js_var_new_blank(flags));
}

js_varlink *js_var_find_child_or_create_by_path(js_var *p, const char *path) {
    char *dup = js_strdup(path);
    char *dot = strchr(dup, '.');
    js_varlink *r;
    if (!dot) {
        r = js_var_find_child_or_create(p, dup, JS_V_OBJECT);
    } else {
        *dot = 0;
        js_var *sub = js_var_find_child_or_create(p, dup, JS_V_OBJECT)->var;
        r = js_var_find_child_or_create_by_path(sub, dot + 1);
    }
    js_free(dup);
    return r;
}

void js_var_remove_link(js_var *p, js_varlink *link) {
    if (!link) return;
    if (link->next) link->next->prev = link->prev;
    if (link->prev) link->prev->next = link->next;
    if (p->lastChild == link) p->lastChild = link->prev;
    if (p->firstChild == link) p->firstChild = link->next;
    js_link_free(link);
}

void js_var_remove_child(js_var *p, js_var *child) {
    js_varlink *link = p->firstChild;
    while (link) {
        if (link->var == child) break;
        link = link->next;
    }
    js_assert(link);
    js_var_remove_link(p, link);
}

void js_var_remove_all_children(js_var *p) {
    js_varlink *c = p->firstChild;
    while (c) {
        js_varlink *n = c->next;
        js_link_free(c);
        c = n;
    }
    p->firstChild = p->lastChild = 0;
}

js_var *js_var_get_array_index(js_var *p, int idx) {
    char s[64]; js_sprintf(s, sizeof(s), "%d", idx);
    js_varlink *link = js_var_find_child(p, s);
    if (link) return link->var;
    return js_var_new_null();
}

void js_var_set_array_index(js_var *p, int idx, js_var *value) {
    char s[64]; js_sprintf(s, sizeof(s), "%d", idx);
    js_varlink *link = js_var_find_child(p, s);
    if (link) {
        if (js_var_is_undefined(value)) js_var_remove_link(p, link);
        else js_link_replace_var(link, value);
    } else {
        if (!js_var_is_undefined(value)) js_var_add_child(p, s, value);
    }
}

int js_var_get_array_length(js_var *p) {
    int highest = -1;
    if (!js_var_is_array(p)) return 0;
    js_varlink *link = p->firstChild;
    while (link) {
        if (js_is_numeric_str(link->name)) {
            int val = js_link_get_int_name(link);
            if (val > highest) highest = val;
        }
        link = link->next;
    }
    return highest + 1;
}

int js_var_get_children(js_var *p) {
    int n = 0;
    js_varlink *link = p->firstChild;
    while (link) { n++; link = link->next; }
    return n;
}

/* ---------------- accessors ---------------- */

long js_var_get_int(js_var *v) {
    if (js_var_is_int(v)) return v->intData;
    if (js_var_is_null(v)) return 0;
    if (js_var_is_undefined(v)) return 0;
    if (js_var_is_double(v)) return (long)v->doubleData;
    return 0;
}
double js_var_get_double(js_var *v) {
    if (js_var_is_double(v)) return v->doubleData;
    if (js_var_is_int(v)) return (double)v->intData;
    if (js_var_is_null(v)) return 0;
    if (js_var_is_undefined(v)) return 0;
    return 0;
}
int js_var_get_bool(js_var *v) { return js_var_get_int(v) != 0; }

void js_var_get_string(js_var *v, char *buf, int buflen) {
    char *s = js_var_to_string(v);
    js_sprintf(buf, buflen, "%s", s);
    js_free(s);
}

void js_var_set_int(js_var *v, long val) {
    v->flags = (v->flags & ~JS_V_TYPEMASK) | JS_V_INTEGER;
    v->intData = val;
    v->doubleData = 0;
    js_free(v->data); v->data = js_strdup(JS_BLANK_DATA);
}
void js_var_set_double(js_var *v, double val) {
    v->flags = (v->flags & ~JS_V_TYPEMASK) | JS_V_DOUBLE;
    v->doubleData = val;
    v->intData = 0;
    js_free(v->data); v->data = js_strdup(JS_BLANK_DATA);
}
void js_var_set_string(js_var *v, const char *str) {
    v->flags = (v->flags & ~JS_V_TYPEMASK) | JS_V_STRING;
    js_free(v->data);
    v->data = js_strdup(str ? str : "");
    v->intData = 0;
    v->doubleData = 0;
}
void js_var_set_undefined(js_var *v) {
    v->flags = (v->flags & ~JS_V_TYPEMASK) | JS_V_UNDEFINED;
    js_free(v->data); v->data = js_strdup(JS_BLANK_DATA);
    v->intData = 0; v->doubleData = 0;
    js_var_remove_all_children(v);
}

/* Set the raw data string (used for function bodies) WITHOUT changing flags. */
void js_var_set_data(js_var *v, const char *s) {
    js_free(v->data);
    v->data = js_strdup(s ? s : "");
}
void js_var_set_array(js_var *v) {
    v->flags = (v->flags & ~JS_V_TYPEMASK) | JS_V_ARRAY;
    js_free(v->data); v->data = js_strdup(JS_BLANK_DATA);
    v->intData = 0; v->doubleData = 0;
    js_var_remove_all_children(v);
}

int js_var_is_undefined(js_var *v) {
    return (v->flags & JS_V_TYPEMASK) == JS_V_UNDEFINED;
}

/* C-string rendering (caller js_free's the result). */
char *js_var_to_string(js_var *v) {
    char buf[64];
    if (js_var_is_int(v)) {
        js_sprintf(buf, sizeof(buf), "%ld", v->intData);
        return js_strdup(buf);
    }
    if (js_var_is_double(v)) {
        js_dtoa(v->doubleData, buf, sizeof(buf));
        return js_strdup(buf);
    }
    if (js_var_is_null(v)) return js_strdup("null");
    if (js_var_is_undefined(v)) return js_strdup("undefined");
    return js_strdup(v->data);
}

/* ---------------- function helpers ---------------- */

js_var *js_var_get_return_var(js_var *v) {
    return js_var_find_child_or_create(v, JS_RETURN_VAR, JS_V_UNDEFINED)->var;
}
void js_var_set_return_var(js_var *v, js_var *val) {
    js_var_find_child_or_create(v, JS_RETURN_VAR, JS_V_UNDEFINED);
    js_varlink *l = js_var_find_child(v, JS_RETURN_VAR);
    js_link_replace_var(l, val);
}
js_var *js_var_get_parameter(js_var *func, const char *name) {
    return js_var_find_child_or_create(func, name, JS_V_UNDEFINED)->var;
}

void js_var_set_callback(js_var *v, js_callback cb, void *ud) {
    v->jsCallback = cb;
    v->jsCallbackUserData = ud;
}
void *js_var_get_user_data(js_var *v) { return v->userCustomData; }
void  js_var_set_user_data(js_var *v, void *p) { v->userCustomData = p; }

/* ---------------- copy / maths ---------------- */

static void js_var_copy_simple(js_var *dst, js_var *src) {
    js_free(dst->data);
    dst->data = js_strdup(src->data);
    dst->intData = src->intData;
    dst->doubleData = src->doubleData;
    dst->userCustomData = src->userCustomData;
    dst->flags = (dst->flags & ~JS_V_TYPEMASK) | (src->flags & JS_V_TYPEMASK);
}

void js_var_copy_value(js_var *dst, js_var *val) {
    if (!val) { js_var_set_undefined(dst); return; }
    js_var_copy_simple(dst, val);
    js_var_remove_all_children(dst);
    js_varlink *c = val->firstChild;
    while (c) {
        js_var *copied = (js_strcmp(c->name, JS_PROTOTYPE_CLASS) == 0)
                         ? c->var : js_var_deep_copy(c->var);
        js_var_add_child(dst, c->name, copied);
        c = c->next;
    }
}

js_var *js_var_deep_copy(js_var *v) {
    js_var *n = js_var_new_undefined();
    js_var_copy_simple(n, v);
    js_varlink *c = v->firstChild;
    while (c) {
        js_var *copied = (js_strcmp(c->name, JS_PROTOTYPE_CLASS) == 0)
                         ? c->var : js_var_deep_copy(c->var);
        js_var_add_child(n, c->name, copied);
        c = c->next;
    }
    return n;
}

/* Value equality (used by Array.contains). Basic values compare by rendered
 * string; objects/arrays compare children recursively. */
int js_var_equals(js_var *a, js_var *b) {
    if (js_var_is_object(a) && js_var_is_object(b)) {
        js_varlink *ca = a->firstChild, *cb;
        int n = 0;
        while (ca) {
            if (js_strcmp(ca->name, JS_RETURN_VAR) == 0) { ca = ca->next; continue; }
            cb = js_var_find_child(b, ca->name);
            if (!cb || !js_var_equals(ca->var, cb->var)) return 0;
            n++; ca = ca->next;
        }
        return n == js_var_get_children(b);
    }
    if (js_var_is_array(a) && js_var_is_array(b)) {
        int la = js_var_get_array_length(a), lb = js_var_get_array_length(b);
        if (la != lb) return 0;
        int i;
        for (i = 0; i < la; i++) {
            js_var *va = js_var_get_array_index(a, i);
            js_var *vb = js_var_get_array_index(b, i);
            if (!js_var_equals(va, vb)) return 0;
        }
        return 1;
    }
    if (js_var_is_basic(a) && js_var_is_basic(b)) {
        char sa[256], sb[256];
        js_var_get_string(a, sa, sizeof(sa));
        js_var_get_string(b, sb, sizeof(sb));
        return js_strcmp(sa, sb) == 0;
    }
    return 0;
}

/* JSON serializer. Appends a JSON representation into buf (bounded by len). */
static void js_var_get_json_rec(js_var *v, char *buf, int len) {
    if (!v) { js_strlcat(buf, "null", len); return; }
    if (js_var_is_array(v)) {
        int i, l = js_var_get_array_length(v);
        js_strlcat(buf, "[", len);
        for (i = 0; i < l; i++) {
            if (i) js_strlcat(buf, ",", len);
            js_var_get_json_rec(js_var_get_array_index(v, i), buf, len);
        }
        js_strlcat(buf, "]", len);
    } else if (js_var_is_function(v)) {
        /* serialize as 'function (a,b) { body }' so eval() can reconstruct it */
        js_strlcat(buf, "function (", len);
        js_varlink *p = v->firstChild;
        int firstp = 1;
        while (p) {
            if (!firstp) js_strlcat(buf, ",", len);
            firstp = 0;
            js_strlcat(buf, p->name, len);
            p = p->next;
        }
        js_strlcat(buf, ") ", len);
        if (v->data) js_strlcat(buf, v->data, len);
    } else if (js_var_is_object(v)) {
        js_strlcat(buf, "{", len);
        int first = 1;
        js_varlink *c = v->firstChild;
        while (c) {
            if (js_strcmp(c->name, JS_RETURN_VAR) != 0 &&
                js_strcmp(c->name, JS_PROTOTYPE_CLASS) != 0 &&
                js_strcmp(c->name, JS_TEMP_NAME) != 0) {
                if (!first) js_strlcat(buf, ",", len);
                first = 0;
                js_strlcat(buf, "\"", len);
                js_strlcat(buf, c->name, len);
                js_strlcat(buf, "\":", len);
                js_var_get_json_rec(c->var, buf, len);
            }
            c = c->next;
        }
        js_strlcat(buf, "}", len);
    } else if (js_var_is_string(v)) {
        js_strlcat(buf, "\"", len);
        /* escape minimal set */
        char *s = v->data;
        while (s && *s) {
            if (*s == '"' || *s == '\\') { js_strlcat(buf, "\\", len); }
            char tmp[2] = { *s, 0 };
            js_strlcat(buf, tmp, len);
            s++;
        }
        js_strlcat(buf, "\"", len);
    } else if (js_var_is_double(v)) {
        char tmp[64];
        js_dtoa(v->doubleData, tmp, sizeof(tmp));
        js_strlcat(buf, tmp, len);
    } else if (js_var_is_int(v)) {
        char tmp[64];
        js_sprintf(tmp, sizeof(tmp), "%ld", v->intData);
        js_strlcat(buf, tmp, len);
    } else if (js_var_is_undefined(v)) {
        js_strlcat(buf, "undefined", len);
    } else {
        js_strlcat(buf, "null", len);
    }
}
void js_var_get_json(js_var *v, char *buf, int len) {
    buf[0] = 0;
    js_var_get_json_rec(v, buf, len);
}

static js_var *js_maths_op_int(long da, long db, int op) {
    switch (op) {
        case '+': return js_var_new_int(da + db);
        case '-': return js_var_new_int(da - db);
        case '*': return js_var_new_int(da * db);
        case '/': return js_var_new_int(da / db);
        case '&': return js_var_new_int(da & db);
        case '|': return js_var_new_int(da | db);
        case '^': return js_var_new_int(da ^ db);
        case '%': return js_var_new_int(da % db);
        case JS_EQUAL:   return js_var_new_int(da == db);
        case JS_NEQUAL:  return js_var_new_int(da != db);
        case '<':        return js_var_new_int(da <  db);
        case JS_LEQUAL:  return js_var_new_int(da <= db);
        case '>':        return js_var_new_int(da >  db);
        case JS_GEQUAL:  return js_var_new_int(da >= db);
        default: js_throw_current("Operation not supported on Int"); return 0;
    }
}
static js_var *js_maths_op_double(double da, double db, int op) {
    switch (op) {
        case '+': return js_var_new_double(da + db);
        case '-': return js_var_new_double(da - db);
        case '*': return js_var_new_double(da * db);
        case '/': return js_var_new_double(da / db);
        case JS_EQUAL:   return js_var_new_int(da == db);
        case JS_NEQUAL:  return js_var_new_int(da != db);
        case '<':        return js_var_new_int(da <  db);
        case JS_LEQUAL:  return js_var_new_int(da <= db);
        case '>':        return js_var_new_int(da >  db);
        case JS_GEQUAL:  return js_var_new_int(da >= db);
        default: js_throw_current("Operation not supported on Double"); return 0;
    }
}

js_var *js_var_maths_op(js_var *a, js_var *b, int op) {
    /* type equality */
    if (op == JS_TYPEEQUAL || op == JS_NTYPEEQUAL) {
        int eql = ((a->flags & JS_V_TYPEMASK) == (b->flags & JS_V_TYPEMASK));
        if (eql) {
            js_var *contents = js_var_maths_op(a, b, JS_EQUAL);
            eql = js_var_get_bool(contents);
            js_var_unref(contents);
        }
        return (op == JS_TYPEEQUAL) ? js_var_new_int(eql) : js_var_new_int(!eql);
    }
    if (js_var_is_undefined(a) && js_var_is_undefined(b)) {
        if (op == JS_EQUAL)  return js_var_new_int(1);
        if (op == JS_NEQUAL) return js_var_new_int(0);
        return js_var_new_undefined();
    }
    if ((js_var_is_numeric(a) || js_var_is_undefined(a)) &&
        (js_var_is_numeric(b) || js_var_is_undefined(b))) {
        if (!js_var_is_double(a) && !js_var_is_double(b))
            return js_maths_op_int(js_var_get_int(a), js_var_get_int(b), op);
        return js_maths_op_double(js_var_get_double(a), js_var_get_double(b), op);
    }
    if (js_var_is_array(a)) {
        if (op == JS_EQUAL)  return js_var_new_int(a == b);
        if (op == JS_NEQUAL) return js_var_new_int(a != b);
        js_throw_current("Operation not supported on Array"); return 0;
    }
    if (js_var_is_object(a)) {
        if (op == JS_EQUAL)  return js_var_new_int(a == b);
        if (op == JS_NEQUAL) return js_var_new_int(a != b);
        js_throw_current("Operation not supported on Object"); return 0;
    }
    /* strings */
    char *da = js_var_to_string(a);
    char *db = js_var_to_string(b);
    js_var *r = 0;
    switch (op) {
        case '+': {
            int la = strlen(da), lb = strlen(db);
            char *cat = (char*)js_malloc(la + lb + 1);
            memcpy(cat, da, la); memcpy(cat + la, db, lb + 1);
            r = js_var_new_string(cat);
            js_free(cat);
            break;
        }
        case JS_EQUAL:   r = js_var_new_int(js_strcmp(da, db) == 0); break;
        case JS_NEQUAL:  r = js_var_new_int(js_strcmp(da, db) != 0); break;
        case '<':        r = js_var_new_int(js_strcmp(da, db) <  0); break;
        case JS_LEQUAL:  r = js_var_new_int(js_strcmp(da, db) <= 0); break;
        case '>':        r = js_var_new_int(js_strcmp(da, db) >  0); break;
        case JS_GEQUAL:  r = js_var_new_int(js_strcmp(da, db) >= 0); break;
        default: js_throw_current("Operation not supported on String"); r = 0;
    }
    js_free(da); js_free(db);
    return r;
}
