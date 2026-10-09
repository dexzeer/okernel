// ojs_int.h — internals of ojs, okernel's JavaScript engine (written from
// scratch to the ECMAScript specification).
//
// Layout of the engine:
//   value.h-style encoding ........ here (jv: NaN-boxed 64-bit values)
//   gc.c ............................ page heap, mark-sweep, conservative C-stack scan
//   str.c ........................... strings (Latin-1 / UTF-16, ropes), atoms
//   obj.c ........................... shapes, objects, property operations
//   num.c ........................... number <-> string (shortest round trip, exact)
//   lex.c / parse.c ................. tokens -> AST
//   compile.c ....................... AST -> bytecode (scopes, closures, TDZ)
//   vm.c ............................ interpreter, calls, exceptions, generators
//   b_*.c ........................... built-in objects
//   re.c ............................ regular expressions (backtracking VM)
//   api.c ........................... the embedding API (ojs.h)
//
// Memory model: a tracing, non-moving mark-sweep collector. Engine and
// host C code may keep values in local variables freely: the collector
// scans the C stack conservatively (any word that points into a heap
// object keeps it alive). Values stored in C heap memory must be
// registered as roots (ojs_add_root). Heap objects reference each other
// through ordinary pointers and are traced precisely.

#ifndef OJS_INT_H
#define OJS_INT_H

#include "ojs.h"
#include "ojs_sys.h"

// ---------------------------------------------------------------- values
//
// jv is a 64-bit NaN-boxed value. Any bit pattern whose top 16 bits are
// below 0xFFF9 is an IEEE double (NaNs are kept canonical: 0x7FF8.. or the
// x87's 0xFFF8..). Tagged values use the top 16 bits:
//   FFF9 int32 (low 32 bits)       FFFA special (undefined, null, bools, hole)
//   FFFB object*   FFFC string*   FFFD symbol*   FFFE bigint*   FFFF internal*

typedef uint64_t jv;

#define TAG_INT     0xFFF9u
#define TAG_SPECIAL 0xFFFAu
#define TAG_OBJ     0xFFFBu
#define TAG_STR     0xFFFCu
#define TAG_SYM     0xFFFDu
#define TAG_BIG     0xFFFEu
#define TAG_PTR     0xFFFFu

#define JV_TAG(v)        ((uint32_t)((v) >> 48))
#define JV_MAKE(tag, lo) (((uint64_t)(tag) << 48) | (uint32_t)(lo))
#define JV_LO(v)         ((uint32_t)(v))
#define JV_PTR(v)        ((void*)(uintptr_t)(uint32_t)(v))

#define JV_UNDEFINED JV_MAKE(TAG_SPECIAL, 0)
#define JV_NULL      JV_MAKE(TAG_SPECIAL, 1)
#define JV_FALSE     JV_MAKE(TAG_SPECIAL, 2)
#define JV_TRUE      JV_MAKE(TAG_SPECIAL, 3)
#define JV_HOLE      JV_MAKE(TAG_SPECIAL, 4)   // array hole / uninitialized binding (TDZ)
#define JV_EXC       JV_MAKE(TAG_SPECIAL, 5)   // "an exception is pending" (never a JS value)
#define JV_NAN_BITS  0x7FF8000000000000ull

static inline int jv_is_num(jv v) { return JV_TAG(v) < TAG_INT; }   // double
static inline int jv_is_int(jv v) { return JV_TAG(v) == TAG_INT; }
static inline int jv_is_number(jv v) { return JV_TAG(v) <= TAG_INT; }
static inline int jv_is_obj(jv v) { return JV_TAG(v) == TAG_OBJ; }
static inline int jv_is_str(jv v) { return JV_TAG(v) == TAG_STR; }
static inline int jv_is_sym(jv v) { return JV_TAG(v) == TAG_SYM; }
static inline int jv_is_big(jv v) { return JV_TAG(v) == TAG_BIG; }
static inline int jv_is_undef(jv v) { return v == JV_UNDEFINED; }
static inline int jv_is_null(jv v) { return v == JV_NULL; }
static inline int jv_is_nullish(jv v) { return v == JV_UNDEFINED || v == JV_NULL; }
static inline int jv_is_bool(jv v) { return v == JV_TRUE || v == JV_FALSE; }
static inline int jv_is_exc(jv v) { return v == JV_EXC; }
static inline int32_t jv_int(jv v) { return (int32_t)JV_LO(v); }
static inline jv jv_from_int(int32_t i) { return JV_MAKE(TAG_INT, (uint32_t)i); }
static inline jv jv_bool(int b) { return b ? JV_TRUE : JV_FALSE; }
static inline double jv_dbl(jv v) { union { uint64_t u; double d; } x; x.u = v; return x.d; }
static inline jv jv_from_dbl_raw(double d) { union { uint64_t u; double d; } x; x.d = d; return x.u; }

// double -> integer truncation toward zero from the IEEE bits: on the x87 a C cast
// switches the rounding mode twice (fldcw), which dominated hot paths. d must be
// finite with |d| < 2^63 (NaN / out of range give 0).
static inline int64_t d2i64(double d) {
    uint64_t u = jv_from_dbl_raw(d);
    int e = (int)((u >> 52) & 0x7FF) - 1075;
    if (e <= -53 || e >= 11) return 0;
    uint64_t m = (u & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    uint64_t r = e >= 0 ? m << e : m >> -e;
    return (u >> 63) ? -(int64_t)r : (int64_t)r;
}
static inline uint32_t d2u32(double d) { return (uint32_t)d2i64(d); }   // 0 <= d < 2^32
// any double -> jv (canonicalizes NaN so it never collides with tags)
static inline jv jv_from_dbl(double d) {
    jv v = jv_from_dbl_raw(d);
    if ((v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0x000FFFFFFFFFFFFFull)) return JV_NAN_BITS;
    return v;
}
// number value as double (int or double)
static inline double jv_num(jv v) { return jv_is_int(v) ? (double)jv_int(v) : jv_dbl(v); }
// a number, as int32 when it is one exactly (and not -0)
jv jv_number(double d);

struct obj; struct str; struct sym; struct bigint;
static inline struct obj* jv_obj(jv v) { return (struct obj*)JV_PTR(v); }
static inline struct str* jv_str(jv v) { return (struct str*)JV_PTR(v); }
static inline struct sym* jv_sym(jv v) { return (struct sym*)JV_PTR(v); }
static inline struct bigint* jv_bigv(jv v) { return (struct bigint*)JV_PTR(v); }
static inline jv jv_from_obj(struct obj* o) { return JV_MAKE(TAG_OBJ, (uintptr_t)o); }
static inline jv jv_from_str(struct str* s) { return JV_MAKE(TAG_STR, (uintptr_t)s); }
static inline jv jv_from_sym(struct sym* s) { return JV_MAKE(TAG_SYM, (uintptr_t)s); }
static inline jv jv_from_big(struct bigint* b) { return JV_MAKE(TAG_BIG, (uintptr_t)b); }
static inline jv jv_from_ptr(void* p) { return JV_MAKE(TAG_PTR, (uintptr_t)p); }

// ---------------------------------------------------------------- GC heap
//
// Every collected object starts with a struct gch. The page allocator
// (gc.c) knows each object's size from its page; large objects carry it
// in a side table.

enum gc_type {
    GT_FREE = 0,
    GT_STR, GT_ROPE, GT_SYM, GT_BIGINT,
    GT_OBJ,          // every JS object (class in obj->cls)
    GT_SHAPE,
    GT_FTEMPL,       // compiled function (bytecode + constants)
    GT_UPVAL,        // captured variable cell
    GT_VALARR,       // a gc-managed array of jv (slots, elements, frames)
    GT_BYTES,        // opaque bytes (no pointers)
    GT_PTRARR,       // array of gc pointers
    GT_MAPTAB,       // Map/Set hash table storage
    GT_MODULE,       // module record
    GT_FRAME,        // heap frame of a suspended generator / async function
    GT_COUNT
};

#define GCF_MARK   0x01
#define GCF_PINNED 0x02     // never collected (intrinsic tables)
#define GCF_FINAL  0x04     // has a finalizer (sweep calls obj_finalize)

struct gch {
    uint8_t type;
    uint8_t gcflags;
    uint16_t aux;           // per-type (object class id, string flags, ...)
    uint32_t aux32;         // per-type (string length, ...)
};

struct ojs;   // runtime + realm (one per page)
typedef struct ojs ojs;

void* gc_alloc(ojs* J, int type, size_t size);          // zeroed; NULL + OOM exception on failure
void  gc_collect(ojs* J);
size_t gc_size(const void* p);                           // allocation size of a gc object
void  gc_mark_value(ojs* J, jv v);
void  gc_mark_ptr(ojs* J, void* p);
void  gc_init(ojs* J);
void  gc_free_all(ojs* J);
void  gc_write_barrier(ojs* J, void* holder);            // (non-incremental: no-op)

// arrays of values owned by an object (realloc-able, gc-managed)
struct valarr { struct gch h; uint32_t cap; uint32_t pad; jv v[]; };
jv*   valarr_new(ojs* J, uint32_t cap);                  // returns ->v, zero-filled with UNDEFINED
jv*   valarr_grow(ojs* J, jv* old, uint32_t oldcap, uint32_t newcap, jv fill);
static inline struct valarr* valarr_of(jv* v) { return (struct valarr*)((char*)v - offsetof(struct valarr, v)); }

// ---------------------------------------------------------------- strings
//
// Flat strings: Latin-1 (8-bit units) or UTF-16 (16-bit units), length in
// h.aux32. Ropes (concatenations) are flattened on first content access;
// the flat result is cached in the rope.

#define SF_WIDE     0x0001   // 16-bit code units
#define SF_ATOM     0x0002   // interned (unique per content)
#define SF_HASHED   0x0004   // hash field valid
#define SF_INDEX    0x0008   // atom is a canonical array index (index in hash)
#define SF_NOTINDEX 0x0010   // atom checked: not an index

struct str {
    struct gch h;           // h.aux = SF_*, h.aux32 = length
    uint32_t hash;
    uint32_t pad;
    union { uint8_t c8[1]; uint16_t c16[1]; } u;
};

struct rope {
    struct gch h;           // h.aux32 = length
    jv left, right;         // strings or ropes (as values: TAG_STR)
    struct str* flat;       // cached flattening
    uint32_t depth;
};

static inline uint32_t str_len(const struct str* s) { return s->h.aux32; }
static inline int str_wide(const struct str* s) { return (s->h.aux & SF_WIDE) != 0; }
static inline uint32_t str_at(const struct str* s, uint32_t i) { return str_wide(s) ? s->u.c16[i] : s->u.c8[i]; }

// a value with TAG_STR may point at a struct str or a struct rope:
// str_flat() returns the flat string (flattening a rope; NULL on OOM)
struct str* str_flat(ojs* J, jv s);
static inline uint32_t jstr_len(jv s) { return ((struct gch*)JV_PTR(s))->aux32; }

struct str* str_new8(ojs* J, const uint8_t* s, uint32_t len);       // Latin-1 units
struct str* str_new16(ojs* J, const uint16_t* s, uint32_t len);     // narrowed if possible
struct str* str_alloc(ojs* J, uint32_t len, int wide);              // uninitialized
struct str* str_from_utf8(ojs* J, const char* s, size_t len);
struct str* str_from_cstr(ojs* J, const char* s);
jv   jstr_concat(ojs* J, jv a, jv b);                    // strings in, string out (JV_EXC on OOM)
jv   jstr_sub(ojs* J, jv s, uint32_t start, uint32_t end);
int  str_eq(const struct str* a, const struct str* b);
int  str_cmp(const struct str* a, const struct str* b);  // code-unit order
uint32_t str_hash(struct str* s);
int  str_eq_ascii(const struct str* a, const char* lit);
char* str_to_utf8(ojs* J, const struct str* s, size_t* len); // sys-malloc'd, NUL-terminated
size_t str_utf8_len(const struct str* s);
size_t str_to_utf8_buf(const struct str* s, char* out, size_t cap); // returns bytes needed
int32_t str_index_of(const struct str* hay, const struct str* needle, int32_t from);
int32_t str_last_index_of(const struct str* hay, const struct str* needle, int32_t from);

// string builder (growable; produces a flat string)
struct sbuf {
    ojs* J;
    uint8_t* b8;
    uint16_t* b16;
    uint32_t len, cap;
    int wide, oom;
};
void sb_init(ojs* J, struct sbuf* b);
void sb_putc(struct sbuf* b, uint32_t cu);            // one UTF-16 code unit
void sb_put_cp(struct sbuf* b, uint32_t cp);          // code point (surrogate pair if needed)
void sb_puts(struct sbuf* b, const char* ascii);
void sb_put_str(struct sbuf* b, const struct str* s);
void sb_put_sub(struct sbuf* b, const struct str* s, uint32_t from, uint32_t to);
void sb_put_utf8(struct sbuf* b, const char* s, size_t n);
jv   sb_done(struct sbuf* b);                         // string value or JV_EXC
void sb_free(struct sbuf* b);

// atoms: interned strings, used as property keys. Integer keys below 2^31
// that are canonical array indices are not strings at all (see pkey).
struct str* atom_str(ojs* J, struct str* s);           // intern (may return s)
struct str* atom_cstr(ojs* J, const char* s);          // intern ASCII/UTF-8 literal
void atoms_sweep(ojs* J);

// ---------------------------------------------------------------- symbols

struct sym {
    struct gch h;
    jv desc;                // string or undefined
    uint32_t registered;    // Symbol.for key (1) / well-known (2) / private name (3)
    uint32_t hash;
};

// ---------------------------------------------------------------- property keys
//
// pkey (32 bits): (index << 1) | 1 for array indices 0 .. 2^31-1, otherwise
// a pointer (8-aligned, low bit 0) to an atom string or a symbol.

typedef uint32_t pkey;
#define PK_IS_INDEX(k) ((k) & 1u)
#define PK_INDEX(k)    ((uint32_t)(k) >> 1)
#define PK_FROM_INDEX(i) (((uint32_t)(i) << 1) | 1u)
#define PK_NONE        0u
static inline int pk_is_sym(pkey k) { return !PK_IS_INDEX(k) && k && ((struct gch*)(uintptr_t)k)->type == GT_SYM; }
static inline int pk_is_str(pkey k) { return !PK_IS_INDEX(k) && k && ((struct gch*)(uintptr_t)k)->type == GT_STR; }
static inline struct str* pk_str(pkey k) { return (struct str*)(uintptr_t)k; }
static inline struct sym* pk_sym(pkey k) { return (struct sym*)(uintptr_t)k; }
static inline pkey pk_from_atom(struct str* a) { return (pkey)(uintptr_t)a; }
static inline pkey pk_from_sym(struct sym* s) { return (pkey)(uintptr_t)s; }

pkey pkey_from_str(ojs* J, struct str* s);             // canonical index or atom (0 on OOM)
pkey pkey_from_cstr(ojs* J, const char* s);
pkey pkey_from_value(ojs* J, jv v);                    // ToPropertyKey (0 + exception)
jv   pkey_to_value(ojs* J, pkey k);                    // string / symbol value
jv   pkey_to_string(ojs* J, pkey k);                   // string (symbols: description form)
void gc_mark_pkey(ojs* J, pkey k);

// ---------------------------------------------------------------- shapes & objects

// property attributes
#define PA_WRITABLE     0x01
#define PA_ENUMERABLE   0x02
#define PA_CONFIGURABLE 0x04
#define PA_ACCESSOR     0x08   // slot holds an accessor pair object (getter/setter)
#define PA_DEFAULT      (PA_WRITABLE | PA_ENUMERABLE | PA_CONFIGURABLE)
#define PA_HIDDEN       (PA_WRITABLE | PA_CONFIGURABLE)   // methods of built-ins

struct prop { pkey key; uint32_t attrs; };

struct shape {
    struct gch h;
    struct shape* parent;       // transition parent (NULL: root)
    struct prop* props;         // nprops entries, insertion order (gc bytes)
    uint32_t nprops;
    uint32_t pcap;              // capacity of props (dictionary shapes grow in place)
    uint32_t hsize;             // hash index size (0: linear search)
    int32_t* hidx;              // open-addressing index into props (gc bytes)
    // transitions: (key, attrs) -> child shape
    struct shape** trans;       // gc ptrarr
    pkey* trans_key;
    uint32_t* trans_attrs;
    uint32_t ntrans, captrans;
    uint8_t dict;               // owned by one object (not shared, mutable)
};

// object classes
enum {
    OC_OBJECT = 1, OC_ARRAY, OC_FUNCTION, OC_NATIVE, OC_BOUND, OC_ERROR, OC_BOOLEAN, OC_NUMBER,
    OC_STRING, OC_SYMBOL, OC_BIGINT, OC_DATE, OC_REGEXP, OC_ARGUMENTS, OC_MAPPED_ARGS,
    OC_MAP, OC_SET, OC_WEAKMAP, OC_WEAKSET, OC_WEAKREF, OC_FINREG,
    OC_PROMISE, OC_PROXY, OC_ARRAYBUFFER, OC_SHAREDARRAYBUFFER, OC_TYPEDARRAY, OC_DATAVIEW,
    OC_GENERATOR, OC_ASYNC_GENERATOR, OC_ASYNC_FROM_SYNC_ITER, OC_ITER, OC_MODULE_NS,
    OC_ACCESSOR,        // internal: getter/setter pair stored in a slot
    OC_HOST,            // embedder object (opaque pointer + host class id)
    OC_FOR_IN,          // internal: for-in enumerator state
    OC_ARRAY_ITER, OC_STRING_ITER, OC_MAP_ITER, OC_SET_ITER, OC_REGEXP_STR_ITER, OC_ITER_HELPER,
    OC_WRAP_ITER, OC_PROMISE_FN, OC_DISPOSABLE_STACK, OC_INTERNAL, OC_ASYNC_FN, OC_RAW_JSON,
    OC_COUNT
};

// object flags
#define OF_EXTENSIBLE   0x0001
#define OF_CALLABLE     0x0002
#define OF_CONSTRUCTOR  0x0004
#define OF_ARRAY_FAST   0x0008   // elems[] holds all indexed properties (dense or with holes)
#define OF_LEN_RO       0x0010   // array "length" non-writable
#define OF_EXOTIC       0x0020   // has class ops (proxy, typed array, string, args, ...)
#define OF_HTMLDDA      0x0040   // (unused; document.all style)
#define OF_FROZEN_ELEMS 0x0080   // elements frozen (fast path writes refuse)
#define OF_SEALED_ELEMS 0x0100
#define OF_CLASS_CTOR   0x0200   // class constructor (not callable without new)
#define OF_IS_ERROR     0x0400
#define OF_INDEX_KEYS   0x1000   // an array-index key was ever stored in the shape (slow elements)

struct obj {
    struct gch h;               // h.aux = class (OC_*)
    uint32_t flags;
    struct shape* shape;
    jv* slots;                  // values per shape prop (valarr)
    struct obj* proto;
    jv* elems;                  // fast elements (valarr) when OF_ARRAY_FAST
    uint32_t elen;              // number of element slots in use (array: <= length)
    uint32_t ecap;
    uint32_t alen;              // array length (OC_ARRAY), arguments length, ...
    uint32_t pad;
    // class-specific data follows in larger allocations
};

static inline int obj_class(const struct obj* o) { return o->h.aux; }
static inline int obj_is_callable(const struct obj* o) { return (o->flags & OF_CALLABLE) != 0; }

// internal record: n traced values (never exposed to scripts)
struct irec { struct obj base; uint32_t n; uint32_t pad; jv v[]; };
struct irec* irec_new(ojs* J, uint32_t n);

// accessor pair (stored in a slot when PA_ACCESSOR)
struct accessor { struct obj base; jv get, set; };

// ---------------------------------------------------------------- runtime

struct ojs_frame;
struct ftempl;

typedef jv (*native_fn)(ojs* J, jv this_v, int argc, jv* argv, int magic);

// realm intrinsics (traced as roots)
struct intrinsics {
    struct obj *global, *object_proto, *function_proto, *array_proto, *string_proto, *number_proto,
        *boolean_proto, *symbol_proto, *bigint_proto, *error_proto, *iterator_proto,
        *array_iter_proto, *string_iter_proto, *map_iter_proto, *set_iter_proto, *regexp_str_iter_proto,
        *generator_proto, *generator_fn_proto, *async_fn_proto, *async_gen_proto, *async_gen_fn_proto,
        *async_iterator_proto, *async_from_sync_iter_proto, *promise_proto, *promise_ctor, *regexp_proto,
        *date_proto, *map_proto, *set_proto, *weakmap_proto, *weakset_proto, *weakref_proto, *finreg_proto,
        *arraybuffer_proto, *sharedarraybuffer_proto, *dataview_proto, *typedarray_proto, *typedarray_ctor,
        *ta_proto[12], *ta_ctor[12], *proxy_ctor, *array_ctor, *object_ctor, *function_ctor,
        *error_ctor, *throw_type_error, *eval_fn, *iterator_helper_proto, *wrap_for_valid_iter_proto,
        *array_proto_values, *promise_resolve_fn, *promise_then, *iterator_proto_ctor, *map_ctor, *set_ctor,
        *weakmap_ctor, *weakset_ctor, *symbol_ctor, *string_ctor, *number_ctor, *boolean_ctor, *bigint_ctor,
        *regexp_ctor, *date_ctor, *arraybuffer_ctor, *dataview_ctor, *array_proto_to_string, *object_proto_to_string,
        *generator_fn_ctor, *async_fn_ctor, *async_gen_fn_ctor, *regexp_exec, *regexp_cache, *legacy_re, *sharedarraybuffer_ctor,
        *args_tmpl, *mapped_args_tmpl,   // property layout of arguments objects (length, @@iterator, callee)
        *match_tmpl,                     // ... and of RegExp match arrays (index, input, groups)
        *regexp_proto_snap;              // RegExp.prototype's slot values at realm start (see regexp_pristine)
    struct obj *native_error_proto[8], *native_error_ctor[8];
    struct shape *empty_shape, *array_shape, *func_shape, *arrow_shape, *method_shape, *ctor_shape,
        *gen_shape, *regexp_shape, *error_shape, *regexp_proto_shape0;
};

enum { NE_EVAL, NE_RANGE, NE_REFERENCE, NE_SYNTAX, NE_TYPE, NE_URI, NE_AGGREGATE, NE_COUNT };

// well-known symbols
enum { WK_ASYNC_ITERATOR, WK_HAS_INSTANCE, WK_IS_CONCAT_SPREADABLE, WK_ITERATOR, WK_MATCH, WK_MATCH_ALL,
       WK_REPLACE, WK_SEARCH, WK_SPECIES, WK_SPLIT, WK_TO_PRIMITIVE, WK_TO_STRING_TAG, WK_UNSCOPABLES, WK_COUNT };

struct job {
    native_fn cfn;              // native job (promise reactions), or NULL: call fn
    jv fn;
    int argc;
    jv argv[3];
    struct job* next;
};

struct root_range { jv* base; int n; struct root_range* next; };

struct ojs {
    // heap
    struct gc_heap* heap;
    size_t mem_limit;
    size_t bytes_since_gc, gc_threshold, heap_bytes;
    int gc_disabled;            // during init / sensitive sections
    uint32_t gc_stress, gc_stress_n;   // testing: collect every gc_stress allocations
    int in_gc;
    // conservative stack scan bounds
    void* stack_top;            // highest address of the C stack region in use (set on entry)
    size_t stack_limit;         // bytes of C stack the engine may use below stack_top
    int entry_depth;
    // exceptions
    jv exc;                     // pending exception (valid while some call returned JV_EXC)
    int has_exc;
    int uncatchable;            // interrupt: exception that try/catch cannot stop
    int oom;                    // the pending exception is an out-of-memory condition
    // atoms
    struct str** atoms;
    uint32_t natoms, atom_cap;
    // common atoms (pre-interned names)
    struct atoms_common* A;
    struct sym* wk[WK_COUNT];
    // symbol registry (Symbol.for)
    jv sym_registry;            // Map-like object (string -> symbol)
    struct intrinsics I;
    // execution
    struct ojs_frame* frame;    // innermost JS frame
    int call_depth;
    jv* vstack;                 // VM value stack chunk (see vm.c)
    jv* vsp; jv* vend;
    struct vstack_chunk* vchunks;
    // jobs (promise reactions, microtasks)
    struct job *jobs_head, *jobs_tail;
    // host
    void* opaque;
    int (*interrupt)(ojs* J, void* op);
    void* interrupt_op;
    uint32_t interrupt_counter;
    void (*rejection_tracker)(ojs* J, jv promise, jv reason, int handled, void* op);
    void* rejection_op;
    struct ojs_module_hooks* modhooks;
    // roots
    struct root_range* roots;
    jv* root_slots;             // registered single slots (array of pointers)
    jv** root_ptrs;
    int nroot_ptrs, caproot_ptrs;
    // modules
    struct module_rec** modules;
    int nmodules, capmodules;
    // misc caches
    struct obj* last_regexp_cache;
    uint32_t random_state[4];
    struct weak_list* weak_maps;    // WeakMap/WeakSet tables (ephemeron processing)
    struct obj* weak_refs;          // list of WeakRef objects
    struct obj* fin_regs;           // FinalizationRegistry objects
    int gc_count;
    double gc_ms;                // time spent collecting (wall clock)
    uint64_t total_alloc;
    // host classes
    struct ojs_class_def* host_classes;
    int nhost_classes;
    // template objects (tagged templates), by site
    jv template_cache;
    jv global_lex;                  // global lexical environment (let/const/class at script top level)
    jv native_new_target;           // new.target of the running native function
    struct obj* native_callee;      // the running native function object (closure data)
    struct obj* join_stack[32];     // Array.prototype.join / toString cycle detection
    int join_depth;
};

// exceptions
jv  ojs_throw(ojs* J, jv v);                           // always returns JV_EXC
jv  throw_error(ojs* J, int ne, const char* fmt, ...); // NE_* (Error constructor family)
jv  throw_type(ojs* J, const char* fmt, ...);
jv  throw_not_callable(ojs* J, jv v, int ctor);   // "a.b is not a function" (callee text from the source)
jv  throw_range(ojs* J, const char* fmt, ...);
jv  throw_ref(ojs* J, const char* fmt, ...);
jv  throw_syntax(ojs* J, const char* fmt, ...);
jv  throw_oom(ojs* J);
jv  throw_stack_overflow(ojs* J);
jv  take_exc(ojs* J);                                  // fetch + clear pending exception
int check_stack(ojs* J);                               // 0 = ok, -1 = overflow thrown

// ---------------------------------------------------------------- conversions (conv.c)
jv   to_primitive(ojs* J, jv v, int hint);             // hint: 0 default, 1 number, 2 string
jv   to_string(ojs* J, jv v);                          // string value or JV_EXC
struct str* to_str(ojs* J, jv v);                      // flat string or NULL (exception)
jv   to_number(ojs* J, jv v);                          // number value or JV_EXC
jv   to_numeric(ojs* J, jv v);                         // number or bigint or JV_EXC
int  to_number_d(ojs* J, jv v, double* out);           // 0 ok, -1 exception
int  to_int32(ojs* J, jv v, int32_t* out);
int  to_uint32(ojs* J, jv v, uint32_t* out);
int  to_integer_or_inf(ojs* J, jv v, double* out);
int  to_length(ojs* J, jv v, int64_t* out);
int  to_index(ojs* J, jv v, uint64_t* out);
jv   to_object(ojs* J, jv v);
int  to_boolean(jv v);
int  same_value(ojs* J, jv a, jv b);
int  same_value_zero(ojs* J, jv a, jv b);
int  strict_equals(ojs* J, jv a, jv b);
int  loose_equals(ojs* J, jv a, jv b);                 // 0/1, -1 exception
int32_t dtoi32(double d);
uint32_t dtou32(double d);
jv   typeof_value(ojs* J, jv v);
int  is_array(ojs* J, jv v);                           // IsArray (proxies): 0/1/-1
int  is_callable(jv v);
int  is_constructor(jv v);
int  is_regexp(ojs* J, jv v);                          // IsRegExp: 0/1/-1
double num_from_str(const struct str* s, int* ok);     // StringToNumber
jv   num_to_string(ojs* J, double d, int radix);
int  num_to_cstr(double d, char* buf);                 // shortest round-trip form (ECMAScript)

// ---------------------------------------------------------------- objects (obj.c)

// property descriptor (fields present per `has`)
struct pdesc {
    uint32_t has;           // PD_* bits present
    uint32_t attrs;         // PA_* values for the bits present
    jv value, get, set;
};
#define PD_VALUE 0x01
#define PD_WRITABLE 0x02
#define PD_GET 0x04
#define PD_SET 0x08
#define PD_ENUMERABLE 0x10
#define PD_CONFIGURABLE 0x20
#define PD_IS_ACCESSOR(d) (((d)->has & (PD_GET | PD_SET)) != 0)
#define PD_IS_DATA(d) (((d)->has & (PD_VALUE | PD_WRITABLE)) != 0)

// internal methods of exotic objects (NULL = ordinary behaviour)
struct class_ops {
    int  (*get_own)(ojs* J, struct obj* o, pkey k, struct pdesc* d);          // 1/0/-1
    int  (*define_own)(ojs* J, struct obj* o, pkey k, const struct pdesc* d);  // 1/0 (rejected)/-1
    int  (*has)(ojs* J, struct obj* o, pkey k);
    jv   (*get)(ojs* J, struct obj* o, pkey k, jv receiver);
    int  (*set)(ojs* J, struct obj* o, pkey k, jv v, jv receiver);           // 1/0/-1
    int  (*del)(ojs* J, struct obj* o, pkey k);                               // 1/0/-1
    jv   (*own_keys)(ojs* J, struct obj* o);                                  // JS array of keys
    struct obj* (*get_proto)(ojs* J, struct obj* o, int* err);
    int  (*set_proto)(ojs* J, struct obj* o, struct obj* proto);
    int  (*is_extensible)(ojs* J, struct obj* o);
    int  (*prevent_ext)(ojs* J, struct obj* o);
    void (*trace)(ojs* J, struct obj* o);                                     // class-specific fields
    void (*finalize)(ojs* J, struct obj* o);
};
extern struct class_ops class_ops[OC_COUNT];

// ordinary versions (exotics delegate to them)
int  ord_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d);
int  ord_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d);
int  ord_has(ojs* J, struct obj* o, pkey k);
jv   ord_get(ojs* J, struct obj* o, pkey k, jv receiver);
jv   ordinary_get(ojs* J, struct obj* o, pkey k, jv receiver);   // OrdinaryGet (no exotic [[Get]] hook)
int  ord_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver);
int  ord_del(ojs* J, struct obj* o, pkey k);
jv   ord_own_keys(ojs* J, struct obj* o);
int  validate_and_apply(ojs* J, struct obj* o, pkey k, int extensible, const struct pdesc* d, const struct pdesc* cur);
int  to_property_descriptor(ojs* J, jv v, struct pdesc* d);
jv   from_property_descriptor(ojs* J, const struct pdesc* d);
void complete_property_descriptor(struct pdesc* d);
struct obj* obj_new(ojs* J, struct obj* proto, int cls, size_t size);   // size 0 = sizeof(struct obj)
struct obj* obj_new_plain(ojs* J);                    // {} with Object.prototype
struct obj* obj_new_array(ojs* J, uint32_t len);
struct obj* array_from_values(ojs* J, jv* v, uint32_t n);
int  obj_elems_reserve(ojs* J, struct obj* o, uint32_t n);
int  obj_append_raw(ojs* J, struct obj* o, pkey k, uint32_t attrs, jv v);
int  shape_find(const struct shape* s, pkey k);
jv   obj_get(ojs* J, struct obj* o, pkey k, jv receiver);
jv   obj_get_v(ojs* J, jv target, pkey k);            // GetV (primitives use their prototype)
int  obj_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver, int throw_on_fail);   // 1 ok, 0 refused, -1 exc
int  obj_has(ojs* J, struct obj* o, pkey k);            // 0/1/-1
int  obj_delete(ojs* J, struct obj* o, pkey k, int throw_on_fail);   // 1/0/-1
int  obj_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d);    // 1 found, 0 absent, -1 exc
int  obj_define(ojs* J, struct obj* o, pkey k, const struct pdesc* d, int throw_on_fail);   // 1/0/-1
int  obj_define_value(ojs* J, struct obj* o, pkey k, jv v, int attrs);   // simple data property (fast)
int  obj_define_accessor(ojs* J, struct obj* o, pkey k, jv getter, jv setter, int attrs);
jv   obj_own_keys(ojs* J, struct obj* o, int flags);    // array of keys (OWNKEYS_*)
int  obj_prevent_extensions(ojs* J, struct obj* o);     // 1/0/-1
int  obj_is_extensible(ojs* J, struct obj* o);          // 1/0/-1
struct obj* obj_get_proto(ojs* J, struct obj* o, int* err);   // NULL + *err for exception
int  obj_set_proto(ojs* J, struct obj* o, struct obj* proto); // 1/0/-1
jv   get_method(ojs* J, jv v, pkey k);                 // undefined if absent; exception if not callable
int  create_data_property(ojs* J, struct obj* o, pkey k, jv v);   // 1/0/-1
int  create_data_property_or_throw(ojs* J, struct obj* o, pkey k, jv v);
int  set_integrity(ojs* J, struct obj* o, int frozen);  // 1/0/-1
int  test_integrity(ojs* J, struct obj* o, int frozen); // 1/0/-1
jv   get_v_str(ojs* J, jv target, const char* name);   // convenience (atom lookup)
int  set_str(ojs* J, struct obj* o, const char* name, jv v);
int  length_of_array_like(ojs* J, struct obj* o, int64_t* out);
jv   species_constructor(ojs* J, struct obj* o, jv def);
jv   ordinary_create_from_ctor(ojs* J, jv new_target, struct obj* default_proto_intr, int cls, size_t size);
struct obj* get_proto_from_ctor(ojs* J, jv new_target, struct obj* fallback);

#define OWNKEYS_STRINGS 1
#define OWNKEYS_SYMBOLS 2
#define OWNKEYS_ENUM_ONLY 4
#define OWNKEYS_ALL (OWNKEYS_STRINGS | OWNKEYS_SYMBOLS)


// ---------------------------------------------------------------- functions & calls (vm.c)
jv   ojs_call_v(ojs* J, jv fn, jv this_v, int argc, jv* argv);
jv   ojs_construct_v(ojs* J, jv fn, int argc, jv* argv, jv new_target);
jv   invoke(ojs* J, jv v, pkey method, int argc, jv* argv);
struct obj* new_native(ojs* J, native_fn fn, const char* name, int length, int magic);
struct obj* new_native_ctor(ojs* J, native_fn fn, const char* name, int length, int magic, struct obj* proto);
int  set_fn_name(ojs* J, struct obj* f, jv name, const char* prefix);
int  set_fn_length(ojs* J, struct obj* f, double len);
void run_jobs(ojs* J);
void enqueue_job(ojs* J, jv fn, int argc, jv* argv);

#endif
