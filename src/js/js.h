/*
 * okai JS — public types and API.
 *
 * A from-scratch C port of TinyJS (recursive-descent JS-alike interpreter).
 * "tinyjs ok edition (tm)": substantial rewrite of gfwilliams/tiny-js +
 * MarcoLizza/tiny-js for a freestanding kernel.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#ifndef JS_H
#define JS_H

#include "js_os.h"

/* ---- lexer token types ---- */
enum {
    JS_EOF = 0,
    JS_ID = 256, JS_INT, JS_FLOAT, JS_STR,

    JS_EQUAL, JS_TYPEEQUAL, JS_NEQUAL, JS_NTYPEEQUAL,
    JS_LEQUAL, JS_LSHIFT, JS_LSHIFTEQUAL, JS_GEQUAL,
    JS_RSHIFT, JS_RSHIFTUNSIGNED, JS_RSHIFTEQUAL,
    JS_PLUSEQUAL, JS_MINUSEQUAL, JS_PLUSPLUS, JS_MINUSMINUS,
    JS_ANDEQUAL, JS_ANDAND, JS_OREQUAL, JS_OROR, JS_XOREQUAL,

    /* reserved words */
    JS_R_IF, JS_R_ELSE, JS_R_DO, JS_R_WHILE, JS_R_FOR,
    JS_R_BREAK, JS_R_CONTINUE, JS_R_FUNCTION, JS_R_RETURN,
    JS_R_VAR, JS_R_TRUE, JS_R_FALSE, JS_R_NULL, JS_R_UNDEFINED, JS_R_NEW
};

/* ---- value flags ---- */
enum {
    JS_V_UNDEFINED  = 0,
    JS_V_FUNCTION   = 1,
    JS_V_OBJECT     = 2,
    JS_V_ARRAY      = 4,
    JS_V_DOUBLE     = 8,
    JS_V_INTEGER    = 16,
    JS_V_STRING     = 32,
    JS_V_NULL       = 64,
    JS_V_NATIVE     = 128,
    JS_V_NUMERIC    = JS_V_NULL | JS_V_DOUBLE | JS_V_INTEGER,
    JS_V_TYPEMASK   = JS_V_DOUBLE | JS_V_INTEGER | JS_V_STRING |
                      JS_V_FUNCTION | JS_V_OBJECT | JS_V_ARRAY | JS_V_NULL
};

#define JS_RETURN_VAR       "return"
#define JS_PROTOTYPE_CLASS  "prototype"
#define JS_TEMP_NAME        ""
#define JS_BLANK_DATA       ""

typedef struct js_var      js_var;
typedef struct js_varlink  js_varlink;
typedef struct js_lex      js_lex;
typedef struct js_tiny     js_tiny;

typedef void (*js_callback)(js_var *var, void *userdata);

/* ---- value + link ---- */
struct js_varlink {
    char        *name;
    js_varlink  *next, *prev;
    js_var      *var;
    int          owned;
};

struct js_var {
    js_varlink  *firstChild, *lastChild;
    int          refs;
    char        *data;          /* string payload (owned) */
    void        *userCustomData;
    long         intData;
    double       doubleData;
    int          flags;
    js_callback  jsCallback;
    void        *jsCallbackUserData;
};

/* ---- lexer ---- */
struct js_lex {
    char   currCh, nextCh;
    int    tk;
    int    tokenStart, tokenEnd, tokenLastEnd;
    char  *tkStr;
    int    tkStrCap;
    char  *data;                /* may be owned or borrowed */
    int    dataStart, dataEnd;
    int    dataOwned;
    int    dataPos;
};

/* ---- interpreter state ---- */
struct js_tiny {
    js_lex      *l;
    js_var     **scopes;
    int          scopes_len, scopes_cap;
    js_var      *root;
    js_var      *stringClass, *objectClass, *arrayClass;
    char       **call_stack;
    int          call_stack_len, call_stack_cap;
    jmp_buf      jb;
    char        *error;
    int          error_cap;
    int          has_error;
};

/* ---- public API ---- */
js_tiny *js_create(void);
void     js_destroy(js_tiny *t);
void     js_execute(js_tiny *t, const char *code);
void     js_evaluate(js_tiny *t, const char *code, char *out, int outlen);
js_var  *js_evaluate_value(js_tiny *t, const char *code);
void     js_add_native(js_tiny *t, const char *funcDesc, js_callback cb, void *userdata);
void     registerFunctions(js_tiny *t);
void     registerMathFunctions(js_tiny *t);
js_var  *js_get_variable(js_tiny *t, const char *path);
int      js_has_error(js_tiny *t);
const char *js_error_msg(js_tiny *t);

/* ---- value constructors / accessors (used by native callbacks) ---- */
js_var *js_var_new_undefined(void);
js_var *js_var_new_int(long v);
js_var *js_var_new_double(double v);
js_var *js_var_new_string(const char *s);
js_var *js_var_new_null(void);
js_var *js_var_new_blank(int flags);
void    js_var_unref(js_var *v);
js_var *js_var_ref(js_var *v);

long    js_var_get_int(js_var *v);
double  js_var_get_double(js_var *v);
int     js_var_get_bool(js_var *v);
void    js_var_get_string(js_var *v, char *buf, int buflen);
int     js_var_is_undefined(js_var *v);

js_var      *js_var_get_parameter(js_var *func, const char *name);
js_var      *js_var_get_return_var(js_var *func);
void         js_var_set_return_var(js_var *func, js_var *val);
js_varlink  *js_var_add_child(js_var *parent, const char *name, js_var *child);
js_varlink  *js_var_add_child_no_dup(js_var *parent, const char *name, js_var *child);
js_var      *js_var_get_child(js_var *parent, const char *name);
js_varlink  *js_var_find_child(js_var *parent, const char *name);
js_varlink  *js_var_find_child_or_create(js_var *parent, const char *name, int flags);
js_varlink  *js_var_find_child_or_create_by_path(js_var *parent, const char *path);
void         js_var_remove_link(js_var *parent, js_varlink *link);
void         js_var_remove_all_children(js_var *v);
void         js_var_set_callback(js_var *v, js_callback cb, void *userdata);

/* C-string rendering of a value (caller js_free's the result). */
char *js_var_to_string(js_var *v);

void    js_var_set_int(js_var *v, long val);
void    js_var_set_double(js_var *v, double val);
void    js_var_set_string(js_var *v, const char *str);
void    js_var_set_undefined(js_var *v);
void    js_var_set_array(js_var *v);
void    js_var_set_data(js_var *v, const char *s);
void    js_var_free(js_var *v);
js_var *js_var_deep_copy(js_var *v);
void     js_var_copy_value(js_var *dst, js_var *val);
js_var  *js_var_get_array_index(js_var *p, int idx);
void     js_var_set_array_index(js_var *p, int idx, js_var *value);

/* ---- lexer API ---- */
void        js_lex_next_token(js_lex *l);
js_lex     *js_lex_create(const char *data);
js_lex     *js_lex_create_sub(js_lex *owner, int start, int len);
void        js_lex_free(js_lex *l);
void        js_lex_reset(js_lex *l);
void        js_lex_match(js_lex *l, int expected);
const char *js_lex_token_str(int tk);
const char *js_lex_token_str_buf(int tk, char *buf);
void        js_lex_position(js_lex *l, char *buf, int len);

/* ---- inline value predicates ---- */
static inline int js_var_is_int(js_var *v)     { return (v->flags & JS_V_INTEGER) != 0; }
static inline int js_var_is_double(js_var *v)  { return (v->flags & JS_V_DOUBLE) != 0; }
static inline int js_var_is_string(js_var *v)  { return (v->flags & JS_V_STRING) != 0; }
static inline int js_var_is_numeric(js_var *v) { return (v->flags & JS_V_NUMERIC) != 0; }
static inline int js_var_is_function(js_var *v){ return (v->flags & JS_V_FUNCTION) != 0; }
static inline int js_var_is_object(js_var *v)  { return (v->flags & JS_V_OBJECT) != 0; }
static inline int js_var_is_array(js_var *v)   { return (v->flags & JS_V_ARRAY) != 0; }
static inline int js_var_is_native(js_var *v)  { return (v->flags & JS_V_NATIVE) != 0; }
static inline int js_var_is_null(js_var *v)    { return (v->flags & JS_V_NULL) != 0; }
static inline int js_var_is_basic(js_var *v)   { return v->firstChild == 0; }

int js_var_get_array_length(js_var *v);
int js_var_get_children(js_var *v);
int js_var_equals(js_var *a, js_var *b);
void js_var_get_json(js_var *v, char *buf, int buflen);

/* ---- link ops (shared with parser) ---- */
js_varlink *js_link_new(js_var *var, const char *name);
void        js_link_free(js_varlink *l);
void        js_link_replace_var(js_varlink *l, js_var *nv);
int         js_link_get_int_name(js_varlink *l);
void        js_link_set_int_name(js_varlink *l, int n);

/* ---- error handling (setjmp/longjmp) ---- */
void js_throw_current(const char *msg);

/* ---- maths ---- */
js_var *js_var_maths_op(js_var *a, js_var *b, int op);

/* ---- link lifecycle macros (mirror TinyJS CLEAN / CREATE_LINK) ---- */
#define JS_CLEAN(x) do { js_varlink *_jl = (x); if (_jl && !_jl->owned) js_link_free(_jl); } while(0)
#define JS_CREATE_LINK(LINK, VAR) do { \
    if (!(LINK) || (LINK)->owned) (LINK) = js_link_new((VAR), JS_TEMP_NAME); \
    else js_link_replace_var((LINK), (VAR)); \
} while(0)

/* ---- kernel glue (js_dom.c): engine lifecycle + <script> runner ---- */
extern js_tiny *g_js_engine;
void js_init(void);
void js_run(const char *code);
void js_dom_run_page(const char *html, int html_len);

#endif /* JS_H */
