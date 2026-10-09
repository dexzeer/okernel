// vm.h — bytecode, function templates, closures and frames.
//
// The VM is a stack machine. Each function has local slots (parameters,
// variables, temporaries) and an operand stack; both live in one frame
// block (VM value stack for normal calls, a heap block for generators and
// async functions, which can suspend). Captured variables use Lua-style
// upvalues: a closure refers to a struct upval that points at the frame
// slot while the frame is live ("open") and holds the value after the
// slot's scope ends ("closed").
#ifndef OJS_VM_H
#define OJS_VM_H

#include "ojs_int.h"

// operand formats: N none, B u8, H u16, W u32/i32 (jumps relative to the
// next instruction), A atom / const (u32 const index), C const (u16), HH two u16,
// AH atom + u16 (inline cache), HB u16 + u8, HA u16 + const, AB const + u8,
// AA two consts, AW atom + jump. A function can have more than 65536 constants
// (the wrapper function of a big bundle): CONST takes a u16 index and CONST_W
// a u32 one; every other const operand is u32.
#define OPS(X) \
    X(NOP, N) X(UNDEF, N) X(NULL_, N) X(TRUE_, N) X(FALSE_, N) X(HOLE, N) X(INT8, B) X(INT32, W) \
    X(CONST, C) X(DUP, N) X(DUP2, N) X(POP, N) X(SWAP, N) X(ROT3L, N) X(ROT3R, N) X(ROT4R, N) \
    X(PICK, B) X(NIP, N) X(INSERT, B) \
    X(GET_LOC, H) X(SET_LOC, H) X(PUT_LOC, H) X(GET_LOC_CHK, HA) X(PUT_LOC_CHK, HA) X(INIT_LOC, H) \
    X(GET_UPV, H) X(SET_UPV, H) X(PUT_UPV, H) X(GET_UPV_CHK, HA) X(PUT_UPV_CHK, HA) X(INIT_UPV, H) \
    X(CLOSE, H) X(HOLE_LOC, H) X(BIND_THIS_LOC, H) X(BIND_THIS_UPV, H) \
    X(GET_GLOBAL, AH) X(PUT_GLOBAL, AH) X(PUT_GLOBAL_STRICT, AH) X(TYPEOF_GLOBAL, AH) X(INIT_GLOBAL_LEX, A) \
    X(DECL_GLOBAL_VAR, A) X(DECL_GLOBAL_FUNC, A) X(DECL_GLOBAL_LEX, AH) X(CHECK_GLOBAL_DECLS, A) \
    X(DELETE_GLOBAL, A) X(HAS_GLOBAL, A) \
    X(GET_FIELD, AH) X(GET_FIELD_KEEP, AH) X(PUT_FIELD, AH) X(GET_ELEM, N) X(GET_ELEM_KEEP, N) \
    X(PUT_ELEM, N) X(GET_LENGTH, N) X(DELETE_FIELD, A) X(DELETE_ELEM, N) X(TO_PROPKEY, N) \
    X(GET_SUPER, N) X(PUT_SUPER, N) X(GET_SUPER_KEEP, N) X(SUPER_CTOR, N) \
    X(GET_PRIVATE, N) X(PUT_PRIVATE, N) X(DEFINE_PRIVATE, N) X(PRIVATE_IN, N) X(ADD_PRIVATE_METHOD, B) \
    X(ADD, N) X(SUB, N) X(MUL, N) X(DIV, N) X(MOD, N) X(POW, N) X(NEG, N) X(PLUS, N) X(BITNOT, N) \
    X(NOT, N) X(INC, N) X(DEC, N) X(TO_NUMERIC, N) X(SHL, N) X(SAR, N) X(SHR, N) X(BAND, N) X(BOR, N) \
    X(BXOR, N) X(EQ, N) X(NE, N) X(SEQ, N) X(SNE, N) X(LT, N) X(LE, N) X(GT, N) X(GE, N) X(IN, N) \
    X(INSTANCEOF, N) X(TYPEOF, N) X(IS_UNDEF, N) X(IS_NULLISH, N) X(TO_STRING, N) X(TO_OBJECT, N) \
    X(JMP, W) X(JF, W) X(JT, W) X(JF_KEEP, W) X(JT_KEEP, W) X(JNULLISH, W) X(JNOTNULLISH_KEEP, W) \
    X(JUNDEF, W) \
    X(CALL, H) X(CALL_METHOD, H) X(NEW, H) X(CALL_SPREAD, N) X(CALL_METHOD_SPREAD, N) X(NEW_SPREAD, N) \
    X(EVAL, HA) X(EVAL_SPREAD, A) X(SUPER_CALL, H) X(SUPER_CALL_SPREAD, N) X(CALL_IGNORED_REF_ERROR, N) \
    X(RETURN, N) X(RETURN_UNDEF, N) X(THROW, N) X(THROW_ERR, AB) X(CHECK_CTOR_RETURN, N) \
    X(OBJECT, N) X(ARRAY, H) X(ARRAY_HOLES, H) X(APPEND, N) X(APPEND_ONE, N) X(APPEND_HOLE, N) \
    X(DEFINE_FIELD, A) X(DEFINE_ELEM, N) X(DEFINE_METHOD, B) X(DEFINE_METHOD_ELEM, B) X(SET_PROTO_LIT, N) \
    X(COPY_DATA_PROPS, B) X(SET_NAME, A) X(SET_NAME_ELEM, B) X(SET_HOME, N) \
    X(CLOSURE, A) X(CLASS, AB) X(CLASS_FIELDS, N) X(RUN_FIELDS, N) X(SET_CLASS_PROTO, N) \
    X(GET_ITER, N) X(GET_ASYNC_ITER, N) X(ITER_NEXT, N) X(ITER_STEP, W) X(FOR_OF_STEP, W) X(ITER_CLOSE, N) \
    X(ITER_CLOSE_QUIET, N) X(ITER_VALUE, N) X(ITER_DONE, N) X(ITER_CALL, B) X(ITER_CHECK_OBJ, N) \
    X(FOR_IN_START, N) X(FOR_IN_NEXT, W) \
    X(REGEXP, AA) X(TEMPLATE, C) X(BIGINT, C) X(SPREAD_ARRAY, N) X(REST, H) X(REST_OBJ, B) \
    X(ARGUMENTS, B) X(THIS_FUNC, N) X(NEW_TARGET, N) X(IMPORT_META, N) X(IMPORT_CALL, B) \
    X(HOME_OBJECT, N) \
    X(YIELD, N) X(YIELD_STAR, N) X(AWAIT, N) X(INITIAL_YIELD, N) X(ASYNC_GEN_YIELD, N) \
    X(GEN_RESUME_CHECK, N) X(GEN_RETURN, N) \
    X(WITH_GET, AW) X(WITH_PUT, AW) X(WITH_TYPEOF, AW) X(WITH_DELETE, AW) X(WITH_CALL_THIS, AW) \
    X(EVAL_VAR_GET, AH) X(EVAL_VAR_PUT, AH) X(EVAL_VAR_DECL, AH) X(DEBUGGER, N) X(LINE, W) X(FRAME_THIS, N) \
    X(PRIVATE_NAME, A) X(TDZ_ERROR, A) X(CONST_ERROR, A) X(GET_REF_GLOBAL, A) X(PUT_REF_GLOBAL, A) \
    X(TO_PRIMITIVE, N) X(ITER_CLOSE_SYNC_RET, N) X(ASYNC_ITER_CLOSE, N) X(GET_PRIVATE_BRAND, N) \
    X(CONST_W, A) X(NOP_END, N)

enum op {
#define OP_ENUM(name, fmt) OP_##name,
    OPS(OP_ENUM)
#undef OP_ENUM
    OP__COUNT
};

// exception table entry: handler runs for throws with pc in [start, end)
struct exc_entry {
    uint32_t start, end, handler;
    uint16_t sp;            // operand stack depth at the handler
    uint8_t kind;           // 0 catch, 1 finally (also intercepts generator return)
    uint8_t pad;
};

// upvalue descriptor: from the enclosing frame's local slot, or its upvalue
struct upval_desc { uint16_t from_local; uint16_t index; };

// function template flags
#define TF_STRICT        0x0001
#define TF_ARROW         0x0002
#define TF_GENERATOR     0x0004
#define TF_ASYNC         0x0008
#define TF_METHOD        0x0010     // [[HomeObject]] set; not a constructor
#define TF_CLASS_CTOR    0x0020
#define TF_DERIVED       0x0040
#define TF_GETTER        0x0080
#define TF_SETTER        0x0100
#define TF_SCRIPT        0x0200
#define TF_MODULE        0x0400
#define TF_EVAL          0x0800
#define TF_SIMPLE_PARAMS 0x1000
#define TF_HAS_FIELDS    0x2000     // class constructor runs field initializers
#define TF_FIELD_INIT    0x4000
#define TF_EXPR_NAME     0x8000     // named function expression (self binding)
#define TF_NO_PROTOTYPE  0x10000
#define TF_CONSTRUCTOR   0x20000    // [[Construct]] allowed
#define TF_STATIC_INIT   0x40000
#define TF_DEFAULT_CTOR  0x80000    // class without an explicit constructor (VM does it)

// an inline cache entry: where a property was found for objects of one shape.
// holder 0: own data slot of the receiver; else a data slot of holder (the
// receiver's prototype, checked by identity) whose shape was hshape. Globals: holder
// is the lexical environment or the global object; aux the lexical binding count
// a global object hit was made at (a later `let` of the same name shadows it).
struct ic { struct shape* shape; struct obj* holder; struct shape* hshape; pkey key; uint32_t slot, aux; };
#define IC_NONE 0xFFFF

#define POS_CALLEE 0x80000000u   // line table: the call's callee is a plain name at this position

struct ftempl {
    struct gch h;
    uint8_t* code;              // gc bytes
    uint32_t code_len;
    uint32_t flags;
    jv* consts;                 // valarr
    uint32_t nconsts;
    uint16_t nparams;           // formal parameters copied from arguments
    uint16_t length;            // the function's "length"
    uint16_t nlocals;
    uint16_t stack_size;
    uint16_t nupvals;
    int16_t this_slot;          // local holding `this` (-1: none needed)
    int16_t newtarget_slot;
    int16_t home_slot;          // local holding the home object (methods with super)
    int16_t callee_slot;        // local holding the function itself
    int16_t args_slot;
    struct upval_desc* upvals;  // gc bytes
    struct exc_entry* exc;      // gc bytes
    uint32_t nexc;
    uint8_t* lines;             // line table, gc bytes: per entry varint(pc delta << 1 | callee flag),
    uint32_t nlines;            //   varint(zigzag source-offset delta); nlines = bytes
    struct str* name;
    struct str* filename;
    struct str* source;         // the whole source text (Function.prototype.toString)
    uint32_t src_start, src_end;
    struct ic* ic;              // inline caches, one per property/global site (gc bytes; made on first
    uint32_t nic;               //   use, dropped by every collection so they never hold stale pointers)
    uint8_t* param_map;         // mapped arguments: 1 if parameter i is its name's binding (gc bytes)
    struct srctext* srct;       // compact copy of the source (then source is NULL; srctext.c)
};

// srctext.c: a script's source kept as UTF-8 for its compiled templates
struct srctext;
struct srccur { const struct srctext* t; uint32_t unit, byte, low; };
struct srctext* srctext_new(ojs* J, const struct str* s);
uint32_t srctext_len(const struct srctext* t);
void     srccur_init(struct srccur* c, const struct srctext* t, uint32_t pos);
uint32_t srccur_next(struct srccur* c);                 // next code unit, 0xFFFFFFFF at the end
jv       srctext_slice(ojs* J, const struct srctext* t, uint32_t from, uint32_t to);
void     srctext_compact(ojs* J, struct ftempl* top, struct ftempl* top2, const struct str* s);   // after a compile

// closure / bytecode function object (class OC_FUNCTION)
struct func {
    struct obj base;
    struct ftempl* t;
    struct upval** upv;         // nupvals (gc ptrarr data)
    jv home;                    // [[HomeObject]] (undefined if none)
    jv fields;                  // class constructor: field initializer function (undefined if none)
    jv this_val;                // arrows: the creating frame's this (script/module/eval code; functions use upvalues)
    jv module;                  // module record value (module functions), or undefined
    jv realm;
};

// native function object (class OC_NATIVE)
struct nfunc {
    struct obj base;
    native_fn fn;
    int magic;
    int ctor_kind;              // 0 not a constructor, 1 constructor
    jv data;                    // extra slot (bound promise functions etc.)
    jv data2;
};

struct upval {
    struct gch h;
    jv* loc;                    // points at a frame slot (open) or at `closed`
    jv closed;
    struct upval* next_open;    // frame's open list, ordered by slot address (descending)
};

struct ojs_frame {
    struct ojs_frame* prev;
    struct func* fn;
    struct ftempl* t;
    jv* locals;
    jv* stack;
    jv* sp;
    uint8_t* pc;
    jv this_v;
    jv new_target;
    jv* argv;
    int argc;
    struct upval* open;
    struct obj* gen;            // generator / async object owning a heap frame
    uint32_t flags;
    jv* block;                  // the value block (locals + stack) and its size
    uint32_t block_n;
    jv completion;              // pending generator return value (finally handling)
    jv eval_vars;               // object holding `var`s created by sloppy direct eval
    int resume_mode;            // generator resumption: 0 none, 1 next, 2 throw, 3 return
    jv resume_value;
    int ystar_mode;             // yield* : how the delegate is resumed (0 next, 1 throw, 2 return)
    int ystar_phase;            // async yield*: 0 call the delegate, 1 awaited its result, 2 awaited a return value, 3 awaited close
};
#define FRF_HEAP     0x01       // locals/stack live in a GT_FRAME heap block
#define FRF_CTOR     0x02       // [[Construct]] call
#define FRF_RETURNING 0x04      // generator return in progress (finally blocks)
#define FRF_SUSPENDED 0x08      // vm_run returned because the frame suspended
#define FRF_YIELD_RAW 0x10      // suspended in yield*: the value is the inner result object
#define FRF_AWAITING  0x20      // suspended at await
#define FRF_INITIAL   0x40      // suspended at the initial yield (no value on resume)
#define FRF_YSTAR     0x80      // suspended inside yield* (resume re-runs YIELD_STAR)

// heap frame storage (generators / async functions)
struct heapframe {
    struct gch h;
    struct ojs_frame f;
    uint32_t n;
    uint32_t pad;
    jv v[];
};

const char* op_name(int op);
int  op_size(int op);           // instruction length in bytes
void ftempl_dump(ojs* J, struct ftempl* t);

// vm.c
jv   vm_call(ojs* J, jv fn, jv this_v, int argc, jv* argv, jv new_target);
jv   vm_run(ojs* J, struct ojs_frame* f);
struct func* closure_new(ojs* J, struct ftempl* t, struct ojs_frame* parent, struct obj* proto);
void close_upvals(ojs* J, struct ojs_frame* f, jv* from);
jv   vm_run_script(ojs* J, struct ftempl* t, jv this_v);
jv   vm_run_func(ojs* J, struct func* fn, jv this_v, jv new_target);

#endif
