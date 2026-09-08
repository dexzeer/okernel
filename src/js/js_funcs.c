/*
 * okai JS — built-in natives (TinyJS_Functions.cpp port).
 *
 * "tinyjs ok edition (tm)": a substantially rewritten C port of Gordon
 * Williams' TinyJS (gfwilliams/tiny-js), adapted to a freestanding C kernel.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js.h"
#include <string.h>

#ifdef JS_KERNEL
/* self-authored PRNG (freestanding kernel has no libc rand()) */
static unsigned long js_rand_state = 123456789UL;
static long js_rand(void) {
    js_rand_state = js_rand_state * 1664525UL + 1013904223UL;
    return (long)(js_rand_state & 0x7fffffffUL);
}
#define JS_RAND_MAX 0x7fffffffL

#else
#include <stdlib.h>
#define js_rand() rand()
#define JS_RAND_MAX RAND_MAX
#endif

/* helper: render a parameter's string value into a temp buffer */
static void js_arg_str(js_var *scope, const char *name, char *buf, int n) {
    js_var_get_string(js_var_get_parameter(scope, name), buf, n);
}
static long js_arg_int(js_var *scope, const char *name) {
    return js_var_get_int(js_var_get_parameter(scope, name));
}

/* ---- print / eval / exec ---- */
static void scExec(js_var *c, void *userdata) {
    js_tiny *t = (js_tiny *)userdata;
    char buf[4096];
    js_arg_str(c, "jsCode", buf, sizeof(buf));
    js_execute(t, buf);
}
static void scEval(js_var *c, void *userdata) {
    js_tiny *t = (js_tiny *)userdata;
    char buf[4096];
    js_arg_str(c, "jsCode", buf, sizeof(buf));
    js_var *res = js_evaluate_value(t, buf);
    if (res) {
        js_var_copy_value(js_var_get_return_var(c), res);
        js_var_unref(res);
    } else {
        js_var_set_undefined(js_var_get_return_var(c));
    }
}
static void scTrace(js_var *c, void *userdata) {
    js_tiny *t = (js_tiny *)userdata;
    char buf[8192];
    js_var_get_json(t->root, buf, sizeof(buf));
    js_printf("TRACE %s\n", buf);
}
static void scObjectDump(js_var *c, void *userdata) {
    (void)userdata;
    char buf[8192];
    js_var_get_json(js_var_get_parameter(c, "this"), buf, sizeof(buf));
    js_printf("DUMP %s\n", buf);
}
static void scObjectClone(js_var *c, void *userdata) {
    (void)userdata;
    js_var *obj = js_var_get_parameter(c, "this");
    js_var_copy_value(js_var_get_return_var(c), obj);
}

/* ---- math (rand lives with the other math in the C++ split) ---- */
static void scMathRand(js_var *c, void *userdata) {
    (void)userdata;
    js_var_set_double(js_var_get_return_var(c), (double)js_rand() / (double)JS_RAND_MAX);
}
static void scMathRandInt(js_var *c, void *userdata) {
    (void)userdata;
    long min = js_arg_int(c, "min"), max = js_arg_int(c, "max");
    js_var_set_int(js_var_get_return_var(c), min + (long)(js_rand() % (1 + max - min)));
}

/* ---- char / string ---- */
static void scCharToInt(js_var *c, void *userdata) {
    (void)userdata;
    char s[256];
    js_arg_str(c, "ch", s, sizeof(s));
    js_var_set_int(js_var_get_return_var(c), s[0] ? (int)(unsigned char)s[0] : 0);
}
static void scStringIndexOf(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096], search[4096];
    js_arg_str(c, "this", str, sizeof(str));
    js_arg_str(c, "search", search, sizeof(search));
    char *p = js_strstr(str, search);
    js_var_set_int(js_var_get_return_var(c), p ? (int)(p - str) : -1);
}
static void scStringSubstring(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096];
    js_arg_str(c, "this", str, sizeof(str));
    int lo = js_arg_int(c, "lo"), hi = js_arg_int(c, "hi");
    int l = hi - lo;
    if (l > 0 && lo >= 0 && lo + l <= (int)strlen(str)) {
        char tmp[4096];
        js_strncpy(tmp, str + lo, l); tmp[l] = 0;
        js_var_set_string(js_var_get_return_var(c), tmp);
    } else {
        js_var_set_string(js_var_get_return_var(c), "");
    }
}
static void scStringCharAt(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096];
    js_arg_str(c, "this", str, sizeof(str));
    int p = js_arg_int(c, "pos");
    char tmp[2] = { 0, 0 };
    if (p >= 0 && p < (int)strlen(str)) tmp[0] = str[p];
    js_var_set_string(js_var_get_return_var(c), tmp);
}
static void scStringCharCodeAt(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096];
    js_arg_str(c, "this", str, sizeof(str));
    int p = js_arg_int(c, "pos");
    int v = (p >= 0 && p < (int)strlen(str)) ? (int)(unsigned char)str[p] : 0;
    js_var_set_int(js_var_get_return_var(c), v);
}
static void scStringSplit(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096], sep[256];
    js_arg_str(c, "this", str, sizeof(str));
    js_arg_str(c, "separator", sep, sizeof(sep));
    js_var *res = js_var_get_return_var(c);
    js_var_set_array(res);
    int length = 0;
    char *cur = str, *pos;
    if (sep[0] == 0) {
        /* empty separator: split into individual characters */
        int i = 0;
        while (str[i]) {
            char tmp[2] = { str[i], 0 };
            js_var_set_array_index(res, length++, js_var_new_string(tmp));
            i++;
        }
        return;
    }
    while ((pos = js_strstr(cur, sep)) != 0) {
        int n = (int)(pos - cur);
        char tmp[4096];
        js_strncpy(tmp, cur, n); tmp[n] = 0;
        js_var_set_array_index(res, length++, js_var_new_string(tmp));
        cur = pos + strlen(sep);
    }
    if (*cur) js_var_set_array_index(res, length++, js_var_new_string(cur));
}
static void scStringFromCharCode(js_var *c, void *userdata) {
    (void)userdata;
    char tmp[2] = { (char)js_arg_int(c, "char"), 0 };
    js_var_set_string(js_var_get_return_var(c), tmp);
}
static void scIntegerParseInt(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096];
    js_arg_str(c, "str", str, sizeof(str));
    js_var_set_int(js_var_get_return_var(c), js_strtol(str, 0, 0));
}
static void scIntegerValueOf(js_var *c, void *userdata) {
    (void)userdata;
    char str[4096];
    js_arg_str(c, "str", str, sizeof(str));
    js_var_set_int(js_var_get_return_var(c), (str[0] && !str[1]) ? (int)(unsigned char)str[0] : 0);
}
static void scJSONStringify(js_var *c, void *userdata) {
    (void)userdata;
    char buf[8192];
    js_var_get_json(js_var_get_parameter(c, "obj"), buf, sizeof(buf));
    js_var_set_string(js_var_get_return_var(c), buf);
}

/* ---- arrays ---- */
static void scArrayContains(js_var *c, void *userdata) {
    (void)userdata;
    js_var *obj = js_var_get_parameter(c, "obj");
    js_var *self = js_var_get_parameter(c, "this");
    int contains = 0;
    js_varlink *v = self->firstChild;
    while (v) {
        if (js_var_equals(v->var, obj)) { contains = 1; break; }
        v = v->next;
    }
    js_var_set_int(js_var_get_return_var(c), contains);
}
static void scArrayRemove(js_var *c, void *userdata) {
    (void)userdata;
    js_var *obj = js_var_get_parameter(c, "obj");
    js_var *self = js_var_get_parameter(c, "this");
    int removed[256], n = 0, i;
    js_varlink *v = self->firstChild;
    while (v) {
        if (js_var_equals(v->var, obj) && n < 256) removed[n++] = js_link_get_int_name(v);
        v = v->next;
    }
    v = self->firstChild;
    while (v) {
        int idx = js_link_get_int_name(v), newn = idx;
        for (i = 0; i < n; i++) if (idx >= removed[i]) newn--;
        if (newn != idx) js_link_set_int_name(v, newn);
        v = v->next;
    }
}
static void scArrayJoin(js_var *c, void *userdata) {
    (void)userdata;
    char sep[256], out[8192];
    js_arg_str(c, "separator", sep, sizeof(sep));
    js_var *arr = js_var_get_parameter(c, "this");
    out[0] = 0;
    int l = js_var_get_array_length(arr), i, n = 0;
    for (i = 0; i < l; i++) {
        if (i > 0) { js_strlcat(out, sep, sizeof(out)); n += (int)strlen(sep); }
        char tmp[256];
        js_var_get_string(js_var_get_array_index(arr, i), tmp, sizeof(tmp));
        js_strlcat(out, tmp, sizeof(out));
    }
    js_var_set_string(js_var_get_return_var(c), out);
}
static void scArrayPush(js_var *c, void *userdata) {
    (void)userdata;
    js_var *obj = js_var_get_parameter(c, "obj");
    js_var *arr = js_var_get_parameter(c, "this");
    int length = js_var_get_array_length(arr);
    js_var_set_array_index(arr, length, obj);
}

void registerFunctions(js_tiny *t) {
    js_add_native(t, "function exec(jsCode)", scExec, t);
    js_add_native(t, "function eval(jsCode)", scEval, t);
    js_add_native(t, "function trace()", scTrace, t);
    js_add_native(t, "function Object.dump()", scObjectDump, 0);
    js_add_native(t, "function Object.clone()", scObjectClone, 0);
    js_add_native(t, "function Math.rand()", scMathRand, 0);
    js_add_native(t, "function Math.randInt(min, max)", scMathRandInt, 0);
    js_add_native(t, "function charToInt(ch)", scCharToInt, 0);
    js_add_native(t, "function String.indexOf(search)", scStringIndexOf, 0);
    js_add_native(t, "function String.substring(lo,hi)", scStringSubstring, 0);
    js_add_native(t, "function String.charAt(pos)", scStringCharAt, 0);
    js_add_native(t, "function String.charCodeAt(pos)", scStringCharCodeAt, 0);
    js_add_native(t, "function String.fromCharCode(char)", scStringFromCharCode, 0);
    js_add_native(t, "function String.split(separator)", scStringSplit, 0);
    js_add_native(t, "function Integer.parseInt(str)", scIntegerParseInt, 0);
    js_add_native(t, "function Integer.valueOf(str)", scIntegerValueOf, 0);
    js_add_native(t, "function JSON.stringify(obj, replacer)", scJSONStringify, 0);
    js_add_native(t, "function Array.contains(obj)", scArrayContains, 0);
    js_add_native(t, "function Array.remove(obj)", scArrayRemove, 0);
    js_add_native(t, "function Array.join(separator)", scArrayJoin, 0);
    js_add_native(t, "function Array.push(obj)", scArrayPush, 0);
}
