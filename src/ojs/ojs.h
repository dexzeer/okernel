// ojs.h — embedding API of ojs, okernel's JavaScript engine.
//
// One `ojs` is one runtime with one realm (global object + intrinsics).
// Values (ojsv) are plain 64-bit words: copy them freely, no reference
// counting. The collector scans the C stack, so values in local variables
// stay alive; values kept in C heap memory must be registered with
// ojs_add_root / ojs_add_root_range.
//
// Errors follow one convention: a function returning ojsv returns
// OJS_EXCEPTION when a JS exception is pending (take it with
// ojs_take_exception); int-returning functions return -1.
//
// Every entry from the host (eval, call, job) must happen between
// ojs_enter() and ojs_leave() on the same C stack; nested entries (a
// native calling back into JS) need not repeat it.

#ifndef OJS_H
#define OJS_H

#include <stdint.h>
#include <stddef.h>

typedef struct ojs ojs;
typedef uint64_t ojsv;

#define OJS_UNDEFINED ((ojsv)0xFFFA000000000000ull)
#define OJS_NULL      ((ojsv)0xFFFA000000000001ull)
#define OJS_FALSE     ((ojsv)0xFFFA000000000002ull)
#define OJS_TRUE      ((ojsv)0xFFFA000000000003ull)
#define OJS_EXCEPTION ((ojsv)0xFFFA000000000005ull)

// ---- runtime
ojs*  ojs_new(void);
void  ojs_free(ojs* J);
void  ojs_set_memory_limit(ojs* J, size_t bytes);
// called before the realm takes more memory from the system (heap chunks,
// large objects, ArrayBuffer storage); returning 0 refuses it (collect, then OOM)
void  ojs_set_memory_guard(ojs* J, int (*guard)(size_t bytes, void* ud), void* ud);
void  ojs_set_stack_size(ojs* J, size_t bytes);       // C stack the engine may use per entry
// ojs_enter marks the C stack top for this entry: the frame of the
// calling function, so values in its locals are scanned by the collector
void  ojs_enter_frame(ojs* J, void* frame);
#define ojs_enter(J) ojs_enter_frame((J), __builtin_frame_address(0))
void  ojs_leave(ojs* J);
void  ojs_set_opaque(ojs* J, void* p);
void* ojs_get_opaque(ojs* J);
// interrupt: called periodically while JS runs; nonzero aborts with an
// uncatchable "interrupted" error
void  ojs_set_interrupt_handler(ojs* J, int (*fn)(ojs* J, void* op), void* op);
// unhandled promise rejections (handled = 1 when a handler is attached later)
void  ojs_set_rejection_tracker(ojs* J, void (*fn)(ojs* J, ojsv promise, ojsv reason, int handled, void* op), void* op);
void  ojs_gc(ojs* J);
size_t ojs_heap_bytes(ojs* J);
struct ojs_stats {
    size_t heap_bytes, live_bytes;
    int gc_count;
    double gc_ms;
    unsigned long long total_alloc;
    size_t live_by_type[16];    // after the last GC: str rope sym bigint obj shape ftempl upval valarr bytes ptrarr maptab module frame
};
#define OJS_STATS_TYPES "str rope sym bigint obj shape ftempl upval valarr bytes ptrarr maptab module frame"
void   ojs_get_stats(ojs* J, struct ojs_stats* st);
int    ojs_where(ojs* J, char* out, int cap);           // innermost JS function "name file:line:col" (profiling)   // heap_bytes: pages + large blocks held; live: after the last GC

// ---- roots (values stored outside the C stack)
int   ojs_add_root(ojs* J, ojsv* slot);
void  ojs_remove_root(ojs* J, ojsv* slot);
int   ojs_add_root_range(ojs* J, ojsv* base, int n);   // re-register after a realloc
void  ojs_remove_root_range(ojs* J, ojsv* base);

// ---- values
int   ojs_is_undefined(ojsv v);
int   ojs_is_null(ojsv v);
int   ojs_is_bool(ojsv v);
int   ojs_is_number(ojsv v);
int   ojs_is_string(ojsv v);
int   ojs_is_object(ojsv v);
int   ojs_is_function(ojs* J, ojsv v);
int   ojs_is_exception(ojsv v);
int   ojs_is_array(ojs* J, ojsv v);
ojsv  ojs_bool(int b);
ojsv  ojs_int(int32_t i);
ojsv  ojs_number(double d);
ojsv  ojs_string(ojs* J, const char* utf8);
ojsv  ojs_string_len(ojs* J, const char* utf8, size_t len);
ojsv  ojs_object(ojs* J);
ojsv  ojs_array(ojs* J);
ojsv  ojs_global(ojs* J);

// conversions (exceptions possible: ToString/ToNumber run user code)
char* ojs_to_cstring(ojs* J, ojsv v, size_t* len);     // UTF-8, NUL-terminated; free with ojs_free_cstring
void  ojs_free_cstring(ojs* J, char* s);
int   ojs_to_int32(ojs* J, int32_t* out, ojsv v);       // 0 ok, -1 exception
int   ojs_to_number(ojs* J, double* out, ojsv v);
int   ojs_to_bool(ojs* J, ojsv v);
int   ojs_get_number(ojsv v, double* out);              // no conversion: 1 if a number

// ---- properties
ojsv  ojs_get(ojs* J, ojsv obj, const char* name);
ojsv  ojs_get_index(ojs* J, ojsv obj, uint32_t i);
ojsv  ojs_get_value(ojs* J, ojsv obj, ojsv key);
int   ojs_set(ojs* J, ojsv obj, const char* name, ojsv v);        // 0 ok, -1 exception
int   ojs_set_index(ojs* J, ojsv obj, uint32_t i, ojsv v);
int   ojs_define_hidden(ojs* J, ojsv obj, const char* name, ojsv v);   // writable, configurable, not enumerable
// own enumerable string keys, as a JS array of strings
ojsv  ojs_own_keys(ojs* J, ojsv obj);

// ---- functions
typedef ojsv (*ojs_cfunc)(ojs* J, ojsv this_v, int argc, ojsv* argv);
ojsv  ojs_function(ojs* J, ojs_cfunc fn, const char* name, int length);
struct ojs_func_entry { const char* name; ojs_cfunc fn; int length; };
int   ojs_set_functions(ojs* J, ojsv obj, const struct ojs_func_entry* list, int n);
ojsv  ojs_call(ojs* J, ojsv fn, ojsv this_v, int argc, ojsv* argv);
ojsv  ojs_new_instance(ojs* J, ojsv ctor, int argc, ojsv* argv);

// ---- host objects: an opaque pointer + a host class id (>0)
int   ojs_host_class(ojs* J, const char* name);           // returns a class id
ojsv  ojs_host_object(ojs* J, int cls, ojsv proto, void* opaque);
void* ojs_host_opaque(ojs* J, ojsv v, int cls);           // NULL if not that class

// ---- errors
ojsv  ojs_throw(ojs* J, ojsv v);
ojsv  ojs_throw_type_error(ojs* J, const char* fmt, ...);
ojsv  ojs_throw_range_error(ojs* J, const char* fmt, ...);
ojsv  ojs_throw_reference_error(ojs* J, const char* fmt, ...);
ojsv  ojs_throw_syntax_error(ojs* J, const char* fmt, ...);
ojsv  ojs_throw_oom(ojs* J);
ojsv  ojs_take_exception(ojs* J);                         // the pending exception (cleared)
int   ojs_has_exception(ojs* J);

// ---- binary data
ojsv  ojs_arraybuffer_copy(ojs* J, const void* data, size_t len);
ojsv  ojs_uint8array_copy(ojs* J, const void* data, size_t len);
// bytes viewed by an ArrayBuffer / typed array / DataView (NULL if none or detached)
uint8_t* ojs_bytes(ojs* J, ojsv v, size_t* len);

// ---- scripts
#define OJS_EVAL_SCRIPT       0
#define OJS_EVAL_MODULE       1
#define OJS_EVAL_COMPILE_ONLY 0x10   // return the compiled script/module, do not run it
#define OJS_EVAL_STRICT       0x20
ojsv  ojs_eval(ojs* J, const char* src, size_t len, const char* filename, int flags);
ojsv  ojs_run_compiled(ojs* J, ojsv compiled);   // compiled script: run; module: link + evaluate
ojsv  ojs_parse_json(ojs* J, const char* src, size_t len);
// microtasks: 1 ran one job, 0 queue empty, -1 the job threw (exception pending)
int   ojs_run_job(ojs* J);

// ---- modules
// The host maps specifiers to URLs and supplies source text. A module
// compiled with ojs_eval(..., OJS_EVAL_MODULE | OJS_EVAL_COMPILE_ONLY) is
// registered under `filename`; its static requests are discoverable before
// linking, so the host can fetch the whole graph first.
struct ojs_module_hooks {
    // resolve `spec` imported by module `referrer` to a module name (URL):
    // sys-malloc'd string, or NULL with an exception pending
    char* (*resolve)(ojs* J, const char* referrer, const char* spec, void* op);
    // load the module `name` (call ojs_eval with COMPILE_ONLY | MODULE):
    // the compiled module, or OJS_EXCEPTION
    ojsv  (*load)(ojs* J, const char* name, void* op);
    // import(): return 1 to take over (the host later calls
    // ojs_finish_dynamic_import with the promise functions), 0 to load now
    int   (*dynamic_import)(ojs* J, const char* referrer, const char* spec, ojsv resolve, ojsv reject, void* op);
    void* op;
};
void  ojs_set_module_hooks(ojs* J, const struct ojs_module_hooks* hooks);
ojsv  ojs_find_module(ojs* J, const char* name);          // compiled module or OJS_UNDEFINED
int   ojs_module_request_count(ojs* J, ojsv module);
char* ojs_module_request(ojs* J, ojsv module, int i);    // sys-malloc'd specifier
ojsv  ojs_module_meta(ojs* J, ojsv module);               // import.meta object
void  ojs_finish_dynamic_import(ojs* J, const char* referrer, const char* spec, ojsv resolve, ojsv reject);

// promise inspection: 0 pending, 1 fulfilled, 2 rejected, -1 not a promise
int   ojs_promise_state(ojs* J, ojsv p, ojsv* result);
void  ojs_set_random_seed(ojs* J, uint64_t seed);
void  ojs_set_gc_stress(ojs* J, uint32_t every);    // testing: a full collection every N allocations
// a printable description of a value (errors: "Name: message" + stack), sys-malloc'd UTF-8
char* ojs_describe(ojs* J, ojsv v);

// ---- diagnostics
struct ojs_mem_usage { size_t heap_bytes, objects, strings, functions; };
void  ojs_memory_usage(ojs* J, struct ojs_mem_usage* u);

#endif
