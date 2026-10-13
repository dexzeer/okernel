// dmccl compiler: tokens -> bytecode in ONE pass (no syntax tree). Recursive descent; jumps are patched after the fact.
//
// Layout of a program: every `when` handler is a stretch of code ending in OP_END; the handler table says which event
// starts which stretch. Top-level statements that are not inside a `when` become a `run` handler of their own, so the
// old "just write statements" scripts still work.
#include "dm_internal.h"

#define MAX_RANGES 256
#define MAX_ARGS 8

struct range { int start, end; };

struct parser {
    struct dm_tokens* tk;
    int pos;
    dm_program* P;
    char* err;
    size_t err_cap;
    int failed;
    int thing;                              // thing being compiled, -1 at level scope
    char locals[DM_MAX_LOCALS][32];         // `repeat .. as i` variables of the handler being compiled
    int nlocals;
    struct range ranges[MAX_RANGES];        // statements of the open blocks (for `above`)
    int nranges;
    int block_base;                         // first range that belongs to the innermost open block
};

// ---- errors ---------------------------------------------------------------------------------------------------------

static void put_str(char* dst, size_t cap, size_t* n, const char* s) {
    while (*s && *n + 1 < cap) dst[(*n)++] = *s++;
    dst[*n] = 0;
}

static void err_at(struct parser* p, int line, const char* what, const char* extra) {
    if (p->failed) return;
    p->failed = 1;
    size_t n = 0;
    char num[12];
    int k = 0, v = line;
    do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    char head[24];
    int h = 0;
    while (k) head[h++] = num[--k];
    head[h] = 0;
    put_str(p->err, p->err_cap, &n, "line ");
    put_str(p->err, p->err_cap, &n, head);
    put_str(p->err, p->err_cap, &n, ": ");
    put_str(p->err, p->err_cap, &n, what);
    if (extra) { put_str(p->err, p->err_cap, &n, " "); put_str(p->err, p->err_cap, &n, extra); }
}

static const struct dm_token* cur(struct parser* p) { return &p->tk->t[p->pos < p->tk->n ? p->pos : p->tk->n - 1]; }
static int kind(struct parser* p) { return cur(p)->kind; }
static int kind_at(struct parser* p, int ahead) { int i = p->pos + ahead; return p->tk->t[i < p->tk->n ? i : p->tk->n - 1].kind; }
static int line_of(struct parser* p) { return cur(p)->line; }
static void adv(struct parser* p) { if (p->pos < p->tk->n - 1) p->pos++; }
static int accept(struct parser* p, int k) { if (kind(p) == k) { adv(p); return 1; } return 0; }

static const char* describe(struct parser* p, char* buf, size_t cap) {
    const struct dm_token* t = cur(p);
    size_t n = 0;
    buf[0] = 0;
    switch (t->kind) {
    case T_EOF: return "the end of the file";
    case T_NL: return "the end of the line";
    case T_NUM: return "a number";
    case T_STR: return "a string";
    default:
        if (t->text) { put_str(buf, cap, &n, "`"); put_str(buf, cap, &n, t->text); put_str(buf, cap, &n, "`"); return buf; }
        return "this symbol";
    }
}

static void unexpected(struct parser* p, const char* wanted) {
    char buf[48], msg[96];
    size_t n = 0;
    put_str(msg, sizeof msg, &n, "expected ");
    put_str(msg, sizeof msg, &n, wanted);
    put_str(msg, sizeof msg, &n, ", got");
    err_at(p, line_of(p), msg, describe(p, buf, sizeof buf));
}

static int expect(struct parser* p, int k, const char* wanted) {
    if (p->failed) return 0;
    if (kind(p) == k) { adv(p); return 1; }
    unexpected(p, wanted);
    return 0;
}

static void skip_nl(struct parser* p) { while (kind(p) == T_NL) adv(p); }

// ---- tables ---------------------------------------------------------------------------------------------------------

static int grow(void** arr, int* cap, int n, size_t elem) {
    if (n < *cap) return 0;
    int nc = *cap ? *cap * 2 : 64;
    void* g = DM_REALLOC(*arr, (size_t)nc * elem);
    if (!g) return -1;
    *arr = g;
    *cap = nc;
    return 0;
}
#define GROW(P, arr, n, cap) grow((void**)&(P)->arr, &(P)->cap, (P)->n, sizeof *(P)->arr)

static int emit(struct parser* p, int op, int n, int a) {
    dm_program* P = p->P;
    if (p->failed) return 0;
    if (GROW(P, code, ncode, capcode)) { err_at(p, line_of(p), "out of memory", 0); return 0; }
    struct dm_ins* i = &P->code[P->ncode];
    i->op = (uint8_t)op;
    i->n = (uint8_t)n;
    i->line = (uint16_t)line_of(p);
    i->a = a;
    return P->ncode++;
}

static int here(struct parser* p) { return p->P->ncode; }
static void patch(struct parser* p, int at, int target) { if (!p->failed) p->P->code[at].a = target - at; }

static int add_num(struct parser* p, double v) {
    dm_program* P = p->P;
    if (GROW(P, nums, nnum, capnum)) { err_at(p, line_of(p), "out of memory", 0); return 0; }
    P->nums[P->nnum] = v;
    return P->nnum++;
}

static int add_str(struct parser* p, const char* s) {
    dm_program* P = p->P;
    for (int i = 0; i < P->nstr; i++) if (!strcmp(P->strs[i], s)) return i;      // equal text = equal index (members compare by pointer)
    if (GROW(P, strs, nstr, capstr)) { err_at(p, line_of(p), "out of memory", 0); return 0; }
    size_t n = strlen(s);
    char* c = (char*)DM_ALLOC(n + 1);
    if (!c) { err_at(p, line_of(p), "out of memory", 0); return 0; }
    memcpy(c, s, n + 1);
    P->strs[P->nstr] = c;
    return P->nstr++;
}

static int global_slot(struct parser* p, const char* name) {
    dm_program* P = p->P;
    for (int i = 0; i < P->nglob; i++) if (!strcmp(P->globals[i], name)) return i;
    if (strlen(name) >= 32) { err_at(p, line_of(p), "variable name longer than 31 characters:", name); return 0; }
    if (GROW(P, globals, nglob, capglob)) { err_at(p, line_of(p), "out of memory", 0); return 0; }
    strcpy(P->globals[P->nglob], name);
    return P->nglob++;
}

static int find_local(struct parser* p, const char* name) {
    for (int i = 0; i < p->nlocals; i++) if (!strcmp(p->locals[i], name)) return i;
    return -1;
}

static int add_local(struct parser* p, const char* name) {
    int i = find_local(p, name);
    if (i >= 0) return i;
    if (p->nlocals >= DM_MAX_LOCALS || strlen(name) >= 32) { err_at(p, line_of(p), "too many `repeat .. as` variables in one handler", 0); return 0; }
    strcpy(p->locals[p->nlocals], name);
    return p->nlocals++;
}

static int add_call(struct parser* p, const char* module, const char* name) {
    dm_program* P = p->P;
    if (strlen(module) >= 24 || strlen(name) >= 32) { err_at(p, line_of(p), "function name too long:", name); return 0; }
    if (GROW(P, calls, ncalls, capcalls)) { err_at(p, line_of(p), "out of memory", 0); return 0; }
    struct dm_call* c = &P->calls[P->ncalls];
    strcpy(c->module, module);
    strcpy(c->name, name);
    c->line = line_of(p);
    return P->ncalls++;
}

// ---- expressions ----------------------------------------------------------------------------------------------------

static void parse_expr(struct parser* p);

static int parse_args(struct parser* p) {      // after '(' ... consumes ')'; returns how many
    int n = 0;
    if (kind(p) != T_RPAREN) {
        do {
            parse_expr(p);
            if (++n > MAX_ARGS) { err_at(p, line_of(p), "too many arguments (the limit is 8)", 0); return n; }
        } while (!p->failed && accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "`)`");
    return n;
}

static int is_word(int k) { return k == T_ID || (k >= T_WHEN && k <= T_OR); }

static void parse_primary(struct parser* p) {
    if (p->failed) return;
    const struct dm_token* t = cur(p);
    switch (t->kind) {
    case T_NUM: emit(p, OP_NUM, 0, add_num(p, t->num)); adv(p); return;
    case T_STR: emit(p, OP_STR, 0, add_str(p, t->text)); adv(p); return;
    case T_LPAREN: adv(p); parse_expr(p); expect(p, T_RPAREN, "`)`"); return;
    case T_NOT: adv(p); parse_primary(p); emit(p, OP_NOT, 0, 0); return;
    case T_ID: break;
    default: unexpected(p, "a value"); return;
    }
    const char* name = t->text;
    adv(p);
    if (kind(p) == T_COLON && kind_at(p, 1) == T_ID && kind_at(p, 2) == T_LPAREN) {      // module:function(args)
        const char* fn = p->tk->t[p->pos + 1].text;
        int call = add_call(p, name, fn);
        adv(p); adv(p); adv(p);
        int argc = parse_args(p);
        emit(p, OP_CALL, argc, call);
        return;
    }
    if (kind(p) == T_LPAREN) {                                                          // function(args)
        int call = add_call(p, "", name);
        adv(p);
        int argc = parse_args(p);
        emit(p, OP_CALL, argc, call);
        return;
    }
    if (!strcmp(name, "true")) { emit(p, OP_TRUE, 0, 0); return; }
    if (!strcmp(name, "false")) { emit(p, OP_FALSE, 0, 0); return; }
    if (!strcmp(name, "nil")) { emit(p, OP_NIL, 0, 0); return; }
    static const char* const CTX[DM_CTX_COUNT] = { "me", "who", "hit", "child", "parent", "time" };
    for (int i = 0; i < DM_CTX_COUNT; i++)
        if (!strcmp(name, CTX[i])) { emit(p, OP_CTX, 0, i); return; }
    int loc = find_local(p, name);
    if (loc >= 0) { emit(p, OP_LOADL, 0, loc); return; }
    emit(p, OP_LOADG, 0, global_slot(p, name));
}

// primary followed by any number of `.member` / `.method(args)`. A trailing `.member` without parens stays an OP_GETM, which
// a bare statement turns into a call (`child.abort`).
static void parse_postfix(struct parser* p) {
    parse_primary(p);
    while (!p->failed && kind(p) == T_DOT) {
        adv(p);
        if (!is_word(kind(p)) || !cur(p)->text) { unexpected(p, "a name after `.`"); return; }
        int name = add_str(p, cur(p)->text);
        adv(p);
        if (kind(p) == T_LPAREN) {
            adv(p);
            int argc = parse_args(p);
            emit(p, OP_CALLM, argc, name);
        } else emit(p, OP_GETM, 0, name);
    }
}

static void parse_unary(struct parser* p) {
    if (kind(p) == T_MINUS) { adv(p); parse_unary(p); emit(p, OP_NEG, 0, 0); return; }
    parse_postfix(p);
}

static void parse_mul(struct parser* p) {
    parse_unary(p);
    while (!p->failed && (kind(p) == T_STAR || kind(p) == T_SLASH || kind(p) == T_PCT)) {
        int k = kind(p);
        adv(p);
        parse_unary(p);
        emit(p, k == T_STAR ? OP_MUL : k == T_SLASH ? OP_DIV : OP_MOD, 0, 0);
    }
}

static void parse_add(struct parser* p) {
    parse_mul(p);
    while (!p->failed && (kind(p) == T_PLUS || kind(p) == T_MINUS)) {
        int k = kind(p);
        adv(p);
        parse_mul(p);
        emit(p, k == T_PLUS ? OP_ADD : OP_SUB, 0, 0);
    }
}

static void parse_cmp(struct parser* p) {
    parse_add(p);
    for (;;) {
        int k = kind(p), op;
        switch (k) {
        case T_EQ: op = OP_EQ; break;
        case T_NE: op = OP_NE; break;
        case T_LT: op = OP_LT; break;
        case T_GT: op = OP_GT; break;
        case T_LE: op = OP_LE; break;
        case T_GE: op = OP_GE; break;
        default: return;
        }
        if (p->failed) return;
        adv(p);
        parse_add(p);
        emit(p, op, 0, 0);
    }
}

static void parse_not(struct parser* p) {
    if (kind(p) == T_NOT) { adv(p); parse_not(p); emit(p, OP_NOT, 0, 0); return; }
    parse_cmp(p);
}

// `a & b` and `a | b` stop at the first answer: the right side is not even run when the left decides. Both give a bool.
static void parse_and(struct parser* p) {
    parse_not(p);
    while (!p->failed && kind(p) == T_AND) {
        adv(p);
        emit(p, OP_DUP, 0, 0);
        int skip = emit(p, OP_JMPF, 0, 0);       // falsy left: keep it as the answer
        emit(p, OP_POP, 0, 0);
        parse_not(p);
        patch(p, skip, here(p));
        emit(p, OP_NOT, 0, 0);
        emit(p, OP_NOT, 0, 0);
    }
}

static void parse_or(struct parser* p) {
    parse_and(p);
    while (!p->failed && kind(p) == T_OR) {
        adv(p);
        emit(p, OP_DUP, 0, 0);
        emit(p, OP_NOT, 0, 0);
        int skip = emit(p, OP_JMPF, 0, 0);       // truthy left: keep it as the answer
        emit(p, OP_POP, 0, 0);
        parse_and(p);
        patch(p, skip, here(p));
        emit(p, OP_NOT, 0, 0);
        emit(p, OP_NOT, 0, 0);
    }
}

static void parse_expr(struct parser* p) { parse_or(p); }

// ---- statements -----------------------------------------------------------------------------------------------------

static void statement(struct parser* p);

static int block_end(int k) { return k == T_ENDIFCHAIN || k == T_ELSEIF || k == T_ELSE || k == T_EOF; }

static void parse_block(struct parser* p) {      // statements up to endifchain / elseif / else
    int saved_base = p->block_base;
    p->block_base = p->nranges;
    skip_nl(p);
    while (!p->failed && !block_end(kind(p))) {
        statement(p);
        skip_nl(p);
    }
    p->nranges = p->block_base;
    p->block_base = saved_base;
}

static void remember(struct parser* p, int start) {
    if (p->failed) return;
    if (p->nranges >= MAX_RANGES) { err_at(p, line_of(p), "too many statements in one block for `above`", 0); return; }
    p->ranges[p->nranges].start = start;
    p->ranges[p->nranges].end = here(p);
    p->nranges++;
}

static void if_statement(struct parser* p) {
    int ends[32], nends = 0;
    adv(p);                                         // if
    parse_expr(p);
    expect(p, T_DO, "`do`");
    int next = emit(p, OP_JMPF, 0, 0);
    parse_block(p);
    for (;;) {
        if (p->failed) return;
        if (kind(p) == T_ELSEIF || kind(p) == T_ELSE) {
            if (nends >= 32) { err_at(p, line_of(p), "too many elseif branches (limit 32)", 0); return; }
            ends[nends++] = emit(p, OP_JMP, 0, 0);
            patch(p, next, here(p));
            next = -1;
            if (accept(p, T_ELSE)) {
                expect(p, T_DO, "`do`");
                parse_block(p);
                break;
            }
            adv(p);                                 // elseif
            parse_expr(p);
            expect(p, T_DO, "`do`");
            next = emit(p, OP_JMPF, 0, 0);
            parse_block(p);
        } else break;
    }
    if (next >= 0) patch(p, next, here(p));
    for (int i = 0; i < nends; i++) patch(p, ends[i], here(p));
    expect(p, T_ENDIFCHAIN, "`endifchain`");
}

static void repeat_statement(struct parser* p) {
    adv(p);                                         // repeat
    parse_expr(p);                                  // how many times
    int var = -1;
    if (accept(p, T_AS)) {
        if (kind(p) != T_ID) { unexpected(p, "a variable name after `as`"); return; }
        var = add_local(p, cur(p)->text);
        adv(p);
    }
    expect(p, T_DO, "`do`");
    emit(p, OP_NUM, 0, add_num(p, 0));              // stack: limit, i
    int test = emit(p, OP_REP_TEST, 0, 0);
    if (var >= 0) emit(p, OP_REP_VAR, 0, var);
    parse_block(p);
    int next = emit(p, OP_REP_NEXT, 0, 0);
    patch(p, next, test);
    patch(p, test, here(p));
    emit(p, OP_POP, 0, 0);
    emit(p, OP_POP, 0, 0);
    expect(p, T_ENDIFCHAIN, "`endifchain`");
}

static void forever_statement(struct parser* p) {
    adv(p);
    expect(p, T_DO, "`do`");
    int top = here(p);
    parse_block(p);
    emit(p, OP_YIELD, 0, 0);                        // one pass per step unless the body waits longer
    int back = emit(p, OP_JMP, 0, 0);
    patch(p, back, top);
    expect(p, T_ENDIFCHAIN, "`endifchain`");
}

// checkfor <condition> <seconds> do ... endifchain: wait until the condition holds; if it does not within the time, run the
// body. (A condition that is a string is the NAME of a variable, as in the original dmccl.)
static void checkfor_statement(struct parser* p) {
    adv(p);
    int cond_start = here(p);
    parse_expr(p);
    int cond_len = here(p) - cond_start;
    struct dm_ins cond[256];                         // the condition's code is lifted out and replayed inside the wait loop
    if (p->failed) return;
    if (cond_len > 256) { err_at(p, line_of(p), "that checkfor condition is too long", 0); return; }
    memcpy(cond, p->P->code + cond_start, (size_t)cond_len * sizeof cond[0]);
    p->P->ncode = cond_start;
    parse_primary(p);                                // seconds
    expect(p, T_DO, "`do`");
    if (p->failed) return;
    emit(p, OP_DEADLINE, 0, 0);
    int loop = here(p);
    for (int i = 0; i < cond_len; i++) {
        if (GROW(p->P, code, ncode, capcode)) { err_at(p, line_of(p), "out of memory", 0); return; }
        p->P->code[p->P->ncode++] = cond[i];
    }
    int check = emit(p, OP_CHECK, 0, 0);
    int back = emit(p, OP_JMP, 0, 0);
    patch(p, back, loop);
    parse_block(p);
    patch(p, check, here(p));
    expect(p, T_ENDIFCHAIN, "`endifchain`");
}

static void above_statement(struct parser* p) {
    adv(p);
    expect(p, T_LPAREN, "`(`");
    if (p->failed) return;
    int avail = p->nranges - p->block_base;
    if (kind(p) == T_ID && !strcmp(cur(p)->text, "loop")) {         // above(loop): do everything above again, forever
        adv(p);
        expect(p, T_RPAREN, "`)`");
        emit(p, OP_YIELD, 0, 0);
        if (avail <= 0) { err_at(p, line_of(p), "above(loop) has nothing above it", 0); return; }
        int back = emit(p, OP_JMP, 0, 0);
        patch(p, back, p->ranges[p->block_base].start);
        return;
    }
    if (kind(p) != T_NUM) { unexpected(p, "a number or `loop`"); return; }
    int n = (int)cur(p)->num;
    adv(p);
    expect(p, T_RPAREN, "`)`");
    if (p->failed) return;
    if (n < 1 || n > avail) { err_at(p, line_of(p), "above() has no statement that far back", 0); return; }
    struct range r = p->ranges[p->nranges - n];            // re-run the n-th statement back by copying its code
    for (int i = r.start; i < r.end; i++) {
        if (GROW(p->P, code, ncode, capcode)) { err_at(p, line_of(p), "out of memory", 0); return; }
        p->P->code[p->P->ncode] = p->P->code[i];
        p->P->code[p->P->ncode].line = (uint16_t)line_of(p);
        p->P->ncode++;
    }
}

// An expression used as a statement, or an assignment (`x = 1`, `guy.coins += 2`).
static void expression_statement(struct parser* p) {
    parse_expr(p);
    if (p->failed) return;
    dm_program* P = p->P;
    int k = kind(p);
    struct dm_ins last = P->code[P->ncode - 1];
    if (k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ) {
        adv(p);
        int line = line_of(p);
        (void)line;
        if (last.op == OP_LOADG || last.op == OP_LOADL) {
            P->ncode--;
            if (k != T_ASSIGN) emit(p, last.op, 0, last.a);
            parse_expr(p);
            if (k == T_PLUSEQ) emit(p, OP_ADD, 0, 0);
            if (k == T_MINUSEQ) emit(p, OP_SUB, 0, 0);
            emit(p, last.op == OP_LOADG ? OP_STOREG : OP_STOREL, 0, last.a);
        } else if (last.op == OP_GETM) {
            P->ncode--;
            if (k != T_ASSIGN) { emit(p, OP_DUP, 0, 0); emit(p, OP_GETM, 0, last.a); }
            parse_expr(p);
            if (k == T_PLUSEQ) emit(p, OP_ADD, 0, 0);
            if (k == T_MINUSEQ) emit(p, OP_SUB, 0, 0);
            emit(p, OP_SETM, 0, last.a);
        } else err_at(p, line_of(p), "you can only assign to a variable or a `thing.field`", 0);
        return;
    }
    if (last.op == OP_GETM) P->code[P->ncode - 1].op = OP_CALLM;     // `child.abort` with no parens is a verb
    emit(p, OP_POP, 0, 0);
}

static void simple_statement(struct parser* p) {
    int start = here(p);
    switch (kind(p)) {
    case T_SUICIDE: adv(p); emit(p, OP_SUICIDE, 0, 0); break;
    case T_STOP: adv(p); emit(p, OP_END, 0, 0); break;
    case T_WAITFOR:
        adv(p);
        expect(p, T_COLON, "`:` after waitfor");
        parse_postfix(p);
        emit(p, OP_WAIT, 0, 0);
        break;
    case T_ABOVE: above_statement(p); break;
    default: expression_statement(p); break;
    }
    remember(p, start);
}

static void statement(struct parser* p) {
    accept(p, T_DO);                                // the original allowed `do` in front of a statement
    int start = here(p);
    switch (kind(p)) {
    case T_IF: if_statement(p); remember(p, start); return;
    case T_REPEAT: repeat_statement(p); remember(p, start); return;
    case T_FOREVER: forever_statement(p); remember(p, start); return;
    case T_CHECKFOR: checkfor_statement(p); remember(p, start); return;
    case T_WHEN: case T_THING: err_at(p, line_of(p), "`when` and `thing` go at the top level, not inside another block", 0); return;
    case T_ENDIFCHAIN: err_at(p, line_of(p), "stray `endifchain` (a `when` whose first statement is on the `do` line ends at the end of that line)", 0); return;
    case T_ELSEIF: case T_ELSE: err_at(p, line_of(p), "`elseif`/`else` without an `if`", 0); return;
    default: break;
    }
    simple_statement(p);
    while (!p->failed && accept(p, T_THEN)) { accept(p, T_DO); simple_statement(p); }
    if (p->failed) return;
    int k = kind(p);
    if (k != T_NL && !block_end(k)) {
        char buf[48], msg[96];
        size_t n = 0;
        put_str(msg, sizeof msg, &n, "unexpected");
        err_at(p, line_of(p), msg, describe(p, buf, sizeof buf));
    }
}

// ---- handlers and things --------------------------------------------------------------------------------------------

static void parse_pattern(struct parser* p, uint8_t* kind_out, int* arg_out) {
    if (kind(p) == T_STR) {
        *kind_out = PAT_TAG;
        *arg_out = add_str(p, cur(p)->text);
        adv(p);
    } else if (kind(p) == T_ID) {
        if (!strcmp(cur(p)->text, "me")) { *kind_out = PAT_ME; *arg_out = 0; }
        else { *kind_out = PAT_VAR; *arg_out = global_slot(p, cur(p)->text); }
        adv(p);
    } else unexpected(p, "a thing or a \"tag\"");
}

static int new_handler(struct parser* p, int ev) {
    dm_program* P = p->P;
    if (p->failed) return -1;
    if (GROW(P, hs, nhs, caphs)) { err_at(p, line_of(p), "out of memory", 0); return -1; }
    struct dm_handler* h = &P->hs[P->nhs];
    memset(h, 0, sizeof *h);
    h->kind = (uint8_t)ev;
    h->thing = p->thing;
    h->entry = here(p);
    h->line = line_of(p);
    return P->nhs++;
}

// `when run do` / `when tick do` / `when every 2s do` / `when key "space" do` / `when guy touches "spikes" do` / `when guy dies do`
static void when_statement(struct parser* p) {
    adv(p);                                          // when
    p->nlocals = 0;
    int h = -1;
    struct dm_handler tmp;
    memset(&tmp, 0, sizeof tmp);
    int ev = -1;
    if (kind(p) == T_ID && kind_at(p, 1) == T_DO && !strcmp(cur(p)->text, "run")) { ev = EV_RUN; adv(p); }
    else if (kind(p) == T_ID && kind_at(p, 1) == T_DO && !strcmp(cur(p)->text, "tick")) { ev = EV_TICK; adv(p); }
    else if (kind(p) == T_ID && kind_at(p, 1) == T_NUM && !strcmp(cur(p)->text, "every")) {
        adv(p);
        ev = EV_EVERY;
        tmp.every_ms = cur(p)->num * 1000.0;
        if (tmp.every_ms < 1) { err_at(p, line_of(p), "`every` needs a time above zero", 0); return; }
        adv(p);
    } else if (kind(p) == T_ID && kind_at(p, 1) == T_STR && !strcmp(cur(p)->text, "key")) {
        adv(p);
        ev = EV_KEY;
        tmp.key = add_str(p, cur(p)->text);
        adv(p);
    } else {
        parse_pattern(p, &tmp.a_kind, &tmp.a_arg);
        if (p->failed) return;
        if (kind(p) == T_ID && !strcmp(cur(p)->text, "touches")) {
            adv(p);
            ev = EV_TOUCH;
            parse_pattern(p, &tmp.b_kind, &tmp.b_arg);
        } else if (kind(p) == T_ID && !strcmp(cur(p)->text, "dies")) {
            adv(p);
            ev = EV_DIES;
        } else { unexpected(p, "`touches` or `dies`"); return; }
    }
    if (p->failed) return;
    if (!expect(p, T_DO, "`do`")) return;
    h = new_handler(p, ev);
    if (h < 0) return;
    {
        struct dm_handler* H = &p->P->hs[h];
        H->a_kind = tmp.a_kind; H->a_arg = tmp.a_arg; H->b_kind = tmp.b_kind; H->b_arg = tmp.b_arg;
        H->every_ms = tmp.every_ms; H->key = tmp.key;
    }
    if (kind(p) != T_NL) {                           // one-liner: `when run do child.abort then suicide`
        int saved = p->block_base;
        p->block_base = p->nranges;
        statement(p);
        p->nranges = p->block_base;
        p->block_base = saved;
        if (!p->failed && kind(p) != T_NL && kind(p) != T_EOF) unexpected(p, "the end of the line");
    } else {
        parse_block(p);
        if (!p->failed && kind(p) != T_ENDIFCHAIN) {
            unexpected(p, "`endifchain`");
        } else adv(p);
    }
    emit(p, OP_END, 0, 0);
}

static void thing_statement(struct parser* p) {
    dm_program* P = p->P;
    adv(p);                                          // thing
    if (kind(p) != T_ID) { unexpected(p, "a name for the thing"); return; }
    if (strlen(cur(p)->text) >= 32) { err_at(p, line_of(p), "thing name too long", 0); return; }
    for (int i = 0; i < P->nthings; i++)
        if (!strcmp(P->things[i].name, cur(p)->text)) { err_at(p, line_of(p), "there is already a thing called", cur(p)->text); return; }
    if (GROW(P, things, nthings, capthings)) { err_at(p, line_of(p), "out of memory", 0); return; }
    struct dm_thing* T = &P->things[P->nthings];
    memset(T, 0, sizeof *T);
    strcpy(T->name, cur(p)->text);
    adv(p);
    strcpy(T->base, "box");
    if (accept(p, T_IS)) {                          // thing bomb is "ball" do
        if (kind(p) != T_STR || strlen(cur(p)->text) >= 32) { unexpected(p, "a kind in quotes after `is`"); return; }
        strcpy(T->base, cur(p)->text);
        adv(p);
    }
    expect(p, T_DO, "`do`");
    int index = P->nthings++;
    p->thing = index;
    P->things[index].first = P->nhs;
    skip_nl(p);
    while (!p->failed && kind(p) != T_ENDIFCHAIN) {
        if (kind(p) != T_WHEN) { unexpected(p, "`when` (a thing only holds handlers)"); break; }
        when_statement(p);
        skip_nl(p);
    }
    P->things[index].count = P->nhs - P->things[index].first;
    p->thing = -1;
    expect(p, T_ENDIFCHAIN, "`endifchain`");
}

static void import_statement(struct parser* p) {
    dm_program* P = p->P;
    adv(p);                                          // from
    if (kind(p) != T_ID) { unexpected(p, "a library name"); return; }
    adv(p);
    if (!expect(p, T_IMPORT, "`import`")) return;
    if (kind(p) != T_ID || strlen(cur(p)->text) >= 24) { unexpected(p, "a module name"); return; }
    for (int i = 0; i < P->nimports; i++) if (!strcmp(P->imports[i], cur(p)->text)) { adv(p); return; }
    if (GROW(P, imports, nimports, capimports)) { err_at(p, line_of(p), "out of memory", 0); return; }
    strcpy(P->imports[P->nimports++], cur(p)->text);
    adv(p);
}

// Loose statements at the top level run once at the start, like `when run`.
static void loose_statements(struct parser* p) {
    p->nlocals = 0;
    new_handler(p, EV_RUN);
    int saved = p->block_base;
    p->block_base = p->nranges;
    while (!p->failed && kind(p) != T_EOF && kind(p) != T_WHEN && kind(p) != T_THING && kind(p) != T_FROM) {
        statement(p);
        skip_nl(p);
    }
    p->nranges = p->block_base;
    p->block_base = saved;
    emit(p, OP_END, 0, 0);
}

int dm_compile_tokens(struct dm_tokens* toks, dm_program* out, char* err, size_t err_cap) {
    struct parser* p = (struct parser*)DM_ALLOC(sizeof *p);
    if (!p) { if (err_cap) { strncpy(err, "out of memory", err_cap); err[err_cap - 1] = 0; } return -1; }
    p->tk = toks;
    p->P = out;
    p->err = err;
    p->err_cap = err_cap;
    p->thing = -1;
    if (err_cap) err[0] = 0;
    skip_nl(p);
    while (!p->failed && kind(p) != T_EOF) {
        switch (kind(p)) {
        case T_FROM: import_statement(p); break;
        case T_WHEN: when_statement(p); break;
        case T_THING: thing_statement(p); break;
        default: loose_statements(p); break;
        }
        skip_nl(p);
    }
    int failed = p->failed;
    DM_FREE(p);
    return failed ? -1 : 0;
}

// ---- public: compile / free / facts ---------------------------------------------------------------------------------

dm_program* dm_compile(const char* src, size_t len, char* err, size_t err_cap) {
    struct dm_tokens toks;
    if (err_cap) err[0] = 0;
    if (dm_lex(src, len, &toks, err, err_cap)) return 0;
    dm_program* P = (dm_program*)DM_ALLOC(sizeof *P);
    if (!P) { dm_tokens_free(&toks); if (err_cap) { strncpy(err, "out of memory", err_cap); err[err_cap - 1] = 0; } return 0; }
    int bad = dm_compile_tokens(&toks, P, err, err_cap);
    dm_tokens_free(&toks);
    if (bad) { dm_program_free(P); return 0; }
    return P;
}

void dm_program_free(dm_program* P) {
    if (!P) return;
    for (int i = 0; i < P->nstr; i++) DM_FREE(P->strs[i]);
    DM_FREE(P->code); DM_FREE(P->nums); DM_FREE(P->strs); DM_FREE(P->globals); DM_FREE(P->calls);
    DM_FREE(P->imports); DM_FREE(P->hs); DM_FREE(P->things);
    DM_FREE(P);
}

int dm_thing_count(const dm_program* p) { return p->nthings; }
const char* dm_thing_name(const dm_program* p, int i) { return i >= 0 && i < p->nthings ? p->things[i].name : 0; }
const char* dm_thing_base(const dm_program* p, const char* thing) {
    for (int i = 0; i < p->nthings; i++) if (!strcmp(p->things[i].name, thing)) return p->things[i].base;
    return 0;
}
