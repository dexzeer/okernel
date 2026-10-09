// parse.c — ECMAScript parser: tokens -> AST with scopes and early errors.
//
// Recursive descent; binary operators by precedence climbing. Arrow
// functions and destructuring assignment use cover grammars: a
// parenthesized expression list / object or array literal is parsed as
// an expression first and reinterpreted (with validation) when `=>` or
// `=` follows. A '/' is lexed as division and rescanned as a regular
// expression where an operand is expected.

#include "parse.h"
#include "unicode.h"
#include "atoms.h"

int regexp_check_syntax(ojs* J, struct str* body, struct str* flags, char* err, int errcap);   // re.c

// ---------------------------------------------------------------- arena

struct arena_chunk { struct arena_chunk* next; size_t used, cap; uint8_t data[]; };

void* arena_alloc(struct parser* P, size_t n) {
    n = (n + 7) & ~(size_t)7;
    struct arena_chunk* c = P->arena;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 - sizeof(struct arena_chunk) ? n : 65536 - sizeof(struct arena_chunk);
        struct arena_chunk* nc = (struct arena_chunk*)ojs_sys_malloc(sizeof(struct arena_chunk) + cap);
        if (!nc) return 0;
        nc->next = c;
        nc->used = 0;
        nc->cap = cap;
        P->arena = nc;
        c = nc;
    }
    void* p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

void parse_free(struct parser* P) {
    if (P->gc_held && P->J) { P->J->gc_disabled--; P->gc_held = 0; }
    for (struct arena_chunk* c = P->arena; c;) {
        struct arena_chunk* n = c->next;
        ojs_sys_free(c);
        c = n;
    }
    P->arena = 0;
    lex_free(&P->L);
}

// ---------------------------------------------------------------- errors

static void perr(struct parser* P, uint32_t pos, const char* fmt, ...) {
    if (P->failed) return;
    P->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    ojs_vsnprintf(P->msg, sizeof P->msg, fmt, ap);
    va_end(ap);
    P->err_pos = pos;
}

#define FAILED (P->failed)
#define TOK (P->L.t.type)
#define TPOS (P->L.t.start)

static void next(struct parser* P) {
    if (P->failed) return;
    P->prev_end = P->L.t.end;
    if (lex_next(&P->L, 0) == T_ERROR) perr(P, P->L.err_pos, "%s", P->L.err);
}

static int is_ident_named(struct parser* P, struct str* a) {
    return TOK == T_IDENT && P->L.t.str == a;
}
#define IS_CTX(id) is_ident_named(P, P->J->A->id)
#define IS_CTX_RAW(id) (IS_CTX(id) && !P->L.t.escaped)

static int expect(struct parser* P, int t) {
    if (TOK != t) {
        if (TOK == T_ERROR) return 0;
        perr(P, TPOS, "unexpected %s, expected %s", tok_name(TOK), tok_name(t));
        return 0;
    }
    next(P);
    return 1;
}

static int asi(struct parser* P) {   // consume ';' or insert one
    if (TOK == T_SEMI) { next(P); return 1; }
    if (TOK == T_RBRACE || TOK == T_EOF || P->L.t.nl_before) return 1;
    perr(P, TPOS, "unexpected %s (missing semicolon?)", tok_name(TOK));
    return 0;
}

// lookahead of one token without consuming (regex not allowed)
static int peek(struct parser* P, struct token* out) {
    struct lexer save = P->L;
    lex_next(&P->L, 0);
    *out = P->L.t;
    int t = P->L.t.type;
    // restore (the line table may have been allocated meanwhile: keep it)
    uint32_t* ls = P->L.line_starts;
    uint32_t nl = P->L.nlines;
    P->L = save;
    P->L.line_starts = ls;
    P->L.nlines = nl;
    return t;
}

// ---------------------------------------------------------------- nodes

static struct node* mk(struct parser* P, int type, uint32_t pos) {
    struct node* n = (struct node*)arena_alloc(P, sizeof(struct node));
    if (!n) { perr(P, pos, "out of memory"); return 0; }
    n->type = (uint8_t)type;
    n->pos = pos;
    n->scope = P->S;
    return n;
}

static int strict(struct parser* P) { return (P->F->flags & FI_STRICT) != 0; }

// ---------------------------------------------------------------- scopes & declarations

static struct scope* push_scope(struct parser* P, int kind) {
    struct scope* s = (struct scope*)arena_alloc(P, sizeof(struct scope));
    if (!s) { perr(P, TPOS, "out of memory"); return 0; }
    s->kind = (uint8_t)kind;
    s->parent = P->S;
    s->fn = P->F;
    P->S = s;
    return s;
}

static void pop_scope(struct parser* P) { if (P->S) P->S = P->S->parent; }

static void scope_index(struct scope* s, struct parser* P) {
    int hs = 32;
    while (hs < s->ndecls * 2) hs *= 2;
    struct decl** h = (struct decl**)arena_alloc(P, (size_t)hs * sizeof(struct decl*));
    if (!h) return;
    for (struct decl* d = s->decls; d; d = d->next) {
        uint32_t i = ((uint32_t)(uintptr_t)d->name * 2654435761u >> 5) & (uint32_t)(hs - 1);
        while (h[i]) i = (i + 1) & (uint32_t)(hs - 1);
        h[i] = d;
    }
    s->hash = h;
    s->hsize = hs;
}

struct decl* scope_find(struct scope* s, struct str* name) {
    if (s->hash) {
        uint32_t i = ((uint32_t)(uintptr_t)name * 2654435761u >> 5) & (uint32_t)(s->hsize - 1);
        for (;;) {
            struct decl* d = s->hash[i];
            if (!d) return 0;
            if (d->name == name) return d;
            i = (i + 1) & (uint32_t)(s->hsize - 1);
        }
    }
    for (struct decl* d = s->decls; d; d = d->next) if (d->name == name) return d;
    return 0;
}

static struct decl* add_decl(struct parser* P, struct scope* s, struct str* name, int kind, struct node* n) {
    struct decl* d = (struct decl*)arena_alloc(P, sizeof(struct decl));
    if (!d) { perr(P, TPOS, "out of memory"); return 0; }
    d->name = name;
    d->kind = (uint8_t)kind;
    d->node = n;
    d->scope = s;
    d->slot = -1;
    if (kind == D_LET || kind == D_CONST || kind == D_CLASS) d->flags |= DF_TDZ;
    if (s->decls_tail) s->decls_tail->next = d; else s->decls = d;
    s->decls_tail = d;
    s->ndecls++;
    if (s->hash) {
        if (s->ndecls * 2 > s->hsize) scope_index(s, P);
        else {
            uint32_t i = ((uint32_t)(uintptr_t)name * 2654435761u >> 5) & (uint32_t)(s->hsize - 1);
            while (s->hash[i]) i = (i + 1) & (uint32_t)(s->hsize - 1);
            s->hash[i] = d;
        }
    } else if (s->ndecls > 12) scope_index(s, P);
    return d;
}

static int has_varname(struct scope* s, struct str* name) {
    for (int i = 0; i < s->nvarnames; i++) if (s->varnames[i] == name) return 1;
    return 0;
}

static void add_varname(struct parser* P, struct scope* s, struct str* name) {
    if (has_varname(s, name)) return;
    if (s->nvarnames >= s->capvarnames) {
        int nc = s->capvarnames ? s->capvarnames * 2 : 8;
        struct str** t = (struct str**)arena_alloc(P, (size_t)nc * sizeof(struct str*));
        if (!t) { perr(P, TPOS, "out of memory"); return; }
        if (s->nvarnames) memcpy(t, s->varnames, (size_t)s->nvarnames * sizeof(struct str*));
        s->varnames = t;
        s->capvarnames = nc;
    }
    s->varnames[s->nvarnames++] = name;
}

static int is_lexical(int k) { return k == D_LET || k == D_CONST || k == D_CLASS || k == D_IMPORT || k == D_FUNCNAME; }

// the scope that holds `var`s of the current function (or script / eval)
static struct scope* var_scope(struct parser* P) { return P->F->body_scope; }

static void declare_var(struct parser* P, struct str* name, uint32_t pos, struct node* n) {
    // conflicts with lexical declarations in every scope up to the var scope
    struct scope* vs = var_scope(P);
    for (struct scope* s = P->S; s; s = s->parent) {
        struct decl* d = scope_find(s, name);
        if (d && (is_lexical(d->kind) || (d->kind == D_FUNC && (d->flags & DF_LEX_FUNC)) ||
                  (d->kind == D_CATCH && d->node && d->node->type != N_IDENT))) {
            perr(P, pos, "Identifier '%S' has already been declared", name);
            return;
        }
        add_varname(P, s, name);
        if (s == vs) break;
    }
    struct decl* d = scope_find(vs, name);
    if (!d) d = add_decl(P, vs, name, D_VAR, n);
    // a var with the same name as a parameter shares the parameter binding
    (void)d;
}

static void declare_lex(struct parser* P, struct str* name, int kind, uint32_t pos, struct node* n) {
    struct scope* s = P->S;
    if ((kind == D_LET || kind == D_CONST) && name == P->J->A->let) {
        perr(P, pos, "let is disallowed as a lexically bound name");
        return;
    }
    struct decl* d = scope_find(s, name);
    if (d) {
        // sloppy-mode block functions may redeclare each other (Annex B)
        int both_fn = d->kind == D_FUNC && kind == D_FUNC && !strict(P) && (d->flags & DF_LEX_FUNC) &&
                      d->node && d->node->type == N_FUNC && !(d->node->u.fn->flags & (FI_ASYNC | FI_GENERATOR)) &&
                      n && n->type == N_FUNC && !(n->u.fn->flags & (FI_ASYNC | FI_GENERATOR));
        if (!both_fn) { perr(P, pos, "Identifier '%S' has already been declared", name); return; }
    }
    if (has_varname(s, name)) { perr(P, pos, "Identifier '%S' has already been declared", name); return; }
    // parameters conflict with lexical declarations of the function body
    if (s->kind == SC_FUNC_BODY && s->parent && s->parent->kind == SC_FUNC_PARAMS && s->parent != s) {
        struct decl* p = scope_find(s->parent, name);
        if (p && p->kind == D_PARAM) { perr(P, pos, "Identifier '%S' has already been declared", name); return; }
    }
    if (s->kind == SC_FUNC_PARAMS || (s == var_scope(P) && P->F->param_scope == s)) {
        struct decl* p = scope_find(s, name);
        if (p && p->kind == D_PARAM) { perr(P, pos, "Identifier '%S' has already been declared", name); return; }
    }
    // the catch parameter scope: lexical redeclaration in the catch block
    if (s->kind == SC_BLOCK && s->parent && s->parent->kind == SC_CATCH && s->owner && s->owner->op == 1) {
        struct decl* p = scope_find(s->parent, name);
        if (p) { perr(P, pos, "Identifier '%S' has already been declared", name); return; }
    }
    d = add_decl(P, s, name, kind, n);
    if (d && kind == D_FUNC) d->flags |= DF_LEX_FUNC;
}

// ---------------------------------------------------------------- identifiers

static int is_strict_reserved(struct parser* P, struct str* s) {
    static const char* const R[] = { "implements", "interface", "let", "package", "private", "protected",
                                     "public", "static", "yield", 0 };
    (void)P;
    for (int i = 0; R[i]; i++) if (str_eq_ascii(s, R[i])) return 1;
    return 0;
}

// can the current token be a binding/reference identifier here?
static int ident_ok(struct parser* P, struct str* s, uint32_t pos, int binding) {
    ojs* J = P->J;
    if (lex_is_keyword(s)) { perr(P, pos, "Keyword must not contain escaped characters"); return 0; }
    if (strict(P) && is_strict_reserved(P, s)) { perr(P, pos, "Unexpected strict mode reserved word '%S'", s); return 0; }
    if (s == J->A->yield && ((P->F->flags & FI_GENERATOR) || strict(P))) { perr(P, pos, "Unexpected identifier 'yield'"); return 0; }
    if (s == J->A->await && ((P->F->flags & FI_ASYNC) || P->is_module || P->in_static_block)) {
        perr(P, pos, "Unexpected reserved word 'await'");
        return 0;
    }
    if (binding && strict(P) && (s == J->A->eval || s == J->A->arguments)) {
        perr(P, pos, "Unexpected eval or arguments in strict mode");
        return 0;
    }
    if (P->in_class_field && s == J->A->arguments && !binding) {
        perr(P, pos, "'arguments' is not allowed in class field initializer or static initialization block");
        return 0;
    }
    return 1;
}

// current token as an identifier (consumes it); NULL on error
static struct str* binding_ident(struct parser* P) {
    if (TOK != T_IDENT) {
        if (TOK >= T_BREAK && TOK < T_COUNT) perr(P, TPOS, "Unexpected reserved word '%s'", tok_name(TOK));
        else perr(P, TPOS, "unexpected %s, expected identifier", tok_name(TOK));
        return 0;
    }
    struct str* s = P->L.t.str;
    if (!ident_ok(P, s, TPOS, 1)) return 0;
    next(P);
    return s;
}

// ---------------------------------------------------------------- forward declarations

static struct node* parse_expression(struct parser* P);
static struct node* parse_assign(struct parser* P);
static struct node* parse_unary(struct parser* P);
static struct node* parse_lhs(struct parser* P, int allow_call);
static struct node* parse_statement(struct parser* P, int ctx);
static struct node* parse_statement_list_item(struct parser* P);
static struct node* parse_function(struct parser* P, uint32_t pos, uint32_t flags, int is_decl);
static struct node* parse_class(struct parser* P, int is_decl);
static struct node* parse_binding_target(struct parser* P, int kind);
static struct node* parse_template(struct parser* P, struct node* tag);
static int to_assign_target(struct parser* P, struct node* n, int simple_only);
static struct node* parse_arrow_body(struct parser* P, struct funcinfo* fi);
static struct funcinfo* new_funcinfo(struct parser* P, uint32_t flags, uint32_t pos);
static void enter_function(struct parser* P, struct funcinfo* fi, struct funcinfo** saved_f, struct scope** saved_s);
static void leave_function(struct parser* P, struct funcinfo* saved_f, struct scope* saved_s);
static void declare_params(struct parser* P, struct funcinfo* fi);
static void finish_function_checks(struct parser* P, struct funcinfo* fi, uint32_t pos);

// ---------------------------------------------------------------- literals

static struct node* parse_object_literal(struct parser* P);
static struct node* parse_array_literal(struct parser* P);

static struct node* parse_property_name(struct parser* P, int* computed, int allow_private) {
    uint32_t pos = TPOS;
    struct node* k = 0;
    *computed = 0;
    switch (TOK) {
    case T_STRING:
        if (P->L.t.bad_escape && strict(P)) { perr(P, pos, "Octal escape sequences are not allowed in strict mode"); return 0; }
        k = mk(P, N_STR, pos);
        if (k) { k->u.str = atom_str(P->J, P->L.t.str); if (!k->u.str) { perr(P, pos, "out of memory"); return 0; } }
        next(P);
        return k;
    case T_NUM:
        if (P->L.t.legacy_octal && strict(P)) { perr(P, pos, "Octal literals are not allowed in strict mode"); return 0; }
        k = mk(P, N_NUM, pos); if (k) k->u.num = P->L.t.num; next(P); return k;
    case T_BIGINT:
        k = mk(P, N_BIGINT, pos); if (k) k->u.str = P->L.t.str; next(P); return k;
    case T_LBRACK: {
        next(P);
        int saved = P->allow_in;
        P->allow_in = 1;
        k = parse_assign(P);
        P->allow_in = saved;
        expect(P, T_RBRACK);
        *computed = 1;
        return k;
    }
    case T_PRIVATE:
        if (!allow_private) { perr(P, pos, "Unexpected private name"); return 0; }
        k = mk(P, N_STR, pos);
        if (k) { k->u.str = P->L.t.str; k->flags |= NF_PRIVATE; }
        next(P);
        return k;
    default:
        if (TOK == T_IDENT || (TOK >= T_BREAK && TOK < T_COUNT)) {
            k = mk(P, N_STR, pos);
            if (k) k->u.str = P->L.t.str ? P->L.t.str : atom_cstr(P->J, tok_name(TOK));
            next(P);
            return k;
        }
        perr(P, pos, "unexpected %s in property name", tok_name(TOK));
        return 0;
    }
}

static int key_is(struct node* k, struct str* name) {
    return k && k->type == N_STR && !(k->flags & NF_PRIVATE) && k->u.str == name;
}

static struct node* method_def(struct parser* P, uint32_t pos, uint32_t fflags, struct node* key, int kind) {
    struct node* fn = parse_function(P, pos, fflags | FI_METHOD | (kind == PK_GET ? FI_GETTER : kind == PK_SET ? FI_SETTER : 0), 0);
    if (!fn) return 0;
    struct funcinfo* fi = fn->u.fn;
    if (kind == PK_GET && fi->nparams) { perr(P, pos, "Getter must not have any formal parameters"); return 0; }
    if (kind == PK_SET && (fi->nparams != 1 || (fi->params && fi->params->type == N_REST))) {
        perr(P, pos, "Setter must have exactly one formal parameter");
        return 0;
    }
    if (key && key->type == N_STR && !(key->flags & NF_COMPUTED)) fi->name = key->u.str;
    return fn;
}

static struct node* parse_object_literal(struct parser* P) {
    struct node* obj = mk(P, N_OBJECT, TPOS);
    if (!obj) return 0;
    next(P);   // {
    struct node** tail = &obj->a;
    int has_proto = 0;
    int saved_in = P->allow_in;
    P->allow_in = 1;
    while (TOK != T_RBRACE && !FAILED) {
        uint32_t pos = TPOS;
        struct node* p = mk(P, N_PROP, pos);
        if (!p) return 0;
        if (TOK == T_ELLIPSIS) {
            next(P);
            p->op = PK_SPREAD;
            p->b = parse_assign(P);
        } else {
            int async = 0, gen = 0, kind = PK_INIT;
            // modifiers: async, get, set, *
            if (IS_CTX(async) && !P->L.t.escaped) {
                struct token t;
                int nt = peek(P, &t);
                if (nt != T_COMMA && nt != T_COLON && nt != T_LPAREN && nt != T_RBRACE && nt != T_ASSIGN && !t.nl_before) {
                    async = 1;
                    next(P);
                }
            }
            if (TOK == T_STAR) { gen = 1; next(P); }
            if (!async && !gen && (IS_CTX(get) || IS_CTX(set)) && !P->L.t.escaped) {
                struct token t;
                int nt = peek(P, &t);
                if (nt != T_COMMA && nt != T_COLON && nt != T_LPAREN && nt != T_RBRACE && nt != T_ASSIGN) {
                    kind = IS_CTX(get) ? PK_GET : PK_SET;
                    next(P);
                }
            }
            int computed;
            int key_tok = TOK;
            int key_escaped = P->L.t.escaped;
            struct str* key_name = (TOK == T_IDENT) ? P->L.t.str : 0;
            p->a = parse_property_name(P, &computed, 0);
            if (!p->a) return 0;
            if (computed) p->flags |= NF_COMPUTED;
            if (async || gen || kind != PK_INIT || TOK == T_LPAREN) {
                uint32_t ff = (async ? FI_ASYNC : 0) | (gen ? FI_GENERATOR : 0);
                p->op = kind == PK_INIT ? PK_METHOD : (uint8_t)kind;
                p->b = method_def(P, pos, ff, p->a, kind == PK_INIT ? PK_METHOD : kind);
            } else if (TOK == T_COLON) {
                next(P);
                p->b = parse_assign(P);
                if (!computed && key_is(p->a, P->J->A->proto) && key_tok != T_LBRACK) {
                    if (has_proto) {
                        // allowed only if this literal becomes a pattern: remember
                        if (!P->cover_init) P->cover_init = p;
                        p->flags |= NF_COVER_INIT;
                        p->op = PK_INIT;
                    } else { has_proto = 1; p->op = PK_PROTO; }
                }
            } else if (key_tok == T_IDENT || (key_tok >= T_BREAK && key_tok < T_COUNT)) {
                // shorthand { a } or { a = init } (cover)
                if (key_tok != T_IDENT) { perr(P, pos, "Unexpected reserved word '%s'", tok_name(key_tok)); return 0; }
                if (key_escaped && lex_is_keyword(key_name)) { perr(P, pos, "Keyword must not contain escaped characters"); return 0; }
                if (!ident_ok(P, key_name, pos, 0)) return 0;
                struct node* id = mk(P, N_IDENT, pos);
                if (!id) return 0;
                id->u.str = key_name;
                p->flags |= NF_SHORTHAND;
                if (TOK == T_ASSIGN) {
                    next(P);
                    struct node* ap = mk(P, N_ASSIGN_PAT, pos);
                    if (!ap) return 0;
                    ap->a = id;
                    ap->b = parse_assign(P);
                    p->b = ap;
                    p->flags |= NF_COVER_INIT;
                    if (!P->cover_init) P->cover_init = p;
                } else p->b = id;
            } else {
                perr(P, TPOS, "unexpected %s in object literal", tok_name(TOK));
                return 0;
            }
        }
        *tail = p;
        tail = &p->next;
        if (TOK != T_RBRACE) {
            if (!expect(P, T_COMMA)) return 0;
            if (p->op == PK_SPREAD && TOK == T_RBRACE) p->flags |= NF_TAIL;   // `...x,}` (pattern error)
        }
    }
    obj->end = P->L.t.end;
    P->allow_in = saved_in;
    expect(P, T_RBRACE);
    return obj;
}

static struct node* parse_array_literal(struct parser* P) {
    struct node* arr = mk(P, N_ARRAY, TPOS);
    if (!arr) return 0;
    next(P);   // [
    struct node** tail = &arr->a;
    int saved_in = P->allow_in;
    P->allow_in = 1;
    while (TOK != T_RBRACK && !FAILED) {
        struct node* e;
        if (TOK == T_COMMA) {
            e = mk(P, N_HOLE, TPOS);
            next(P);
            *tail = e;
            tail = &e->next;
            continue;
        }
        if (TOK == T_ELLIPSIS) {
            uint32_t pos = TPOS;
            next(P);
            e = mk(P, N_SPREAD, pos);
            if (e) e->a = parse_assign(P);
            arr->flags |= NF_HAS_SPREAD;
        } else e = parse_assign(P);
        if (!e) return 0;
        *tail = e;
        tail = &e->next;
        if (TOK != T_RBRACK) {
            if (!expect(P, T_COMMA)) return 0;
            if (e->type == N_SPREAD && TOK == T_RBRACK) e->flags |= NF_SHORTHAND;   // trailing comma after spread (pattern error)
        }
    }
    P->allow_in = saved_in;
    expect(P, T_RBRACK);
    return arr;
}

static struct node* parse_regexp(struct parser* P) {
    uint32_t pos = TPOS;
    lex_rescan_regex(&P->L);
    if (P->L.t.type == T_ERROR) { perr(P, P->L.err_pos, "%s", P->L.err); return 0; }
    struct node* n = mk(P, N_REGEXP, pos);
    if (!n) return 0;
    n->u.str = P->L.t.raw;
    n->str2 = P->L.t.flags;
    char err[200];
    if (regexp_check_syntax(P->J, n->u.str, n->str2, err, sizeof err) < 0) {
        perr(P, pos, "Invalid regular expression: /%S/%S: %s", n->u.str, n->str2, err);
        return 0;
    }
    next(P);
    return n;
}

// template literal starting at the current T_TEMPLATE token. tag: the
// tag expression (tagged template: invalid escapes allowed) or NULL
static struct node* parse_template(struct parser* P, struct node* tag) {
    uint32_t pos = TPOS;
    struct node* t = mk(P, tag ? N_TAGGED : N_TEMPLATE, pos);
    if (!t) return 0;
    t->a = tag;
    struct node** qtail = &t->b;   // quasis (N_STR: u.str cooked or NULL, str2 raw)
    struct node** etail = &t->c;   // substitutions
    for (;;) {
        if (TOK != T_TEMPLATE) { perr(P, TPOS, "unexpected %s in template literal", tok_name(TOK)); return 0; }
        if (!tag && P->L.t.bad_escape) { perr(P, TPOS, "Invalid escape sequence in template"); return 0; }
        struct node* q = mk(P, N_STR, TPOS);
        if (!q) return 0;
        q->u.str = P->L.t.str;
        q->str2 = P->L.t.raw;
        *qtail = q;
        qtail = &q->next;
        if (P->L.t.template_tail) { t->end = P->L.t.end; next(P); break; }
        next(P);
        int saved = P->allow_in;
        P->allow_in = 1;
        struct node* e = parse_expression(P);
        P->allow_in = saved;
        if (!e) return 0;
        *etail = e;
        etail = &e->next;
        if (TOK != T_RBRACE) { perr(P, TPOS, "unexpected %s in template literal", tok_name(TOK)); return 0; }
        if (lex_template_continue(&P->L) == T_ERROR) { perr(P, P->L.err_pos, "%s", P->L.err); return 0; }
    }
    return t;
}

// ---------------------------------------------------------------- primary expressions

static struct node* parse_paren_or_arrow(struct parser* P, int is_async, uint32_t async_pos);

static struct node* make_ident(struct parser* P, struct str* s, uint32_t pos) {
    struct node* n = mk(P, N_IDENT, pos);
    if (n) n->u.str = s;
    return n;
}

static struct node* arrow_single_param(struct parser* P, struct node* param, uint32_t pos, int is_async);

static struct node* parse_primary(struct parser* P) {
    uint32_t pos = TPOS;
    ojs* J = P->J;
    struct node* n;
    switch (TOK) {
    case T_THIS:
        n = mk(P, N_THIS, pos);
        // an arrow's this is its enclosing function's: that one needs the binding
        for (struct funcinfo* f = P->F; f; f = f->parent) {
            f->flags |= FI_USES_THIS;
            if (!(f->flags & FI_ARROW)) break;
        }
        next(P);
        return n;
    case T_NUM:
        if (P->L.t.legacy_octal && strict(P)) { perr(P, pos, "Octal literals are not allowed in strict mode"); return 0; }
        n = mk(P, N_NUM, pos);
        if (n) n->u.num = P->L.t.num;
        next(P);
        return n;
    case T_BIGINT:
        n = mk(P, N_BIGINT, pos);
        if (n) n->u.str = P->L.t.str;
        next(P);
        return n;
    case T_STRING:
        if (P->L.t.bad_escape && strict(P)) { perr(P, pos, "Octal escape sequences are not allowed in strict mode"); return 0; }
        n = mk(P, N_STR, pos);
        if (n) n->u.str = P->L.t.str;
        next(P);
        return n;
    case T_TEMPLATE: return parse_template(P, 0);
    case T_SLASH: case T_SLASH_ASSIGN: return parse_regexp(P);
    case T_NULL: n = mk(P, N_NULL, pos); next(P); return n;
    case T_TRUE: n = mk(P, N_TRUE, pos); next(P); return n;
    case T_FALSE: n = mk(P, N_FALSE, pos); next(P); return n;
    case T_LBRACE: return parse_object_literal(P);
    case T_LBRACK: return parse_array_literal(P);
    case T_LPAREN: return parse_paren_or_arrow(P, 0, 0);
    case T_FUNCTION: {
        next(P);
        uint32_t fl = 0;
        if (TOK == T_STAR) { fl |= FI_GENERATOR; next(P); }
        return parse_function(P, pos, fl, 0);
    }
    case T_CLASS: return parse_class(P, 0);
    case T_NEW: {
        next(P);
        if (TOK == T_DOT) {
            next(P);
            if (!(IS_CTX(target) && !P->L.t.escaped)) { perr(P, TPOS, "unexpected %s after new.", tok_name(TOK)); return 0; }
            // allowed in non-arrow functions (or arrows/eval inside them) and class field initializers
            int ok = 0;
            for (struct funcinfo* f = P->F; f; f = f->parent) {
                if (f->flags & (FI_SCRIPT | FI_MODULE)) break;
                if (f->flags & FI_EVAL) { ok = f->flags & FI_USES_NEW_TARGET ? 1 : 0; break; }
                if (!(f->flags & FI_ARROW)) { ok = 1; break; }
            }
            if (!ok) { perr(P, pos, "new.target expression is not allowed here"); return 0; }
            for (struct funcinfo* f = P->F; f && (f->flags & FI_ARROW); f = f->parent) f->flags |= FI_USES_THIS;
            P->F->flags |= FI_USES_THIS | FI_USES_NEW_TARGET;
            next(P);
            return mk(P, N_NEW_TARGET, pos);
        }
        if (TOK == T_IMPORT) {
            struct token t;
            if (peek(P, &t) != T_DOT) { perr(P, TPOS, "Cannot use new with import"); return 0; }
        }
        n = mk(P, N_NEW, pos);
        if (!n) return 0;
        n->a = parse_lhs(P, 0);   // member expression (no call)
        if (!n->a) return 0;
        if (n->a->type == N_OPTCHAIN || (n->a->flags & NF_OPTIONAL)) { perr(P, pos, "Invalid optional chain from new expression"); return 0; }
        if (TOK == T_LPAREN) {
            // arguments
            next(P);
            struct node** tail = &n->b;
            while (TOK != T_RPAREN && !FAILED) {
                struct node* a;
                if (TOK == T_ELLIPSIS) {
                    uint32_t sp = TPOS;
                    next(P);
                    a = mk(P, N_SPREAD, sp);
                    if (a) a->a = parse_assign(P);
                    n->flags |= NF_HAS_SPREAD;
                } else a = parse_assign(P);
                if (!a) return 0;
                *tail = a;
                tail = &a->next;
                if (TOK != T_RPAREN && !expect(P, T_COMMA)) return 0;
            }
            expect(P, T_RPAREN);
        }
        return n;
    }
    case T_SUPER: {
        next(P);
        n = mk(P, N_SUPER, pos);
        if (TOK == T_LPAREN) {
            // super(): derived class constructors (and arrows / eval inside them)
            struct funcinfo* f = P->F;
            while (f && (f->flags & FI_ARROW)) f = f->parent;
            int ok = f && (f->flags & FI_DERIVED) && (f->flags & FI_CLASS_CTOR);
            if (f && (f->flags & FI_EVAL) && (f->flags & FI_USES_SUPER_CALL)) ok = 1;
            if (!ok) { perr(P, pos, "'super' keyword unexpected here"); return 0; }
            for (struct funcinfo* g = P->F; g && (g->flags & FI_ARROW); g = g->parent) g->flags |= FI_USES_THIS;
            P->F->flags |= FI_USES_THIS;
            if (f) f->flags |= FI_USES_SUPER_CALL;
            return n;
        }
        if (TOK == T_DOT || TOK == T_LBRACK) {
            struct funcinfo* f = P->F;
            while (f && (f->flags & FI_ARROW)) f = f->parent;
            int ok = f && ((f->flags & FI_METHOD) || (f->flags & (FI_FIELD_INIT | FI_STATIC_BLOCK)));
            if (f && (f->flags & FI_EVAL) && (f->flags & FI_USES_SUPER_PROP)) ok = 1;
            if (!ok) { perr(P, pos, "'super' keyword unexpected here"); return 0; }
            for (struct funcinfo* g = P->F; g && (g->flags & FI_ARROW); g = g->parent) g->flags |= FI_USES_THIS;
            P->F->flags |= FI_USES_THIS;
            if (f) f->flags |= FI_USES_SUPER_PROP;
            return n;
        }
        perr(P, pos, "'super' keyword unexpected here");
        return 0;
    }
    case T_IMPORT: {
        next(P);
        if (TOK == T_DOT) {
            next(P);
            if (!(IS_CTX(meta) && !P->L.t.escaped)) { perr(P, TPOS, "unexpected %s after import.", tok_name(TOK)); return 0; }
            if (!P->is_module) { perr(P, pos, "Cannot use 'import.meta' outside a module"); return 0; }
            next(P);
            return mk(P, N_IMPORT_META, pos);
        }
        if (TOK != T_LPAREN) { perr(P, pos, "Cannot use import statement outside a module"); return 0; }
        next(P);
        n = mk(P, N_IMPORT_CALL, pos);
        if (!n) return 0;
        int saved = P->allow_in;
        P->allow_in = 1;
        n->a = parse_assign(P);
        if (TOK == T_COMMA) {
            next(P);
            if (TOK != T_RPAREN) {
                n->b = parse_assign(P);   // options (import attributes)
                if (TOK == T_COMMA) next(P);
            }
        }
        P->allow_in = saved;
        expect(P, T_RPAREN);
        return n;
    }
    case T_PRIVATE: {
        // #x in obj
        struct str* name = P->L.t.str;
        next(P);
        if (TOK != T_IN) { perr(P, pos, "Unexpected private name"); return 0; }
        n = mk(P, N_PRIVATE_IN, pos);
        if (n) n->u.str = name;
        return n;
    }
    case T_IDENT: {
        struct str* s = P->L.t.str;
        int escaped = P->L.t.escaped;
        // async function / async arrow
        if (s == J->A->async && !escaped) {
            struct token t;
            int nt = peek(P, &t);
            if (nt == T_FUNCTION && !t.nl_before) {
                next(P);
                next(P);
                uint32_t fl = FI_ASYNC;
                if (TOK == T_STAR) { fl |= FI_GENERATOR; next(P); }
                return parse_function(P, pos, fl, 0);
            }
            if (nt == T_IDENT && !t.nl_before) {
                // async x => ...
                next(P);
                struct str* pname = P->L.t.str;
                uint32_t ppos = TPOS;
                struct token t2;
                if (peek(P, &t2) == T_ARROW && !t2.nl_before) {
                    if (pname == J->A->await) { perr(P, ppos, "'await' is not a valid identifier name in an async function"); return 0; }
                    next(P);
                    struct node* param = make_ident(P, pname, ppos);
                    return arrow_single_param(P, param, pos, 1);
                }
                // `async` as an identifier: for await (async of x)
                return make_ident(P, s, pos);
            }
            if (nt == T_LPAREN && !t.nl_before) {
                next(P);
                return parse_paren_or_arrow(P, 1, pos);
            }
        }
        if (escaped && lex_is_keyword(s)) { perr(P, pos, "Keyword must not contain escaped characters"); return 0; }
        if (!ident_ok(P, s, pos, 0)) return 0;
        next(P);
        // x => ...
        if (TOK == T_ARROW && !P->L.t.nl_before) {
            if (strict(P) && (s == J->A->eval || s == J->A->arguments)) { perr(P, pos, "Unexpected eval or arguments in strict mode"); return 0; }
            return arrow_single_param(P, make_ident(P, s, pos), pos, 0);
        }
        if (s == J->A->arguments) {
            for (struct funcinfo* f = P->F; f; f = f->parent) {
                f->flags |= FI_USES_ARGS;
                if (!(f->flags & FI_ARROW)) break;
            }
        }
        return make_ident(P, s, pos);
    }
    default:
        if (TOK >= T_BREAK && TOK < T_COUNT) perr(P, pos, "Unexpected reserved word '%s'", tok_name(TOK));
        else perr(P, pos, "unexpected %s", tok_name(TOK));
        return 0;
    }
}

// ---------------------------------------------------------------- arrows (cover grammar)

static int contains_type(struct node* n, int type, uint32_t* where) {
    // search an expression tree (not into nested functions)
    for (; n; n = n->next) {
        if (n->type == type) { if (where) *where = n->pos; return 1; }
        if (n->type == N_FUNC || n->type == N_CLASS) continue;
        if (n->a && contains_type(n->a, type, where)) return 1;
        if (n->b && contains_type(n->b, type, where)) return 1;
        if (n->c && contains_type(n->c, type, where)) return 1;
        if (n->d && contains_type(n->d, type, where)) return 1;
        break;   // siblings handled by callers that pass lists
    }
    return 0;
}

// an identifier reference/binding named `name` (also in nested arrow parameters)
static int contains_name(struct node* n, struct str* name, uint32_t* where) {
    for (; n; n = n->next) {
        if (n->type == N_IDENT && n->u.str == name) { *where = n->pos; return 1; }
        if (n->type == N_PROP && (n->flags & NF_SHORTHAND) && n->b && n->b->type == N_IDENT && n->b->u.str == name) { *where = n->pos; return 1; }
        if (n->type == N_FUNC) {
            if ((n->u.fn->flags & FI_ARROW) && contains_name(n->u.fn->params, name, where)) return 1;
            continue;
        }
        if (n->type == N_CLASS) continue;
        if (n->a && contains_name(n->a, name, where)) return 1;
        if (n->b && contains_name(n->b, name, where)) return 1;
        if (n->c && contains_name(n->c, name, where)) return 1;
        if (n->d && contains_name(n->d, name, where)) return 1;
    }
    return 0;
}

static int list_contains(struct node* list, int type, uint32_t* where) {
    for (struct node* n = list; n; n = n->next) {
        struct node* save = n->next;
        n->next = 0;
        int r = contains_type(n, type, where);
        n->next = save;
        if (r) return 1;
    }
    return 0;
}

// convert a cover expression to a binding/assignment pattern (in place)
static int to_pattern(struct parser* P, struct node* n, int binding);

// Arrow parameters are parsed (as a cover expression) before we know they
// belong to an arrow: identifiers and nested functions/classes in them
// were attached to the enclosing scope and function. Move them into the
// arrow.
static void rescope(struct node* n, struct scope* olds, struct scope* news,
                    struct funcinfo* oldf, struct funcinfo* newf) {
    for (; n; n = n->next) {
        if (n->scope == olds) n->scope = news;
        // a direct eval in an arrow's parameters belongs to the arrow
        if (n->type == N_CALL && (n->flags & NF_DIRECT_EVAL)) {
            newf->flags |= FI_OWN_EVAL | FI_DIRECT_EVAL | FI_USES_ARGS | FI_USES_THIS;
            news->has_eval = 1;
        }
        if (n->type == N_FUNC) {
            struct funcinfo* fi = n->u.fn;
            if (fi->parent == oldf) {
                fi->parent = newf;
                fi->depth = newf->depth + 1;
                // move from oldf->children to newf->children
                struct funcinfo** pp = &oldf->children;
                struct funcinfo* prev = 0;
                while (*pp && *pp != fi) { prev = *pp; pp = &(*pp)->next_sibling; }
                if (*pp) {
                    *pp = fi->next_sibling;
                    if (oldf->last_child == fi) oldf->last_child = prev;
                    fi->next_sibling = 0;
                    if (newf->last_child) newf->last_child->next_sibling = fi;
                    else newf->children = fi;
                    newf->last_child = fi;
                }
            }
            if (fi->param_scope && fi->param_scope->parent == olds) fi->param_scope->parent = news;
            continue;   // its body has its own scopes
        }
        if (n->type == N_CLASS) {
            if (n->scope && n->scope->parent == olds) n->scope->parent = news;
            // methods / field initializers of the class
            if (n->b) rescope(n->b, olds, news, oldf, newf);
            for (struct node* m = n->c; m; m = m->next) {
                if (m->a && (m->flags & NF_COMPUTED)) rescope(m->a, olds, news, oldf, newf);
                if (m->b && m->b->type == N_FUNC) rescope(m->b, olds, news, oldf, newf);
            }
            if (n->d) { rescope(n->d->a, olds, news, oldf, newf); rescope(n->d->b, olds, news, oldf, newf); }
            continue;
        }
        if (n->a) rescope(n->a, olds, news, oldf, newf);
        if (n->b) rescope(n->b, olds, news, oldf, newf);
        if (n->c) rescope(n->c, olds, news, oldf, newf);
        if (n->d) rescope(n->d, olds, news, oldf, newf);
    }
}

static struct node* arrow_from_params(struct parser* P, struct node* params, uint32_t pos, int is_async, int trailing_comma_rest) {
    (void)trailing_comma_rest;
    struct scope* outer_scope = P->S;
    struct funcinfo* outer_fn = P->F;
    struct funcinfo* fi = new_funcinfo(P, FI_ARROW | (is_async ? FI_ASYNC : 0), pos);
    if (!fi) return 0;
    fi->params = params;
    struct funcinfo* saved_f;
    struct scope* saved_s;
    enter_function(P, fi, &saved_f, &saved_s);
    rescope(params, outer_scope, fi->param_scope, outer_fn, fi);
    for (struct node* p = params; p; p = p->next) {
        if (!to_pattern(P, p, 1)) { leave_function(P, saved_f, saved_s); return 0; }
        fi->nparams++;
    }
    declare_params(P, fi);
    struct node* fn = parse_arrow_body(P, fi);
    leave_function(P, saved_f, saved_s);
    if (fn) fn->pos = pos;
    return fn;
}

static struct node* arrow_single_param(struct parser* P, struct node* param, uint32_t pos, int is_async) {
    if (TOK != T_ARROW) { perr(P, TPOS, "expected =>"); return 0; }
    return arrow_from_params(P, param, pos, is_async, 0);
}

// `(` ... `)` either a parenthesized expression or arrow parameters.
// is_async: `async (` ... (call or async arrow params)
static struct node* parse_paren_or_arrow(struct parser* P, int is_async, uint32_t async_pos) {
    uint32_t pos = TPOS;
    next(P);   // (
    struct node* items = 0;
    struct node** tail = &items;
    int has_rest = 0, trailing_comma = 0, count = 0;
    uint32_t rest_pos = 0;
    struct node* saved_cover = P->cover_init;
    P->cover_init = 0;
    int saved_in = P->allow_in;
    P->allow_in = 1;
    // await/yield inside potential arrow parameters
    while (TOK != T_RPAREN && !FAILED) {
        struct node* e;
        if (TOK == T_ELLIPSIS) {
            uint32_t sp = TPOS;
            next(P);
            e = mk(P, N_REST, sp);
            if (!e) return 0;
            e->a = parse_binding_target(P, 0);
            if (!e->a) return 0;
            if (TOK == T_ASSIGN) { perr(P, TPOS, "Rest parameter may not have a default initializer"); return 0; }
            has_rest = 1;
            rest_pos = sp;
            if (is_async) { e->type = N_SPREAD; }   // async(...x): may be a call
        } else e = parse_assign(P);
        if (!e) return 0;
        count++;
        *tail = e;
        tail = &e->next;
        if (TOK == T_RPAREN) break;
        if (!expect(P, T_COMMA)) return 0;
        if (TOK == T_RPAREN) { trailing_comma = 1; break; }
        if (has_rest) { perr(P, rest_pos, "Rest parameter must be last formal parameter"); return 0; }
    }
    P->allow_in = saved_in;
    uint32_t rparen_end = P->L.t.end;
    expect(P, T_RPAREN);
    if (FAILED) return 0;
    if (TOK == T_ARROW && !P->L.t.nl_before) {
        if (has_rest && trailing_comma) { perr(P, rest_pos, "Rest parameter must be last formal parameter"); return 0; }
        // yield / await expressions are not allowed in arrow parameters
        uint32_t w;
        if (list_contains(items, N_YIELD, &w)) { perr(P, w, "Yield expression not allowed in formal parameter"); return 0; }
        if (list_contains(items, N_AWAIT, &w)) { perr(P, w, "Illegal await-expression in formal parameters of async function"); return 0; }
        if (is_async && contains_name(items, P->J->A->await, &w)) { perr(P, w, "'await' is not a valid identifier name in an async function"); return 0; }
        for (struct node* it = items; it; it = it->next) if (it->type == N_SPREAD) it->type = N_REST;
        P->cover_init = saved_cover;
        return arrow_from_params(P, items, is_async ? async_pos : pos, is_async, trailing_comma);
    }
    if (P->cover_init) { perr(P, P->cover_init->pos, "Invalid shorthand property initializer"); return 0; }
    P->cover_init = saved_cover;
    if (is_async) {
        // a call to a function named async
        struct node* callee = make_ident(P, P->J->A->async, async_pos);
        struct node* c = mk(P, N_CALL, async_pos);
        if (!c) return 0;
        c->a = callee;
        c->b = items;
        for (struct node* it = items; it; it = it->next) if (it->type == N_SPREAD) c->flags |= NF_HAS_SPREAD;
        (void)rparen_end;
        return c;
    }
    if (!count) { perr(P, TPOS, "unexpected token )"); return 0; }
    if (has_rest) { perr(P, rest_pos, "unexpected ..."); return 0; }
    if (trailing_comma) { perr(P, TPOS, "unexpected token )"); return 0; }
    struct node* e;
    if (count == 1) e = items;
    else {
        e = mk(P, N_SEQ, pos);
        if (!e) return 0;
        e->a = items;
    }
    e->flags |= NF_PAREN;
    return e;
}

// ---------------------------------------------------------------- member / call chains

static struct node* parse_arguments(struct parser* P, struct node* call) {
    next(P);   // (
    struct node** tail = &call->b;
    int saved = P->allow_in;
    P->allow_in = 1;
    while (TOK != T_RPAREN && !FAILED) {
        struct node* a;
        if (TOK == T_ELLIPSIS) {
            uint32_t sp = TPOS;
            next(P);
            a = mk(P, N_SPREAD, sp);
            if (a) a->a = parse_assign(P);
            call->flags |= NF_HAS_SPREAD;
        } else a = parse_assign(P);
        if (!a) return 0;
        *tail = a;
        tail = &a->next;
        if (TOK != T_RPAREN && !expect(P, T_COMMA)) return 0;
    }
    P->allow_in = saved;
    expect(P, T_RPAREN);
    return call;
}

static int private_name_check(struct parser* P, struct str* name, uint32_t pos);

static int bare_arrow(struct node* e) {
    return e && e->type == N_FUNC && (e->u.fn->flags & FI_ARROW) && !(e->flags & NF_PAREN);
}

static struct node* parse_lhs(struct parser* P, int allow_call) {
    struct node* e = parse_primary(P);
    if (!e) return 0;
    if (bare_arrow(e)) {
        // an arrow is never a callee or object; after a line break ASI ends the statement
        if (P->L.t.nl_before) return e;
        if (TOK == T_DOT || TOK == T_LBRACK || TOK == T_LPAREN || TOK == T_TEMPLATE || TOK == T_OPTCHAIN) {
            perr(P, TPOS, "unexpected %s after arrow function", tok_name(TOK));
            return 0;
        }
        return e;
    }
    int in_chain = 0;   // inside an optional chain (?.)
    for (;;) {
        uint32_t pos = TPOS;
        if (TOK == T_DOT) {
            next(P);
            struct node* m = mk(P, N_MEMBER, pos);
            if (!m) return 0;
            m->a = e;
            if (TOK == T_PRIVATE) {
                if (e->type == N_SUPER) { perr(P, TPOS, "Unexpected private field"); return 0; }
                m->flags |= NF_PRIVATE;
                m->u.str = P->L.t.str;
                if (!private_name_check(P, m->u.str, TPOS)) return 0;
                next(P);
            } else if (TOK == T_IDENT || (TOK >= T_BREAK && TOK < T_COUNT)) {
                m->u.str = P->L.t.str ? P->L.t.str : atom_cstr(P->J, tok_name(TOK));
                next(P);
            } else { perr(P, TPOS, "unexpected %s after .", tok_name(TOK)); return 0; }
            e = m;
        } else if (TOK == T_LBRACK) {
            next(P);
            struct node* m = mk(P, N_MEMBER, pos);
            if (!m) return 0;
            m->a = e;
            m->flags |= NF_COMPUTED;
            int saved = P->allow_in;
            P->allow_in = 1;
            m->b = parse_expression(P);
            P->allow_in = saved;
            if (!expect(P, T_RBRACK)) return 0;
            e = m;
        } else if (TOK == T_TEMPLATE) {
            if (in_chain) { perr(P, pos, "Invalid tagged template on optional chain"); return 0; }
            e = parse_template(P, e);
            if (!e) return 0;
        } else if (TOK == T_LPAREN && allow_call) {
            struct node* c = mk(P, N_CALL, pos);
            if (!c) return 0;
            c->a = e;
            if (!parse_arguments(P, c)) return 0;
            if (e->type == N_IDENT && e->u.str == P->J->A->eval && !(e->flags & NF_PAREN)) {
                c->flags |= NF_DIRECT_EVAL;
                P->F->flags |= FI_OWN_EVAL;
                for (struct funcinfo* f = P->F; f; f = f->parent) f->flags |= FI_DIRECT_EVAL | FI_USES_ARGS | FI_USES_THIS;
                for (struct scope* s = P->S; s; s = s->parent) s->has_eval = 1;
            }
            e = c;
        } else if (TOK == T_OPTCHAIN && allow_call) {
            next(P);
            in_chain = 1;
            struct node* m;
            if (TOK == T_LPAREN) {
                m = mk(P, N_CALL, pos);
                if (!m) return 0;
                m->a = e;
                m->flags |= NF_OPTIONAL;
                if (!parse_arguments(P, m)) return 0;
            } else if (TOK == T_LBRACK) {
                next(P);
                m = mk(P, N_MEMBER, pos);
                if (!m) return 0;
                m->a = e;
                m->flags |= NF_COMPUTED | NF_OPTIONAL;
                int saved = P->allow_in;
                P->allow_in = 1;
                m->b = parse_expression(P);
                P->allow_in = saved;
                if (!expect(P, T_RBRACK)) return 0;
            } else if (TOK == T_TEMPLATE) {
                perr(P, pos, "Invalid tagged template on optional chain");
                return 0;
            } else {
                m = mk(P, N_MEMBER, pos);
                if (!m) return 0;
                m->a = e;
                m->flags |= NF_OPTIONAL;
                if (TOK == T_PRIVATE) {
                    m->flags |= NF_PRIVATE;
                    m->u.str = P->L.t.str;
                    if (!private_name_check(P, m->u.str, TPOS)) return 0;
                    next(P);
                } else if (TOK == T_IDENT || (TOK >= T_BREAK && TOK < T_COUNT)) {
                    m->u.str = P->L.t.str ? P->L.t.str : atom_cstr(P->J, tok_name(TOK));
                    next(P);
                } else { perr(P, TPOS, "unexpected %s after ?.", tok_name(TOK)); return 0; }
            }
            e = m;
        } else break;
    }
    if (in_chain) {
        // wrap: the whole chain short-circuits to undefined
        struct node* w = mk(P, N_OPTCHAIN, e->pos);
        if (!w) return 0;
        w->a = e;
        return w;
    }
    if (e->type == N_SUPER) { perr(P, e->pos, "'super' keyword unexpected here"); return 0; }
    return e;
}

// ---------------------------------------------------------------- unary / binary

static int binary_prec(int t, int allow_in) {
    switch (t) {
    case T_NULLISH: return 1;
    case T_OR: return 2;
    case T_AND: return 3;
    case T_PIPE: return 4;
    case T_CARET: return 5;
    case T_AMP: return 6;
    case T_EQ: case T_NE: case T_SEQ: case T_SNE: return 7;
    case T_LT: case T_GT: case T_LE: case T_GE: case T_INSTANCEOF: return 8;
    case T_IN: return allow_in ? 8 : 0;
    case T_SHL: case T_SAR: case T_SHR: return 9;
    case T_PLUS: case T_MINUS: return 10;
    case T_STAR: case T_SLASH: case T_PERCENT: return 11;
    case T_STARSTAR: return 12;
    default: return 0;
    }
}

static int in_async(struct parser* P) { return (P->F->flags & FI_ASYNC) != 0; }

static struct node* parse_postfix(struct parser* P) {
    uint32_t pos = TPOS;
    struct node* e = parse_lhs(P, 1);
    if (!e) return 0;
    if ((TOK == T_INC || TOK == T_DEC) && !P->L.t.nl_before) {
        if (bare_arrow(e)) { perr(P, TPOS, "Invalid left-hand side expression in postfix operation"); return 0; }
        if (!to_assign_target(P, e, 1)) return 0;
        struct node* u = mk(P, N_UPDATE, pos);
        if (!u) return 0;
        u->op = (uint8_t)TOK;
        u->a = e;
        next(P);
        return u;
    }
    return e;
}

static struct node* parse_unary(struct parser* P) {
    uint32_t pos = TPOS;
    int t = TOK;
    switch (t) {
    case T_DELETE: case T_VOID: case T_TYPEOF: case T_PLUS: case T_MINUS: case T_TILDE: case T_NOT: {
        next(P);
        struct node* u = mk(P, N_UNARY, pos);
        if (!u) return 0;
        u->op = (uint8_t)t;
        u->a = parse_unary(P);
        if (!u->a) return 0;
        if (t == T_DELETE) {
            struct node* a = u->a;
            while (a->type == N_OPTCHAIN) a = a->a;
            if (strict(P) && u->a->type == N_IDENT) { perr(P, pos, "Delete of an unqualified identifier in strict mode."); return 0; }
            if (a->type == N_MEMBER && (a->flags & NF_PRIVATE)) { perr(P, pos, "Private fields can not be deleted"); return 0; }
        }
        if (TOK == T_STARSTAR) { perr(P, TPOS, "Unary operator used immediately before exponentiation expression. Parenthesis must be used to disambiguate operator precedence"); return 0; }
        return u;
    }
    case T_INC: case T_DEC: {
        next(P);
        struct node* u = mk(P, N_UPDATE, pos);
        if (!u) return 0;
        u->op = (uint8_t)t;
        u->flags |= NF_PREFIX;
        u->a = parse_unary(P);
        if (!u->a || !to_assign_target(P, u->a, 1)) return 0;
        return u;
    }
    default:
        break;
    }
    if (TOK == T_IDENT && P->L.t.str == P->J->A->await && !P->L.t.escaped &&
        (in_async(P) || (P->F->flags & FI_MODULE))) {
        if (P->in_class_field && !(P->F->flags & FI_ASYNC)) { perr(P, pos, "await is not allowed here"); return 0; }
        next(P);
        struct node* a = mk(P, N_AWAIT, pos);
        if (!a) return 0;
        a->a = parse_unary(P);
        if (!a->a) return 0;
        if (TOK == T_STARSTAR) { perr(P, TPOS, "Unary operator used immediately before exponentiation expression"); return 0; }
        return a;
    }
    if (TOK == T_IDENT && P->L.t.str == P->J->A->await && P->in_static_block) {
        perr(P, pos, "await is not allowed in class static blocks");
        return 0;
    }
    return parse_postfix(P);
}

static struct node* parse_binary(struct parser* P, int min_prec) {
    uint32_t pos = TPOS;
    struct node* left;
    if (TOK == T_PRIVATE) {
        left = parse_primary(P);   // #x in obj
        if (!left) return 0;
        if (!private_name_check(P, left->u.str, pos)) return 0;
        if (min_prec > 8) { perr(P, pos, "Unexpected private name"); return 0; }
    } else left = parse_unary(P);
    if (!left) return 0;
    for (;;) {
        int t = TOK;
        int prec = binary_prec(t, P->allow_in);
        if (!prec || prec < min_prec) break;
        uint32_t opos = TPOS;
        if (bare_arrow(left) && P->L.t.nl_before) break;   // ASI: `() => {}` NL `+1`
        if (bare_arrow(left)) { perr(P, opos, "unexpected %s after arrow function", tok_name(t)); return 0; }
        next(P);
        struct node* right;
        if (t == T_STARSTAR) right = parse_binary(P, prec);   // right-associative
        else right = parse_binary(P, prec + 1);
        if (!right) return 0;
        if (bare_arrow(right)) { perr(P, right->pos, "Malformed arrow function parameter list"); return 0; }
        if (left->type == N_PRIVATE_IN && t != T_IN) { perr(P, left->pos, "Unexpected private name"); return 0; }
        struct node* b;
        if (t == T_AND || t == T_OR || t == T_NULLISH) {
            // ?? cannot mix with && / || without parentheses
            if (t == T_NULLISH) {
                if ((left->type == N_LOGICAL && left->op != T_NULLISH && !(left->flags & NF_PAREN)) ||
                    (right->type == N_LOGICAL && right->op != T_NULLISH && !(right->flags & NF_PAREN))) {
                    perr(P, opos, "Unexpected token '?\?'");
                    return 0;
                }
            } else if ((left->type == N_LOGICAL && left->op == T_NULLISH && !(left->flags & NF_PAREN)) ||
                       (right->type == N_LOGICAL && right->op == T_NULLISH && !(right->flags & NF_PAREN))) {
                perr(P, opos, "Unexpected token '%s'", tok_name(t));
                return 0;
            }
            b = mk(P, N_LOGICAL, pos);
        } else if (left->type == N_PRIVATE_IN) {
            b = left;
            b->a = right;
            left = b;
            continue;
        } else b = mk(P, N_BINARY, pos);
        if (!b) return 0;
        b->op = (uint8_t)t;
        b->a = left;
        b->b = right;
        left = b;
    }
    return left;
}

static struct node* parse_conditional(struct parser* P) {
    uint32_t pos = TPOS;
    struct node* c = parse_binary(P, 1);
    if (!c || TOK != T_QUESTION) return c;
    if (bare_arrow(c)) { perr(P, TPOS, "unexpected ? after arrow function"); return 0; }
    if (c->type == N_PRIVATE_IN && !c->a) { perr(P, pos, "Unexpected private name"); return 0; }
    next(P);
    struct node* n = mk(P, N_COND, pos);
    if (!n) return 0;
    n->a = c;
    int saved = P->allow_in;
    P->allow_in = 1;
    n->b = parse_assign(P);
    P->allow_in = saved;
    if (!expect(P, T_COLON)) return 0;
    n->c = parse_assign(P);
    if (!n->b || !n->c) return 0;
    return n;
}

static int is_assign_op(int t) {
    return t == T_ASSIGN || (t >= T_PLUS_ASSIGN && t <= T_NULLISH_ASSIGN) || t == T_SLASH_ASSIGN;
}

static struct node* parse_yield(struct parser* P) {
    uint32_t pos = TPOS;
    next(P);
    struct node* y = mk(P, N_YIELD, pos);
    if (!y) return 0;
    P->F->flags |= FI_HAS_YIELD;
    if (P->L.t.nl_before) return y;
    if (TOK == T_STAR) {
        y->flags |= NF_DELEGATE;
        next(P);
        y->a = parse_assign(P);
        return y->a ? y : 0;
    }
    switch (TOK) {
    case T_RPAREN: case T_RBRACK: case T_RBRACE: case T_COMMA: case T_SEMI: case T_COLON: case T_EOF:
    case T_IN:
        return y;
    default:
        if (TOK == T_IDENT && IS_CTX(of)) return y;
        y->a = parse_assign(P);
        return y->a ? y : 0;
    }
}

static struct node* parse_assign(struct parser* P) {
    if (FAILED) return 0;
    // yield in generators
    if (TOK == T_IDENT && P->L.t.str == P->J->A->yield && (P->F->flags & FI_GENERATOR)) {
        if (P->L.t.escaped) { perr(P, TPOS, "Keyword must not contain escaped characters"); return 0; }
        if (P->in_class_field) { perr(P, TPOS, "yield is not allowed here"); return 0; }
        return parse_yield(P);
    }
    uint32_t pos = TPOS;
    struct node* saved_cover = P->cover_init;
    P->cover_init = 0;
    struct node* left = parse_conditional(P);
    if (!left) return 0;
    if (left->type == N_FUNC && (left->u.fn->flags & FI_ARROW)) {
        // an arrow function is a complete AssignmentExpression
        P->cover_init = saved_cover;
        if (is_assign_op(TOK)) { perr(P, TPOS, "Invalid left-hand side in assignment"); return 0; }
        return left;
    }
    if (is_assign_op(TOK)) {
        int op = TOK;
        if (op == T_ASSIGN && (left->type == N_OBJECT || left->type == N_ARRAY) && !(left->flags & NF_PAREN)) {
            if (!to_pattern(P, left, 0)) return 0;
            P->cover_init = 0;
        } else {
            if (P->cover_init) { perr(P, P->cover_init->pos, "Invalid shorthand property initializer"); return 0; }
            int logical = op == T_AND_ASSIGN || op == T_OR_ASSIGN || op == T_NULLISH_ASSIGN;
            if (!to_assign_target(P, left, logical ? 2 : 1)) return 0;
        }
        next(P);
        struct node* a = mk(P, N_ASSIGN, pos);
        if (!a) return 0;
        a->op = (uint8_t)op;
        a->a = left;
        a->b = parse_assign(P);
        if (!a->b) return 0;
        // anonymous function naming: x = function () {}
        P->cover_init = saved_cover;
        return a;
    }
    if (P->cover_init) {
        // `{ a = 1 }` is only valid as a pattern: let the caller decide
        // (for-in/of heads and nested patterns); expressions reject it
        struct node* ci = P->cover_init;
        P->cover_init = saved_cover ? saved_cover : ci;
        return left;
    }
    P->cover_init = saved_cover;
    return left;
}

static struct node* parse_expression(struct parser* P) {
    uint32_t pos = TPOS;
    struct node* e = parse_assign(P);
    if (!e || TOK != T_COMMA) return e;
    struct node* s = mk(P, N_SEQ, pos);
    if (!s) return 0;
    s->a = e;
    struct node* tail = e;
    while (TOK == T_COMMA && !FAILED) {
        next(P);
        struct node* x = parse_assign(P);
        if (!x) return 0;
        tail->next = x;
        tail = x;
    }
    return s;
}

// expression in a context where a cover initializer is an error
static struct node* parse_expression_checked(struct parser* P) {
    struct node* saved = P->cover_init;
    P->cover_init = 0;
    struct node* e = parse_expression(P);
    if (e && P->cover_init) { perr(P, P->cover_init->pos, "Invalid shorthand property initializer"); return 0; }
    P->cover_init = saved;
    return e;
}

#include "parse_stmt.c"
