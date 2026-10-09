// builtins.h — interfaces between the VM and the built-in objects.
#ifndef OJS_BUILTINS_H
#define OJS_BUILTINS_H

#include "ojs_int.h"

struct ojs_frame;
struct func;

// object flag: an eval var object (declarative: `this` is undefined in calls)
#define OF_EVALVARS 0x0800

// iterator record (internal object, class OC_ITER)
struct iterrec {
    struct obj base;
    jv iter;
    jv next;
    int done;
};
jv   iter_get(ojs* J, jv v, int async);                 // iterator record (object value)
jv   iter_step_value(ojs* J, struct iterrec* r);        // value, JV_HOLE when done, JV_EXC
int  iter_close(ojs* J, struct iterrec* r, int quiet);  // quiet: completion is a throw (ignore errors)
jv   iter_to_list(ojs* J, jv iterable);                 // JS array of the iterated values
jv   create_iter_result(ojs* J, jv value, int done);

// functions
jv   bound_call(ojs* J, struct obj* b, jv this_v, int argc, jv* argv, jv new_target);
jv   bound_target(struct obj* b);
jv   proxy_call(ojs* J, struct obj* p, jv this_v, int argc, jv* argv, jv new_target);
int  proxy_is_array(ojs* J, struct obj* p);
jv   wrap_primitive(ojs* J, jv v);
int  bigint_is_zero(jv v);

// arguments objects
jv   make_arguments(ojs* J, struct ojs_frame* f, int mapped);

// for-in
jv   for_in_start(ojs* J, jv v);
jv   for_in_next(ojs* J, jv it);                        // key string, JV_HOLE when done, JV_EXC

// regexp
jv   regexp_create(ojs* J, jv pattern, jv flags);

// promises / jobs
struct promise_cap { jv promise, resolve, reject; };
int  new_promise_capability(ojs* J, jv ctor, struct promise_cap* cap);
jv   promise_resolve(ojs* J, jv ctor, jv value);         // PromiseResolve
int  perform_promise_then(ojs* J, jv promise, jv on_ful, jv on_rej, struct promise_cap* result);
jv   promise_new_internal(ojs* J);                       // a pending %Promise%
int  is_promise(jv v);
int  instance_of(ojs* J, jv o, jv c);
int  ordinary_has_instance(ojs* J, jv c, jv o);

// errors
jv   err_new(ojs* J, int ne, jv message);
void err_capture_stack(ojs* J, struct obj* e);

// global environment
jv   global_get(ojs* J, struct str* name, int typeof_mode);
int  global_put(ojs* J, struct str* name, jv v, int strict);
int  global_check_decls(ojs* J, struct obj* names);
int  global_decl_var(ojs* J, struct str* name, int deletable);
int  global_decl_func(ojs* J, struct str* name, jv fn, int deletable);
int  global_decl_lex(ojs* J, struct str* name, int is_const);
int  global_init_lex(ojs* J, struct str* name, jv v);
jv   global_delete(ojs* J, struct str* name);

// eval
jv   direct_eval(ojs* J, struct ojs_frame* caller, jv src, jv env);
jv   indirect_eval(ojs* J, jv src);

// modules
jv   module_import_meta(ojs* J, struct ojs_frame* f);
jv   module_dynamic_import(ojs* J, struct ojs_frame* f, jv spec, jv options);

// generators / async (generator.c)
jv   gen_create(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target);
jv   async_start(ojs* J, struct func* fn, jv this_v, int argc, jv* argv, jv new_target);

// ---------------------------------------------------------------- definition helpers (realm.c)

// a table of native methods / accessors. Names "@@iterator" etc. are
// well-known symbols (function name "[Symbol.iterator]").
struct bdef {
    const char* name;
    native_fn fn;
    int16_t len;
    int16_t magic;
    uint8_t kind;               // BK_*
};
enum { BK_METHOD, BK_GETTER, BK_SETTER };
#define FN(n, f, l, m) { n, f, l, m, BK_METHOD }
#define GETTER(n, f, m) { n, f, 0, m, BK_GETTER }
#define SETTER(n, f, m) { n, f, 1, m, BK_SETTER }
int  def_fns(ojs* J, struct obj* o, const struct bdef* d, int n);
#define DEF_FNS(o, tab) def_fns(J, o, tab, (int)(sizeof(tab) / sizeof((tab)[0])))
pkey bname_key(ojs* J, const char* name);
int  def_value(ojs* J, struct obj* o, const char* name, jv v, int attrs);
int  def_global(ojs* J, const char* name, jv v);
// constructor `name` (prototype object `proto`, may be NULL), bound on the global object
struct obj* def_ctor(ojs* J, native_fn fn, const char* name, int len, int magic, struct obj* proto);
struct obj* this_class(ojs* J, jv t, int cls, const char* what);   // NULL + TypeError
jv   str_value(ojs* J, const char* s);                               // string value of a C literal (atom)

// primitive wrapper objects (Boolean, Number, String, Symbol, BigInt)
struct prim { struct obj base; jv v; };

// bound function exotic object
struct bound {
    struct obj base;
    jv target;
    jv this_v;
    jv* args;                   // valarr
    uint32_t nargs;
};

// realm parts (each b_*.c)
int  b_object_init(ojs* J);
int  b_function_init(ojs* J);
int  b_error_init(ojs* J);
int  b_iter_init(ojs* J);
int  b_array_init(ojs* J);
int  b_string_init(ojs* J);
int  b_number_init(ojs* J);
int  b_symbol_init(ojs* J);
int  b_math_init(ojs* J);
int  b_global_init(ojs* J);
int  b_promise_init(ojs* J);
int  b_generator_init(ojs* J);
int  b_json_init(ojs* J);
int  b_date_init(ojs* J);
int  b_regexp_init(ojs* J);
int  b_map_init(ojs* J);
int  b_proxy_init(ojs* J);
int  b_typed_init(ojs* J);
int  b_bigint_init(ojs* J);
void classes_init(void);
int  realm_init(ojs* J);

#endif
