// compile3.c — classes and the program entry points (included by compile2.c)


jv bigint_from_literal(ojs* J, struct str* text);   // bigint.c

static jv bigint_literal(ojs* J, struct str* text) { return bigint_from_literal(J, text); }

// ---------------------------------------------------------------- classes

static struct decl* scope_decl_atom(struct cfunc* c, struct scope* s, struct str* name, int kind) {
    struct decl* d = scope_find(s, name);
    if (d) return d;
    d = (struct decl*)arena_alloc(c->P, sizeof(struct decl));
    if (!d) { c->failed = 1; return 0; }
    d->name = name;
    d->kind = (uint8_t)kind;
    d->scope = s;
    d->slot = -1;
    if (s->decls_tail) s->decls_tail->next = d; else s->decls = d;
    s->decls_tail = d;
    s->ndecls++;
    s->hash = 0;
    return d;
}

static struct ftempl* default_ctor_template(struct cfunc* c, int derived, struct str* name, struct node* cls) {
    ojs* J = c->J;
    struct ftempl* t = (struct ftempl*)gc_alloc(J, GT_FTEMPL, sizeof(struct ftempl));
    if (!t) return 0;
    t->code = (uint8_t*)bytes_new(J, 1);
    t->consts = valarr_new(J, 1);
    if (!t->code || !t->consts) return 0;
    t->code[0] = OP_RETURN_UNDEF;
    t->code_len = 1;
    t->flags = TF_STRICT | TF_CLASS_CTOR | TF_CONSTRUCTOR | TF_DEFAULT_CTOR | (derived ? TF_DERIVED : 0);
    t->this_slot = t->newtarget_slot = t->home_slot = t->callee_slot = t->args_slot = -1;
    t->stack_size = 4;
    t->name = name ? name : J->A->empty;
    t->filename = c->P->filename ? atom_cstr(J, c->P->filename) : J->A->empty;
    t->source = c->P->L.src;
    t->src_start = cls->pos;
    t->src_end = cls->end;
    return t;
}

static void comp_class(struct cfunc* c, struct node* cls, struct str* inferred) {
    ojs* J = c->J;
    struct scope* cs = cls->scope;
    struct node* holder = cls->d;   // a: instance init fn, b: static init fn, c: constructor fn
    struct str* name = cls->u.str ? cls->u.str : inferred;
    int has_heritage = cls->b != 0;
    int nmembers = 0;
    for (struct node* m = cls->c; m; m = m->next) nmembers++;
    struct decl** keydecls = (struct decl**)arena_alloc(c->P, sizeof(struct decl*) * (size_t)(nmembers + 1));
    struct decl** methdecls = (struct decl**)arena_alloc(c->P, sizeof(struct decl*) * (size_t)(nmembers + 1));
    if (!keydecls || !methdecls) { c->failed = 1; return; }
    // private names are bindings "#x" of the class scope
    for (struct private_name* p = cls->privs; p; p = p->next) {
        struct decl* d = scope_decl_atom(c, cs, priv_atom(c, p->name), D_PRIVATE_BRAND);
        if (d) p->d = d;
    }
    // hidden bindings: computed field keys, private methods
    int i = 0;
    for (struct node* m = cls->c; m; m = m->next, i++) {
        if (m->type == N_CLASS_STATIC_BLOCK) continue;
        if (m->op == PK_FIELD && (m->flags & NF_COMPUTED)) keydecls[i] = hidden_decl(c, cs, "key");
        if (m->op != PK_FIELD && (m->flags & NF_PRIVATE)) methdecls[i] = hidden_decl(c, cs, "pm");
    }
    struct cblock* sb = push_block(c, CB_SCOPE);
    enter_scope(c, cs, 0);   // class name binding (TDZ), private names, hidden slots
    // a fresh private name (symbol) per class evaluation
    for (struct private_name* p = cls->privs; p; p = p->next) {
        if (!p->d) continue;
        op32(c, OP_PRIVATE_NAME, atom_const(c, priv_atom(c, p->name)), 1);
        store_decl(c, p->d, 0);
    }
    if (has_heritage) comp_expr(c, cls->b);
    struct ftempl* ct;
    if (holder->c) {
        holder->c->u.fn->name = name ? name : J->A->empty;
        ct = compile_func(J, c->P, holder->c->u.fn, c, c->lm);
    } else ct = default_ctor_template(c, has_heritage, name, cls);
    if (!ct) { c->failed = 1; return; }
    ct->name = name ? name : J->A->empty;
    ct->src_start = cls->pos;
    ct->src_end = cls->end;
    // CLASS: [heritage?] -> [ctor, proto]
    op(c, OP_CLASS, has_heritage ? 1 : 2);
    put32(c, add_const(c, jv_from_ptr(ct)));
    put8(c, (uint32_t)has_heritage);
    // methods and accessors, computed keys in order
    i = 0;
    int has_instance_init = 0, has_static_init = 0;
    for (struct node* m = cls->c; m; m = m->next, i++) {
        int st = (m->flags & NF_STATIC) != 0;
        if (m->type == N_CLASS_STATIC_BLOCK) { has_static_init = 1; continue; }
        if (m->flags & NF_SHORTHAND && m->op == PK_METHOD && holder->c && m->b == holder->c) continue;   // the constructor
        if (m->op == PK_FIELD) {
            if (st) has_static_init = 1; else has_instance_init = 1;
            if (m->flags & NF_COMPUTED) {
                prop_key(c, m->a, 1);
                store_decl(c, keydecls[i], 0);
            }
            continue;
        }
        int kind = m->op == PK_GET ? DM_GET : m->op == PK_SET ? DM_SET : DM_METHOD;
        if (m->flags & NF_PRIVATE) {
            // the closure (home = proto / ctor) kept in a hidden binding
            op8(c, OP_PICK, st ? 1 : 0, 1);          // home object
            comp_function_node(c, m->b);             // [.., home, fn]
            op(c, OP_SET_HOME, -1);                  // [.., fn]
            struct str* pn = priv_atom(c, m->a->u.str);
            if (kind == DM_GET || kind == DM_SET) {
                struct sbuf b;
                sb_init(J, &b);
                sb_puts(&b, kind == DM_GET ? "get " : "set ");
                sb_put_str(&b, pn);
                jv nm = sb_done(&b);
                if (nm == JV_EXC) { c->failed = 1; return; }
                op32(c, OP_SET_NAME, atom_const(c, atom_str(J, jv_str(nm))), 0);
            } else op32(c, OP_SET_NAME, atom_const(c, pn), 0);
            store_decl(c, methdecls[i], 0);
            if (st) {
                // static private methods live on the constructor itself
                op8(c, OP_PICK, 1, 1);
                load_name(c, pn, cs, m->pos, 0);
                load_decl(c, methdecls[i]);
                op8(c, OP_ADD_PRIVATE_METHOD, (uint32_t)(kind == DM_GET ? 1 : kind == DM_SET ? 2 : 0), -3);
            } else has_instance_init = 1;
            continue;
        }
        op8(c, OP_PICK, st ? 1 : 0, 1);              // target: ctor or proto
        struct str* a = prop_key(c, m->a, (m->flags & NF_COMPUTED) != 0);
        if (!(m->flags & NF_COMPUTED)) push_str(c, a);
        comp_function_node(c, m->b);
        op8(c, OP_DEFINE_METHOD, (uint32_t)kind, -2);   // class methods are not enumerable
        op(c, OP_POP, -1);
    }
    // the inner class name binding
    struct decl* nd = cls->u.str ? scope_find(cs, cls->u.str) : 0;
    if (nd && nd->kind == D_CLASSNAME) {
        op8(c, OP_PICK, 1, 1);
        struct ref r;
        memset(&r, 0, sizeof r);
        r.d = nd;
        r.name = nd->name;
        store_ref_static(c, &r, cls->pos, 0, 1);
    }
    // instance fields / private methods: an initializer the constructor runs
    if (has_instance_init) {
        struct ftempl* ft = compile_class_init(c, cls, holder->a->u.fn, 0, keydecls, methdecls);
        if (!ft) { c->failed = 1; return; }
        op8(c, OP_PICK, 0, 1);                        // home: proto
        op32(c, OP_CLOSURE, add_const(c, jv_from_ptr(ft)), 1);
        op(c, OP_SET_HOME, -1);
        op(c, OP_CLASS_FIELDS, -1);                   // [ctor, proto, fn] -> [ctor, proto]
    }
    // static fields and blocks, in order, with this = the class
    if (has_static_init) {
        struct ftempl* st = compile_class_init(c, cls, holder->b->u.fn, 1, keydecls, methdecls);
        if (!st) { c->failed = 1; return; }
        op8(c, OP_PICK, 1, 1);                        // home: ctor
        op32(c, OP_CLOSURE, add_const(c, jv_from_ptr(st)), 1);
        op(c, OP_SET_HOME, -1);                       // [ctor, proto, fn]
        op8(c, OP_PICK, 2, 1);                        // this = ctor
        op16(c, OP_CALL, 0, -1);
        op(c, OP_POP, -1);
    }
    op(c, OP_POP, -1);   // proto
    sb->close_slot = scope_has_captures(c, cs) ? cs->first_slot : -1;
    leave_scope(c, cs);
    pop_block(c);
}

// ---------------------------------------------------------------- program entry points

static void cfunc_init_top(struct cfunc* c, ojs* J, struct parser* P, struct funcinfo* fi) {
    memset(c, 0, sizeof *c);
    c->J = J;
    c->P = P;
    c->fi = fi;
    c->this_slot = c->newtarget_slot = c->home_slot = c->callee_slot = c->args_slot = c->completion_slot = -1;
}

// global declaration instantiation of a script (ECMA-262 §16.1.7)
static void global_decls(struct cfunc* c, struct scope* s, int is_eval) {
    ojs* J = c->J;
    // collect names: [nlex, nvar, nfn, lex..., var..., fn...]
    int nlex = 0, nvar = 0, nfn = 0;
    for (struct decl* d = s->decls; d; d = d->next) {
        if (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS) nlex++;
        else if (d->kind == D_FUNC && !(d->flags & DF_LEX_FUNC)) nfn++;
        else if (d->kind == D_VAR) nvar++;
    }
    struct obj* names = obj_new(J, 0, OC_OBJECT, 0);
    if (!names) { c->failed = 1; return; }
    jv* v = valarr_new(J, (uint32_t)(nlex + nvar + nfn + 4));
    if (!v) { c->failed = 1; return; }
    names->elems = v;
    v[0] = jv_from_int(nlex);
    v[1] = jv_from_int(nvar);
    v[2] = jv_from_int(nfn);
    v[3] = jv_from_int(is_eval);
    int k = 4;
    for (struct decl* d = s->decls; d; d = d->next) if (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS) v[k++] = jv_from_str(d->name);
    for (struct decl* d = s->decls; d; d = d->next) if (d->kind == D_VAR) v[k++] = jv_from_str(d->name);
    for (struct decl* d = s->decls; d; d = d->next) if (d->kind == D_FUNC && !(d->flags & DF_LEX_FUNC)) v[k++] = jv_from_str(d->name);
    names->elen = (uint32_t)k;
    op32(c, OP_CHECK_GLOBAL_DECLS, add_const(c, jv_from_obj(names)), 0);
    // functions (the last declaration of a name wins), then vars, then lexical bindings
    for (struct decl* d = s->decls; d; d = d->next) {
        if (d->kind != D_FUNC || (d->flags & DF_LEX_FUNC) || !d->node) continue;
        struct decl* last = d;
        for (struct decl* e = d->next; e; e = e->next) if (e->name == d->name && e->kind == D_FUNC && !(e->flags & DF_LEX_FUNC)) last = e;
        if (last != d) continue;
        comp_function_node(c, d->node);
        op32(c, OP_DECL_GLOBAL_FUNC, atom_const(c, d->name), -1);
    }
    for (struct decl* d = s->decls; d; d = d->next)
        if (d->kind == D_VAR) op32(c, OP_DECL_GLOBAL_VAR, atom_const(c, d->name), 0);
    for (struct decl* d = s->decls; d; d = d->next)
        if (!is_eval && (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS)) {   // eval keeps them local
            op(c, OP_DECL_GLOBAL_LEX, 0);
            put32(c, atom_const(c, d->name));
            put16(c, d->kind == D_CONST ? 1 : 0);
        }
    // block-level functions of the script are also global vars (Annex B.3.3.2)
    (void)is_eval;
}

// does any Annex B var of a script / eval conflict? (the decl list is final)
static void script_annexb(struct cfunc* c, struct funcinfo* top) {
    if (top->flags & FI_STRICT) return;
    collect_annexb(c, top->body ? top->body : 0, top);
}

struct ftempl* compile_script(ojs* J, struct parser* P) {
    struct cfunc C;
    struct cfunc* c = &C;
    struct funcinfo* top = P->top;
    cfunc_init_top(c, J, P, top);
    c->is_global_code = 1;
    top->body = P->program->a;
    c->completion_slot = new_local(c);
    script_annexb(c, top);
    global_decls(c, top->body_scope, 0);
    if (c->failed) return 0;
    comp_body_and_finish(c);
    return finish_template(c);
}

// eval code. env: the direct-eval environment (may be NULL: indirect eval)
struct ftempl* compile_eval(ojs* J, struct parser* P, struct scope* envscope, int in_function, int sloppy_vars_to_caller) {
    struct cfunc C;
    struct cfunc* c = &C;
    struct funcinfo* top = P->top;
    cfunc_init_top(c, J, P, top);
    c->is_eval = 1;
    top->body = P->program->a;
    struct scope* es = top->body_scope;
    es->parent = envscope;   // the caller's visible bindings (direct eval)
    c->completion_slot = new_local(c);
    int strict = (top->flags & FI_STRICT) != 0;
    if (!strict) script_annexb(c, top);
    if (!strict && !in_function) {
        // global sloppy eval: vars and functions are (deletable) global properties
        global_decls(c, es, 1);
        // leave let/const/class as locals of this eval
        for (struct decl* d = es->decls; d; d = d->next) {
            if (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS) {
                d->slot = new_local(c);
                op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
            }
        }
    } else if (!strict && sloppy_vars_to_caller) {
        // direct sloppy eval in a function: vars / functions go to the caller's eval var object
        // the caller's eval var object: the nearest *evalvars* binding of the environment
        int vix = -1;
        for (struct scope* x = envscope; x && vix < 0; x = x->parent)
            for (struct decl* d = x->decls; d; d = d->next) if (d->kind == D_EVALVARS || (d->kind == D_WITH && (d->flags & 0x20))) { vix = d->slot; break; }
        if (vix < 0) { cerr(c, 0, "internal: eval var scope missing"); return 0; }
        c->eval_dyn_vars = 1;
        for (struct decl* d = es->decls; d; d = d->next) {
            if (d->kind == D_VAR) { op32(c, OP_EVAL_VAR_DECL, atom_const(c, d->name), 0); put16(c, (uint32_t)vix); }
        }
        for (struct decl* d = es->decls; d; d = d->next) {
            if (d->kind != D_FUNC || (d->flags & DF_LEX_FUNC) || !d->node) continue;
            op32(c, OP_EVAL_VAR_DECL, atom_const(c, d->name), 0);
            put16(c, (uint32_t)vix);
        }
        // var / function names are no longer static bindings of the eval code
        struct decl** pp = &es->decls;
        struct decl* keep_funcs = 0;
        (void)keep_funcs;
        while (*pp) {
            struct decl* d = *pp;
            if (d->kind == D_VAR || (d->kind == D_FUNC && !(d->flags & DF_LEX_FUNC))) {
                *pp = d->next;
                es->ndecls--;
                continue;
            }
            pp = &d->next;
        }
        es->decls_tail = 0;
        for (struct decl* d = es->decls; d; d = d->next) es->decls_tail = d;
        es->hash = 0;
        for (struct decl* d = es->decls; d; d = d->next) {
            d->slot = new_local(c);
            if (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
        }
        // hoisted functions: create and store through the (now dynamic) names
        for (struct node* s = top->body; s; s = s->next) {
            if (s->type != N_FUNC || !(s->u.fn->flags & FI_DECL) || (s->flags & NF_ANNEXB)) continue;
            comp_function_node(c, s);
            store_name(c, s->u.fn->name, es, s->pos, 0, 0);
        }
    } else {
        // strict eval: everything is local to the eval code
        for (struct decl* d = es->decls; d; d = d->next) {
            d->slot = new_local(c);
            if (d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS) op16(c, OP_HOLE_LOC, (uint32_t)d->slot, 0);
        }
        for (struct decl* d = es->decls; d; d = d->next) {
            if (d->kind != D_FUNC || (d->flags & DF_LEX_FUNC) || !d->node) continue;
            struct decl* last = d;
            for (struct decl* e = d->next; e; e = e->next) if (e->name == d->name && e->kind == D_FUNC) last = e;
            if (last != d) continue;
            comp_function_node(c, d->node);
            op16(c, OP_PUT_LOC, (uint32_t)d->slot, -1);
        }
    }
    if (c->failed) return 0;
    comp_body_and_finish(c);
    return finish_template(c);
}

// a module: every module-scope binding is a cell (upvalue slot = binding
// index); hoisted function declarations are created by a separate init
// template that runs when the module is linked
struct ftempl* compile_module(ojs* J, struct parser* P, struct ftempl** init_out, int* ncells) {
    struct funcinfo* top = P->top;
    struct scope* ms = top->body_scope;
    int n = 0;
    for (struct decl* d = ms->decls; d; d = d->next) d->slot = n++;
    *ncells = n;
    struct cfunc CI;
    cfunc_init_top(&CI, J, P, top);
    CI.is_module = 1;
    // top-level function declarations of a module are lexical (DF_LEX_FUNC) but
    // instantiated with the module, before any of its code runs
    for (struct decl* d = ms->decls; d; d = d->next) {
        if (d->kind != D_FUNC || !d->node) continue;
        comp_function_node(&CI, d->node);
        op16(&CI, OP_INIT_UPV, (uint32_t)d->slot, 0);
        op(&CI, OP_POP, -1);
    }
    op(&CI, OP_RETURN_UNDEF, 0);
    struct ftempl* init = finish_template(&CI);
    if (!init) return 0;
    struct cfunc C;
    cfunc_init_top(&C, J, P, top);
    C.is_module = 1;
    top->body = P->program->a;
    comp_body_and_finish(&C);
    struct ftempl* body = finish_template(&C);
    if (!body) return 0;
    init->nupvals = body->nupvals = 0;   // cells are attached by the linker
    *init_out = init;
    return body;
}

// a single function from source (new Function): P->top's only statement
struct ftempl* compile_function_source(ojs* J, struct parser* P, struct funcinfo* fi) {
    return compile_func(J, P, fi, 0, 0);
}

// ---------------------------------------------------------------- lazy first-call fill

// point the freshly compiled templates at the canonical source text (their own
// parse ran on a slice/reconstruction of it; the offsets are absolute already)
static void lazy_fix_source(struct ftempl* x, struct ftempl* stub, int depth) {
    if (!x || depth > 200) return;
    x->source = stub->source;
    x->srct = stub->srct;
    x->filename = stub->filename;
    for (uint32_t i = 0; i < x->nconsts; i++) {
        jv v = x->consts[i];
        if (JV_TAG(v) == TAG_PTR && ((struct gch*)JV_PTR(v))->type == GT_FTEMPL)
            lazy_fix_source((struct ftempl*)JV_PTR(v), stub, depth + 1);
    }
}

// fill a lazy stub (called at the function's first call, vm.c): parse its body
// from the function's own source range with the recorded context, compile it
// against the stub's upvalue map, and move the result into the stub in place.
// The stub's upvalue table must not change: closures created before the fill
// hold cells built from it.
struct ftempl* lazy_compile_body(ojs* J, struct ftempl* t) {
    jv srcv = JV_UNDEFINED;
    if (t->srct) srcv = srctext_slice(J, t->srct, 0, srctext_len(t->srct));
    else if (t->source) srcv = jv_from_str(t->source);
    if (!jv_is_str(srcv)) {
        if (!jv_is_exc(srcv)) throw_syntax(J, "internal: lazy function has no source");
        return 0;
    }
    struct parse_opts o;
    memset(&o, 0, sizeof o);
    o.kind = PARSE_SCRIPT;
    o.strict = (t->flags & TF_STRICT) != 0;
    o.ctx = t->ctx_flags;
    o.start = t->src_start;
    o.end = t->src_end;
    struct parser P2;
    if (parse_program(J, jv_str(srcv), &o, &P2) < 0) {
        parse_free(&P2);
        return 0;   // SyntaxError pending (the recorded context must reproduce the original parse)
    }
    struct node* s = P2.program->a;
    if (s && s->type == N_EXPR_STMT) s = s->a;
    if (!s || s->type != N_FUNC || s->next || P2.failed) {
        parse_free(&P2);
        throw_syntax(J, "internal: lazy re-parse did not yield one function");
        return 0;
    }
    struct funcinfo* fi2 = s->u.fn;
    struct ftempl* t2 = compile_func(J, &P2, fi2, 0, t);
    if (!t2) {
        parse_free(&P2);
        if (!jv_is_exc(J->exc)) throw_syntax(J, "internal: lazy compile failed");
        return 0;
    }
    lazy_fix_source(t2, t, 0);
    // move the body into the stub while the collector is still paused (parse_free releases it)
    t->code = t2->code;
    t->code_len = t2->code_len;
    t->consts = t2->consts;
    t->nconsts = t2->nconsts;
    t->nlocals = t2->nlocals;
    t->stack_size = t2->stack_size;
    t->this_slot = t2->this_slot;
    t->newtarget_slot = t2->newtarget_slot;
    t->home_slot = t2->home_slot;
    t->callee_slot = t2->callee_slot;
    t->args_slot = t2->args_slot;
    t->exc = t2->exc;
    t->nexc = t2->nexc;
    t->lines = t2->lines;
    t->nlines = t2->nlines;
    t->param_map = t2->param_map;
    t->nic = t2->nic;
    t->flags = t2->flags | (t->flags & TF_HIDDEN);   // TF_LAZY gone; keep hide_source
    parse_free(&P2);
    return t;
}
