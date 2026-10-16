#ifndef WEB_WASM_H
#define WEB_WASM_H

// WebAssembly core: binary decoder + validator + compiler to an internal code stream, and an interpreter.
// Written from the WebAssembly 2.0 core specification (MVP + sign extension, non-trapping float-to-int, multi-value,
// bulk memory, reference types, multiple tables). No SIMD, threads/atomics, exceptions, tail calls, GC, memory64.
//
// The core knows nothing about JavaScript: imports are resolved by the embedder (struct wasm_store holds every function,
// table, memory and global a module may touch), host functions are called through `host`, and linear memory belongs to
// the embedder (the JS API keeps it in an ArrayBuffer).

#include <stdint.h>
#include <stddef.h>

enum { WT_I32 = 0x7F, WT_I64 = 0x7E, WT_F32 = 0x7D, WT_F64 = 0x7C, WT_FUNCREF = 0x70, WT_EXTERNREF = 0x6F };
enum { WK_FUNC = 0, WK_TABLE = 1, WK_MEM = 2, WK_GLOBAL = 3 };

typedef union { uint32_t u32; int32_t s32; uint64_t u64; int64_t s64; float f32; double f64; void* ref; } wval;

enum {
    WTRAP_NONE = 0, WTRAP_UNREACHABLE, WTRAP_MEM_OOB, WTRAP_DIV0, WTRAP_OVERFLOW, WTRAP_INVALID_CONV, WTRAP_NULL_ELEM,
    WTRAP_SIG_MISMATCH, WTRAP_TABLE_OOB, WTRAP_STACK, WTRAP_BULK_OOB, WTRAP_HOST /* an exception is pending in the embedder */,
    WTRAP_INTERRUPTED, WTRAP_UNDEF_ELEM, WTRAP_NULL_REF
};
const char* wasm_trap_name(int trap);

struct wasm_type { uint8_t np, nr; uint8_t* p; uint8_t* r; };

struct wasm_code {
    uint32_t* code; uint32_t ncode;
    uint32_t nparams, nresults, nlocals;     // nlocals excludes the parameters
    uint32_t max_stack;                      // operand slots needed above the locals
};

struct wasm_limits { uint32_t min, max; int has_max; };
struct wasm_tabtype { uint8_t type; struct wasm_limits lim; };

struct wasm_const { uint8_t kind; uint64_t v; };      // kind: 0 i32, 1 i64, 2 f32, 3 f64, 4 global.get, 5 ref.null, 6 ref.func

struct wasm_import { char* module; char* name; uint8_t kind; uint32_t type;      // func: type index
                     struct wasm_tabtype table; struct wasm_limits mem; uint8_t gtype, gmut; };
struct wasm_export { char* name; uint8_t kind; uint32_t idx; };
struct wasm_global_def { uint8_t type, mut; struct wasm_const init; };
struct wasm_elem { uint8_t mode; uint32_t table; struct wasm_const offset; uint8_t type; uint32_t n; struct wasm_const* items; };   // mode 0 active, 1 passive, 2 declarative
struct wasm_data { uint8_t mode; uint32_t mem; struct wasm_const offset; uint8_t* bytes; uint32_t len; };
struct wasm_custom { char* name; uint8_t* bytes; uint32_t len; };

struct wasm_module {
    struct wasm_type* types; uint32_t ntypes;
    struct wasm_import* imports; uint32_t nimports;
    uint32_t* func_types; uint32_t nfuncs;           // type index of every DEFINED function
    struct wasm_code* codes;                         // one per defined function
    struct wasm_tabtype* tables; uint32_t ntables;   // defined tables
    struct wasm_limits* mems; uint32_t nmems;        // defined memories
    struct wasm_global_def* globals; uint32_t nglobals;
    struct wasm_export* exports; uint32_t nexports;
    int has_start; uint32_t start;
    struct wasm_elem* elems; uint32_t nelems;
    struct wasm_data* datas; uint32_t ndatas;
    struct wasm_custom* customs; uint32_t ncustoms;
    // imported counts per kind (the index spaces start with the imports)
    uint32_t imp_funcs, imp_tables, imp_mems, imp_globals;
    int* declared_funcs;                             // functions that may be named by ref.func (bitmap-ish list), -1 terminated
    uint32_t ndeclared;
};

struct wasm_inst;
struct wasm_fn {          // a function in the store: defined by an instance, or provided by the host
    struct wasm_inst* inst; uint32_t idx;            // defined function: index among the module's defined functions
    const struct wasm_code* code;
    int host;                                        // host function id (>= 0) or -1
    struct wasm_type* type; int own_type;            // type (host functions own theirs)
    void* user;                                      // embedder cookie (the JS function object id)
};
struct wasm_mem { uint8_t* data; uint32_t len; uint32_t pages; uint32_t max_pages; int has_max; void* user; };
struct wasm_tab { void** el; uint32_t n; uint32_t max; int has_max; uint8_t type; void* user; };
struct wasm_glob { wval v; uint8_t type, mut; void* user; };

struct wasm_inst {
    struct wasm_module* mod;
    struct wasm_fn** funcs; struct wasm_tab** tabs; struct wasm_mem** mems; struct wasm_glob** globs;
    uint8_t* data_dropped; uint8_t* elem_dropped;
    struct wasm_fn** own_funcs;                      // functions this instance defines
};

struct wasm_store;
struct wasm_host {
    // call a host function; returns 0, or WTRAP_HOST when an exception is pending in the embedder
    int (*call)(void* ud, struct wasm_fn* fn, const wval* args, wval* results);
    // called every so often while wasm runs: nonzero aborts it (time budget); also a place to yield
    int (*poll)(void* ud);
    // linear memory growth: the embedder allocates; returns 0 and updates m->data/len/pages, or -1
    int (*mem_grow)(void* ud, struct wasm_mem* m, uint32_t new_pages);
    void* ud;
};

struct wasm_store {
    struct wasm_host host;
    // everything allocated through the store is released by wasm_store_free
    void** objs; uint32_t nobjs, capobjs; uint8_t* objkind;
    wval* stack; uint32_t stack_cap;                 // operand stack shared by every call
    int depth;                                       // nested wasm_call (host -> wasm re-entry)
    uint32_t stack_used, frames_used;                // tops left by an outer call that is inside a host function
};

struct wasm_store* wasm_store_new(const struct wasm_host* host);
void wasm_store_free(struct wasm_store* s);

// decode + validate + compile; 0 ok (module in *out), else an error text in err
int wasm_module_new(struct wasm_store* s, const uint8_t* bytes, size_t len, struct wasm_module** out, char* err, int errcap);

// store objects (all owned by the store)
struct wasm_mem* wasm_mem_new(struct wasm_store* s, uint32_t pages, uint32_t max_pages, int has_max);
struct wasm_tab* wasm_tab_new(struct wasm_store* s, uint8_t type, uint32_t n, uint32_t max, int has_max);
struct wasm_glob* wasm_glob_new(struct wasm_store* s, uint8_t type, int mut, wval v);
struct wasm_fn* wasm_host_fn_new(struct wasm_store* s, const uint8_t* params, int np, const uint8_t* results, int nr, int host_id, void* user);

// instantiate: imports[] in import order (funcs: wasm_fn*, tables: wasm_tab*, mems: wasm_mem*, globals: wasm_glob*), all already
// type-checked by the embedder through wasm_import_ok. Returns 0, a link error (-1, text in err) or a trap code (> 0).
struct wasm_inst* wasm_instantiate(struct wasm_store* s, struct wasm_module* m, void** imports, int* trap, char* err, int errcap);
// check one resolved import against what the module declared: 0 ok, else a message
const char* wasm_import_ok(struct wasm_module* m, uint32_t import_idx, void* obj);
int wasm_func_type_eq(const struct wasm_type* a, const struct wasm_type* b);

// call: args/results are wval arrays sized by the function type; returns a trap code (0 = ok)
int wasm_call(struct wasm_store* s, struct wasm_fn* fn, const wval* args, wval* results);
int wasm_mem_grow(struct wasm_store* s, struct wasm_mem* m, uint32_t delta);   // old page count or -1
int wasm_tab_grow(struct wasm_store* s, struct wasm_tab* t, uint32_t delta, void* init);   // old length or -1

#endif
