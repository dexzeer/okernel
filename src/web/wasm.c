// wasm.c - WebAssembly decoder, validator, compiler and interpreter (see wasm.h).

#include "wasm.h"
#include "wcommon.h"

#if defined(KERNEL) && KERNEL
double sqrt(double);
#else
#include <math.h>
#endif

// ---------------------------------------------------------------------------
// reader

struct dec {
    const uint8_t* p;
    const uint8_t* end;
    const char* err;
    struct wasm_module* m;
    struct wasm_store* st;
    // validator state for the function being compiled
    uint8_t* vt; int nvt, capvt;                 // operand type stack
    struct ctl* ct; int nct, capct;              // control stack
    uint32_t* out; uint32_t nout, capout;        // code under construction
    uint8_t* lt; uint32_t nlocals_total;         // local types (params first)
    uint32_t cur_np, cur_nr;
    const struct wasm_type* cur_type;
    uint32_t max_stack;
    uint8_t* is_ref;                             // function indices usable by ref.func
    uint32_t total_funcs, total_tables, total_mems, total_globals;
    uint8_t* gtypes; uint8_t* gmuts;             // every global (imports first)
    uint8_t* ttypes;                             // table element types
    uint8_t* ftypes_idx_ok;
    uint32_t* all_func_types;                    // type index of every function (imports first)
    int data_count_seen; uint32_t data_count;
};

static int rebuild_spaces(struct dec* d);
static int store_add(struct wasm_store* s, void* p, uint8_t kind);
#define FAIL(msg) do { d->err = (msg); return -1; } while (0)
#define NEED(cond, msg) do { if (!(cond)) FAIL(msg); } while (0)

static int rd_byte(struct dec* d, uint8_t* v) { NEED(d->p < d->end, "unexpected end"); *v = *d->p++; return 0; }

static int rd_u32(struct dec* d, uint32_t* v) {
    uint32_t r = 0;
    for (int i = 0; i < 5; i++) {
        NEED(d->p < d->end, "unexpected end");
        uint8_t b = *d->p++;
        if (i == 4 && (b & 0xF0)) FAIL("integer too large");
        r |= (uint32_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *v = r; return 0; }
    }
    FAIL("integer representation too long");
}

static int rd_s32(struct dec* d, int32_t* v) {
    uint32_t r = 0;
    for (int i = 0; i < 5; i++) {
        NEED(d->p < d->end, "unexpected end");
        uint8_t b = *d->p++;
        r |= (uint32_t)(b & 0x7F) << (7 * i);
        if (i == 4) {
            if (b & 0x80) FAIL("integer representation too long");
            uint8_t hi = b & 0x70;               // bits beyond 32: must be a sign extension
            if (!((b & 0x08) ? hi == 0x70 : hi == 0)) FAIL("integer too large");
        }
        if (!(b & 0x80)) {
            if (i < 4 && (b & 0x40)) r |= ~0u << (7 * (i + 1));
            *v = (int32_t)r;
            return 0;
        }
    }
    FAIL("integer representation too long");
}

static int rd_s64(struct dec* d, int64_t* v) {
    uint64_t r = 0;
    for (int i = 0; i < 10; i++) {
        NEED(d->p < d->end, "unexpected end");
        uint8_t b = *d->p++;
        r |= (uint64_t)(b & 0x7F) << (7 * i);
        if (i == 9) {
            if (b & 0x80) FAIL("integer representation too long");
            if (!((b & 0x01) ? (b & 0x7E) == 0x7E : (b & 0x7E) == 0)) FAIL("integer too large");
        }
        if (!(b & 0x80)) {
            if (i < 9 && (b & 0x40)) r |= ~(uint64_t)0 << (7 * (i + 1));
            *v = (int64_t)r;
            return 0;
        }
    }
    FAIL("integer representation too long");
}

static int rd_s33(struct dec* d, int64_t* v) {      // block type: a signed 33-bit LEB
    uint64_t r = 0;
    for (int i = 0; i < 5; i++) {
        NEED(d->p < d->end, "unexpected end");
        uint8_t b = *d->p++;
        r |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            if (b & 0x40) r |= ~(uint64_t)0 << (7 * (i + 1));
            *v = (int64_t)r;
            return 0;
        }
    }
    FAIL("integer representation too long");
}

static int rd_f32(struct dec* d, uint32_t* v) { NEED(d->end - d->p >= 4, "unexpected end"); memcpy(v, d->p, 4); d->p += 4; return 0; }
static int rd_f64(struct dec* d, uint64_t* v) { NEED(d->end - d->p >= 8, "unexpected end"); memcpy(v, d->p, 8); d->p += 8; return 0; }

static int utf8_valid(const uint8_t* s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        uint32_t need = 0, cp = 0;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return 0;
        if (i + need >= n + 0 && i + need > n - 1 + 0) { if (i + need >= n + 1) return 0; }
        if (i + need >= n + 1) return 0;
        for (uint32_t k = 1; k <= need; k++) {
            if (i + k >= n || (s[i + k] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += need + 1;
    }
    return 1;
}

static int rd_name(struct dec* d, char** out) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED((uint32_t)(d->end - d->p) >= n, "unexpected end");
    NEED(utf8_valid(d->p, n), "malformed UTF-8 encoding");
    char* s = (char*)w_malloc(n + 1);
    NEED(s, "out of memory");
    memcpy(s, d->p, n);
    s[n] = 0;
    d->p += n;
    *out = s;
    return 0;
}

static int valtype_ok(struct dec* d, uint8_t t) {
    if (t == WT_I32 || t == WT_I64 || t == WT_F32 || t == WT_F64 || t == WT_FUNCREF || t == WT_EXTERNREF) return 0;
    if (t == 0x7B) FAIL("SIMD is not supported");
    FAIL("invalid value type");
}
static int rd_valtype(struct dec* d, uint8_t* t) { if (rd_byte(d, t) < 0) return -1; return valtype_ok(d, *t); }
static int rd_reftype(struct dec* d, uint8_t* t) {
    if (rd_byte(d, t) < 0) return -1;
    NEED(*t == WT_FUNCREF || *t == WT_EXTERNREF, "invalid reference type");
    return 0;
}

static int rd_limits(struct dec* d, struct wasm_limits* l, uint32_t cap, const char* what) {
    uint8_t f;
    if (rd_byte(d, &f) < 0) return -1;
    NEED(f <= 1, f <= 3 ? "shared memory (threads) is not supported" : "invalid limits flags");
    if (rd_u32(d, &l->min) < 0) return -1;
    l->has_max = f & 1;
    l->max = 0;
    if (l->has_max && rd_u32(d, &l->max) < 0) return -1;
    NEED(l->min <= cap, what);
    if (l->has_max) { NEED(l->max <= cap, what); NEED(l->max >= l->min, "size minimum must not be greater than maximum"); }
    return 0;
}

// ---------------------------------------------------------------------------
// growable arrays

#define GROW(arr, n, cap, T) do { if ((uint32_t)(n) >= (uint32_t)(cap)) { uint32_t nc = (cap) ? (uint32_t)(cap) * 2 : 16; T* na = (T*)w_realloc((arr), nc * sizeof(T)); if (!na) { d->err = "out of memory"; return -1; } (arr) = na; (cap) = nc; } } while (0)

static int out_word(struct dec* d, uint32_t w) {
    GROW(d->out, d->nout, d->capout, uint32_t);
    d->out[d->nout++] = w;
    return 0;
}

// ---------------------------------------------------------------------------
// control stack and operand type stack for the validator

struct ctl {
    uint8_t kind;               // 0 function, 1 block, 2 loop, 3 if, 4 else
    uint8_t unreachable;
    const uint8_t* params; int np;
    const uint8_t* results; int nr;
    int height;                 // operand stack height at entry, below the params
    uint32_t start_pc;          // loop target
    uint32_t* fix; int nfix, capfix;   // forward branches to patch with the end pc
    uint32_t if_patch;          // position of the false-branch target of an `if`
    int has_if_patch;
    uint8_t* tmp_types;         // owned storage for single-valtype block types
};

static int vt_push(struct dec* d, uint8_t t) {
    GROW(d->vt, d->nvt, d->capvt, uint8_t);
    d->vt[d->nvt++] = t;
    if ((uint32_t)d->nvt > d->max_stack) d->max_stack = (uint32_t)d->nvt;
    return 0;
}

static int vt_pop(struct dec* d, uint8_t* t) {         // *t = 0 when unknown
    struct ctl* c = &d->ct[d->nct - 1];
    if (d->nvt == c->height) {
        if (c->unreachable) { *t = 0; return 0; }
        FAIL("type mismatch: not enough operands");
    }
    *t = d->vt[--d->nvt];
    return 0;
}

static int vt_pop_expect(struct dec* d, uint8_t want) {
    uint8_t got;
    if (vt_pop(d, &got) < 0) return -1;
    NEED(got == 0 || want == 0 || got == want, "type mismatch");
    return 0;
}

static int vt_pop_vals(struct dec* d, const uint8_t* ts, int n) {
    for (int i = n - 1; i >= 0; i--) if (vt_pop_expect(d, ts[i]) < 0) return -1;
    return 0;
}
static int vt_push_vals(struct dec* d, const uint8_t* ts, int n) {
    for (int i = 0; i < n; i++) if (vt_push(d, ts[i]) < 0) return -1;
    return 0;
}

static int ctl_push(struct dec* d, uint8_t kind, const uint8_t* params, int np, const uint8_t* results, int nr, uint8_t* owned) {
    GROW(d->ct, d->nct, d->capct, struct ctl);
    struct ctl* c = &d->ct[d->nct++];
    memset(c, 0, sizeof *c);
    c->kind = kind; c->params = params; c->np = np; c->results = results; c->nr = nr;
    c->height = d->nvt;
    c->tmp_types = owned;
    if (vt_push_vals(d, params, np) < 0) return -1;
    return 0;
}

static void ctl_free(struct ctl* c) { w_free(c->fix); w_free(c->tmp_types); c->fix = 0; c->tmp_types = 0; }

static void set_unreachable(struct dec* d) {
    struct ctl* c = &d->ct[d->nct - 1];
    d->nvt = c->height;
    c->unreachable = 1;
}

static int label_ctl(struct dec* d, uint32_t depth, struct ctl** out) {
    NEED(depth < (uint32_t)d->nct, "unknown label");
    *out = &d->ct[d->nct - 1 - depth];
    return 0;
}
static const uint8_t* label_types(struct ctl* c, int* n) { if (c->kind == 2) { *n = c->np; return c->params; } *n = c->nr; return c->results; }

// emit a branch target triple (target, height, arity) for label `c`; forward targets are patched at `end`
static int emit_branch_triple(struct dec* d, struct ctl* c) {
    int n;
    label_types(c, &n);
    uint32_t pos = d->nout;
    if (out_word(d, 0) < 0 || out_word(d, (uint32_t)c->height) < 0 || out_word(d, (uint32_t)n) < 0) return -1;
    if (c->kind == 2) d->out[pos] = c->start_pc;
    else {
        GROW(c->fix, c->nfix, c->capfix, uint32_t);
        c->fix[c->nfix++] = pos;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// constant expressions

static int rd_const_expr(struct dec* d, uint8_t want, struct wasm_const* c) {
    uint8_t op;
    if (rd_byte(d, &op) < 0) return -1;
    uint8_t got = 0;
    switch (op) {
    case 0x41: { int32_t v; if (rd_s32(d, &v) < 0) return -1; c->kind = 0; c->v = (uint32_t)v; got = WT_I32; break; }
    case 0x42: { int64_t v; if (rd_s64(d, &v) < 0) return -1; c->kind = 1; c->v = (uint64_t)v; got = WT_I64; break; }
    case 0x43: { uint32_t v; if (rd_f32(d, &v) < 0) return -1; c->kind = 2; c->v = v; got = WT_F32; break; }
    case 0x44: { uint64_t v; if (rd_f64(d, &v) < 0) return -1; c->kind = 3; c->v = v; got = WT_F64; break; }
    case 0x23: {
        uint32_t g;
        if (rd_u32(d, &g) < 0) return -1;
        NEED(g < d->m->imp_globals && g < d->total_globals, "unknown global");
        NEED(!d->gmuts[g], "constant expression required");
        c->kind = 4; c->v = g; got = d->gtypes[g];
        break;
    }
    case 0xD0: { uint8_t t; if (rd_reftype(d, &t) < 0) return -1; c->kind = 5; c->v = t; got = t; break; }
    case 0xD2: {
        uint32_t f;
        if (rd_u32(d, &f) < 0) return -1;
        NEED(f < d->total_funcs, "unknown function");
        c->kind = 6; c->v = f; got = WT_FUNCREF;
        if (d->is_ref) d->is_ref[f] = 1;
        break;
    }
    default: FAIL("constant expression required");
    }
    uint8_t end;
    if (rd_byte(d, &end) < 0) return -1;
    NEED(end == 0x0B, "constant expression required");
    NEED(got == want, "type mismatch in constant expression");
    return 0;
}

// ---------------------------------------------------------------------------
// function body validation + compilation

enum { OP_UNREACHABLE = 0x00, OP_NOP = 0x01, OP_IF = 0x04, OP_BR = 0x0C, OP_BR_IF = 0x0D, OP_BR_TABLE = 0x0E, OP_RETURN = 0x0F,
       OP_CALL = 0x10, OP_CALL_INDIRECT = 0x11, OP_DROP = 0x1A, OP_SELECT = 0x1B };

// numeric opcode signature: returns 1 and fills in/out for plain stack operators
static int numeric_sig(uint8_t op, uint8_t* a, uint8_t* b, int* nin, uint8_t* out) {
    *a = *b = 0; *nin = 0; *out = 0;
    #define SIG1(x, o) do { *a = (x); *nin = 1; *out = (o); return 1; } while (0)
    #define SIG2(x, o) do { *a = (x); *b = (x); *nin = 2; *out = (o); return 1; } while (0)
    if (op == 0x45) SIG1(WT_I32, WT_I32);
    if (op >= 0x46 && op <= 0x4F) SIG2(WT_I32, WT_I32);
    if (op == 0x50) SIG1(WT_I64, WT_I32);
    if (op >= 0x51 && op <= 0x5A) SIG2(WT_I64, WT_I32);
    if (op >= 0x5B && op <= 0x60) SIG2(WT_F32, WT_I32);
    if (op >= 0x61 && op <= 0x66) SIG2(WT_F64, WT_I32);
    if (op >= 0x67 && op <= 0x69) SIG1(WT_I32, WT_I32);
    if (op >= 0x6A && op <= 0x78) SIG2(WT_I32, WT_I32);
    if (op >= 0x79 && op <= 0x7B) SIG1(WT_I64, WT_I64);
    if (op >= 0x7C && op <= 0x8A) SIG2(WT_I64, WT_I64);
    if (op >= 0x8B && op <= 0x91) SIG1(WT_F32, WT_F32);
    if (op >= 0x92 && op <= 0x98) SIG2(WT_F32, WT_F32);
    if (op >= 0x99 && op <= 0x9F) SIG1(WT_F64, WT_F64);
    if (op >= 0xA0 && op <= 0xA6) SIG2(WT_F64, WT_F64);
    if (op == 0xA7) SIG1(WT_I64, WT_I32);
    if (op == 0xA8 || op == 0xA9) SIG1(WT_F32, WT_I32);
    if (op == 0xAA || op == 0xAB) SIG1(WT_F64, WT_I32);
    if (op == 0xAC || op == 0xAD) SIG1(WT_I32, WT_I64);
    if (op == 0xAE || op == 0xAF) SIG1(WT_F32, WT_I64);
    if (op == 0xB0 || op == 0xB1) SIG1(WT_F64, WT_I64);
    if (op == 0xB2 || op == 0xB3) SIG1(WT_I32, WT_F32);
    if (op == 0xB4 || op == 0xB5) SIG1(WT_I64, WT_F32);
    if (op == 0xB6) SIG1(WT_F64, WT_F32);
    if (op == 0xB7 || op == 0xB8) SIG1(WT_I32, WT_F64);
    if (op == 0xB9 || op == 0xBA) SIG1(WT_I64, WT_F64);
    if (op == 0xBB) SIG1(WT_F32, WT_F64);
    if (op == 0xBC) SIG1(WT_F32, WT_I32);
    if (op == 0xBD) SIG1(WT_F64, WT_I64);
    if (op == 0xBE) SIG1(WT_I32, WT_F32);
    if (op == 0xBF) SIG1(WT_I64, WT_F64);
    if (op == 0xC0 || op == 0xC1) SIG1(WT_I32, WT_I32);
    if (op >= 0xC2 && op <= 0xC4) SIG1(WT_I64, WT_I64);
    #undef SIG1
    #undef SIG2
    return 0;
}

// memory access: opcode -> value type, access bytes (log2 of natural alignment), is store
static int mem_op_info(uint8_t op, uint8_t* vt, int* log2, int* store) {
    *store = op >= 0x36;
    switch (op) {
    case 0x28: *vt = WT_I32; *log2 = 2; return 1;
    case 0x29: *vt = WT_I64; *log2 = 3; return 1;
    case 0x2A: *vt = WT_F32; *log2 = 2; return 1;
    case 0x2B: *vt = WT_F64; *log2 = 3; return 1;
    case 0x2C: case 0x2D: *vt = WT_I32; *log2 = 0; return 1;
    case 0x2E: case 0x2F: *vt = WT_I32; *log2 = 1; return 1;
    case 0x30: case 0x31: *vt = WT_I64; *log2 = 0; return 1;
    case 0x32: case 0x33: *vt = WT_I64; *log2 = 1; return 1;
    case 0x34: case 0x35: *vt = WT_I64; *log2 = 2; return 1;
    case 0x36: *vt = WT_I32; *log2 = 2; return 1;
    case 0x37: *vt = WT_I64; *log2 = 3; return 1;
    case 0x38: *vt = WT_F32; *log2 = 2; return 1;
    case 0x39: *vt = WT_F64; *log2 = 3; return 1;
    case 0x3A: *vt = WT_I32; *log2 = 0; return 1;
    case 0x3B: *vt = WT_I32; *log2 = 1; return 1;
    case 0x3C: *vt = WT_I64; *log2 = 0; return 1;
    case 0x3D: *vt = WT_I64; *log2 = 1; return 1;
    case 0x3E: *vt = WT_I64; *log2 = 2; return 1;
    }
    return 0;
}

static int rd_blocktype(struct dec* d, const uint8_t** params, int* np, const uint8_t** results, int* nr, uint8_t** owned) {
    *owned = 0; *params = 0; *np = 0; *results = 0; *nr = 0;
    NEED(d->p < d->end, "unexpected end");
    if (*d->p == 0x40) { d->p++; return 0; }
    if (*d->p == WT_I32 || *d->p == WT_I64 || *d->p == WT_F32 || *d->p == WT_F64 || *d->p == WT_FUNCREF || *d->p == WT_EXTERNREF) {
        uint8_t* t = (uint8_t*)w_malloc(1);
        NEED(t, "out of memory");
        *t = *d->p++;
        *owned = t; *results = t; *nr = 1;
        return 0;
    }
    if (*d->p == 0x7B) FAIL("SIMD is not supported");
    int64_t idx;
    if (rd_s33(d, &idx) < 0) return -1;
    NEED(idx >= 0 && (uint64_t)idx < d->m->ntypes, "unknown type");
    const struct wasm_type* t = &d->m->types[idx];
    *params = t->p; *np = t->np; *results = t->r; *nr = t->nr;
    return 0;
}

static int check_mem0(struct dec* d) { NEED(d->total_mems > 0, "unknown memory"); return 0; }

static int rd_memarg(struct dec* d, int max_log2, uint32_t* offset) {
    uint32_t align;
    if (rd_u32(d, &align) < 0) return -1;
    NEED(align <= (uint32_t)max_log2, "alignment must not be larger than natural");
    if (rd_u32(d, offset) < 0) return -1;
    return 0;
}

static int end_block(struct dec* d, int at_else) {
    struct ctl* c = &d->ct[d->nct - 1];
    // the stack must hold exactly the results
    if (vt_pop_vals(d, c->results, c->nr) < 0) return -1;
    NEED(d->nvt == c->height, "type mismatch: values remaining at end of block");
    if (c->kind == 3 && !at_else) {            // if without else: params must equal results
        NEED(c->np == c->nr, "type mismatch: if without else");
        for (int i = 0; i < c->np; i++) NEED(c->params[i] == c->results[i], "type mismatch: if without else");
    }
    return 0;
}

static int compile_function(struct dec* d, uint32_t fidx, const uint8_t* body, uint32_t blen, struct wasm_code* code) {
    const struct wasm_type* ft = &d->m->types[d->all_func_types[d->m->imp_funcs + fidx]];
    const uint8_t* save_p = d->p; const uint8_t* save_end = d->end;
    d->p = body; d->end = body + blen;
    d->nvt = 0; d->nct = 0; d->nout = 0; d->max_stack = 0;
    d->cur_type = ft; d->cur_np = ft->np; d->cur_nr = ft->nr;
    // locals
    uint32_t nloc_groups;
    if (rd_u32(d, &nloc_groups) < 0) return -1;
    uint64_t total = ft->np;
    for (uint32_t i = 0; i < ft->np; i++) d->lt[i] = ft->p[i];
    for (uint32_t g = 0; g < nloc_groups; g++) {
        uint32_t cnt; uint8_t t;
        if (rd_u32(d, &cnt) < 0 || rd_valtype(d, &t) < 0) return -1;
        total += cnt;
        NEED(total <= 50000, "too many locals");
        for (uint32_t k = 0; k < cnt; k++) d->lt[total - cnt + k] = t;
    }
    d->nlocals_total = (uint32_t)total;
    if (ctl_push(d, 0, 0, 0, ft->r, (int)ft->nr, 0) < 0) return -1;
    d->ct[0].height = 0;
    d->nvt = 0;
    for (;;) {
        if (d->p >= d->end) {
            // the function's own `end` must have been seen
            FAIL("unexpected end of function body");
        }
        uint8_t op = *d->p++;
        uint8_t ta, tb, to; int nin;
        switch (op) {
        case 0x00: if (out_word(d, OP_UNREACHABLE) < 0) return -1; set_unreachable(d); break;
        case 0x01: break;
        case 0x02: case 0x03: {
            const uint8_t *p, *r; int np, nr; uint8_t* owned;
            if (rd_blocktype(d, &p, &np, &r, &nr, &owned) < 0) return -1;
            if (vt_pop_vals(d, p, np) < 0) { w_free(owned); return -1; }
            // the params are re-pushed by ctl_push; height excludes them
            if (ctl_push(d, op == 0x02 ? 1 : 2, p, np, r, nr, owned) < 0) return -1;
            d->ct[d->nct - 1].height = d->nvt - np;
            if (op == 0x03) d->ct[d->nct - 1].start_pc = d->nout;
            break;
        }
        case 0x04: {
            const uint8_t *p, *r; int np, nr; uint8_t* owned;
            if (rd_blocktype(d, &p, &np, &r, &nr, &owned) < 0) return -1;
            if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_vals(d, p, np) < 0) { w_free(owned); return -1; }
            if (ctl_push(d, 3, p, np, r, nr, owned) < 0) return -1;
            d->ct[d->nct - 1].height = d->nvt - np;
            if (out_word(d, OP_IF) < 0) return -1;
            d->ct[d->nct - 1].if_patch = d->nout;
            d->ct[d->nct - 1].has_if_patch = 1;
            if (out_word(d, 0) < 0) return -1;
            break;
        }
        case 0x05: {
            struct ctl* c = &d->ct[d->nct - 1];
            NEED(c->kind == 3, "else without if");
            if (end_block(d, 1) < 0) return -1;
            // fall-through of the then-branch jumps to the end
            if (out_word(d, OP_BR) < 0) return -1;
            uint32_t pos = d->nout;
            if (out_word(d, 0) < 0 || out_word(d, (uint32_t)c->height) < 0 || out_word(d, (uint32_t)c->nr) < 0) return -1;
            GROW(c->fix, c->nfix, c->capfix, uint32_t);
            c->fix[c->nfix++] = pos;
            d->out[c->if_patch] = d->nout;      // false branch lands here
            c->has_if_patch = 0;
            c->kind = 4;
            d->nvt = c->height;
            c->unreachable = 0;
            if (vt_push_vals(d, c->params, c->np) < 0) return -1;
            break;
        }
        case 0x0B: {
            struct ctl* c = &d->ct[d->nct - 1];
            if (end_block(d, 0) < 0) return -1;
            if (c->has_if_patch) d->out[c->if_patch] = d->nout;
            if (c->kind == 0) {
                // end of the function
                if (out_word(d, OP_RETURN) < 0) return -1;
                for (int i = 0; i < c->nfix; i++) d->out[c->fix[i]] = d->nout - 1;     // branches to the function label return
                ctl_free(c);
                d->nct--;
                NEED(d->p == d->end, "operators remaining after end of function");
                goto done;
            }
            for (int i = 0; i < c->nfix; i++) d->out[c->fix[i]] = d->nout;
            if (vt_push_vals(d, c->results, c->nr) < 0) return -1;
            ctl_free(c);
            d->nct--;
            break;
        }
        case 0x0C: {
            uint32_t l; struct ctl* c;
            if (rd_u32(d, &l) < 0 || label_ctl(d, l, &c) < 0) return -1;
            int n; const uint8_t* ts = label_types(c, &n);
            if (vt_pop_vals(d, ts, n) < 0) return -1;
            if (out_word(d, OP_BR) < 0 || emit_branch_triple(d, c) < 0) return -1;
            set_unreachable(d);
            break;
        }
        case 0x0D: {
            uint32_t l; struct ctl* c;
            if (rd_u32(d, &l) < 0 || label_ctl(d, l, &c) < 0) return -1;
            if (vt_pop_expect(d, WT_I32) < 0) return -1;
            int n; const uint8_t* ts = label_types(c, &n);
            if (vt_pop_vals(d, ts, n) < 0) return -1;
            if (vt_push_vals(d, ts, n) < 0) return -1;
            if (out_word(d, OP_BR_IF) < 0 || emit_branch_triple(d, c) < 0) return -1;
            break;
        }
        case 0x0E: {
            uint32_t cnt;
            if (rd_u32(d, &cnt) < 0) return -1;
            NEED(cnt < 1000000 && (uint64_t)cnt <= (uint64_t)(d->end - d->p), "too many br_table targets");
            if (vt_pop_expect(d, WT_I32) < 0) return -1;
            if (out_word(d, OP_BR_TABLE) < 0 || out_word(d, cnt) < 0) return -1;
            int arity = -1;
            struct ctl* defc = 0;
            for (uint32_t i = 0; i <= cnt; i++) {
                uint32_t l; struct ctl* c;
                if (rd_u32(d, &l) < 0 || label_ctl(d, l, &c) < 0) return -1;
                int n; const uint8_t* ts = label_types(c, &n);
                if (arity < 0) arity = n;
                NEED(n == arity, "type mismatch: br_table arity");
                // every target must accept the operands (checked by popping against a copy of the stack)
                int saved = d->nvt;
                NEED(n <= 64, "br_table arity too large");
                struct ctl* top = &d->ct[d->nct - 1];
                for (int k = n - 1; k >= 0; k--) {
                    uint8_t got;
                    if (d->nvt == top->height) { if (top->unreachable) got = 0; else FAIL("type mismatch: not enough operands"); }
                    else got = d->vt[--d->nvt];
                    NEED(got == 0 || got == ts[k], "type mismatch in br_table");
                }
                d->nvt = saved;
                if (emit_branch_triple(d, c) < 0) return -1;
                defc = c;
            }
            int n; const uint8_t* ts = label_types(defc, &n);
            if (vt_pop_vals(d, ts, n) < 0) return -1;
            set_unreachable(d);
            break;
        }
        case 0x0F: {
            if (vt_pop_vals(d, d->cur_type->r, (int)d->cur_nr) < 0) return -1;
            if (out_word(d, OP_RETURN) < 0) return -1;
            set_unreachable(d);
            break;
        }
        case 0x10: {
            uint32_t f;
            if (rd_u32(d, &f) < 0) return -1;
            NEED(f < d->total_funcs, "unknown function");
            const struct wasm_type* t = &d->m->types[d->all_func_types[f]];
            if (vt_pop_vals(d, t->p, t->np) < 0 || vt_push_vals(d, t->r, t->nr) < 0) return -1;
            if (out_word(d, OP_CALL) < 0 || out_word(d, f) < 0) return -1;
            break;
        }
        case 0x11: {
            uint32_t ti, tab;
            if (rd_u32(d, &ti) < 0 || rd_u32(d, &tab) < 0) return -1;
            NEED(ti < d->m->ntypes, "unknown type");
            NEED(tab < d->total_tables, "unknown table");
            NEED(d->ttypes[tab] == WT_FUNCREF, "call_indirect on a non-funcref table");
            const struct wasm_type* t = &d->m->types[ti];
            if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_vals(d, t->p, t->np) < 0 || vt_push_vals(d, t->r, t->nr) < 0) return -1;
            if (out_word(d, OP_CALL_INDIRECT) < 0 || out_word(d, ti) < 0 || out_word(d, tab) < 0) return -1;
            break;
        }
        case 0x1A: { uint8_t t; if (vt_pop(d, &t) < 0 || out_word(d, OP_DROP) < 0) return -1; break; }
        case 0x1B: case 0x1C: {
            uint8_t t1, t2, c;
            if (op == 0x1C) {
                uint32_t n;
                if (rd_u32(d, &n) < 0) return -1;
                NEED(n == 1, "invalid result arity for select");
                uint8_t t;
                if (rd_valtype(d, &t) < 0) return -1;
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, t) < 0 || vt_pop_expect(d, t) < 0 || vt_push(d, t) < 0) return -1;
            } else {
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop(d, &t1) < 0 || vt_pop(d, &t2) < 0) return -1;
                (void)c;
                NEED(t1 == 0 || t2 == 0 || t1 == t2, "type mismatch in select");
                uint8_t r = t1 ? t1 : t2;
                NEED(r == 0 || r == WT_I32 || r == WT_I64 || r == WT_F32 || r == WT_F64, "select needs a type annotation for references");
                if (vt_push(d, r) < 0) return -1;
            }
            if (out_word(d, OP_SELECT) < 0) return -1;
            break;
        }
        case 0x20: case 0x21: case 0x22: {
            uint32_t i;
            if (rd_u32(d, &i) < 0) return -1;
            NEED(i < d->nlocals_total, "unknown local");
            uint8_t t = d->lt[i];
            if (op == 0x20) { if (vt_push(d, t) < 0) return -1; }
            else if (op == 0x21) { if (vt_pop_expect(d, t) < 0) return -1; }
            else { if (vt_pop_expect(d, t) < 0 || vt_push(d, t) < 0) return -1; }
            if (out_word(d, op) < 0 || out_word(d, i) < 0) return -1;
            break;
        }
        case 0x23: case 0x24: {
            uint32_t g;
            if (rd_u32(d, &g) < 0) return -1;
            NEED(g < d->total_globals, "unknown global");
            if (op == 0x23) { if (vt_push(d, d->gtypes[g]) < 0) return -1; }
            else { NEED(d->gmuts[g], "global is immutable"); if (vt_pop_expect(d, d->gtypes[g]) < 0) return -1; }
            if (out_word(d, op) < 0 || out_word(d, g) < 0) return -1;
            break;
        }
        case 0x25: case 0x26: {
            uint32_t t;
            if (rd_u32(d, &t) < 0) return -1;
            NEED(t < d->total_tables, "unknown table");
            if (op == 0x25) { if (vt_pop_expect(d, WT_I32) < 0 || vt_push(d, d->ttypes[t]) < 0) return -1; }
            else { if (vt_pop_expect(d, d->ttypes[t]) < 0 || vt_pop_expect(d, WT_I32) < 0) return -1; }
            if (out_word(d, op) < 0 || out_word(d, t) < 0) return -1;
            break;
        }
        case 0x3F: case 0x40: {
            uint8_t z;
            if (rd_byte(d, &z) < 0) return -1;
            NEED(z == 0, "zero byte expected");
            if (check_mem0(d) < 0) return -1;
            if (op == 0x40) { if (vt_pop_expect(d, WT_I32) < 0) return -1; }
            if (vt_push(d, WT_I32) < 0 || out_word(d, op) < 0) return -1;
            break;
        }
        case 0x41: { int32_t v; if (rd_s32(d, &v) < 0 || vt_push(d, WT_I32) < 0 || out_word(d, 0x41) < 0 || out_word(d, (uint32_t)v) < 0) return -1; break; }
        case 0x42: { int64_t v; if (rd_s64(d, &v) < 0 || vt_push(d, WT_I64) < 0 || out_word(d, 0x42) < 0 || out_word(d, (uint32_t)v) < 0 || out_word(d, (uint32_t)((uint64_t)v >> 32)) < 0) return -1; break; }
        case 0x43: { uint32_t v; if (rd_f32(d, &v) < 0 || vt_push(d, WT_F32) < 0 || out_word(d, 0x43) < 0 || out_word(d, v) < 0) return -1; break; }
        case 0x44: { uint64_t v; if (rd_f64(d, &v) < 0 || vt_push(d, WT_F64) < 0 || out_word(d, 0x44) < 0 || out_word(d, (uint32_t)v) < 0 || out_word(d, (uint32_t)(v >> 32)) < 0) return -1; break; }
        case 0xD0: { uint8_t t; if (rd_reftype(d, &t) < 0 || vt_push(d, t) < 0 || out_word(d, 0xD0) < 0) return -1; break; }
        case 0xD1: {
            uint8_t t;
            if (vt_pop(d, &t) < 0) return -1;
            NEED(t == 0 || t == WT_FUNCREF || t == WT_EXTERNREF, "type mismatch: ref.is_null needs a reference");
            if (vt_push(d, WT_I32) < 0 || out_word(d, 0xD1) < 0) return -1;
            break;
        }
        case 0xD2: {
            uint32_t f;
            if (rd_u32(d, &f) < 0) return -1;
            NEED(f < d->total_funcs, "unknown function");
            NEED(d->is_ref[f], "undeclared function reference");
            if (vt_push(d, WT_FUNCREF) < 0 || out_word(d, 0xD2) < 0 || out_word(d, f) < 0) return -1;
            break;
        }
        case 0xFC: {
            uint32_t sub;
            if (rd_u32(d, &sub) < 0) return -1;
            if (sub <= 7) {
                static const uint8_t from[8] = { WT_F32, WT_F32, WT_F64, WT_F64, WT_F32, WT_F32, WT_F64, WT_F64 };
                static const uint8_t to_[8] = { WT_I32, WT_I32, WT_I32, WT_I32, WT_I64, WT_I64, WT_I64, WT_I64 };
                if (vt_pop_expect(d, from[sub]) < 0 || vt_push(d, to_[sub]) < 0 || out_word(d, 0x100 + sub) < 0) return -1;
                break;
            }
            switch (sub) {
            case 8: {            // memory.init data, 0
                uint32_t di; uint8_t z;
                if (rd_u32(d, &di) < 0 || rd_byte(d, &z) < 0) return -1;
                NEED(z == 0, "zero byte expected");
                if (check_mem0(d) < 0) return -1;
                NEED(d->data_count_seen, "data count section required");
                NEED(di < d->data_count, "unknown data segment");
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0) return -1;
                if (out_word(d, 0x108) < 0 || out_word(d, di) < 0) return -1;
                break;
            }
            case 9: {
                uint32_t di;
                if (rd_u32(d, &di) < 0) return -1;
                NEED(d->data_count_seen, "data count section required");
                NEED(di < d->data_count, "unknown data segment");
                if (out_word(d, 0x109) < 0 || out_word(d, di) < 0) return -1;
                break;
            }
            case 10: {
                uint8_t a, b;
                if (rd_byte(d, &a) < 0 || rd_byte(d, &b) < 0) return -1;
                NEED(a == 0 && b == 0, "zero byte expected");
                if (check_mem0(d) < 0) return -1;
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || out_word(d, 0x10A) < 0) return -1;
                break;
            }
            case 11: {
                uint8_t a;
                if (rd_byte(d, &a) < 0) return -1;
                NEED(a == 0, "zero byte expected");
                if (check_mem0(d) < 0) return -1;
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || out_word(d, 0x10B) < 0) return -1;
                break;
            }
            case 12: {           // table.init elem, table
                uint32_t ei, ti;
                if (rd_u32(d, &ei) < 0 || rd_u32(d, &ti) < 0) return -1;
                NEED(ti < d->total_tables, "unknown table");
                NEED(ei < d->m->nelems, "unknown element segment");
                NEED(d->m->elems[ei].type == d->ttypes[ti], "type mismatch: table.init");
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0) return -1;
                if (out_word(d, 0x10C) < 0 || out_word(d, ei) < 0 || out_word(d, ti) < 0) return -1;
                break;
            }
            case 13: {
                uint32_t ei;
                if (rd_u32(d, &ei) < 0) return -1;
                NEED(ei < d->m->nelems, "unknown element segment");
                if (out_word(d, 0x10D) < 0 || out_word(d, ei) < 0) return -1;
                break;
            }
            case 14: {
                uint32_t dst, src;
                if (rd_u32(d, &dst) < 0 || rd_u32(d, &src) < 0) return -1;
                NEED(dst < d->total_tables && src < d->total_tables, "unknown table");
                NEED(d->ttypes[dst] == d->ttypes[src], "type mismatch: table.copy");
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, WT_I32) < 0) return -1;
                if (out_word(d, 0x10E) < 0 || out_word(d, dst) < 0 || out_word(d, src) < 0) return -1;
                break;
            }
            case 15: {
                uint32_t t;
                if (rd_u32(d, &t) < 0) return -1;
                NEED(t < d->total_tables, "unknown table");
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, d->ttypes[t]) < 0 || vt_push(d, WT_I32) < 0) return -1;
                if (out_word(d, 0x10F) < 0 || out_word(d, t) < 0) return -1;
                break;
            }
            case 16: {
                uint32_t t;
                if (rd_u32(d, &t) < 0) return -1;
                NEED(t < d->total_tables, "unknown table");
                if (vt_push(d, WT_I32) < 0 || out_word(d, 0x110) < 0 || out_word(d, t) < 0) return -1;
                break;
            }
            case 17: {
                uint32_t t;
                if (rd_u32(d, &t) < 0) return -1;
                NEED(t < d->total_tables, "unknown table");
                if (vt_pop_expect(d, WT_I32) < 0 || vt_pop_expect(d, d->ttypes[t]) < 0 || vt_pop_expect(d, WT_I32) < 0) return -1;
                if (out_word(d, 0x111) < 0 || out_word(d, t) < 0) return -1;
                break;
            }
            default: FAIL("unknown 0xFC opcode");
            }
            break;
        }
        default: {
            uint8_t vtp; int log2, store;
            if (mem_op_info(op, &vtp, &log2, &store)) {
                uint32_t off;
                if (check_mem0(d) < 0 || rd_memarg(d, log2, &off) < 0) return -1;
                if (store) { if (vt_pop_expect(d, vtp) < 0 || vt_pop_expect(d, WT_I32) < 0) return -1; }
                else { if (vt_pop_expect(d, WT_I32) < 0 || vt_push(d, vtp) < 0) return -1; }
                if (out_word(d, op) < 0 || out_word(d, off) < 0) return -1;
                break;
            }
            if (numeric_sig(op, &ta, &tb, &nin, &to)) {
                if (nin == 2) { if (vt_pop_expect(d, tb) < 0) return -1; }
                if (vt_pop_expect(d, ta) < 0 || vt_push(d, to) < 0 || out_word(d, op) < 0) return -1;
                break;
            }
            if (op >= 0xFD && op != 0xFC) FAIL(op == 0xFD ? "SIMD is not supported" : "invalid opcode");
            FAIL("invalid opcode");
        }
        }
    }
done:
    d->p = save_p; d->end = save_end;
    code->ncode = d->nout;
    code->code = (uint32_t*)w_malloc((size_t)d->nout * 4 + 4);
    NEED(code->code, "out of memory");
    memcpy(code->code, d->out, (size_t)d->nout * 4);
    code->nparams = ft->np;
    code->nresults = ft->nr;
    code->nlocals = d->nlocals_total - ft->np;
    code->max_stack = d->max_stack + 8;
    return 0;
}

// ---------------------------------------------------------------------------
// module decoding

static void free_const_list(struct wasm_const* c) { w_free(c); }

void wasm_module_free_internal(struct wasm_module* m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->ntypes; i++) { w_free(m->types[i].p); w_free(m->types[i].r); }
    w_free(m->types);
    for (uint32_t i = 0; i < m->nimports; i++) { w_free(m->imports[i].module); w_free(m->imports[i].name); }
    w_free(m->imports);
    w_free(m->func_types);
    if (m->codes) for (uint32_t i = 0; i < m->nfuncs; i++) w_free(m->codes[i].code);
    w_free(m->codes);
    w_free(m->tables); w_free(m->mems); w_free(m->globals);
    for (uint32_t i = 0; i < m->nexports; i++) w_free(m->exports[i].name);
    w_free(m->exports);
    for (uint32_t i = 0; i < m->nelems; i++) free_const_list(m->elems[i].items);
    w_free(m->elems);
    for (uint32_t i = 0; i < m->ndatas; i++) w_free(m->datas[i].bytes);
    w_free(m->datas);
    for (uint32_t i = 0; i < m->ncustoms; i++) { w_free(m->customs[i].name); w_free(m->customs[i].bytes); }
    w_free(m->customs);
    w_free(m->declared_funcs);
    w_free(m);
}

// the lookup tables of the index spaces (imports first, then definitions): rebuilt whenever a definition section was read
static int rebuild_spaces(struct dec* d) {
    struct wasm_module* m = d->m;
    uint32_t nf = m->imp_funcs + m->nfuncs, nt = m->imp_tables + m->ntables, nm = m->imp_mems + m->nmems, ng = m->imp_globals + m->nglobals;
    uint8_t* ir = (uint8_t*)w_calloc(nf ? nf : 1, 1);
    uint32_t* ft = (uint32_t*)w_malloc((nf ? nf : 1) * 4);
    uint8_t* gt = (uint8_t*)w_calloc(ng ? ng : 1, 1);
    uint8_t* gm = (uint8_t*)w_calloc(ng ? ng : 1, 1);
    uint8_t* tt = (uint8_t*)w_calloc(nt ? nt : 1, 1);
    if (!ir || !ft || !gt || !gm || !tt) { w_free(ir); w_free(ft); w_free(gt); w_free(gm); w_free(tt); d->err = "out of memory"; return -1; }
    if (d->is_ref) memcpy(ir, d->is_ref, W_MIN(d->total_funcs, nf));
    uint32_t fi = 0, ti = 0, gi = 0;
    for (uint32_t i = 0; i < m->nimports; i++) {
        struct wasm_import* im = &m->imports[i];
        if (im->kind == WK_FUNC) ft[fi++] = im->type;
        else if (im->kind == WK_TABLE) tt[ti++] = im->table.type;
        else if (im->kind == WK_GLOBAL) { gt[gi] = im->gtype; gm[gi] = im->gmut; gi++; }
    }
    for (uint32_t i = 0; i < m->nfuncs; i++) ft[fi++] = m->func_types[i];
    for (uint32_t i = 0; i < m->ntables; i++) tt[ti++] = m->tables[i].type;
    if (d->gtypes && d->total_globals == ng) { memcpy(gt, d->gtypes, ng); memcpy(gm, d->gmuts, ng); }     // keep defined globals already typed
    for (uint32_t i = 0; i < m->nglobals; i++) if (m->globals[i].type) { gt[m->imp_globals + i] = m->globals[i].type; gm[m->imp_globals + i] = m->globals[i].mut; }
    w_free(d->is_ref); w_free(d->all_func_types); w_free(d->gtypes); w_free(d->gmuts); w_free(d->ttypes);
    d->is_ref = ir; d->all_func_types = ft; d->gtypes = gt; d->gmuts = gm; d->ttypes = tt;
    d->total_funcs = nf; d->total_tables = nt; d->total_mems = nm; d->total_globals = ng;
    return 0;
}

static int dec_types(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 100000, "too many types");
    d->m->types = (struct wasm_type*)w_calloc(n ? n : 1, sizeof(struct wasm_type));
    NEED(d->m->types, "out of memory");
    d->m->ntypes = n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t form;
        if (rd_byte(d, &form) < 0) return -1;
        NEED(form == 0x60, "invalid function type form");
        struct wasm_type* t = &d->m->types[i];
        uint32_t np, nr;
        if (rd_u32(d, &np) < 0) return -1;
        NEED(np <= 1000, "too many parameters");
        t->p = (uint8_t*)w_malloc(np + 1);
        NEED(t->p, "out of memory");
        for (uint32_t k = 0; k < np; k++) if (rd_valtype(d, &t->p[k]) < 0) return -1;
        t->np = (uint8_t)np;
        if (np > 255) FAIL("too many parameters");
        if (rd_u32(d, &nr) < 0) return -1;
        NEED(nr <= 1000, "too many results");
        t->r = (uint8_t*)w_malloc(nr + 1);
        NEED(t->r, "out of memory");
        for (uint32_t k = 0; k < nr; k++) if (rd_valtype(d, &t->r[k]) < 0) return -1;
        if (nr > 255) FAIL("too many results");
        t->nr = (uint8_t)nr;
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_imports(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 100000, "too many imports");
    d->m->imports = (struct wasm_import*)w_calloc(n ? n : 1, sizeof(struct wasm_import));
    NEED(d->m->imports, "out of memory");
    d->m->nimports = n;
    for (uint32_t i = 0; i < n; i++) {
        struct wasm_import* im = &d->m->imports[i];
        uint8_t kind;
        if (rd_name(d, &im->module) < 0 || rd_name(d, &im->name) < 0 || rd_byte(d, &kind) < 0) return -1;
        im->kind = kind;
        switch (kind) {
        case WK_FUNC:
            if (rd_u32(d, &im->type) < 0) return -1;
            NEED(im->type < d->m->ntypes, "unknown type");
            d->m->imp_funcs++;
            break;
        case WK_TABLE:
            if (rd_reftype(d, &im->table.type) < 0 || rd_limits(d, &im->table.lim, 10000000, "table size too large") < 0) return -1;
            d->m->imp_tables++;
            break;
        case WK_MEM:
            if (rd_limits(d, &im->mem, 65536, "memory size must be at most 65536 pages (4GiB)") < 0) return -1;
            d->m->imp_mems++;
            break;
        case WK_GLOBAL: {
            uint8_t mut;
            if (rd_valtype(d, &im->gtype) < 0 || rd_byte(d, &mut) < 0) return -1;
            NEED(mut <= 1, "invalid mutability");
            im->gmut = mut;
            d->m->imp_globals++;
            break;
        }
        default: FAIL("invalid import kind");
        }
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_functions(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 1000000, "too many functions");
    d->m->func_types = (uint32_t*)w_malloc((n ? n : 1) * 4);
    NEED(d->m->func_types, "out of memory");
    d->m->nfuncs = n;
    for (uint32_t i = 0; i < n; i++) {
        if (rd_u32(d, &d->m->func_types[i]) < 0) return -1;
        NEED(d->m->func_types[i] < d->m->ntypes, "unknown type");
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_tables(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 100000 && d->m->imp_tables + n <= 100000, "too many tables");
    d->m->tables = (struct wasm_tabtype*)w_calloc(n ? n : 1, sizeof(struct wasm_tabtype));
    NEED(d->m->tables, "out of memory");
    d->m->ntables = n;
    for (uint32_t i = 0; i < n; i++)
        if (rd_reftype(d, &d->m->tables[i].type) < 0 || rd_limits(d, &d->m->tables[i].lim, 10000000, "table size too large") < 0) return -1;
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_memories(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n + d->m->imp_mems <= 1, "multiple memories are not supported");
    d->m->mems = (struct wasm_limits*)w_calloc(n ? n : 1, sizeof(struct wasm_limits));
    NEED(d->m->mems, "out of memory");
    d->m->nmems = n;
    for (uint32_t i = 0; i < n; i++)
        if (rd_limits(d, &d->m->mems[i], 65536, "memory size must be at most 65536 pages (4GiB)") < 0) return -1;
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_globals(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 1000000, "too many globals");
    d->m->globals = (struct wasm_global_def*)w_calloc(n ? n : 1, sizeof(struct wasm_global_def));
    NEED(d->m->globals, "out of memory");
    d->m->nglobals = n;
    if (rebuild_spaces(d) < 0) return -1;
    for (uint32_t i = 0; i < n; i++) {
        struct wasm_global_def* g = &d->m->globals[i];
        uint8_t mut;
        if (rd_valtype(d, &g->type) < 0 || rd_byte(d, &mut) < 0) return -1;
        NEED(mut <= 1, "invalid mutability");
        g->mut = mut;
        // the global index space seen by this initialiser is the imports only (plus earlier immutable globals in 2.0; keep imports)
        if (rd_const_expr(d, g->type, &g->init) < 0) return -1;
        d->gtypes[d->m->imp_globals + i] = g->type;
        d->gmuts[d->m->imp_globals + i] = g->mut;
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_exports(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 1000000, "too many exports");
    d->m->exports = (struct wasm_export*)w_calloc(n ? n : 1, sizeof(struct wasm_export));
    NEED(d->m->exports, "out of memory");
    d->m->nexports = n;
    for (uint32_t i = 0; i < n; i++) {
        struct wasm_export* e = &d->m->exports[i];
        uint8_t kind;
        if (rd_name(d, &e->name) < 0 || rd_byte(d, &kind) < 0 || rd_u32(d, &e->idx) < 0) return -1;
        e->kind = kind;
        for (uint32_t k = 0; k < i; k++) NEED(strcmp(d->m->exports[k].name, e->name), "duplicate export name");
        switch (kind) {
        case WK_FUNC: NEED(e->idx < d->total_funcs, "unknown function"); d->is_ref[e->idx] = 1; break;
        case WK_TABLE: NEED(e->idx < d->total_tables, "unknown table"); break;
        case WK_MEM: NEED(e->idx < d->total_mems, "unknown memory"); break;
        case WK_GLOBAL: NEED(e->idx < d->total_globals, "unknown global"); break;
        default: FAIL("invalid export kind");
        }
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_start(struct dec* d, const uint8_t* end) {
    uint32_t f;
    if (rd_u32(d, &f) < 0) return -1;
    NEED(f < d->total_funcs, "unknown function");
    const struct wasm_type* t = &d->m->types[d->all_func_types[f]];
    NEED(t->np == 0 && t->nr == 0, "invalid start function: non-zero parameter or return count");
    d->m->has_start = 1; d->m->start = f;
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_elems(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 1000000, "too many element segments");
    d->m->elems = (struct wasm_elem*)w_calloc(n ? n : 1, sizeof(struct wasm_elem));
    NEED(d->m->elems, "out of memory");
    d->m->nelems = n;
    // element types are needed by table.init validation while compiling code, so record them first
    for (uint32_t i = 0; i < n; i++) {
        struct wasm_elem* e = &d->m->elems[i];
        uint32_t flag;
        if (rd_u32(d, &flag) < 0) return -1;
        NEED(flag <= 7, "invalid element segment flags");
        e->mode = (flag & 1) ? ((flag & 2) ? 2 : 1) : 0;
        int explicit_table = (flag & 2) && !(flag & 1);
        int exprs = flag & 4;
        e->type = WT_FUNCREF;
        e->table = 0;
        if (e->mode == 0) {
            if (explicit_table && rd_u32(d, &e->table) < 0) return -1;
            NEED(e->table < d->total_tables, "unknown table");
            if (rd_const_expr(d, WT_I32, &e->offset) < 0) return -1;
        }
        if (flag & 3) {            // elemkind / reftype present
            if (exprs) { if (rd_reftype(d, &e->type) < 0) return -1; }
            else { uint8_t k; if (rd_byte(d, &k) < 0) return -1; NEED(k == 0, "invalid element kind"); }
        }
        if (e->mode == 0) NEED(d->ttypes[e->table] == e->type, "type mismatch: element segment and table");
        uint32_t cnt;
        if (rd_u32(d, &cnt) < 0) return -1;
        NEED(cnt <= (uint32_t)(d->end - d->p) + 1 || exprs, "too many elements");
        NEED(cnt <= 10000000, "too many elements");
        e->items = (struct wasm_const*)w_calloc(cnt ? cnt : 1, sizeof(struct wasm_const));
        NEED(e->items, "out of memory");
        e->n = cnt;
        for (uint32_t k = 0; k < cnt; k++) {
            if (exprs) { if (rd_const_expr(d, e->type, &e->items[k]) < 0) return -1; }
            else {
                uint32_t f;
                if (rd_u32(d, &f) < 0) return -1;
                NEED(f < d->total_funcs, "unknown function");
                d->is_ref[f] = 1;
                e->items[k].kind = 6; e->items[k].v = f;
            }
        }
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_data(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n <= 1000000, "too many data segments");
    if (d->data_count_seen) NEED(n == d->data_count, "data count and data section have inconsistent lengths");
    d->m->datas = (struct wasm_data*)w_calloc(n ? n : 1, sizeof(struct wasm_data));
    NEED(d->m->datas, "out of memory");
    d->m->ndatas = n;
    for (uint32_t i = 0; i < n; i++) {
        struct wasm_data* s = &d->m->datas[i];
        uint32_t flag;
        if (rd_u32(d, &flag) < 0) return -1;
        NEED(flag <= 2, "invalid data segment flags");
        s->mode = flag == 1 ? 1 : 0;
        if (flag == 2) { if (rd_u32(d, &s->mem) < 0) return -1; }
        if (s->mode == 0) {
            NEED(s->mem < d->total_mems, "unknown memory");
            if (rd_const_expr(d, WT_I32, &s->offset) < 0) return -1;
        }
        uint32_t len;
        if (rd_u32(d, &len) < 0) return -1;
        NEED((uint32_t)(d->end - d->p) >= len, "unexpected end");
        s->bytes = (uint8_t*)w_malloc(len + 1);
        NEED(s->bytes, "out of memory");
        memcpy(s->bytes, d->p, len);
        s->len = len;
        d->p += len;
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static int dec_code(struct dec* d, const uint8_t* end) {
    uint32_t n;
    if (rd_u32(d, &n) < 0) return -1;
    NEED(n == d->m->nfuncs, "function and code section have inconsistent lengths");
    d->m->codes = (struct wasm_code*)w_calloc(n ? n : 1, sizeof(struct wasm_code));
    NEED(d->m->codes, "out of memory");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t size;
        if (rd_u32(d, &size) < 0) return -1;
        NEED((uint32_t)(d->end - d->p) >= size, "unexpected end");
        const uint8_t* body = d->p;
        d->p += size;
        if (compile_function(d, i, body, size, &d->m->codes[i]) < 0) {
            // "function #N: reason": the message buffer lives in the decoder state
            static char fmsg[160];
            int n = 0;
            const char* a = "Compiling function #";
            while (*a) fmsg[n++] = *a++;
            char dg[12]; int dl = 0; uint32_t v = d->m->imp_funcs + i;
            do { dg[dl++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (dl) fmsg[n++] = dg[--dl];
            a = " failed: ";
            while (*a) fmsg[n++] = *a++;
            for (const char* r = d->err ? d->err : "error"; *r && n < (int)sizeof fmsg - 1; r++) fmsg[n++] = *r;
            fmsg[n] = 0;
            d->err = fmsg;
            return -1;
        }
    }
    NEED(d->p == end, "section size mismatch");
    return 0;
}

static void dec_free_state(struct dec* d) {
    w_free(d->vt); w_free(d->out); w_free(d->lt); w_free(d->is_ref); w_free(d->gtypes); w_free(d->gmuts); w_free(d->ttypes); w_free(d->all_func_types);
    for (int i = 0; i < d->nct; i++) ctl_free(&d->ct[i]);
    w_free(d->ct);
}

int wasm_module_new(struct wasm_store* st, const uint8_t* bytes, size_t len, struct wasm_module** out, char* err, int errcap) {
    struct dec dd;
    memset(&dd, 0, sizeof dd);
    struct dec* d = &dd;
    d->st = st;
    struct wasm_module* m = (struct wasm_module*)w_calloc(1, sizeof *m);
    if (!m) { if (errcap > 0) strncpy(err, "out of memory", (size_t)errcap - 1); return -1; }
    d->m = m;
    d->p = bytes; d->end = bytes + len;
    uint32_t magic, version;
    if (len < 8) { d->err = len < 4 ? "BufferSource argument is empty or too short" : "unexpected end"; goto fail; }
    memcpy(&magic, bytes, 4); memcpy(&version, bytes + 4, 4);
    if (magic != 0x6D736100u) { d->err = "expected magic word 00 61 73 6d"; goto fail; }
    if (version != 1) { d->err = "expected version 01 00 00 00"; goto fail; }
    d->p += 8;
    int last_rank = 0;
    d->lt = (uint8_t*)w_malloc(50001);
    if (!d->lt) { d->err = "out of memory"; goto fail; }
    if (rebuild_spaces(d) < 0) goto fail;
    while (d->p < d->end) {
        uint8_t id;
        uint32_t size;
        if (rd_byte(d, &id) < 0 || rd_u32(d, &size) < 0) goto fail;
        if ((uint32_t)(d->end - d->p) < size) { d->err = "unexpected end of section or function"; goto fail; }
        const uint8_t* send = d->p + size;
        if (id == 0) {
            struct dec sec = *d;
            sec.end = send;
            char* nm;
            if (rd_name(&sec, &nm) < 0) { d->err = sec.err; goto fail; }
            struct wasm_custom* nc = (struct wasm_custom*)w_realloc(m->customs, (m->ncustoms + 1) * sizeof(struct wasm_custom));
            if (!nc) { w_free(nm); d->err = "out of memory"; goto fail; }
            m->customs = nc;
            struct wasm_custom* c = &m->customs[m->ncustoms++];
            c->name = nm;
            c->len = (uint32_t)(send - sec.p);
            c->bytes = (uint8_t*)w_malloc(c->len + 1);
            if (!c->bytes) { d->err = "out of memory"; goto fail; }
            memcpy(c->bytes, sec.p, c->len);
            d->p = send;
            continue;
        }
        static const int rank_of[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 10 };
        if (id > 12) { d->err = "unknown section code"; goto fail; }
        int rank = rank_of[id];
        if (rank <= last_rank) { d->err = "unexpected section order"; goto fail; }
        last_rank = rank;
        if (id == 12) {
            uint32_t n;
            if (rd_u32(d, &n) < 0) goto fail;
            d->data_count_seen = 1; d->data_count = n;
            if (d->p != send) { d->err = "section size mismatch"; goto fail; }
            continue;
        }
        int r = 0;
        switch (id) {
        case 1: r = dec_types(d, send); break;
        case 2: r = dec_imports(d, send); if (r == 0) r = rebuild_spaces(d); break;
        case 3: r = dec_functions(d, send); if (r == 0) r = rebuild_spaces(d); break;
        case 4: r = dec_tables(d, send); if (r == 0) r = rebuild_spaces(d); break;
        case 5: r = dec_memories(d, send); if (r == 0) r = rebuild_spaces(d); break;
        case 6: r = dec_globals(d, send); break;
        case 7: r = dec_exports(d, send); break;
        case 8: r = dec_start(d, send); break;
        case 9: r = dec_elems(d, send); break;
        case 10: r = dec_code(d, send); break;
        case 11: r = dec_data(d, send); break;
        }
        if (r < 0) goto fail;
        if (d->p != send) { d->err = "section size mismatch"; goto fail; }
    }
    if (m->nfuncs && !m->codes) { d->err = "function and code section have inconsistent lengths"; goto fail; }
    if (d->data_count_seen && m->ndatas != d->data_count) { d->err = "data count and data section have inconsistent lengths"; goto fail; }
    // functions that ref.func may name: kept for the instance (exports/elems/globals gathered them)
    if (store_add(st, m, 1) < 0) { d->err = "out of memory"; goto fail; }
    *out = m;
    dec_free_state(d);
    return 0;
fail:
    if (errcap > 0) {
        strncpy(err, d->err ? d->err : "invalid module", (size_t)errcap - 1);
        err[errcap - 1] = 0;
    }
    dec_free_state(d);
    wasm_module_free_internal(m);
    return -1;
}

// ---------------------------------------------------------------------------
// store

const char* wasm_trap_name(int trap) {
    switch (trap) {
    case WTRAP_UNREACHABLE: return "unreachable";
    case WTRAP_MEM_OOB: return "memory access out of bounds";
    case WTRAP_DIV0: return "divide by zero";
    case WTRAP_OVERFLOW: return "divide result unrepresentable";
    case WTRAP_INVALID_CONV: return "float unrepresentable in integer range";
    case WTRAP_NULL_ELEM: return "null function or function signature mismatch";
    case WTRAP_SIG_MISMATCH: return "null function or function signature mismatch";
    case WTRAP_TABLE_OOB: return "table index is out of bounds";
    case WTRAP_STACK: return "call stack exhausted";
    case WTRAP_BULK_OOB: return "memory access out of bounds";
    case WTRAP_HOST: return "exception in host function";
    case WTRAP_INTERRUPTED: return "interrupted";
    case WTRAP_UNDEF_ELEM: return "table index is out of bounds";
    case WTRAP_NULL_REF: return "null reference";
    }
    return "trap";
}

enum { OBJ_MODULE = 1, OBJ_INST, OBJ_FN, OBJ_MEM, OBJ_TAB, OBJ_GLOB };

struct wasm_store* wasm_store_new(const struct wasm_host* host) {
    struct wasm_store* s = (struct wasm_store*)w_calloc(1, sizeof *s);
    if (!s) return 0;
    s->host = *host;
    return s;
}

static int store_add(struct wasm_store* s, void* p, uint8_t kind) {
    if (s->nobjs >= s->capobjs) {
        uint32_t nc = s->capobjs ? s->capobjs * 2 : 64;
        void** no = (void**)w_realloc(s->objs, nc * sizeof(void*));
        uint8_t* nk = (uint8_t*)w_realloc(s->objkind, nc);
        if (!no || !nk) { if (no) s->objs = no; if (nk) s->objkind = nk; return -1; }
        s->objs = no; s->objkind = nk; s->capobjs = nc;
    }
    s->objs[s->nobjs] = p;
    s->objkind[s->nobjs++] = kind;
    return 0;
}

void wasm_module_free_internal(struct wasm_module* m);

void wasm_store_free(struct wasm_store* s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->nobjs; i++) {
        void* p = s->objs[i];
        switch (s->objkind[i]) {
        case OBJ_MODULE: wasm_module_free_internal((struct wasm_module*)p); break;
        case OBJ_INST: {
            struct wasm_inst* in = (struct wasm_inst*)p;
            w_free(in->funcs); w_free(in->tabs); w_free(in->mems); w_free(in->globs);
            w_free(in->data_dropped); w_free(in->elem_dropped); w_free(in->own_funcs);
            w_free(in);
            break;
        }
        case OBJ_FN: { struct wasm_fn* f = (struct wasm_fn*)p; if (f->own_type && f->type) { w_free(f->type->p); w_free(f->type->r); w_free(f->type); } w_free(f); break; }
        case OBJ_MEM: w_free(p); break;
        case OBJ_TAB: { struct wasm_tab* t = (struct wasm_tab*)p; w_free(t->el); w_free(t); break; }
        case OBJ_GLOB: w_free(p); break;
        }
    }
    w_free(s->objs); w_free(s->objkind); w_free(s->stack);
    w_free(s);
}

struct wasm_mem* wasm_mem_new(struct wasm_store* s, uint32_t pages, uint32_t max_pages, int has_max) {
    struct wasm_mem* m = (struct wasm_mem*)w_calloc(1, sizeof *m);
    if (!m) return 0;
    m->pages = pages; m->max_pages = max_pages; m->has_max = has_max;
    if (store_add(s, m, OBJ_MEM) < 0) { w_free(m); return 0; }
    return m;                                  // the embedder allocates the bytes (it owns them) and sets data/len
}

struct wasm_tab* wasm_tab_new(struct wasm_store* s, uint8_t type, uint32_t n, uint32_t max, int has_max) {
    if (n > 10000000) return 0;
    struct wasm_tab* t = (struct wasm_tab*)w_calloc(1, sizeof *t);
    if (!t) return 0;
    t->el = (void**)w_calloc(n ? n : 1, sizeof(void*));
    if (!t->el) { w_free(t); return 0; }
    t->n = n; t->max = max; t->has_max = has_max; t->type = type;
    if (store_add(s, t, OBJ_TAB) < 0) { w_free(t->el); w_free(t); return 0; }
    return t;
}

struct wasm_glob* wasm_glob_new(struct wasm_store* s, uint8_t type, int mut, wval v) {
    struct wasm_glob* g = (struct wasm_glob*)w_calloc(1, sizeof *g);
    if (!g) return 0;
    g->type = type; g->mut = (uint8_t)mut; g->v = v;
    if (store_add(s, g, OBJ_GLOB) < 0) { w_free(g); return 0; }
    return g;
}

struct wasm_fn* wasm_host_fn_new(struct wasm_store* s, const uint8_t* params, int np, const uint8_t* results, int nr, int host_id, void* user) {
    struct wasm_fn* f = (struct wasm_fn*)w_calloc(1, sizeof *f);
    struct wasm_type* t = (struct wasm_type*)w_calloc(1, sizeof *t);
    if (!f || !t) { w_free(f); w_free(t); return 0; }
    t->p = (uint8_t*)w_malloc((size_t)np + 1);
    t->r = (uint8_t*)w_malloc((size_t)nr + 1);
    if (!t->p || !t->r) { w_free(t->p); w_free(t->r); w_free(t); w_free(f); return 0; }
    memcpy(t->p, params, (size_t)np); memcpy(t->r, results, (size_t)nr);
    t->np = (uint8_t)np; t->nr = (uint8_t)nr;
    f->type = t; f->own_type = 1; f->host = host_id; f->user = user;
    if (store_add(s, f, OBJ_FN) < 0) { w_free(t->p); w_free(t->r); w_free(t); w_free(f); return 0; }
    return f;
}

int wasm_func_type_eq(const struct wasm_type* a, const struct wasm_type* b) {
    return a->np == b->np && a->nr == b->nr && !memcmp(a->p, b->p, a->np) && !memcmp(a->r, b->r, a->nr);
}

int wasm_mem_grow(struct wasm_store* s, struct wasm_mem* m, uint32_t delta) {
    uint32_t old = m->pages;
    uint64_t np = (uint64_t)old + delta;
    uint32_t cap = m->has_max ? m->max_pages : 65536;
    if (np > cap || np > 65536) return -1;
    if (delta == 0) return (int)old;
    if (!s->host.mem_grow || s->host.mem_grow(s->host.ud, m, (uint32_t)np) < 0) return -1;
    return (int)old;
}

int wasm_tab_grow(struct wasm_store* s, struct wasm_tab* t, uint32_t delta, void* init) {
    (void)s;
    uint32_t old = t->n;
    uint64_t nn = (uint64_t)old + delta;
    uint32_t cap = t->has_max ? t->max : 10000000;
    if (nn > cap || nn > 10000000) return -1;
    if (delta == 0) return (int)old;
    void** ne = (void**)w_realloc(t->el, (size_t)nn * sizeof(void*));
    if (!ne) return -1;
    t->el = ne;
    for (uint32_t i = old; i < nn; i++) t->el[i] = init;
    t->n = (uint32_t)nn;
    return (int)old;
}

// ---------------------------------------------------------------------------
// imports and instantiation

const char* wasm_import_ok(struct wasm_module* m, uint32_t idx, void* obj) {
    struct wasm_import* im = &m->imports[idx];
    switch (im->kind) {
    case WK_FUNC: {
        struct wasm_fn* f = (struct wasm_fn*)obj;
        return wasm_func_type_eq(f->type, &m->types[im->type]) ? 0 : "imported function does not match the expected type";
    }
    case WK_TABLE: {
        struct wasm_tab* t = (struct wasm_tab*)obj;
        if (t->type != im->table.type) return "imported table has the wrong element type";
        if (t->n < im->table.lim.min) return "table import is smaller than the declared minimum";
        if (im->table.lim.has_max) { if (!t->has_max) return "table import has no maximum"; if (t->max > im->table.lim.max) return "table import maximum is larger than declared"; }
        return 0;
    }
    case WK_MEM: {
        struct wasm_mem* mm = (struct wasm_mem*)obj;
        if (mm->pages < im->mem.min) return "memory import is smaller than the declared minimum";
        if (im->mem.has_max) { if (!mm->has_max) return "memory import has no maximum"; if (mm->max_pages > im->mem.max) return "memory import maximum is larger than declared"; }
        return 0;
    }
    case WK_GLOBAL: {
        struct wasm_glob* g = (struct wasm_glob*)obj;
        if (g->type != im->gtype) return "imported global has the wrong type";
        if (g->mut != im->gmut) return "imported global has the wrong mutability";
        return 0;
    }
    }
    return "bad import";
}

static wval const_eval(struct wasm_inst* in, const struct wasm_const* c) {
    wval v;
    v.u64 = 0;
    switch (c->kind) {
    case 0: v.u32 = (uint32_t)c->v; break;
    case 1: v.u64 = c->v; break;
    case 2: v.u32 = (uint32_t)c->v; break;
    case 3: v.u64 = c->v; break;
    case 4: v = in->globs[c->v]->v; break;
    case 5: v.ref = 0; break;
    case 6: v.ref = in->funcs[c->v]; break;
    }
    return v;
}

struct wasm_inst* wasm_instantiate(struct wasm_store* s, struct wasm_module* m, void** imports, int* trap, char* err, int errcap) {
    *trap = 0;
    struct wasm_inst* in = (struct wasm_inst*)w_calloc(1, sizeof *in);
    if (!in) { strncpy(err, "out of memory", (size_t)errcap - 1); *trap = -1; return 0; }
    in->mod = m;
    uint32_t nf = m->imp_funcs + m->nfuncs, nt = m->imp_tables + m->ntables, nm = m->imp_mems + m->nmems, ng = m->imp_globals + m->nglobals;
    in->funcs = (struct wasm_fn**)w_calloc(nf ? nf : 1, sizeof(void*));
    in->tabs = (struct wasm_tab**)w_calloc(nt ? nt : 1, sizeof(void*));
    in->mems = (struct wasm_mem**)w_calloc(nm ? nm : 1, sizeof(void*));
    in->globs = (struct wasm_glob**)w_calloc(ng ? ng : 1, sizeof(void*));
    in->data_dropped = (uint8_t*)w_calloc(m->ndatas ? m->ndatas : 1, 1);
    in->elem_dropped = (uint8_t*)w_calloc(m->nelems ? m->nelems : 1, 1);
    in->own_funcs = (struct wasm_fn**)w_calloc(m->nfuncs ? m->nfuncs : 1, sizeof(void*));
    if (!in->funcs || !in->tabs || !in->mems || !in->globs || !in->data_dropped || !in->elem_dropped || !in->own_funcs || store_add(s, in, OBJ_INST) < 0) {
        w_free(in->funcs); w_free(in->tabs); w_free(in->mems); w_free(in->globs); w_free(in->data_dropped); w_free(in->elem_dropped); w_free(in->own_funcs); w_free(in);
        strncpy(err, "out of memory", (size_t)errcap - 1);
        *trap = -1;
        return 0;
    }
    uint32_t fi = 0, ti = 0, mi = 0, gi = 0;
    for (uint32_t i = 0; i < m->nimports; i++) {
        switch (m->imports[i].kind) {
        case WK_FUNC: in->funcs[fi++] = (struct wasm_fn*)imports[i]; break;
        case WK_TABLE: in->tabs[ti++] = (struct wasm_tab*)imports[i]; break;
        case WK_MEM: in->mems[mi++] = (struct wasm_mem*)imports[i]; break;
        case WK_GLOBAL: in->globs[gi++] = (struct wasm_glob*)imports[i]; break;
        }
    }
    for (uint32_t i = 0; i < m->nfuncs; i++) {
        struct wasm_fn* f = (struct wasm_fn*)w_calloc(1, sizeof *f);
        if (!f || store_add(s, f, OBJ_FN) < 0) { w_free(f); strncpy(err, "out of memory", (size_t)errcap - 1); *trap = -1; return 0; }
        f->inst = in; f->idx = i; f->code = &m->codes[i]; f->host = -1; f->type = &m->types[m->func_types[i]];
        in->funcs[fi++] = f;
        in->own_funcs[i] = f;
    }
    for (uint32_t i = 0; i < m->ntables; i++) {
        struct wasm_tab* t = wasm_tab_new(s, m->tables[i].type, m->tables[i].lim.min, m->tables[i].lim.max, m->tables[i].lim.has_max);
        if (!t) { strncpy(err, "out of memory allocating a table", (size_t)errcap - 1); *trap = -1; return 0; }
        in->tabs[ti++] = t;
    }
    for (uint32_t i = 0; i < m->nmems; i++) {
        struct wasm_mem* mem = wasm_mem_new(s, m->mems[i].min, m->mems[i].max, m->mems[i].has_max);
        if (!mem || !s->host.mem_grow || s->host.mem_grow(s->host.ud, mem, m->mems[i].min) < 0) { strncpy(err, "out of memory allocating a memory", (size_t)errcap - 1); *trap = -1; return 0; }
        in->mems[mi++] = mem;
    }
    for (uint32_t i = 0; i < m->nglobals; i++) {
        wval v = const_eval(in, &m->globals[i].init);
        struct wasm_glob* g = wasm_glob_new(s, m->globals[i].type, m->globals[i].mut, v);
        if (!g) { strncpy(err, "out of memory", (size_t)errcap - 1); *trap = -1; return 0; }
        in->globs[gi++] = g;
    }
    // element and data segments (active ones are applied in order; an out-of-bounds one traps)
    for (uint32_t i = 0; i < m->nelems; i++) {
        struct wasm_elem* e = &m->elems[i];
        if (e->mode == 0) {
            struct wasm_tab* t = in->tabs[e->table];
            uint32_t off = const_eval(in, &e->offset).u32;
            if ((uint64_t)off + e->n > t->n) { *trap = WTRAP_TABLE_OOB; return in; }
            for (uint32_t k = 0; k < e->n; k++) t->el[off + k] = const_eval(in, &e->items[k]).ref;
            in->elem_dropped[i] = 1;
        } else if (e->mode == 2) in->elem_dropped[i] = 1;
    }
    for (uint32_t i = 0; i < m->ndatas; i++) {
        struct wasm_data* d = &m->datas[i];
        if (d->mode == 0) {
            struct wasm_mem* mem = in->mems[d->mem];
            uint32_t off = const_eval(in, &d->offset).u32;
            if ((uint64_t)off + d->len > mem->len) { *trap = WTRAP_MEM_OOB; return in; }
            if (d->len) memcpy(mem->data + off, d->bytes, d->len);
            in->data_dropped[i] = 1;
        }
    }
    return in;
}

// ---------------------------------------------------------------------------
// interpreter

#define FRAMES_MAX 12000
#define STACK_SLOTS (1u << 20)

struct frame { const uint32_t* retpc; const struct wasm_code* code; struct wasm_inst* inst; wval* fp; wval* opbase; };

#if defined(KERNEL) && KERNEL
double km_floor(double), km_ceil(double), km_trunc(double);
#define WFLOOR km_floor
#define WCEIL km_ceil
#define WTRUNC km_trunc
#define WSQRT sqrt
#else
#define WFLOOR floor
#define WCEIL ceil
#define WTRUNC trunc
#define WSQRT sqrt
#endif

static inline uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float f32_from(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t f64_bits(double f) { uint64_t u; memcpy(&u, &f, 8); return u; }
static inline double f64_from(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

static uint32_t clz32(uint32_t x) { if (!x) return 32; uint32_t n = 0; if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; } if (!(x & 0xFF000000u)) { n += 8; x <<= 8; } if (!(x & 0xF0000000u)) { n += 4; x <<= 4; } if (!(x & 0xC0000000u)) { n += 2; x <<= 2; } if (!(x & 0x80000000u)) n += 1; return n; }
static uint32_t ctz32(uint32_t x) { if (!x) return 32; uint32_t n = 0; if (!(x & 0xFFFFu)) { n += 16; x >>= 16; } if (!(x & 0xFFu)) { n += 8; x >>= 8; } if (!(x & 0xFu)) { n += 4; x >>= 4; } if (!(x & 3u)) { n += 2; x >>= 2; } if (!(x & 1u)) n += 1; return n; }
static uint32_t pop32(uint32_t x) { x = x - ((x >> 1) & 0x55555555u); x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u); x = (x + (x >> 4)) & 0x0F0F0F0Fu; return (x * 0x01010101u) >> 24; }
static uint64_t clz64(uint64_t x) { uint32_t hi = (uint32_t)(x >> 32); return hi ? clz32(hi) : 32 + clz32((uint32_t)x); }
static uint64_t ctz64(uint64_t x) { uint32_t lo = (uint32_t)x; return lo ? ctz32(lo) : 32 + ctz32((uint32_t)(x >> 32)); }
static uint64_t pop64(uint64_t x) { return pop32((uint32_t)x) + pop32((uint32_t)(x >> 32)); }

static float fmin32(float a, float b) {
    if (a != a || b != b) return f32_from(0x7FC00000u);
    if (a == 0 && b == 0) return (f32_bits(a) | f32_bits(b)) & 0x80000000u ? f32_from(0x80000000u) : 0.0f;
    return a < b ? a : b;
}
static float fmax32(float a, float b) {
    if (a != a || b != b) return f32_from(0x7FC00000u);
    if (a == 0 && b == 0) return (f32_bits(a) & f32_bits(b)) & 0x80000000u ? f32_from(0x80000000u) : 0.0f;
    return a > b ? a : b;
}
static double fmin64(double a, double b) {
    if (a != a || b != b) return f64_from(0x7FF8000000000000ull);
    if (a == 0 && b == 0) return (f64_bits(a) | f64_bits(b)) >> 63 ? f64_from(0x8000000000000000ull) : 0.0;
    return a < b ? a : b;
}
static double fmax64(double a, double b) {
    if (a != a || b != b) return f64_from(0x7FF8000000000000ull);
    if (a == 0 && b == 0) return (f64_bits(a) & f64_bits(b)) >> 63 ? f64_from(0x8000000000000000ull) : 0.0;
    return a > b ? a : b;
}
static double fnearest64(double x) {
    if (x != x || x == 0) return x;
    double ax = x < 0 ? -x : x;
    if (ax >= 4503599627370496.0) return x;                // 2^52: already integral
    double f = WFLOOR(ax), r = ax - f;
    double res = r > 0.5 ? f + 1 : (r < 0.5 ? f : (((int64_t)f & 1) ? f + 1 : f));
    return x < 0 ? -res : res;                             // keeps -0
}
static double fceil64(double x) { double r = WCEIL(x); return (r == 0 && (f64_bits(x) >> 63)) ? -0.0 : r; }
static double ffloor64(double x) { return WFLOOR(x); }
static double ftrunc64(double x) { return WTRUNC(x); }
static float fnearest32(float x) { return (float)fnearest64((double)x); }
static float fceil32(float x) { return (float)fceil64((double)x); }
static float ffloor32(float x) { return (float)WFLOOR((double)x); }
static float ftrunc32(float x) { return (float)WTRUNC((double)x); }

static double u64_to_f64(uint64_t x) {
    if (!(x >> 63)) return (double)(int64_t)x;
    uint64_t h = (x >> 1) | (x & 1);
    return (double)(int64_t)h * 2.0;
}
static float u64_to_f32(uint64_t x) {
    if (!(x >> 63)) return (float)(int64_t)x;
    uint64_t h = (x >> 1) | (x & 1);
    return (float)(int64_t)h * 2.0f;
}
static uint64_t f64_to_u64(double d) {            // d is in [0, 2^64)
    if (d < 9223372036854775808.0) return (uint64_t)(int64_t)d;
    return (uint64_t)(int64_t)(d - 9223372036854775808.0) + 0x8000000000000000ull;
}

static void tab_fill_range(void** el, uint32_t off, uint32_t n, void* v) { for (uint32_t i = 0; i < n; i++) el[off + i] = v; }

static int host_invoke(struct wasm_store* st, struct wasm_fn* f, wval* args, wval* results) {
    if (!st->host.call) return WTRAP_HOST;
    return st->host.call(st->host.ud, f, args, results);
}

#define TRAP(code) do { trap = (code); goto trapped; } while (0)

static int exec(struct wasm_store* st, struct wasm_fn* entry, const wval* args, wval* results) {
    if (!st->stack) {
        st->stack = (wval*)w_malloc((size_t)STACK_SLOTS * sizeof(wval) + (size_t)FRAMES_MAX * sizeof(struct frame));
        if (!st->stack) return WTRAP_STACK;
        st->stack_cap = STACK_SLOTS;
        st->depth = 0;
    }
    struct frame* frames = (struct frame*)(st->stack + STACK_SLOTS);
    // nested calls (a host function calling back into wasm) continue above what the outer call uses; the outer call
    // records its tops in the store before it invokes the host
    wval* sp_base = st->stack + (st->depth ? (uint32_t)(st->stack_used) : 0);
    struct frame* fr_base = frames + (st->depth ? st->frames_used : 0);
    if (st->depth > 200) return WTRAP_STACK;
    st->depth++;
    int trap = 0;
    const struct wasm_type* et = entry->type;
    wval* sp = sp_base;
    for (int i = 0; i < et->np; i++) *sp++ = args[i];
    struct frame* fp_frames = fr_base;
    struct frame* frame = fp_frames;       // next free frame
    if (entry->host >= 0) {
        wval tmp[260];
        trap = host_invoke(st, entry, sp_base, tmp);
        if (!trap) for (int i = 0; i < et->nr; i++) results[i] = tmp[i];
        st->depth--;
        return trap;
    }
    // current function state
    const struct wasm_code* code = entry->code;
    struct wasm_inst* inst = entry->inst;
    wval* fp = sp_base;
    if (fp + code->nparams + code->nlocals + code->max_stack > st->stack + STACK_SLOTS) { st->depth--; return WTRAP_STACK; }
    for (uint32_t i = 0; i < code->nlocals; i++) fp[code->nparams + i].u64 = 0;
    wval* opbase = fp + code->nparams + code->nlocals;
    sp = opbase;
    const uint32_t* pc = code->code;
    struct wasm_mem* m0 = inst->mod->imp_mems + inst->mod->nmems ? inst->mems[0] : 0;
    uint8_t* mb = m0 ? m0->data : 0;
    uint32_t ml = m0 ? m0->len : 0;
    int32_t fuel = 30000;
    #define RELOAD_MEM() do { m0 = inst->mod->imp_mems + inst->mod->nmems ? inst->mems[0] : 0; mb = m0 ? m0->data : 0; ml = m0 ? m0->len : 0; } while (0)
    #define POLL() do { if (--fuel <= 0) { fuel = 30000; if (st->host.poll && st->host.poll(st->host.ud)) TRAP(WTRAP_INTERRUPTED); } } while (0)

    for (;;) {
        uint32_t op = *pc++;
        switch (op) {
        case 0x00: TRAP(WTRAP_UNREACHABLE);
        case 0x04: { uint32_t target = *pc++; if (!(--sp)->u32) pc = code->code + target; break; }
        case 0x0C: {
            uint32_t target = pc[0], height = pc[1], arity = pc[2];
            wval* dst = opbase + height;
            if (arity && dst != sp - arity) { for (uint32_t i = 0; i < arity; i++) dst[i] = sp[(int)i - (int)arity]; }
            sp = dst + arity;
            if (code->code + target <= pc) POLL();
            pc = code->code + target;
            break;
        }
        case 0x0D: {
            uint32_t target = pc[0], height = pc[1], arity = pc[2];
            if ((--sp)->u32) {
                wval* dst = opbase + height;
                if (arity && dst != sp - arity) { for (uint32_t i = 0; i < arity; i++) dst[i] = sp[(int)i - (int)arity]; }
                sp = dst + arity;
                if (code->code + target <= pc) POLL();
                pc = code->code + target;
            } else pc += 3;
            break;
        }
        case 0x0E: {
            uint32_t n = *pc++;
            uint32_t i = (--sp)->u32;
            if (i > n) i = n;
            const uint32_t* t = pc + i * 3;
            uint32_t target = t[0], height = t[1], arity = t[2];
            wval* dst = opbase + height;
            if (arity && dst != sp - arity) { for (uint32_t k = 0; k < arity; k++) dst[k] = sp[(int)k - (int)arity]; }
            sp = dst + arity;
            if (code->code + target <= pc) POLL();
            pc = code->code + target;
            break;
        }
        case 0x0F: {
            uint32_t nr = code->nresults;
            for (uint32_t i = 0; i < nr; i++) fp[i] = sp[(int)i - (int)nr];
            sp = fp + nr;
            if (frame == fp_frames) {
                for (uint32_t i = 0; i < nr; i++) results[i] = fp[i];
                st->depth--;
                return 0;
            }
            frame--;
            pc = frame->retpc; code = frame->code; inst = frame->inst; fp = frame->fp; opbase = frame->opbase;
            RELOAD_MEM();
            break;
        }
        case 0x10: case 0x11: {
            struct wasm_fn* callee;
            if (op == 0x10) callee = inst->funcs[*pc++];
            else {
                uint32_t ti = pc[0], tb = pc[1];
                pc += 2;
                struct wasm_tab* t = inst->tabs[tb];
                uint32_t idx = (--sp)->u32;
                if (idx >= t->n) TRAP(WTRAP_TABLE_OOB);
                callee = (struct wasm_fn*)t->el[idx];
                if (!callee) TRAP(WTRAP_NULL_ELEM);
                if (!wasm_func_type_eq(callee->type, &inst->mod->types[ti])) TRAP(WTRAP_SIG_MISMATCH);
            }
            POLL();
            if (callee->host >= 0) {
                int np = callee->type->np, nr = callee->type->nr;
                wval tmp[260];
                wval* a = sp - np;
                st->stack_used = (uint32_t)(sp - st->stack);
                st->frames_used = (uint32_t)(frame - frames);
                int r = host_invoke(st, callee, a, tmp);
                if (r) TRAP(r);
                for (int i = 0; i < nr; i++) a[i] = tmp[i];
                sp = a + nr;
                RELOAD_MEM();
                break;
            }
            const struct wasm_code* cc = callee->code;
            if (frame - frames >= FRAMES_MAX - 1) TRAP(WTRAP_STACK);
            wval* nfp = sp - cc->nparams;
            if (nfp + cc->nparams + cc->nlocals + cc->max_stack > st->stack + STACK_SLOTS) TRAP(WTRAP_STACK);
            frame->retpc = pc; frame->code = code; frame->inst = inst; frame->fp = fp; frame->opbase = opbase;
            frame++;
            code = cc; inst = callee->inst; fp = nfp;
            for (uint32_t i = 0; i < cc->nlocals; i++) fp[cc->nparams + i].u64 = 0;
            opbase = fp + cc->nparams + cc->nlocals;
            sp = opbase;
            pc = cc->code;
            RELOAD_MEM();
            break;
        }
        case 0x1A: sp--; break;
        case 0x1B: { uint32_t c = (--sp)->u32; sp--; if (!c) sp[-1] = sp[0]; break; }
        case 0x20: *sp++ = fp[*pc++]; break;
        case 0x21: fp[*pc++] = *--sp; break;
        case 0x22: fp[*pc++] = sp[-1]; break;
        case 0x23: *sp++ = inst->globs[*pc++]->v; break;
        case 0x24: inst->globs[*pc++]->v = *--sp; break;
        case 0x25: { struct wasm_tab* t = inst->tabs[*pc++]; uint32_t i = sp[-1].u32; if (i >= t->n) TRAP(WTRAP_TABLE_OOB); sp[-1].ref = t->el[i]; break; }
        case 0x26: { struct wasm_tab* t = inst->tabs[*pc++]; void* v = sp[-1].ref; uint32_t i = sp[-2].u32; sp -= 2; if (i >= t->n) TRAP(WTRAP_TABLE_OOB); t->el[i] = v; break; }
        // loads
        #define LOADOP(code_, T, field, ct) case code_: { uint32_t off = *pc++; uint64_t ea = (uint64_t)sp[-1].u32 + off; if (ea + sizeof(T) > ml) TRAP(WTRAP_MEM_OOB); T x; memcpy(&x, mb + ea, sizeof(T)); sp[-1].u64 = 0; sp[-1].field = (ct)x; break; }
        LOADOP(0x28, uint32_t, u32, uint32_t)
        LOADOP(0x29, uint64_t, u64, uint64_t)
        LOADOP(0x2A, uint32_t, u32, uint32_t)
        LOADOP(0x2B, uint64_t, u64, uint64_t)
        LOADOP(0x2C, int8_t, s32, int32_t)
        LOADOP(0x2D, uint8_t, u32, uint32_t)
        LOADOP(0x2E, int16_t, s32, int32_t)
        LOADOP(0x2F, uint16_t, u32, uint32_t)
        LOADOP(0x30, int8_t, s64, int64_t)
        LOADOP(0x31, uint8_t, u64, uint64_t)
        LOADOP(0x32, int16_t, s64, int64_t)
        LOADOP(0x33, uint16_t, u64, uint64_t)
        LOADOP(0x34, int32_t, s64, int64_t)
        LOADOP(0x35, uint32_t, u64, uint64_t)
        #define STOREOP(code_, T, field) case code_: { uint32_t off = *pc++; uint64_t ea = (uint64_t)sp[-2].u32 + off; if (ea + sizeof(T) > ml) TRAP(WTRAP_MEM_OOB); T x = (T)sp[-1].field; memcpy(mb + ea, &x, sizeof(T)); sp -= 2; break; }
        STOREOP(0x36, uint32_t, u32)
        STOREOP(0x37, uint64_t, u64)
        STOREOP(0x38, uint32_t, u32)
        STOREOP(0x39, uint64_t, u64)
        STOREOP(0x3A, uint8_t, u32)
        STOREOP(0x3B, uint16_t, u32)
        STOREOP(0x3C, uint8_t, u64)
        STOREOP(0x3D, uint16_t, u64)
        STOREOP(0x3E, uint32_t, u64)
        case 0x3F: (sp++)->u64 = 0; sp[-1].u32 = m0->pages; break;
        case 0x40: {
            uint32_t delta = sp[-1].u32;
            int r = wasm_mem_grow(st, m0, delta);
            sp[-1].u64 = 0;
            sp[-1].s32 = r;
            RELOAD_MEM();
            break;
        }
        case 0x41: (sp++)->u64 = 0; sp[-1].u32 = *pc++; break;
        case 0x42: { uint32_t lo = pc[0], hi = pc[1]; pc += 2; (sp++)->u64 = (uint64_t)hi << 32 | lo; break; }
        case 0x43: (sp++)->u64 = 0; sp[-1].u32 = *pc++; break;
        case 0x44: { uint32_t lo = pc[0], hi = pc[1]; pc += 2; (sp++)->u64 = (uint64_t)hi << 32 | lo; break; }
        // i32 comparisons
        case 0x45: sp[-1].u32 = sp[-1].u32 == 0; break;
        #define CMP32(code_, expr) case code_: { uint32_t a = sp[-2].u32, b = sp[-1].u32; (void)a; (void)b; sp--; sp[-1].u64 = 0; sp[-1].u32 = (expr); break; }
        CMP32(0x46, a == b) CMP32(0x47, a != b) CMP32(0x48, (int32_t)a < (int32_t)b) CMP32(0x49, a < b) CMP32(0x4A, (int32_t)a > (int32_t)b)
        CMP32(0x4B, a > b) CMP32(0x4C, (int32_t)a <= (int32_t)b) CMP32(0x4D, a <= b) CMP32(0x4E, (int32_t)a >= (int32_t)b) CMP32(0x4F, a >= b)
        case 0x50: sp[-1].u64 = sp[-1].u64 == 0; break;
        #define CMP64(code_, expr) case code_: { uint64_t a = sp[-2].u64, b = sp[-1].u64; (void)a; (void)b; sp--; sp[-1].u64 = (expr) ? 1 : 0; break; }
        CMP64(0x51, a == b) CMP64(0x52, a != b) CMP64(0x53, (int64_t)a < (int64_t)b) CMP64(0x54, a < b) CMP64(0x55, (int64_t)a > (int64_t)b)
        CMP64(0x56, a > b) CMP64(0x57, (int64_t)a <= (int64_t)b) CMP64(0x58, a <= b) CMP64(0x59, (int64_t)a >= (int64_t)b) CMP64(0x5A, a >= b)
        #define CMPF32(code_, expr) case code_: { float a = sp[-2].f32, b = sp[-1].f32; sp--; sp[-1].u64 = (expr) ? 1 : 0; break; }
        CMPF32(0x5B, a == b) CMPF32(0x5C, a != b) CMPF32(0x5D, a < b) CMPF32(0x5E, a > b) CMPF32(0x5F, a <= b) CMPF32(0x60, a >= b)
        #define CMPF64(code_, expr) case code_: { double a = sp[-2].f64, b = sp[-1].f64; sp--; sp[-1].u64 = (expr) ? 1 : 0; break; }
        CMPF64(0x61, a == b) CMPF64(0x62, a != b) CMPF64(0x63, a < b) CMPF64(0x64, a > b) CMPF64(0x65, a <= b) CMPF64(0x66, a >= b)
        // i32 arithmetic
        case 0x67: sp[-1].u32 = clz32(sp[-1].u32); break;
        case 0x68: sp[-1].u32 = ctz32(sp[-1].u32); break;
        case 0x69: sp[-1].u32 = pop32(sp[-1].u32); break;
        #define BIN32(code_, expr) case code_: { uint32_t a = sp[-2].u32, b = sp[-1].u32; sp--; sp[-1].u64 = 0; sp[-1].u32 = (expr); break; }
        BIN32(0x6A, a + b) BIN32(0x6B, a - b) BIN32(0x6C, a * b)
        case 0x6D: { int32_t a = sp[-2].s32, b = sp[-1].s32; if (b == 0) TRAP(WTRAP_DIV0); if (a == (int32_t)0x80000000 && b == -1) TRAP(WTRAP_OVERFLOW); sp--; sp[-1].u64 = 0; sp[-1].s32 = a / b; break; }
        case 0x6E: { uint32_t a = sp[-2].u32, b = sp[-1].u32; if (b == 0) TRAP(WTRAP_DIV0); sp--; sp[-1].u64 = 0; sp[-1].u32 = a / b; break; }
        case 0x6F: { int32_t a = sp[-2].s32, b = sp[-1].s32; if (b == 0) TRAP(WTRAP_DIV0); sp--; sp[-1].u64 = 0; sp[-1].s32 = (b == -1) ? 0 : a % b; break; }
        case 0x70: { uint32_t a = sp[-2].u32, b = sp[-1].u32; if (b == 0) TRAP(WTRAP_DIV0); sp--; sp[-1].u64 = 0; sp[-1].u32 = a % b; break; }
        BIN32(0x71, a & b) BIN32(0x72, a | b) BIN32(0x73, a ^ b) BIN32(0x74, a << (b & 31)) BIN32(0x75, (uint32_t)((int32_t)a >> (b & 31)))
        BIN32(0x76, a >> (b & 31)) BIN32(0x77, (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31))) BIN32(0x78, (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)))
        // i64 arithmetic
        case 0x79: sp[-1].u64 = clz64(sp[-1].u64); break;
        case 0x7A: sp[-1].u64 = ctz64(sp[-1].u64); break;
        case 0x7B: sp[-1].u64 = pop64(sp[-1].u64); break;
        #define BIN64(code_, expr) case code_: { uint64_t a = sp[-2].u64, b = sp[-1].u64; sp--; sp[-1].u64 = (expr); break; }
        BIN64(0x7C, a + b) BIN64(0x7D, a - b) BIN64(0x7E, a * b)
        case 0x7F: { int64_t a = sp[-2].s64, b = sp[-1].s64; if (b == 0) TRAP(WTRAP_DIV0); if (a == (int64_t)0x8000000000000000ull && b == -1) TRAP(WTRAP_OVERFLOW); sp--; sp[-1].s64 = a / b; break; }
        case 0x80: { uint64_t a = sp[-2].u64, b = sp[-1].u64; if (b == 0) TRAP(WTRAP_DIV0); sp--; sp[-1].u64 = a / b; break; }
        case 0x81: { int64_t a = sp[-2].s64, b = sp[-1].s64; if (b == 0) TRAP(WTRAP_DIV0); sp--; sp[-1].s64 = (b == -1) ? 0 : a % b; break; }
        case 0x82: { uint64_t a = sp[-2].u64, b = sp[-1].u64; if (b == 0) TRAP(WTRAP_DIV0); sp--; sp[-1].u64 = a % b; break; }
        BIN64(0x83, a & b) BIN64(0x84, a | b) BIN64(0x85, a ^ b) BIN64(0x86, a << (b & 63)) BIN64(0x87, (uint64_t)((int64_t)a >> (b & 63)))
        BIN64(0x88, a >> (b & 63)) BIN64(0x89, (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63))) BIN64(0x8A, (a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)))
        // f32
        case 0x8B: sp[-1].u32 &= 0x7FFFFFFFu; break;
        case 0x8C: sp[-1].u32 ^= 0x80000000u; break;
        case 0x8D: sp[-1].f32 = fceil32(sp[-1].f32); break;
        case 0x8E: sp[-1].f32 = ffloor32(sp[-1].f32); break;
        case 0x8F: sp[-1].f32 = ftrunc32(sp[-1].f32); break;
        case 0x90: sp[-1].f32 = fnearest32(sp[-1].f32); break;
        case 0x91: sp[-1].f32 = (float)WSQRT((double)sp[-1].f32); break;
        #define BINF32(code_, expr) case code_: { float a = sp[-2].f32, b = sp[-1].f32; sp--; sp[-1].u64 = 0; sp[-1].f32 = (expr); break; }
        BINF32(0x92, a + b) BINF32(0x93, a - b) BINF32(0x94, a * b) BINF32(0x95, a / b) BINF32(0x96, fmin32(a, b)) BINF32(0x97, fmax32(a, b))
        case 0x98: { uint32_t a = sp[-2].u32, b = sp[-1].u32; sp--; sp[-1].u64 = 0; sp[-1].u32 = (a & 0x7FFFFFFFu) | (b & 0x80000000u); break; }
        // f64
        case 0x99: sp[-1].u64 &= 0x7FFFFFFFFFFFFFFFull; break;
        case 0x9A: sp[-1].u64 ^= 0x8000000000000000ull; break;
        case 0x9B: sp[-1].f64 = fceil64(sp[-1].f64); break;
        case 0x9C: sp[-1].f64 = ffloor64(sp[-1].f64); break;
        case 0x9D: sp[-1].f64 = ftrunc64(sp[-1].f64); break;
        case 0x9E: sp[-1].f64 = fnearest64(sp[-1].f64); break;
        case 0x9F: sp[-1].f64 = WSQRT(sp[-1].f64); break;
        #define BINF64(code_, expr) case code_: { double a = sp[-2].f64, b = sp[-1].f64; sp--; sp[-1].f64 = (expr); break; }
        BINF64(0xA0, a + b) BINF64(0xA1, a - b) BINF64(0xA2, a * b) BINF64(0xA3, a / b) BINF64(0xA4, fmin64(a, b)) BINF64(0xA5, fmax64(a, b))
        case 0xA6: { uint64_t a = sp[-2].u64, b = sp[-1].u64; sp--; sp[-1].u64 = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); break; }
        // conversions
        case 0xA7: sp[-1].u64 = (uint32_t)sp[-1].u64; break;
        case 0xA8: { float f = sp[-1].f32; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!((double)f > -2147483649.0 && (double)f < 2147483648.0)) TRAP(WTRAP_OVERFLOW); sp[-1].u64 = 0; sp[-1].s32 = (int32_t)(double)f; break; }
        case 0xA9: { float f = sp[-1].f32; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!((double)f > -1.0 && (double)f < 4294967296.0)) TRAP(WTRAP_OVERFLOW); sp[-1].u64 = 0; sp[-1].u32 = (uint32_t)(int64_t)(double)f; break; }
        case 0xAA: { double f = sp[-1].f64; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!(f > -2147483649.0 && f < 2147483648.0)) TRAP(WTRAP_OVERFLOW); sp[-1].u64 = 0; sp[-1].s32 = (int32_t)f; break; }
        case 0xAB: { double f = sp[-1].f64; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!(f > -1.0 && f < 4294967296.0)) TRAP(WTRAP_OVERFLOW); sp[-1].u64 = 0; sp[-1].u32 = (uint32_t)(int64_t)f; break; }
        case 0xAC: sp[-1].s64 = sp[-1].s32; break;
        case 0xAD: sp[-1].u64 = sp[-1].u32; break;
        case 0xAE: { float f = sp[-1].f32; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!((double)f >= -9223372036854775808.0 && (double)f < 9223372036854775808.0)) TRAP(WTRAP_OVERFLOW); sp[-1].s64 = (int64_t)(double)f; break; }
        case 0xAF: { float f = sp[-1].f32; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!((double)f > -1.0 && (double)f < 18446744073709551616.0)) TRAP(WTRAP_OVERFLOW); sp[-1].u64 = f64_to_u64((double)f); break; }
        case 0xB0: { double f = sp[-1].f64; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!(f >= -9223372036854775808.0 && f < 9223372036854775808.0)) TRAP(WTRAP_OVERFLOW); sp[-1].s64 = (int64_t)f; break; }
        case 0xB1: { double f = sp[-1].f64; if (f != f) TRAP(WTRAP_INVALID_CONV); if (!(f > -1.0 && f < 18446744073709551616.0)) TRAP(WTRAP_OVERFLOW); sp[-1].u64 = f64_to_u64(f); break; }
        case 0xB2: { int32_t v = sp[-1].s32; sp[-1].u64 = 0; sp[-1].f32 = (float)v; break; }
        case 0xB3: { uint32_t v = sp[-1].u32; sp[-1].u64 = 0; sp[-1].f32 = (float)(int64_t)v; break; }
        case 0xB4: { int64_t v = sp[-1].s64; sp[-1].u64 = 0; sp[-1].f32 = (float)v; break; }
        case 0xB5: { uint64_t v = sp[-1].u64; sp[-1].u64 = 0; sp[-1].f32 = u64_to_f32(v); break; }
        case 0xB6: { double v = sp[-1].f64; sp[-1].u64 = 0; sp[-1].f32 = (float)v; break; }
        case 0xB7: sp[-1].f64 = (double)sp[-1].s32; break;
        case 0xB8: sp[-1].f64 = (double)sp[-1].u32; break;
        case 0xB9: sp[-1].f64 = (double)sp[-1].s64; break;
        case 0xBA: sp[-1].f64 = u64_to_f64(sp[-1].u64); break;
        case 0xBB: { float v = sp[-1].f32; sp[-1].f64 = (double)v; break; }
        case 0xBC: case 0xBE: sp[-1].u64 = sp[-1].u32; break;        // reinterpret: the bits stay, upper half cleared
        case 0xBD: case 0xBF: break;
        case 0xC0: sp[-1].s32 = (int8_t)sp[-1].u32; sp[-1].u64 &= 0xFFFFFFFFull; break;
        case 0xC1: sp[-1].s32 = (int16_t)sp[-1].u32; sp[-1].u64 &= 0xFFFFFFFFull; break;
        case 0xC2: sp[-1].s64 = (int8_t)sp[-1].u64; break;
        case 0xC3: sp[-1].s64 = (int16_t)sp[-1].u64; break;
        case 0xC4: sp[-1].s64 = (int32_t)sp[-1].u64; break;
        case 0xD0: (sp++)->ref = 0; break;
        case 0xD1: { void* r = sp[-1].ref; sp[-1].u64 = 0; sp[-1].u32 = r == 0; break; }
        case 0xD2: { struct wasm_fn* f = inst->funcs[*pc++]; (sp++)->u64 = 0; sp[-1].ref = f; break; }
        // saturating conversions
        case 0x100: { float f = sp[-1].f32; int32_t r = f != f ? 0 : (double)f <= -2147483648.0 ? (int32_t)0x80000000 : (double)f >= 2147483648.0 ? 0x7FFFFFFF : (int32_t)(double)f; sp[-1].u64 = 0; sp[-1].s32 = r; break; }
        case 0x101: { float f = sp[-1].f32; uint32_t r = f != f || (double)f <= 0 ? 0 : (double)f >= 4294967296.0 ? 0xFFFFFFFFu : (uint32_t)(int64_t)(double)f; sp[-1].u64 = 0; sp[-1].u32 = r; break; }
        case 0x102: { double f = sp[-1].f64; int32_t r = f != f ? 0 : f <= -2147483648.0 ? (int32_t)0x80000000 : f >= 2147483648.0 ? 0x7FFFFFFF : (int32_t)f; sp[-1].u64 = 0; sp[-1].s32 = r; break; }
        case 0x103: { double f = sp[-1].f64; uint32_t r = f != f || f <= 0 ? 0 : f >= 4294967296.0 ? 0xFFFFFFFFu : (uint32_t)(int64_t)f; sp[-1].u64 = 0; sp[-1].u32 = r; break; }
        case 0x104: { double f = (double)sp[-1].f32; int64_t r = f != f ? 0 : f <= -9223372036854775808.0 ? (int64_t)0x8000000000000000ull : f >= 9223372036854775808.0 ? 0x7FFFFFFFFFFFFFFFll : (int64_t)f; sp[-1].s64 = r; break; }
        case 0x105: { double f = (double)sp[-1].f32; uint64_t r = f != f || f <= 0 ? 0 : f >= 18446744073709551616.0 ? ~(uint64_t)0 : f64_to_u64(f); sp[-1].u64 = r; break; }
        case 0x106: { double f = sp[-1].f64; int64_t r = f != f ? 0 : f <= -9223372036854775808.0 ? (int64_t)0x8000000000000000ull : f >= 9223372036854775808.0 ? 0x7FFFFFFFFFFFFFFFll : (int64_t)f; sp[-1].s64 = r; break; }
        case 0x107: { double f = sp[-1].f64; uint64_t r = f != f || f <= 0 ? 0 : f >= 18446744073709551616.0 ? ~(uint64_t)0 : f64_to_u64(f); sp[-1].u64 = r; break; }
        // bulk memory and tables
        case 0x108: {       // memory.init
            uint32_t di = *pc++;
            uint32_t n = sp[-1].u32, src = sp[-2].u32, dst = sp[-3].u32;
            sp -= 3;
            const struct wasm_data* ds = &inst->mod->datas[di];
            uint32_t dlen = inst->data_dropped[di] ? 0 : ds->len;
            if ((uint64_t)src + n > dlen || (uint64_t)dst + n > ml) TRAP(WTRAP_BULK_OOB);
            if (n) memcpy(mb + dst, ds->bytes + src, n);
            break;
        }
        case 0x109: inst->data_dropped[*pc++] = 1; break;
        case 0x10A: {       // memory.copy
            uint32_t n = sp[-1].u32, src = sp[-2].u32, dst = sp[-3].u32;
            sp -= 3;
            if ((uint64_t)src + n > ml || (uint64_t)dst + n > ml) TRAP(WTRAP_BULK_OOB);
            if (n) memmove(mb + dst, mb + src, n);
            break;
        }
        case 0x10B: {       // memory.fill
            uint32_t n = sp[-1].u32, v = sp[-2].u32, dst = sp[-3].u32;
            sp -= 3;
            if ((uint64_t)dst + n > ml) TRAP(WTRAP_BULK_OOB);
            if (n) memset(mb + dst, (int)(v & 0xFF), n);
            break;
        }
        case 0x10C: {       // table.init
            uint32_t ei = pc[0], ti = pc[1];
            pc += 2;
            uint32_t n = sp[-1].u32, src = sp[-2].u32, dst = sp[-3].u32;
            sp -= 3;
            struct wasm_tab* t = inst->tabs[ti];
            const struct wasm_elem* es = &inst->mod->elems[ei];
            uint32_t elen = inst->elem_dropped[ei] ? 0 : es->n;
            if ((uint64_t)src + n > elen || (uint64_t)dst + n > t->n) TRAP(WTRAP_TABLE_OOB);
            for (uint32_t k = 0; k < n; k++) t->el[dst + k] = const_eval(inst, &es->items[src + k]).ref;
            break;
        }
        case 0x10D: inst->elem_dropped[*pc++] = 1; break;
        case 0x10E: {       // table.copy
            uint32_t dti = pc[0], sti = pc[1];
            pc += 2;
            uint32_t n = sp[-1].u32, src = sp[-2].u32, dst = sp[-3].u32;
            sp -= 3;
            struct wasm_tab* dt = inst->tabs[dti]; struct wasm_tab* stb = inst->tabs[sti];
            if ((uint64_t)src + n > stb->n || (uint64_t)dst + n > dt->n) TRAP(WTRAP_TABLE_OOB);
            if (n) memmove(dt->el + dst, stb->el + src, (size_t)n * sizeof(void*));
            break;
        }
        case 0x10F: {       // table.grow
            struct wasm_tab* t = inst->tabs[*pc++];
            uint32_t n = sp[-1].u32;
            void* init = sp[-2].ref;
            sp--;
            sp[-1].u64 = 0;
            sp[-1].s32 = wasm_tab_grow(st, t, n, init);
            break;
        }
        case 0x110: { struct wasm_tab* t = inst->tabs[*pc++]; (sp++)->u64 = 0; sp[-1].u32 = t->n; break; }
        case 0x111: {       // table.fill
            struct wasm_tab* t = inst->tabs[*pc++];
            uint32_t n = sp[-1].u32; void* v = sp[-2].ref; uint32_t i = sp[-3].u32;
            sp -= 3;
            if ((uint64_t)i + n > t->n) TRAP(WTRAP_TABLE_OOB);
            tab_fill_range(t->el, i, n, v);
            break;
        }
        default:
            TRAP(WTRAP_UNREACHABLE);
        }
    }
trapped:
    st->depth--;
    return trap;
}

int wasm_call(struct wasm_store* s, struct wasm_fn* fn, const wval* args, wval* results) {
    return exec(s, fn, args, results);
}
