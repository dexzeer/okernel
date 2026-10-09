// api.c — the embedding API (ojs.h).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "parse.h"

int atoms_init(ojs* J);
struct ftempl* compile_script(ojs* J, struct parser* P);
jv json_parse(ojs* J, struct str* s, jv reviver);
jv module_compile(ojs* J, struct str* src, const char* name);
jv module_evaluate(ojs* J, jv mv);
struct module_rec* module_find(ojs* J, const char* name);
int module_request_count(jv mv);
jv module_request(jv mv, int i);
struct module_rec* module_of(jv mv);
jv module_meta_object(ojs* J, struct module_rec* m);
void module_finish_dynamic_import(ojs* J, const char* referrer, const char* spec, jv resolve, jv reject);
int run_one_job(ojs* J);
int promise_state(jv promise, jv* result);
uint8_t* typed_bytes(jv v, size_t* len);
jv arraybuffer_copy(ojs* J, const void* data, size_t len);
jv uint8array_copy(ojs* J, const void* data, size_t len);

// ---------------------------------------------------------------- host objects

struct hostobj { struct obj base; void* opaque; int hcls; int pad; };
struct ojs_class_def { char name[32]; };

void api_classes(void) { }

// ---------------------------------------------------------------- runtime

ojs* ojs_new(void) {
    classes_init();
    ojs* J = (ojs*)ojs_sys_malloc(sizeof(ojs));
    if (!J) return 0;
    memset(J, 0, sizeof *J);
    J->exc = JV_UNDEFINED;
    J->native_new_target = JV_UNDEFINED;
    J->sym_registry = J->template_cache = J->global_lex = JV_UNDEFINED;
    J->stack_limit = 256 * 1024;
    gc_init(J);
    if (!J->heap) { ojs_sys_free(J); return 0; }
    J->gc_disabled = 1;
    if (atoms_init(J) < 0 || realm_init(J) < 0) {
        ojs_free(J);
        return 0;
    }
    J->gc_disabled = 0;
    return J;
}

struct vstack_chunk { struct vstack_chunk* prev; jv* base; jv* top; jv* end; };
struct weak_list { void** v; int n, cap; void** it; int nit, capit; };

void ojs_free(ojs* J) {
    if (!J) return;
    while (J->jobs_head) {
        struct job* n = J->jobs_head->next;
        ojs_sys_free(J->jobs_head);
        J->jobs_head = n;
    }
    gc_free_all(J);
    while (J->vchunks) {
        struct vstack_chunk* p = J->vchunks->prev;
        ojs_sys_free(J->vchunks);
        J->vchunks = p;
    }
    for (struct root_range* r = J->roots; r;) {
        struct root_range* n = r->next;
        ojs_sys_free(r);
        r = n;
    }
    if (J->weak_maps) {
        struct weak_list* L = (struct weak_list*)J->weak_maps;
        ojs_sys_free(L->v);
        ojs_sys_free(L->it);
        ojs_sys_free(L);
    }
    ojs_sys_free(J->root_ptrs);
    ojs_sys_free(J->modules);
    ojs_sys_free(J->atoms);
    ojs_sys_free(J->A);
    ojs_sys_free(J->host_classes);
    ojs_sys_free(J->heap);
    ojs_sys_free(J);
}

void ojs_set_memory_limit(ojs* J, size_t bytes) { J->mem_limit = bytes; }
void ojs_set_stack_size(ojs* J, size_t bytes) { J->stack_limit = bytes; }

void ojs_enter_frame(ojs* J, void* frame) {
    if (J->entry_depth++ == 0) J->stack_top = frame;
}

void ojs_leave(ojs* J) {
    if (J->entry_depth > 0 && --J->entry_depth == 0) J->stack_top = 0;
}

void ojs_set_opaque(ojs* J, void* p) { J->opaque = p; }
void* ojs_get_opaque(ojs* J) { return J->opaque; }

void ojs_set_interrupt_handler(ojs* J, int (*fn)(ojs* J, void* op), void* op) {
    J->interrupt = fn;
    J->interrupt_op = op;
    J->interrupt_counter = 4096;
}

void ojs_set_rejection_tracker(ojs* J, void (*fn)(ojs* J, ojsv promise, ojsv reason, int handled, void* op), void* op) {
    J->rejection_tracker = fn;
    J->rejection_op = op;
}

void ojs_gc(ojs* J) { if (J->stack_top) gc_collect(J); }
size_t ojs_heap_bytes(ojs* J) { return J->heap_bytes; }
void ojs_get_stats(ojs* J, struct ojs_stats* st) {
    st->heap_bytes = J->heap_bytes;
    st->live_bytes = J->heap ? gc_live_bytes(J) : 0;
    st->gc_count = J->gc_count;
    st->gc_ms = J->gc_ms;
    st->total_alloc = J->total_alloc;
    for (int i = 0; i < 16; i++) st->live_by_type[i] = J->heap ? gc_live_of_type(J, i + 1) : 0;
}

void ojs_set_gc_stress(ojs* J, uint32_t every) { J->gc_stress = every; J->gc_stress_n = 0; }

void ojs_set_random_seed(ojs* J, uint64_t seed) {
    uint64_t a = seed ^ 0x9E3779B97F4A7C15ull, b = (seed * 0xBF58476D1CE4E5B9ull) ^ 0x94D049BB133111EBull;
    if (!a && !b) a = 1;
    J->random_state[0] = (uint32_t)(a >> 32);
    J->random_state[1] = (uint32_t)a;
    J->random_state[2] = (uint32_t)(b >> 32);
    J->random_state[3] = (uint32_t)b;
}

// ---------------------------------------------------------------- roots

int ojs_add_root(ojs* J, ojsv* slot) {
    if (J->nroot_ptrs >= J->caproot_ptrs) {
        int nc = J->caproot_ptrs ? J->caproot_ptrs * 2 : 64;
        jv** t = (jv**)ojs_sys_realloc(J->root_ptrs, (size_t)nc * sizeof(jv*));
        if (!t) return -1;
        J->root_ptrs = t;
        J->caproot_ptrs = nc;
    }
    J->root_ptrs[J->nroot_ptrs++] = slot;
    return 0;
}

void ojs_remove_root(ojs* J, ojsv* slot) {
    for (int i = J->nroot_ptrs - 1; i >= 0; i--)
        if (J->root_ptrs[i] == slot) {
            J->root_ptrs[i] = J->root_ptrs[--J->nroot_ptrs];
            return;
        }
}

int ojs_add_root_range(ojs* J, ojsv* base, int n) {
    for (struct root_range* r = J->roots; r; r = r->next)
        if (r->base == base) { r->n = n; return 0; }
    struct root_range* r = (struct root_range*)ojs_sys_malloc(sizeof *r);
    if (!r) return -1;
    r->base = base;
    r->n = n;
    r->next = J->roots;
    J->roots = r;
    return 0;
}

void ojs_remove_root_range(ojs* J, ojsv* base) {
    for (struct root_range** pp = &J->roots; *pp; pp = &(*pp)->next)
        if ((*pp)->base == base) {
            struct root_range* r = *pp;
            *pp = r->next;
            ojs_sys_free(r);
            return;
        }
}

// ---------------------------------------------------------------- values

int ojs_is_undefined(ojsv v) { return v == JV_UNDEFINED; }
int ojs_is_null(ojsv v) { return v == JV_NULL; }
int ojs_is_bool(ojsv v) { return jv_is_bool(v); }
int ojs_is_number(ojsv v) { return jv_is_number(v); }
int ojs_is_string(ojsv v) { return jv_is_str(v); }
int ojs_is_object(ojsv v) { return jv_is_obj(v); }
int ojs_is_function(ojs* J, ojsv v) { (void)J; return is_callable(v); }
int ojs_is_exception(ojsv v) { return v == JV_EXC; }
int ojs_is_array(ojs* J, ojsv v) { int r = is_array(J, v); if (r < 0) { take_exc(J); return 0; } return r; }
ojsv ojs_bool(int b) { return jv_bool(b); }
ojsv ojs_int(int32_t i) { return jv_from_int(i); }
ojsv ojs_number(double d) { return jv_number(d); }

ojsv ojs_string(ojs* J, const char* utf8) { return ojs_string_len(J, utf8, strlen(utf8)); }

ojsv ojs_string_len(ojs* J, const char* utf8, size_t len) {
    struct str* s = str_from_utf8(J, utf8, len);
    return s ? jv_from_str(s) : JV_EXC;
}

ojsv ojs_object(ojs* J) { struct obj* o = obj_new_plain(J); return o ? jv_from_obj(o) : JV_EXC; }
ojsv ojs_array(ojs* J) { struct obj* o = obj_new_array(J, 0); return o ? jv_from_obj(o) : JV_EXC; }
ojsv ojs_global(ojs* J) { return jv_from_obj(J->I.global); }

char* ojs_to_cstring(ojs* J, ojsv v, size_t* len) {
    struct str* s = to_str(J, v);
    if (!s) return 0;
    char* r = str_to_utf8(J, s, len);
    if (!r) throw_oom(J);
    return r;
}

void ojs_free_cstring(ojs* J, char* s) { (void)J; ojs_sys_free(s); }

int ojs_to_int32(ojs* J, int32_t* out, ojsv v) { return to_int32(J, v, out); }
int ojs_to_number(ojs* J, double* out, ojsv v) { return to_number_d(J, v, out); }
int ojs_to_bool(ojs* J, ojsv v) { (void)J; return to_boolean(v); }

int ojs_get_number(ojsv v, double* out) {
    if (!jv_is_number(v)) return 0;
    *out = jv_num(v);
    return 1;
}

// ---------------------------------------------------------------- properties

ojsv ojs_get(ojs* J, ojsv obj, const char* name) {
    pkey k = pkey_from_cstr(J, name);
    if (!k) return JV_EXC;
    return obj_get_v(J, obj, k);
}

ojsv ojs_get_index(ojs* J, ojsv obj, uint32_t i) {
    pkey k = i <= 0x7FFFFFFF ? PK_FROM_INDEX(i) : pkey_from_value(J, jv_number(i));
    if (!k) return JV_EXC;
    return obj_get_v(J, obj, k);
}

ojsv ojs_get_value(ojs* J, ojsv obj, ojsv key) {
    pkey k = pkey_from_value(J, key);
    if (!k) return JV_EXC;
    return obj_get_v(J, obj, k);
}

static int set_key(ojs* J, ojsv obj, pkey k, ojsv v) {
    if (!jv_is_obj(obj)) { throw_type(J, "Cannot set a property of a non-object"); return -1; }
    return obj_set(J, jv_obj(obj), k, v, obj, 1) < 0 ? -1 : 0;
}

int ojs_set(ojs* J, ojsv obj, const char* name, ojsv v) {
    pkey k = pkey_from_cstr(J, name);
    if (!k) return -1;
    return set_key(J, obj, k, v);
}

int ojs_set_index(ojs* J, ojsv obj, uint32_t i, ojsv v) {
    pkey k = i <= 0x7FFFFFFF ? PK_FROM_INDEX(i) : pkey_from_value(J, jv_number(i));
    if (!k) return -1;
    return set_key(J, obj, k, v);
}

int ojs_define_hidden(ojs* J, ojsv obj, const char* name, ojsv v) {
    if (!jv_is_obj(obj)) { throw_type(J, "Cannot define a property on a non-object"); return -1; }
    pkey k = pkey_from_cstr(J, name);
    if (!k) return -1;
    return obj_define_value(J, jv_obj(obj), k, v, PA_HIDDEN) < 0 ? -1 : 0;
}

ojsv ojs_own_keys(ojs* J, ojsv obj) {
    jv o = to_object(J, obj);
    if (o == JV_EXC) return JV_EXC;
    return obj_own_keys(J, jv_obj(o), OWNKEYS_STRINGS | OWNKEYS_ENUM_ONLY);
}

// ---------------------------------------------------------------- functions

static jv host_trampoline(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    ojs_cfunc fn = (ojs_cfunc)(uintptr_t)(uint32_t)magic;
    return fn(J, this_v, argc, argv);
}

ojsv ojs_function(ojs* J, ojs_cfunc fn, const char* name, int length) {
    struct obj* f = new_native(J, host_trampoline, name, length, (int)(uint32_t)(uintptr_t)fn);
    return f ? jv_from_obj(f) : JV_EXC;
}

int ojs_set_functions(ojs* J, ojsv obj, const struct ojs_func_entry* list, int n) {
    if (!jv_is_obj(obj)) return -1;
    for (int i = 0; i < n; i++) {
        jv f = ojs_function(J, list[i].fn, list[i].name, list[i].length);
        if (f == JV_EXC) return -1;
        if (ojs_define_hidden(J, obj, list[i].name, f) < 0) return -1;
    }
    return 0;
}

ojsv ojs_call(ojs* J, ojsv fn, ojsv this_v, int argc, ojsv* argv) {
    if (!is_callable(fn)) return throw_type(J, "not a function");
    return vm_call(J, fn, this_v, argc, argv, JV_UNDEFINED);
}

ojsv ojs_new_instance(ojs* J, ojsv ctor, int argc, ojsv* argv) { return ojs_construct_v(J, ctor, argc, argv, ctor); }

// ---------------------------------------------------------------- host objects

int ojs_host_class(ojs* J, const char* name) {
    struct ojs_class_def* t = (struct ojs_class_def*)ojs_sys_realloc(J->host_classes, sizeof(struct ojs_class_def) * (size_t)(J->nhost_classes + 1));
    if (!t) return -1;
    J->host_classes = t;
    ojs_snprintf(t[J->nhost_classes].name, 32, "%s", name);
    return ++J->nhost_classes;
}

ojsv ojs_host_object(ojs* J, int cls, ojsv proto, void* opaque) {
    struct obj* p = jv_is_obj(proto) ? jv_obj(proto) : jv_is_null(proto) ? 0 : J->I.object_proto;
    struct hostobj* h = (struct hostobj*)obj_new(J, p, OC_HOST, sizeof(struct hostobj));
    if (!h) return JV_EXC;
    h->opaque = opaque;
    h->hcls = cls;
    return jv_from_obj(&h->base);
}

void* ojs_host_opaque(ojs* J, ojsv v, int cls) {
    (void)J;
    if (!jv_is_obj(v) || obj_class(jv_obj(v)) != OC_HOST) return 0;
    struct hostobj* h = (struct hostobj*)jv_obj(v);
    return h->hcls == cls ? h->opaque : 0;
}

// ---------------------------------------------------------------- errors

#define FMT_THROW(fn, ne) \
    ojsv fn(ojs* J, const char* fmt, ...) { \
        char buf[512]; va_list ap; va_start(ap, fmt); \
        int n = ojs_vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap); \
        if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1; \
        struct str* s = str_from_utf8(J, buf, (size_t)n); \
        if (!s) return JV_EXC; \
        jv e = err_new(J, ne, jv_from_str(s)); \
        if (e == JV_EXC) return JV_EXC; \
        return ojs_throw(J, e); \
    }
FMT_THROW(ojs_throw_type_error, NE_TYPE)
FMT_THROW(ojs_throw_range_error, NE_RANGE)
FMT_THROW(ojs_throw_reference_error, NE_REFERENCE)
FMT_THROW(ojs_throw_syntax_error, NE_SYNTAX)

ojsv ojs_throw_oom(ojs* J) { return throw_oom(J); }
ojsv ojs_take_exception(ojs* J) { J->uncatchable = 0; return take_exc(J); }
int ojs_has_exception(ojs* J) { return J->has_exc; }

// ---------------------------------------------------------------- binary data

ojsv ojs_arraybuffer_copy(ojs* J, const void* data, size_t len) { return arraybuffer_copy(J, data, len); }
ojsv ojs_uint8array_copy(ojs* J, const void* data, size_t len) { return uint8array_copy(J, data, len); }
uint8_t* ojs_bytes(ojs* J, ojsv v, size_t* len) { (void)J; size_t l = 0; uint8_t* p = typed_bytes(v, &l); if (len) *len = l; return p; }

// ---------------------------------------------------------------- scripts

ojsv ojs_eval(ojs* J, const char* src, size_t len, const char* filename, int flags) {
    struct str* s = str_from_utf8(J, src, len);
    if (!s) return JV_EXC;
    if ((flags & 0xF) == OJS_EVAL_MODULE) {
        jv m = module_compile(J, s, filename ? filename : "<module>");
        if (m == JV_EXC || (flags & OJS_EVAL_COMPILE_ONLY)) return m;
        jv p = module_evaluate(J, m);
        if (p == JV_EXC) return JV_EXC;
        run_jobs(J);
        return p;
    }
    struct parser P;
    struct parse_opts o;
    memset(&o, 0, sizeof o);
    o.kind = PARSE_SCRIPT;
    o.strict = (flags & OJS_EVAL_STRICT) != 0;
    o.filename = filename ? filename : "<script>";
    if (parse_program(J, s, &o, &P) < 0) { parse_free(&P); return JV_EXC; }
    struct ftempl* t = compile_script(J, &P);
    parse_free(&P);
    if (!t) return JV_EXC;
    srctext_compact(J, t, 0, s);   // big UTF-16 sources live on as UTF-8
#ifndef KERNEL
    {
        extern char* getenv(const char*);
        if (getenv("OJS_DUMP")) ftempl_dump(J, t);
    }
#endif
    if (flags & OJS_EVAL_COMPILE_ONLY) return jv_from_ptr(t);
    return vm_run_script(J, t, jv_from_obj(J->I.global));
}

ojsv ojs_run_compiled(ojs* J, ojsv compiled) {
    if (JV_TAG(compiled) != TAG_PTR) return throw_type(J, "not a compiled script or module");
    struct gch* g = (struct gch*)JV_PTR(compiled);
    if (g->type == GT_FTEMPL) return vm_run_script(J, (struct ftempl*)g, jv_from_obj(J->I.global));
    if (g->type == GT_MODULE) return module_evaluate(J, compiled);
    return throw_type(J, "not a compiled script or module");
}

ojsv ojs_parse_json(ojs* J, const char* src, size_t len) {
    struct str* s = str_from_utf8(J, src, len);
    if (!s) return JV_EXC;
    return json_parse(J, s, JV_UNDEFINED);
}

int ojs_run_job(ojs* J) { return run_one_job(J); }

int ojs_promise_state(ojs* J, ojsv p, ojsv* result) {
    (void)J;
    if (!is_promise(p)) return -1;
    return promise_state(p, result);
}

// ---------------------------------------------------------------- modules

void ojs_set_module_hooks(ojs* J, const struct ojs_module_hooks* hooks) {
    if (!J->modhooks) {
        J->modhooks = (struct ojs_module_hooks*)ojs_sys_malloc(sizeof(struct ojs_module_hooks));
        if (!J->modhooks) return;
    }
    *J->modhooks = *hooks;
}

ojsv ojs_find_module(ojs* J, const char* name) {
    struct module_rec* m = module_find(J, name);
    return m ? jv_from_ptr(m) : JV_UNDEFINED;
}

int ojs_module_request_count(ojs* J, ojsv module) { (void)J; return module_request_count(module); }

char* ojs_module_request(ojs* J, ojsv module, int i) {
    struct str* s = str_flat(J, module_request(module, i));
    return s ? str_to_utf8(J, s, 0) : 0;
}

ojsv ojs_module_meta(ojs* J, ojsv module) { return module_meta_object(J, module_of(module)); }

void ojs_finish_dynamic_import(ojs* J, const char* referrer, const char* spec, ojsv resolve, ojsv reject) {
    module_finish_dynamic_import(J, referrer, spec, resolve, reject);
}

// ---------------------------------------------------------------- diagnostics

char* ojs_describe(ojs* J, ojsv v) {
    jv saved = J->exc;
    int had = J->has_exc;
    J->has_exc = 0;
    jv s = JV_UNDEFINED;
    if (jv_is_obj(v) && (jv_obj(v)->flags & OF_IS_ERROR)) {
        s = get_v_str(J, v, "stack");
        if (!jv_is_str(s)) s = JV_UNDEFINED;
    }
    if (jv_is_undef(s)) {
        jv symbol_descriptive_string(ojs* J, jv s);
        s = jv_is_sym(v) ? symbol_descriptive_string(J, v) : to_string(J, v);
    }
    char* r = 0;
    if (jv_is_str(s)) {
        struct str* f = str_flat(J, s);
        if (f) r = str_to_utf8(J, f, 0);
    }
    if (!r) {
        take_exc(J);
        const char* fb = "<unprintable value>";
        r = (char*)ojs_sys_malloc(strlen(fb) + 1);
        if (r) memcpy(r, fb, strlen(fb) + 1);
    }
    J->exc = saved;
    J->has_exc = had;
    return r;
}

void ojs_memory_usage(ojs* J, struct ojs_mem_usage* u) {
    memset(u, 0, sizeof *u);
    u->heap_bytes = J->heap_bytes;
}
