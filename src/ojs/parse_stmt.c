// parse_stmt.c — statements, patterns, functions, classes and modules.
// (Included by parse.c: shares its static helpers.)

// ---------------------------------------------------------------- patterns

static int check_binding_name(struct parser* P, struct node* id) {
    struct str* s = id->u.str;
    if (strict(P) && (s == P->J->A->eval || s == P->J->A->arguments)) {
        perr(P, id->pos, "Unexpected eval or arguments in strict mode");
        return 0;
    }
    if (strict(P) && is_strict_reserved(P, s)) {
        perr(P, id->pos, "Unexpected strict mode reserved word '%S'", s);
        return 0;
    }
    return 1;
}

// simple assignment target (x = ..., x++, for (x in ...))
static int to_assign_target(struct parser* P, struct node* n, int simple_only) {
    (void)simple_only;
    switch (n->type) {
    case N_IDENT:
        if (strict(P) && (n->u.str == P->J->A->eval || n->u.str == P->J->A->arguments)) {
            perr(P, n->pos, "Unexpected eval or arguments in strict mode");
            return 0;
        }
        return 1;
    case N_MEMBER:
        if (n->flags & NF_OPTIONAL) break;
        return 1;
    case N_CALL:
        // Annex B web compatibility: a ReferenceError at run time (sloppy only)
        if (!strict(P) && simple_only != 2 && !(n->flags & NF_OPTIONAL)) return 1;
        break;
    default:
        break;
    }
    perr(P, n->pos, "Invalid left-hand side in assignment");
    return 0;
}

static int to_pattern(struct parser* P, struct node* n, int binding) {
    if (FAILED) return 0;
    switch (n->type) {
    case N_IDENT:
        if (n->flags & NF_PAREN && binding) break;
        if (binding) return check_binding_name(P, n);
        return to_assign_target(P, n, 1);
    case N_MEMBER:
        if (binding || (n->flags & NF_OPTIONAL)) break;
        return 1;
    case N_ASSIGN_PAT:
        if (!to_pattern(P, n->a, binding)) return 0;
        return 1;
    case N_ASSIGN:
        if (n->op != T_ASSIGN || (n->flags & NF_PAREN)) break;
        n->type = N_ASSIGN_PAT;
        return to_pattern(P, n->a, binding);
    case N_OBJECT_PAT: case N_ARRAY_PAT:
        return 1;   // already converted (nested cover)
    case N_OBJECT: {
        if (n->flags & NF_PAREN) break;
        n->type = N_OBJECT_PAT;
        for (struct node* p = n->a; p; p = p->next) {
            if (p->op == PK_SPREAD) {
                if (p->next || (p->flags & NF_TAIL)) { perr(P, p->pos, "Rest element must be last element"); return 0; }
                struct node* t = p->b;
                if (binding ? t->type != N_IDENT : !(t->type == N_IDENT || t->type == N_MEMBER)) {
                    perr(P, p->pos, "`...` must be followed by an assignable reference in assignment contexts");
                    return 0;
                }
                if (!to_pattern(P, t, binding)) return 0;
                p->op = PK_SPREAD;
                continue;
            }
            if (p->op == PK_PROTO) p->op = PK_INIT;
            if (p->op != PK_INIT) { perr(P, p->pos, "Invalid destructuring assignment target"); return 0; }
            p->flags &= ~NF_COVER_INIT;
            if (!to_pattern(P, p->b, binding)) return 0;
        }
        return 1;
    }
    case N_ARRAY: {
        if (n->flags & NF_PAREN) break;
        n->type = N_ARRAY_PAT;
        for (struct node* e = n->a; e; e = e->next) {
            if (e->type == N_HOLE) continue;
            if (e->type == N_SPREAD) {
                if (e->next || (e->flags & NF_SHORTHAND)) { perr(P, e->pos, "Rest element must be last element"); return 0; }
                if (e->a->type == N_ASSIGN && !(e->a->flags & NF_PAREN)) { perr(P, e->pos, "Rest element may not have a default initializer"); return 0; }
                e->type = N_REST;
                if (!to_pattern(P, e->a, binding)) return 0;
                continue;
            }
            if (!to_pattern(P, e, binding)) return 0;
        }
        return 1;
    }
    case N_REST:
        return to_pattern(P, n->a, binding);
    default:
        break;
    }
    perr(P, n->pos, "Invalid destructuring assignment target");
    return 0;
}

// binding pattern / identifier for declarations and parameters
static struct node* parse_binding_element(struct parser* P, int kind);

static struct node* parse_binding_target(struct parser* P, int kind) {
    uint32_t pos = TPOS;
    if (TOK == T_LBRACK) {
        struct node* a = mk(P, N_ARRAY_PAT, pos);
        if (!a) return 0;
        next(P);
        struct node** tail = &a->a;
        while (TOK != T_RBRACK && !FAILED) {
            struct node* e;
            if (TOK == T_COMMA) { e = mk(P, N_HOLE, TPOS); next(P); *tail = e; tail = &e->next; continue; }
            if (TOK == T_ELLIPSIS) {
                uint32_t rp = TPOS;
                next(P);
                e = mk(P, N_REST, rp);
                if (!e) return 0;
                e->a = parse_binding_target(P, kind);
                if (!e->a) return 0;
                *tail = e;
                tail = &e->next;
                if (TOK != T_RBRACK) { perr(P, TPOS, "Rest element must be last element"); return 0; }
                break;
            }
            e = parse_binding_element(P, kind);
            if (!e) return 0;
            *tail = e;
            tail = &e->next;
            if (TOK != T_RBRACK && !expect(P, T_COMMA)) return 0;
        }
        expect(P, T_RBRACK);
        return a;
    }
    if (TOK == T_LBRACE) {
        struct node* o = mk(P, N_OBJECT_PAT, pos);
        if (!o) return 0;
        next(P);
        struct node** tail = &o->a;
        while (TOK != T_RBRACE && !FAILED) {
            struct node* p = mk(P, N_PROP, TPOS);
            if (!p) return 0;
            if (TOK == T_ELLIPSIS) {
                next(P);
                p->op = PK_SPREAD;
                uint32_t ip = TPOS;
                struct str* nm = binding_ident(P);
                if (!nm) return 0;
                p->b = make_ident(P, nm, ip);
                *tail = p;
                if (TOK != T_RBRACE) { perr(P, TPOS, "Rest element must be last element"); return 0; }
                break;
            }
            p->op = PK_INIT;
            int key_tok = TOK, key_esc = P->L.t.escaped;
            struct str* key_name = TOK == T_IDENT ? P->L.t.str : 0;
            uint32_t kp = TPOS;
            int computed;
            p->a = parse_property_name(P, &computed, 0);
            if (!p->a) return 0;
            if (computed) p->flags |= NF_COMPUTED;
            if (TOK == T_COLON) {
                next(P);
                p->b = parse_binding_element(P, kind);
            } else {
                if (key_tok != T_IDENT) { perr(P, kp, "unexpected %s in object pattern", tok_name(key_tok)); return 0; }
                if (key_esc && lex_is_keyword(key_name)) { perr(P, kp, "Keyword must not contain escaped characters"); return 0; }
                if (!ident_ok(P, key_name, kp, 1)) return 0;
                struct node* id = make_ident(P, key_name, kp);
                p->flags |= NF_SHORTHAND;
                if (TOK == T_ASSIGN) {
                    next(P);
                    struct node* ap = mk(P, N_ASSIGN_PAT, kp);
                    if (!ap) return 0;
                    ap->a = id;
                    ap->b = parse_assign(P);
                    p->b = ap;
                } else p->b = id;
            }
            if (!p->b) return 0;
            *tail = p;
            tail = &p->next;
            if (TOK != T_RBRACE && !expect(P, T_COMMA)) return 0;
        }
        expect(P, T_RBRACE);
        return o;
    }
    struct str* nm = binding_ident(P);
    if (!nm) return 0;
    return make_ident(P, nm, pos);
}

static struct node* parse_binding_element(struct parser* P, int kind) {
    uint32_t pos = TPOS;
    struct node* t = parse_binding_target(P, kind);
    if (!t) return 0;
    if (TOK == T_ASSIGN) {
        next(P);
        struct node* ap = mk(P, N_ASSIGN_PAT, pos);
        if (!ap) return 0;
        ap->a = t;
        ap->b = parse_assign(P);
        if (!ap->b) return 0;
        return ap;
    }
    return t;
}

// declare every identifier of a binding pattern
static void declare_pattern(struct parser* P, struct node* n, int kind) {
    if (!n || FAILED) return;
    switch (n->type) {
    case N_IDENT:
        if (kind == D_VAR) declare_var(P, n->u.str, n->pos, n);
        else if (kind == D_PARAM || kind == D_CATCH) {
            struct decl* d = scope_find(P->S, n->u.str);
            if (d && d->kind == kind) {
                if (kind == D_CATCH) { perr(P, n->pos, "Identifier '%S' has already been declared", n->u.str); return; }
                d->flags |= DF_PARAM_DUP;   // one binding; the last argument wins (sloppy only)
            } else add_decl(P, P->S, n->u.str, kind, n);
        } else declare_lex(P, n->u.str, kind, n->pos, n);
        n->op = (uint8_t)kind;
        return;
    case N_ASSIGN_PAT: declare_pattern(P, n->a, kind); return;
    case N_REST: declare_pattern(P, n->a, kind); return;
    case N_ARRAY_PAT:
        for (struct node* e = n->a; e; e = e->next) if (e->type != N_HOLE) declare_pattern(P, e, kind);
        return;
    case N_OBJECT_PAT:
        for (struct node* p = n->a; p; p = p->next) declare_pattern(P, p->b, kind);
        return;
    default: return;
    }
}

// ---------------------------------------------------------------- labels

struct label { struct str* name; int is_loop; struct label* next; int fn_depth; };

static int label_find(struct parser* P, struct str* name, int need_loop) {
    for (struct label* l = P->labels; l; l = l->next) {
        if (l->fn_depth != P->F->depth) break;
        if (l->name == name) return need_loop ? (l->is_loop ? 1 : -1) : 1;
    }
    return 0;
}

// ---------------------------------------------------------------- variable statements

// var / let / const declaration list. for_head: in a for(...) head (no `in`,
// initializers optional for for-in/of)
static struct node* parse_var_decl(struct parser* P, int kind, int for_head) {
    uint32_t pos = TPOS;
    struct node* v = mk(P, N_VAR, pos);
    if (!v) return 0;
    v->op = (uint8_t)kind;
    next(P);   // var / let / const
    struct node** tail = &v->a;
    for (;;) {
        struct node* d = mk(P, N_DECLARATOR, TPOS);
        if (!d) return 0;
        d->a = parse_binding_target(P, kind);
        if (!d->a) return 0;
        if (TOK == T_ASSIGN) {
            next(P);
            d->b = parse_assign(P);
            if (!d->b) return 0;
        } else if (!for_head || (TOK != T_IN && !IS_CTX(of))) {
            if (kind == D_CONST) { perr(P, TPOS, "Missing initializer in const declaration"); return 0; }
            if (d->a->type != N_IDENT) { perr(P, TPOS, "Missing initializer in destructuring declaration"); return 0; }
        }
        declare_pattern(P, d->a, kind);
        *tail = d;
        tail = &d->next;
        if (TOK != T_COMMA) break;
        next(P);
    }
    return v;
}

// is `let` here the start of a lexical declaration?
static int let_is_decl(struct parser* P, int stmt_list) {
    if (!(IS_CTX(let))) return 0;
    struct token t;
    int nt = peek(P, &t);
    if (nt == T_LBRACK || nt == T_LBRACE) return 1;
    if (nt == T_IDENT || (nt >= T_BREAK && nt < T_COUNT && nt != T_IN && nt != T_INSTANCEOF)) {
        if (!stmt_list && t.nl_before) return 0;   // `let` identifier statement followed by ASI
        if (nt == T_IDENT && t.str == P->J->A->let && !strict(P)) return 1;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- statements

#define CTX_LIST    0x01   // StatementListItem (declarations allowed)
#define CTX_LABELED 0x02   // body of a labelled statement (sloppy function declarations allowed)
#define CTX_IF      0x04   // body of if (sloppy function declarations allowed)

static struct node* parse_block_body(struct parser* P, struct node* owner, int kind) {
    struct node* b = mk(P, N_BLOCK, TPOS);
    if (!b) return 0;
    if (!expect(P, T_LBRACE)) return 0;
    b->scope = push_scope(P, kind);
    if (!b->scope) return 0;
    b->scope->owner = owner ? owner : b;
    struct node** tail = &b->a;
    while (TOK != T_RBRACE && TOK != T_EOF && !FAILED) {
        struct node* s = parse_statement_list_item(P);
        if (!s) return 0;
        *tail = s;
        tail = &s->next;
    }
    b->end = P->L.t.end;
    pop_scope(P);
    expect(P, T_RBRACE);
    return b;
}

static struct node* parse_for(struct parser* P) {
    uint32_t pos = TPOS;
    next(P);   // for
    int is_await = 0;
    if (IS_CTX(await) && !P->L.t.escaped) {
        if (!(in_async(P) || (P->F->flags & FI_MODULE))) { perr(P, TPOS, "for await is only valid in async functions and modules"); return 0; }
        is_await = 1;
        next(P);
    }
    if (!expect(P, T_LPAREN)) return 0;
    struct scope* sc = push_scope(P, SC_BLOCK);   // for (let ...) bindings
    if (!sc) return 0;
    sc->is_loop_body = 1;
    struct node* init = 0;
    int decl_kind = 0;
    uint32_t init_pos = TPOS;
    int saved_in = P->allow_in;
    if (TOK == T_SEMI) {
        // no init
    } else if (TOK == T_VAR) {
        P->allow_in = 0;
        init = parse_var_decl(P, D_VAR, 1);
        P->allow_in = saved_in;
        decl_kind = D_VAR;
    } else if (TOK == T_CONST || (IS_CTX(let) && let_is_decl(P, 1))) {
        int k = TOK == T_CONST ? D_CONST : D_LET;
        P->allow_in = 0;
        init = parse_var_decl(P, k, 1);
        P->allow_in = saved_in;
        decl_kind = k;
    } else {
        // expression (or pattern for for-in/of)
        int starts_let = IS_CTX(let) && !P->L.t.escaped;
        int starts_async = IS_CTX(async) && !P->L.t.escaped;
        struct node* saved_cover = P->cover_init;
        P->cover_init = 0;
        P->allow_in = 0;
        init = parse_expression(P);
        P->allow_in = saved_in;
        if (!init) return 0;
        if (TOK == T_IN || (IS_CTX(of) && !P->L.t.escaped)) {
            int is_of = TOK != T_IN;
            if (is_of && starts_let) { perr(P, init_pos, "The left-hand side of a for-of loop may not be 'let'."); return 0; }
            if (is_of && starts_async && init->type == N_IDENT && !is_await && !(init->flags & NF_PAREN)) {
                perr(P, init_pos, "The left-hand side of a for-of loop may not be 'async'.");
                return 0;
            }
            if ((init->type == N_OBJECT || init->type == N_ARRAY) && !(init->flags & NF_PAREN)) {
                if (!to_pattern(P, init, 0)) return 0;
            } else if (!to_assign_target(P, init, 1)) return 0;
            P->cover_init = saved_cover;
        } else {
            if (P->cover_init) { perr(P, P->cover_init->pos, "Invalid shorthand property initializer"); return 0; }
            P->cover_init = saved_cover;
        }
    }
    if (FAILED) return 0;
    struct node* f;
    if (TOK == T_IN || (IS_CTX(of) && !P->L.t.escaped)) {
        int is_of = TOK != T_IN;
        if (is_await && !is_of) { perr(P, TPOS, "for await requires 'of'"); return 0; }
        f = mk(P, is_of ? N_FOR_OF : N_FOR_IN, pos);
        if (!f) return 0;
        if (is_await) f->flags |= NF_AWAIT;
        if (decl_kind) {
            struct node* d = init->a;
            if (d->next) { perr(P, init_pos, "Invalid left-hand side in for-%s loop: Must have a single binding.", is_of ? "of" : "in"); return 0; }
            if (d->b) {
                // for (var x = 1 in o) is legal only in sloppy mode, for-in, var, simple identifier
                if (is_of || decl_kind != D_VAR || strict(P) || d->a->type != N_IDENT) {
                    perr(P, init_pos, "for-%s loop variable declaration may not have an initializer.", is_of ? "of" : "in");
                    return 0;
                }
            }
            if (decl_kind != D_VAR) {
                // the bound names may not include "let"
            }
        }
        next(P);
        f->a = init;
        f->b = is_of ? parse_assign(P) : parse_expression_checked(P);
        if (!f->b) return 0;
    } else {
        if (is_await) { perr(P, TPOS, "for await requires 'of'"); return 0; }
        f = mk(P, N_FOR, pos);
        if (!f) return 0;
        f->a = init;
        if (init && init->type == N_VAR && init->op != D_VAR) {
            for (struct node* d = init->a; d; d = d->next)
                if (!d->b && init->op == D_CONST) { perr(P, init_pos, "Missing initializer in const declaration"); return 0; }
        }
        if (!expect(P, T_SEMI)) return 0;
        if (TOK != T_SEMI) { f->b = parse_expression_checked(P); if (!f->b) return 0; }
        if (!expect(P, T_SEMI)) return 0;
        if (TOK != T_RPAREN) { f->c = parse_expression_checked(P); if (!f->c) return 0; }
    }
    if (!expect(P, T_RPAREN)) return 0;
    f->scope = sc;
    sc->owner = f;
    P->iteration_depth++;
    f->d = parse_statement(P, 0);
    P->iteration_depth--;
    pop_scope(P);
    if (!f->d) return 0;
    // the body's lexical declarations may not shadow the head's (checked by scopes)
    if (decl_kind == D_LET || decl_kind == D_CONST) {
        // bound names of the head conflict with var declarations in the body
        for (struct decl* d = sc->decls; d; d = d->next)
            if (has_varname(sc, d->name)) { perr(P, pos, "Identifier '%S' has already been declared", d->name); return 0; }
    }
    return f;
}

static struct node* parse_switch(struct parser* P) {
    uint32_t pos = TPOS;
    next(P);
    struct node* s = mk(P, N_SWITCH, pos);
    if (!s) return 0;
    if (!expect(P, T_LPAREN)) return 0;
    s->a = parse_expression_checked(P);
    if (!s->a || !expect(P, T_RPAREN) || !expect(P, T_LBRACE)) return 0;
    s->scope = push_scope(P, SC_BLOCK);
    if (!s->scope) return 0;
    s->scope->owner = s;
    P->switch_depth++;
    struct node** tail = &s->b;
    int has_default = 0;
    while (TOK != T_RBRACE && !FAILED) {
        struct node* c = mk(P, N_CASE, TPOS);
        if (!c) return 0;
        if (TOK == T_CASE) {
            next(P);
            c->a = parse_expression_checked(P);
            if (!c->a) return 0;
        } else if (TOK == T_DEFAULT) {
            if (has_default) { perr(P, TPOS, "More than one default clause in switch statement"); return 0; }
            has_default = 1;
            next(P);
        } else { perr(P, TPOS, "unexpected %s in switch", tok_name(TOK)); return 0; }
        if (!expect(P, T_COLON)) return 0;
        struct node** st = &c->b;
        while (TOK != T_CASE && TOK != T_DEFAULT && TOK != T_RBRACE && !FAILED) {
            struct node* x = parse_statement_list_item(P);
            if (!x) return 0;
            *st = x;
            st = &x->next;
        }
        *tail = c;
        tail = &c->next;
    }
    P->switch_depth--;
    pop_scope(P);
    expect(P, T_RBRACE);
    return s;
}

static struct node* parse_try(struct parser* P) {
    uint32_t pos = TPOS;
    next(P);
    struct node* t = mk(P, N_TRY, pos);
    if (!t) return 0;
    t->a = parse_block_body(P, 0, SC_BLOCK);
    if (!t->a) return 0;
    if (TOK == T_CATCH) {
        next(P);
        struct node* h = mk(P, N_BLOCK, TPOS);   // catch: b = param pattern (or NULL), a = body
        if (!h) return 0;
        h->scope = push_scope(P, SC_CATCH);
        if (!h->scope) return 0;
        h->scope->owner = h;
        if (TOK == T_LPAREN) {
            next(P);
            h->b = parse_binding_target(P, D_CATCH);
            if (!h->b) return 0;
            declare_pattern(P, h->b, D_CATCH);
            if (!expect(P, T_RPAREN)) return 0;
        }
        // the block shares the catch scope's conflict rules (marked via owner->op)
        h->op = 1;
        struct node* body = mk(P, N_BLOCK, TPOS);
        if (!body) return 0;
        if (!expect(P, T_LBRACE)) return 0;
        body->scope = push_scope(P, SC_BLOCK);
        if (!body->scope) return 0;
        body->scope->owner = h;
        struct node** tail = &body->a;
        while (TOK != T_RBRACE && TOK != T_EOF && !FAILED) {
            struct node* s = parse_statement_list_item(P);
            if (!s) return 0;
            *tail = s;
            tail = &s->next;
        }
        pop_scope(P);
        expect(P, T_RBRACE);
        // a catch parameter conflicts with lexical declarations in the block
        if (h->b) {
            for (struct decl* d = h->scope->decls; d; d = d->next) {
                struct decl* x = scope_find(body->scope, d->name);
                if (x && (is_lexical(x->kind) || x->kind == D_FUNC)) { perr(P, x->node ? x->node->pos : pos, "Identifier '%S' has already been declared", d->name); return 0; }
                // var of the same name is allowed only for simple catch parameters (Annex B)
                if (h->b->type != N_IDENT && has_varname(body->scope, d->name)) {
                    perr(P, pos, "Identifier '%S' has already been declared", d->name);
                    return 0;
                }
            }
        }
        h->a = body;
        pop_scope(P);
        t->b = h;
    }
    if (TOK == T_FINALLY) {
        next(P);
        t->c = parse_block_body(P, 0, SC_BLOCK);
        if (!t->c) return 0;
    }
    if (!t->b && !t->c) { perr(P, TPOS, "Missing catch or finally after try"); return 0; }
    return t;
}

static struct node* parse_function_declaration(struct parser* P, uint32_t pos, uint32_t fl, int ctx);

static struct node* parse_statement(struct parser* P, int ctx) {
    uint32_t pos = TPOS;
    if (FAILED) return 0;
    switch (TOK) {
    case T_LBRACE: return parse_block_body(P, 0, SC_BLOCK);
    case T_SEMI: { struct node* e = mk(P, N_EMPTY, pos); next(P); return e; }
    case T_VAR: {
        struct node* v = parse_var_decl(P, D_VAR, 0);
        if (!v || !asi(P)) return 0;
        return v;
    }
    case T_IF: {
        next(P);
        struct node* n = mk(P, N_IF, pos);
        if (!n || !expect(P, T_LPAREN)) return 0;
        n->a = parse_expression_checked(P);
        if (!n->a || !expect(P, T_RPAREN)) return 0;
        n->b = parse_statement(P, CTX_IF);
        if (!n->b) return 0;
        if (TOK == T_ELSE) {
            next(P);
            n->c = parse_statement(P, CTX_IF);
            if (!n->c) return 0;
        }
        return n;
    }
    case T_FOR: return parse_for(P);
    case T_WHILE: {
        next(P);
        struct node* n = mk(P, N_WHILE, pos);
        if (!n || !expect(P, T_LPAREN)) return 0;
        n->a = parse_expression_checked(P);
        if (!n->a || !expect(P, T_RPAREN)) return 0;
        P->iteration_depth++;
        n->b = parse_statement(P, 0);
        P->iteration_depth--;
        return n->b ? n : 0;
    }
    case T_DO: {
        next(P);
        struct node* n = mk(P, N_DO_WHILE, pos);
        if (!n) return 0;
        P->iteration_depth++;
        n->b = parse_statement(P, 0);
        P->iteration_depth--;
        if (!n->b || !expect(P, T_WHILE) || !expect(P, T_LPAREN)) return 0;
        n->a = parse_expression_checked(P);
        if (!n->a || !expect(P, T_RPAREN)) return 0;
        if (TOK == T_SEMI) next(P);   // ASI after do-while is always allowed
        return n;
    }
    case T_CONTINUE: case T_BREAK: {
        int is_cont = TOK == T_CONTINUE;
        next(P);
        struct node* n = mk(P, is_cont ? N_CONTINUE : N_BREAK, pos);
        if (!n) return 0;
        if (TOK == T_IDENT && !P->L.t.nl_before) {
            struct str* l = P->L.t.str;
            int r = label_find(P, l, is_cont);
            if (r == 0) { perr(P, TPOS, "Undefined label '%S'", l); return 0; }
            if (r < 0) { perr(P, TPOS, "Illegal continue statement: '%S' does not denote an iteration statement", l); return 0; }
            n->str2 = l;
            next(P);
        } else if (is_cont ? !P->iteration_depth : (!P->iteration_depth && !P->switch_depth)) {
            perr(P, pos, is_cont ? "Illegal continue statement: no surrounding iteration statement" :
                                   "Illegal break statement");
            return 0;
        }
        if (!asi(P)) return 0;
        return n;
    }
    case T_RETURN: {
        if (!P->in_function_body) { perr(P, pos, "Illegal return statement"); return 0; }
        next(P);
        struct node* n = mk(P, N_RETURN, pos);
        if (!n) return 0;
        if (TOK != T_SEMI && TOK != T_RBRACE && TOK != T_EOF && !P->L.t.nl_before) {
            n->a = parse_expression_checked(P);
            if (!n->a) return 0;
        }
        if (!asi(P)) return 0;
        return n;
    }
    case T_WITH: {
        if (strict(P)) { perr(P, pos, "Strict mode code may not include a with statement"); return 0; }
        next(P);
        struct node* n = mk(P, N_WITH, pos);
        if (!n || !expect(P, T_LPAREN)) return 0;
        n->a = parse_expression_checked(P);
        if (!n->a || !expect(P, T_RPAREN)) return 0;
        n->scope = push_scope(P, SC_WITH);
        if (!n->scope) return 0;
        n->scope->owner = n;
        // the with object lives in a hidden binding of the with scope
        struct str* wn = atom_cstr(P->J, "*with*");
        if (!wn || !add_decl(P, n->scope, wn, D_WITH, n)) return 0;
        n->b = parse_statement(P, 0);
        pop_scope(P);
        return n->b ? n : 0;
    }
    case T_SWITCH: return parse_switch(P);
    case T_THROW: {
        next(P);
        if (P->L.t.nl_before) { perr(P, TPOS, "Illegal newline after throw"); return 0; }
        struct node* n = mk(P, N_THROW, pos);
        if (!n) return 0;
        n->a = parse_expression_checked(P);
        if (!n->a || !asi(P)) return 0;
        return n;
    }
    case T_TRY: return parse_try(P);
    case T_DEBUGGER: {
        next(P);
        struct node* n = mk(P, N_DEBUGGER, pos);
        if (!asi(P)) return 0;
        return n;
    }
    case T_FUNCTION: {
        // only in StatementList (handled there), sloppy if-bodies and labels
        if (strict(P) || !(ctx & (CTX_IF | CTX_LABELED))) {
            perr(P, pos, strict(P) ? "In strict mode code, functions can only be declared at top level or inside a block." :
                                     "In non-strict mode code, functions can only be declared at top level, inside a block, or as the body of an if statement.");
            return 0;
        }
        next(P);
        if (TOK == T_STAR) { perr(P, pos, "Generators can only be declared at the top level or inside a block."); return 0; }
        if (ctx & CTX_IF) {
            // treated as if wrapped in a block
            struct node* b = mk(P, N_BLOCK, pos);
            if (!b) return 0;
            b->scope = push_scope(P, SC_BLOCK);
            if (!b->scope) return 0;
            b->scope->owner = b;
            b->a = parse_function_declaration(P, pos, 0, ctx);
            pop_scope(P);
            return b->a ? b : 0;
        }
        return parse_function_declaration(P, pos, 0, ctx);
    }
    case T_CLASS: perr(P, pos, "Unexpected token 'class'"); return 0;
    case T_CONST: perr(P, pos, "Lexical declaration cannot appear in a single-statement context"); return 0;
    case T_IMPORT: {
        struct token t;
        int nt = peek(P, &t);
        if (nt != T_LPAREN && nt != T_DOT) { perr(P, pos, "Cannot use import statement here"); return 0; }
        break;
    }
    case T_EXPORT: perr(P, pos, "Unexpected token 'export'"); return 0;
    case T_IDENT: {
        struct str* s = P->L.t.str;
        // labelled statement
        struct token t;
        int nt = peek(P, &t);
        if (nt == T_COLON) {
            if (P->L.t.escaped && lex_is_keyword(s)) { perr(P, pos, "Keyword must not contain escaped characters"); return 0; }
            if (!ident_ok(P, s, pos, 0)) return 0;
            if (s == P->J->A->await && (in_async(P) || P->is_module)) { perr(P, pos, "Unexpected reserved word 'await'"); return 0; }
            if (label_find(P, s, 0)) { perr(P, pos, "Label '%S' has already been declared", s); return 0; }
            next(P);
            next(P);
            struct label lb = { s, TOK == T_FOR || TOK == T_WHILE || TOK == T_DO, P->labels, P->F->depth };
            // a label on a label on a loop is also a loop label
            for (struct label* x = P->labels; x && x->fn_depth == P->F->depth && x->name && x->is_loop == 2;) { (void)x; break; }
            P->labels = &lb;
            struct node* n = mk(P, N_LABEL, pos);
            if (!n) { P->labels = lb.next; return 0; }
            n->str2 = s;
            // propagate "is loop" through chains of labels
            if (TOK == T_IDENT) {
                struct token t2;
                if (peek(P, &t2) == T_COLON) lb.is_loop = 3;   // decided by the inner statement
            }
            if (lb.is_loop == 3) {
                // look through the label chain
                struct lexer save = P->L;
                int depth = 0;
                while (TOK == T_IDENT) {
                    struct token t3;
                    if (peek(P, &t3) != T_COLON) break;
                    next(P); next(P);
                    depth++;
                }
                lb.is_loop = TOK == T_FOR || TOK == T_WHILE || TOK == T_DO;
                uint32_t* ls = P->L.line_starts; uint32_t nl = P->L.nlines;
                P->L = save; P->L.line_starts = ls; P->L.nlines = nl;
                (void)depth;
            }
            n->a = parse_statement(P, (ctx & (CTX_LIST | CTX_LABELED)) ? CTX_LABELED : 0);
            P->labels = lb.next;
            if (n->a && n->a->type == N_BLOCK && n->a->a && n->a->a->type == N_FUNC && 0) {}
            return n->a ? n : 0;
        }
        if (s == P->J->A->let && !P->L.t.escaped) {
            if (nt == T_LBRACK) { perr(P, pos, "Lexical declaration cannot appear in a single-statement context"); return 0; }
            if ((nt == T_IDENT || nt == T_LBRACE) && !t.nl_before) { perr(P, pos, "Lexical declaration cannot appear in a single-statement context"); return 0; }
        }
        if (s == P->J->A->async && !P->L.t.escaped && nt == T_FUNCTION && !t.nl_before) {
            perr(P, pos, "Async functions can only be declared at the top level or inside a block.");
            return 0;
        }
        break;
    }
    default:
        break;
    }
    // expression statement
    struct node* st = mk(P, N_EXPR_STMT, pos);
    if (!st) return 0;
    st->a = parse_expression_checked(P);
    if (!st->a || !asi(P)) return 0;
    return st;
}

static struct node* parse_function_declaration(struct parser* P, uint32_t pos, uint32_t fl, int ctx) {
    (void)ctx;
    return parse_function(P, pos, fl, 1);
}

static struct node* parse_statement_list_item(struct parser* P) {
    uint32_t pos = TPOS;
    switch (TOK) {
    case T_FUNCTION: {
        next(P);
        uint32_t fl = 0;
        if (TOK == T_STAR) { fl |= FI_GENERATOR; next(P); }
        return parse_function(P, pos, fl, 1);
    }
    case T_CLASS: return parse_class(P, 1);
    case T_CONST: {
        struct node* v = parse_var_decl(P, D_CONST, 0);
        if (!v || !asi(P)) return 0;
        return v;
    }
    case T_IDENT:
        if (IS_CTX(let) && !P->L.t.escaped && let_is_decl(P, 1)) {
            struct node* v = parse_var_decl(P, D_LET, 0);
            if (!v || !asi(P)) return 0;
            return v;
        }
        if (IS_CTX(async) && !P->L.t.escaped) {
            struct token t;
            if (peek(P, &t) == T_FUNCTION && !t.nl_before) {
                next(P);
                next(P);
                uint32_t fl = FI_ASYNC;
                if (TOK == T_STAR) { fl |= FI_GENERATOR; next(P); }
                return parse_function(P, pos, fl, 1);
            }
        }
        break;
    case T_IMPORT: case T_EXPORT:
        break;
    default:
        break;
    }
    return parse_statement(P, CTX_LIST);
}

// ---------------------------------------------------------------- functions

static struct funcinfo* new_funcinfo(struct parser* P, uint32_t flags, uint32_t pos) {
    struct funcinfo* fi = (struct funcinfo*)arena_alloc(P, sizeof(struct funcinfo));
    if (!fi) { perr(P, pos, "out of memory"); return 0; }
    fi->flags = flags;
    fi->parent = P->F;
    fi->src_start = pos;
    if (P->F) {
        fi->depth = P->F->depth + 1;
        if (P->F->flags & FI_STRICT) fi->flags |= FI_STRICT;
        // link as a child (compile order)
        if (!P->F->children) P->F->children = fi;
        else {
            struct funcinfo* c = P->F->children;
            while (c->next_sibling) c = c->next_sibling;
            c->next_sibling = fi;
        }
    }
    return fi;
}

static void enter_function(struct parser* P, struct funcinfo* fi, struct funcinfo** saved_f, struct scope** saved_s) {
    *saved_f = P->F;
    *saved_s = P->S;
    P->F = fi;
    fi->param_scope = push_scope(P, SC_FUNC_PARAMS);
    if (fi->param_scope) fi->param_scope->fn = fi;
    fi->body_scope = fi->param_scope;   // a separate body scope is added when params are not simple
}

static void leave_function(struct parser* P, struct funcinfo* saved_f, struct scope* saved_s) {
    P->F = saved_f;
    P->S = saved_s;
}

static int params_simple(struct node* params) {
    for (struct node* p = params; p; p = p->next) if (p->type != N_IDENT) return 0;
    return 1;
}

// declare parameter names into fi->param_scope (P->S must be it)
static void declare_params(struct parser* P, struct funcinfo* fi) {
    int len = 0, counting = 1;
    for (struct node* p = fi->params; p; p = p->next) {
        if (p->type == N_ASSIGN_PAT || p->type == N_REST) counting = 0;
        if (counting) len++;
        declare_pattern(P, p, D_PARAM);
    }
    fi->nparams_len = len;
    if (params_simple(fi->params)) fi->flags |= FI_SIMPLE_PARAMS;
}

// parameter list rules that depend on the body (strictness, simplicity)
static void finish_function_checks(struct parser* P, struct funcinfo* fi, uint32_t pos) {
    int dup_err = (fi->flags & (FI_STRICT | FI_ARROW | FI_METHOD)) || !(fi->flags & FI_SIMPLE_PARAMS);
    for (struct decl* d = fi->param_scope->decls; d; d = d->next) {
        if (d->kind != D_PARAM) continue;
        if (dup_err && (d->flags & DF_PARAM_DUP)) { perr(P, d->node ? d->node->pos : pos, "Duplicate parameter name not allowed in this context"); return; }
        if (fi->flags & FI_STRICT) {
            if (d->name == P->J->A->eval || d->name == P->J->A->arguments) { perr(P, d->node ? d->node->pos : pos, "Unexpected eval or arguments in strict mode"); return; }
            if (is_strict_reserved(P, d->name)) { perr(P, d->node ? d->node->pos : pos, "Unexpected strict mode reserved word"); return; }
        }
    }
    if ((fi->flags & FI_STRICT) && fi->name && (fi->flags & (FI_DECL | FI_EXPR)) &&
        (fi->name == P->J->A->eval || fi->name == P->J->A->arguments)) {
        perr(P, pos, "Unexpected eval or arguments in strict mode");
    }
}

// directive prologue + statements; returns 0 on failure. P->S is the body scope.
static int parse_function_body(struct parser* P, struct funcinfo* fi) {
    if (!expect(P, T_LBRACE)) return 0;
    struct node** tail = &fi->body;
    int in_prologue = 1;
    uint32_t first_bad_escape = 0;
    int had_bad = 0;
    int saved_body = P->in_function_body, saved_iter = P->iteration_depth, saved_switch = P->switch_depth;
    struct label* saved_labels = P->labels;
    P->in_function_body = 1;
    P->iteration_depth = 0;
    P->switch_depth = 0;
    P->labels = 0;
    while (TOK != T_RBRACE && TOK != T_EOF && !FAILED) {
        if (in_prologue) {
            if (TOK == T_STRING) {
                uint32_t sp = TPOS, se = P->L.t.end;
                int bad = P->L.t.bad_escape;
                struct str* val = P->L.t.str;
                struct token t;
                int nt = peek(P, &t);
                int is_directive = nt == T_SEMI || nt == T_RBRACE || nt == T_EOF || t.nl_before;
                if (nt != T_SEMI && nt != T_RBRACE && nt != T_EOF && t.nl_before) {
                    // the string may continue an expression on the next line ("a"\n + "b")
                    is_directive = !(nt == T_DOT || nt == T_LBRACK || nt == T_LPAREN || (nt >= T_LT && nt <= T_SLASH_ASSIGN) ||
                                     nt == T_COMMA || nt == T_QUESTION || nt == T_TEMPLATE || nt == T_IN || nt == T_INSTANCEOF);
                }
                if (is_directive) {
                    if (bad && !had_bad) { had_bad = 1; first_bad_escape = sp; }
                    if (se - sp == 12 && val && str_eq_ascii(val, "use strict")) {
                        if (!(fi->flags & FI_SIMPLE_PARAMS)) { perr(P, sp, "Illegal 'use strict' directive in function with non-simple parameter list"); return 0; }
                        fi->flags |= FI_STRICT;
                        P->L.strict = 1;
                        if (had_bad) { perr(P, first_bad_escape, "Octal escape sequences are not allowed in strict mode"); return 0; }
                    }
                } else in_prologue = 0;
            } else in_prologue = 0;
        }
        struct node* s = parse_statement_list_item(P);
        if (!s) return 0;
        *tail = s;
        tail = &s->next;
    }
    fi->src_end = P->L.t.end;
    P->in_function_body = saved_body;
    P->iteration_depth = saved_iter;
    P->switch_depth = saved_switch;
    P->labels = saved_labels;
    return expect(P, T_RBRACE);
}

static struct node* parse_arrow_body(struct parser* P, struct funcinfo* fi) {
    struct node* fn = mk(P, N_FUNC, fi->src_start);
    if (!fn) return 0;
    fn->u.fn = fi;
    fi->node = fn;
    next(P);   // =>
    int saved_sb = P->in_static_block;
    P->in_static_block = 0;   // `await` is an identifier again inside arrows
    fi->body_scope = push_scope(P, SC_FUNC_BODY);
    if (!fi->body_scope) return 0;
    fi->body_scope->fn = fi;
    if (TOK == T_LBRACE) {
        if (!parse_function_body(P, fi)) return 0;
    } else {
        fi->flags |= FI_EXPR_BODY;
        int saved_body = P->in_function_body;
        P->in_function_body = 1;
        struct node* e = parse_assign(P);
        P->in_function_body = saved_body;
        if (!e) return 0;
        fi->body = e;
        fi->src_end = P->prev_end;
    }
    finish_function_checks(P, fi, fi->src_start);
    P->in_static_block = saved_sb;
    fn->end = fi->src_end;
    return fn;
}

static struct node* parse_function(struct parser* P, uint32_t pos, uint32_t flags, int is_decl) {
    struct str* name = 0;
    uint32_t name_pos = TPOS;
    int is_method = (flags & FI_METHOD) != 0;
    int name_escaped = 0;
    if (!is_method && TOK != T_LPAREN) {
        if (TOK != T_IDENT) {
            if (TOK >= T_BREAK && TOK < T_COUNT) { perr(P, TPOS, "Unexpected reserved word '%s'", tok_name(TOK)); return 0; }
            perr(P, TPOS, "unexpected %s, expected function name", tok_name(TOK));
            return 0;
        }
        name = P->L.t.str;
        name_escaped = P->L.t.escaped;
        if (name_escaped && lex_is_keyword(name)) { perr(P, TPOS, "Keyword must not contain escaped characters"); return 0; }
        // the name of a function expression follows the function's own rules
        if (!is_decl) {
            ojs* J = P->J;
            if (name == J->A->yield && ((flags & FI_GENERATOR) || strict(P))) { perr(P, TPOS, "Unexpected identifier 'yield'"); return 0; }
            if (name == J->A->await && ((flags & FI_ASYNC) || P->is_module)) { perr(P, TPOS, "Unexpected reserved word 'await'"); return 0; }
            if (strict(P) && is_strict_reserved(P, name)) { perr(P, TPOS, "Unexpected strict mode reserved word"); return 0; }
        } else if (!ident_ok(P, name, TPOS, 1)) return 0;
        next(P);
    } else if (is_decl && !is_method && !name && !P->allow_in && 0) {
    }
    if (is_decl && !name && !(flags & FI_METHOD) && !P->top) { perr(P, pos, "Function statements require a function name"); return 0; }
    struct funcinfo* fi = new_funcinfo(P, flags | (is_decl ? FI_DECL : (is_method ? 0 : FI_EXPR)), pos);
    if (!fi) return 0;
    fi->name = name;
    struct node* fn = mk(P, N_FUNC, pos);
    if (!fn) return 0;
    fn->u.fn = fi;
    fi->node = fn;
    // declarations bind in the enclosing scope
    if (is_decl && name) {
        struct scope* s = P->S;
        int top = s == var_scope(P) || s->kind == SC_FUNC_PARAMS || s->kind == SC_SCRIPT || s->kind == SC_MODULE || s->kind == SC_EVAL;
        if (top && s->kind != SC_MODULE) {
            // var-like (but a lexical declaration of the same name in this scope conflicts)
            struct decl* d = scope_find(s, name);
            if (d && (is_lexical(d->kind))) { perr(P, name_pos, "Identifier '%S' has already been declared", name); return 0; }
            for (struct scope* x = P->S; x; x = x->parent) { add_varname(P, x, name); if (x == var_scope(P)) break; }
            if (!d) d = add_decl(P, s, name, D_FUNC, fn);
            else if (d->kind == D_VAR || d->kind == D_FUNC || d->kind == D_PARAM) {
                // a later declaration wins: record it
                struct decl* nd = add_decl(P, s, name, D_FUNC, fn);
                (void)nd;
            }
        } else {
            uint32_t fl = flags & (FI_ASYNC | FI_GENERATOR);
            declare_lex(P, name, D_FUNC, name_pos, fn);
            // Annex B.3.3: a plain function in a sloppy block is also a var
            if (!strict(P) && !fl && !FAILED) fn->flags |= NF_ANNEXB;
        }
        if (FAILED) return 0;
    }
    struct funcinfo* saved_f;
    struct scope* saved_s;
    int saved_field = P->in_class_field, saved_sb = P->in_static_block, saved_in = P->allow_in;
    struct node* saved_cover = P->cover_init;
    P->in_class_field = 0;
    P->in_static_block = 0;
    P->allow_in = 1;
    P->cover_init = 0;
    enter_function(P, fi, &saved_f, &saved_s);
    // named function expressions see their own name (immutable binding)
    if (!is_decl && name && !is_method) {
        add_decl(P, fi->param_scope, name, D_FUNCNAME, fn);
    }
    if (!expect(P, T_LPAREN)) return 0;
    struct node** tail = &fi->params;
    while (TOK != T_RPAREN && !FAILED) {
        struct node* p;
        if (TOK == T_ELLIPSIS) {
            uint32_t rp = TPOS;
            next(P);
            p = mk(P, N_REST, rp);
            if (!p) return 0;
            p->a = parse_binding_target(P, D_PARAM);
            if (!p->a) return 0;
            if (TOK == T_ASSIGN) { perr(P, TPOS, "Rest parameter may not have a default initializer"); return 0; }
            *tail = p;
            fi->nparams++;
            if (TOK != T_RPAREN) { perr(P, TPOS, "Rest parameter must be last formal parameter"); return 0; }
            break;
        }
        p = parse_binding_element(P, D_PARAM);
        if (!p) return 0;
        // yield / await expressions in parameters
        uint32_t w;
        if ((fi->flags & FI_GENERATOR) && contains_type(p, N_YIELD, &w)) { perr(P, w, "Yield expression not allowed in formal parameter"); return 0; }
        if ((fi->flags & FI_ASYNC) && contains_type(p, N_AWAIT, &w)) { perr(P, w, "Illegal await-expression in formal parameters of async function"); return 0; }
        *tail = p;
        tail = &p->next;
        fi->nparams++;
        if (TOK != T_RPAREN && !expect(P, T_COMMA)) return 0;
    }
    if (!expect(P, T_RPAREN)) return 0;
    if (FAILED) return 0;
    {
        // declare after the list (pattern names) with the current scope = params
        struct scope* here = P->S;
        P->S = fi->param_scope;
        declare_params(P, fi);
        P->S = here;
    }
    fi->body_scope = push_scope(P, SC_FUNC_BODY);
    if (!fi->body_scope) return 0;
    fi->body_scope->fn = fi;
    if (!parse_function_body(P, fi)) return 0;
    finish_function_checks(P, fi, pos);
    if (strict(P) || (fi->flags & FI_STRICT)) {
        if (name && is_decl == 0 && !is_method && (fi->flags & FI_STRICT) && is_strict_reserved(P, name)) {
            perr(P, name_pos, "Unexpected strict mode reserved word");
            return 0;
        }
    }
    leave_function(P, saved_f, saved_s);
    P->in_class_field = saved_field;
    P->in_static_block = saved_sb;
    P->allow_in = saved_in;
    P->cover_init = saved_cover;
    fn->end = fi->src_end;
    return fn;
}

// ---------------------------------------------------------------- classes

struct class_ctx {
    struct class_ctx* parent;
    struct private_name* names;
    struct pref { struct str* name; uint32_t pos; struct pref* next; }* refs;
};

static int private_name_check(struct parser* P, struct str* name, uint32_t pos) {
    if (!P->cls) {
        // eval code inside a class may reference the class's private names
        if (P->eval_env && ojs_eval_env_has_private(P->eval_env, name)) return 1;
        perr(P, pos, "Private field '#%S' must be declared in an enclosing class", name);
        return 0;
    }
    struct pref* r = (struct pref*)arena_alloc(P, sizeof(struct pref));
    if (!r) { perr(P, pos, "out of memory"); return 0; }
    r->name = name;
    r->pos = pos;
    r->next = P->cls->refs;
    P->cls->refs = r;
    return 1;
}

static struct private_name* private_find(struct class_ctx* c, struct str* name) {
    for (struct private_name* p = c->names; p; p = p->next) if (p->name == name) return p;
    return 0;
}

static struct node* parse_class(struct parser* P, int is_decl) {
    uint32_t pos = TPOS;
    next(P);   // class
    struct node* cls = mk(P, N_CLASS, pos);
    if (!cls) return 0;
    // class code is strict (including the name and heritage)
    uint32_t saved_flags = P->F->flags;
    P->F->flags |= FI_STRICT;
    struct str* name = 0;
    uint32_t name_pos = TPOS;
    if (TOK == T_IDENT && !IS_CTX(let)) {
        if (P->L.t.escaped && lex_is_keyword(P->L.t.str)) { perr(P, TPOS, "Keyword must not contain escaped characters"); return 0; }
        name = binding_ident(P);
        if (!name) return 0;
    } else if (TOK == T_IDENT && IS_CTX(let)) {
        perr(P, TPOS, "let is disallowed as a lexically bound name");
        return 0;
    }
    if (is_decl && !name && !P->top) { perr(P, pos, "Class statements require a class name"); return 0; }
    if (is_decl && name) declare_lex(P, name, D_CLASS, name_pos, cls);
    if (FAILED) return 0;
    cls->u.str = name;
    // class scope: inner name binding + private names
    struct scope* cs = push_scope(P, SC_CLASS);
    if (!cs) return 0;
    cs->owner = cls;
    cls->scope = cs;
    if (name) add_decl(P, cs, name, D_CLASSNAME, cls);
    if (TOK == T_EXTENDS) {
        next(P);
        cls->b = parse_lhs(P, 1);
        if (!cls->b) return 0;
        if (bare_arrow(cls->b)) { perr(P, cls->b->pos, "Class heritage may not be an arrow function"); return 0; }
    }
    if (!expect(P, T_LBRACE)) return 0;
    struct class_ctx ctx = { P->cls, 0, 0 };
    P->cls = &ctx;
    // synthetic functions: instance field initializer, static initializer
    struct funcinfo* finit = new_funcinfo(P, FI_METHOD | FI_FIELD_INIT | FI_STRICT, pos);
    struct funcinfo* sinit = new_funcinfo(P, FI_METHOD | FI_STATIC_BLOCK | FI_STRICT, pos);
    if (!finit || !sinit) return 0;
    struct node* fnode = mk(P, N_FUNC, pos);
    struct node* snode = mk(P, N_FUNC, pos);
    if (!fnode || !snode) return 0;
    fnode->u.fn = finit; finit->node = fnode; finit->class_node = cls;
    snode->u.fn = sinit; sinit->node = snode; sinit->class_node = cls;
    {
        struct funcinfo* sf; struct scope* ss;
        enter_function(P, finit, &sf, &ss);
        finit->body_scope = push_scope(P, SC_FUNC_BODY);
        finit->body_scope->fn = finit;
        finit->flags |= FI_SIMPLE_PARAMS;
        leave_function(P, sf, ss);
        enter_function(P, sinit, &sf, &ss);
        sinit->body_scope = push_scope(P, SC_FUNC_BODY);
        sinit->body_scope->fn = sinit;
        sinit->flags |= FI_SIMPLE_PARAMS;
        leave_function(P, sf, ss);
    }
    struct node* holder = mk(P, N_CLASS_STATIC_BLOCK, pos);   // a = instance init, b = static init, c = constructor
    if (!holder) return 0;
    holder->a = fnode;
    holder->b = snode;
    cls->d = holder;
    struct node** tail = &cls->c;
    int has_ctor = 0;
    while (TOK != T_RBRACE && !FAILED) {
        if (TOK == T_SEMI) { next(P); continue; }
        uint32_t mpos = TPOS;
        struct node* m = mk(P, N_MEMBER_DEF, mpos);
        if (!m) return 0;
        int is_static = 0, async = 0, gen = 0, kind = PK_METHOD;
        if (IS_CTX_RAW(static_)) {
            struct token t;
            int nt = peek(P, &t);
            if (nt != T_LPAREN && nt != T_ASSIGN && nt != T_SEMI && nt != T_RBRACE) {
                is_static = 1;
                next(P);
            }
        }
        if (is_static && TOK == T_LBRACE) {
            // static { ... } runs in the static initializer with its own var scope
            struct funcinfo* outer = P->F;
            struct scope* outer_s = P->S;
            int saved_sb = P->in_static_block, saved_field = P->in_class_field, saved_body = P->in_function_body;
            struct funcinfo* blk = new_funcinfo(P, FI_METHOD | FI_STATIC_BLOCK | FI_STRICT | FI_SIMPLE_PARAMS, mpos);
            if (!blk) return 0;
            struct node* bnode = mk(P, N_FUNC, mpos);
            if (!bnode) return 0;
            bnode->u.fn = blk; blk->node = bnode; blk->class_node = cls;
            P->F = blk;
            blk->param_scope = push_scope(P, SC_FUNC_PARAMS);
            blk->param_scope->fn = blk;
            blk->body_scope = push_scope(P, SC_STATIC_BLOCK);
            blk->body_scope->fn = blk;
            P->in_static_block = 1;
            P->in_class_field = 1;
            P->in_function_body = 0;
            struct label* sl = P->labels; int si = P->iteration_depth, ssw = P->switch_depth;
            P->labels = 0; P->iteration_depth = 0; P->switch_depth = 0;
            if (!expect(P, T_LBRACE)) return 0;
            struct node** bt = &blk->body;
            while (TOK != T_RBRACE && TOK != T_EOF && !FAILED) {
                struct node* s = parse_statement_list_item(P);
                if (!s) return 0;
                *bt = s;
                bt = &s->next;
            }
            blk->src_end = P->L.t.end;
            expect(P, T_RBRACE);
            P->labels = sl; P->iteration_depth = si; P->switch_depth = ssw;
            P->F = outer;
            P->S = outer_s;
            P->in_static_block = saved_sb;
            P->in_class_field = saved_field;
            P->in_function_body = saved_body;
            m->op = PK_METHOD;
            m->type = N_CLASS_STATIC_BLOCK;
            m->flags |= NF_STATIC;
            m->b = bnode;
            *tail = m;
            tail = &m->next;
            continue;
        }
        if (IS_CTX_RAW(async)) {
            struct token t;
            int nt = peek(P, &t);
            if (nt != T_LPAREN && nt != T_ASSIGN && nt != T_SEMI && nt != T_RBRACE && !t.nl_before) { async = 1; next(P); }
        }
        if (TOK == T_STAR) { gen = 1; next(P); }
        if (!async && !gen && (IS_CTX_RAW(get) || IS_CTX_RAW(set))) {
            struct token t;
            int nt = peek(P, &t);
            if (nt != T_LPAREN && nt != T_ASSIGN && nt != T_SEMI && nt != T_RBRACE && nt != T_STAR) {
                kind = IS_CTX(get) ? PK_GET : PK_SET;
                next(P);
            }
        }
        int computed;
        int key_tok = TOK;
        m->a = parse_property_name(P, &computed, 1);
        if (!m->a) return 0;
        if (computed) m->flags |= NF_COMPUTED;
        if (m->a->flags & NF_PRIVATE) m->flags |= NF_PRIVATE;
        if (is_static) m->flags |= NF_STATIC;
        struct str* kname = (!computed && m->a->type == N_STR) ? m->a->u.str : 0;
        int is_private = (m->flags & NF_PRIVATE) != 0;
        if (is_private && kname == P->J->A->constructor) { perr(P, mpos, "Classes may not have a private field named '#constructor'"); return 0; }
        if (TOK == T_LPAREN) {
            m->op = (uint8_t)kind;
            uint32_t ff = (async ? FI_ASYNC : 0) | (gen ? FI_GENERATOR : 0) | FI_STRICT;
            int is_ctor = !is_static && !is_private && kname == P->J->A->constructor && key_tok != T_LBRACK &&
                          m->a->type == N_STR;
            if (is_ctor) {
                if (kind != PK_METHOD || async || gen) { perr(P, mpos, "Class constructor may not be a%s", kind == PK_GET ? " getter" : kind == PK_SET ? " setter" : async ? "n async method" : " generator"); return 0; }
                if (has_ctor) { perr(P, mpos, "A class may only have one constructor"); return 0; }
                has_ctor = 1;
                ff |= FI_CLASS_CTOR | (cls->b ? FI_DERIVED : 0);
            }
            if (is_static && kname == P->J->A->prototype && !is_private) { perr(P, mpos, "Classes may not have a static property named 'prototype'"); return 0; }
            m->b = method_def(P, mpos, ff, m->a, kind);
            if (!m->b) return 0;
            m->b->u.fn->class_node = cls;
            if (is_ctor) { holder->c = m->b; m->op = PK_METHOD; m->flags |= NF_SHORTHAND; }   // NF_SHORTHAND marks the constructor
            if (is_private) {
                struct private_name* pn = private_find(&ctx, kname);
                int k2 = kind == PK_METHOD ? PK_METHOD : kind;
                if (pn) {
                    int ok = pn->is_static == is_static && ((pn->kind == PK_GET && k2 == PK_SET) || (pn->kind == PK_SET && k2 == PK_GET));
                    if (!ok) { perr(P, mpos, "Identifier '#%S' has already been declared", kname); return 0; }
                    pn->kind = PK_GET | 0x100;   // getter + setter pair
                } else {
                    pn = (struct private_name*)arena_alloc(P, sizeof *pn);
                    if (!pn) return 0;
                    pn->name = kname; pn->kind = k2; pn->is_static = is_static; pn->next = ctx.names; ctx.names = pn;
                }
            }
        } else {
            // field
            if (async || gen || kind != PK_METHOD) { perr(P, TPOS, "unexpected %s in class body", tok_name(TOK)); return 0; }
            if (!computed && kname == P->J->A->constructor && !is_private) { perr(P, mpos, "Classes may not have a field named 'constructor'"); return 0; }
            if (is_static && !computed && kname == P->J->A->prototype && !is_private) { perr(P, mpos, "Classes may not have a static property named 'prototype'"); return 0; }
            m->op = PK_FIELD;
            if (is_private) {
                if (private_find(&ctx, kname)) { perr(P, mpos, "Identifier '#%S' has already been declared", kname); return 0; }
                struct private_name* pn = (struct private_name*)arena_alloc(P, sizeof *pn);
                if (!pn) return 0;
                pn->name = kname; pn->kind = PK_FIELD; pn->is_static = is_static; pn->next = ctx.names; ctx.names = pn;
            }
            if (TOK == T_ASSIGN) {
                next(P);
                // initializer: parsed inside the (instance or static) initializer function
                struct funcinfo* outer = P->F;
                struct scope* outer_s = P->S;
                struct funcinfo* target = is_static ? sinit : finit;
                int saved_field = P->in_class_field, saved_body = P->in_function_body;
                P->F = target;
                P->S = target->body_scope;
                P->in_class_field = 1;
                P->in_function_body = 0;
                m->b = parse_assign(P);
                P->F = outer;
                P->S = outer_s;
                P->in_class_field = saved_field;
                P->in_function_body = saved_body;
                if (!m->b) return 0;
                if (P->cover_init) { perr(P, P->cover_init->pos, "Invalid shorthand property initializer"); return 0; }
            }
            if (!asi(P)) return 0;
            if (is_static) sinit->flags |= FI_CLASS_FIELDS; else finit->flags |= FI_CLASS_FIELDS;
        }
        *tail = m;
        tail = &m->next;
    }
    cls->end = P->L.t.end;
    expect(P, T_RBRACE);
    pop_scope(P);
    P->cls = ctx.parent;
    P->F->flags = saved_flags | (P->F->flags & ~FI_STRICT & ~saved_flags);
    if (!(saved_flags & FI_STRICT)) P->F->flags &= ~FI_STRICT;
    // private references: declared here, in an enclosing class, or an error
    cls->privs = ctx.names;
    for (struct pref* r = ctx.refs; r; r = r->next) {
        if (private_find(&ctx, r->name)) continue;
        if (ctx.parent) {
            struct pref* nr = (struct pref*)arena_alloc(P, sizeof(struct pref));
            if (!nr) return 0;
            *nr = *r;
            nr->next = ctx.parent->refs;
            ctx.parent->refs = nr;
        } else if (!(P->eval_env && ojs_eval_env_has_private(P->eval_env, r->name))) {
            perr(P, r->pos, "Private field '#%S' must be declared in an enclosing class", r->name);
            return 0;
        }
    }
    return cls;
}

// ---------------------------------------------------------------- modules

static void add_export_name(struct parser* P, struct str* name, uint32_t pos) {
    for (int i = 0; i < P->nexport_names; i++)
        if (P->export_names[i] == name) { perr(P, pos, "Duplicate export of '%S'", name); return; }
    if (P->nexport_names >= P->capexport_names) {
        int nc = P->capexport_names ? P->capexport_names * 2 : 16;
        struct str** t = (struct str**)arena_alloc(P, (size_t)nc * sizeof(struct str*));
        if (!t) { perr(P, pos, "out of memory"); return; }
        if (P->nexport_names) memcpy(t, P->export_names, (size_t)P->nexport_names * sizeof(struct str*));
        P->export_names = t;
        P->capexport_names = nc;
    }
    P->export_names[P->nexport_names++] = name;
}

// ModuleExportName: identifier name or string (must be well-formed UTF-16)
static struct str* module_export_name(struct parser* P, int* is_string) {
    *is_string = 0;
    if (TOK == T_STRING) {
        struct str* s = P->L.t.str;
        for (uint32_t i = 0; i < str_len(s); i++) {
            uint32_t c = str_at(s, i);
            if (c >= 0xD800 && c <= 0xDBFF) {
                if (i + 1 < str_len(s) && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF) { i++; continue; }
                perr(P, TPOS, "Invalid module export name: contains unpaired surrogate");
                return 0;
            }
            if (c >= 0xDC00 && c <= 0xDFFF) { perr(P, TPOS, "Invalid module export name: contains unpaired surrogate"); return 0; }
        }
        *is_string = 1;
        struct str* a = atom_str(P->J, s);
        next(P);
        return a;
    }
    if (TOK == T_IDENT || (TOK >= T_BREAK && TOK < T_COUNT)) {
        struct str* s = P->L.t.str ? P->L.t.str : atom_cstr(P->J, tok_name(TOK));
        next(P);
        return s;
    }
    perr(P, TPOS, "unexpected %s in module specifier", tok_name(TOK));
    return 0;
}

static struct str* module_specifier(struct parser* P) {
    if (TOK != T_STRING) { perr(P, TPOS, "unexpected %s, expected module specifier", tok_name(TOK)); return 0; }
    struct str* s = P->L.t.str;
    next(P);
    // import attributes: with { type: "json" }
    if (TOK == T_WITH) {
        next(P);
        if (!expect(P, T_LBRACE)) return 0;
        struct str* keys[32];
        int nkeys = 0;
        while (TOK != T_RBRACE && !FAILED) {
            int c;
            uint32_t kp = TPOS;
            if (TOK != T_IDENT && TOK != T_STRING && !(TOK >= T_BREAK && TOK < T_COUNT)) { perr(P, kp, "invalid import attribute key"); return 0; }
            struct node* key = parse_property_name(P, &c, 0);
            if (!key) return 0;
            for (int i = 0; i < nkeys; i++) if (keys[i] == key->u.str) { perr(P, kp, "Duplicate import attribute key"); return 0; }
            if (nkeys < 32) keys[nkeys++] = key->u.str;
            if (!expect(P, T_COLON)) return 0;
            if (TOK != T_STRING) { perr(P, TPOS, "import attribute value must be a string"); return 0; }
            next(P);
            if (TOK != T_RBRACE && !expect(P, T_COMMA)) return 0;
        }
        expect(P, T_RBRACE);
    }
    return s;
}

static struct node* parse_import(struct parser* P) {
    uint32_t pos = TPOS;
    next(P);   // import
    struct node* im = mk(P, N_IMPORT_DECL, pos);
    if (!im) return 0;
    if (TOK == T_STRING) {
        im->str2 = module_specifier(P);
        if (!im->str2 || !asi(P)) return 0;
        return im;
    }
    struct node** tail = &im->a;
    // default binding
    if (TOK == T_IDENT) {
        uint32_t ip = TPOS;
        struct str* local = binding_ident(P);
        if (!local) return 0;
        struct node* sp = mk(P, N_IMPORT_SPEC, ip);
        if (!sp) return 0;
        sp->u.str = local;
        sp->str2 = P->J->A->default_;
        *tail = sp;
        tail = &sp->next;
        if (TOK == T_COMMA) next(P);
        else goto from;
    }
    if (TOK == T_STAR) {
        next(P);
        if (!(IS_CTX(as) && !P->L.t.escaped)) { perr(P, TPOS, "expected 'as'"); return 0; }
        next(P);
        uint32_t ip = TPOS;
        struct str* local = binding_ident(P);
        if (!local) return 0;
        struct node* sp = mk(P, N_IMPORT_SPEC, ip);
        if (!sp) return 0;
        sp->u.str = local;
        sp->str2 = P->J->A->ns_star;
        *tail = sp;
        tail = &sp->next;
    } else if (TOK == T_LBRACE) {
        next(P);
        while (TOK != T_RBRACE && !FAILED) {
            uint32_t ip = TPOS;
            int is_str;
            int was_ident = TOK == T_IDENT;
            int was_kw = TOK >= T_BREAK && TOK < T_COUNT;
            struct str* imported = module_export_name(P, &is_str);
            if (!imported) return 0;
            struct str* local = imported;
            if (IS_CTX(as) && !P->L.t.escaped) {
                next(P);
                ip = TPOS;
                local = binding_ident(P);
                if (!local) return 0;
            } else {
                if (is_str || was_kw || !was_ident) { perr(P, ip, "unexpected string in import specifier"); return 0; }
                if (!ident_ok(P, local, ip, 1)) return 0;
                if (lex_is_keyword(local)) { perr(P, ip, "Unexpected reserved word"); return 0; }
            }
            struct node* sp = mk(P, N_IMPORT_SPEC, ip);
            if (!sp) return 0;
            sp->u.str = local;
            sp->str2 = imported;
            *tail = sp;
            tail = &sp->next;
            if (TOK != T_RBRACE && !expect(P, T_COMMA)) return 0;
        }
        expect(P, T_RBRACE);
    } else { perr(P, TPOS, "unexpected %s in import", tok_name(TOK)); return 0; }
from:
    if (!(IS_CTX(from) && !P->L.t.escaped)) { perr(P, TPOS, "expected 'from'"); return 0; }
    next(P);
    im->str2 = module_specifier(P);
    if (!im->str2 || !asi(P)) return 0;
    for (struct node* sp = im->a; sp; sp = sp->next) {
        declare_lex(P, sp->u.str, D_IMPORT, sp->pos, sp);
        struct decl* d = scope_find(P->S, sp->u.str);
        if (d) { d->import_name = sp->str2; d->import_from = im->str2; }
    }
    return im;
}

static struct node* parse_export(struct parser* P) {
    uint32_t pos = TPOS;
    next(P);   // export
    struct node* ex = mk(P, N_EXPORT, pos);
    if (!ex) return 0;
    if (TOK == T_STAR) {
        // export * from "m" / export * as ns from "m"
        next(P);
        ex->op = 3;
        if (IS_CTX(as) && !P->L.t.escaped) {
            next(P);
            int is_str;
            struct str* nm = module_export_name(P, &is_str);
            if (!nm) return 0;
            add_export_name(P, nm, pos);
            ex->u.str = nm;
        }
        if (!(IS_CTX(from) && !P->L.t.escaped)) { perr(P, TPOS, "expected 'from'"); return 0; }
        next(P);
        ex->str2 = module_specifier(P);
        if (!ex->str2 || !asi(P)) return 0;
        return ex;
    }
    if (TOK == T_DEFAULT) {
        next(P);
        add_export_name(P, P->J->A->default_, pos);
        ex->op = 2;
        if (TOK == T_FUNCTION || (IS_CTX_RAW(async) && 0)) {
            uint32_t fp = TPOS;
            next(P);
            uint32_t fl = 0;
            if (TOK == T_STAR) { fl |= FI_GENERATOR; next(P); }
            struct funcinfo* saved_top = (struct funcinfo*)P->top;
            P->top = P->F;   // allows a nameless declaration
            ex->a = parse_function(P, fp, fl, 1);
            P->top = saved_top;
            if (ex->a && !ex->a->u.fn->name) {
                ex->a->u.fn->name = P->J->A->default_;
                add_decl(P, P->S, P->J->A->star_default, D_FUNC, ex->a);
                ex->a->flags |= NF_SHORTHAND;   // declared under *default*
            }
            return ex->a ? ex : 0;
        }
        if (IS_CTX_RAW(async)) {
            struct token t;
            if (peek(P, &t) == T_FUNCTION && !t.nl_before) {
                uint32_t fp = TPOS;
                next(P); next(P);
                uint32_t fl = FI_ASYNC;
                if (TOK == T_STAR) { fl |= FI_GENERATOR; next(P); }
                struct funcinfo* saved_top = P->top;
                P->top = P->F;
                ex->a = parse_function(P, fp, fl, 1);
                P->top = saved_top;
                if (ex->a && !ex->a->u.fn->name) {
                    ex->a->u.fn->name = P->J->A->default_;
                    add_decl(P, P->S, P->J->A->star_default, D_FUNC, ex->a);
                    ex->a->flags |= NF_SHORTHAND;
                }
                return ex->a ? ex : 0;
            }
        }
        if (TOK == T_CLASS) {
            struct funcinfo* saved_top = P->top;
            P->top = P->F;
            ex->a = parse_class(P, 1);
            P->top = saved_top;
            if (ex->a && !ex->a->u.str) {
                add_decl(P, P->S, P->J->A->star_default, D_CLASS, ex->a);
                ex->a->flags |= NF_SHORTHAND;
            }
            return ex->a ? ex : 0;
        }
        // export default AssignmentExpression;
        ex->b = parse_assign(P);
        if (!ex->b) return 0;
        if (P->cover_init) { perr(P, P->cover_init->pos, "Invalid shorthand property initializer"); return 0; }
        add_decl(P, P->S, P->J->A->star_default, D_CONST, ex);
        if (!asi(P)) return 0;
        return ex;
    }
    if (TOK == T_LBRACE) {
        // export { a, b as c } [from "m"]
        next(P);
        ex->op = 1;
        struct node** tail = &ex->a;
        while (TOK != T_RBRACE && !FAILED) {
            uint32_t sp_pos = TPOS;
            int is_str;
            struct str* local = module_export_name(P, &is_str);
            if (!local) return 0;
            struct str* exported = local;
            if (IS_CTX(as) && !P->L.t.escaped) {
                next(P);
                int s2;
                exported = module_export_name(P, &s2);
                if (!exported) return 0;
            }
            struct node* sp = mk(P, N_EXPORT_SPEC, sp_pos);
            if (!sp) return 0;
            sp->u.str = local;
            sp->str2 = exported;
            sp->op = (uint8_t)is_str;
            add_export_name(P, exported, sp_pos);
            *tail = sp;
            tail = &sp->next;
            if (TOK != T_RBRACE && !expect(P, T_COMMA)) return 0;
        }
        expect(P, T_RBRACE);
        if (IS_CTX(from) && !P->L.t.escaped) {
            next(P);
            ex->str2 = module_specifier(P);
            if (!ex->str2) return 0;
        } else {
            // local exports must name local bindings (checked at the end), not strings / reserved words
            for (struct node* sp = ex->a; sp; sp = sp->next) {
                if (sp->op || lex_is_keyword(sp->u.str)) { perr(P, sp->pos, "Unexpected export name"); return 0; }
                struct node* u = mk(P, N_EXPORT_SPEC, sp->pos);
                if (!u) return 0;
                u->u.str = sp->u.str;
                u->next = P->unresolved_exports;
                P->unresolved_exports = u;
            }
        }
        if (!asi(P)) return 0;
        return ex;
    }
    // export declaration
    ex->op = 4;
    if (TOK == T_VAR || TOK == T_CONST || (IS_CTX(let) && !P->L.t.escaped)) {
        int k = TOK == T_VAR ? D_VAR : TOK == T_CONST ? D_CONST : D_LET;
        ex->a = parse_var_decl(P, k, 0);
        if (!ex->a || !asi(P)) return 0;
        // exported names: every bound identifier
        struct node* stack[64];
        int sp = 0;
        for (struct node* d = ex->a->a; d; d = d->next) if (sp < 64) stack[sp++] = d->a;
        while (sp) {
            struct node* n = stack[--sp];
            if (!n) continue;
            if (n->type == N_IDENT) add_export_name(P, n->u.str, n->pos);
            else if (n->type == N_ASSIGN_PAT || n->type == N_REST) { if (sp < 64) stack[sp++] = n->a; }
            else if (n->type == N_ARRAY_PAT) { for (struct node* e = n->a; e; e = e->next) if (sp < 64 && e->type != N_HOLE) stack[sp++] = e; }
            else if (n->type == N_OBJECT_PAT) { for (struct node* p = n->a; p; p = p->next) if (sp < 64) stack[sp++] = p->b; }
        }
        return ex;
    }
    if (TOK == T_FUNCTION || TOK == T_CLASS || (IS_CTX_RAW(async))) {
        ex->a = parse_statement_list_item(P);
        if (!ex->a) return 0;
        struct str* nm = ex->a->type == N_FUNC ? ex->a->u.fn->name : ex->a->type == N_CLASS ? ex->a->u.str : 0;
        if (!nm) { perr(P, pos, "unexpected export"); return 0; }
        add_export_name(P, nm, pos);
        return ex;
    }
    perr(P, TPOS, "unexpected %s after export", tok_name(TOK));
    return 0;
}

// ---------------------------------------------------------------- entry point

int parse_program(ojs* J, struct str* src, const struct parse_opts* o, struct parser* P) {
    memset(P, 0, sizeof *P);
    P->J = J;
    J->gc_disabled++;
    P->gc_held = 1;
    P->filename = o->filename;
    P->is_module = o->kind == PARSE_MODULE;
    P->eval_env = o->eval_env;
    lex_init(&P->L, J, src, P->is_module);
    uint32_t fl = o->kind == PARSE_MODULE ? (FI_MODULE | FI_STRICT | FI_ASYNC) :
                  o->kind == PARSE_EVAL ? FI_EVAL : FI_SCRIPT;
    if (o->strict) fl |= FI_STRICT;
    if (o->kind == PARSE_EVAL) {
        if (o->allow_new_target) fl |= FI_USES_NEW_TARGET;
        if (o->allow_super_prop) fl |= FI_USES_SUPER_PROP;
        if (o->allow_super_call) fl |= FI_USES_SUPER_CALL;
    }
    struct funcinfo* top = (struct funcinfo*)arena_alloc(P, sizeof(struct funcinfo));
    if (!top) { perr(P, 0, "out of memory"); goto fail; }
    top->flags = fl;
    P->F = top;
    P->top = 0;
    top->param_scope = push_scope(P, o->kind == PARSE_MODULE ? SC_MODULE : o->kind == PARSE_EVAL ? SC_EVAL : SC_SCRIPT);
    if (!top->param_scope) goto fail;
    top->param_scope->fn = top;
    top->body_scope = top->param_scope;
    P->allow_in = 1;
    P->in_class_field = o->in_class_field;
    P->in_function_body = o->kind == PARSE_EVAL ? 0 : 0;
    if (o->kind == PARSE_MODULE) top->flags &= ~FI_ASYNC, top->flags |= FI_MODULE;
    next(P);
    struct node* prog = mk(P, N_PROGRAM, 0);
    if (!prog) goto fail;
    P->program = prog;
    top->node = prog;
    prog->u.fn = top;
    struct node** tail = &prog->a;
    int in_prologue = 1;
    while (TOK != T_EOF && !FAILED) {
        if (in_prologue && TOK == T_STRING) {
            uint32_t sp = TPOS, se = P->L.t.end;
            struct str* val = P->L.t.str;
            struct token t;
            int nt = peek(P, &t);
            if (nt == T_SEMI || nt == T_RBRACE || nt == T_EOF || t.nl_before) {
                if (se - sp == 12 && val && str_eq_ascii(val, "use strict")) { top->flags |= FI_STRICT; P->L.strict = 1; }
            } else in_prologue = 0;
        } else in_prologue = 0;
        struct node* s;
        if (P->is_module && TOK == T_IMPORT) {
            struct token t;
            int nt = peek(P, &t);
            s = (nt == T_LPAREN || nt == T_DOT) ? parse_statement_list_item(P) : parse_import(P);
        } else if (P->is_module && TOK == T_EXPORT) s = parse_export(P);
        else s = parse_statement_list_item(P);
        if (!s) break;
        *tail = s;
        tail = &s->next;
    }
    if (TOK == T_ERROR && !FAILED) perr(P, P->L.err_pos, "%s", P->L.err);
    top->src_end = P->L.len;
    if (!FAILED && P->is_module) {
        for (struct node* u = P->unresolved_exports; u; u = u->next)
            if (!scope_find(top->param_scope, u->u.str)) { perr(P, u->pos, "Export '%S' is not defined in module", u->u.str); break; }
    }
    if (FAILED) goto fail;
    P->top = top;
    return 0;
fail:
    {
        int line, col;
        lex_line_col(&P->L, P->err_pos, &line, &col);
        throw_syntax(J, "%s (%s:%d:%d)", P->msg[0] ? P->msg : "syntax error", o->filename ? o->filename : "<input>", line, col);
    }
    return -1;
}
