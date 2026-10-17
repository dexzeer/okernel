// jit.c — the baseline compiler (see jit.h). Included by vm.c: it shares the interpreter's helpers and the frame layout.
//
// Registers in compiled code:  ebx = f->locals (the operand stack follows the locals: slot k is [ebx + 8k]), esi = J, edi = f,
// ebp = the C frame. eax/ecx/edx are scratch. The C frame has 32 bytes of outgoing arguments at [esp] and a result slot for
// jit_step at [esp+32].
//
// Every value is a jv in the frame; nothing lives in a register across instructions. Instructions this file translates have a fast path
// that handles the common types inline; if a guard fails (or the instruction is not translated at all) the instruction is executed by
// vm_step, which is the interpreter body for exactly one instruction, and compiled code continues wherever the interpreter says.

#include "jit.h"
#if !(defined(KERNEL) && KERNEL)
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#endif

int ojs_jit_mode = 0;
uint32_t ojs_jit_threshold = 40;
struct jit_stats ojs_jit_stats;
#if !(defined(KERNEL) && KERNEL)
static double jit_ms;
#endif

static void* exec_alloc(size_t n) {
#if defined(KERNEL) && KERNEL
    return ojs_sys_malloc(n);       // the kernel's pages are all executable
#else
    void* p = mmap(0, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
#endif
}
static void exec_free(void* p, size_t n) {
#if defined(KERNEL) && KERNEL
    (void)n;
    ojs_sys_free(p);
#else
    munmap(p, n);
#endif
}

void jit_free(struct jitcode* c) {
    if (!c) return;
    if (c->mem) exec_free(c->mem, c->size);
    ojs_sys_free(c->pcs);
    ojs_sys_free(c->addr);
    ojs_sys_free(c);
}

void* jit_native_at(const struct jitcode* c, uint32_t pc_off) {
    uint32_t lo = 0, hi = c->n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (c->pcs[m] < pc_off) lo = m + 1; else hi = m;
    }
    return lo < c->n && c->pcs[lo] == pc_off ? c->addr[lo] : 0;
}

// ---------------------------------------------------------------- helpers compiled code calls directly (cheaper than a vm_step)

// a global read / write through the inline cache. 1: done (the value is in place); 0: take the slow road
static int jit_gl_get(ojs* J, struct ftempl* t, uint32_t ici, jv* dst) {
    jv v;
    if (!ic_global_get(J, t, ici, &v)) return 0;
    *dst = v;
    return 1;
}
static int jit_gl_put(ojs* J, struct ftempl* t, uint32_t ici, jv* top) { return ic_global_put(J, t, ici, *top); }

// the inline cache of a GET_FIELD / PUT_FIELD site when the inline check of the compiled code failed (prototype hits, string receivers, the
// second way ...). `slot` is the object's stack slot (for a put the value follows it); the result replaces it.
static int jit_ic_get(ojs* J, struct ftempl* t, uint32_t ici, jv* slot) {
    jv a = *slot, v;
    if (jv_is_obj(a)) {
        if (ic_get(J, t, ici, jv_obj(a), &v) != 1) return 0;
    } else if (jv_is_str(a)) {
        if (!ic_get_str(J, t, ici, &v)) return 0;
    } else return 0;
    *slot = v;
    return 1;
}
static int jit_ic_define(ojs* J, struct ftempl* t, uint32_t ici, jv* slot) { return ic_define(J, t, ici, jv_obj(slot[0]), slot[1]); }
static int jit_ic_put(ojs* J, struct ftempl* t, uint32_t ici, jv* slot) {
    jv a = slot[0], b = slot[1], setter;
    if (!jv_is_obj(a) || ic_put(J, t, ici, jv_obj(a), b, &setter) != 1) return 0;
    slot[0] = b;
    return 1;
}

// OP_CALL / OP_CALL_METHOD / OP_NEW: f->pc and f->sp are set; args points at the first argument, [-2] is the function, [-1] this / new.target
static jv jit_call(ojs* J, struct ojs_frame* f, jv* args, int argc) {
    (void)f;
    if (--J->interrupt_counter == 0 && interrupt_check(J) < 0) return JV_EXC;
    jv a = args[-2], b = args[-1];
    if (jv_is_obj(a) && obj_class(jv_obj(a)) == OC_FUNCTION && !(((struct func*)jv_obj(a))->t->flags & TF_NOT_PLAIN))
        return call_plain(J, (struct func*)jv_obj(a), b, argc, args);
    if (!is_callable(a)) { throw_not_callable(J, a, 0); return JV_EXC; }
    return vm_call(J, a, b, argc, args, JV_UNDEFINED);
}
static jv jit_new(ojs* J, struct ojs_frame* f, jv* args, int argc) {
    (void)f;
    jv a = args[-2], b = args[-1];
    if (!is_constructor(a)) { throw_not_callable(J, a, 1); return JV_EXC; }
    return vm_call(J, a, JV_UNDEFINED, argc, args, b);
}

// ---------------------------------------------------------------- analysis: stack depth of every instruction

struct fx {
    int8_t need;                // stack items the instruction reads or pops
    int8_t dfall, djump;        // depth change on the fall-through / jump path
    uint8_t kind;               // 0 sequential, 1 unconditional jump, 2 conditional jump, 3 ends the function or throws
    int32_t target;             // jumps: bytecode offset of the target
};

// 0, or -1 for an instruction compiled code cannot hold (with-scopes, eval, generators ...)
static int op_fx(const uint8_t* ip, uint32_t pc, struct fx* x) {
    int op = ip[0];
    x->kind = 0; x->target = -1; x->need = 0; x->dfall = x->djump = 0;
#define FX(n, d) do { x->need = (n); x->dfall = x->djump = (d); } while (0)
    switch (op) {
    case OP_NOP: case OP_DEBUGGER: case OP_NOP_END: case OP_LINE: break;
    case OP_UNDEF: case OP_NULL_: case OP_TRUE_: case OP_FALSE_: case OP_HOLE: case OP_INT8: case OP_INT32: case OP_CONST: case OP_CONST_W:
    case OP_GET_LOC: case OP_GET_LOC_CHK: case OP_GET_UPV: case OP_GET_UPV_CHK: case OP_GET_GLOBAL: case OP_TYPEOF_GLOBAL:
    case OP_OBJECT: case OP_CLOSURE: case OP_REGEXP: case OP_REST: case OP_ARGUMENTS: case OP_THIS_FUNC: case OP_FRAME_THIS:
    case OP_NEW_TARGET: case OP_HOME_OBJECT: case OP_PRIVATE_NAME: FX(0, 1); break;
    case OP_DUP: FX(1, 1); break;
    case OP_DUP2: FX(2, 2); break;
    case OP_PICK: FX(ip[1] + 1, 1); break;
    case OP_POP: FX(1, -1); break;
    case OP_NIP: FX(2, -1); break;
    case OP_SWAP: FX(2, 0); break;
    case OP_ROT3L: case OP_ROT3R: FX(3, 0); break;
    case OP_ROT4R: FX(4, 0); break;
    case OP_INSERT: FX(ip[1] + 1, 0); break;
    case OP_SET_LOC: case OP_INIT_LOC: case OP_PUT_LOC_CHK: case OP_SET_UPV: case OP_INIT_UPV: case OP_PUT_UPV_CHK:
    case OP_BIND_THIS_LOC: case OP_BIND_THIS_UPV: case OP_PUT_GLOBAL: case OP_PUT_GLOBAL_STRICT: FX(1, 0); break;
    case OP_HOLE_LOC: case OP_CLOSE: break;
    case OP_PUT_LOC: case OP_PUT_UPV: FX(1, -1); break;
    case OP_GET_FIELD: case OP_DELETE_FIELD: case OP_TO_PROPKEY: FX(1, 0); break;
    case OP_GET_FIELD_KEEP: FX(1, 1); break;
    case OP_PUT_FIELD: case OP_GET_ELEM: case OP_DELETE_ELEM: case OP_GET_PRIVATE: case OP_PRIVATE_IN: FX(2, -1); break;
    case OP_GET_ELEM_KEEP: FX(2, 0); break;
    case OP_PUT_ELEM: case OP_GET_SUPER: case OP_PUT_PRIVATE: FX(3, -2); break;
    case OP_GET_SUPER_KEEP: FX(3, -1); break;
    case OP_PUT_SUPER: case OP_DEFINE_PRIVATE: case OP_ADD_PRIVATE_METHOD: FX(4, -3); break;
    case OP_SUPER_CTOR: FX(1, 0); break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_SHL: case OP_SAR: case OP_SHR:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: case OP_LT: case OP_LE:
    case OP_GT: case OP_GE: case OP_IN: case OP_INSTANCEOF: FX(2, -1); break;
    case OP_NEG: case OP_PLUS: case OP_BITNOT: case OP_NOT: case OP_INC: case OP_DEC: case OP_TO_NUMERIC: case OP_TYPEOF:
    case OP_IS_UNDEF: case OP_IS_NULLISH: case OP_TO_STRING: case OP_TO_OBJECT: case OP_TO_PRIMITIVE: FX(1, 0); break;
    case OP_JMP: x->kind = 1; x->target = (int32_t)(pc + 5 + (int32_t)rd32(ip + 1)); break;
    case OP_JF: case OP_JT: case OP_JNULLISH: case OP_JUNDEF:
        x->kind = 2; FX(1, -1); x->target = (int32_t)(pc + 5 + (int32_t)rd32(ip + 1)); break;
    case OP_JF_KEEP: case OP_JT_KEEP: case OP_JNOTNULLISH_KEEP:
        x->kind = 2; FX(1, 0); x->target = (int32_t)(pc + 5 + (int32_t)rd32(ip + 1)); break;
    case OP_ITER_STEP: x->kind = 2; x->need = 2; x->dfall = 0; x->djump = -1; x->target = (int32_t)(pc + 5 + (int32_t)rd32(ip + 1)); break;
    case OP_FOR_OF_STEP: case OP_FOR_IN_NEXT:
        x->kind = 2; x->need = 1; x->dfall = 1; x->djump = 0; x->target = (int32_t)(pc + 5 + (int32_t)rd32(ip + 1)); break;
    case OP_CALL: case OP_CALL_METHOD: case OP_NEW: case OP_SUPER_CALL: { int argc = rd16(ip + 1); FX(argc + 2, -(argc + 1)); break; }
    case OP_CALL_SPREAD: case OP_CALL_METHOD_SPREAD: case OP_NEW_SPREAD: case OP_SUPER_CALL_SPREAD: FX(3, -2); break;
    case OP_RETURN: case OP_THROW: x->kind = 3; x->need = 1; break;
    case OP_RETURN_UNDEF: case OP_THROW_ERR: case OP_TDZ_ERROR: case OP_CONST_ERROR: case OP_CALL_IGNORED_REF_ERROR: x->kind = 3; break;
    case OP_ARRAY: { int n = rd16(ip + 1); FX(n, 1 - n); break; }
    case OP_APPEND_ONE: case OP_APPEND: case OP_DEFINE_FIELD: case OP_SET_PROTO_LIT: case OP_COPY_DATA_PROPS: case OP_REST_OBJ:
    case OP_SET_HOME: case OP_RUN_FIELDS: FX(2, -1); break;
    case OP_ITER_CLOSE: case OP_ITER_CLOSE_QUIET: FX(1, -1); break;
    case OP_APPEND_HOLE: case OP_SET_NAME: case OP_GET_ITER: case OP_GET_ASYNC_ITER: case OP_ASYNC_ITER_CLOSE: case OP_SPREAD_ARRAY: case OP_FOR_IN_START: FX(1, 0); break;
    case OP_DEFINE_ELEM: case OP_DEFINE_METHOD: case OP_DEFINE_METHOD_ELEM: FX(3, -2); break;
    case OP_SET_NAME_ELEM: case OP_ITER_CHECK_OBJ: FX(2, 0); break;
    case OP_CLASS: { int her = ip[5]; FX(her, her ? 1 : 2); break; }
    case OP_CLASS_FIELDS: FX(3, -1); break;
    case OP_ITER_CALL: case OP_ITER_VALUE: FX(1, 1); break;
    default: return -1;
    }
#undef FX
    return 0;
}

struct jan {                    // the result of analysing one template
    uint32_t ni;                // instructions
    uint32_t* ord_pc;           // ni + 1 entries (the last = code_len)
    uint32_t* pc_ord;           // code_len + 1 entries: ordinal at an instruction start, ~0 elsewhere
    int32_t* depth;             // per ordinal: operand stack depth before it, -1 unreachable
    uint8_t* target;            // per ordinal: a jump or handler lands here
};

static void jan_free(struct jan* a) {
    ojs_sys_free(a->ord_pc); ojs_sys_free(a->pc_ord); ojs_sys_free(a->depth); ojs_sys_free(a->target);
    memset(a, 0, sizeof *a);
}

static int jan_run(struct ftempl* t, struct jan* a) {
    memset(a, 0, sizeof *a);
    uint32_t len = t->code_len;
    const uint8_t* code = t->code;
    uint32_t ni = 0;
    for (uint32_t pc = 0; pc < len; ni++) {
        if (code[pc] >= OP__COUNT) return -1;
        pc += (uint32_t)op_size(code[pc]);
        if (pc > len) return -1;
    }
    a->ni = ni;
    a->ord_pc = (uint32_t*)ojs_sys_malloc((ni + 1) * sizeof(uint32_t));
    a->pc_ord = (uint32_t*)ojs_sys_malloc((len + 1) * sizeof(uint32_t));
    a->depth = (int32_t*)ojs_sys_malloc((ni + 1) * sizeof(int32_t));
    a->target = (uint8_t*)ojs_sys_malloc(ni + 1);
    if (!a->ord_pc || !a->pc_ord || !a->depth || !a->target) { jan_free(a); return -1; }
    for (uint32_t i = 0; i <= len; i++) a->pc_ord[i] = ~0u;
    uint32_t o = 0;
    for (uint32_t pc = 0; pc < len; o++) {
        a->ord_pc[o] = pc;
        a->pc_ord[pc] = o;
        pc += (uint32_t)op_size(code[pc]);
    }
    a->ord_pc[ni] = len;
    a->pc_ord[len] = ni;
    for (uint32_t i = 0; i <= ni; i++) { a->depth[i] = -1; a->target[i] = 0; }
    if (ni == 0) { jan_free(a); return -1; }

    uint32_t* work = (uint32_t*)ojs_sys_malloc((ni + t->nexc + 8) * sizeof(uint32_t) * 2);
    if (!work) { jan_free(a); return -1; }
    uint32_t nw = 0;
    int maxd = t->stack_size;
#define SEED(ord, dep) do { \
        int32_t dd_ = (dep); \
        if ((ord) > ni || dd_ < 0 || dd_ > maxd) { ojs_sys_free(work); jan_free(a); return -1; } \
        if (a->depth[(ord)] < 0) { a->depth[(ord)] = dd_; work[nw++] = (ord); } \
        else if (a->depth[(ord)] != dd_) { ojs_sys_free(work); jan_free(a); return -1; } \
    } while (0)
    SEED(0, 0);
    for (uint32_t i = 0; i < t->nexc; i++) {
        uint32_t h = t->exc[i].handler;
        if (h >= len || a->pc_ord[h] == ~0u) { ojs_sys_free(work); jan_free(a); return -1; }
        a->target[a->pc_ord[h]] = 1;
        SEED(a->pc_ord[h], t->exc[i].sp + 1);
    }
    while (nw) {
        uint32_t o2 = work[--nw];
        uint32_t pc = a->ord_pc[o2];
        struct fx x;
        if (op_fx(code + pc, pc, &x) < 0) { ojs_sys_free(work); jan_free(a); return -1; }
        int32_t d = a->depth[o2];
        if (d < x.need) { ojs_sys_free(work); jan_free(a); return -1; }
        if (x.kind != 1 && x.kind != 3) {
            if (o2 + 1 >= ni) { ojs_sys_free(work); jan_free(a); return -1; }          // falls off the end
            SEED(o2 + 1, d + x.dfall);
        }
        if (x.kind == 1 || x.kind == 2) {
            if (x.target < 0 || (uint32_t)x.target >= len || a->pc_ord[x.target] == ~0u) { ojs_sys_free(work); jan_free(a); return -1; }
            a->target[a->pc_ord[x.target]] = 1;
            SEED(a->pc_ord[x.target], d + x.djump);
        }
    }
#undef SEED
    ojs_sys_free(work);
    return 0;
}

int jit_depth_at(struct ftempl* t, uint32_t pc_off) {
    struct jan a;
    if (jan_run(t, &a) < 0) return -2;
    int r = pc_off < t->code_len && a.pc_ord[pc_off] != ~0u ? a.depth[a.pc_ord[pc_off]] : -1;
    jan_free(&a);
    return r;
}

// ---------------------------------------------------------------- the assembler

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
#define NOBASE (-1)

struct label_ref { uint32_t pos; uint32_t lab; };
struct call_ref { uint32_t pos; const void* fn; };

struct cg {
    ojs* J;
    struct ftempl* t;
    struct jan an;
    uint32_t nl;                    // locals (the operand stack starts at slot nl)
    uint8_t* b; uint32_t n, cap;
    int fail;
    uint32_t* lab; uint32_t nlab, caplab;                 // label positions (~0: not bound)
    struct label_ref* rel; uint32_t nrel, caprel;         // rel32 -> label
    struct label_ref* abs; uint32_t nabs, capabs;         // abs32 -> label (rebased)
    struct call_ref* calls; uint32_t ncalls, capcalls;    // rel32 -> C function
    uint32_t* stub;                                       // per ordinal: label of its stub (0 none)
    uint8_t* skind;                                       // and what the stub does first: 0 nothing, 1 the cached get, 2 the cached put
    uint32_t* stub_list; uint32_t nstub;
    uint32_t L_epi, L_fin, L_exc;
    uint32_t nnative, nstepped;
};

static void em8(struct cg* c, uint32_t v) {
    if (c->n + 1 > c->cap) {
        uint32_t nc = c->cap ? c->cap * 2 : 4096;
        uint8_t* nb = (uint8_t*)ojs_sys_realloc(c->b, nc);
        if (!nb) { c->fail = 1; return; }
        c->b = nb; c->cap = nc;
    }
    c->b[c->n++] = (uint8_t)v;
}
static void em32(struct cg* c, uint32_t v) { em8(c, v); em8(c, v >> 8); em8(c, v >> 16); em8(c, v >> 24); }

static uint32_t new_label(struct cg* c) {
    if (c->nlab == c->caplab) {
        uint32_t nc = c->caplab ? c->caplab * 2 : 256;
        uint32_t* nl = (uint32_t*)ojs_sys_realloc(c->lab, nc * sizeof(uint32_t));
        if (!nl) { c->fail = 1; return 0; }
        c->lab = nl; c->caplab = nc;
    }
    c->lab[c->nlab] = ~0u;
    return c->nlab++;
}
static void bind(struct cg* c, uint32_t l) { c->lab[l] = c->n; }

#define PUSH_REF(arr, narr, cap, T, ...) do { \
        if (c->narr == c->cap) { \
            uint32_t nc_ = c->cap ? c->cap * 2 : 256; \
            T* nr_ = (T*)ojs_sys_realloc(c->arr, nc_ * sizeof(T)); \
            if (!nr_) { c->fail = 1; return; } \
            c->arr = nr_; c->cap = nc_; \
        } \
        c->arr[c->narr++] = (T){ __VA_ARGS__ }; \
    } while (0)

static void rel32_to(struct cg* c, uint32_t lab) { PUSH_REF(rel, nrel, caprel, struct label_ref, c->n, lab); em32(c, 0); }
static void abs32_of(struct cg* c, uint32_t lab) { PUSH_REF(abs, nabs, capabs, struct label_ref, c->n, lab); em32(c, 0); }

// ModRM for [base + disp] (NOBASE: [disp32])
static void mrm(struct cg* c, int reg, int base, int32_t disp) {
    if (base == NOBASE) { em8(c, (reg << 3) | 5); em32(c, (uint32_t)disp); return; }
    int mod = (disp == 0 && base != EBP) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    em8(c, (mod << 6) | (reg << 3) | (base == ESP ? 4 : base));
    if (base == ESP) em8(c, 0x24);
    if (mod == 1) em8(c, (uint32_t)disp);
    else if (mod == 2) em32(c, (uint32_t)disp);
}
static void mov_rm(struct cg* c, int r, int base, int32_t d) { em8(c, 0x8B); mrm(c, r, base, d); }      // mov r, [m]
static void mov_mr(struct cg* c, int base, int32_t d, int r) { em8(c, 0x89); mrm(c, r, base, d); }      // mov [m], r
static void mov_ri(struct cg* c, int r, uint32_t imm) { em8(c, 0xB8 + r); em32(c, imm); }
static void mov_mi(struct cg* c, int base, int32_t d, uint32_t imm) { em8(c, 0xC7); mrm(c, 0, base, d); em32(c, imm); }
static void mov_rr(struct cg* c, int dst, int src) { em8(c, 0x89); em8(c, 0xC0 | (src << 3) | dst); }
// ALU r, [m]: 0x03 add, 0x0B or, 0x23 and, 0x2B sub, 0x33 xor, 0x3B cmp
static void alu_rm(struct cg* c, int opc, int r, int base, int32_t d) { em8(c, opc); mrm(c, r, base, d); }
// ALU with an immediate: n = 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp
static void alu_ri(struct cg* c, int n, int r, int32_t imm) {
    if (imm >= -128 && imm <= 127) { em8(c, 0x83); em8(c, 0xC0 | (n << 3) | r); em8(c, (uint32_t)imm); }
    else { em8(c, 0x81); em8(c, 0xC0 | (n << 3) | r); em32(c, (uint32_t)imm); }
}
static void alu_mi(struct cg* c, int n, int base, int32_t d, int32_t imm) {
    if (imm >= -128 && imm <= 127) { em8(c, 0x83); mrm(c, n, base, d); em8(c, (uint32_t)imm); }
    else { em8(c, 0x81); mrm(c, n, base, d); em32(c, (uint32_t)imm); }
}
static void test_rr(struct cg* c, int a, int b) { em8(c, 0x85); em8(c, 0xC0 | (b << 3) | a); }
static void lea(struct cg* c, int r, int base, int32_t d) { em8(c, 0x8D); mrm(c, r, base, d); }
static void jcc(struct cg* c, int cc, uint32_t lab) { em8(c, 0x0F); em8(c, 0x80 + cc); rel32_to(c, lab); }
static void jmp(struct cg* c, uint32_t lab) { em8(c, 0xE9); rel32_to(c, lab); }
static void jmp_r(struct cg* c, int r) { em8(c, 0xFF); em8(c, 0xE0 | r); }
static void call_c(struct cg* c, const void* fn) { em8(c, 0xE8); PUSH_REF(calls, ncalls, capcalls, struct call_ref, c->n, fn); em32(c, 0); }
static void setcc(struct cg* c, int cc, int r8) { em8(c, 0x0F); em8(c, 0x90 + cc); em8(c, 0xC0 | r8); }
static void movzx8(struct cg* c, int dst, int src8) { em8(c, 0x0F); em8(c, 0xB6); em8(c, 0xC0 | (dst << 3) | src8); }
static void shift_cl(struct cg* c, int n, int r) { em8(c, 0xD3); em8(c, 0xC0 | (n << 3) | r); }        // 4 shl, 5 shr, 7 sar
static void unary(struct cg* c, int n, int r) { em8(c, 0xF7); em8(c, 0xC0 | (n << 3) | r); }            // 2 not, 3 neg, 6 div, 7 idiv
static void imul_rm(struct cg* c, int r, int base, int32_t d) { em8(c, 0x0F); em8(c, 0xAF); mrm(c, r, base, d); }
static void ret(struct cg* c) { em8(c, 0xC3); }
// [base + index*8 + disp] (SIB)
static void mrm_idx8(struct cg* c, int reg, int base, int index, int32_t disp) {
    int mod = (disp == 0 && base != EBP) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    em8(c, (mod << 6) | (reg << 3) | 4);
    em8(c, (3 << 6) | (index << 3) | base);
    if (mod == 1) em8(c, (uint32_t)disp); else if (mod == 2) em32(c, (uint32_t)disp);
}
static void mov_rx(struct cg* c, int r, int base, int index, int32_t d) { em8(c, 0x8B); mrm_idx8(c, r, base, index, d); }   // mov r, [base+index*8+d]
static void mov_xr(struct cg* c, int base, int index, int32_t d, int r) { em8(c, 0x89); mrm_idx8(c, r, base, index, d); }
static void lea_x(struct cg* c, int r, int base, int index, int32_t d) { em8(c, 0x8D); mrm_idx8(c, r, base, index, d); }
static void cmpw_mi(struct cg* c, int base, int32_t d, uint32_t imm) { em8(c, 0x66); em8(c, 0x81); mrm(c, 7, base, d); em8(c, imm); em8(c, imm >> 8); }
static void test_mi(struct cg* c, int base, int32_t d, uint32_t imm) { em8(c, 0xF7); mrm(c, 0, base, d); em32(c, imm); }

#define SLOT_LO(k) EBX, (int32_t)(8 * (c->nl + (k)))
#define SLOT_HI(k) EBX, (int32_t)(8 * (c->nl + (k)) + 4)
#define LOC_LO(i)  EBX, (int32_t)(8 * (i))
#define LOC_HI(i)  EBX, (int32_t)(8 * (i) + 4)
#define F_OFF(m) ((int32_t)offsetof(struct ojs_frame, m))
#define J_OFF(m) ((int32_t)offsetof(struct ojs, m))

#define V_INT     0xFFF90000u
#define V_SPECIAL 0xFFFA0000u
#define V_OBJ     0xFFFB0000u
#define V_STR     0xFFFC0000u
#define V_SYM     0xFFFD0000u

// copy a 64-bit slot: dst <- src (through ecx)
static void cpy(struct cg* c, int db, int32_t dd, int sb, int32_t sd) {
    mov_rm(c, ECX, sb, sd);     mov_mr(c, db, dd, ECX);
    mov_rm(c, ECX, sb, sd + 4); mov_mr(c, db, dd + 4, ECX);
}
#define CPY(dst, src) cpy(c, EBX, (int32_t)(8 * (dst)), EBX, (int32_t)(8 * (src)))   // slot indices (absolute in the block)
#define ST(k) (c->nl + (uint32_t)(k))                                                 // absolute slot index of stack entry k

static void load_slot(struct cg* c, int rlo, int rhi, uint32_t slot) {
    mov_rm(c, rlo, EBX, (int32_t)(8 * slot)); mov_rm(c, rhi, EBX, (int32_t)(8 * slot + 4));
}
static void store_slot(struct cg* c, uint32_t slot, int rlo, int rhi) {
    mov_mr(c, EBX, (int32_t)(8 * slot), rlo); mov_mr(c, EBX, (int32_t)(8 * slot + 4), rhi);
}
static void push_const(struct cg* c, uint32_t slot, uint32_t lo, uint32_t hi) {
    mov_mi(c, EBX, (int32_t)(8 * slot), lo); mov_mi(c, EBX, (int32_t)(8 * slot + 4), hi);
}

// ---- the step call: run the instruction at `ord` in the interpreter and continue wherever it says
static void gen_step(struct cg* c, uint32_t ord, int d) {
    struct ftempl* t = c->t;
    uint32_t pc = c->an.ord_pc[ord];
    mov_mi(c, EDI, F_OFF(pc), (uint32_t)(uintptr_t)(t->code + pc));
    lea(c, EAX, EBX, (int32_t)(8 * (c->nl + (uint32_t)d)));
    mov_mr(c, EDI, F_OFF(sp), EAX);
    mov_mr(c, ESP, 0, ESI);
    mov_mr(c, ESP, 4, EDI);
    em8(c, 0xC7); mrm(c, 0, ESP, 8); abs32_of(c, ord + 1);                         // resume: the next instruction
    mov_mi(c, ESP, 12, (uint32_t)(uintptr_t)(t->code + c->an.ord_pc[ord + 1]));   // its pc
    lea(c, EAX, ESP, 32); mov_mr(c, ESP, 16, EAX);
    mov_mi(c, ESP, 20, 0);
    call_c(c, (const void*)jit_step);
    test_rr(c, EAX, EAX);
    jcc(c, CC_E, c->L_fin);
    jmp_r(c, EAX);
}

// a label whose code runs instruction `ord` in the interpreter (emitted after the main body)
static uint32_t step_stub_k(struct cg* c, uint32_t ord, int kind) {
    if (!c->stub[ord]) {
        c->stub[ord] = new_label(c);
        c->skind[ord] = (uint8_t)kind;
        c->stub_list[c->nstub++] = ord;
    }
    return c->stub[ord];
}
static uint32_t step_stub(struct cg* c, uint32_t ord) { return step_stub_k(c, ord, 0); }

// call helper(J, t, ici, slot pointer) with the address of stack entry slot_k; the answer is in eax
static void call_ic_helper(struct cg* c, const void* fn, uint32_t ici, int slot_k) {
    mov_mr(c, ESP, 0, ESI);
    mov_mi(c, ESP, 4, (uint32_t)(uintptr_t)c->t);
    mov_mi(c, ESP, 8, ici);
    lea(c, EAX, EBX, (int32_t)(8 * (c->nl + (uint32_t)slot_k)));
    mov_mr(c, ESP, 12, EAX);
    call_c(c, fn);
}

static void guard_int(struct cg* c, int d_slot_k, uint32_t slow) {            // jump to `slow` unless stack entry k holds an int
    alu_mi(c, 7, SLOT_HI(d_slot_k), (int32_t)V_INT);
    jcc(c, CC_NE, slow);
}

// materialise a boolean in ecx (0/1 in cl already) into stack entry k
static void store_bool_cl(struct cg* c, int k) {
    movzx8(c, ECX, ECX);                       // cl -> ecx
    alu_ri(c, 0, ECX, 2);                      // JV_FALSE = 2, JV_TRUE = 3
    mov_mr(c, SLOT_LO(k), ECX);
    mov_mi(c, SLOT_HI(k), V_SPECIAL);
}

// ---- one instruction. d = operand stack depth before it.
static void gen_ins(struct cg* c, uint32_t ord, int d) {
    struct ftempl* t = c->t;
    ojs* J = c->J;
    (void)J;
    const uint8_t* ip = t->code + c->an.ord_pc[ord];
    int op = ip[0];
    uint32_t slow = 0;
#define SLOW() (slow ? slow : (slow = step_stub(c, ord)))
#define NEXT_L (ord + 1)
    c->nnative++;
#if !(defined(KERNEL) && KERNEL)
    { static int nonative = -1; if (nonative < 0) nonative = getenv("OJS_JIT_NONATIVE") != 0; if (nonative && op != OP_RETURN && op != OP_RETURN_UNDEF) goto generic; }       // measuring: every instruction through vm_step
#endif
    switch (op) {
    case OP_NOP: case OP_DEBUGGER: case OP_NOP_END: case OP_LINE: break;
    case OP_UNDEF: push_const(c, ST(d), 0, V_SPECIAL); break;
    case OP_NULL_: push_const(c, ST(d), 1, V_SPECIAL); break;
    case OP_FALSE_: push_const(c, ST(d), 2, V_SPECIAL); break;
    case OP_TRUE_: push_const(c, ST(d), 3, V_SPECIAL); break;
    case OP_HOLE: push_const(c, ST(d), 4, V_SPECIAL); break;
    case OP_INT8: push_const(c, ST(d), (uint32_t)(int32_t)(int8_t)ip[1], V_INT); break;
    case OP_INT32: push_const(c, ST(d), rd32(ip + 1), V_INT); break;
    case OP_CONST: case OP_CONST_W: {
        uint32_t k = op == OP_CONST ? rd16(ip + 1) : rd32(ip + 1);
        uint32_t a = (uint32_t)(uintptr_t)t->consts + 8 * k;
        mov_rm(c, ECX, NOBASE, (int32_t)a);       mov_mr(c, SLOT_LO(d), ECX);
        mov_rm(c, ECX, NOBASE, (int32_t)a + 4);   mov_mr(c, SLOT_HI(d), ECX);
        break;
    }
    case OP_DUP: CPY(ST(d), ST(d - 1)); break;
    case OP_DUP2: CPY(ST(d), ST(d - 2)); CPY(ST(d + 1), ST(d - 1)); break;
    case OP_POP: break;
    case OP_PICK: CPY(ST(d), ST(d - 1 - ip[1])); break;
    case OP_NIP: CPY(ST(d - 2), ST(d - 1)); break;
    case OP_SWAP:
        load_slot(c, EAX, EDX, ST(d - 1)); CPY(ST(d - 1), ST(d - 2)); store_slot(c, ST(d - 2), EAX, EDX); break;
    case OP_ROT3L:      // [a b c] -> [b c a]
        load_slot(c, EAX, EDX, ST(d - 3)); CPY(ST(d - 3), ST(d - 2)); CPY(ST(d - 2), ST(d - 1)); store_slot(c, ST(d - 1), EAX, EDX); break;
    case OP_ROT3R:      // [a b c] -> [c a b]
        load_slot(c, EAX, EDX, ST(d - 1)); CPY(ST(d - 1), ST(d - 2)); CPY(ST(d - 2), ST(d - 3)); store_slot(c, ST(d - 3), EAX, EDX); break;
    case OP_ROT4R:
        load_slot(c, EAX, EDX, ST(d - 1)); CPY(ST(d - 1), ST(d - 2)); CPY(ST(d - 2), ST(d - 3)); CPY(ST(d - 3), ST(d - 4)); store_slot(c, ST(d - 4), EAX, EDX); break;
    case OP_INSERT: {   // move the top below n items
        int n = ip[1];
        load_slot(c, EAX, EDX, ST(d - 1));
        for (int i = 1; i <= n; i++) CPY(ST(d - i), ST(d - i - 1));
        store_slot(c, ST(d - 1 - n), EAX, EDX);
        break;
    }
    case OP_GET_LOC: CPY(ST(d), rd16(ip + 1)); break;
    case OP_SET_LOC: case OP_INIT_LOC: CPY(rd16(ip + 1), ST(d - 1)); break;
    case OP_PUT_LOC: CPY(rd16(ip + 1), ST(d - 1)); break;
    case OP_HOLE_LOC: { uint32_t s = rd16(ip + 1); mov_mi(c, LOC_LO(s), 4); mov_mi(c, LOC_HI(s), V_SPECIAL); break; }
    case OP_GET_LOC_CHK: case OP_PUT_LOC_CHK: {
        uint32_t s = rd16(ip + 1);
        uint32_t ok = new_label(c);
        alu_mi(c, 7, LOC_HI(s), (int32_t)V_SPECIAL); jcc(c, CC_NE, ok);
        alu_mi(c, 7, LOC_LO(s), 4); jcc(c, CC_E, SLOW());
        bind(c, ok);
        if (op == OP_GET_LOC_CHK) CPY(ST(d), s); else CPY(s, ST(d - 1));
        break;
    }
    case OP_GET_UPV: case OP_SET_UPV: case OP_PUT_UPV: case OP_INIT_UPV: {
        uint32_t i = rd16(ip + 1);
        mov_rm(c, EAX, EDI, F_OFF(fn));
        mov_rm(c, EAX, EAX, (int32_t)offsetof(struct func, upv));
        mov_rm(c, EAX, EAX, (int32_t)(4 * i));
        mov_rm(c, EAX, EAX, (int32_t)offsetof(struct upval, loc));
        if (op == OP_GET_UPV) {
            mov_rm(c, ECX, EAX, 0); mov_mr(c, SLOT_LO(d), ECX);
            mov_rm(c, ECX, EAX, 4); mov_mr(c, SLOT_HI(d), ECX);
        } else {
            mov_rm(c, ECX, SLOT_LO(d - 1)); mov_mr(c, EAX, 0, ECX);
            mov_rm(c, ECX, SLOT_HI(d - 1)); mov_mr(c, EAX, 4, ECX);
        }
        break;
    }
    case OP_JMP: {
        int32_t off = (int32_t)rd32(ip + 1);
        uint32_t tgt = c->an.pc_ord[c->an.ord_pc[ord] + 5 + off];
        if (off < 0) {          // a loop: the interrupt counter (the interpreter's own step takes the tick that reaches zero)
            alu_mi(c, 7, ESI, J_OFF(interrupt_counter), 1); jcc(c, CC_E, SLOW());
            em8(c, 0xFF); mrm(c, 1, ESI, J_OFF(interrupt_counter));     // dec dword [J->interrupt_counter]
        }
        jmp(c, tgt);
        break;
    }
    case OP_JF: case OP_JT: case OP_JF_KEEP: case OP_JT_KEEP: {
        int32_t off = (int32_t)rd32(ip + 1);
        uint32_t tgt = c->an.pc_ord[c->an.ord_pc[ord] + 5 + off];
        int jump_if_true = (op == OP_JT || op == OP_JT_KEEP);
        uint32_t istrue = new_label(c), isfalse = new_label(c);
        mov_rm(c, EAX, SLOT_HI(d - 1));
        alu_ri(c, 7, EAX, (int32_t)V_SPECIAL);
        uint32_t notspecial = new_label(c);
        jcc(c, CC_NE, notspecial);
        alu_mi(c, 7, SLOT_LO(d - 1), 3); jcc(c, CC_E, istrue); jmp(c, isfalse);        // only `true` is truthy among the specials
        bind(c, notspecial);
        alu_ri(c, 7, EAX, (int32_t)V_INT); jcc(c, CC_NE, SLOW());
        alu_mi(c, 7, SLOT_LO(d - 1), 0); jcc(c, CC_E, isfalse);
        bind(c, istrue);
        if (jump_if_true) {
            if (off < 0 && op == OP_JT) { alu_mi(c, 7, ESI, J_OFF(interrupt_counter), 1); jcc(c, CC_E, SLOW()); em8(c, 0xFF); mrm(c, 1, ESI, J_OFF(interrupt_counter)); }
            jmp(c, tgt);
        } else jmp(c, NEXT_L);
        bind(c, isfalse);
        if (jump_if_true) jmp(c, NEXT_L); else jmp(c, tgt);
        break;
    }
    case OP_INC: case OP_DEC: {
        guard_int(c, d - 1, SLOW());
        mov_rm(c, EAX, SLOT_LO(d - 1));
        alu_ri(c, op == OP_INC ? 0 : 5, EAX, 1); jcc(c, CC_O, SLOW());
        mov_mr(c, SLOT_LO(d - 1), EAX);
        break;
    }
    case OP_NEG: {
        guard_int(c, d - 1, SLOW());
        mov_rm(c, EAX, SLOT_LO(d - 1));
        test_rr(c, EAX, EAX); jcc(c, CC_E, SLOW());
        unary(c, 3, EAX); jcc(c, CC_O, SLOW());
        mov_mr(c, SLOT_LO(d - 1), EAX);
        break;
    }
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_BAND: case OP_BOR: case OP_BXOR: {
        guard_int(c, d - 2, SLOW()); guard_int(c, d - 1, SLOW());
        mov_rm(c, EAX, SLOT_LO(d - 2));
        if (op == OP_MUL) {
            imul_rm(c, EAX, SLOT_LO(d - 1)); jcc(c, CC_O, SLOW());
            test_rr(c, EAX, EAX); jcc(c, CC_E, SLOW());               // 0 might be -0
        } else {
            static const uint8_t opc[] = { [OP_ADD] = 0x03, [OP_SUB] = 0x2B, [OP_BAND] = 0x23, [OP_BOR] = 0x0B, [OP_BXOR] = 0x33 };
            alu_rm(c, opc[op], EAX, SLOT_LO(d - 1));
            if (op == OP_ADD || op == OP_SUB) jcc(c, CC_O, SLOW());
        }
        mov_mr(c, SLOT_LO(d - 2), EAX);
        break;
    }
    case OP_SHL: case OP_SAR: case OP_SHR: {
        guard_int(c, d - 2, SLOW()); guard_int(c, d - 1, SLOW());
        mov_rm(c, ECX, SLOT_LO(d - 1));
        mov_rm(c, EAX, SLOT_LO(d - 2));
        shift_cl(c, op == OP_SHL ? 4 : op == OP_SAR ? 7 : 5, EAX);
        if (op == OP_SHR) { test_rr(c, EAX, EAX); jcc(c, CC_S, SLOW()); }      // above INT32_MAX: a double
        mov_mr(c, SLOT_LO(d - 2), EAX);
        break;
    }
    case OP_MOD: {
        guard_int(c, d - 2, SLOW()); guard_int(c, d - 1, SLOW());
        mov_rm(c, EAX, SLOT_LO(d - 2)); test_rr(c, EAX, EAX); jcc(c, CC_S, SLOW());
        mov_rm(c, ECX, SLOT_LO(d - 1)); test_rr(c, ECX, ECX); jcc(c, CC_LE, SLOW());
        mov_ri(c, EDX, 0);
        unary(c, 6, ECX);                                                  // div ecx: edx = eax % ecx
        mov_mr(c, SLOT_LO(d - 2), EDX);
        break;
    }
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: {
        guard_int(c, d - 2, SLOW()); guard_int(c, d - 1, SLOW());
        mov_rm(c, EAX, SLOT_LO(d - 2));
        alu_rm(c, 0x3B, EAX, SLOT_LO(d - 1));
        int cc = op == OP_LT ? CC_L : op == OP_LE ? CC_LE : op == OP_GT ? CC_G : op == OP_GE ? CC_GE : (op == OP_EQ || op == OP_SEQ) ? CC_E : CC_NE;
        setcc(c, cc, ECX);
        store_bool_cl(c, d - 2);
        break;
    }
    case OP_IS_UNDEF: {
        mov_rm(c, EAX, SLOT_HI(d - 1)); alu_ri(c, 6, EAX, (int32_t)V_SPECIAL);
        alu_rm(c, 0x0B, EAX, SLOT_LO(d - 1));
        setcc(c, CC_E, ECX);
        store_bool_cl(c, d - 1);
        break;
    }
    case OP_IS_NULLISH: {
        uint32_t no = new_label(c), done = new_label(c);
        mov_ri(c, ECX, 0);
        alu_mi(c, 7, SLOT_HI(d - 1), (int32_t)V_SPECIAL); jcc(c, CC_NE, no);
        alu_mi(c, 7, SLOT_LO(d - 1), 1); setcc(c, CC_BE, ECX);
        bind(c, no);
        store_bool_cl(c, d - 1);
        (void)done;
        break;
    }
    case OP_TO_NUMERIC:
        alu_mi(c, 7, SLOT_HI(d - 1), (int32_t)V_SPECIAL); jcc(c, CC_AE, SLOW()); break;      // already a number: nothing to do
    case OP_TO_STRING:
        alu_mi(c, 7, SLOT_HI(d - 1), (int32_t)V_STR); jcc(c, CC_NE, SLOW()); break;
    case OP_TO_OBJECT:
        alu_mi(c, 7, SLOT_HI(d - 1), (int32_t)V_OBJ); jcc(c, CC_NE, SLOW()); break;
    case OP_GET_FIELD: case OP_GET_FIELD_KEEP: {
        uint32_t ici = rd16(ip + 5);
        if (!t->ic || ici >= t->nic) goto generic;
        int keep = op == OP_GET_FIELD_KEEP;
        int D = d - 1;
        uint32_t miss = step_stub_k(c, ord, 1);
        uint32_t w0 = (uint32_t)(uintptr_t)&t->ic[(size_t)ici * IC_WAYS], w1 = w0 + (uint32_t)sizeof(struct ic);
        if (keep) CPY(ST(d), ST(D));
        if (!keep && atom_key(J, t->consts[rd32(ip + 1)]) == A(length)) {     // x.length
            uint32_t notstr = new_label(c), icpath = new_label(c);
            mov_rm(c, EDX, SLOT_HI(D));
            alu_ri(c, 7, EDX, (int32_t)V_STR); jcc(c, CC_NE, notstr);
            mov_rm(c, EAX, SLOT_LO(D));
            mov_rm(c, EAX, EAX, (int32_t)offsetof(struct gch, aux32));
            mov_mr(c, SLOT_LO(D), EAX); mov_mi(c, SLOT_HI(D), V_INT);
            jmp(c, ord + 1);
            bind(c, notstr);
            alu_ri(c, 7, EDX, (int32_t)V_OBJ); jcc(c, CC_NE, miss);
            mov_rm(c, EAX, SLOT_LO(D));
            cmpw_mi(c, EAX, (int32_t)offsetof(struct gch, aux), OC_ARRAY); jcc(c, CC_NE, icpath);
            mov_rm(c, ECX, EAX, (int32_t)offsetof(struct obj, alen));
            test_rr(c, ECX, ECX); jcc(c, CC_S, miss);
            mov_mr(c, SLOT_LO(D), ECX); mov_mi(c, SLOT_HI(D), V_INT);
            jmp(c, ord + 1);
            bind(c, icpath);
        } else {
            alu_mi(c, 7, SLOT_HI(D), (int32_t)V_OBJ); jcc(c, CC_NE, miss);
            mov_rm(c, EAX, SLOT_LO(D));
        }
        test_mi(c, EAX, (int32_t)offsetof(struct obj, flags), OF_EXOTIC); jcc(c, CC_NE, miss);
        uint32_t try1 = new_label(c), load = new_label(c);
        mov_rm(c, EDX, EAX, (int32_t)offsetof(struct obj, shape));
        alu_rm(c, 0x3B, EDX, NOBASE, (int32_t)(w0 + offsetof(struct ic, fshape))); jcc(c, CC_NE, try1);
        mov_rm(c, ECX, NOBASE, (int32_t)(w0 + offsetof(struct ic, fslot))); jmp(c, load);
        bind(c, try1);
        alu_rm(c, 0x3B, EDX, NOBASE, (int32_t)(w1 + offsetof(struct ic, fshape))); jcc(c, CC_NE, miss);
        mov_rm(c, ECX, NOBASE, (int32_t)(w1 + offsetof(struct ic, fslot)));
        bind(c, load);
        mov_rm(c, EAX, EAX, (int32_t)offsetof(struct obj, slots));
        mov_rx(c, EDX, EAX, ECX, 4);
        mov_rx(c, EAX, EAX, ECX, 0);
        mov_mr(c, SLOT_LO(D), EAX); mov_mr(c, SLOT_HI(D), EDX);
        break;
    }
    case OP_PUT_FIELD: {                // [obj, value] -> [value]
        uint32_t ici = rd16(ip + 5);
        if (!t->ic || ici >= t->nic) goto generic;
        int O = d - 2, V = d - 1;
        uint32_t miss = step_stub_k(c, ord, 2);
        uint32_t w0 = (uint32_t)(uintptr_t)&t->ic[(size_t)ici * IC_WAYS], w1 = w0 + (uint32_t)sizeof(struct ic);
        alu_mi(c, 7, SLOT_HI(O), (int32_t)V_OBJ); jcc(c, CC_NE, miss);
        mov_rm(c, EAX, SLOT_LO(O));
        test_mi(c, EAX, (int32_t)offsetof(struct obj, flags), OF_EXOTIC); jcc(c, CC_NE, miss);
        uint32_t try1 = new_label(c), store = new_label(c);
        mov_rm(c, EDX, EAX, (int32_t)offsetof(struct obj, shape));
        alu_rm(c, 0x3B, EDX, NOBASE, (int32_t)(w0 + offsetof(struct ic, fshape))); jcc(c, CC_NE, try1);
        mov_rm(c, ECX, NOBASE, (int32_t)(w0 + offsetof(struct ic, fslot))); jmp(c, store);
        bind(c, try1);
        alu_rm(c, 0x3B, EDX, NOBASE, (int32_t)(w1 + offsetof(struct ic, fshape))); jcc(c, CC_NE, miss);
        mov_rm(c, ECX, NOBASE, (int32_t)(w1 + offsetof(struct ic, fslot)));
        bind(c, store);
        mov_rm(c, EAX, EAX, (int32_t)offsetof(struct obj, slots));
        mov_rm(c, EDX, SLOT_LO(V)); mov_xr(c, EAX, ECX, 0, EDX);
        mov_rm(c, EDX, SLOT_HI(V)); mov_xr(c, EAX, ECX, 4, EDX);
        CPY(ST(O), ST(V));
        break;
    }
    case OP_GET_ELEM: {                 // [obj, key] -> [value]: an element of a dense array
        int O = d - 2, K = d - 1;
        guard_int(c, K, SLOW());
        alu_mi(c, 7, SLOT_HI(O), (int32_t)V_OBJ); jcc(c, CC_NE, SLOW());
        mov_rm(c, EAX, SLOT_LO(O));
        mov_rm(c, ECX, SLOT_LO(K));
        mov_rm(c, EDX, EAX, (int32_t)offsetof(struct obj, flags));
        alu_ri(c, 4, EDX, OF_ARRAY_FAST | OF_EXOTIC); alu_ri(c, 7, EDX, OF_ARRAY_FAST); jcc(c, CC_NE, SLOW());
        alu_rm(c, 0x3B, ECX, EAX, (int32_t)offsetof(struct obj, elen)); jcc(c, CC_AE, SLOW());
        mov_rm(c, EAX, EAX, (int32_t)offsetof(struct obj, elems));
        mov_rx(c, EDX, EAX, ECX, 4);
        mov_rx(c, EAX, EAX, ECX, 0);
        uint32_t ok = new_label(c);
        alu_ri(c, 7, EDX, (int32_t)V_SPECIAL); jcc(c, CC_NE, ok);
        alu_ri(c, 7, EAX, 4); jcc(c, CC_E, SLOW());                      // a hole: the prototype chain may have the element
        bind(c, ok);
        mov_mr(c, SLOT_LO(O), EAX); mov_mr(c, SLOT_HI(O), EDX);
        break;
    }
    case OP_PUT_ELEM: {                 // [obj, key, value] -> [value]: overwriting an element of a dense array
        int O = d - 3, K = d - 2, V = d - 1;
        guard_int(c, K, SLOW());
        alu_mi(c, 7, SLOT_HI(O), (int32_t)V_OBJ); jcc(c, CC_NE, SLOW());
        mov_rm(c, EAX, SLOT_LO(O));
        mov_rm(c, ECX, SLOT_LO(K));
        mov_rm(c, EDX, EAX, (int32_t)offsetof(struct obj, flags));
        alu_ri(c, 4, EDX, OF_ARRAY_FAST | OF_EXOTIC | OF_FROZEN_ELEMS | OF_SEALED_ELEMS); alu_ri(c, 7, EDX, OF_ARRAY_FAST); jcc(c, CC_NE, SLOW());
        alu_rm(c, 0x3B, ECX, EAX, (int32_t)offsetof(struct obj, elen)); jcc(c, CC_AE, SLOW());
        mov_rm(c, EAX, EAX, (int32_t)offsetof(struct obj, elems));
        lea_x(c, EAX, EAX, ECX, 0);                                      // &elems[i]
        uint32_t ok = new_label(c);
        mov_rm(c, EDX, EAX, 4);
        alu_ri(c, 7, EDX, (int32_t)V_SPECIAL); jcc(c, CC_NE, ok);
        alu_mi(c, 7, EAX, 0, 4); jcc(c, CC_E, SLOW());                   // a hole
        bind(c, ok);
        mov_rm(c, ECX, SLOT_LO(V)); mov_mr(c, EAX, 0, ECX);
        mov_rm(c, ECX, SLOT_HI(V)); mov_mr(c, EAX, 4, ECX);
        CPY(ST(O), ST(V));
        break;
    }
    case OP_DEFINE_FIELD: {             // [obj, v] -> [obj]
        uint32_t ici = rd16(ip + 5);
        if (!t->ic || ici >= t->nic) goto generic;
        call_ic_helper(c, (const void*)jit_ic_define, ici, d - 2);
        test_rr(c, EAX, EAX); jcc(c, CC_E, SLOW());
        break;
    }
    case OP_GET_GLOBAL: {
        uint32_t ici = rd16(ip + 5);
        if (!t->ic || ici >= t->nic) goto generic;
        call_ic_helper(c, (const void*)jit_gl_get, ici, d);
        test_rr(c, EAX, EAX); jcc(c, CC_E, SLOW());
        break;
    }
    case OP_PUT_GLOBAL: case OP_PUT_GLOBAL_STRICT: {
        uint32_t ici = rd16(ip + 5);
        if (!t->ic || ici >= t->nic) goto generic;
        call_ic_helper(c, (const void*)jit_gl_put, ici, d - 1);
        test_rr(c, EAX, EAX); jcc(c, CC_E, SLOW());
        break;
    }
    case OP_CALL: case OP_CALL_METHOD: case OP_NEW: {
        int argc = rd16(ip + 1);
        int base = d - argc - 2;                                         // the callee's slot: [fn, this, args...] -> [result]
        mov_mi(c, EDI, F_OFF(pc), (uint32_t)(uintptr_t)(t->code + c->an.ord_pc[ord] + 3));
        lea(c, EAX, EBX, (int32_t)(8 * (c->nl + (uint32_t)d)));
        mov_mr(c, EDI, F_OFF(sp), EAX);
        mov_mr(c, ESP, 0, ESI);
        mov_mr(c, ESP, 4, EDI);
        lea(c, EAX, EBX, (int32_t)(8 * (c->nl + (uint32_t)(d - argc)))); mov_mr(c, ESP, 8, EAX);
        mov_mi(c, ESP, 12, (uint32_t)argc);
        call_c(c, op == OP_NEW ? (const void*)jit_new : (const void*)jit_call);
        alu_ri(c, 7, EDX, (int32_t)V_SPECIAL);
        { uint32_t ok = new_label(c); jcc(c, CC_NE, ok); alu_ri(c, 7, EAX, 5); jcc(c, CC_E, c->L_exc); bind(c, ok); }
        store_slot(c, ST(base), EAX, EDX);
        break;
    }
    case OP_FRAME_THIS: case OP_NEW_TARGET: {
        int32_t off = op == OP_FRAME_THIS ? F_OFF(this_v) : F_OFF(new_target);
        mov_rm(c, ECX, EDI, off);     mov_mr(c, SLOT_LO(d), ECX);
        mov_rm(c, ECX, EDI, off + 4); mov_mr(c, SLOT_HI(d), ECX);
        break;
    }
    case OP_RETURN:
        load_slot(c, EAX, EDX, ST(d - 1));
        jmp(c, c->L_epi);
        break;
    case OP_RETURN_UNDEF:
        mov_ri(c, EAX, 0); mov_ri(c, EDX, V_SPECIAL);
        jmp(c, c->L_epi);
        break;
    default: generic:
        c->nnative--; c->nstepped++;
        gen_step(c, ord, d);
        break;
    }
    if (c->fail) return;
#undef SLOW
#undef NEXT_L
}

// ---- the whole function
static int jit_compile_(ojs* J, struct ftempl* t);
int jit_compile(ojs* J, struct ftempl* t) {
#if !(defined(KERNEL) && KERNEL)
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = jit_compile_(J, t);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    jit_ms += (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    return r;
#else
    return jit_compile_(J, t);
#endif
}
static int jit_compile_(ojs* J, struct ftempl* t) {
    if (t->jit) return 0;
    if (t->flags & (TF_GENERATOR | TF_ASYNC | TF_SCRIPT | TF_MODULE | TF_EVAL | TF_DEFAULT_CTOR)) return -1;
    struct cg c;
    memset(&c, 0, sizeof c);
    c.J = J;
    c.t = t;
    c.nl = t->nlocals;
    if (!t->ic && t->nic) ic_entry(J, t, 0);        // the compiled code addresses the cache entries directly
    if (jan_run(t, &c.an) < 0) { ojs_jit_stats.rejected++; return -1; }
    uint32_t ni = c.an.ni;
    int ok = 0;
    // labels 0..ni = instruction starts (ni = end of code), then auxiliary ones
    for (uint32_t i = 0; i <= ni + 3; i++) new_label(&c);
    c.L_epi = ni + 1;
    c.L_fin = ni + 2;
    c.L_exc = ni + 3;
    c.skind = (uint8_t*)ojs_sys_malloc(ni + 1);
    c.stub = (uint32_t*)ojs_sys_malloc((ni + 1) * sizeof(uint32_t));
    c.stub_list = (uint32_t*)ojs_sys_malloc((ni + 1) * sizeof(uint32_t));
    if (c.fail || !c.stub || !c.stub_list || !c.skind) goto out;
    memset(c.stub, 0, (ni + 1) * sizeof(uint32_t));
    memset(c.skind, 0, ni + 1);

    // prologue: push ebp; mov ebp,esp; push ebx/esi/edi; sub esp,44; load J, f, locals; jump to the target in [ebp+16]
    em8(&c, 0x55); em8(&c, 0x89); em8(&c, 0xE5);
    em8(&c, 0x53); em8(&c, 0x56); em8(&c, 0x57);
    alu_ri(&c, 5, ESP, 44);
    mov_rm(&c, ESI, EBP, 8);
    mov_rm(&c, EDI, EBP, 12);
    mov_rm(&c, EBX, EDI, F_OFF(locals));
    em8(&c, 0xFF); mrm(&c, 4, EBP, 16);

    for (uint32_t o = 0; o < ni; o++) {
        bind(&c, o);
        if (c.an.depth[o] < 0) { em8(&c, 0x0F); em8(&c, 0x0B); continue; }          // ud2: never reached
        gen_ins(&c, o, c.an.depth[o]);
        if (c.fail) goto out;
    }
    bind(&c, ni);
    em8(&c, 0x0F); em8(&c, 0x0B);       // falling off the end cannot happen (the analysis checked)

    // the step stubs
    for (uint32_t i = 0; i < c.nstub; i++) {
        uint32_t o = c.stub_list[i];
        int dd = c.an.depth[o];
        bind(&c, c.stub[o]);
        if (c.skind[o] == 1) {          // GET_FIELD: the cache through C first
            call_ic_helper(&c, (const void*)jit_ic_get, rd16(t->code + c.an.ord_pc[o] + 5), dd - 1);
            test_rr(&c, EAX, EAX); jcc(&c, CC_NE, o + 1);
        } else if (c.skind[o] == 2) {   // PUT_FIELD
            call_ic_helper(&c, (const void*)jit_ic_put, rd16(t->code + c.an.ord_pc[o] + 5), dd - 2);
            test_rr(&c, EAX, EAX); jcc(&c, CC_NE, o + 1);
        }
        gen_step(&c, o, dd);
    }
    // a callee threw: unwind from f->pc (set by the call)
    bind(&c, c.L_exc);
    mov_mr(&c, ESP, 0, ESI);
    mov_mr(&c, ESP, 4, EDI);
    mov_mi(&c, ESP, 8, 0);
    mov_mi(&c, ESP, 12, 0);
    lea(&c, EAX, ESP, 32); mov_mr(&c, ESP, 16, EAX);
    mov_mi(&c, ESP, 20, 1);
    call_c(&c, (const void*)jit_step);
    test_rr(&c, EAX, EAX);
    jcc(&c, CC_E, c.L_fin);
    jmp_r(&c, EAX);
    // function exit: the result of a step is at [esp+32]
    bind(&c, c.L_fin);
    mov_rm(&c, EAX, ESP, 32);
    mov_rm(&c, EDX, ESP, 36);
    bind(&c, c.L_epi);
    alu_ri(&c, 0, ESP, 44);
    em8(&c, 0x5F); em8(&c, 0x5E); em8(&c, 0x5B); em8(&c, 0x5D);
    ret(&c);
    if (c.fail) goto out;

    // place it
    {
        size_t size = c.n;
        uint8_t* mem = (uint8_t*)exec_alloc(size);
        if (!mem) goto out;
        memcpy(mem, c.b, size);
        for (uint32_t i = 0; i < c.nrel; i++) {
            uint32_t tgt = c.lab[c.rel[i].lab];
            if (tgt == ~0u) { exec_free(mem, size); goto out; }
            uint32_t v = tgt - (c.rel[i].pos + 4);
            memcpy(mem + c.rel[i].pos, &v, 4);
        }
        for (uint32_t i = 0; i < c.nabs; i++) {
            uint32_t tgt = c.lab[c.abs[i].lab];
            if (tgt == ~0u) { exec_free(mem, size); goto out; }
            uint32_t v = (uint32_t)(uintptr_t)mem + tgt;
            memcpy(mem + c.abs[i].pos, &v, 4);
        }
        for (uint32_t i = 0; i < c.ncalls; i++) {
            uint32_t v = (uint32_t)(uintptr_t)c.calls[i].fn - ((uint32_t)(uintptr_t)mem + c.calls[i].pos + 4);
            memcpy(mem + c.calls[i].pos, &v, 4);
        }
        struct jitcode* jc = (struct jitcode*)ojs_sys_malloc(sizeof *jc);
        uint32_t nmap = 0;
        for (uint32_t o = 0; o < ni; o++) if (c.an.depth[o] >= 0) nmap++;
        uint32_t* pcs = (uint32_t*)ojs_sys_malloc((nmap + 1) * sizeof(uint32_t));
        void** addr = (void**)ojs_sys_malloc((nmap + 1) * sizeof(void*));
        if (!jc || !pcs || !addr) { ojs_sys_free(jc); ojs_sys_free(pcs); ojs_sys_free(addr); exec_free(mem, size); goto out; }
        uint32_t k = 0;
        for (uint32_t o = 0; o < ni; o++) if (c.an.depth[o] >= 0) { pcs[k] = c.an.ord_pc[o]; addr[k] = mem + c.lab[o]; k++; }
        jc->mem = mem; jc->size = size; jc->entry = mem; jc->start = mem + c.lab[0];
        jc->n = nmap; jc->pcs = pcs; jc->addr = addr;
        t->jit = jc;
        t->h.gcflags |= GCF_FINAL;
        ojs_jit_stats.compiled++;
        ojs_jit_stats.code_bytes += size;
        ojs_jit_stats.instructions += ni;
        ojs_jit_stats.native += c.nnative;
        ojs_jit_stats.stepped += c.nstepped;
#if !(defined(KERNEL) && KERNEL)
        { const char* dump = getenv("OJS_JIT_DUMP");
          if (dump && *dump) { FILE* fp = fopen(dump, "ab"); if (fp) { fwrite(mem, 1, size, fp); fclose(fp); } } }
#endif
        ok = 1;
    }
out:
    ojs_sys_free(c.b); ojs_sys_free(c.lab); ojs_sys_free(c.rel); ojs_sys_free(c.abs); ojs_sys_free(c.calls);
    ojs_sys_free(c.stub); ojs_sys_free(c.stub_list); ojs_sys_free(c.skind);
    jan_free(&c.an);
    if (!ok) { ojs_jit_stats.rejected++; return -1; }
    return 0;
}

// ---------------------------------------------------------------- run time

static int jit_hot(ojs* J, struct ftempl* t) {
    if (t->jit_bad) return 0;
    if (ojs_jit_mode < 2 && ++t->hot < ojs_jit_threshold) return 0;
    if (jit_compile(J, t) < 0) { t->jit_bad = 1; return 0; }
    return t->jit != 0;
}

// a loop back-edge in the interpreter: compile the function once it is hot and carry on in native code from the loop head. 1: the function ran
// to its end there (*res is its result)
static int jit_osr(ojs* J, struct ojs_frame* f, struct ftempl* t, uint8_t* pc, jv* res) {
    if (!t->jit) {
        if (f->resume_mode || (t->flags & (TF_GENERATOR | TF_ASYNC))) return 0;
        if (ojs_jit_mode < 2 && ++t->hot < ojs_jit_threshold) return 0;
        if (jit_compile(J, t) < 0) { t->jit_bad = 1; return 0; }
    }
    void* a = jit_native_at(t->jit, (uint32_t)(pc - t->code));
    if (!a) return 0;
    *res = jit_exec(J, f, a);
    return 1;
}

static jv jit_exec(ojs* J, struct ojs_frame* f, void* target) {
    return ((jv (*)(ojs*, struct ojs_frame*, void*))f->t->jit->entry)(J, f, target);
}

// a whole activation of a compiled function (what vm_run does around the loop)
static jv jit_run_frame(ojs* J, struct ojs_frame* f) {
    struct ojs_frame* saved = J->frame;
    f->prev = saved;
    J->frame = f;
    jv r = jit_exec(J, f, f->t->jit->start);
    if (f->open) close_upvals(J, f, f->locals);
    J->frame = saved;
    return r;
}

// Compiled code asks for one instruction to be run by the interpreter (mode 1: unwind an exception first). Returns the native address to
// continue at, or 0 when the function is over (*res = its result, JV_EXC for an exception nobody catches).
#if !(defined(KERNEL) && KERNEL)
static unsigned long long step_count[256];
#endif
void* jit_step(ojs* J, struct ojs_frame* f, void* resume, const uint8_t* resume_pc, jv* res, int mode) {
#if !(defined(KERNEL) && KERNEL)
    if (!mode) step_count[*f->pc]++; else step_count[0]++;
#endif
    int r = vm_step(J, f, res, mode);
    if (r) return 0;
    if (f->pc == resume_pc) return resume;
    void* a = jit_native_at(f->t->jit, (uint32_t)(f->pc - f->t->code));
    if (a) return a;
    throw_error(J, NE_TYPE, "internal: no compiled code at pc %d", (int)(f->pc - f->t->code));
    *res = JV_EXC;
    return 0;
}

#if !(defined(KERNEL) && KERNEL)
__attribute__((destructor)) static void jit_stats_dump(void) {
    if (!getenv("OJS_JIT_STATS")) return;
    { unsigned long long tot = 0; for (int i = 0; i < 256; i++) tot += step_count[i];
      fprintf(stderr, "[jit] %llu steps through the interpreter\n", tot);
      for (int r = 0; r < 25; r++) { int b = -1; for (int i = 0; i < 256; i++) if (step_count[i] && (b < 0 || step_count[i] > step_count[b])) b = i;
        if (b < 0) break; fprintf(stderr, "[jit]   %-18s %llu\n", b == 0 ? "(call exception)" : op_name(b), step_count[b]); step_count[b] = 0; } }
    fprintf(stderr, "[jit] compiled %u in %.0f ms, rejected %u, %llu bytes of code, %llu instructions (%llu native, %llu via the interpreter)\n",
            ojs_jit_stats.compiled, jit_ms, ojs_jit_stats.rejected, (unsigned long long)ojs_jit_stats.code_bytes,
            (unsigned long long)ojs_jit_stats.instructions, (unsigned long long)ojs_jit_stats.native, (unsigned long long)ojs_jit_stats.stepped);
}
#endif

#ifdef OJS_JIT_CHECK
// test builds: every instruction the interpreter executes is compared with the statically computed stack depth of the analysis
static struct { struct ftempl* t; struct jan an; int ok; } chk_cache[8192];
static unsigned chk_n;
static void jit_check_depth(struct ftempl* t, uint32_t pc_off, int depth) {
    static unsigned last;
    unsigned i = last;
    if (i >= chk_n || chk_cache[i].t != t) for (i = 0; i < chk_n; i++) if (chk_cache[i].t == t) break;
    last = i;
    if (i == chk_n) {
        if (chk_n == sizeof chk_cache / sizeof chk_cache[0]) return;
        chk_cache[i].t = t;
        chk_cache[i].ok = jan_run(t, &chk_cache[i].an) == 0;
        chk_n++;
    }
    if (!chk_cache[i].ok) return;
    struct jan* a = &chk_cache[i].an;
    uint32_t o = pc_off <= a->ord_pc[a->ni] ? a->pc_ord[pc_off] : ~0u;      // (the length of the code when it was analysed)
    if (o == ~0u || a->depth[o] != depth) {           // (a collected template's address may have been reused: analyse again before believing it)
        jan_free(a);
        chk_cache[i].ok = jan_run(t, a) == 0;
        if (!chk_cache[i].ok) return;
        o = a->pc_ord[pc_off];
    }
    if (o == ~0u) { fprintf(stderr, "[jit-check] pc %u is not an instruction start\n", pc_off); abort(); }
    if (a->depth[o] != depth) {
        fprintf(stderr, "[jit-check] %s at pc %u: interpreter depth %d, analysis %d\n", op_name(t->code[pc_off]), pc_off, depth, a->depth[o]);
        abort();
    }
}
#endif

// shell command `jit`: the mode and what has been compiled
void ojs_jit_set_mode(int m) { ojs_jit_mode = m; }
void ojs_jit_report(char* buf, int cap) {
    ojs_snprintf(buf, cap, "JIT %s: %u functions compiled (%u refused), %u KB of code, %u instructions, %u translated, %u run by the interpreter; threshold %u\n",
                 ojs_jit_mode == 0 ? "off" : ojs_jit_mode == 1 ? "on" : "eager", ojs_jit_stats.compiled, ojs_jit_stats.rejected,
                 (unsigned)(ojs_jit_stats.code_bytes >> 10), (unsigned)ojs_jit_stats.instructions, (unsigned)ojs_jit_stats.native,
                 (unsigned)ojs_jit_stats.stepped, ojs_jit_threshold);
}
void ojs_jit_set_threshold(uint32_t n) { ojs_jit_threshold = n; }
