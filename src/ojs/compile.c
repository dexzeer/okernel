// compile.c — AST -> bytecode.
//
// One struct cfunc per function being compiled; nested functions are
// compiled when encountered (depth first) and become constants of their
// parent, instantiated with OP_CLOSURE.
//
// Bindings: every declaration in a scope that belongs to the function gets
// a local slot when the scope is entered. A reference from an inner
// function becomes an upvalue (struct upval_desc chain, Lua style). Names
// that resolve to nothing are global references (the global lexical
// environment, then the global object). `this`, `new.target`, the home
// object and `arguments` are pseudo-variables of the nearest non-arrow
// function, so arrows reach them through upvalues like any other binding.
//
// Control flow: try/catch/finally use the exception table; jumps that
// leave a try with a finally inline the finally code (and close the
// protected ranges around it). for-of keeps its iterator record on the
// operand stack and closes it on abrupt exits.

#include "parse.h"
#include "vm.h"
#include "gc_int.h"
#include "atoms.h"

struct cfunc;

// control-flow block (break/continue targets and what to unwind)
enum { CB_LOOP = 1, CB_SWITCH, CB_LABEL, CB_TRY_FINALLY, CB_FOR_OF, CB_FOR_IN, CB_SCOPE, CB_CATCH_PROT, CB_FINALLY_BODY };

struct patch { uint32_t at; struct patch* next; };

struct cblock {
    int kind;
    struct cblock* prev;
    struct str* label;          // label name (CB_LABEL and the loop it labels)
    struct str** labels;        // labels applied to a loop
    int nlabels;
    struct patch* breaks;
    struct patch* continues;
    uint32_t continue_target;   // when known (do-while: patched later)
    int has_continue_target;
    int stack_depth;            // operand stack depth inside the block
    struct node* finally_node;  // CB_TRY_FINALLY: the finally block
    struct cexc* prot;          // protected ranges to close while inlining finally
    struct cexc* prot2;
    int close_slot;             // CB_SCOPE: first slot whose upvalues must close on exit
    int is_async_iter;
    struct cblock* iter;        // CB_LOOP of a for-in/of: its iterator block (closed by break, kept by continue)
};

// exception table entry under construction (ranges may be split)
struct cexc {
    uint32_t handler;
    uint8_t kind;
    uint16_t sp;
    uint32_t* ranges;           // start/end pairs
    int nranges, capranges;
    uint32_t open_at;           // start of the currently open range (if open)
    int open;
    struct cexc* next;
};

struct cupv { struct decl* d; uint16_t from_local, index; };

struct cfunc {
    ojs* J;
    struct parser* P;
    struct cfunc* parent;
    struct funcinfo* fi;
    uint8_t* code; uint32_t len, cap;
    jv* consts; uint32_t nconsts, capconsts;
    int nlocals, maxlocals;
    int sp, maxsp;
    struct cupv* upv; int nupv, capupv;
    struct cexc* excs;
    uint32_t* lines; int nlines, caplines;
    uint32_t nic;               // inline cache sites so far
    // peephole state: the last instruction, and the last position something may jump to
    // (or an exception range starts/ends at): instructions are never merged across it
    uint32_t last_op_pos, barrier;
    int last_op;
    uint32_t last_line_pos;
    struct cblock* blocks;
    int this_slot, newtarget_slot, home_slot, callee_slot, args_slot, completion_slot;
    struct decl* this_decl, *newtarget_decl, *home_decl, *callee_decl;
    int failed;
    int is_global_code;         // script top level: var/function go to the global object
    int is_eval;
    int in_params;              // compiling parameter initializers (direct eval restrictions)
    int is_module;
    int nparam_slots;
    struct scope* cur_scope;
};

static void cerr(struct cfunc* c, uint32_t pos, const char* msg) {
    if (c->failed) return;
    c->failed = 1;
    int line, col;
    lex_line_col(&c->P->L, pos, &line, &col);
    throw_syntax(c->J, "%s (%s:%d:%d)", msg, c->P->filename ? c->P->filename : "<input>", line, col);
}

// ---------------------------------------------------------------- emission

static void grow_code(struct cfunc* c, uint32_t n) {
    if (c->len + n <= c->cap) return;
    uint32_t nc = c->cap ? c->cap * 2 : 256;
    while (nc < c->len + n) nc *= 2;
    uint8_t* t = (uint8_t*)ojs_sys_realloc(c->code, nc);
    if (!t) { c->failed = 1; throw_oom(c->J); return; }
    c->code = t;
    c->cap = nc;
}

static void put8(struct cfunc* c, uint32_t v) { grow_code(c, 1); if (!c->failed) c->code[c->len++] = (uint8_t)v; }
static void put16(struct cfunc* c, uint32_t v) { put8(c, v & 0xFF); put8(c, (v >> 8) & 0xFF); }
static void put32(struct cfunc* c, uint32_t v) { put16(c, v & 0xFFFF); put16(c, v >> 16); }

static void adjust(struct cfunc* c, int d) {
    c->sp += d;
    if (c->sp > c->maxsp) c->maxsp = c->sp;
    if (c->sp < 0) c->sp = 0;   // (unreachable code after jumps)
}

// emit an opcode with a stack effect. SET_LOC/SET_UPV followed by POP becomes PUT_LOC/
// PUT_UPV (assignments whose value is unused) unless a jump lands between them.
static void op(struct cfunc* c, int o, int d) {
    if (o == OP_POP && c->barrier != c->len && !c->failed && c->last_op_pos + 3 == c->len &&
        (c->last_op == OP_SET_LOC || c->last_op == OP_SET_UPV)) {
        c->code[c->last_op_pos] = (uint8_t)(c->last_op == OP_SET_LOC ? OP_PUT_LOC : OP_PUT_UPV);
        c->last_op = c->code[c->last_op_pos];
        adjust(c, d);
        return;
    }
    c->last_op_pos = c->len;
    c->last_op = o;
    put8(c, (uint32_t)o);
    adjust(c, d);
}
static void op8(struct cfunc* c, int o, uint32_t a, int d) { op(c, o, d); put8(c, a); }
static void op16(struct cfunc* c, int o, uint32_t a, int d) { op(c, o, d); put16(c, a); }
static void op32(struct cfunc* c, int o, uint32_t a, int d) { op(c, o, d); put32(c, a); }
static void op16_16(struct cfunc* c, int o, uint32_t a, uint32_t b, int d) { op(c, o, d); put16(c, a); put16(c, b); }

static uint32_t here(struct cfunc* c) { c->barrier = c->len; return c->len; }

// forward jump: returns the position of its operand
static uint32_t jump(struct cfunc* c, int o, int d) {
    op(c, o, d);
    uint32_t at = c->len;
    put32(c, 0);
    return at;
}

static void patch_to(struct cfunc* c, uint32_t at, uint32_t target) {
    if (c->failed) return;
    int32_t rel = (int32_t)target - (int32_t)(at + 4);
    c->code[at] = (uint8_t)rel;
    c->code[at + 1] = (uint8_t)(rel >> 8);
    c->code[at + 2] = (uint8_t)(rel >> 16);
    c->code[at + 3] = (uint8_t)(rel >> 24);
}

static void patch_here(struct cfunc* c, uint32_t at) { c->barrier = c->len; patch_to(c, at, c->len); }

static void jump_back(struct cfunc* c, int o, uint32_t target, int d) {
    op(c, o, d);
    uint32_t at = c->len;
    put32(c, 0);
    patch_to(c, at, target);
}

static void add_patch(struct cfunc* c, struct patch** list, uint32_t at) {
    struct patch* p = (struct patch*)arena_alloc(c->P, sizeof *p);
    if (!p) { c->failed = 1; return; }
    p->at = at;
    p->next = *list;
    *list = p;
}

static void patch_list(struct cfunc* c, struct patch* l, uint32_t target) {
    for (; l; l = l->next) patch_to(c, l->at, target);
}

// ---------------------------------------------------------------- constants

static uint32_t add_const(struct cfunc* c, jv v) {
    for (uint32_t i = 0; i < c->nconsts; i++) {
        jv x = c->consts[i];
        if (x == v && !jv_is_num(v)) return i;
        if (jv_is_num(v) && jv_is_num(x) && jv_from_dbl_raw(jv_dbl(v)) == jv_from_dbl_raw(jv_dbl(x))) return i;
    }
    if (c->nconsts >= c->capconsts) {
        uint32_t nc = c->capconsts ? c->capconsts * 2 : 16;
        jv* t = (jv*)ojs_sys_realloc(c->consts, (size_t)nc * sizeof(jv));
        if (!t) { c->failed = 1; throw_oom(c->J); return 0; }
        c->consts = t;
        c->capconsts = nc;
    }
    c->consts[c->nconsts] = v;
    return c->nconsts++;
}

static uint32_t atom_const(struct cfunc* c, struct str* a) { return add_const(c, jv_from_str(a)); }

static void push_const(struct cfunc* c, jv v) {
    if (jv_is_int(v) && jv_int(v) >= -128 && jv_int(v) <= 127) { op8(c, OP_INT8, (uint8_t)(int8_t)jv_int(v), 1); return; }
    if (jv_is_int(v)) { op32(c, OP_INT32, (uint32_t)jv_int(v), 1); return; }
    uint32_t k = add_const(c, v);
    op16(c, OP_CONST, k, 1);
}

static void push_str(struct cfunc* c, struct str* s) { push_const(c, jv_from_str(s)); }

// position -> line table (one entry per new line)
static void mark_pos(struct cfunc* c, uint32_t pos) {
    if (pos == c->last_line_pos) return;
    c->last_line_pos = pos;
    if (c->nlines + 2 > c->caplines) {
        int nc = c->caplines ? c->caplines * 2 : 64;
        uint32_t* t = (uint32_t*)ojs_sys_realloc(c->lines, (size_t)nc * sizeof(uint32_t));
        if (!t) return;
        c->lines = t;
        c->caplines = nc;
    }
    if (c->nlines >= 2 && c->lines[c->nlines - 2] == c->len) { c->lines[c->nlines - 1] = pos; return; }
    c->lines[c->nlines++] = c->len;
    c->lines[c->nlines++] = pos;
}

// ---------------------------------------------------------------- exception ranges

static struct cexc* exc_begin(struct cfunc* c, int kind) {
    struct cexc* e = (struct cexc*)arena_alloc(c->P, sizeof *e);
    if (!e) { c->failed = 1; return 0; }
    e->kind = (uint8_t)kind;
    e->sp = (uint16_t)c->sp;
    e->open = 1;
    e->open_at = c->len;
    c->barrier = c->len;
    e->next = c->excs;
    c->excs = e;
    return e;
}

static void exc_close(struct cfunc* c, struct cexc* e) {
    if (!e || !e->open) return;
    e->open = 0;
    if (c->len == e->open_at) return;
    if (e->nranges + 2 > e->capranges) {
        int nc = e->capranges ? e->capranges * 2 : 4;
        uint32_t* t = (uint32_t*)arena_alloc(c->P, (size_t)nc * sizeof(uint32_t));
        if (!t) { c->failed = 1; return; }
        if (e->nranges) memcpy(t, e->ranges, (size_t)e->nranges * sizeof(uint32_t));
        e->ranges = t;
        e->capranges = nc;
    }
    e->ranges[e->nranges++] = e->open_at;
    e->ranges[e->nranges++] = c->len;
    c->barrier = c->len;
}

static void exc_reopen(struct cfunc* c, struct cexc* e) {
    if (!e || e->open) return;
    e->open = 1;
    e->open_at = c->len;
    c->barrier = c->len;
}

// ---------------------------------------------------------------- locals

static int new_local(struct cfunc* c) {
    int s = c->nlocals++;
    if (c->nlocals > c->maxlocals) c->maxlocals = c->nlocals;
    if (c->nlocals > 65000) cerr(c, 0, "too many local variables");
    return s;
}

static struct str* pseudo_name(struct cfunc* c, const char* s) { return atom_cstr(c->J, s); }

// ---------------------------------------------------------------- name resolution

enum { R_LOCAL = 1, R_UPVAL, R_GLOBAL, R_WITH };

struct ref {
    int kind;
    int index;              // slot / upvalue index
    struct decl* d;
    struct str* name;
    int tdz;                // needs a TDZ check
    int is_const;           // assignment throws TypeError (const) / silently fails (function name)
    int with_count;         // R_WITH: number of with scopes between
};

static int upval_index(struct cfunc* c, struct decl* d) {
    if ((d->flags & 0x40) && !c->parent) return d->slot;   // eval environment cell
    for (int i = 0; i < c->nupv; i++) if (c->upv[i].d == d) return i;
    struct cfunc* p = c->parent;
    if (!p) { cerr(c, 0, "internal: upvalue without an enclosing function"); return 0; }
    uint16_t from_local, index;
    if (d->scope->kind == SC_MODULE || (d->flags & 0x40)) {
        // module cells / eval environment bindings: upvalues of the top function
        if (d->scope->fn == p->fi || (d->flags & 0x40 && !p->parent)) { from_local = 0; index = (uint16_t)d->slot; }
        else { from_local = 0; index = (uint16_t)upval_index(p, d); }
    } else if (d->scope->fn == p->fi) {
        from_local = 1;
        index = (uint16_t)d->slot;
        if (d->slot < 0) { cerr(c, d->node ? d->node->pos : 0, "internal: captured binding has no slot"); return 0; }
    } else {
        from_local = 0;
        index = (uint16_t)upval_index(p, d);
    }
    d->flags |= DF_CAPTURED;
    if (c->nupv >= c->capupv) {
        int nc = c->capupv ? c->capupv * 2 : 8;
        struct cupv* t = (struct cupv*)ojs_sys_realloc(c->upv, (size_t)nc * sizeof(struct cupv));
        if (!t) { c->failed = 1; throw_oom(c->J); return 0; }
        c->upv = t;
        c->capupv = nc;
    }
    c->upv[c->nupv].d = d;
    c->upv[c->nupv].from_local = from_local;
    c->upv[c->nupv].index = index;
    return c->nupv++;
}

static int is_tdz_kind(int k) { return k == D_LET || k == D_CONST || k == D_CLASS || k == D_CLASSNAME; }

// a function body whose function contains a sloppy direct eval: eval may
// add vars there at run time (looked up like a with object)
static int is_evalvar_scope(struct scope* s) { return s->has_eval == 2; }   // holds the *evalvars* binding

// resolve `name` as seen from scope `s` in function c
static void resolve(struct cfunc* c, struct str* name, struct scope* s, struct ref* r) {
    memset(r, 0, sizeof *r);
    r->name = name;
    int withs = 0;
    for (; s; s = s->parent) {
        if (s->kind == SC_WITH) { withs++; continue; }
        struct decl* d = scope_find(s, name);
        if (!d && is_evalvar_scope(s)) { withs++; continue; }
        if (!d) continue;
        // module import bindings are module cells (upvalues of the module function)
        r->d = d;
        r->tdz = is_tdz_kind(d->kind) || d->kind == D_IMPORT;
        r->is_const = d->kind == D_CONST || d->kind == D_CLASSNAME || d->kind == D_FUNCNAME || d->kind == D_IMPORT;
        r->with_count = withs;
        if (s->kind == SC_SCRIPT || (s->kind == SC_EVAL && d->slot < 0 && !(d->flags & 0x40))) {
            // script top level / global eval: the global object and global lexical environment
            r->d = 0;
            r->kind = withs ? R_WITH : R_GLOBAL;
            return;
        }
        if (s->kind == SC_MODULE || (d->flags & 0x40)) {
            // module cells and eval-environment bindings are upvalues of the top function
            if (c->fi == s->fn || ((d->flags & 0x40) && !c->parent)) { r->kind = R_UPVAL; r->index = d->slot; }
            else { r->kind = R_UPVAL; r->index = upval_index(c, d); }
        } else if (d->scope->fn == c->fi && d->slot >= 0) { r->kind = R_LOCAL; r->index = d->slot; }
        else { r->kind = R_UPVAL; r->index = upval_index(c, d); }
        if (withs) r->kind = R_WITH;
        return;
    }
    r->kind = withs ? R_WITH : R_GLOBAL;
    r->with_count = withs;
}

// the nearest non-arrow function (for this / new.target / home object / arguments)
static struct funcinfo* this_func(struct funcinfo* fi) {
    while (fi && (fi->flags & FI_ARROW)) fi = fi->parent;
    return fi;
}

// ---------------------------------------------------------------- field access helpers

// GET_FIELD atom, ic: [obj] -> [value]
static uint32_t new_ic(struct cfunc* c) { return c->nic < IC_NONE ? c->nic++ : IC_NONE; }

static void get_field(struct cfunc* c, struct str* a) { op32(c, OP_GET_FIELD, atom_const(c, a), 0); put16(c, new_ic(c)); }
// GET_FIELD_KEEP atom, ic: [obj] -> [value, obj] (method call: function, this)
static void emit_get_method(struct cfunc* c, struct str* a) { op32(c, OP_GET_FIELD_KEEP, atom_const(c, a), 1); put16(c, new_ic(c)); }
// PUT_FIELD atom, ic: [obj, value] -> [value]
static void put_field(struct cfunc* c, struct str* a) { op32(c, OP_PUT_FIELD, atom_const(c, a), -1); put16(c, new_ic(c)); }

// ---------------------------------------------------------------- variable access


// a ref built from a known decl (pseudo bindings, hidden class bindings): local or upvalue
static void ref_kind(struct cfunc* c, struct ref* r) {
    if (r->kind || !r->d) return;
    struct decl* d = r->d;
    if (d->scope->fn == c->fi && d->slot >= 0 && d->scope->kind != SC_MODULE && !(d->flags & 0x40)) {
        r->kind = R_LOCAL;
        r->index = d->slot;
    } else {
        r->kind = R_UPVAL;
        r->index = upval_index(c, d);
    }
}

static void load_ref_static(struct cfunc* c, struct ref* r, uint32_t pos, int for_typeof) {
    ref_kind(c, r);
    if (r->kind == R_LOCAL || (r->kind == R_WITH && r->d && r->d->scope->fn == c->fi && r->d->slot >= 0 &&
                               r->d->scope->kind != SC_MODULE && !(r->d->flags & 0x40))) {
        if (r->tdz) op16_16(c, OP_GET_LOC_CHK, (uint32_t)r->d->slot, atom_const(c, r->name), 1);
        else op16(c, OP_GET_LOC, (uint32_t)r->d->slot, 1);
    } else if (r->d && r->kind != R_GLOBAL) {
        int ix = r->kind == R_UPVAL ? r->index : upval_index(c, r->d);
        if (r->tdz) op16_16(c, OP_GET_UPV_CHK, (uint32_t)ix, atom_const(c, r->name), 1);
        else op16(c, OP_GET_UPV, (uint32_t)ix, 1);
    } else {
        op32(c, for_typeof ? OP_TYPEOF_GLOBAL : OP_GET_GLOBAL, atom_const(c, r->name), 1);
        put16(c, new_ic(c));
    }
    (void)pos;
}

static void store_ref_static(struct cfunc* c, struct ref* r, uint32_t pos, int keep, int init) {
    // value on top of the stack; keep = leave it there
    ref_kind(c, r);
    if (r->d && r->is_const && !init) {
        if (r->d->kind == D_FUNCNAME) {
            // assignment to a named function expression's own name: ignored (strict: TypeError)
            if (c->fi->flags & FI_STRICT) op32(c, OP_CONST_ERROR, atom_const(c, r->name), 0);
            if (!keep) op(c, OP_POP, -1);
            return;
        }
        // const: TDZ check first (a ReferenceError beats the TypeError), then TypeError
        if (r->d->scope->fn == c->fi && r->d->slot >= 0) op16_16(c, OP_GET_LOC_CHK, (uint32_t)r->d->slot, atom_const(c, r->name), 1);
        else op16_16(c, OP_GET_UPV_CHK, (uint32_t)upval_index(c, r->d), atom_const(c, r->name), 1);
        op(c, OP_POP, -1);
        op32(c, OP_CONST_ERROR, atom_const(c, r->name), 0);
        if (!keep) op(c, OP_POP, -1);
        return;
    }
    if (r->d && r->kind != R_GLOBAL && r->kind != R_UPVAL && r->d->scope->fn == c->fi && r->d->slot >= 0 &&
        r->d->scope->kind != SC_MODULE && !(r->d->flags & 0x40)) {
        uint32_t s = (uint32_t)r->d->slot;
        if (init) { op16(c, OP_INIT_LOC, s, 0); if (!keep) op(c, OP_POP, -1); return; }
        if (r->tdz) { op16_16(c, OP_PUT_LOC_CHK, s, atom_const(c, r->name), 0); if (!keep) op(c, OP_POP, -1); return; }
        if (keep) op16(c, OP_SET_LOC, s, 0); else op16(c, OP_PUT_LOC, s, -1);
        return;
    }
    if (r->d && r->kind != R_GLOBAL) {
        uint32_t ix = r->kind == R_UPVAL ? (uint32_t)r->index : (uint32_t)upval_index(c, r->d);
        if (init) { op16(c, OP_INIT_UPV, ix, 0); if (!keep) op(c, OP_POP, -1); return; }
        if (r->tdz) { op16_16(c, OP_PUT_UPV_CHK, ix, atom_const(c, r->name), 0); if (!keep) op(c, OP_POP, -1); return; }
        if (keep) op16(c, OP_SET_UPV, ix, 0); else op16(c, OP_PUT_UPV, ix, -1);
        return;
    }
    if (init) op32(c, OP_INIT_GLOBAL_LEX, atom_const(c, r->name), 0);
    else { op32(c, (c->fi->flags & FI_STRICT) ? OP_PUT_GLOBAL_STRICT : OP_PUT_GLOBAL, atom_const(c, r->name), 0); put16(c, new_ic(c)); }
    if (!keep) op(c, OP_POP, -1);
    (void)pos;
}

// push the dynamic object of the i-th dynamic scope (with / eval vars, innermost first)
static void push_with_obj(struct cfunc* c, struct scope* s, int i) {
    for (; s; s = s->parent) {
        if (s->kind != SC_WITH && !is_evalvar_scope(s)) continue;
        if (i-- == 0) {
            struct decl* d = 0;
            for (struct decl* e = s->decls; e; e = e->next) if (e->kind == D_WITH || e->kind == D_EVALVARS) { d = e; break; }
            if (!d) { op(c, OP_UNDEF, 1); return; }
            if (d->scope->fn == c->fi) op16(c, OP_GET_LOC, (uint32_t)d->slot, 1);
            else op16(c, OP_GET_UPV, (uint32_t)upval_index(c, d), 1);
            return;
        }
    }
}

// emits the with-lookup chain; returns patches to the "found" exit
static struct patch* with_chain(struct cfunc* c, struct scope* s, struct ref* r, int opc) {
    struct patch* found = 0;
    for (int i = 0; i < r->with_count; i++) {
        push_with_obj(c, s, i);
        op32(c, opc, atom_const(c, r->name), 0);
        uint32_t at = c->len;
        put32(c, 0);
        add_patch(c, &found, at);
        // not found: the op popped the object
        adjust(c, -1);
    }
    return found;
}

static void load_name(struct cfunc* c, struct str* name, struct scope* s, uint32_t pos, int for_typeof) {
    struct ref r;
    resolve(c, name, s, &r);
    if (r.kind == R_WITH) {
        struct patch* found = with_chain(c, s, &r, OP_WITH_GET);
        load_ref_static(c, &r, pos, for_typeof);
        patch_list(c, found, here(c));
        return;
    }
    load_ref_static(c, &r, pos, for_typeof);
}

// value on top; leaves it there when keep
static void store_name(struct cfunc* c, struct str* name, struct scope* s, uint32_t pos, int keep, int init) {
    struct ref r;
    resolve(c, name, s, &r);
    if (r.kind == R_WITH && !init) {
        if (!keep) { /* keep a copy for the with path's result */ }
        struct patch* found = 0;
        for (int i = 0; i < r.with_count; i++) {
            push_with_obj(c, s, i);
            op32(c, OP_WITH_PUT, atom_const(c, r.name), -1);
            uint32_t at = c->len;
            put32(c, 0);
            add_patch(c, &found, at);
        }
        store_ref_static(c, &r, pos, 1, 0);
        patch_list(c, found, here(c));
        if (!keep) op(c, OP_POP, -1);
        return;
    }
    store_ref_static(c, &r, pos, keep, init);
}

// ---------------------------------------------------------------- forward decls

static void comp_expr(struct cfunc* c, struct node* n);
static void comp_expr_named(struct cfunc* c, struct node* n, struct str* name);
static void comp_stmt(struct cfunc* c, struct node* n);
static void comp_stmts(struct cfunc* c, struct node* list);
static void comp_pattern_assign(struct cfunc* c, struct node* pat, int kind);
static void comp_function_node(struct cfunc* c, struct node* fn);
static void comp_class(struct cfunc* c, struct node* cls, struct str* inferred);
static void enter_scope(struct cfunc* c, struct scope* s, int hoist_functions);
static void leave_scope(struct cfunc* c, struct scope* s);
static int  scope_has_captures(struct cfunc* c, struct scope* s);
static void comp_optchain(struct cfunc* c, struct node* n, int as_callee);
static void super_ref(struct cfunc* c, struct node* n);
static jv bigint_literal(ojs* J, struct str* text);
static void throw_err(struct cfunc* c, int kind, const char* msg);
static jv template_object(struct cfunc* c, struct node* n);
static jv eval_env_descriptor(struct cfunc* c, struct scope* s);
static struct str* priv_atom(struct cfunc* c, struct str* name);

// ---------------------------------------------------------------- this / super / new.target

static void comp_this(struct cfunc* c, uint32_t pos);

static void load_pseudo(struct cfunc* c, const char* which, uint32_t pos) {
    struct funcinfo* tf = this_func(c->fi);
    struct str* nm = pseudo_name(c, which);
    // the pseudo decl lives in the target function's param scope (or, for
    // eval code, in the eval environment's scopes)
    struct decl* d = tf ? scope_find(tf->param_scope, nm) : 0;
    if (!d && tf && (tf->flags & FI_EVAL)) {
        for (struct scope* s = c->cur_scope ? c->cur_scope : tf->param_scope; s && !d; s = s->parent) d = scope_find(s, nm);
    }
    if (!d) {
        // global / module / eval code
        if (!strcmp(which, "this")) { op(c, OP_FRAME_THIS, 1); return; }   // the frame's this
        if (!strcmp(which, "new.target")) { op(c, OP_NEW_TARGET, 1); return; }
        op(c, OP_HOME_OBJECT, 1);
        return;
    }
    struct ref r;
    memset(&r, 0, sizeof r);
    r.d = d;
    r.name = nm;
    r.tdz = d->kind == D_THIS && (tf->flags & FI_DERIVED);   // this before super()
    load_ref_static(c, &r, pos, 0);
}

// [key, home, this] for super property access
static void super_ref(struct cfunc* c, struct node* n) {
    if (n->flags & NF_COMPUTED) { comp_expr(c, n->b); op(c, OP_TO_PROPKEY, 0); }
    else push_str(c, n->u.str);
    load_pseudo(c, "*home*", n->pos);
    comp_this(c, n->pos);
}

static void comp_this(struct cfunc* c, uint32_t pos) {
    struct funcinfo* tf = this_func(c->fi);
    if (tf && (tf->flags & FI_DERIVED)) {
        struct decl* d = scope_find(tf->param_scope, pseudo_name(c, "this"));
        if (d) {
            struct ref r;
            memset(&r, 0, sizeof r);
            r.d = d;
            r.name = pseudo_name(c, "this");
            r.tdz = 1;
            load_ref_static(c, &r, pos, 0);
            return;
        }
    }
    load_pseudo(c, "this", pos);
}

// ---------------------------------------------------------------- literals

static void comp_template(struct cfunc* c, struct node* n) {
    // `a${x}b${y}c` -> "a" + ToString(x) + "b" + ...
    struct node* q = n->b;
    struct node* e = n->c;
    push_str(c, q->u.str);
    q = q->next;
    while (e) {
        comp_expr(c, e);
        op(c, OP_TO_STRING, 0);
        op(c, OP_ADD, -1);
        if (q && str_len(q->u.str)) { push_str(c, q->u.str); op(c, OP_ADD, -1); }
        q = q ? q->next : 0;
        e = e->next;
    }
}

static int is_anon_fn(struct node* n) {
    // a parenthesized function / class is still an anonymous function definition
    if (!n) return 0;
    if (n->type == N_FUNC) return !n->u.fn->name || (n->u.fn->flags & FI_ARROW) == FI_ARROW ? !n->u.fn->name : 0;
    if (n->type == N_CLASS) return !n->u.str;
    return 0;
}

static void comp_array(struct cfunc* c, struct node* n) {
    int simple = !(n->flags & NF_HAS_SPREAD);
    int count = 0;
    for (struct node* e = n->a; e; e = e->next) { count++; if (e->type == N_HOLE) simple = 0; }
    if (simple && count < 1024) {
        for (struct node* e = n->a; e; e = e->next) comp_expr(c, e);
        op16(c, OP_ARRAY, (uint32_t)count, 1 - count);
        return;
    }
    op16(c, OP_ARRAY, 0, 1);
    for (struct node* e = n->a; e; e = e->next) {
        if (e->type == N_HOLE) { op(c, OP_APPEND_HOLE, 0); continue; }
        if (e->type == N_SPREAD) { comp_expr(c, e->a); op(c, OP_APPEND, -1); continue; }
        comp_expr(c, e);
        op(c, OP_APPEND_ONE, -1);
    }
}

// property key on the stack (computed) or as an atom; returns the atom or NULL
static struct str* prop_key(struct cfunc* c, struct node* k, int computed) {
    if (computed) { comp_expr(c, k); op(c, OP_TO_PROPKEY, 0); return 0; }
    if (k->type == N_NUM) {
        jv kv = num_to_string(c->J, k->u.num, 10);
        if (kv == JV_EXC) { c->failed = 1; return 0; }
        return atom_str(c->J, jv_str(kv));
    }
    if (k->type == N_BIGINT) {
        // canonical numeric string of the BigInt literal
        jv bv = bigint_literal(c->J, k->u.str);
        jv kv = bv == JV_EXC ? JV_EXC : to_string(c->J, bv);
        if (kv == JV_EXC) { c->failed = 1; return 0; }
        return atom_str(c->J, str_flat(c->J, kv));
    }
    return atom_str(c->J, k->u.str);
}

// method flags for DEFINE_METHOD: kind | enumerable<<4
#define DM_METHOD 0
#define DM_GET 1
#define DM_SET 2
#define DM_ENUM 0x10

static void comp_object(struct cfunc* c, struct node* n) {
    op(c, OP_OBJECT, 1);
    for (struct node* p = n->a; p; p = p->next) {
        int computed = (p->flags & NF_COMPUTED) != 0;
        switch (p->op) {
        case PK_SPREAD:
            comp_expr(c, p->b);
            op8(c, OP_COPY_DATA_PROPS, 0, -1);
            break;
        case PK_PROTO:
            comp_expr(c, p->b);
            op(c, OP_SET_PROTO_LIT, -1);
            break;
        case PK_INIT: {
            struct str* a = prop_key(c, p->a, computed);
            if (computed) {
                if (is_anon_fn(p->b)) { comp_expr(c, p->b); op8(c, OP_SET_NAME_ELEM, 0, 0); }
                else comp_expr(c, p->b);
                op(c, OP_DEFINE_ELEM, -2);
            } else {
                comp_expr_named(c, p->b, a);
                op32(c, OP_DEFINE_FIELD, atom_const(c, a), -1);
            }
            break;
        }
        default: {   // methods, getters, setters: [obj, key, fn]
            int kind = p->op == PK_GET ? DM_GET : p->op == PK_SET ? DM_SET : DM_METHOD;
            struct str* a = prop_key(c, p->a, computed);
            if (!computed) push_str(c, a);
            comp_function_node(c, p->b);
            op8(c, OP_DEFINE_METHOD, (uint32_t)(kind | DM_ENUM), -2);
            break;
        }
        }
    }
}

// ---------------------------------------------------------------- operators

static int binop_code(int tok) {
    switch (tok) {
    case T_PLUS: return OP_ADD; case T_MINUS: return OP_SUB; case T_STAR: return OP_MUL;
    case T_SLASH: return OP_DIV; case T_PERCENT: return OP_MOD; case T_STARSTAR: return OP_POW;
    case T_SHL: return OP_SHL; case T_SAR: return OP_SAR; case T_SHR: return OP_SHR;
    case T_AMP: return OP_BAND; case T_PIPE: return OP_BOR; case T_CARET: return OP_BXOR;
    case T_EQ: return OP_EQ; case T_NE: return OP_NE; case T_SEQ: return OP_SEQ; case T_SNE: return OP_SNE;
    case T_LT: return OP_LT; case T_LE: return OP_LE; case T_GT: return OP_GT; case T_GE: return OP_GE;
    case T_IN: return OP_IN; case T_INSTANCEOF: return OP_INSTANCEOF;
    case T_PLUS_ASSIGN: return OP_ADD; case T_MINUS_ASSIGN: return OP_SUB; case T_STAR_ASSIGN: return OP_MUL;
    case T_SLASH_ASSIGN: return OP_DIV; case T_PERCENT_ASSIGN: return OP_MOD; case T_STARSTAR_ASSIGN: return OP_POW;
    case T_SHL_ASSIGN: return OP_SHL; case T_SAR_ASSIGN: return OP_SAR; case T_SHR_ASSIGN: return OP_SHR;
    case T_AMP_ASSIGN: return OP_BAND; case T_PIPE_ASSIGN: return OP_BOR; case T_CARET_ASSIGN: return OP_BXOR;
    default: return -1;
    }
}

// ---------------------------------------------------------------- references (assignment targets)

enum { LV_NAME = 1, LV_FIELD, LV_ELEM, LV_SUPER, LV_PRIVATE, LV_CALL };

struct lval {
    int kind;
    struct node* n;
    struct str* atom;   // LV_FIELD
};

// evaluate the parts of an assignment target onto the stack
static void lval_prepare(struct cfunc* c, struct node* n, struct lval* lv) {
    memset(lv, 0, sizeof *lv);
    lv->n = n;
    if (n->type == N_IDENT) { lv->kind = LV_NAME; return; }
    if (n->type == N_CALL) {
        // Annex B: evaluate the call, then a ReferenceError
        lv->kind = LV_CALL;
        comp_expr(c, n);
        return;
    }
    // N_MEMBER
    if (n->a->type == N_SUPER) {
        lv->kind = LV_SUPER;
        super_ref(c, n);   // [key, home, this]
        return;
    }
    comp_expr(c, n->a);
    if (n->flags & NF_PRIVATE) {
        lv->kind = LV_PRIVATE;
        load_name(c, priv_atom(c, n->u.str), n->scope, n->pos, 0);   // the private name (a symbol) in the class scope
        return;
    }
    if (n->flags & NF_COMPUTED) {
        lv->kind = LV_ELEM;
        comp_expr(c, n->b);
        op(c, OP_TO_PROPKEY, 0);
        return;
    }
    lv->kind = LV_FIELD;
    lv->atom = n->u.str;
}

// stack items a prepared target occupies
static int lval_parts(const struct lval* lv) {
    switch (lv->kind) {
    case LV_FIELD: case LV_CALL: return 1;
    case LV_ELEM: case LV_PRIVATE: return 2;
    case LV_SUPER: return 3;
    default: return 0;
    }
}

// current value of a prepared target (keeps the parts below it)
static void lval_get(struct cfunc* c, struct lval* lv) {
    switch (lv->kind) {
    case LV_NAME: load_name(c, lv->n->u.str, lv->n->scope, lv->n->pos, 0); break;
    case LV_FIELD: op(c, OP_DUP, 1); get_field(c, lv->atom); break;
    case LV_ELEM: op(c, OP_DUP2, 2); op(c, OP_GET_ELEM, -1); break;
    case LV_SUPER: op8(c, OP_PICK, 2, 1); op8(c, OP_PICK, 2, 1); op8(c, OP_PICK, 2, 1); op(c, OP_GET_SUPER, -2); break;
    case LV_PRIVATE: op(c, OP_DUP2, 2); op(c, OP_GET_PRIVATE, -1); break;
    case LV_CALL: op(c, OP_CALL_IGNORED_REF_ERROR, 1); break;   // always throws ReferenceError
    }
}

// store the value on top of the stack into the target (the parts below are
// consumed); the value stays on the stack
static void lval_put(struct cfunc* c, struct lval* lv) {
    switch (lv->kind) {
    case LV_NAME: store_name(c, lv->n->u.str, lv->n->scope, lv->n->pos, 1, 0); break;
    case LV_FIELD: put_field(c, lv->atom); break;
    case LV_ELEM: op(c, OP_PUT_ELEM, -2); break;
    case LV_SUPER: op(c, OP_PUT_SUPER, -3); break;
    case LV_PRIVATE: op(c, OP_PUT_PRIVATE, -2); break;
    case LV_CALL: op(c, OP_CALL_IGNORED_REF_ERROR, -1); break;   // always throws ReferenceError
    }
}

// ---------------------------------------------------------------- calls

// push call arguments; returns the count, or -1 when they were packed into
// an array (spread)
static int comp_args(struct cfunc* c, struct node* args, int has_spread) {
    if (!has_spread) {
        int n = 0;
        for (struct node* a = args; a; a = a->next) { comp_expr(c, a); n++; }
        if (n > 65535) cerr(c, args ? args->pos : 0, "too many arguments");
        return n;
    }
    op16(c, OP_ARRAY, 0, 1);
    for (struct node* a = args; a; a = a->next) {
        if (a->type == N_SPREAD) { comp_expr(c, a->a); op(c, OP_APPEND, -1); }
        else { comp_expr(c, a); op(c, OP_APPEND_ONE, -1); }
    }
    return -1;
}

// a, a.b.c, this.x, a.#p: the callee text names the function in "x is not a function"
static int plain_callee(struct node* f) {
    if (f->flags & NF_PAREN) return 0;
    if (f->type == N_IDENT || f->type == N_THIS) return 1;
    if (f->type == N_MEMBER && !(f->flags & NF_COMPUTED) && f->a->type != N_SUPER) return plain_callee(f->a);
    return 0;
}

// the call instruction's own position (stack traces, callee names in errors):
// a plain callee is marked at its first token
static void mark_call(struct cfunc* c, struct node* call, struct node* callee) {
    if (!plain_callee(callee)) { mark_pos(c, call->pos); return; }
    if (call->type == N_NEW) { mark_pos(c, call->pos | POS_CALLEE); return; }   // at `new`
    struct node* x = callee;
    while (x->type == N_MEMBER) x = x->a;
    mark_pos(c, x->pos | POS_CALLEE);
}

static void emit_call(struct cfunc* c, int argc, int spread) {
    // stack: [func, this, args...] or [func, this, argsarray]
    if (spread) op(c, OP_CALL_SPREAD, -2);
    else op16(c, OP_CALL, (uint32_t)argc, -(argc + 1));
}

// push [func, this] for the callee expression
static void comp_callee(struct cfunc* c, struct node* f) {
    struct node* x = f;
    if (x->type == N_MEMBER && !(x->flags & NF_PAREN && 0)) {
        if (x->a->type == N_SUPER) {
            super_ref(c, x);
            op(c, OP_GET_SUPER_KEEP, -1);   // [key, home, this] -> [fn, this]
            return;
        }
        comp_expr(c, x->a);
        if (x->flags & NF_PRIVATE) {
            op(c, OP_DUP, 1);
            load_name(c, priv_atom(c, x->u.str), x->scope, x->pos, 0);
            op(c, OP_GET_PRIVATE, -1);
            op(c, OP_SWAP, 0);
            return;
        }
        if (x->flags & NF_COMPUTED) {
            comp_expr(c, x->b);
            op(c, OP_GET_ELEM_KEEP, 0);
            return;
        }
        emit_get_method(c, x->u.str);
        return;
    }
    if (x->type == N_IDENT) {
        struct ref r;
        resolve(c, x->u.str, x->scope, &r);
        if (r.kind == R_WITH) {
            // with (o) f(): `this` is o when f is found on it
            struct patch* found = 0;
            for (int i = 0; i < r.with_count; i++) {
                push_with_obj(c, x->scope, i);
                op32(c, OP_WITH_CALL_THIS, atom_const(c, r.name), 0);
                uint32_t at = c->len;
                put32(c, 0);
                add_patch(c, &found, at);
                adjust(c, -1);
            }
            load_ref_static(c, &r, x->pos, 0);
            op(c, OP_UNDEF, 1);
            patch_list(c, found, here(c));
            return;
        }
    }
    comp_expr(c, f);
    op(c, OP_UNDEF, 1);
}

static void comp_call(struct cfunc* c, struct node* n) {
    struct node* f = n->a;
    if (f->type == N_SUPER) {
        // super(...args): the parent constructor is the active class
        // constructor's [[Prototype]]; then bind this and run field initializers
        load_pseudo(c, "*func*", n->pos);
        op(c, OP_SUPER_CTOR, 0);                 // [parent]
        load_pseudo(c, "new.target", n->pos);    // [parent, newtarget]
        int argc = comp_args(c, n->b, (n->flags & NF_HAS_SPREAD) != 0);
        if (argc < 0) op(c, OP_SUPER_CALL_SPREAD, -2);
        else op16(c, OP_SUPER_CALL, (uint32_t)argc, -(argc + 1));
        struct funcinfo* tf = this_func(c->fi);
        struct decl* d = tf ? scope_find(tf->param_scope, pseudo_name(c, "this")) : 0;
        if (d) {
            if (d->scope->fn == c->fi) op16(c, OP_BIND_THIS_LOC, (uint32_t)d->slot, 0);
            else op16(c, OP_BIND_THIS_UPV, (uint32_t)upval_index(c, d), 0);
        }
        load_pseudo(c, "*func*", n->pos);
        op(c, OP_RUN_FIELDS, -1);                // [this]
        return;
    }
    if (n->flags & NF_DIRECT_EVAL) {
        // direct eval: callee + this, then args; the VM decides whether it is really eval
        comp_callee(c, f);
        int argc = comp_args(c, n->b, (n->flags & NF_HAS_SPREAD) != 0);
        uint32_t env = add_const(c, eval_env_descriptor(c, n->scope));   // visible bindings
        mark_call(c, n, f);
        if (argc < 0) { op16(c, OP_EVAL_SPREAD, env, -2); }
        else { op16_16(c, OP_EVAL, (uint32_t)argc, env, -(argc + 1)); }
        return;
    }
    if (f->type == N_OPTCHAIN) {
        // (a?.b)(...): the parenthesized chain still supplies `this` (a?.b() itself is
        // compiled inside the chain)
        comp_optchain(c, f, 1);   // leaves [func, this]
    } else comp_callee(c, f);
    int argc = comp_args(c, n->b, (n->flags & NF_HAS_SPREAD) != 0);
    mark_call(c, n, f);
    emit_call(c, argc < 0 ? 0 : argc, argc < 0);
}

// ---------------------------------------------------------------- optional chains

// a?.b.c(d)?.[e] ... : one chain. Short-circuit sites jump to `end` with
// the right stack depth (each site pops what it has above the base).
struct chain { int base_sp; struct patch* to_end; };

static void chain_check(struct cfunc* c, struct chain* ch, int extra_below) {
    // top of stack: value to test; extra_below more items above base below it
    (void)extra_below;
    op(c, OP_DUP, 1);
    op(c, OP_IS_NULLISH, 0);
    uint32_t skip = jump(c, OP_JF, -1);
    int depth = c->sp - ch->base_sp;
    for (int i = 0; i < depth; i++) op(c, OP_POP, -1);
    op(c, OP_UNDEF, 1);
    uint32_t j = jump(c, OP_JMP, 0);
    add_patch(c, &ch->to_end, j);
    c->sp = ch->base_sp + depth;   // the fall-through path keeps its depth
    patch_here(c, skip);
}

// compile a member/call chain element; as_callee: leave [func, this]
static void comp_chain_elem(struct cfunc* c, struct node* n, struct chain* ch, int as_callee) {
    if (n->type == N_MEMBER) {
        if (n->a->type == N_SUPER) {
            super_ref(c, n);
            op(c, as_callee ? OP_GET_SUPER_KEEP : OP_GET_SUPER, as_callee ? -1 : -2);
            return;
        }
        comp_chain_elem(c, n->a, ch, 0);
        if (n->flags & NF_OPTIONAL) chain_check(c, ch, 0);
        if (n->flags & NF_PRIVATE) {
            if (as_callee) op(c, OP_DUP, 1);
            load_name(c, priv_atom(c, n->u.str), n->scope, n->pos, 0);
            op(c, OP_GET_PRIVATE, -1);
            if (as_callee) op(c, OP_SWAP, 0);
            return;
        }
        if (n->flags & NF_COMPUTED) {
            comp_expr(c, n->b);
            if (as_callee) op(c, OP_GET_ELEM_KEEP, 0); else op(c, OP_GET_ELEM, -1);
            return;
        }
        if (as_callee) emit_get_method(c, n->u.str); else get_field(c, n->u.str);
        return;
    }
    if (n->type == N_CALL && n->a->type != N_SUPER && !(n->flags & NF_DIRECT_EVAL)) {
        comp_chain_elem(c, n->a, ch, 1);   // [func, this]
        if (n->flags & NF_OPTIONAL) {
            // test the function (below this)
            op8(c, OP_PICK, 1, 1);
            op(c, OP_IS_NULLISH, 0);
            uint32_t skip = jump(c, OP_JF, -1);
            int depth = c->sp - ch->base_sp;
            for (int i = 0; i < depth; i++) op(c, OP_POP, -1);
            op(c, OP_UNDEF, 1);
            uint32_t j = jump(c, OP_JMP, 0);
            add_patch(c, &ch->to_end, j);
            c->sp = ch->base_sp + depth;
            patch_here(c, skip);
        }
        int argc = comp_args(c, n->b, (n->flags & NF_HAS_SPREAD) != 0);
        emit_call(c, argc < 0 ? 0 : argc, argc < 0);
        if (as_callee) op(c, OP_UNDEF, 1);
        return;
    }
    if (n->type == N_OPTCHAIN) {   // nested parenthesized chain element
        comp_optchain(c, n, as_callee);
        return;
    }
    if (as_callee) comp_callee(c, n);
    else comp_expr(c, n);
}

static void comp_optchain(struct cfunc* c, struct node* n, int as_callee) {
    struct chain ch;
    ch.base_sp = c->sp;
    ch.to_end = 0;
    comp_chain_elem(c, n->a, &ch, as_callee);
    if (as_callee) {
        // short-circuited sites push only undefined; give them [undefined, undefined]
        uint32_t over = jump(c, OP_JMP, 0);
        int done_sp = c->sp;
        patch_list(c, ch.to_end, here(c));
        c->sp = ch.base_sp + 1;
        op(c, OP_UNDEF, 1);
        patch_here(c, over);
        c->sp = done_sp;
        return;
    }
    patch_list(c, ch.to_end, here(c));
    c->sp = ch.base_sp + 1;
}

// ---------------------------------------------------------------- assignment

static void comp_assign(struct cfunc* c, struct node* n) {
    struct node* t = n->a;
    int o = n->op;
    if (o == T_ASSIGN) {
        if (t->type == N_OBJECT_PAT || t->type == N_ARRAY_PAT) {
            comp_expr(c, n->b);
            op(c, OP_DUP, 1);
            comp_pattern_assign(c, t, 0);
            return;
        }
        if (t->type == N_IDENT) {
            comp_expr_named(c, n->b, t->u.str);
            store_name(c, t->u.str, t->scope, t->pos, 1, 0);
            return;
        }
        struct lval lv;
        lval_prepare(c, t, &lv);
        comp_expr(c, n->b);
        lval_put(c, &lv);
        return;
    }
    if (o == T_AND_ASSIGN || o == T_OR_ASSIGN || o == T_NULLISH_ASSIGN) {
        // a &&= b: only assigns when the test passes; the result is the value seen
        struct lval lv;
        lval_prepare(c, t, &lv);
        int parts = c->sp;
        lval_get(c, &lv);
        op(c, OP_DUP, 1);
        uint32_t skip;
        if (o == T_AND_ASSIGN) skip = jump(c, OP_JF, -1);
        else if (o == T_OR_ASSIGN) skip = jump(c, OP_JT, -1);
        else { op(c, OP_IS_NULLISH, 0); skip = jump(c, OP_JF, -1); }
        op(c, OP_POP, -1);
        if (t->type == N_IDENT) comp_expr_named(c, n->b, t->u.str);
        else comp_expr(c, n->b);
        lval_put(c, &lv);
        uint32_t done = jump(c, OP_JMP, 0);
        int done_sp = c->sp;
        patch_here(c, skip);
        // not assigned: drop the target parts below the value
        int below = parts - (done_sp - 1);
        (void)below;
        // stack here: [parts..., value]; remove the parts
        int nparts = lval_parts(&lv);
        for (int i = 0; i < nparts; i++) op(c, OP_NIP, -1);
        patch_here(c, done);
        c->sp = done_sp;
        return;
    }
    // compound: a op= b
    struct lval lv;
    lval_prepare(c, t, &lv);
    lval_get(c, &lv);
    comp_expr(c, n->b);
    int bc = binop_code(o);
    op(c, bc, -1);
    lval_put(c, &lv);
}

static void comp_update(struct cfunc* c, struct node* n) {
    struct lval lv;
    int inc = n->op == T_INC;
    int prefix = (n->flags & NF_PREFIX) != 0;
    lval_prepare(c, n->a, &lv);
    lval_get(c, &lv);
    if (prefix) {
        op(c, inc ? OP_INC : OP_DEC, 0);   // INC/DEC apply ToNumeric themselves
        lval_put(c, &lv);
        return;
    }
    op(c, OP_TO_NUMERIC, 0);
    // postfix: result is the old (numeric) value
    int nparts = lval_parts(&lv);
    op(c, OP_DUP, 1);
    if (nparts) op8(c, OP_INSERT, (uint32_t)(nparts + 1), 0);   // [parts, old, old] -> [old, parts, old]
    op(c, inc ? OP_INC : OP_DEC, 0);
    lval_put(c, &lv);
    op(c, OP_POP, -1);
}

// an expression whose value is unused (statements, for-loop clauses, comma operands):
// x++ becomes ++x (same effects, no old value kept); the POP merges into a store
static void comp_expr_void(struct cfunc* c, struct node* n) {
    if (n->type == N_UPDATE && !(n->flags & NF_PREFIX)) {
        mark_pos(c, n->pos);
        struct lval lv;
        lval_prepare(c, n->a, &lv);
        lval_get(c, &lv);
        op(c, n->op == T_INC ? OP_INC : OP_DEC, 0);
        lval_put(c, &lv);
    } else comp_expr(c, n);
    op(c, OP_POP, -1);
}

// ---------------------------------------------------------------- unary

static void comp_delete(struct cfunc* c, struct node* n) {
    struct node* t = n->a;
    while (t->type == N_OPTCHAIN && 0) t = t->a;
    if (t->type == N_IDENT && !(t->flags & NF_PAREN && 0)) {
        struct ref r;
        resolve(c, t->u.str, t->scope, &r);
        if (r.kind == R_GLOBAL) { op32(c, OP_DELETE_GLOBAL, atom_const(c, t->u.str), 1); return; }
        if (r.kind == R_WITH) {
            struct patch* found = 0;
            for (int i = 0; i < r.with_count; i++) {
                push_with_obj(c, t->scope, i);
                op32(c, OP_WITH_DELETE, atom_const(c, r.name), 0);
                uint32_t at = c->len;
                put32(c, 0);
                add_patch(c, &found, at);
                adjust(c, -1);
            }
            if (r.d) op(c, OP_FALSE_, 1);
            else op32(c, OP_DELETE_GLOBAL, atom_const(c, t->u.str), 1);
            patch_list(c, found, here(c));
            return;
        }
        op(c, OP_FALSE_, 1);   // declared bindings cannot be deleted
        return;
    }
    if (t->type == N_MEMBER) {
        if (t->a->type == N_SUPER) {
            // delete super.x: ReferenceError (after evaluating the key and this)
            if (t->flags & NF_COMPUTED) { comp_expr(c, t->b); op(c, OP_POP, -1); }
            comp_this(c, t->pos);
            op(c, OP_POP, -1);
            throw_err(c, NE_REFERENCE, "Unsupported reference to 'super'");
            op(c, OP_TRUE_, 1);
            return;
        }
        comp_expr(c, t->a);
        if (t->flags & NF_COMPUTED) { comp_expr(c, t->b); op(c, OP_DELETE_ELEM, -1); }
        else op32(c, OP_DELETE_FIELD, atom_const(c, t->u.str), 0);
        return;
    }
    if (t->type == N_OPTCHAIN) {
        // delete a?.b : true when short-circuited
        struct chain ch;
        ch.base_sp = c->sp;
        ch.to_end = 0;
        struct node* m = t->a;
        if (m->type == N_MEMBER) {
            comp_chain_elem(c, m->a, &ch, 0);
            if (m->flags & NF_OPTIONAL) chain_check(c, &ch, 0);
            if (m->flags & NF_COMPUTED) { comp_expr(c, m->b); op(c, OP_DELETE_ELEM, -1); }
            else op32(c, OP_DELETE_FIELD, atom_const(c, m->u.str), 0);
        } else { comp_expr(c, t); op(c, OP_POP, -1); op(c, OP_TRUE_, 1); }
        uint32_t over = jump(c, OP_JMP, 0);
        patch_list(c, ch.to_end, here(c));
        c->sp = ch.base_sp + 1;
        op(c, OP_POP, -1);
        op(c, OP_TRUE_, 1);
        patch_here(c, over);
        return;
    }
    comp_expr(c, t);
    op(c, OP_POP, -1);
    op(c, OP_TRUE_, 1);
}

static void comp_unary(struct cfunc* c, struct node* n) {
    switch (n->op) {
    case T_DELETE: comp_delete(c, n); return;
    case T_TYPEOF:
        if (n->a->type == N_IDENT) load_name(c, n->a->u.str, n->a->scope, n->a->pos, 1);
        else comp_expr(c, n->a);
        op(c, OP_TYPEOF, 0);
        return;
    case T_VOID: comp_expr(c, n->a); op(c, OP_POP, -1); op(c, OP_UNDEF, 1); return;
    case T_MINUS: comp_expr(c, n->a); op(c, OP_NEG, 0); return;
    case T_PLUS: comp_expr(c, n->a); op(c, OP_PLUS, 0); return;
    case T_TILDE: comp_expr(c, n->a); op(c, OP_BITNOT, 0); return;
    case T_NOT: comp_expr(c, n->a); op(c, OP_NOT, 0); return;
    }
}

// ---------------------------------------------------------------- expressions

static void comp_logical(struct cfunc* c, struct node* n) {
    comp_expr(c, n->a);
    uint32_t j;
    if (n->op == T_AND) j = jump(c, OP_JF_KEEP, 0);
    else if (n->op == T_OR) j = jump(c, OP_JT_KEEP, 0);
    else j = jump(c, OP_JNOTNULLISH_KEEP, 0);
    op(c, OP_POP, -1);
    comp_expr(c, n->b);
    patch_here(c, j);
}

static void comp_expr_named(struct cfunc* c, struct node* n, struct str* name) {
    // NamedEvaluation: anonymous functions/classes take the binding's name
    if (name && is_anon_fn(n)) {
        if (n->type == N_FUNC) {
            if (!n->u.fn->name) n->u.fn->name = name;
            comp_function_node(c, n);
        } else comp_class(c, n, name);
        return;
    }
    comp_expr(c, n);
}

static void comp_expr(struct cfunc* c, struct node* n) {
    if (c->failed || !n) return;
    switch (n->type) {
    case N_NUM: push_const(c, jv_number(n->u.num)); return;
    case N_STR: push_str(c, n->u.str); return;
    case N_BIGINT: {
        jv b = bigint_literal(c->J, n->u.str);
        if (b == JV_EXC) { c->failed = 1; return; }
        op16(c, OP_CONST, add_const(c, b), 1);
        return;
    }
    case N_TEMPLATE: comp_template(c, n); return;
    case N_TAGGED: {
        // tag(templateObject, ...subs): one frozen template object per site,
        // created here (one realm per runtime)
        jv tobj = template_object(c, n);
        if (tobj == JV_EXC) { c->failed = 1; return; }
        if (n->a->type == N_MEMBER || n->a->type == N_IDENT) comp_callee(c, n->a);
        else { comp_expr(c, n->a); op(c, OP_UNDEF, 1); }
        op16(c, OP_CONST, add_const(c, tobj), 1);
        int argc = 1;
        for (struct node* e = n->c; e; e = e->next) { comp_expr(c, e); argc++; }
        emit_call(c, argc, 0);
        return;
    }
    case N_REGEXP: op16_16(c, OP_REGEXP, atom_const(c, atom_str(c->J, n->u.str)), atom_const(c, atom_str(c->J, n->str2)), 1); return;
    case N_NULL: op(c, OP_NULL_, 1); return;
    case N_TRUE: op(c, OP_TRUE_, 1); return;
    case N_FALSE: op(c, OP_FALSE_, 1); return;
    case N_THIS: comp_this(c, n->pos); return;
    case N_IDENT: mark_pos(c, n->pos); load_name(c, n->u.str, n->scope, n->pos, 0); return;
    case N_NEW_TARGET: load_pseudo(c, "new.target", n->pos); return;
    case N_IMPORT_META: op(c, OP_IMPORT_META, 1); return;
    case N_ARRAY: comp_array(c, n); return;
    case N_OBJECT: comp_object(c, n); return;
    case N_FUNC: comp_function_node(c, n); return;
    case N_CLASS: comp_class(c, n, 0); return;
    case N_UNARY: comp_unary(c, n); return;
    case N_UPDATE: mark_pos(c, n->pos); comp_update(c, n); return;
    case N_BINARY:
        comp_expr(c, n->a);
        comp_expr(c, n->b);
        mark_pos(c, n->pos);
        op(c, binop_code(n->op), -1);
        return;
    case N_PRIVATE_IN:
        // #x in obj
        comp_expr(c, n->a);
        load_name(c, priv_atom(c, n->u.str), n->scope, n->pos, 0);
        op(c, OP_PRIVATE_IN, -1);
        return;
    case N_LOGICAL: comp_logical(c, n); return;
    case N_ASSIGN: mark_pos(c, n->pos); comp_assign(c, n); return;
    case N_COND: {
        comp_expr(c, n->a);
        uint32_t jf = jump(c, OP_JF, -1);
        comp_expr(c, n->b);
        uint32_t je = jump(c, OP_JMP, 0);
        adjust(c, -1);
        patch_here(c, jf);
        comp_expr(c, n->c);
        patch_here(c, je);
        return;
    }
    case N_CALL: mark_pos(c, n->pos); comp_call(c, n); return;
    case N_NEW: {
        mark_pos(c, n->pos);
        comp_expr(c, n->a);
        op(c, OP_DUP, 1);   // new.target = the constructor
        int argc = comp_args(c, n->b, (n->flags & NF_HAS_SPREAD) != 0);
        mark_call(c, n, n->a);
        if (argc < 0) op(c, OP_NEW_SPREAD, -2);
        else op16(c, OP_NEW, (uint32_t)argc, -(argc + 1));
        return;
    }
    case N_MEMBER:
        mark_pos(c, n->pos);
        if (n->a->type == N_SUPER) {
            super_ref(c, n);
            op(c, OP_GET_SUPER, -2);
            return;
        }
        comp_expr(c, n->a);
        if (n->flags & NF_PRIVATE) {
            load_name(c, priv_atom(c, n->u.str), n->scope, n->pos, 0);
            op(c, OP_GET_PRIVATE, -1);
        } else if (n->flags & NF_COMPUTED) {
            comp_expr(c, n->b);
            op(c, OP_GET_ELEM, -1);
        } else get_field(c, n->u.str);
        return;
    case N_OPTCHAIN: comp_optchain(c, n, 0); return;
    case N_SEQ:
        for (struct node* e = n->a; e; e = e->next) {
            if (e->next) comp_expr_void(c, e);
            else comp_expr(c, e);
        }
        return;
    case N_YIELD:
        mark_pos(c, n->pos);
        if (n->flags & NF_DELEGATE) {
            comp_expr(c, n->a);
            op(c, (c->fi->flags & FI_ASYNC) ? OP_GET_ASYNC_ITER : OP_GET_ITER, 0);
            op(c, OP_UNDEF, 1);                   // first value sent: undefined
            op(c, OP_YIELD_STAR, -1);             // [rec, v] -> [result]
            return;
        }
        if (n->a) comp_expr(c, n->a); else op(c, OP_UNDEF, 1);
        if (c->fi->flags & FI_ASYNC) op(c, OP_AWAIT, 0);   // async generators await the operand
        op(c, (c->fi->flags & FI_ASYNC) ? OP_ASYNC_GEN_YIELD : OP_YIELD, 0);
        return;
    case N_AWAIT:
        mark_pos(c, n->pos);
        comp_expr(c, n->a);
        op(c, OP_AWAIT, 0);
        return;
    case N_IMPORT_CALL:
        comp_expr(c, n->a);
        if (n->b) { comp_expr(c, n->b); op8(c, OP_IMPORT_CALL, 1, -1); }
        else op8(c, OP_IMPORT_CALL, 0, 0);
        return;
    case N_SPREAD:
        cerr(c, n->pos, "unexpected spread");
        return;
    default:
        cerr(c, n->pos, "internal: cannot compile expression");
        return;
    }
}

#include "compile2.c"
