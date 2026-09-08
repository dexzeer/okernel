/*
 * okai JS — interpreter (CTinyJS port).
 *
 * Recursive-descent parser + tree-walking interpreter. Pass-by-reference
 * `bool &execute` becomes `int *execute` (so `if`/`return` can switch off
 * execution in a branch). Errors use setjmp/longjmp via g_js_current.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js.h"
#include <string.h>

#define JS_LOOP_MAX 1000000

/* ---- current interpreter (for error handling) ---- */
js_tiny *g_js_current = 0;

void js_throw_current(const char *msg) {
    js_tiny *t = g_js_current;
    if (!t) { js_printf("JS error (no interpreter): %s\n", msg); return; }
    int cap = strlen(msg) + 128;
    char *buf = (char*)js_malloc(cap);
    js_sprintf(buf, cap, "%s", msg);
    if (t->l) {
        char pos[64]; js_lex_position(t->l, pos, sizeof(pos));
        int off = strlen(buf);
        js_sprintf(buf + off, cap - off, " at %s", pos);
    }
    if (t->error) js_free(t->error);
    t->error = buf;
    t->has_error = 1;
    longjmp(t->jb, 1);
}

/* ---- scope stack ---- */
static void js_push_scope(js_tiny *t, js_var *v) {
    if (t->scopes_len >= t->scopes_cap) {
        t->scopes_cap = t->scopes_cap ? t->scopes_cap * 2 : 8;
        t->scopes = (js_var**)js_realloc(t->scopes, t->scopes_cap * sizeof(js_var*));
    }
    t->scopes[t->scopes_len++] = v;
}
static js_var *js_pop_scope(js_tiny *t) { return t->scopes[--t->scopes_len]; }
static js_var *js_top_scope(js_tiny *t) { return t->scopes[t->scopes_len - 1]; }

static js_varlink *js_find_in_scopes(js_tiny *t, const char *name) {
    int i;
    for (i = t->scopes_len - 1; i >= 0; i--) {
        js_varlink *v = js_var_find_child(t->scopes[i], name);
        if (v) return v;
    }
    return 0;
}

static js_varlink *js_find_in_parent_classes(js_tiny *t, js_var *object, const char *name) {
    if (!name || !name[0]) return 0;
    js_varlink *parentClass = js_var_find_child(object, JS_PROTOTYPE_CLASS);
    while (parentClass) {
        js_varlink *impl = js_var_find_child(parentClass->var, name);
        if (impl) return impl;
        parentClass = js_var_find_child(parentClass->var, JS_PROTOTYPE_CLASS);
    }
    if (js_var_is_string(object)) {
        js_varlink *impl = js_var_find_child(t->stringClass, name);
        if (impl) return impl;
    }
    if (js_var_is_array(object)) {
        js_varlink *impl = js_var_find_child(t->arrayClass, name);
        if (impl) return impl;
    }
    return js_var_find_child(t->objectClass, name);
}

static char *js_lex_substr(js_lex *l, int start, int end) {
    int n = end - start;
    if (n < 0) n = 0;
    char *s = (char*)js_malloc(n + 1);
    memcpy(s, l->data + start, n);
    s[n] = 0;
    return s;
}

/* ---- forward decls ---- */
static js_varlink *js_factor(js_tiny *t, int *execute);
static js_varlink *js_base(js_tiny *t, int *execute);
static js_varlink *js_function_call(js_tiny *t, js_varlink *function, js_var *parent, int *execute);
static void js_statement(js_tiny *t, int *execute);
static void js_block(js_tiny *t, int *execute);
static js_varlink *js_parse_function_definition(js_tiny *t);
static void js_parse_function_args(js_tiny *t, js_var *funcVar);

/* ---- public lifecycle ---- */
js_tiny *js_create(void) {
    js_tiny *t = (js_tiny*)js_malloc(sizeof(js_tiny));
    memset(t, 0, sizeof(js_tiny));
    t->root = js_var_new_blank(JS_V_OBJECT);
    t->root->flags |= JS_V_FUNCTION;
    js_var_ref(t->root);
    t->stringClass = js_var_new_blank(JS_V_OBJECT);
    t->objectClass = js_var_new_blank(JS_V_OBJECT);
    t->arrayClass  = js_var_new_blank(JS_V_ARRAY);
    js_var_add_child(t->root, "String", t->stringClass);
    js_var_add_child(t->root, "Object", t->objectClass);
    js_var_add_child(t->root, "Array",  t->arrayClass);
    t->scopes = 0; t->scopes_len = t->scopes_cap = 0;
    t->call_stack = 0; t->call_stack_len = t->call_stack_cap = 0;
    t->error = 0; t->has_error = 0;
    t->l = 0;
    return t;
}

void js_destroy(js_tiny *t) {
    if (!t) return;
    js_var_unref(t->root);
    if (t->scopes) js_free(t->scopes);
    if (t->call_stack) js_free(t->call_stack);
    if (t->error) js_free(t->error);
    js_free(t);
}

js_var *js_get_variable(js_tiny *t, const char *path) {
    js_varlink *link = js_var_find_child_or_create_by_path(t->root, path);
    return link->var;
}
int js_has_error(js_tiny *t) { return t->has_error; }
const char *js_error_msg(js_tiny *t) { return t->error ? t->error : ""; }

/* ---- native binding ---- */
void js_add_native(js_tiny *t, const char *funcDesc, js_callback cb, void *userdata) {
    js_lex *old = t->l;
    t->l = js_lex_create(funcDesc);
    js_var *base = t->root;
    js_lex_match(t->l, JS_R_FUNCTION);
    char *funcName = js_strdup(t->l->tkStr);
    js_lex_match(t->l, JS_ID);
    while (t->l->tk == '.') {
        js_lex_match(t->l, '.');
        js_varlink *link = js_var_find_child(base, funcName);
        if (!link) link = js_var_add_child(base, funcName, js_var_new_blank(JS_V_OBJECT));
        base = link->var;
        js_free(funcName);
        funcName = js_strdup(t->l->tkStr);
        js_lex_match(t->l, JS_ID);
    }
    js_var *funcVar = js_var_new_blank(JS_V_FUNCTION | JS_V_NATIVE);
    js_var_set_callback(funcVar, cb, userdata);
    js_parse_function_args(t, funcVar);
    js_lex_free(t->l);
    t->l = old;
    js_var_add_child(base, funcName, funcVar);
    js_free(funcName);
}

static void js_parse_function_args(js_tiny *t, js_var *funcVar) {
    js_lex *l = t->l;
    js_lex_match(l, '(');
    while (l->tk != ')') {
        js_var_add_child_no_dup(funcVar, l->tkStr, js_var_new_undefined());
        js_lex_match(l, JS_ID);
        if (l->tk != ')') js_lex_match(l, ',');
    }
    js_lex_match(l, ')');
}

static js_varlink *js_parse_function_definition(js_tiny *t) {
    js_lex *l = t->l;
    js_lex_match(l, JS_R_FUNCTION);
    char *funcName = js_strdup(JS_TEMP_NAME);
    if (l->tk == JS_ID) {
        js_free(funcName);
        funcName = js_strdup(l->tkStr);
        js_lex_match(l, JS_ID);
    }
    js_varlink *funcVar = js_link_new(js_var_new_blank(JS_V_FUNCTION), funcName);
    js_parse_function_args(t, funcVar->var);
    int funcBegin = l->tokenStart;
    int noexecute = 0;
    js_block(t, &noexecute);
    char *body = js_lex_substr(l, funcBegin, l->tokenLastEnd + 1);
    js_var_set_data(funcVar->var, body);
    js_free((void *)body);
    js_free(funcName);
    return funcVar;
}

/* ---- function call ---- */
static js_varlink *js_function_call(js_tiny *t, js_varlink *function, js_var *parent, int *execute) {
    js_lex *l = t->l;
    if (*execute) {
        if (!js_var_is_function(function->var)) {
            char msg[256];
            js_sprintf(msg, sizeof(msg), "Expecting '%s' to be a function",
                       function->name ? function->name : "");
            js_throw_current(msg);
        }
        js_lex_match(l, '(');
        js_var *functionRoot = js_var_new_blank(JS_V_FUNCTION);
        if (parent) js_var_add_child_no_dup(functionRoot, "this", parent);
        js_varlink *v = function->var->firstChild;
        while (l->tk != ')') {
            js_varlink *value = js_base(t, execute);
            if (*execute && v) {
                if (js_var_is_basic(value->var))
                    js_var_add_child_no_dup(functionRoot, v->name, js_var_deep_copy(value->var));
                else
                    js_var_add_child_no_dup(functionRoot, v->name, value->var);
            }
            JS_CLEAN(value);
            if (l->tk != ')') js_lex_match(l, ',');
            if (v) v = v->next;
        }
        js_lex_match(l, ')');
        js_varlink *returnVarLink = js_var_add_child(functionRoot, JS_RETURN_VAR, js_var_new_undefined());
        js_push_scope(t, functionRoot);
        if (js_var_is_native(function->var)) {
            js_assert(function->var->jsCallback);
            function->var->jsCallback(functionRoot, function->var->jsCallbackUserData);
        } else {
            js_lex *oldLex = t->l;
            js_lex *newLex = js_lex_create(function->var->data);
            t->l = newLex;
            js_block(t, execute);
            *execute = 1;
            js_lex_free(newLex);
            t->l = oldLex;
        }
        js_pop_scope(t);
        js_var *retVar = returnVarLink->var;
        js_varlink *returnVar = js_link_new(retVar, "");
        js_var_remove_link(functionRoot, returnVarLink);
        js_var_free(functionRoot);
        return returnVar;
    } else {
        js_lex_match(l, '(');
        while (l->tk != ')') {
            js_varlink *value = js_base(t, execute);
            JS_CLEAN(value);
            if (l->tk != ')') js_lex_match(l, ',');
        }
        js_lex_match(l, ')');
        if (l->tk == '{') js_block(t, execute);
        return function;
    }
}

/* ---- factor ---- */
static js_varlink *js_factor(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    if (l->tk == '(') {
        js_lex_next_token(l);
        js_varlink *a = js_base(t, execute);
        js_lex_match(l, ')');
        return a;
    }
    if (l->tk == JS_R_TRUE)  { js_lex_next_token(l); return js_link_new(js_var_new_int(1), ""); }
    if (l->tk == JS_R_FALSE) { js_lex_next_token(l); return js_link_new(js_var_new_int(0), ""); }
    if (l->tk == JS_R_NULL)  { js_lex_next_token(l); return js_link_new(js_var_new_null(), ""); }
    if (l->tk == JS_R_UNDEFINED) { js_lex_next_token(l); return js_link_new(js_var_new_undefined(), ""); }
    if (l->tk == JS_ID) {
        js_varlink *a = *execute ? js_find_in_scopes(t, l->tkStr) : js_link_new(js_var_new_undefined(), "");
        js_var *parent = 0;
        if (*execute && !a) {
            a = js_link_new(js_var_new_undefined(), l->tkStr);
        }
        js_lex_next_token(l);
        while (l->tk == '(' || l->tk == '.' || l->tk == '[') {
            if (l->tk == '(') {
                a = js_function_call(t, a, parent, execute);
            } else if (l->tk == '.') {
                js_lex_next_token(l);
                if (*execute) {
                    const char *name = l->tkStr;
                    js_varlink *child = js_var_find_child(a->var, name);
                    if (!child) child = js_find_in_parent_classes(t, a->var, name);
                    if (!child) {
                        if (js_var_is_array(a->var) && js_strcmp(name, "length") == 0) {
                            child = js_link_new(js_var_new_int(js_var_get_array_length(a->var)), "");
                        } else if (js_var_is_string(a->var) && js_strcmp(name, "length") == 0) {
                            char *s = js_var_to_string(a->var);
                            int len = strlen(s); js_free(s);
                            child = js_link_new(js_var_new_int(len), "");
                        } else {
                            child = js_var_add_child(a->var, name, js_var_new_undefined());
                        }
                    }
                    parent = a->var;
                    a = child;
                }
                js_lex_match(l, JS_ID);
            } else if (l->tk == '[') {
                js_lex_next_token(l);
                js_varlink *index = js_base(t, execute);
                js_lex_match(l, ']');
                if (*execute) {
                    char *idxName = js_var_to_string(index->var);
                    js_varlink *child = js_var_find_child_or_create(a->var, idxName, JS_V_UNDEFINED);
                    js_free(idxName);
                    parent = a->var;
                    a = child;
                }
                JS_CLEAN(index);
            } else js_assert(0);
        }
        return a;
    }
    if (l->tk == JS_INT || l->tk == JS_FLOAT) {
        js_var *a;
        if (l->tk == JS_INT) a = js_var_new_int(js_strtol(l->tkStr, 0, 0));
        else                 a = js_var_new_double(js_strtod(l->tkStr, 0));
        js_lex_next_token(l);
        return js_link_new(a, "");
    }
    if (l->tk == JS_STR) {
        js_var *a = js_var_new_string(l->tkStr);
        js_lex_next_token(l);
        return js_link_new(a, "");
    }
    if (l->tk == '{') {
        js_var *contents = js_var_new_blank(JS_V_OBJECT);
        js_lex_next_token(l);
        while (l->tk != '}') {
            char *id = js_strdup(l->tkStr);
            if (l->tk == JS_STR) js_lex_next_token(l);
            else js_lex_match(l, JS_ID);
            js_lex_match(l, ':');
            if (*execute) {
                js_varlink *a = js_base(t, execute);
                js_var_add_child(contents, id, a->var);
                JS_CLEAN(a);
            }
            if (l->tk != '}') js_lex_match(l, ',');
            js_free(id);
        }
        js_lex_match(l, '}');
        return js_link_new(contents, "");
    }
    if (l->tk == '[') {
        js_var *contents = js_var_new_blank(JS_V_ARRAY);
        js_lex_next_token(l);
        int idx = 0;
        while (l->tk != ']') {
            if (*execute) {
                char idx_str[16];
                js_sprintf(idx_str, sizeof(idx_str), "%d", idx);
                js_varlink *a = js_base(t, execute);
                js_var_add_child(contents, idx_str, a->var);
                JS_CLEAN(a);
            }
            if (l->tk != ']') js_lex_match(l, ',');
            idx++;
        }
        js_lex_match(l, ']');
        return js_link_new(contents, "");
    }
    if (l->tk == JS_R_FUNCTION) {
        js_varlink *funcVar = js_parse_function_definition(t);
        if (js_strcmp(funcVar->name, JS_TEMP_NAME) != 0)
            js_printf("Functions not defined at statement-level are not meant to have a name\n");
        return funcVar;
    }
    if (l->tk == JS_R_NEW) {
        js_lex_next_token(l);
        const char *className = l->tkStr;
        if (*execute) {
            js_varlink *objClassOrFunc = js_find_in_scopes(t, className);
            if (!objClassOrFunc) {
                /* unknown class -> plain object (mimics new Object()) */
                js_lex_next_token(l);
                if (l->tk == '(') { js_lex_match(l, '('); js_lex_match(l, ')'); }
                return js_link_new(js_var_new_blank(JS_V_OBJECT), "");
            }
            js_lex_next_token(l);
            js_var *obj = js_var_new_blank(JS_V_OBJECT);
            js_varlink *objLink = js_link_new(obj, "");
            if (js_var_is_function(objClassOrFunc->var)) {
                JS_CLEAN(js_function_call(t, objClassOrFunc, obj, execute));
            } else {
                js_var_add_child(obj, JS_PROTOTYPE_CLASS, objClassOrFunc->var);
                if (l->tk == '(') { js_lex_match(l, '('); js_lex_match(l, ')'); }
            }
            return objLink;
        } else {
            js_lex_next_token(l);
            if (l->tk == '(') { js_lex_match(l, '('); js_lex_match(l, ')'); }
            return js_link_new(js_var_new_undefined(), "");
        }
    }
    js_lex_match(l, JS_EOF);
    return 0;
}

static js_varlink *js_unary(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *a;
    if (l->tk == '!') {
        js_lex_next_token(l);
        a = js_factor(t, execute);
        if (*execute) {
            js_var *zero = js_var_new_int(0);
            js_var *res = js_var_maths_op(a->var, zero, JS_EQUAL);
            JS_CREATE_LINK(a, res);
            js_var_free(zero);
        }
    } else {
        a = js_factor(t, execute);
    }
    return a;
}

static js_varlink *js_term(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *a = js_unary(t, execute);
    while (l->tk == '*' || l->tk == '/' || l->tk == '%') {
        int op = l->tk; js_lex_next_token(l);
        js_varlink *b = js_unary(t, execute);
        if (*execute) {
            js_var *res = js_var_maths_op(a->var, b->var, op);
            JS_CREATE_LINK(a, res);
        }
        JS_CLEAN(b);
    }
    return a;
}

static js_varlink *js_expression(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    int negate = 0;
    if (l->tk == '-') { js_lex_next_token(l); negate = 1; }
    js_varlink *a = js_term(t, execute);
    if (negate) {
        js_var *zero = js_var_new_int(0);
        js_var *res = js_var_maths_op(zero, a->var, '-');
        JS_CREATE_LINK(a, res);
        js_var_free(zero);
    }
    while (l->tk == '+' || l->tk == '-' || l->tk == JS_PLUSPLUS || l->tk == JS_MINUSMINUS) {
        int op = l->tk; js_lex_next_token(l);
        if (op == JS_PLUSPLUS || op == JS_MINUSMINUS) {
            if (*execute) {
                js_var *one = js_var_new_int(1);
                js_var *res = js_var_maths_op(a->var, one, op == JS_PLUSPLUS ? '+' : '-');
                js_varlink *oldValue = js_link_new(a->var, "");
                js_link_replace_var(a, res);
                JS_CLEAN(a);
                a = oldValue;
                js_var_free(one);
            }
        } else {
            js_varlink *b = js_term(t, execute);
            if (*execute) {
                js_var *res = js_var_maths_op(a->var, b->var, op);
                JS_CREATE_LINK(a, res);
            }
            JS_CLEAN(b);
        }
    }
    return a;
}

static js_varlink *js_shift(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *a = js_expression(t, execute);
    if (l->tk == JS_LSHIFT || l->tk == JS_RSHIFT || l->tk == JS_RSHIFTUNSIGNED) {
        int op = l->tk; js_lex_next_token(l);
        js_varlink *b = js_base(t, execute);
        int shift = *execute ? js_var_get_int(b->var) : 0;
        JS_CLEAN(b);
        if (*execute) {
            if (op == JS_LSHIFT)
                js_var_set_int(a->var, js_var_get_int(a->var) << shift);
            else if (op == JS_RSHIFT)
                js_var_set_int(a->var, js_var_get_int(a->var) >> shift);
            else /* RSHIFTUNSIGNED */
                js_var_set_int(a->var, (int)((unsigned int)js_var_get_int(a->var) >> shift));
        }
    }
    return a;
}

static js_varlink *js_condition(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *a = js_shift(t, execute);
    while (l->tk == JS_EQUAL || l->tk == JS_NEQUAL || l->tk == JS_TYPEEQUAL ||
           l->tk == JS_NTYPEEQUAL || l->tk == JS_LEQUAL || l->tk == JS_GEQUAL ||
           l->tk == '<' || l->tk == '>') {
        int op = l->tk; js_lex_next_token(l);
        js_varlink *b = js_shift(t, execute);
        if (*execute) {
            js_var *res = js_var_maths_op(a->var, b->var, op);
            JS_CREATE_LINK(a, res);
        }
        JS_CLEAN(b);
    }
    return a;
}

static js_varlink *js_logic(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *a = js_condition(t, execute);
    while (l->tk == '&' || l->tk == '|' || l->tk == '^' ||
           l->tk == JS_ANDAND || l->tk == JS_OROR) {
        int op = l->tk; js_lex_next_token(l);
        int shortCircuit = 0, boolean = 0;
        if (op == JS_ANDAND)      { op = '&'; shortCircuit = !js_var_get_bool(a->var); boolean = 1; }
        else if (op == JS_OROR)   { op = '|'; shortCircuit =  js_var_get_bool(a->var); boolean = 1; }
        int noexecute = 0;
        js_varlink *b = js_condition(t, shortCircuit ? &noexecute : execute);
        if (*execute && !shortCircuit) {
            if (boolean) {
                js_var *newa = js_var_new_int(js_var_get_bool(a->var));
                js_var *newb = js_var_new_int(js_var_get_bool(b->var));
                JS_CREATE_LINK(a, newa);
                JS_CREATE_LINK(b, newb);
            }
            js_var *res = js_var_maths_op(a->var, b->var, op);
            JS_CREATE_LINK(a, res);
        }
        JS_CLEAN(b);
    }
    return a;
}

static js_varlink *js_ternary(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *lhs = js_logic(t, execute);
    int noexec = 0;
    if (l->tk == '?') {
        js_lex_next_token(l);
        if (!*execute) {
            JS_CLEAN(lhs);
            JS_CLEAN(js_base(t, &noexec));
            js_lex_match(l, ':');
            JS_CLEAN(js_base(t, &noexec));
        } else {
            int first = js_var_get_bool(lhs->var);
            JS_CLEAN(lhs);
            if (first) {
                lhs = js_base(t, execute);
                js_lex_match(l, ':');
                JS_CLEAN(js_base(t, &noexec));
            } else {
                JS_CLEAN(js_base(t, &noexec));
                js_lex_match(l, ':');
                lhs = js_base(t, execute);
            }
        }
    }
    return lhs;
}

static js_varlink *js_base(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_varlink *lhs = js_ternary(t, execute);
    if (l->tk == '=' || l->tk == JS_PLUSEQUAL || l->tk == JS_MINUSEQUAL) {
        if (*execute && !lhs->owned) {
            if (lhs->name && lhs->name[0]) {
                js_varlink *realLhs = js_var_add_child_no_dup(t->root, lhs->name, lhs->var);
                JS_CLEAN(lhs);
                lhs = realLhs;
            } else {
                js_printf("Trying to assign to an un-named type\n");
            }
        }
        int op = l->tk; js_lex_next_token(l);
        js_varlink *rhs = js_base(t, execute);
        if (*execute) {
            if (op == '=') {
                js_link_replace_var(lhs, rhs->var);
            } else {
                js_var *res = js_var_maths_op(lhs->var, rhs->var, op == JS_PLUSEQUAL ? '+' : '-');
                js_link_replace_var(lhs, res);
            }
        }
        JS_CLEAN(rhs);
    }
    return lhs;
}

static void js_block(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    js_lex_match(l, '{');
    if (*execute) {
        while (l->tk && l->tk != '}') js_statement(t, execute);
        js_lex_match(l, '}');
    } else {
        int brackets = 1;
        while (l->tk && brackets) {
            if (l->tk == '{') brackets++;
            if (l->tk == '}') brackets--;
            js_lex_next_token(l);
        }
    }
}

static void js_trace_root(js_tiny *t) {
    js_printf("JS: loop exceeded (root symbols: %d)\n", js_var_get_children(t->root));
}

static void js_statement(js_tiny *t, int *execute) {
    js_lex *l = t->l;
    if (l->tk == JS_ID || l->tk == JS_INT || l->tk == JS_FLOAT ||
        l->tk == JS_STR || l->tk == '-') {
        JS_CLEAN(js_base(t, execute));
        js_lex_match(l, ';');
    } else if (l->tk == '{') {
        js_block(t, execute);
    } else if (l->tk == ';') {
        js_lex_next_token(l);
    } else if (l->tk == JS_R_VAR) {
        js_lex_next_token(l);
        while (l->tk != ';') {
            js_varlink *a = 0;
            if (*execute) a = js_var_find_child_or_create(js_top_scope(t), l->tkStr, JS_V_UNDEFINED);
            js_lex_match(l, JS_ID);
            while (l->tk == '.') {
                js_lex_next_token(l);
                if (*execute) {
                    js_varlink *lastA = a;
                    a = js_var_find_child_or_create(lastA->var, l->tkStr, JS_V_UNDEFINED);
                }
                js_lex_match(l, JS_ID);
            }
            if (l->tk == '=') {
                js_lex_next_token(l);
                js_varlink *var = js_base(t, execute);
                if (*execute) js_link_replace_var(a, var->var);
                JS_CLEAN(var);
            }
            if (l->tk != ';') js_lex_match(l, ',');
        }
        js_lex_match(l, ';');
    } else if (l->tk == JS_R_IF) {
        js_lex_next_token(l);
        js_lex_match(l, '(');
        js_varlink *var = js_base(t, execute);
        js_lex_match(l, ')');
        int cond = *execute && js_var_get_bool(var->var);
        JS_CLEAN(var);
        int noexecute = 0;
        js_statement(t, cond ? execute : &noexecute);
        if (l->tk == JS_R_ELSE) {
            js_lex_next_token(l);
            js_statement(t, cond ? &noexecute : execute);
        }
    } else if (l->tk == JS_R_WHILE) {
        js_lex_next_token(l);
        js_lex_match(l, '(');
        int whileCondStart = l->tokenStart;
        int noexecute = 0;
        js_varlink *cond = js_base(t, execute);
        int loopCond = *execute && js_var_get_bool(cond->var);
        JS_CLEAN(cond);
        js_lex *whileCond = js_lex_create_sub(l, whileCondStart, l->tokenLastEnd + 1 - whileCondStart);
        js_lex_match(l, ')');
        int whileBodyStart = l->tokenStart;
        js_statement(t, loopCond ? execute : &noexecute);
        js_lex *whileBody = js_lex_create_sub(l, whileBodyStart, l->tokenLastEnd + 1 - whileBodyStart);
        js_lex *oldLex = t->l;
        int loopCount = JS_LOOP_MAX;
        while (loopCond && loopCount-- > 0) {
            js_lex_reset(whileCond);
            t->l = whileCond;
            cond = js_base(t, execute);
            loopCond = *execute && js_var_get_bool(cond->var);
            JS_CLEAN(cond);
            if (loopCond) {
                js_lex_reset(whileBody);
                t->l = whileBody;
                js_statement(t, execute);
            }
        }
        t->l = oldLex;
        js_lex_free(whileCond);
        js_lex_free(whileBody);
        if (loopCount <= 0) {
            js_trace_root(t);
            js_throw_current("WHILE Loop exceeded max iterations");
        }
    } else if (l->tk == JS_R_FOR) {
        js_lex_next_token(l);
        js_lex_match(l, '(');
        js_statement(t, execute);
        int forCondStart = l->tokenStart;
        int noexecute = 0;
        js_varlink *cond = js_base(t, execute);
        int loopCond = *execute && js_var_get_bool(cond->var);
        JS_CLEAN(cond);
        js_lex *forCond = js_lex_create_sub(l, forCondStart, l->tokenLastEnd + 1 - forCondStart);
        js_lex_match(l, ';');
        int forIterStart = l->tokenStart;
        JS_CLEAN(js_base(t, &noexecute));
        js_lex *forIter = js_lex_create_sub(l, forIterStart, l->tokenLastEnd + 1 - forIterStart);
        js_lex_match(l, ')');
        int forBodyStart = l->tokenStart;
        js_statement(t, loopCond ? execute : &noexecute);
        js_lex *forBody = js_lex_create_sub(l, forBodyStart, l->tokenLastEnd + 1 - forBodyStart);
        js_lex *oldLex = t->l;
        if (loopCond) {
            js_lex_reset(forIter);
            t->l = forIter;
            JS_CLEAN(js_base(t, execute));
        }
        int loopCount = JS_LOOP_MAX;
        while (*execute && loopCond && loopCount-- > 0) {
            js_lex_reset(forCond);
            t->l = forCond;
            cond = js_base(t, execute);
            loopCond = js_var_get_bool(cond->var);
            JS_CLEAN(cond);
            if (*execute && loopCond) {
                js_lex_reset(forBody);
                t->l = forBody;
                js_statement(t, execute);
            }
            if (*execute && loopCond) {
                js_lex_reset(forIter);
                t->l = forIter;
                JS_CLEAN(js_base(t, execute));
            }
        }
        t->l = oldLex;
        js_lex_free(forCond);
        js_lex_free(forIter);
        js_lex_free(forBody);
        if (loopCount <= 0) {
            js_trace_root(t);
            js_throw_current("FOR Loop exceeded max iterations");
        }
    } else if (l->tk == JS_R_RETURN) {
        js_lex_next_token(l);
        js_varlink *result = 0;
        if (l->tk != ';') result = js_base(t, execute);
        if (*execute) {
            js_varlink *resultVar = js_var_find_child(js_top_scope(t), JS_RETURN_VAR);
            if (resultVar) js_link_replace_var(resultVar, result->var);
            else js_printf("RETURN statement, but not in a function.\n");
            *execute = 0;
        }
        JS_CLEAN(result);
        js_lex_match(l, ';');
    } else if (l->tk == JS_R_FUNCTION) {
        js_varlink *funcVar = js_parse_function_definition(t);
        if (*execute) {
            if (js_strcmp(funcVar->name, JS_TEMP_NAME) == 0)
                js_printf("Functions defined at statement-level are meant to have a name\n");
            else
                js_var_add_child_no_dup(js_top_scope(t), funcVar->name, funcVar->var);
        }
        JS_CLEAN(funcVar);
    } else {
        js_lex_match(l, JS_EOF);
    }
}

/* ---- entry points ---- */
void js_execute(js_tiny *t, const char *code) {
    js_lex *old = t->l;
    int old_scopes_len = t->scopes_len;
    int was_current = (g_js_current == t);
    jmp_buf saved_jb;
    if (was_current) memcpy(&saved_jb, &t->jb, sizeof(jmp_buf));
    g_js_current = t;
    t->has_error = 0;
    if (setjmp(t->jb)) {
        if (t->l) js_lex_free(t->l);
        t->l = old;
        t->scopes_len = old_scopes_len;
        if (!was_current) g_js_current = 0;
        else memcpy(&t->jb, &saved_jb, sizeof(jmp_buf));
        return;
    }
    t->l = js_lex_create(code);
    t->scopes_len = 0;
    js_push_scope(t, t->root);
    int execute = 1;
    while (t->l->tk != JS_EOF) js_statement(t, &execute);
    js_pop_scope(t);
    js_lex_free(t->l);
    t->l = old;
    t->scopes_len = old_scopes_len;
    if (!was_current) g_js_current = 0;
    else memcpy(&t->jb, &saved_jb, sizeof(jmp_buf));
}

void js_evaluate(js_tiny *t, const char *code, char *out, int outlen) {
    js_lex *old = t->l;
    int old_scopes_len = t->scopes_len;
    t->l = js_lex_create(code);
    t->scopes_len = 0;
    js_push_scope(t, t->root);
    int execute = 1;
    js_varlink *v = 0;
    do {
        JS_CLEAN(v);
        v = js_base(t, &execute);
        if (t->l->tk != JS_EOF) js_lex_match(t->l, ';');
    } while (t->l->tk != JS_EOF);
    if (v) {
        if (out) { char *s = js_var_to_string(v->var); js_sprintf(out, outlen, "%s", s); js_free(s); }
        JS_CLEAN(v);
    } else if (out) out[0] = 0;
    js_pop_scope(t);
    js_lex_free(t->l);
    t->l = old;
    t->scopes_len = old_scopes_len;
}

/* Like js_evaluate, but returns the result js_var* (caller owns via refcount).
   Used by eval() which must yield the actual value, not its string form. */
js_var *js_evaluate_value(js_tiny *t, const char *code) {
    js_lex *old = t->l;
    int old_scopes_len = t->scopes_len;
    t->l = js_lex_create(code);
    t->scopes_len = 0;
    js_push_scope(t, t->root);
    int execute = 1;
    js_varlink *v = 0;
    do {
        JS_CLEAN(v);
        v = js_base(t, &execute);
        if (t->l->tk != JS_EOF) js_lex_match(t->l, ';');
    } while (t->l->tk != JS_EOF);
    js_var *result = 0;
    if (v) {
        result = js_var_ref(v->var);
        JS_CLEAN(v);
    }
    js_pop_scope(t);
    js_lex_free(t->l);
    t->l = old;
    t->scopes_len = old_scopes_len;
    return result;
}
