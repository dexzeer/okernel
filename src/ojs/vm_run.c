// vm_run.c — the bytecode interpreter (included by vm.c).

#include "vm_helpers.c"

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static jv tdz_error(ojs* J, jv name) {
    if (jv_is_str(name)) return throw_ref(J, "Cannot access '%S' before initialization", str_flat(J, name));
    return throw_ref(J, "Cannot access binding before initialization");
}

static int interrupt_check(ojs* J) {
    if (!J->interrupt) return 0;
    J->interrupt_counter = 4096;
    if (J->interrupt(J, J->interrupt_op)) {
        throw_error(J, NE_RANGE, "interrupted");
        J->uncatchable = 1;
        return -1;
    }
    return 0;
}

// unscopables: is `name` blocked on with object o?
static int with_has(ojs* J, struct obj* o, pkey k) {
    int h = obj_has(J, o, k);
    if (h <= 0) return h;
    if (o->flags & OF_EVALVARS) return 1;
    jv u = obj_get(J, o, pk_from_sym(J->wk[WK_UNSCOPABLES]), jv_from_obj(o));
    if (u == JV_EXC) return -1;
    if (jv_is_obj(u)) {
        jv b = obj_get(J, jv_obj(u), k, u);
        if (b == JV_EXC) return -1;
        if (to_boolean(b)) return 0;
    }
    return 1;
}

jv vm_run(ojs* J, struct ojs_frame* f) {
    struct ojs_frame* saved_frame = J->frame;
    f->prev = saved_frame;
    J->frame = f;
    struct ftempl* t = f->t;
    uint8_t* code = t->code;
    uint8_t* pc = f->pc;
    jv* sp = f->sp;
    jv* locals = f->locals;
    jv* consts = t->consts;
    struct upval** upv = f->fn ? f->fn->upv : 0;
    int strict = (t->flags & TF_STRICT) != 0;
    jv pending = JV_UNDEFINED;   // exception being unwound
    jv result = JV_UNDEFINED;
    jv a, b, c, v;

#define PUSH(x) (*sp++ = (x))
#define POP() (*--sp)
#define TOP (sp[-1])
#define SYNC() (f->pc = pc, f->sp = sp)
#define CHECK(x) do { if ((x) == JV_EXC) goto exception; } while (0)
#define CHECKI(x) do { if ((x) < 0) goto exception; } while (0)
#define JUMP(off) (pc += (int32_t)(off))

    // generator / async resumption
    if (f->resume_mode) {
        int m = f->resume_mode;
        jv rv = f->resume_value;
        f->resume_mode = 0;
        f->resume_value = JV_UNDEFINED;
        int initial = (f->flags & FRF_INITIAL) != 0;
        int ystar = (f->flags & FRF_YSTAR) != 0;
        f->flags &= ~(FRF_SUSPENDED | FRF_YIELD_RAW | FRF_AWAITING | FRF_INITIAL | FRF_YSTAR);
        if (ystar) {
            // re-run YIELD_STAR with the received value and mode
            if (f->ystar_phase) {
                // an await inside an async yield*: fulfilled -> continue, rejected -> throw
                if (m == 2) {
                    f->ystar_phase = 0;
                    f->ystar_mode = 0;
                    pending = rv;
                    goto unwind;
                }
            } else f->ystar_mode = m - 1;
            PUSH(rv);
            pc--;   // YIELD_STAR (no operands) runs again
        } else if (m == 1) {
            if (!initial) PUSH(rv);
        } else if (m == 2) {
            pending = rv;
            goto unwind;
        } else {
            f->completion = rv;
            pending = JV_GENRET;
            goto unwind;
        }
    }

#if defined(__GNUC__) && !defined(OJS_SWITCH_DISPATCH)
    // direct threading: the jump table is indexed by the opcode byte itself, and GCC
    // copies the (small) dispatch block into every instruction's tail
    // (opcodes without a case of their own fall to L_default; the range default is
    // overridden entry by entry on purpose)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
    static const void* const dispatch[256] = {
        [0 ... 255] = &&L_default,
        [OP_NOP] = &&L_OP_NOP, [OP_DEBUGGER] = &&L_OP_DEBUGGER, [OP_NOP_END] = &&L_OP_NOP_END,
        [OP_LINE] = &&L_OP_LINE, [OP_UNDEF] = &&L_OP_UNDEF, [OP_NULL_] = &&L_OP_NULL_, [OP_TRUE_] = &&L_OP_TRUE_,
        [OP_FALSE_] = &&L_OP_FALSE_, [OP_HOLE] = &&L_OP_HOLE, [OP_INT8] = &&L_OP_INT8, [OP_INT32] = &&L_OP_INT32,
        [OP_CONST] = &&L_OP_CONST, [OP_CONST_W] = &&L_OP_CONST_W, [OP_DUP] = &&L_OP_DUP, [OP_DUP2] = &&L_OP_DUP2, [OP_POP] = &&L_OP_POP,
        [OP_SWAP] = &&L_OP_SWAP, [OP_ROT3L] = &&L_OP_ROT3L, [OP_ROT3R] = &&L_OP_ROT3R, [OP_ROT4R] = &&L_OP_ROT4R,
        [OP_PICK] = &&L_OP_PICK, [OP_NIP] = &&L_OP_NIP, [OP_INSERT] = &&L_OP_INSERT, [OP_GET_LOC] = &&L_OP_GET_LOC,
        [OP_SET_LOC] = &&L_OP_SET_LOC, [OP_PUT_LOC] = &&L_OP_PUT_LOC, [OP_INIT_LOC] = &&L_OP_INIT_LOC,
        [OP_HOLE_LOC] = &&L_OP_HOLE_LOC, [OP_GET_LOC_CHK] = &&L_OP_GET_LOC_CHK,
        [OP_PUT_LOC_CHK] = &&L_OP_PUT_LOC_CHK, [OP_GET_UPV] = &&L_OP_GET_UPV, [OP_SET_UPV] = &&L_OP_SET_UPV,
        [OP_PUT_UPV] = &&L_OP_PUT_UPV, [OP_INIT_UPV] = &&L_OP_INIT_UPV, [OP_GET_UPV_CHK] = &&L_OP_GET_UPV_CHK,
        [OP_PUT_UPV_CHK] = &&L_OP_PUT_UPV_CHK, [OP_CLOSE] = &&L_OP_CLOSE, [OP_BIND_THIS_LOC] = &&L_OP_BIND_THIS_LOC,
        [OP_BIND_THIS_UPV] = &&L_OP_BIND_THIS_UPV, [OP_GET_GLOBAL] = &&L_OP_GET_GLOBAL,
        [OP_TYPEOF_GLOBAL] = &&L_OP_TYPEOF_GLOBAL, [OP_PUT_GLOBAL] = &&L_OP_PUT_GLOBAL,
        [OP_PUT_GLOBAL_STRICT] = &&L_OP_PUT_GLOBAL_STRICT, [OP_INIT_GLOBAL_LEX] = &&L_OP_INIT_GLOBAL_LEX,
        [OP_DECL_GLOBAL_VAR] = &&L_OP_DECL_GLOBAL_VAR, [OP_DECL_GLOBAL_FUNC] = &&L_OP_DECL_GLOBAL_FUNC,
        [OP_DECL_GLOBAL_LEX] = &&L_OP_DECL_GLOBAL_LEX, [OP_CHECK_GLOBAL_DECLS] = &&L_OP_CHECK_GLOBAL_DECLS,
        [OP_DELETE_GLOBAL] = &&L_OP_DELETE_GLOBAL, [OP_GET_FIELD] = &&L_OP_GET_FIELD,
        [OP_GET_FIELD_KEEP] = &&L_OP_GET_FIELD_KEEP, [OP_PUT_FIELD] = &&L_OP_PUT_FIELD,
        [OP_GET_ELEM] = &&L_OP_GET_ELEM, [OP_GET_ELEM_KEEP] = &&L_OP_GET_ELEM_KEEP, [OP_PUT_ELEM] = &&L_OP_PUT_ELEM,
        [OP_DELETE_FIELD] = &&L_OP_DELETE_FIELD, [OP_DELETE_ELEM] = &&L_OP_DELETE_ELEM,
        [OP_TO_PROPKEY] = &&L_OP_TO_PROPKEY, [OP_GET_SUPER] = &&L_OP_GET_SUPER,
        [OP_GET_SUPER_KEEP] = &&L_OP_GET_SUPER_KEEP, [OP_PUT_SUPER] = &&L_OP_PUT_SUPER,
        [OP_SUPER_CTOR] = &&L_OP_SUPER_CTOR, [OP_GET_PRIVATE] = &&L_OP_GET_PRIVATE,
        [OP_PUT_PRIVATE] = &&L_OP_PUT_PRIVATE, [OP_DEFINE_PRIVATE] = &&L_OP_DEFINE_PRIVATE,
        [OP_PRIVATE_IN] = &&L_OP_PRIVATE_IN, [OP_ADD_PRIVATE_METHOD] = &&L_OP_ADD_PRIVATE_METHOD,
        [OP_PRIVATE_NAME] = &&L_OP_PRIVATE_NAME, [OP_ADD] = &&L_OP_ADD, [OP_SUB] = &&L_OP_SUB, [OP_MUL] = &&L_OP_MUL,
        [OP_DIV] = &&L_OP_DIV, [OP_MOD] = &&L_OP_MOD, [OP_BAND] = &&L_OP_BAND, [OP_BOR] = &&L_OP_BOR,
        [OP_BXOR] = &&L_OP_BXOR, [OP_SHL] = &&L_OP_SHL, [OP_SAR] = &&L_OP_SAR, [OP_SHR] = &&L_OP_SHR,
        [OP_POW] = &&L_OP_POW, [OP_NEG] = &&L_OP_NEG, [OP_INC] = &&L_OP_INC, [OP_DEC] = &&L_OP_DEC,
        [OP_PLUS] = &&L_OP_PLUS, [OP_BITNOT] = &&L_OP_BITNOT, [OP_TO_NUMERIC] = &&L_OP_TO_NUMERIC,
        [OP_NOT] = &&L_OP_NOT, [OP_EQ] = &&L_OP_EQ, [OP_NE] = &&L_OP_NE, [OP_SEQ] = &&L_OP_SEQ,
        [OP_SNE] = &&L_OP_SNE, [OP_LT] = &&L_OP_LT, [OP_LE] = &&L_OP_LE, [OP_GT] = &&L_OP_GT, [OP_GE] = &&L_OP_GE,
        [OP_IN] = &&L_OP_IN, [OP_INSTANCEOF] = &&L_OP_INSTANCEOF, [OP_TYPEOF] = &&L_OP_TYPEOF,
        [OP_IS_UNDEF] = &&L_OP_IS_UNDEF, [OP_IS_NULLISH] = &&L_OP_IS_NULLISH, [OP_TO_STRING] = &&L_OP_TO_STRING,
        [OP_TO_OBJECT] = &&L_OP_TO_OBJECT, [OP_TO_PRIMITIVE] = &&L_OP_TO_PRIMITIVE, [OP_JMP] = &&L_OP_JMP,
        [OP_JF] = &&L_OP_JF, [OP_JT] = &&L_OP_JT, [OP_JF_KEEP] = &&L_OP_JF_KEEP, [OP_JT_KEEP] = &&L_OP_JT_KEEP,
        [OP_JNULLISH] = &&L_OP_JNULLISH, [OP_JNOTNULLISH_KEEP] = &&L_OP_JNOTNULLISH_KEEP,
        [OP_JUNDEF] = &&L_OP_JUNDEF, [OP_CALL] = &&L_OP_CALL, [OP_CALL_METHOD] = &&L_OP_CALL_METHOD,
        [OP_NEW] = &&L_OP_NEW, [OP_CALL_SPREAD] = &&L_OP_CALL_SPREAD,
        [OP_CALL_METHOD_SPREAD] = &&L_OP_CALL_METHOD_SPREAD, [OP_NEW_SPREAD] = &&L_OP_NEW_SPREAD,
        [OP_SUPER_CALL] = &&L_OP_SUPER_CALL, [OP_SUPER_CALL_SPREAD] = &&L_OP_SUPER_CALL_SPREAD,
        [OP_EVAL] = &&L_OP_EVAL, [OP_EVAL_SPREAD] = &&L_OP_EVAL_SPREAD,
        [OP_CALL_IGNORED_REF_ERROR] = &&L_OP_CALL_IGNORED_REF_ERROR, [OP_RETURN] = &&L_OP_RETURN,
        [OP_RETURN_UNDEF] = &&L_OP_RETURN_UNDEF, [OP_THROW] = &&L_OP_THROW, [OP_THROW_ERR] = &&L_OP_THROW_ERR,
        [OP_TDZ_ERROR] = &&L_OP_TDZ_ERROR, [OP_CONST_ERROR] = &&L_OP_CONST_ERROR, [OP_OBJECT] = &&L_OP_OBJECT,
        [OP_ARRAY] = &&L_OP_ARRAY, [OP_APPEND_ONE] = &&L_OP_APPEND_ONE, [OP_APPEND_HOLE] = &&L_OP_APPEND_HOLE,
        [OP_APPEND] = &&L_OP_APPEND, [OP_DEFINE_FIELD] = &&L_OP_DEFINE_FIELD, [OP_DEFINE_ELEM] = &&L_OP_DEFINE_ELEM,
        [OP_DEFINE_METHOD] = &&L_OP_DEFINE_METHOD, [OP_DEFINE_METHOD_ELEM] = &&L_OP_DEFINE_METHOD_ELEM,
        [OP_SET_PROTO_LIT] = &&L_OP_SET_PROTO_LIT, [OP_COPY_DATA_PROPS] = &&L_OP_COPY_DATA_PROPS,
        [OP_REST_OBJ] = &&L_OP_REST_OBJ, [OP_SET_NAME] = &&L_OP_SET_NAME, [OP_SET_NAME_ELEM] = &&L_OP_SET_NAME_ELEM,
        [OP_SET_HOME] = &&L_OP_SET_HOME, [OP_CLOSURE] = &&L_OP_CLOSURE, [OP_CLASS] = &&L_OP_CLASS,
        [OP_CLASS_FIELDS] = &&L_OP_CLASS_FIELDS, [OP_RUN_FIELDS] = &&L_OP_RUN_FIELDS,
        [OP_GET_ITER] = &&L_OP_GET_ITER, [OP_GET_ASYNC_ITER] = &&L_OP_GET_ASYNC_ITER,
        [OP_ITER_CALL] = &&L_OP_ITER_CALL, [OP_ITER_CHECK_OBJ] = &&L_OP_ITER_CHECK_OBJ,
        [OP_ITER_STEP] = &&L_OP_ITER_STEP, [OP_FOR_OF_STEP] = &&L_OP_FOR_OF_STEP, [OP_ITER_VALUE] = &&L_OP_ITER_VALUE, [OP_ITER_CLOSE] = &&L_OP_ITER_CLOSE,
        [OP_ITER_CLOSE_QUIET] = &&L_OP_ITER_CLOSE_QUIET, [OP_ASYNC_ITER_CLOSE] = &&L_OP_ASYNC_ITER_CLOSE,
        [OP_SPREAD_ARRAY] = &&L_OP_SPREAD_ARRAY, [OP_FOR_IN_START] = &&L_OP_FOR_IN_START,
        [OP_FOR_IN_NEXT] = &&L_OP_FOR_IN_NEXT, [OP_REGEXP] = &&L_OP_REGEXP, [OP_REST] = &&L_OP_REST,
        [OP_ARGUMENTS] = &&L_OP_ARGUMENTS, [OP_THIS_FUNC] = &&L_OP_THIS_FUNC, [OP_FRAME_THIS] = &&L_OP_FRAME_THIS,
        [OP_NEW_TARGET] = &&L_OP_NEW_TARGET, [OP_HOME_OBJECT] = &&L_OP_HOME_OBJECT,
        [OP_IMPORT_META] = &&L_OP_IMPORT_META, [OP_IMPORT_CALL] = &&L_OP_IMPORT_CALL,
        [OP_WITH_GET] = &&L_OP_WITH_GET, [OP_WITH_TYPEOF] = &&L_OP_WITH_TYPEOF,
        [OP_WITH_CALL_THIS] = &&L_OP_WITH_CALL_THIS, [OP_WITH_DELETE] = &&L_OP_WITH_DELETE,
        [OP_WITH_PUT] = &&L_OP_WITH_PUT, [OP_EVAL_VAR_DECL] = &&L_OP_EVAL_VAR_DECL,
        [OP_INITIAL_YIELD] = &&L_OP_INITIAL_YIELD, [OP_YIELD] = &&L_OP_YIELD,
        [OP_ASYNC_GEN_YIELD] = &&L_OP_ASYNC_GEN_YIELD, [OP_AWAIT] = &&L_OP_AWAIT,
        [OP_YIELD_STAR] = &&L_OP_YIELD_STAR,
    };
#pragma GCC diagnostic pop
#endif
    uint8_t opc;
    for (;;) {
        // f->sp is only synced where it matters (SYNC before calls, suspension, exit):
        // the collector scans whole VM stack chunks and running heap frames
        opc = *pc++;
#if defined(__GNUC__) && !defined(OJS_SWITCH_DISPATCH)
        goto *dispatch[opc];
#endif
        switch (opc) {
        case OP_NOP: L_OP_NOP: case OP_DEBUGGER: L_OP_DEBUGGER: case OP_NOP_END: L_OP_NOP_END: break;
        case OP_LINE: L_OP_LINE: pc += 4; break;
        case OP_UNDEF: L_OP_UNDEF: PUSH(JV_UNDEFINED); break;
        case OP_NULL_: L_OP_NULL_: PUSH(JV_NULL); break;
        case OP_TRUE_: L_OP_TRUE_: PUSH(JV_TRUE); break;
        case OP_FALSE_: L_OP_FALSE_: PUSH(JV_FALSE); break;
        case OP_HOLE: L_OP_HOLE: PUSH(JV_HOLE); break;
        case OP_INT8: L_OP_INT8: PUSH(jv_from_int((int8_t)*pc)); pc++; break;
        case OP_INT32: L_OP_INT32: PUSH(jv_from_int((int32_t)rd32(pc))); pc += 4; break;
        case OP_CONST: L_OP_CONST: PUSH(consts[rd16(pc)]); pc += 2; break;
        case OP_CONST_W: L_OP_CONST_W: PUSH(consts[rd32(pc)]); pc += 4; break;
        case OP_DUP: L_OP_DUP: a = TOP; PUSH(a); break;
        case OP_DUP2: L_OP_DUP2: a = sp[-2]; b = sp[-1]; PUSH(a); PUSH(b); break;
        case OP_POP: L_OP_POP: sp--; break;
        case OP_SWAP: L_OP_SWAP: a = sp[-1]; sp[-1] = sp[-2]; sp[-2] = a; break;
        case OP_ROT3L: L_OP_ROT3L: a = sp[-3]; sp[-3] = sp[-2]; sp[-2] = sp[-1]; sp[-1] = a; break;   // [a b c] -> [b c a]
        case OP_ROT3R: L_OP_ROT3R: a = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = a; break;   // [a b c] -> [c a b]
        case OP_ROT4R: L_OP_ROT4R: a = sp[-1]; sp[-1] = sp[-2]; sp[-2] = sp[-3]; sp[-3] = sp[-4]; sp[-4] = a; break;
        case OP_PICK: L_OP_PICK: { uint8_t n = *pc++; a = sp[-1 - n]; PUSH(a); break; }
        case OP_NIP: L_OP_NIP: sp[-2] = sp[-1]; sp--; break;
        case OP_INSERT: L_OP_INSERT: {   // move the top below n items
            uint8_t n = *pc++;
            a = sp[-1];
            for (int i = 1; i <= n; i++) sp[-i] = sp[-i - 1];
            sp[-1 - n] = a;
            break;
        }

        // ---- locals / upvalues
        case OP_GET_LOC: L_OP_GET_LOC: PUSH(locals[rd16(pc)]); pc += 2; break;
        case OP_SET_LOC: L_OP_SET_LOC: locals[rd16(pc)] = TOP; pc += 2; break;
        case OP_PUT_LOC: L_OP_PUT_LOC: locals[rd16(pc)] = POP(); pc += 2; break;
        case OP_INIT_LOC: L_OP_INIT_LOC: locals[rd16(pc)] = TOP; pc += 2; break;
        case OP_HOLE_LOC: L_OP_HOLE_LOC: locals[rd16(pc)] = JV_HOLE; pc += 2; break;
        case OP_GET_LOC_CHK: L_OP_GET_LOC_CHK: {
            uint16_t s = rd16(pc);
            uint32_t nm = rd32(pc + 2);
            pc += 6;
            a = locals[s];
            if (a == JV_HOLE) { SYNC(); tdz_error(J, consts[nm]); goto exception; }
            PUSH(a);
            break;
        }
        case OP_PUT_LOC_CHK: L_OP_PUT_LOC_CHK: {
            uint16_t s = rd16(pc);
            uint32_t nm = rd32(pc + 2);
            pc += 6;
            if (locals[s] == JV_HOLE) { SYNC(); tdz_error(J, consts[nm]); goto exception; }
            locals[s] = TOP;
            break;
        }
        case OP_GET_UPV: L_OP_GET_UPV: PUSH(*upv[rd16(pc)]->loc); pc += 2; break;
        case OP_SET_UPV: L_OP_SET_UPV: *upv[rd16(pc)]->loc = TOP; pc += 2; break;
        case OP_PUT_UPV: L_OP_PUT_UPV: *upv[rd16(pc)]->loc = POP(); pc += 2; break;
        case OP_INIT_UPV: L_OP_INIT_UPV: *upv[rd16(pc)]->loc = TOP; pc += 2; break;
        case OP_GET_UPV_CHK: L_OP_GET_UPV_CHK: {
            uint16_t s = rd16(pc);
            uint32_t nm = rd32(pc + 2);
            pc += 6;
            a = *upv[s]->loc;
            if (a == JV_HOLE) { SYNC(); tdz_error(J, consts[nm]); goto exception; }
            PUSH(a);
            break;
        }
        case OP_PUT_UPV_CHK: L_OP_PUT_UPV_CHK: {
            uint16_t s = rd16(pc);
            uint32_t nm = rd32(pc + 2);
            pc += 6;
            if (*upv[s]->loc == JV_HOLE) { SYNC(); tdz_error(J, consts[nm]); goto exception; }
            *upv[s]->loc = TOP;
            break;
        }
        case OP_CLOSE: L_OP_CLOSE: close_upvals(J, f, &locals[rd16(pc)]); pc += 2; break;
        case OP_BIND_THIS_LOC: L_OP_BIND_THIS_LOC: {
            uint16_t s = rd16(pc);
            pc += 2;
            if (locals[s] != JV_HOLE) { SYNC(); throw_ref(J, "Super constructor may only be called once"); goto exception; }
            locals[s] = TOP;
            f->this_v = TOP;
            break;
        }
        case OP_BIND_THIS_UPV: L_OP_BIND_THIS_UPV: {
            uint16_t s = rd16(pc);
            pc += 2;
            if (*upv[s]->loc != JV_HOLE) { SYNC(); throw_ref(J, "Super constructor may only be called once"); goto exception; }
            *upv[s]->loc = TOP;
            break;
        }

        // ---- globals
        case OP_GET_GLOBAL: L_OP_GET_GLOBAL: case OP_TYPEOF_GLOBAL: L_OP_TYPEOF_GLOBAL: {
            jv nm = consts[rd32(pc)];
            uint32_t ici = rd16(pc + 4);
            pc += 6;
            if (ic_global_get(J, t, ici, &v)) { PUSH(v); break; }
            SYNC();
            v = global_get(J, jv_str(nm), opc == OP_TYPEOF_GLOBAL);
            CHECK(v);
            PUSH(v);
            ic_fill_global(J, t, ici, atom_key(J, nm), 0);
            break;
        }
        case OP_PUT_GLOBAL: L_OP_PUT_GLOBAL: case OP_PUT_GLOBAL_STRICT: L_OP_PUT_GLOBAL_STRICT: {
            jv nm = consts[rd32(pc)];
            uint32_t ici = rd16(pc + 4);
            pc += 6;
            if (ic_global_put(J, t, ici, TOP)) break;
            SYNC();
            CHECKI(global_put(J, jv_str(nm), TOP, opc == OP_PUT_GLOBAL_STRICT));
            ic_fill_global(J, t, ici, atom_key(J, nm), 1);
            break;
        }
        case OP_INIT_GLOBAL_LEX: L_OP_INIT_GLOBAL_LEX: {
            SYNC();
            jv nm = consts[rd32(pc)];
            pc += 4;
            CHECKI(global_init_lex(J, jv_str(nm), TOP));
            break;
        }
        case OP_DECL_GLOBAL_VAR: L_OP_DECL_GLOBAL_VAR: {
            SYNC();
            jv nm = consts[rd32(pc)];
            pc += 4;
            CHECKI(global_decl_var(J, jv_str(nm), (t->flags & TF_EVAL) != 0));
            break;
        }
        case OP_DECL_GLOBAL_FUNC: L_OP_DECL_GLOBAL_FUNC: {
            SYNC();
            jv nm = consts[rd32(pc)];
            pc += 4;
            a = POP();
            CHECKI(global_decl_func(J, jv_str(nm), a, (t->flags & TF_EVAL) != 0));
            break;
        }
        case OP_DECL_GLOBAL_LEX: L_OP_DECL_GLOBAL_LEX: {
            SYNC();
            jv nm = consts[rd32(pc)];
            uint16_t is_const = rd16(pc + 4);
            pc += 6;
            CHECKI(global_decl_lex(J, jv_str(nm), is_const));
            break;
        }
        case OP_CHECK_GLOBAL_DECLS: L_OP_CHECK_GLOBAL_DECLS: {
            SYNC();
            jv names = consts[rd32(pc)];
            pc += 4;
            CHECKI(global_check_decls(J, jv_obj(names)));
            break;
        }
        case OP_DELETE_GLOBAL: L_OP_DELETE_GLOBAL: {
            SYNC();
            jv nm = consts[rd32(pc)];
            pc += 4;
            v = global_delete(J, jv_str(nm));
            CHECK(v);
            PUSH(v);
            break;
        }

        // ---- properties
        case OP_GET_FIELD: L_OP_GET_FIELD: {
            jv nm = consts[rd32(pc)];
            uint32_t ici = rd16(pc + 4);
            pc += 6;
            a = TOP;
            if (jv_is_obj(a) && ic_get(t, ici, jv_obj(a), &v)) { TOP = v; break; }
            SYNC();
            pkey k = atom_key(J, nm);
            v = get_prop(J, a, k);
            CHECK(v);
            TOP = v;
            if (jv_is_obj(a)) ic_fill_get(J, t, ici, jv_obj(a), k);
            break;
        }
        case OP_GET_FIELD_KEEP: L_OP_GET_FIELD_KEEP: {   // [obj] -> [value, obj]
            jv nm = consts[rd32(pc)];
            uint32_t ici = rd16(pc + 4);
            pc += 6;
            a = TOP;
            if (jv_is_obj(a) ? ic_get(t, ici, jv_obj(a), &v) : jv_is_str(a) && ic_get_str(J, t, ici, &v)) { TOP = v; PUSH(a); break; }
            SYNC();
            pkey k = atom_key(J, nm);
            v = get_prop(J, a, k);
            CHECK(v);
            TOP = v;
            PUSH(a);
            if (jv_is_obj(a)) ic_fill_get(J, t, ici, jv_obj(a), k);
            else if (jv_is_str(a)) ic_fill_get_str(J, t, ici, k);
            break;
        }
        case OP_PUT_FIELD: L_OP_PUT_FIELD: {   // [obj, value] -> [value]
            jv nm = consts[rd32(pc)];
            uint32_t ici = rd16(pc + 4);
            pc += 6;
            b = sp[-1];
            a = sp[-2];
            if (jv_is_obj(a) && ic_put(t, ici, jv_obj(a), b)) { sp--; TOP = b; break; }
            SYNC();
            b = POP();
            a = TOP;
            pkey k = atom_key(J, nm);
            CHECKI(put_prop(J, a, k, b, strict));
            TOP = b;
            if (jv_is_obj(a)) ic_fill_put(J, t, ici, jv_obj(a), k);
            break;
        }
        case OP_GET_ELEM: L_OP_GET_ELEM: {   // [obj, key] -> [value]
            SYNC();
            b = POP();
            a = TOP;
            v = get_elem(J, a, b);
            CHECK(v);
            TOP = v;
            break;
        }
        case OP_GET_ELEM_KEEP: L_OP_GET_ELEM_KEEP: {   // [obj, key] -> [value, obj]
            SYNC();
            b = sp[-1];
            a = sp[-2];
            v = get_elem(J, a, b);
            CHECK(v);
            sp[-2] = v;
            sp[-1] = a;
            break;
        }
        case OP_PUT_ELEM: L_OP_PUT_ELEM: {   // [obj, key, value] -> [value]
            SYNC();
            c = POP();
            b = POP();
            a = TOP;
            CHECKI(put_elem(J, a, b, c, strict));
            TOP = c;
            break;
        }
        case OP_DELETE_FIELD: L_OP_DELETE_FIELD: {
            SYNC();
            jv nm = consts[rd32(pc)];
            pc += 4;
            a = TOP;
            if (jv_is_nullish(a)) { throw_type(J, "Cannot convert undefined or null to object"); goto exception; }
            v = delete_prop(J, a, atom_key(J, nm), strict);
            CHECK(v);
            TOP = v;
            break;
        }
        case OP_DELETE_ELEM: L_OP_DELETE_ELEM: {
            SYNC();
            b = POP();
            a = TOP;
            if (jv_is_nullish(a)) { throw_type(J, "Cannot convert undefined or null to object"); goto exception; }
            pkey k = pkey_from_value(J, b);
            if (!k) goto exception;
            v = delete_prop(J, a, k, strict);
            CHECK(v);
            TOP = v;
            break;
        }
        case OP_TO_PROPKEY: L_OP_TO_PROPKEY: {
            SYNC();
            a = TOP;
            if (!jv_is_str(a) && !jv_is_sym(a) && !jv_is_int(a)) {
                pkey k = pkey_from_value(J, a);
                if (!k) goto exception;
                v = pkey_to_value(J, k);
                CHECK(v);
                TOP = v;
            }
            break;
        }

        // ---- super
        case OP_GET_SUPER: L_OP_GET_SUPER: {   // [key, home, this] -> [value]
            SYNC();
            c = POP(); b = POP(); a = TOP;
            v = super_get(J, a, b, c);
            CHECK(v);
            TOP = v;
            break;
        }
        case OP_GET_SUPER_KEEP: L_OP_GET_SUPER_KEEP: {   // [key, home, this] -> [value, this]
            SYNC();
            c = sp[-1]; b = sp[-2]; a = sp[-3];
            v = super_get(J, a, b, c);
            CHECK(v);
            sp -= 1;
            sp[-2] = v;
            sp[-1] = c;
            break;
        }
        case OP_PUT_SUPER: L_OP_PUT_SUPER: {   // [key, home, this, value] -> [value]
            SYNC();
            v = POP(); c = POP(); b = POP(); a = TOP;
            CHECKI(super_put(J, a, b, c, v, strict));
            TOP = v;
            break;
        }
        case OP_SUPER_CTOR: L_OP_SUPER_CTOR: {   // [func] -> [its prototype, which must be a constructor]
            SYNC();
            a = TOP;
            int err = 0;
            struct obj* p = jv_is_obj(a) ? obj_get_proto(J, jv_obj(a), &err) : 0;
            if (err) goto exception;
            if (!p || !(p->flags & OF_CONSTRUCTOR)) { throw_type(J, "Super constructor is not a constructor"); goto exception; }
            TOP = jv_from_obj(p);
            break;
        }

        // ---- private names
        case OP_GET_PRIVATE: L_OP_GET_PRIVATE: { SYNC(); b = POP(); a = TOP; v = private_get(J, a, b); CHECK(v); TOP = v; break; }
        case OP_PUT_PRIVATE: L_OP_PUT_PRIVATE: { SYNC(); c = POP(); b = POP(); a = TOP; CHECKI(private_put(J, a, b, c)); TOP = c; break; }
        case OP_DEFINE_PRIVATE: L_OP_DEFINE_PRIVATE: { SYNC(); c = POP(); b = POP(); a = POP(); CHECKI(private_define(J, a, b, c)); break; }
        case OP_PRIVATE_IN: L_OP_PRIVATE_IN: {   // [obj, name] -> [bool]
            SYNC();
            b = POP();
            a = TOP;
            if (!jv_is_obj(a)) { throw_type(J, "Cannot use 'in' operator to search for a private field in a non-object"); goto exception; }
            int i;
            TOP = jv_bool(private_find(jv_obj(a), pk_from_sym(jv_sym(b)), &i));
            break;
        }
        case OP_ADD_PRIVATE_METHOD: L_OP_ADD_PRIVATE_METHOD: {   // [obj, name, fn] -> []
            uint8_t kind = *pc++;
            SYNC();
            c = POP(); b = POP(); a = POP();
            CHECKI(private_add_method(J, a, b, c, kind));
            break;
        }
        case OP_PRIVATE_NAME: L_OP_PRIVATE_NAME: {
            jv nm = consts[rd32(pc)];
            pc += 4;
            SYNC();
            struct sym* s = (struct sym*)gc_alloc(J, GT_SYM, sizeof(struct sym));
            if (!s) goto exception;
            s->desc = nm;
            s->registered = 3;
            PUSH(jv_from_sym(s));
            break;
        }

        // ---- arithmetic
        case OP_ADD: L_OP_ADD:
            a = sp[-2]; b = sp[-1];
            if (jv_is_int(a) && jv_is_int(b)) {
                int64_t r = (int64_t)jv_int(a) + jv_int(b);
                sp--;
                TOP = (r >= -2147483647 - 1 && r <= 2147483647) ? jv_from_int((int32_t)r) : jv_from_dbl((double)r);
                break;
            }
            if (jv_is_number(a) && jv_is_number(b)) { sp--; TOP = jv_number(jv_num(a) + jv_num(b)); break; }
            SYNC();
            v = op_binary(J, OP_ADD, a, b);
            CHECK(v);
            sp--;
            TOP = v;
            break;
        case OP_SUB: L_OP_SUB:
            a = sp[-2]; b = sp[-1];
            if (jv_is_int(a) && jv_is_int(b)) {
                int64_t r = (int64_t)jv_int(a) - jv_int(b);
                sp--;
                TOP = (r >= -2147483647 - 1 && r <= 2147483647) ? jv_from_int((int32_t)r) : jv_from_dbl((double)r);
                break;
            }
            if (jv_is_number(a) && jv_is_number(b)) { sp--; TOP = jv_number(jv_num(a) - jv_num(b)); break; }
            goto binop;
        case OP_MUL: L_OP_MUL:
            a = sp[-2]; b = sp[-1];
            if (jv_is_int(a) && jv_is_int(b)) {
                int64_t r = (int64_t)jv_int(a) * jv_int(b);
                // an int32 result, except 0 from a negative operand (-0)
                if (r >= -2147483647 - 1 && r <= 2147483647 && (r != 0 || (jv_int(a) >= 0 && jv_int(b) >= 0))) {
                    sp--;
                    TOP = jv_from_int((int32_t)r);
                    break;
                }
            }
            if (jv_is_number(a) && jv_is_number(b)) { sp--; TOP = jv_number(jv_num(a) * jv_num(b)); break; }
            goto binop;
        case OP_DIV: L_OP_DIV:
            a = sp[-2]; b = sp[-1];
            if (jv_is_number(a) && jv_is_number(b)) { sp--; TOP = jv_number(jv_num(a) / jv_num(b)); break; }
            goto binop;
        case OP_MOD: L_OP_MOD:
            a = sp[-2]; b = sp[-1];
            if (jv_is_int(a) && jv_is_int(b) && jv_int(a) >= 0 && jv_int(b) > 0) { sp--; TOP = jv_from_int(jv_int(a) % jv_int(b)); break; }
            goto binop;
        case OP_BAND: L_OP_BAND: case OP_BOR: L_OP_BOR: case OP_BXOR: L_OP_BXOR: case OP_SHL: L_OP_SHL: case OP_SAR: L_OP_SAR: case OP_SHR: L_OP_SHR:
            a = sp[-2]; b = sp[-1];
            if (jv_is_int(a) && jv_is_int(b)) {
                int32_t x = jv_int(a), y = jv_int(b);
                sp--;
                switch (opc) {
                case OP_BAND: TOP = jv_from_int(x & y); break;
                case OP_BOR: TOP = jv_from_int(x | y); break;
                case OP_BXOR: TOP = jv_from_int(x ^ y); break;
                case OP_SHL: TOP = jv_from_int((int32_t)((uint32_t)x << (y & 31))); break;
                case OP_SAR: TOP = jv_from_int(x >> (y & 31)); break;
                default: TOP = jv_number((double)((uint32_t)x >> (y & 31))); break;
                }
                break;
            }
            if (jv_is_number(a) && jv_is_number(b)) {   // doubles: ToInt32 without the generic path
                int32_t x = jv_is_int(a) ? jv_int(a) : dtoi32(jv_num(a)), y = jv_is_int(b) ? jv_int(b) : dtoi32(jv_num(b));
                sp--;
                switch (opc) {
                case OP_BAND: TOP = jv_from_int(x & y); break;
                case OP_BOR: TOP = jv_from_int(x | y); break;
                case OP_BXOR: TOP = jv_from_int(x ^ y); break;
                case OP_SHL: TOP = jv_from_int((int32_t)((uint32_t)x << (y & 31))); break;
                case OP_SAR: TOP = jv_from_int(x >> (y & 31)); break;
                default: TOP = jv_number((double)((uint32_t)x >> (y & 31))); break;
                }
                break;
            }
            goto binop;
        case OP_POW: L_OP_POW:
        binop:
            SYNC();
            a = sp[-2]; b = sp[-1];
            v = op_binary(J, opc, a, b);
            CHECK(v);
            sp--;
            TOP = v;
            break;
        case OP_NEG: L_OP_NEG:
            a = TOP;
            if (jv_is_int(a) && jv_int(a) != 0 && jv_int(a) != (int32_t)0x80000000) { TOP = jv_from_int(-jv_int(a)); break; }
            if (jv_is_number(a)) { TOP = jv_from_dbl(-jv_num(a)); break; }
            goto unop;
        case OP_INC: L_OP_INC:
            a = TOP;
            if (jv_is_int(a) && jv_int(a) != 0x7FFFFFFF) { TOP = jv_from_int(jv_int(a) + 1); break; }
            if (jv_is_number(a)) { TOP = jv_number(jv_num(a) + 1); break; }
            goto unop;
        case OP_DEC: L_OP_DEC:
            a = TOP;
            if (jv_is_int(a) && jv_int(a) != (int32_t)0x80000000) { TOP = jv_from_int(jv_int(a) - 1); break; }
            if (jv_is_number(a)) { TOP = jv_number(jv_num(a) - 1); break; }
            goto unop;
        case OP_PLUS: L_OP_PLUS: case OP_BITNOT: L_OP_BITNOT:
        unop:
            SYNC();
            if (opc == OP_PLUS) { v = to_number(J, TOP); CHECK(v); TOP = v; break; }
            v = op_unary(J, opc, TOP);
            CHECK(v);
            TOP = v;
            break;
        case OP_TO_NUMERIC: L_OP_TO_NUMERIC:
            if (jv_is_number(TOP)) break;
            SYNC();
            v = to_numeric(J, TOP);
            CHECK(v);
            TOP = v;
            break;
        case OP_NOT: L_OP_NOT: TOP = jv_bool(!to_boolean(TOP)); break;
        case OP_EQ: L_OP_EQ: case OP_NE: L_OP_NE: {
            a = sp[-2]; b = sp[-1];
            int r;
            if (jv_is_int(a) && jv_is_int(b)) r = a == b;
            else { SYNC(); r = loose_equals(J, a, b); if (r < 0) goto exception; }
            sp--;
            TOP = jv_bool(opc == OP_EQ ? r : !r);
            break;
        }
        case OP_SEQ: L_OP_SEQ: case OP_SNE: L_OP_SNE: {
            a = sp[-2]; b = sp[-1];
            int r = (jv_is_int(a) && jv_is_int(b)) ? a == b : strict_equals(J, a, b);
            sp--;
            TOP = jv_bool(opc == OP_SEQ ? r : !r);
            break;
        }
        case OP_LT: L_OP_LT: case OP_LE: L_OP_LE: case OP_GT: L_OP_GT: case OP_GE: L_OP_GE: {
            a = sp[-2]; b = sp[-1];
            int r;
            if (jv_is_int(a) && jv_is_int(b)) {
                int32_t x = jv_int(a), y = jv_int(b);
                r = opc == OP_LT ? x < y : opc == OP_LE ? x <= y : opc == OP_GT ? x > y : x >= y;
            } else if (jv_is_number(a) && jv_is_number(b)) {
                double x = jv_num(a), y = jv_num(b);
                r = opc == OP_LT ? x < y : opc == OP_LE ? x <= y : opc == OP_GT ? x > y : x >= y;
            } else {
                SYNC();
                r = op_relational(J, opc, a, b);
                if (r < 0) goto exception;
            }
            sp--;
            TOP = jv_bool(r);
            break;
        }
        case OP_IN: L_OP_IN: {   // [key, obj] -> [bool]
            SYNC();
            a = sp[-2]; b = sp[-1];
            if (!jv_is_obj(b)) { throw_type(J, "Cannot use 'in' operator to search for a key in a non-object"); goto exception; }
            pkey k = pkey_from_value(J, a);
            if (!k) goto exception;
            int r = obj_has(J, jv_obj(b), k);
            if (r < 0) goto exception;
            sp--;
            TOP = jv_bool(r);
            break;
        }
        case OP_INSTANCEOF: L_OP_INSTANCEOF: {
            SYNC();
            a = sp[-2]; b = sp[-1];
            int r = instance_of(J, a, b);
            if (r < 0) goto exception;
            sp--;
            TOP = jv_bool(r);
            break;
        }
        case OP_TYPEOF: L_OP_TYPEOF: TOP = typeof_value(J, TOP); break;
        case OP_IS_UNDEF: L_OP_IS_UNDEF: TOP = jv_bool(jv_is_undef(TOP)); break;
        case OP_IS_NULLISH: L_OP_IS_NULLISH: TOP = jv_bool(jv_is_nullish(TOP)); break;
        case OP_TO_STRING: L_OP_TO_STRING:
            if (jv_is_str(TOP)) break;
            SYNC();
            v = to_string(J, TOP);
            CHECK(v);
            TOP = v;
            break;
        case OP_TO_OBJECT: L_OP_TO_OBJECT:
            if (jv_is_obj(TOP)) break;
            SYNC();
            v = to_object(J, TOP);
            CHECK(v);
            TOP = v;
            break;
        case OP_TO_PRIMITIVE: L_OP_TO_PRIMITIVE:
            SYNC();
            v = to_primitive(J, TOP, 0);
            CHECK(v);
            TOP = v;
            break;

        // ---- jumps
        case OP_JMP: L_OP_JMP: {
            int32_t off = (int32_t)rd32(pc);
            pc += 4;
            if (off < 0 && --J->interrupt_counter == 0) { SYNC(); if (interrupt_check(J) < 0) goto exception; }
            JUMP(off);
            break;
        }
// comparison results are booleans: test them without a call
#define TRUTHY(x) ((x) == JV_TRUE || ((x) != JV_FALSE && (jv_is_int(x) ? jv_int(x) != 0 : to_boolean(x))))
        case OP_JF: L_OP_JF: { int32_t off = (int32_t)rd32(pc); pc += 4; a = POP(); if (!TRUTHY(a)) JUMP(off); break; }
        case OP_JT: L_OP_JT: {
            int32_t off = (int32_t)rd32(pc);
            pc += 4;
            a = POP();
            if (TRUTHY(a)) {
                if (off < 0 && --J->interrupt_counter == 0) { SYNC(); if (interrupt_check(J) < 0) goto exception; }
                JUMP(off);
            }
            break;
        }
        case OP_JF_KEEP: L_OP_JF_KEEP: { int32_t off = (int32_t)rd32(pc); pc += 4; a = TOP; if (!TRUTHY(a)) JUMP(off); break; }
        case OP_JT_KEEP: L_OP_JT_KEEP: { int32_t off = (int32_t)rd32(pc); pc += 4; a = TOP; if (TRUTHY(a)) JUMP(off); break; }
        case OP_JNULLISH: L_OP_JNULLISH: { int32_t off = (int32_t)rd32(pc); pc += 4; if (jv_is_nullish(POP())) JUMP(off); break; }
        case OP_JNOTNULLISH_KEEP: L_OP_JNOTNULLISH_KEEP: { int32_t off = (int32_t)rd32(pc); pc += 4; if (!jv_is_nullish(TOP)) JUMP(off); break; }
        case OP_JUNDEF: L_OP_JUNDEF: { int32_t off = (int32_t)rd32(pc); pc += 4; if (jv_is_undef(POP())) JUMP(off); break; }

        // ---- calls
        case OP_CALL: L_OP_CALL: case OP_CALL_METHOD: L_OP_CALL_METHOD: {
            uint16_t argc = rd16(pc);
            pc += 2;
            SYNC();
            if (--J->interrupt_counter == 0 && interrupt_check(J) < 0) goto exception;
            jv* args = sp - argc;
            a = args[-2];   // function
            b = args[-1];   // this
            if (jv_is_obj(a) && obj_class(jv_obj(a)) == OC_FUNCTION && !(((struct func*)jv_obj(a))->t->flags & TF_NOT_PLAIN))
                v = call_plain(J, (struct func*)jv_obj(a), b, argc, args);
            else if (!is_callable(a)) {
                throw_not_callable(J, a, 0);
                goto exception;
            } else v = vm_call(J, a, b, argc, args, JV_UNDEFINED);
            CHECK(v);
            sp = args - 2;
            PUSH(v);
            break;
        }
        case OP_NEW: L_OP_NEW: {
            uint16_t argc = rd16(pc);
            pc += 2;
            SYNC();
            jv* args = sp - argc;
            a = args[-2];   // constructor
            b = args[-1];   // new.target
            if (!is_constructor(a)) { throw_not_callable(J, a, 1); goto exception; }
            v = vm_call(J, a, JV_UNDEFINED, argc, args, b);
            CHECK(v);
            sp = args - 2;
            PUSH(v);
            break;
        }
        case OP_CALL_SPREAD: L_OP_CALL_SPREAD: case OP_CALL_METHOD_SPREAD: L_OP_CALL_METHOD_SPREAD: case OP_NEW_SPREAD: L_OP_NEW_SPREAD: {   // [f, this|nt, array]
            SYNC();
            c = sp[-1]; b = sp[-2]; a = sp[-3];
            struct obj* arr = jv_obj(c);
            uint32_t n = arr->elen;
            for (uint32_t i = 0; i < n; i++) if (arr->elems[i] == JV_HOLE) arr->elems[i] = JV_UNDEFINED;
            if (opc == OP_NEW_SPREAD) {
                if (!is_constructor(a)) { throw_not_callable(J, a, 1); goto exception; }
                v = vm_call(J, a, JV_UNDEFINED, (int)n, arr->elems, b);
            } else {
                if (!is_callable(a)) { throw_not_callable(J, a, 0); goto exception; }
                v = vm_call(J, a, b, (int)n, arr->elems, JV_UNDEFINED);
            }
            CHECK(v);
            sp -= 3;
            PUSH(v);
            break;
        }
        case OP_SUPER_CALL: L_OP_SUPER_CALL: {   // [parent, newtarget, args...] -> [this]
            uint16_t argc = rd16(pc);
            pc += 2;
            SYNC();
            jv* args = sp - argc;
            a = args[-2];
            b = args[-1];
            if (jv_is_undef(b)) { throw_ref(J, "'super' keyword unexpected here"); goto exception; }
            v = vm_call(J, a, JV_UNDEFINED, argc, args, b);
            CHECK(v);
            sp = args - 2;
            PUSH(v);
            break;
        }
        case OP_SUPER_CALL_SPREAD: L_OP_SUPER_CALL_SPREAD: {   // [parent, newtarget, array] -> [this]
            SYNC();
            c = sp[-1]; b = sp[-2]; a = sp[-3];
            struct obj* arr = jv_obj(c);
            for (uint32_t i = 0; i < arr->elen; i++) if (arr->elems[i] == JV_HOLE) arr->elems[i] = JV_UNDEFINED;
            v = vm_call(J, a, JV_UNDEFINED, (int)arr->elen, arr->elems, b);
            CHECK(v);
            sp -= 3;
            PUSH(v);
            break;
        }
        case OP_EVAL: L_OP_EVAL: case OP_EVAL_SPREAD: L_OP_EVAL_SPREAD: {
            uint16_t argc = 0;
            uint32_t envk;
            if (opc == OP_EVAL) { argc = rd16(pc); envk = rd32(pc + 2); pc += 6; }
            else { envk = rd32(pc); pc += 4; }
            SYNC();
            jv* args;
            int n;
            if (opc == OP_EVAL) { args = sp - argc; n = argc; }
            else {
                struct obj* arr = jv_obj(sp[-1]);
                for (uint32_t i = 0; i < arr->elen; i++) if (arr->elems[i] == JV_HOLE) arr->elems[i] = JV_UNDEFINED;
                args = arr->elems;
                n = (int)arr->elen;
            }
            jv* base = opc == OP_EVAL ? sp - argc : sp - 1;
            a = base[-2];
            b = base[-1];
            if (jv_is_obj(a) && jv_obj(a) == J->I.eval_fn) {
                v = direct_eval(J, f, n ? args[0] : JV_UNDEFINED, consts[envk]);
            } else {
                if (!is_callable(a)) { throw_not_callable(J, a, 0); goto exception; }
                v = vm_call(J, a, b, n, args, JV_UNDEFINED);
            }
            CHECK(v);
            sp = base - 2;
            PUSH(v);
            break;
        }
        case OP_CALL_IGNORED_REF_ERROR: L_OP_CALL_IGNORED_REF_ERROR:
            SYNC();
            throw_ref(J, "Invalid left-hand side in assignment");
            goto exception;

        // ---- returns
        case OP_RETURN: L_OP_RETURN: result = POP(); goto done;
        case OP_RETURN_UNDEF: L_OP_RETURN_UNDEF: result = JV_UNDEFINED; goto done;
        case OP_THROW: L_OP_THROW:
            SYNC();
            pending = POP();
            goto unwind;
        case OP_THROW_ERR: L_OP_THROW_ERR: {
            uint32_t k = rd32(pc);
            uint8_t kind = pc[4];
            pc += 5;
            SYNC();
            throw_error(J, kind, "%S", jv_str(consts[k]));
            goto exception;
        }
        case OP_TDZ_ERROR: L_OP_TDZ_ERROR: { jv nm = consts[rd32(pc)]; pc += 4; SYNC(); tdz_error(J, nm); goto exception; }
        case OP_CONST_ERROR: L_OP_CONST_ERROR: {
            jv nm = consts[rd32(pc)];
            pc += 4;
            SYNC();
            throw_type(J, "Assignment to constant variable '%S'", str_flat(J, nm));
            goto exception;
        }

        // ---- literals
        case OP_OBJECT: L_OP_OBJECT: {
            SYNC();
            struct obj* o = obj_new_plain(J);
            if (!o) goto exception;
            PUSH(jv_from_obj(o));
            break;
        }
        case OP_ARRAY: L_OP_ARRAY: {
            uint16_t n = rd16(pc);
            pc += 2;
            SYNC();
            v = array_from_stack(J, sp - n, n);
            CHECK(v);
            sp -= n;
            PUSH(v);
            break;
        }
        case OP_APPEND_ONE: L_OP_APPEND_ONE: {   // [arr, v] -> [arr]
            SYNC();
            b = POP();
            CHECKI(array_push_fast(J, jv_obj(TOP), b));
            break;
        }
        case OP_APPEND_HOLE: L_OP_APPEND_HOLE: {
            struct obj* arr = jv_obj(TOP);
            arr->alen++;
            break;
        }
        case OP_APPEND: L_OP_APPEND: {   // [arr, iterable] -> [arr]
            SYNC();
            b = POP();
            jv list = iter_to_list(J, b);
            CHECK(list);
            struct obj* src = jv_obj(list);
            struct obj* dst = jv_obj(TOP);
            for (uint32_t i = 0; i < src->elen; i++) CHECKI(array_push_fast(J, dst, src->elems[i]));
            break;
        }
        case OP_DEFINE_FIELD: L_OP_DEFINE_FIELD: {   // [obj, v] -> [obj]
            jv nm = consts[rd32(pc)];
            pc += 4;
            SYNC();
            b = POP();
            CHECKI(create_data_property_or_throw(J, jv_obj(TOP), atom_key(J, nm), b));
            break;
        }
        case OP_DEFINE_ELEM: L_OP_DEFINE_ELEM: {   // [obj, key, v] -> [obj]
            SYNC();
            c = POP();
            b = POP();
            pkey k = pkey_from_value(J, b);
            if (!k) goto exception;
            if (!jv_is_obj(TOP)) { throw_type(J, "Cannot define a field on a non-object"); goto exception; }
            CHECKI(create_data_property_or_throw(J, jv_obj(TOP), k, c));
            break;
        }
        case OP_DEFINE_METHOD: L_OP_DEFINE_METHOD: case OP_DEFINE_METHOD_ELEM: L_OP_DEFINE_METHOD_ELEM: {   // [obj, key, fn] -> [obj]
            uint8_t fl = *pc++;
            SYNC();
            c = POP();
            b = POP();
            CHECKI(define_method(J, jv_obj(TOP), b, c, fl));
            break;
        }
        case OP_SET_PROTO_LIT: L_OP_SET_PROTO_LIT: {   // [obj, proto] -> [obj]
            b = POP();
            if (jv_is_obj(b) || jv_is_null(b)) jv_obj(TOP)->proto = jv_is_null(b) ? 0 : jv_obj(b);
            break;
        }
        case OP_COPY_DATA_PROPS: L_OP_COPY_DATA_PROPS: {   // [target, source] -> [target]
            pc++;
            SYNC();
            b = POP();
            CHECKI(copy_data_properties(J, jv_obj(TOP), b, 0));
            break;
        }
        case OP_REST_OBJ: L_OP_REST_OBJ: {   // [source, keys] -> [rest]
            pc++;
            SYNC();
            b = POP();
            a = TOP;
            struct obj* r = obj_new_plain(J);
            if (!r) goto exception;
            CHECKI(copy_data_properties(J, r, a, jv_obj(b)));
            TOP = jv_from_obj(r);
            break;
        }
        case OP_SET_NAME: L_OP_SET_NAME: {   // [fn] -> [fn]
            jv nm = consts[rd32(pc)];
            pc += 4;
            SYNC();
            if (jv_is_obj(TOP)) CHECKI(set_fn_name(J, jv_obj(TOP), nm, 0));
            break;
        }
        case OP_SET_NAME_ELEM: L_OP_SET_NAME_ELEM: {   // [key, fn] -> [key, fn]
            pc++;
            SYNC();
            if (jv_is_obj(TOP)) CHECKI(set_fn_name(J, jv_obj(TOP), sp[-2], 0));
            break;
        }
        case OP_SET_HOME: L_OP_SET_HOME: {   // [obj, fn] -> [fn]
            b = POP();
            if (jv_is_obj(b) && obj_class(jv_obj(b)) == OC_FUNCTION) ((struct func*)jv_obj(b))->home = TOP;
            TOP = b;
            break;
        }
        case OP_CLOSURE: L_OP_CLOSURE: {
            jv tv = consts[rd32(pc)];
            pc += 4;
            SYNC();
            struct func* fn = closure_new(J, (struct ftempl*)JV_PTR(tv), f, 0);
            if (!fn) goto exception;
            if (fn->t->flags & (TF_ARROW | TF_METHOD | TF_FIELD_INIT | TF_STATIC_INIT)) {
                // arrows and methods start with the enclosing home object
                if (t->home_slot >= 0 && (fn->t->flags & TF_ARROW)) fn->home = locals[t->home_slot];
                else if (fn->t->flags & TF_ARROW) fn->home = f->fn ? f->fn->home : JV_UNDEFINED;
                if (fn->t->flags & TF_ARROW) fn->this_val = f->this_v;   // lexical this outside functions
            }
            PUSH(jv_from_obj(&fn->base));
            break;
        }
        case OP_CLASS: L_OP_CLASS: {   // [heritage?] -> [ctor, proto]
            jv tv = consts[rd32(pc)];
            uint8_t has_her = pc[4];
            pc += 5;
            SYNC();
            jv her = has_her ? TOP : JV_UNDEFINED;
            jv proto;
            v = make_class(J, f, (struct ftempl*)JV_PTR(tv), her, has_her, &proto);
            CHECK(v);
            if (has_her) sp--;
            PUSH(v);
            PUSH(proto);
            break;
        }
        case OP_CLASS_FIELDS: L_OP_CLASS_FIELDS: {   // [ctor, proto, fn] -> [ctor, proto]
            b = POP();
            ((struct func*)jv_obj(sp[-2]))->fields = b;
            break;
        }
        case OP_RUN_FIELDS: L_OP_RUN_FIELDS: {   // [this, ctor] -> [this]
            SYNC();
            b = POP();
            CHECKI(run_fields(J, TOP, b));
            break;
        }

        // ---- iteration
        case OP_GET_ITER: L_OP_GET_ITER: case OP_GET_ASYNC_ITER: L_OP_GET_ASYNC_ITER:
            SYNC();
            v = iter_get(J, TOP, opc == OP_GET_ASYNC_ITER);
            CHECK(v);
            TOP = v;
            break;
        case OP_ITER_CALL: L_OP_ITER_CALL: {   // [rec] -> [rec, result]
            pc++;
            SYNC();
            struct iterrec* r = (struct iterrec*)jv_obj(TOP);
            v = ojs_call_v(J, r->next, r->iter, 0, 0);
            if (v == JV_EXC) { r->done = 1; goto exception; }
            PUSH(v);
            break;
        }
        case OP_ITER_CHECK_OBJ: L_OP_ITER_CHECK_OBJ:
            if (!jv_is_obj(TOP) && TOP != JV_HOLE) {
                SYNC();
                if (jv_is_obj(sp[-2]) && obj_class(jv_obj(sp[-2])) == OC_ITER) ((struct iterrec*)jv_obj(sp[-2]))->done = 1;
                throw_type(J, "Iterator result is not an object");
                goto exception;
            }
            break;
        case OP_ITER_STEP: L_OP_ITER_STEP: {   // [rec, result] -> done: [rec] + jump ; else [rec, value]
            int32_t off = (int32_t)rd32(pc);
            pc += 4;
            SYNC();
            struct iterrec* r = (struct iterrec*)jv_obj(sp[-2]);
            a = TOP;
            jv d = obj_get(J, jv_obj(a), A(done), a);
            if (d == JV_EXC) { r->done = 1; goto exception; }
            if (to_boolean(d)) { r->done = 1; sp--; JUMP(off); break; }
            v = obj_get(J, jv_obj(a), A(value), a);
            if (v == JV_EXC) { r->done = 1; goto exception; }
            TOP = v;
            break;
        }
        case OP_FOR_OF_STEP: L_OP_FOR_OF_STEP: {   // [rec] -> done: [rec] + jump ; else [rec, value]
            int32_t off = (int32_t)rd32(pc);
            pc += 4;
            SYNC();
            v = iter_step_value(J, (struct iterrec*)jv_obj(TOP));   // marks the record done on errors
            CHECK(v);
            if (v == JV_HOLE) { JUMP(off); break; }
            PUSH(v);
            break;
        }
        case OP_ITER_VALUE: L_OP_ITER_VALUE: {   // [rec] -> [rec, value]
            SYNC();
            struct iterrec* r = (struct iterrec*)jv_obj(TOP);
            v = iter_step_value(J, r);
            CHECK(v);
            PUSH(v == JV_HOLE ? JV_UNDEFINED : v);
            break;
        }
        case OP_ITER_CLOSE: L_OP_ITER_CLOSE: case OP_ITER_CLOSE_QUIET: L_OP_ITER_CLOSE_QUIET: {   // [rec] -> []
            SYNC();
            a = POP();
            struct iterrec* r = (struct iterrec*)jv_obj(a);
            f->sp = sp;
            if (opc == OP_ITER_CLOSE_QUIET) {
                jv saved = J->exc;
                int had = J->has_exc;
                iter_close(J, r, 1);
                J->exc = saved;
                J->has_exc = had;
            } else CHECKI(iter_close(J, r, 0));
            break;
        }
        case OP_ASYNC_ITER_CLOSE: L_OP_ASYNC_ITER_CLOSE: {   // [rec] -> [return() result (to await)] or [undefined]
            SYNC();
            struct iterrec* r = (struct iterrec*)jv_obj(TOP);
            // nothing to close: JV_HOLE makes the following AWAIT / ITER_CHECK_OBJ no-ops
            if (r->done) { TOP = JV_HOLE; break; }
            r->done = 1;
            jv m = get_method(J, r->iter, A(return_));
            CHECK(m);
            if (jv_is_undef(m)) { TOP = JV_HOLE; break; }
            v = ojs_call_v(J, m, r->iter, 0, 0);
            CHECK(v);
            TOP = v;
            break;
        }
        case OP_SPREAD_ARRAY: L_OP_SPREAD_ARRAY: {   // [rec] -> [array]
            SYNC();
            struct iterrec* r = (struct iterrec*)jv_obj(TOP);
            struct obj* arr = obj_new_array(J, 0);
            if (!arr) goto exception;
            TOP = jv_from_obj(arr);
            PUSH(jv_from_obj(&r->base));   // keep the record reachable
            for (;;) {
                v = iter_step_value(J, r);
                if (v == JV_EXC) goto exception;
                if (v == JV_HOLE) break;
                CHECKI(array_push_fast(J, arr, v));
            }
            sp--;
            break;
        }
        case OP_FOR_IN_START: L_OP_FOR_IN_START:
            SYNC();
            v = for_in_start(J, TOP);
            CHECK(v);
            TOP = v;
            break;
        case OP_FOR_IN_NEXT: L_OP_FOR_IN_NEXT: {   // [enum] -> [enum, key] or jump with [enum]
            int32_t off = (int32_t)rd32(pc);
            pc += 4;
            SYNC();
            v = for_in_next(J, TOP);
            CHECK(v);
            if (v == JV_HOLE) { JUMP(off); break; }
            PUSH(v);
            break;
        }

        // ---- misc
        case OP_REGEXP: L_OP_REGEXP: {
            jv p = consts[rd32(pc)], fl = consts[rd32(pc + 4)];
            pc += 8;
            SYNC();
            v = regexp_create(J, p, fl);
            CHECK(v);
            PUSH(v);
            break;
        }
        case OP_REST: L_OP_REST: {   // rest parameter from argument index
            uint16_t from = rd16(pc);
            pc += 2;
            SYNC();
            v = rest_array(J, f, from);
            CHECK(v);
            PUSH(v);
            break;
        }
        case OP_ARGUMENTS: L_OP_ARGUMENTS: {
            uint8_t mapped = *pc++;
            SYNC();
            v = make_arguments(J, f, mapped);
            CHECK(v);
            PUSH(v);
            break;
        }
        case OP_THIS_FUNC: L_OP_THIS_FUNC: PUSH(f->fn ? jv_from_obj(&f->fn->base) : JV_UNDEFINED); break;
        case OP_FRAME_THIS: L_OP_FRAME_THIS: PUSH(f->this_v); break;
        case OP_NEW_TARGET: L_OP_NEW_TARGET: PUSH(f->new_target); break;
        case OP_HOME_OBJECT: L_OP_HOME_OBJECT: PUSH(f->fn ? f->fn->home : JV_UNDEFINED); break;
        case OP_IMPORT_META: L_OP_IMPORT_META:
            SYNC();
            v = module_import_meta(J, f);
            CHECK(v);
            PUSH(v);
            break;
        case OP_IMPORT_CALL: L_OP_IMPORT_CALL: {
            uint8_t has_opts = *pc++;
            SYNC();
            jv opts = has_opts ? POP() : JV_UNDEFINED;
            v = module_dynamic_import(J, f, TOP, opts);
            CHECK(v);
            TOP = v;
            break;
        }

        // ---- with / eval vars
        case OP_WITH_GET: L_OP_WITH_GET: case OP_WITH_TYPEOF: L_OP_WITH_TYPEOF: case OP_WITH_CALL_THIS: L_OP_WITH_CALL_THIS: case OP_WITH_DELETE: L_OP_WITH_DELETE: {   // [obj] -> found: [value(,this)] + jump
            jv nm = consts[rd32(pc)];
            int32_t off = (int32_t)rd32(pc + 4);
            pc += 8;
            SYNC();
            a = POP();
            if (!jv_is_obj(a)) break;   // eval var object not created yet
            pkey k = atom_key(J, nm);
            int h = with_has(J, jv_obj(a), k);
            if (h < 0) goto exception;
            if (!h) break;
            if (opc == OP_WITH_DELETE) {
                int r = obj_delete(J, jv_obj(a), k, 0);
                if (r < 0) goto exception;
                PUSH(jv_bool(r));
                JUMP(off);
                break;
            }
            v = obj_get(J, jv_obj(a), k, a);
            CHECK(v);
            PUSH(v);
            if (opc == OP_WITH_CALL_THIS) PUSH((jv_obj(a)->flags & OF_EVALVARS) ? JV_UNDEFINED : a);
            JUMP(off);
            break;
        }
        case OP_WITH_PUT: L_OP_WITH_PUT: {   // [v, obj] -> [v] (+ jump when found)
            jv nm = consts[rd32(pc)];
            int32_t off = (int32_t)rd32(pc + 4);
            pc += 8;
            SYNC();
            a = POP();
            if (!jv_is_obj(a)) break;
            pkey k = atom_key(J, nm);
            int h = with_has(J, jv_obj(a), k);
            if (h < 0) goto exception;
            if (!h) break;
            int r = obj_set(J, jv_obj(a), k, TOP, a, strict);
            if (r < 0) goto exception;
            JUMP(off);
            break;
        }
        case OP_EVAL_VAR_DECL: L_OP_EVAL_VAR_DECL: {   // create a var in the caller's eval var object
            jv nm = consts[rd32(pc)];
            uint16_t ix = rd16(pc + 4);
            pc += 6;
            SYNC();
            jv* cell = upv[ix]->loc;
            if (!jv_is_obj(*cell)) {
                struct obj* o = obj_new(J, 0, OC_OBJECT, 0);
                if (!o) goto exception;
                o->flags |= OF_EVALVARS;
                *cell = jv_from_obj(o);
            }
            pkey k = atom_key(J, nm);
            struct obj* o = jv_obj(*cell);
            int h = obj_get_own(J, o, k, 0);
            if (h < 0) goto exception;
            if (!h && obj_define_value(J, o, k, JV_UNDEFINED, PA_DEFAULT) < 0) goto exception;
            break;
        }

        // ---- generators
        case OP_INITIAL_YIELD: L_OP_INITIAL_YIELD:
            SYNC();
            f->flags |= FRF_SUSPENDED | FRF_INITIAL;
            result = JV_UNDEFINED;
            goto suspend;
        case OP_YIELD: L_OP_YIELD: case OP_ASYNC_GEN_YIELD: L_OP_ASYNC_GEN_YIELD:
            result = POP();
            SYNC();
            f->flags |= FRF_SUSPENDED;
            goto suspend;
        case OP_AWAIT: L_OP_AWAIT:
            if (TOP == JV_HOLE) break;   // AsyncIteratorClose without a return method
            result = POP();
            SYNC();
            f->flags |= FRF_SUSPENDED | FRF_AWAITING;
            goto suspend;
        case OP_YIELD_STAR: L_OP_YIELD_STAR: {   // [rec, received] -> [value] when the delegate is done
            SYNC();
            b = POP();
            struct iterrec* r = (struct iterrec*)jv_obj(TOP);
            int mode = f->ystar_mode, phase = f->ystar_phase;
            int async = (t->flags & TF_ASYNC) != 0;
            f->ystar_mode = 0;
            f->ystar_phase = 0;
            jv inner;
            if (phase == 2) {   // awaited the value of a return without a delegate return method
                f->completion = b;
                pending = JV_GENRET;
                sp--;
                goto unwind;
            }
            if (phase == 3) {   // awaited the delegate's return() after a throw it cannot handle
                if (!jv_is_obj(b)) { throw_type(J, "Iterator result is not an object"); goto exception; }
                throw_type(J, "The iterator does not provide a 'throw' method");
                goto exception;
            }
            if (phase == 1) inner = b;
            else {
                if (mode == 0) {
                    inner = ojs_call_v(J, r->next, r->iter, 1, &b);
                    CHECK(inner);
                } else if (mode == 1) {
                    jv m = get_method(J, r->iter, A(throw_));
                    CHECK(m);
                    if (jv_is_undef(m)) {
                        // no throw method: close the delegate, then a TypeError
                        if (async) {
                            r->done = 1;
                            jv rm = get_method(J, r->iter, A(return_));
                            CHECK(rm);
                            if (!jv_is_undef(rm)) {
                                jv cr = ojs_call_v(J, rm, r->iter, 0, 0);
                                CHECK(cr);
                                f->ystar_phase = 3;
                                PUSH(JV_UNDEFINED);   // stack shape for the re-run: [rec, x]
                                sp--;
                                SYNC();
                                f->flags |= FRF_SUSPENDED | FRF_AWAITING | FRF_YSTAR;
                                result = cr;
                                goto suspend;
                            }
                        } else CHECKI(iter_close(J, r, 0));
                        throw_type(J, "The iterator does not provide a 'throw' method");
                        goto exception;
                    }
                    inner = ojs_call_v(J, m, r->iter, 1, &b);
                    CHECK(inner);
                } else {
                    jv m = get_method(J, r->iter, A(return_));
                    CHECK(m);
                    if (jv_is_undef(m)) {
                        if (async) {
                            f->ystar_phase = 2;
                            SYNC();
                            f->flags |= FRF_SUSPENDED | FRF_AWAITING | FRF_YSTAR;
                            result = b;
                            goto suspend;
                        }
                        f->completion = b;
                        pending = JV_GENRET;
                        sp--;
                        goto unwind;
                    }
                    inner = ojs_call_v(J, m, r->iter, 1, &b);
                    CHECK(inner);
                }
                if (async) {
                    // await the delegate's result, then come back here in phase 1
                    f->ystar_mode = mode;
                    f->ystar_phase = 1;
                    SYNC();
                    f->flags |= FRF_SUSPENDED | FRF_AWAITING | FRF_YSTAR;
                    result = inner;
                    goto suspend;
                }
            }
            if (!jv_is_obj(inner)) { throw_type(J, "Iterator result is not an object"); goto exception; }
            jv d = obj_get(J, jv_obj(inner), A(done), inner);
            CHECK(d);
            if (to_boolean(d)) {
                v = obj_get(J, jv_obj(inner), A(value), inner);
                CHECK(v);
                if (mode == 2) { f->completion = v; pending = JV_GENRET; sp--; goto unwind; }
                TOP = v;
                break;
            }
            SYNC();
            if (async) {
                // async generators yield the delegate's value (AsyncGeneratorYield)
                v = obj_get(J, jv_obj(inner), A(value), inner);
                CHECK(v);
                f->flags |= FRF_SUSPENDED | FRF_YSTAR;
                result = v;
                goto suspend;
            }
            // yield the inner result object as is; this instruction re-runs on resume
            f->flags |= FRF_SUSPENDED | FRF_YIELD_RAW | FRF_YSTAR;
            result = inner;
            goto suspend;
        }

        default: L_default:
            SYNC();
            throw_error(J, NE_TYPE, "internal: bad opcode %d", opc);
            goto exception;
        }
        continue;

    exception:
        if (!J->has_exc) throw_oom(J);
        pending = take_exc(J);
    unwind: {
            // pc - 1 lies inside the instruction that threw, whatever operands it has read
            uint32_t off = (uint32_t)(pc - 1 - code);
            int found = 0;
            if (!J->uncatchable) {
                for (uint32_t i = 0; i < t->nexc; i++) {
                    struct exc_entry* e = &t->exc[i];
                    if (off < e->start || off >= e->end) continue;
                    if (pending == JV_GENRET && e->kind == 0) continue;   // catch does not stop a generator return
                    sp = f->stack + e->sp;
                    PUSH(pending);
                    pc = code + e->handler;
                    found = 1;
                    break;
                }
            }
            if (found) continue;
            // leave the frame
            close_upvals(J, f, locals);
            if (pending == JV_GENRET) { result = f->completion; f->flags |= FRF_RETURNING; goto done_closed; }
            ojs_throw(J, pending);
            result = JV_EXC;
            J->frame = saved_frame;
            f->sp = sp;
            return JV_EXC;
        }
    }
done:
    close_upvals(J, f, locals);
done_closed:
    J->frame = saved_frame;
    f->sp = sp;
    f->flags &= ~FRF_SUSPENDED;
    return result;
suspend:
    // generator / async suspension: the frame keeps pc/sp; upvalues stay open
    J->frame = saved_frame;
    return result;
#undef PUSH
#undef POP
#undef TOP
#undef SYNC
#undef CHECK
#undef CHECKI
#undef JUMP
}
