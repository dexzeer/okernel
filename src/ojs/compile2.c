// compile2.c � patterns, statements, functions, classes (included by compile.c)

// ---------------------------------------------------------------- small helpers

// a class node owns its scope (inner name binding): declarations bind in the enclosing one
static struct scope* outer_scope(struct node* cls) {
    return cls->scope && cls->scope->kind == SC_CLASS ? cls->scope->parent : cls->scope;
}

static void throw_err(struct cfunc* c, int kind, const char* msg) {
    op(c, OP_THROW_ERR, 0);
    put32(c, atom_const(c, atom_cstr(c->J, msg)));
    put8(c, (uint32_t)kind);
}

// the frozen template object of a tagged template site: an array of cooked
// strings (undefined for invalid escapes) with a frozen `raw` array
static jv template_object(struct cfunc* c, struct node* n) {
    ojs* J = c->J;
    uint32_t count = 0;
    for (struct node* q = n->b; q; q = q->next) count++;
    struct obj* cooked = obj_new_array(J, 0);
    struct obj* raw = obj_new_array(J, 0);
    if (!cooked || !raw) return JV_EXC;
    uint32_t i = 0;
    for (struct node* q = n->b; q; q = q->next, i++) {
        if (obj_define_value(J, cooked, PK_FROM_INDEX(i), q->u.str ? jv_from_str(q->u.str) : JV_UNDEFINED, PA_ENUMERABLE) < 0) return JV_EXC;
        if (obj_define_value(J, raw, PK_FROM_INDEX(i), jv_from_str(q->str2), PA_ENUMERABLE) < 0) return JV_EXC;
    }
    cooked->alen = raw->alen = count;
    if (set_integrity(J, raw, 1) < 0) return JV_EXC;
    if (obj_define_value(J, cooked, A(raw), jv_from_obj(raw), 0) < 0) return JV_EXC;
    if (set_integrity(J, cooked, 1) < 0) return JV_EXC;
    return jv_from_obj(cooked);
}

// Direct eval environment: every binding visible at the call site, as
// (name, flags, location) triples. location >= 0: local slot of this
// frame; < 0: upvalue index -1-loc of this closure. Dynamic scopes (with
// objects, eval var objects) appear in order as entries with EF_DYNAMIC.
#define EF_TDZ      0x01
#define EF_CONST    0x02
#define EF_DYNAMIC  0x04   // a with object / eval var object (name unused)
#define EF_FNAME    0x08   // named function expression binding (silent assignment)
#define EF_STRICT   0x10   // (first entry) the call site is strict code
#define EF_VARSCOPE 0x20   // the eval var object of the function receiving sloppy vars
#define EF_THIS_TDZ 0x40
#define EF_PARAM    0x80   // a formal parameter
static jv eval_env_descriptor(struct cfunc* c, struct scope* s) {
    ojs* J = c->J;
    int cap = 64, n = 0;
    jv* v = valarr_new(J, (uint32_t)cap * 3);
    if (!v) return JV_EXC;
    struct str** seen = (struct str**)ojs_sys_malloc(sizeof(struct str*) * 4096);
    int nseen = 0;
    for (struct scope* x = s; x; x = x->parent) {
        if (x->kind == SC_SCRIPT || (x->kind == SC_EVAL && !x->parent)) continue;   // global: resolved dynamically
        int dynamic = x->kind == SC_WITH || x->has_eval == 2;
        for (struct decl* d = x->decls; d; d = d->next) {
            if (d->kind == D_WITH || d->kind == D_EVALVARS) continue;
            int dup = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == d->name) { dup = 1; break; }
            if (dup) continue;
            if (seen && nseen < 4096) seen[nseen++] = d->name;
            if (d->slot < 0 && !(d->flags & 0x40) && d->scope->kind != SC_MODULE) continue;   // not materialized
            int loc;
            if (d->flags & 0x40) loc = -1 - upval_index(c, d);          // nested eval: a captured cell
            else if (d->scope->kind == SC_MODULE) loc = -1 - upval_index(c, d);
            else if (d->scope->fn == c->fi && d->slot >= 0) {
                loc = d->slot;
                d->flags |= DF_CAPTURED;   // eval code may close over it: per-iteration copies and scope exits close the cell
            } else loc = -1 - upval_index(c, d);
            int fl = 0;
            if (is_tdz_kind(d->kind) || d->kind == D_IMPORT) fl |= EF_TDZ;
            if (d->kind == D_CONST || d->kind == D_CLASSNAME || d->kind == D_IMPORT) fl |= EF_CONST;
            if (d->kind == D_FUNCNAME) fl |= EF_FNAME;
            if ((d->kind == D_PARAM || d->kind == D_ARGUMENTS) && d->scope->fn == c->fi && c->in_params) fl |= EF_PARAM;
            if (n >= cap) {
                jv* nv = valarr_grow(J, v, (uint32_t)cap * 3, (uint32_t)cap * 6, JV_UNDEFINED);
                if (!nv) { ojs_sys_free(seen); return JV_EXC; }
                v = nv;
                cap *= 2;
            }
            v[n * 3] = jv_from_str(d->name);
            v[n * 3 + 1] = jv_from_int(fl);
            v[n * 3 + 2] = jv_from_int(loc);
            n++;
        }
        if (dynamic) {
            // the with object / eval vars binding of this scope
            struct decl* d = 0;
            for (struct decl* e = x->decls; e; e = e->next) if (e->kind == D_WITH || e->kind == D_EVALVARS) { d = e; break; }
            if (d) {
                if (n >= cap) {
                    jv* nv = valarr_grow(J, v, (uint32_t)cap * 3, (uint32_t)cap * 6, JV_UNDEFINED);
                    if (!nv) { ojs_sys_free(seen); return JV_EXC; }
                    v = nv;
                    cap *= 2;
                }
                int loc = (d->scope->fn == c->fi && d->slot >= 0) ? d->slot : -1 - upval_index(c, d);
                v[n * 3] = jv_from_str(d->name);
                v[n * 3 + 1] = jv_from_int(EF_DYNAMIC | (d->kind == D_EVALVARS ? EF_VARSCOPE : 0));
                v[n * 3 + 2] = jv_from_int(loc);
                n++;
            }
        }
    }
    ojs_sys_free(seen);
    // terminator with the call site's strictness and count
    struct obj* holder = obj_new(J, 0, OC_OBJECT, 0);   // internal (never exposed)
    if (!holder) return JV_EXC;
    holder->elems = v;
    holder->elen = (uint32_t)n * 3;
    holder->alen = (uint32_t)n;
    holder->pad = ((c->fi->flags & FI_STRICT) ? 1 : 0) | (c->in_params ? 2 : 0) |
                  (c->in_params && !(c->fi->flags & FI_ARROW) ? 4 : 0);   // parameters of a function with its own arguments
    return jv_from_obj(holder);
}

// ---------------------------------------------------------------- destructuring

static void default_value(struct cfunc* c, struct node* init, struct node* target);

// assign the value on top of the stack to a pattern / target (consumes it).
// kind: 0 assignment, D_VAR/D_LET/D_CONST/D_PARAM/D_CATCH binding initialization
static void comp_target_store(struct cfunc* c, struct node* t, int kind) {
    if (t->type == N_IDENT) {
        int init = kind == D_LET || kind == D_CONST || kind == D_CATCH || kind == D_PARAM || kind == D_CLASS;
        if (kind == D_VAR) init = 0;
        store_name(c, t->u.str, t->scope, t->pos, 0, init);
        return;
    }
    if (t->type == N_OBJECT_PAT || t->type == N_ARRAY_PAT) { comp_pattern_assign(c, t, kind); return; }
    if (t->type == N_ASSIGN_PAT) {
        default_value(c, t->b, t->a);
        comp_target_store(c, t->a, kind);
        return;
    }
    // member / call target (for-in/of heads): the value comes first, then
    // the reference; copy the value above the reference parts and store
    struct lval lv;
    lval_prepare(c, t, &lv);
    int parts = lval_parts(&lv);
    op8(c, OP_PICK, (uint32_t)parts, 1);   // [v, parts..., v]
    lval_put(c, &lv);                       // [v, v]
    op(c, OP_POP, -1);
    op(c, OP_POP, -1);
}

// read a default when the value on top is undefined (NamedEvaluation for
// anonymous functions bound to an identifier)
static void default_value(struct cfunc* c, struct node* init, struct node* target) {
    op(c, OP_DUP, 1);
    uint32_t skip = jump(c, OP_JUNDEF, -1);
    uint32_t over = jump(c, OP_JMP, 0);
    patch_here(c, skip);
    op(c, OP_POP, -1);
    if (target && target->type == N_IDENT) comp_expr_named(c, init, target->u.str);
    else comp_expr(c, init);
    patch_here(c, over);
}

// assign one pattern element whose value is produced by `get` (a callback
// emitting the value): reference targets are evaluated before the value
typedef void (*valfn)(struct cfunc* c, void* ctx);

static void assign_element(struct cfunc* c, struct node* el, int kind, valfn get, void* ctx) {
    struct node* tgt = el;
    struct node* init = 0;
    if (tgt->type == N_ASSIGN_PAT) { init = tgt->b; tgt = tgt->a; }
    if (tgt->type == N_MEMBER || tgt->type == N_CALL) {
        struct lval lv;
        lval_prepare(c, tgt, &lv);
        get(c, ctx);
        if (init) default_value(c, init, 0);
        lval_put(c, &lv);
        op(c, OP_POP, -1);
        return;
    }
    get(c, ctx);
    if (init) default_value(c, init, tgt);
    comp_target_store(c, tgt, kind);
}

struct objget { int src; int key; struct str* atom; };
static void objget_fn(struct cfunc* c, void* p) {
    struct objget* g = (struct objget*)p;
    op16(c, OP_GET_LOC, (uint32_t)g->src, 1);
    if (g->atom) get_field(c, g->atom);
    else { op16(c, OP_GET_LOC, (uint32_t)g->key, 1); op(c, OP_GET_ELEM, -1); }
}

struct arrget { int rec; };
static void arrget_fn(struct cfunc* c, void* p) {
    struct arrget* g = (struct arrget*)p;
    op16(c, OP_GET_LOC, (uint32_t)g->rec, 1);
    op(c, OP_ITER_VALUE, 1);   // [rec, value]
    op(c, OP_NIP, -1);         // [value]
}
static void arrrest_fn(struct cfunc* c, void* p) {
    struct arrget* g = (struct arrget*)p;
    op16(c, OP_GET_LOC, (uint32_t)g->rec, 1);
    op(c, OP_SPREAD_ARRAY, 0);   // [rec] -> [array] (drains the iterator)
}

static void comp_pattern_assign(struct cfunc* c, struct node* pat, int kind) {
    // value to destructure on top of the stack (consumed)
    if (pat->type == N_OBJECT_PAT) {
        op(c, OP_DUP, 1);
        op(c, OP_TO_OBJECT, 0);   // RequireObjectCoercible: TypeError for null / undefined
        op(c, OP_POP, -1);
        int src = new_local(c);
        op16(c, OP_PUT_LOC, (uint32_t)src, -1);
        int has_rest = 0;
        for (struct node* p = pat->a; p; p = p->next) if (p->op == PK_SPREAD) has_rest = 1;
        int keys = -1;
        if (has_rest) { keys = new_local(c); op16(c, OP_ARRAY, 0, 1); op16(c, OP_PUT_LOC, (uint32_t)keys, -1); }
        for (struct node* p = pat->a; p; p = p->next) {
            if (p->op == PK_SPREAD) {
                struct node* tgt = p->b;
                if (tgt->type == N_MEMBER || tgt->type == N_CALL) {
                    struct lval lv;
                    lval_prepare(c, tgt, &lv);
                    op16(c, OP_GET_LOC, (uint32_t)src, 1);
                    op16(c, OP_GET_LOC, (uint32_t)keys, 1);
                    op8(c, OP_REST_OBJ, 0, -1);
                    lval_put(c, &lv);
                    op(c, OP_POP, -1);
                } else {
                    op16(c, OP_GET_LOC, (uint32_t)src, 1);
                    op16(c, OP_GET_LOC, (uint32_t)keys, 1);
                    op8(c, OP_REST_OBJ, 0, -1);
                    comp_target_store(c, tgt, kind);
                }
                continue;
            }
            struct objget g = { src, -1, 0 };
            if (p->flags & NF_COMPUTED) {
                g.key = new_local(c);
                prop_key(c, p->a, 1);
                op16(c, OP_PUT_LOC, (uint32_t)g.key, -1);
                if (has_rest) {
                    op16(c, OP_GET_LOC, (uint32_t)keys, 1);
                    op16(c, OP_GET_LOC, (uint32_t)g.key, 1);
                    op(c, OP_APPEND_ONE, -1);
                    op(c, OP_POP, -1);
                }
            } else {
                g.atom = prop_key(c, p->a, 0);
                if (has_rest) {
                    op16(c, OP_GET_LOC, (uint32_t)keys, 1);
                    push_str(c, g.atom);
                    op(c, OP_APPEND_ONE, -1);
                    op(c, OP_POP, -1);
                }
            }
            assign_element(c, p->b, kind, objget_fn, &g);
        }
        return;
    }
    // array pattern: iterator protocol, closing the iterator on abrupt exits
    op(c, OP_GET_ITER, 0);
    int rec = new_local(c);
    op16(c, OP_PUT_LOC, (uint32_t)rec, -1);
    struct cexc* prot = exc_begin(c, 1);
    struct arrget g = { rec };
    for (struct node* e = pat->a; e; e = e->next) {
        if (e->type == N_HOLE) {
            op16(c, OP_GET_LOC, (uint32_t)rec, 1);
            op(c, OP_ITER_VALUE, 1);
            op(c, OP_POP, -1);
            op(c, OP_POP, -1);
            continue;
        }
        if (e->type == N_REST) { assign_element(c, e->a, kind, arrrest_fn, &g); continue; }
        assign_element(c, e, kind, arrget_fn, &g);
    }
    exc_close(c, prot);
    uint32_t over = jump(c, OP_JMP, 0);
    prot->handler = here(c);
    c->sp = prot->sp + 1;                       // [exc]
    op16(c, OP_GET_LOC, (uint32_t)rec, 1);
    op(c, OP_ITER_CLOSE_QUIET, -1);             // errors from return() are ignored
    op(c, OP_THROW, -1);
    patch_here(c, over);
    c->sp = prot->sp;
    op16(c, OP_GET_LOC, (uint32_t)rec, 1);
    op(c, OP_ITER_CLOSE, -1);                   // closes it if not exhausted
}

// ---------------------------------------------------------------- scopes

// assign slots to the scope's declarations; lexical ones start in TDZ;
// block-level functions are created here (hoisted to the block start)
static void enter_scope(struct cfunc* c, struct scope* s, int hoist_functions) {
    // a scope compiled again (finally bodies: once per exit path) keeps its slots and
    // first_slot, but every copy must put its lexical bindings back into TDZ
    int again = s->entered;
    if (!again) s->first_slot = c->nlocals;
    s->entered = 1;
    struct scope* saved = c->cur_scope;
    c->cur_scope = s;
    for (struct decl* d = s->decls; d; d = d->next) {
        int lex = d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS;
        if (d->slot >= 0) {
            if (again && lex) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
            continue;
        }
        d->slot = new_local(c);
        if (lex) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
    }
    if (hoist_functions) {
        for (struct decl* d = s->decls; d; d = d->next) {
            if (d->kind != D_FUNC || !(d->flags & DF_LEX_FUNC)) continue;
            // the last declaration of a name wins (sloppy duplicates)
            struct decl* last = d;
            for (struct decl* e = d->next; e; e = e->next) if (e->name == d->name && e->kind == D_FUNC) last = e;
            if (last != d) continue;
            comp_function_node(c, d->node);
            op16(c, OP_PUT_LOC, (uint32_t)d->slot, -1);
        }
    }
    c->cur_scope = saved;
}

static int scope_has_captures(struct cfunc* c, struct scope* s) {
    (void)c;
    for (struct decl* d = s->decls; d; d = d->next) if (d->flags & DF_CAPTURED) return 1;
    return 0;
}

static void leave_scope(struct cfunc* c, struct scope* s) {
    if (scope_has_captures(c, s)) op16(c, OP_CLOSE, (uint32_t)s->first_slot, 0);
    // slots are not reused: frames stay small enough and debugging is simpler
}

// ---------------------------------------------------------------- control-flow blocks

static struct cblock* push_block(struct cfunc* c, int kind) {
    struct cblock* b = (struct cblock*)arena_alloc(c->P, sizeof *b);
    if (!b) { c->failed = 1; return 0; }
    b->kind = kind;
    b->prev = c->blocks;
    b->stack_depth = c->sp;
    b->close_slot = -1;
    c->blocks = b;
    return b;
}

static void pop_block(struct cfunc* c) { if (c->blocks) c->blocks = c->blocks->prev; }

// leave every block from the innermost up to (not including) `target`
// (target NULL: leave the function). Returns nothing; emits cleanup code.
// The protected ranges closed for inlined finally blocks are reopened by
// unwind_reopen after the jump.
static void comp_stmts_in(struct cfunc* c, struct node* list);
static void unwind(struct cfunc* c, struct cblock* target, int keep_top) {
    for (struct cblock* b = c->blocks; b && b != target; b = b->prev) {
        switch (b->kind) {
        case CB_FOR_OF:
            if (keep_top) { op(c, OP_SWAP, 0); }
            if (b->is_async_iter) {
                op(c, OP_ASYNC_ITER_CLOSE, 0);   // [rec] -> [promise] (returns()'s result)
                op(c, OP_AWAIT, 0);
                op(c, OP_ITER_CHECK_OBJ, 0);
                op(c, OP_POP, -1);
            } else op(c, OP_ITER_CLOSE, -1);
            break;
        case CB_FOR_IN:
            if (keep_top) op(c, OP_SWAP, 0);
            op(c, OP_POP, -1);
            break;
        case CB_SCOPE:
            if (b->close_slot >= 0) op16(c, OP_CLOSE, (uint32_t)b->close_slot, 0);
            break;
        case CB_TRY_FINALLY: {
            // run the finally block here, outside the try's protection
            exc_close(c, b->prot);
            exc_close(c, b->prot2);
            struct cblock* saved = c->blocks;
            c->blocks = b->prev;   // the finally body is outside this try
            int saved_sp = c->sp;
            comp_stmt(c, b->finally_node);   // a block: enters its scope
            c->sp = saved_sp;
            c->blocks = saved;
            break;
        }
        case CB_CATCH_PROT:
            exc_close(c, b->prot);
            break;
        default:
            break;
        }
    }
}

static void unwind_reopen(struct cfunc* c, struct cblock* target) {
    for (struct cblock* b = c->blocks; b && b != target; b = b->prev) {
        if (b->kind == CB_TRY_FINALLY) { exc_reopen(c, b->prot); exc_reopen(c, b->prot2); }
        if (b->kind == CB_CATCH_PROT) exc_reopen(c, b->prot);
    }
}

static int block_has_label(struct cblock* b, struct str* l) {
    if (b->label == l) return 1;
    for (int i = 0; i < b->nlabels; i++) if (b->labels[i] == l) return 1;
    return 0;
}

static void comp_break_continue(struct cfunc* c, struct node* n, int is_continue) {
    struct cblock* t = 0;
    for (struct cblock* b = c->blocks; b; b = b->prev) {
        if (is_continue) {
            if (b->kind != CB_LOOP) continue;
            if (!n->str2 || block_has_label(b, n->str2)) { t = b; break; }
        } else {
            if (n->str2) { if ((b->kind == CB_LABEL || b->kind == CB_LOOP || b->kind == CB_SWITCH) && block_has_label(b, n->str2)) { t = b; break; } }
            else if (b->kind == CB_LOOP || b->kind == CB_SWITCH) { t = b; break; }
        }
    }
    if (!t) { cerr(c, n->pos, "internal: no target for break/continue"); return; }
    int saved_sp = c->sp;
    // break leaves a for-in/of entirely: its iterator is closed and popped too
    struct cblock* stop = (!is_continue && t->iter) ? t->iter->prev : t;
    unwind(c, stop, 0);
    uint32_t j = jump(c, OP_JMP, 0);
    add_patch(c, is_continue ? &t->continues : &t->breaks, j);
    unwind_reopen(c, stop);
    c->sp = saved_sp;
}

static void set_completion(struct cfunc* c) {
    // value on top -> completion slot (eval / script); otherwise popped
    if (c->completion_slot >= 0) op16(c, OP_PUT_LOC, (uint32_t)c->completion_slot, -1);
    else op(c, OP_POP, -1);
}

static void reset_completion(struct cfunc* c) {
    if (c->completion_slot >= 0) { op(c, OP_UNDEF, 1); op16(c, OP_PUT_LOC, (uint32_t)c->completion_slot, -1); }
}

// ---------------------------------------------------------------- declarations

static void comp_var(struct cfunc* c, struct node* v) {
    for (struct node* d = v->a; d; d = d->next) {
        if (!d->b) {
            if (v->op == D_LET) {
                // let x;  -> initialized to undefined
                op(c, OP_UNDEF, 1);
                comp_target_store(c, d->a, D_LET);
            }
            continue;   // var x; does nothing
        }
        mark_pos(c, d->pos);
        if (d->a->type == N_IDENT) comp_expr_named(c, d->b, d->a->u.str);
        else comp_expr(c, d->b);
        comp_target_store(c, d->a, v->op);
    }
}

// ---------------------------------------------------------------- loops

// the labels directly applied to the statement being compiled
static struct str* pending_labels[32];
static int npending_labels;

struct lblset { struct str* l[32]; int n; };

static void grab_labels(struct lblset* ls) {
    ls->n = npending_labels;
    for (int i = 0; i < npending_labels; i++) ls->l[i] = pending_labels[i];
    npending_labels = 0;
}

static void apply_labels(struct cfunc* c, struct cblock* b, struct lblset* ls) {
    if (!ls->n) return;
    b->labels = (struct str**)arena_alloc(c->P, sizeof(struct str*) * (size_t)ls->n);
    if (!b->labels) return;
    memcpy(b->labels, ls->l, sizeof(struct str*) * (size_t)ls->n);
    b->nlabels = ls->n;
}

static void comp_while(struct cfunc* c, struct node* n) {
    struct lblset ls;
    grab_labels(&ls);
    reset_completion(c);
    struct cblock* b = push_block(c, CB_LOOP);
    apply_labels(c, b, &ls);
    uint32_t top = here(c);
    comp_expr(c, n->a);
    uint32_t exit = jump(c, OP_JF, -1);
    comp_stmt(c, n->b);
    patch_list(c, b->continues, top);
    jump_back(c, OP_JMP, top, 0);
    patch_here(c, exit);
    patch_list(c, b->breaks, here(c));
    pop_block(c);
}

static void comp_do_while(struct cfunc* c, struct node* n) {
    struct lblset ls;
    grab_labels(&ls);
    reset_completion(c);
    struct cblock* b = push_block(c, CB_LOOP);
    apply_labels(c, b, &ls);
    uint32_t top = here(c);
    comp_stmt(c, n->b);
    patch_list(c, b->continues, here(c));
    comp_expr(c, n->a);
    jump_back(c, OP_JT, top, -1);
    patch_list(c, b->breaks, here(c));
    pop_block(c);
}

static void comp_for(struct cfunc* c, struct node* n) {
    struct lblset ls;
    grab_labels(&ls);
    reset_completion(c);
    struct scope* hs = n->scope;   // head scope (let/const bindings)
    struct cblock* sb = push_block(c, CB_SCOPE);
    enter_scope(c, hs, 1);
    if (n->a) {
        if (n->a->type == N_VAR) comp_var(c, n->a);
        else comp_expr_void(c, n->a);
    }
    int per_iter = n->a && n->a->type == N_VAR && n->a->op != D_VAR;
    struct cblock* b = push_block(c, CB_LOOP);
    apply_labels(c, b, &ls);
    uint32_t top = here(c);
    uint32_t exit = 0;
    int has_test = n->b != 0;
    if (has_test) { comp_expr(c, n->b); exit = jump(c, OP_JF, -1); }
    comp_stmt(c, n->d);
    patch_list(c, b->continues, here(c));
    // a fresh copy of the loop bindings for the next iteration
    if (per_iter && scope_has_captures(c, hs)) op16(c, OP_CLOSE, (uint32_t)hs->first_slot, 0);
    if (n->c) comp_expr_void(c, n->c);
    jump_back(c, OP_JMP, top, 0);
    if (has_test) patch_here(c, exit);
    patch_list(c, b->breaks, here(c));
    pop_block(c);
    sb->close_slot = scope_has_captures(c, hs) ? hs->first_slot : -1;
    leave_scope(c, hs);
    pop_block(c);
}

// assign the value on top to the for-in/of head (declaration or target)
static void comp_for_head_store(struct cfunc* c, struct node* head) {
    if (head->type == N_VAR) {
        struct node* d = head->a;
        comp_target_store(c, d->a, head->op);
        return;
    }
    if (head->type == N_IDENT || head->type == N_OBJECT_PAT || head->type == N_ARRAY_PAT) {
        comp_target_store(c, head, 0);
        return;
    }
    comp_target_store(c, head, 0);
}

static void comp_for_in_of(struct cfunc* c, struct node* n) {
    int is_of = n->type == N_FOR_OF;
    int is_await = (n->flags & NF_AWAIT) != 0;
    struct lblset ls;
    grab_labels(&ls);
    reset_completion(c);
    struct scope* hs = n->scope;
    struct cblock* sb = push_block(c, CB_SCOPE);
    // the iterable is evaluated with the head bindings in TDZ
    enter_scope(c, hs, 0);
    // for (var x = init in o): Annex B initializer
    if (n->a->type == N_VAR && n->a->a->b) {
        struct node* d = n->a->a;
        comp_expr_named(c, d->b, d->a->type == N_IDENT ? d->a->u.str : 0);
        comp_target_store(c, d->a, D_VAR);
    }
    comp_expr(c, n->b);
    // the head bindings are fresh per iteration: reset TDZ for the body
    if (is_of) op(c, is_await ? OP_GET_ASYNC_ITER : OP_GET_ITER, 0);
    else op(c, OP_FOR_IN_START, 0);
    struct cblock* it = push_block(c, is_of ? CB_FOR_OF : CB_FOR_IN);
    it->is_async_iter = is_await;
    struct cblock* b = push_block(c, CB_LOOP);
    b->iter = it;
    apply_labels(c, b, &ls);
    uint32_t top = here(c);
    uint32_t exit;
    if (is_of && is_await) {
        op8(c, OP_ITER_CALL, 0, 1);    // [rec, promise]
        op(c, OP_AWAIT, 0);
        op(c, OP_ITER_CHECK_OBJ, 0);   // [rec, result] (TypeError if not an object)
        exit = jump(c, OP_ITER_STEP, 0);   // done: -> [rec]; else [rec, value]
    } else if (is_of) {
        exit = jump(c, OP_FOR_OF_STEP, 1);   // IteratorStepValue: [rec] -> [rec, value] or done
    } else {
        exit = jump(c, OP_FOR_IN_NEXT, 1);   // [enum, key] or done -> [enum]
    }
    // per-iteration bindings start uninitialized
    for (struct decl* d = hs->decls; d; d = d->next)
        if (d->kind == D_LET || d->kind == D_CONST) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
    // assignment / body under protection: abrupt completion closes the iterator
    struct cexc* prot = is_of ? exc_begin(c, 1) : 0;
    if (prot) prot->sp = (uint16_t)(c->sp - 1);   // handler sees [rec, exc]
    struct cblock* pb = 0;
    if (prot) { pb = push_block(c, CB_CATCH_PROT); pb->prot = prot; }
    comp_for_head_store(c, n->a);
    comp_stmt(c, n->d);
    if (pb) pop_block(c);
    if (prot) exc_close(c, prot);
    patch_list(c, b->continues, here(c));
    if (scope_has_captures(c, hs)) op16(c, OP_CLOSE, (uint32_t)hs->first_slot, 0);
    jump_back(c, OP_JMP, top, 0);
    // exhausted: [rec] (or [enum])
    patch_here(c, exit);
    c->sp = it->stack_depth;   // [rec] / [enum]
    op(c, OP_POP, -1);
    uint32_t after = jump(c, OP_JMP, 0);
    if (prot) {
        // throw from the body: close the iterator (errors from return() are ignored), rethrow
        prot->handler = here(c);
        c->sp = prot->sp + 1;   // [rec, exc]
        op(c, OP_SWAP, 0);
        if (is_await) {
            op(c, OP_ASYNC_ITER_CLOSE, 0);
            op(c, OP_AWAIT, 0);
            op(c, OP_POP, -1);
        } else op(c, OP_ITER_CLOSE_QUIET, -1);
        op(c, OP_THROW, -1);
    }
    // break targets: the iterator was already closed / popped by unwind()
    c->sp = it->stack_depth - 1;
    patch_list(c, b->breaks, here(c));
    patch_here(c, after);
    pop_block(c);   // loop
    pop_block(c);   // iterator
    sb->close_slot = scope_has_captures(c, hs) ? hs->first_slot : -1;
    leave_scope(c, hs);
    pop_block(c);
}

// ---------------------------------------------------------------- switch

static void comp_switch(struct cfunc* c, struct node* n) {
    struct lblset ls;
    grab_labels(&ls);
    reset_completion(c);
    comp_expr(c, n->a);
    int tmp = new_local(c);
    op16(c, OP_PUT_LOC, (uint32_t)tmp, -1);
    struct cblock* sb = push_block(c, CB_SCOPE);
    enter_scope(c, n->scope, 1);
    struct cblock* b = push_block(c, CB_SWITCH);
    apply_labels(c, b, &ls);
    int ncases = 0;
    for (struct node* k = n->b; k; k = k->next) ncases++;
    uint32_t* targets = (uint32_t*)ojs_sys_malloc(sizeof(uint32_t) * (size_t)(ncases ? ncases : 1));
    if (!targets) { c->failed = 1; return; }
    int i = 0;
    struct node* def = 0;
    int def_index = -1;
    for (struct node* k = n->b; k; k = k->next, i++) {
        if (!k->a) { def = k; def_index = i; continue; }
        op16(c, OP_GET_LOC, (uint32_t)tmp, 1);
        comp_expr(c, k->a);
        op(c, OP_SEQ, -1);
        targets[i] = jump(c, OP_JT, -1);
    }
    uint32_t to_default = jump(c, OP_JMP, 0);
    i = 0;
    for (struct node* k = n->b; k; k = k->next, i++) {
        if (k == def) patch_here(c, to_default);
        else patch_here(c, targets[i]);
        comp_stmts(c, k->b);
    }
    if (!def) patch_here(c, to_default);
    (void)def_index;
    ojs_sys_free(targets);
    patch_list(c, b->breaks, here(c));
    pop_block(c);
    sb->close_slot = scope_has_captures(c, n->scope) ? n->scope->first_slot : -1;
    leave_scope(c, n->scope);
    pop_block(c);
}

// ---------------------------------------------------------------- try

static void comp_try(struct cfunc* c, struct node* n) {
    reset_completion(c);
    struct node* handler = n->b;   // N_BLOCK: b = param, a = body
    struct node* fin = n->c;
    struct cblock* fb = 0;
    struct cexc* fexc = 0;
    if (fin) {
        fexc = exc_begin(c, 1);
        fb = push_block(c, CB_TRY_FINALLY);
        fb->finally_node = fin;
        fb->prot = fexc;
    }
    struct cexc* cexc = handler ? exc_begin(c, 0) : 0;
    struct cblock* cb = 0;
    if (cexc) { cb = push_block(c, CB_CATCH_PROT); cb->prot = cexc; }
    comp_stmt(c, n->a);
    if (cb) pop_block(c);
    exc_close(c, cexc);
    uint32_t after_try = 0;
    struct patch* to_finally_normal = 0;
    if (handler) {
        after_try = jump(c, OP_JMP, 0);
        cexc->handler = here(c);
        c->sp = cexc->sp + 1;   // [exc]
        struct scope* cs = handler->scope;
        struct cblock* csb = push_block(c, CB_SCOPE);
        enter_scope(c, cs, 0);
        if (handler->b) comp_target_store(c, handler->b, D_CATCH);
        else op(c, OP_POP, -1);
        struct node* body = handler->a;
        struct cblock* bsb = push_block(c, CB_SCOPE);
        enter_scope(c, body->scope, 1);
        comp_stmts(c, body->a);
        bsb->close_slot = scope_has_captures(c, body->scope) ? body->scope->first_slot : -1;
        leave_scope(c, body->scope);
        pop_block(c);
        csb->close_slot = scope_has_captures(c, cs) ? cs->first_slot : -1;
        leave_scope(c, cs);
        pop_block(c);
        patch_here(c, after_try);
    }
    (void)to_finally_normal;
    if (fin) {
        pop_block(c);   // the finally block itself is not protected by this try
        exc_close(c, fexc);
        // normal completion: run finally inline
        comp_stmt(c, fin);
        uint32_t done = jump(c, OP_JMP, 0);
        // abrupt completion (throw / generator return): save, run finally, rethrow
        fexc->handler = here(c);
        c->sp = fexc->sp + 1;
        int tmp = new_local(c);
        op16(c, OP_PUT_LOC, (uint32_t)tmp, -1);
        comp_stmt(c, fin);
        op16(c, OP_GET_LOC, (uint32_t)tmp, 1);
        op(c, OP_THROW, -1);
        patch_here(c, done);
        c->sp = fexc->sp;
    }
}

// ---------------------------------------------------------------- statements

static void comp_stmts_in(struct cfunc* c, struct node* list) {
    for (struct node* s = list; s && !c->failed; s = s->next) comp_stmt(c, s);
}

static void comp_stmts(struct cfunc* c, struct node* list) { comp_stmts_in(c, list); }

static void comp_block(struct cfunc* c, struct node* b) {
    struct cblock* sb = push_block(c, CB_SCOPE);
    enter_scope(c, b->scope, 1);
    comp_stmts(c, b->a);
    sb->close_slot = scope_has_captures(c, b->scope) ? b->scope->first_slot : -1;
    leave_scope(c, b->scope);
    pop_block(c);
}

static void comp_return(struct cfunc* c, struct node* n) {
    if (n->a) comp_expr(c, n->a); else op(c, OP_UNDEF, 1);
    if ((c->fi->flags & FI_ASYNC) && (c->fi->flags & FI_GENERATOR) && n->a) op(c, OP_AWAIT, 0);   // async generator return awaits
    // anything to unwind?
    int need = 0;
    for (struct cblock* b = c->blocks; b; b = b->prev)
        if (b->kind == CB_TRY_FINALLY || b->kind == CB_FOR_OF || b->kind == CB_FOR_IN) { need = 1; break; }
    if (!need) { op(c, OP_RETURN, -1); return; }
    int tmp = new_local(c);
    op16(c, OP_PUT_LOC, (uint32_t)tmp, -1);
    int saved_sp = c->sp;
    unwind(c, 0, 0);
    op16(c, OP_GET_LOC, (uint32_t)tmp, 1);
    op(c, OP_RETURN, -1);
    unwind_reopen(c, 0);
    c->sp = saved_sp;
}

static void comp_with(struct cfunc* c, struct node* n) {
    reset_completion(c);
    comp_expr(c, n->a);
    op(c, OP_TO_OBJECT, 0);
    struct scope* ws = n->scope;
    struct cblock* sb = push_block(c, CB_SCOPE);
    enter_scope(c, ws, 0);   // the hidden with-object binding
    struct decl* d = ws->decls;
    op16(c, OP_PUT_LOC, (uint32_t)d->slot, -1);
    comp_stmt(c, n->b);
    sb->close_slot = scope_has_captures(c, ws) ? ws->first_slot : -1;
    leave_scope(c, ws);
    pop_block(c);
}

static void comp_stmt(struct cfunc* c, struct node* n) {
    if (c->failed || !n) return;
    mark_pos(c, n->pos);
    switch (n->type) {
    case N_EXPR_STMT:
        if (c->completion_slot < 0) { comp_expr_void(c, n->a); return; }
        comp_expr(c, n->a);
        set_completion(c);
        return;
    case N_VAR: comp_var(c, n); return;
    case N_FUNC:
        // declarations were hoisted; Annex B block functions also copy to the var
        if ((n->flags & NF_ANNEXB) && n->u.fn->name) {
            struct funcinfo* fi = n->u.fn;
            struct scope* vs = c->fi->body_scope;
            struct decl* vd = scope_find(vs, fi->name);
            if (vd && vd->kind == D_VAR && (vd->flags & 0x80)) {
                load_name(c, fi->name, n->scope, n->pos, 0);
                store_name(c, fi->name, vs, n->pos, 0, 0);
            }
        }
        return;
    case N_CLASS:
        comp_class(c, n, 0);
        store_name(c, n->u.str ? n->u.str : c->J->A->star_default, outer_scope(n), n->pos, 0, 1);
        return;
    case N_BLOCK: comp_block(c, n); return;
    case N_EMPTY: case N_DEBUGGER: return;
    case N_IF: {
        reset_completion(c);
        comp_expr(c, n->a);
        uint32_t jf = jump(c, OP_JF, -1);
        comp_stmt(c, n->b);
        if (n->c) {
            uint32_t je = jump(c, OP_JMP, 0);
            patch_here(c, jf);
            comp_stmt(c, n->c);
            patch_here(c, je);
        } else patch_here(c, jf);
        return;
    }
    case N_WHILE: comp_while(c, n); return;
    case N_DO_WHILE: comp_do_while(c, n); return;
    case N_FOR: comp_for(c, n); return;
    case N_FOR_IN: case N_FOR_OF: comp_for_in_of(c, n); return;
    case N_CONTINUE: comp_break_continue(c, n, 1); return;
    case N_BREAK: comp_break_continue(c, n, 0); return;
    case N_RETURN: comp_return(c, n); return;
    case N_THROW: comp_expr(c, n->a); op(c, OP_THROW, -1); return;
    case N_TRY: comp_try(c, n); return;
    case N_SWITCH: comp_switch(c, n); return;
    case N_WITH: comp_with(c, n); return;
    case N_LABEL: {
        // a label applies to the statement it labels (loops: break + continue)
        struct node* s = n->a;
        if (npending_labels < 32) pending_labels[npending_labels++] = n->str2;
        if (s->type == N_FOR || s->type == N_FOR_IN || s->type == N_FOR_OF || s->type == N_WHILE ||
            s->type == N_DO_WHILE || s->type == N_LABEL || s->type == N_SWITCH) {
            comp_stmt(c, s);
            npending_labels = 0;
            return;
        }
        npending_labels = 0;
        struct cblock* b = push_block(c, CB_LABEL);
        b->label = n->str2;
        comp_stmt(c, s);
        patch_list(c, b->breaks, here(c));
        pop_block(c);
        return;
    }
    case N_IMPORT_DECL: return;   // handled at module instantiation
    case N_EXPORT:
        if (n->op == 4 || (n->op == 2 && n->a)) {   // export declaration / export default function|class
            struct node* d = n->a;
            if (d->type == N_CLASS) {
                comp_class(c, d, d->u.str ? 0 : c->J->A->default_);
                store_name(c, d->u.str ? d->u.str : c->J->A->star_default, outer_scope(d), n->pos, 0, 1);
            } else if (d->type == N_FUNC) {
                // hoisted at instantiation
            } else comp_stmt(c, d);
            return;
        }
        if (n->op == 2) {   // export default expression
            comp_expr_named(c, n->b, c->J->A->default_);
            store_name(c, c->J->A->star_default, n->scope, n->pos, 0, 1);
            return;
        }
        return;   // export { ... } / export * : link time
    default:
        cerr(c, n->pos, "internal: cannot compile statement");
        return;
    }
}

// ---------------------------------------------------------------- function templates

static struct ftempl* compile_func(ojs* J, struct parser* P, struct funcinfo* fi, struct cfunc* parent);

static void comp_function_node(struct cfunc* c, struct node* fn) {
    struct funcinfo* fi = fn->u.fn;
    struct ftempl* t = compile_func(c->J, c->P, fi, c);
    if (!t) { c->failed = 1; return; }
    op32(c, OP_CLOSURE, add_const(c, jv_from_ptr(t)), 1);
}

// Annex B.3.3: sloppy block functions that can also be vars of the function
static int annexb_ok(struct funcinfo* fi, struct node* fn) {
    struct str* name = fn->u.fn->name;
    struct scope* s = fn->scope;              // the block holding the declaration
    if (!s) return 0;
    for (struct scope* x = s->parent; x; x = x->parent) {
        struct decl* d = scope_find(x, name);
        if (d && (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS ||
                  (d->kind == D_FUNC && (d->flags & DF_LEX_FUNC)) || (d->kind == D_CATCH && d->node && d->node->type != N_IDENT)))
            return 0;
        if (x == fi->body_scope) break;
    }
    // not a parameter name
    struct decl* p = scope_find(fi->param_scope, name);
    if (p && p->kind == D_PARAM) return 0;
    if (fi->param_scope != fi->body_scope) {
        p = scope_find(fi->body_scope, name);
        if (p && (p->kind == D_LET || p->kind == D_CONST || p->kind == D_CLASS)) return 0;
    }
    return 1;
}

static void collect_annexb(struct cfunc* c, struct node* list, struct funcinfo* fi);

static void collect_annexb_stmt(struct cfunc* c, struct node* s, struct funcinfo* fi) {
    if (!s) return;
    switch (s->type) {
    case N_FUNC:
        if ((s->flags & NF_ANNEXB) && s->u.fn->name) {
            if (annexb_ok(fi, s)) {
                struct decl* d = scope_find(fi->body_scope, s->u.fn->name);
                if (!d) {
                    d = (struct decl*)arena_alloc(c->P, sizeof(struct decl));
                    if (!d) return;
                    d->name = s->u.fn->name;
                    d->kind = D_VAR;
                    d->scope = fi->body_scope;
                    d->slot = -1;
                    d->node = s;
                    if (fi->body_scope->decls_tail) fi->body_scope->decls_tail->next = d; else fi->body_scope->decls = d;
                    fi->body_scope->decls_tail = d;
                    fi->body_scope->ndecls++;
                    fi->body_scope->hash = 0;   // rebuilt lazily by scope_find users (linear meanwhile)
                }
                if (d->kind == D_VAR) d->flags |= 0x80;   // Annex B var
            } else s->flags &= ~NF_ANNEXB;
        }
        return;
    case N_BLOCK: collect_annexb(c, s->a, fi); return;
    case N_IF: collect_annexb_stmt(c, s->b, fi); collect_annexb_stmt(c, s->c, fi); return;
    case N_FOR: case N_FOR_IN: case N_FOR_OF: collect_annexb_stmt(c, s->d, fi); return;
    case N_WHILE: case N_DO_WHILE: collect_annexb_stmt(c, s->b, fi); return;
    case N_LABEL: collect_annexb_stmt(c, s->a, fi); return;
    case N_WITH: collect_annexb_stmt(c, s->b, fi); return;
    case N_TRY:
        collect_annexb_stmt(c, s->a, fi);
        if (s->b) collect_annexb_stmt(c, s->b->a, fi);
        collect_annexb_stmt(c, s->c, fi);
        return;
    case N_SWITCH:
        for (struct node* k = s->b; k; k = k->next) collect_annexb(c, k->b, fi);
        return;
    default: return;
    }
}

static void collect_annexb(struct cfunc* c, struct node* list, struct funcinfo* fi) {
    for (struct node* s = list; s; s = s->next) {
        if (s->type == N_FUNC && !(s->flags & NF_ANNEXB)) continue;   // top-level declaration
        collect_annexb_stmt(c, s, fi);
    }
}

// add a hidden pseudo binding (this, new.target, home object, callee) to a scope
static struct decl* pseudo_decl(struct cfunc* c, struct scope* s, const char* name, int kind) {
    struct str* nm = pseudo_name(c, name);
    struct decl* d = scope_find(s, nm);
    if (d) return d;
    d = (struct decl*)arena_alloc(c->P, sizeof(struct decl));
    if (!d) { c->failed = 1; return 0; }
    d->name = nm;
    d->kind = (uint8_t)kind;
    d->scope = s;
    d->slot = -1;
    if (s->decls_tail) s->decls_tail->next = d; else s->decls = d;
    s->decls_tail = d;
    s->ndecls++;
    s->hash = 0;
    return d;
}

// parameter list initialization for non-simple parameters
static void comp_params(struct cfunc* c, struct funcinfo* fi) {
    int i = 0;
    c->in_params++;
    for (struct node* p = fi->params; p; p = p->next, i++) {
        if (p->type == N_REST) {
            op16(c, OP_REST, (uint32_t)i, 1);
            comp_target_store(c, p->a, D_PARAM);
            continue;
        }
        op16(c, OP_GET_LOC, (uint32_t)i, 1);   // raw argument i
        comp_target_store(c, p, D_PARAM);
    }
    c->in_params--;
}

static int needs_this_slot(struct funcinfo* fi) {
    return !(fi->flags & FI_ARROW) && (fi->flags & (FI_USES_THIS | FI_DIRECT_EVAL | FI_DERIVED | FI_CLASS_CTOR | FI_FIELD_INIT | FI_STATIC_BLOCK));
}

static struct ftempl* finish_template(struct cfunc* c);
static void comp_body_and_finish(struct cfunc* c);

static struct ftempl* compile_func(ojs* J, struct parser* P, struct funcinfo* fi, struct cfunc* parent) {
    struct cfunc C;
    struct cfunc* c = &C;
    memset(c, 0, sizeof C);
    c->J = J;
    c->P = P;
    c->parent = parent;
    c->fi = fi;
    c->this_slot = c->newtarget_slot = c->home_slot = c->callee_slot = c->args_slot = c->completion_slot = -1;
    c->is_global_code = (fi->flags & FI_SCRIPT) != 0;
    c->is_module = (fi->flags & FI_MODULE) != 0;
    c->is_eval = (fi->flags & FI_EVAL) != 0;
    struct scope* ps = fi->param_scope;
    struct scope* bs = fi->body_scope;
    int simple = (fi->flags & FI_SIMPLE_PARAMS) != 0;
    // parameters occupy the first slots (raw arguments for non-simple lists)
    int nparams = fi->nparams;
    if (simple) {
        int i = 0;
        for (struct node* p = fi->params; p; p = p->next, i++) {
            struct decl* d = scope_find(ps, p->u.str);
            if (d) d->slot = i;   // duplicates: the last one wins
        }
        c->nlocals = c->maxlocals = nparams;
        // other declarations of the parameter scope (function name, pseudo vars)
    } else {
        c->nlocals = c->maxlocals = nparams;
    }
    c->nparam_slots = nparams;
    // pseudo bindings of non-arrow functions
    if (!(fi->flags & (FI_ARROW | FI_SCRIPT | FI_MODULE | FI_EVAL))) {
        if (needs_this_slot(fi)) {
            struct decl* d = pseudo_decl(c, ps, "this", D_THIS);
            if (d) { d->slot = new_local(c); c->this_slot = d->slot; }
        }
        if (fi->flags & (FI_USES_NEW_TARGET | FI_DIRECT_EVAL | FI_DERIVED)) {
            struct decl* d = pseudo_decl(c, ps, "new.target", D_NEWTARGET);
            if (d) { d->slot = new_local(c); c->newtarget_slot = d->slot; }
        }
        if (fi->flags & (FI_METHOD | FI_CLASS_CTOR)) {
            struct decl* d = pseudo_decl(c, ps, "*home*", D_HOME);
            if (d) { d->slot = new_local(c); c->home_slot = d->slot; }
        }
    }
    // arguments object
    struct decl* argd = 0;
    if ((fi->flags & FI_USES_ARGS) && !(fi->flags & (FI_ARROW | FI_SCRIPT | FI_MODULE | FI_EVAL)) &&
        !(fi->flags & (FI_FIELD_INIT | FI_STATIC_BLOCK))) {
        struct decl* shadow = scope_find(ps, J->A->arguments);
        int shadowed = shadow && shadow->kind == D_PARAM;
        struct decl* bd = bs != ps ? scope_find(bs, J->A->arguments) : 0;
        if (bd && (bd->kind == D_FUNC || bd->kind == D_LET || bd->kind == D_CONST || bd->kind == D_CLASS)) shadowed = 1;
        if (!bd && shadow && shadow->kind == D_FUNC) shadowed = 1;
        if (!shadowed) {
            argd = scope_find(ps, J->A->arguments);
            if (!argd) {
                argd = pseudo_decl(c, ps, "arguments", D_ARGUMENTS);
            }
            if (argd) { argd->slot = new_local(c); c->args_slot = argd->slot; }
        }
    }
    // remaining parameter-scope declarations (pattern names, function name)
    for (struct decl* d = ps->decls; d; d = d->next) {
        if (d->slot >= 0) continue;
        d->slot = new_local(c);
        if (d->kind == D_PARAM && !simple) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);   // TDZ until initialized
    }
    // named function expression: its own name
    for (struct decl* d = ps->decls; d; d = d->next) {
        if (d->kind == D_FUNCNAME) {
            op(c, OP_THIS_FUNC, 1);
            op16(c, OP_PUT_LOC, (uint32_t)d->slot, -1);
            c->callee_slot = d->slot;
        }
    }
    // global / eval / module code: completion value and global declarations
    if (c->is_global_code || c->is_eval) c->completion_slot = new_local(c);
    // arguments object before parameter initialization (defaults may use it)
    if (argd) {
        int mapped = simple && !(fi->flags & FI_STRICT);
        op8(c, OP_ARGUMENTS, (uint32_t)mapped, 1);
        op16(c, OP_PUT_LOC, (uint32_t)argd->slot, -1);
    }
    // eval var object (sloppy direct eval may add vars to this function); it
    // belongs to the parameter scope, so evals in parameter initializers see it too
    if ((fi->flags & FI_OWN_EVAL) && !(fi->flags & FI_STRICT) && !(fi->flags & (FI_SCRIPT | FI_EVAL))) {
        struct decl* ed = pseudo_decl(c, ps, "*evalvars*", D_EVALVARS);
        if (ed && ed->slot < 0) ed->slot = new_local(c);
        ps->has_eval = 2;
        // with parameter expressions the body has its own var environment: a body eval's
        // vars land there, out of sight of closures created in the parameter list
        if (!simple && bs != ps) {
            struct decl* bd = pseudo_decl(c, bs, "*evalvars*", D_EVALVARS);
            if (bd && bd->slot < 0) bd->slot = new_local(c);
            bs->has_eval = 2;
        }
    }
    if (!simple) comp_params(c, fi);
    // Annex B block functions that become vars
    if (!(fi->flags & FI_STRICT)) collect_annexb(c, fi->body ? fi->body : 0, fi);
    // body var scope
    if (bs != ps) {
        bs->first_slot = c->nlocals;
        for (struct decl* d = bs->decls; d; d = d->next) {
            if (d->slot >= 0) continue;
            if (c->is_global_code && (d->kind == D_VAR || d->kind == D_FUNC)) continue;   // global object
            if (c->is_global_code && (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS)) continue;   // global lexical env
            if (c->is_module) continue;   // module cells (assigned by compile_module)
            // simple parameter lists: `var a` / `function a` of a parameter's (or
            // arguments') name is that same binding
            if (simple && (d->kind == D_VAR || d->kind == D_FUNC) && !(d->flags & DF_LEX_FUNC)) {
                struct decl* pd = scope_find(ps, d->name);
                if (pd && (pd->kind == D_PARAM || pd->kind == D_ARGUMENTS) && pd->slot >= 0) { d->slot = pd->slot; continue; }
            }
            d->slot = new_local(c);
            if (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
            // non-simple params: a var with a parameter's name starts with its value
            if (d->kind == D_VAR && !simple) {
                struct decl* pd = scope_find(ps, d->name);
                if (pd && (pd->kind == D_PARAM || pd->kind == D_ARGUMENTS)) {
                    op16(c, OP_GET_LOC, (uint32_t)pd->slot, 1);
                    op16(c, OP_PUT_LOC, (uint32_t)d->slot, -1);
                }
            }
        }
    }
    // hoisted function declarations of the body
    for (struct decl* d = bs->decls; d; d = d->next) {
        if (d->kind != D_FUNC || (d->flags & DF_LEX_FUNC) || !d->node) continue;
        struct decl* last = d;
        for (struct decl* e = d->next; e; e = e->next) if (e->name == d->name && e->kind == D_FUNC && !(e->flags & DF_LEX_FUNC)) last = e;
        if (last != d) continue;
        if (c->is_global_code) continue;   // DECL_GLOBAL_FUNC in compile_script
        if (c->is_module) continue;        // module instantiation
        comp_function_node(c, d->node);
        op16(c, OP_PUT_LOC, (uint32_t)d->slot, -1);
    }
    // derived class constructors: the function itself (super() and field
    // initializers reach it from arrows through this binding)
    if (fi->flags & FI_CLASS_CTOR) {
        struct decl* fd = pseudo_decl(c, ps, "*func*", D_INTERNAL);
        if (fd) {
            fd->slot = new_local(c);
            op(c, OP_THIS_FUNC, 1);
            op16(c, OP_PUT_LOC, (uint32_t)fd->slot, -1);
        }
    }
    if (c->failed) return 0;
    comp_body_and_finish(c);
    return finish_template(c);
}

// ---------------------------------------------------------------- function body (continued from compile_func)

static void comp_body_and_finish(struct cfunc* c) {
    struct funcinfo* fi = c->fi;
    // generators: parameters are evaluated by the call; the body waits for next()
    if ((fi->flags & FI_GENERATOR) && !(fi->flags & (FI_MODULE))) op(c, OP_INITIAL_YIELD, 0);
    // base class constructors run field initializers right away
    if ((fi->flags & FI_CLASS_CTOR) && !(fi->flags & FI_DERIVED)) {
        load_pseudo(c, "this", 0);
        op(c, OP_THIS_FUNC, 1);
        op(c, OP_RUN_FIELDS, -1);
        op(c, OP_POP, -1);
    }
    struct cblock* sb = push_block(c, CB_SCOPE);
    (void)sb;
    if (fi->flags & FI_EXPR_BODY) {
        comp_expr(c, fi->body);
        if (fi->flags & FI_ASYNC) { /* async arrow: value resolves the promise */ }
        op(c, OP_RETURN, -1);
    } else {
        // block-level functions of the body scope are hoisted like the body's own
        comp_stmts(c, fi->body);
        if (c->completion_slot >= 0) {
            op16(c, OP_GET_LOC, (uint32_t)c->completion_slot, 1);
            op(c, OP_RETURN, -1);
        } else op(c, OP_RETURN_UNDEF, 0);
    }
    pop_block(c);
}

// ---------------------------------------------------------------- template output

static struct ftempl* finish_template(struct cfunc* c) {
    ojs* J = c->J;
    struct funcinfo* fi = c->fi;
    if (c->failed) return 0;
    struct ftempl* t = (struct ftempl*)gc_alloc(J, GT_FTEMPL, sizeof(struct ftempl));
    if (!t) return 0;
    t->code = (uint8_t*)bytes_new(J, c->len ? c->len : 1);
    t->consts = valarr_new(J, c->nconsts ? c->nconsts : 1);
    if (!t->code || !t->consts) return 0;
    memcpy(t->code, c->code, c->len);
    t->code_len = c->len;
    for (uint32_t i = 0; i < c->nconsts; i++) t->consts[i] = c->consts[i];
    t->nconsts = c->nconsts;
    t->nparams = (uint16_t)c->nparam_slots;
    t->length = (uint16_t)fi->nparams_len;
    t->nlocals = (uint16_t)c->maxlocals;
    t->stack_size = (uint16_t)(c->maxsp + 4);
    t->this_slot = (int16_t)c->this_slot;
    t->newtarget_slot = (int16_t)c->newtarget_slot;
    t->home_slot = (int16_t)c->home_slot;
    t->callee_slot = (int16_t)c->callee_slot;
    t->args_slot = (int16_t)c->args_slot;
    uint32_t fl = 0;
    if (fi->flags & FI_STRICT) fl |= TF_STRICT;
    if (fi->flags & FI_ARROW) fl |= TF_ARROW;
    if (fi->flags & FI_GENERATOR) fl |= TF_GENERATOR;
    if (fi->flags & FI_ASYNC) fl |= TF_ASYNC;
    if (fi->flags & FI_METHOD) fl |= TF_METHOD;
    if (fi->flags & FI_CLASS_CTOR) fl |= TF_CLASS_CTOR;
    if (fi->flags & FI_DERIVED) fl |= TF_DERIVED;
    if (fi->flags & FI_GETTER) fl |= TF_GETTER;
    if (fi->flags & FI_SETTER) fl |= TF_SETTER;
    if (fi->flags & FI_SCRIPT) fl |= TF_SCRIPT;
    if (fi->flags & FI_MODULE) fl |= TF_MODULE;
    if (fi->flags & FI_EVAL) fl |= TF_EVAL;
    if (fi->flags & FI_SIMPLE_PARAMS) fl |= TF_SIMPLE_PARAMS;
    if (fi->flags & FI_FIELD_INIT) fl |= TF_FIELD_INIT;
    if (fi->flags & FI_STATIC_BLOCK) fl |= TF_STATIC_INIT;
    if (fi->flags & FI_EXPR) fl |= TF_EXPR_NAME;
    // constructors: ordinary functions (not methods / arrows / generators / async) and classes
    if (((fi->flags & (FI_ARROW | FI_METHOD | FI_GENERATOR | FI_ASYNC | FI_GETTER | FI_SETTER | FI_FIELD_INIT | FI_STATIC_BLOCK)) == 0 &&
         !(fi->flags & (FI_SCRIPT | FI_MODULE | FI_EVAL))) || (fi->flags & FI_CLASS_CTOR))
        fl |= TF_CONSTRUCTOR;
    t->flags = fl;
    t->nupvals = (uint16_t)c->nupv;
    t->nic = c->nic;
    if (c->nupv) {
        t->upvals = (struct upval_desc*)bytes_new(J, sizeof(struct upval_desc) * (size_t)c->nupv);
        if (!t->upvals) return 0;
        for (int i = 0; i < c->nupv; i++) { t->upvals[i].from_local = c->upv[i].from_local; t->upvals[i].index = c->upv[i].index; }
    }
    // exception table: one entry per range (innermost entries first)
    uint32_t nexc = 0;
    for (struct cexc* e = c->excs; e; e = e->next) { exc_close(c, e); nexc += (uint32_t)e->nranges / 2; }
    if (nexc) {
        t->exc = (struct exc_entry*)bytes_new(J, sizeof(struct exc_entry) * nexc);
        if (!t->exc) return 0;
        uint32_t k = 0;
        // c->excs is newest first = innermost first for nested trys
        for (struct cexc* e = c->excs; e; e = e->next)
            for (int r = 0; r < e->nranges; r += 2) {
                t->exc[k].start = e->ranges[r];
                t->exc[k].end = e->ranges[r + 1];
                t->exc[k].handler = e->handler;
                t->exc[k].sp = e->sp;
                t->exc[k].kind = e->kind;
                k++;
            }
        t->nexc = nexc;
    }
    if (c->nlines) {
        // (pc, pos) pairs as deltas: a few bytes per entry instead of eight
        uint8_t* tmp = (uint8_t*)ojs_sys_malloc((size_t)c->nlines * 5);
        if (!tmp) { throw_oom(J); return 0; }
        uint32_t n = 0, ppc = 0, ppos = 0;
        for (int i = 0; i < c->nlines; i += 2) {
            uint32_t pc = c->lines[i], raw = c->lines[i + 1], pos = raw & ~POS_CALLEE;
            uint32_t a = ((pc - ppc) << 1) | ((raw & POS_CALLEE) ? 1 : 0);
            int32_t d = (int32_t)(pos - ppos);
            uint32_t b = ((uint32_t)d << 1) ^ (uint32_t)(d >> 31);
            while (a >= 0x80) { tmp[n++] = (uint8_t)(a | 0x80); a >>= 7; }
            tmp[n++] = (uint8_t)a;
            while (b >= 0x80) { tmp[n++] = (uint8_t)(b | 0x80); b >>= 7; }
            tmp[n++] = (uint8_t)b;
            ppc = pc;
            ppos = pos;
        }
        t->lines = (uint8_t*)bytes_new(J, n);
        if (t->lines) { memcpy(t->lines, tmp, n); t->nlines = n; }
        ojs_sys_free(tmp);
        if (!t->lines) return 0;
    }
    t->name = fi->name ? fi->name : J->A->empty;
    t->filename = c->P->filename ? atom_cstr(J, c->P->filename) : J->A->empty;
    t->source = c->P->L.src;
    t->src_start = fi->src_start;
    t->src_end = fi->src_end ? fi->src_end : fi->src_start;
    // mapped arguments: which parameter indices own their name's binding
    if (c->args_slot >= 0 && (fi->flags & FI_SIMPLE_PARAMS) && !(fi->flags & FI_STRICT) && fi->nparams > 0) {
        t->param_map = (uint8_t*)bytes_new(J, (size_t)fi->nparams);
        if (!t->param_map) return 0;
        int i = 0;
        for (struct node* p = fi->params; p; p = p->next, i++) {
            struct decl* d = scope_find(fi->param_scope, p->u.str);
            t->param_map[i] = d && d->slot == i && d->kind == D_PARAM;
        }
    }
    ojs_sys_free(c->code);
    ojs_sys_free(c->consts);
    ojs_sys_free(c->chash);
    ojs_sys_free(c->upv);
    ojs_sys_free(c->lines);
    fi->tmpl = t;
    return t;
}

// ---------------------------------------------------------------- classes

static struct str* priv_atom(struct cfunc* c, struct str* name) {
    struct sbuf b;
    sb_init(c->J, &b);
    sb_putc(&b, '#');
    sb_put_str(&b, name);
    jv s = sb_done(&b);
    if (s == JV_EXC) { c->failed = 1; return c->J->A->empty; }
    return atom_str(c->J, jv_str(s));
}

static int hidden_counter;

static struct decl* hidden_decl(struct cfunc* c, struct scope* s, const char* prefix) {
    char nm[32];
    ojs_snprintf(nm, sizeof nm, "*%s%d*", prefix, hidden_counter++);
    struct decl* d = pseudo_decl(c, s, nm, D_INTERNAL);
    if (d && d->slot < 0 && s->fn == c->fi) d->slot = new_local(c);
    return d;
}

static void store_decl(struct cfunc* c, struct decl* d, int keep) {
    struct ref r;
    memset(&r, 0, sizeof r);
    r.d = d;
    r.name = d->name;
    store_ref_static(c, &r, 0, keep, 0);
}

static void load_decl(struct cfunc* c, struct decl* d) {
    struct ref r;
    memset(&r, 0, sizeof r);
    r.d = d;
    r.name = d->name;
    load_ref_static(c, &r, 0, 0);
}

// the instance-field / static-element initializer function of a class
static struct ftempl* compile_class_init(struct cfunc* parent, struct node* cls, struct funcinfo* fi, int is_static,
                                         struct decl** keydecls, struct decl** methdecls) {
    ojs* J = parent->J;
    struct cfunc C;
    struct cfunc* c = &C;
    memset(c, 0, sizeof C);
    c->J = J;
    c->P = parent->P;
    c->parent = parent;
    c->fi = fi;
    c->this_slot = c->newtarget_slot = c->home_slot = c->callee_slot = c->args_slot = c->completion_slot = -1;
    struct decl* td = pseudo_decl(c, fi->param_scope, "this", D_THIS);
    if (td) { td->slot = new_local(c); c->this_slot = td->slot; }
    struct decl* hd = pseudo_decl(c, fi->param_scope, "*home*", D_HOME);
    if (hd) { hd->slot = new_local(c); c->home_slot = hd->slot; }
    enter_scope(c, fi->body_scope, 0);
    int i = 0;
    // instance private methods / accessors first (brand), then fields in order
    if (!is_static) {
        i = 0;
        for (struct node* m = cls->c; m; m = m->next, i++) {
            if ((m->flags & NF_STATIC) || !(m->flags & NF_PRIVATE) || m->op == PK_FIELD || m->type == N_CLASS_STATIC_BLOCK) continue;
            if (!methdecls[i]) continue;
            load_pseudo(c, "this", m->pos);
            load_name(c, priv_atom(c, m->a->u.str), cls->scope, m->pos, 0);
            load_decl(c, methdecls[i]);
            op8(c, OP_ADD_PRIVATE_METHOD, (uint32_t)(m->op == PK_GET ? 1 : m->op == PK_SET ? 2 : 0), -3);
        }
    }
    i = 0;
    for (struct node* m = cls->c; m; m = m->next, i++) {
        int st = (m->flags & NF_STATIC) != 0;
        if (st != is_static) continue;
        if (m->type == N_CLASS_STATIC_BLOCK) {
            // static { ... }: call the block function with this = the class
            comp_function_node(c, m->b);
            load_pseudo(c, "this", m->pos);
            op16(c, OP_CALL, 0, -1);
            op(c, OP_POP, -1);
            continue;
        }
        if (m->op != PK_FIELD) continue;
        mark_pos(c, m->pos);
        load_pseudo(c, "this", m->pos);
        if (m->flags & NF_PRIVATE) {
            struct str* pa = priv_atom(c, m->a->u.str);
            load_name(c, pa, cls->scope, m->pos, 0);
            if (m->b) comp_expr_named(c, m->b, pa); else op(c, OP_UNDEF, 1);
            op(c, OP_DEFINE_PRIVATE, -3);
            continue;
        }
        if (m->flags & NF_COMPUTED) {
            load_decl(c, keydecls[i]);
            if (m->b) {
                if (is_anon_fn(m->b)) { comp_expr(c, m->b); op8(c, OP_SET_NAME_ELEM, 0, 0); }
                else comp_expr(c, m->b);
            } else op(c, OP_UNDEF, 1);
            op(c, OP_DEFINE_ELEM, -2);
        } else {
            struct str* a = prop_key(c, m->a, 0);
            if (m->b) comp_expr_named(c, m->b, a); else op(c, OP_UNDEF, 1);
            // CreateDataPropertyOrThrow (not a [[Set]]): DEFINE_FIELD with the throwing flag
            op32(c, OP_DEFINE_FIELD, atom_const(c, a), -1);
        }
        op(c, OP_POP, -1);
    }
    leave_scope(c, fi->body_scope);
    op(c, OP_RETURN_UNDEF, 0);
    struct ftempl* t = finish_template(c);
    return t;
}
#include "compile3.c"
