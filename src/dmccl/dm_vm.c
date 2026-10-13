// dmccl VM: runs the bytecode as many small threads (one per running handler), cooperatively.
//
// A thread runs until it yields (waitfor, checkfor, a `forever` pass, or a full slice of instructions), so a script can never
// hang the game. Time is the game's: dm_step(dt) advances the VM clock and wakes sleepers.
//
// Memory: strings are heap blocks on one list and a mark-and-sweep pass frees the unreachable ones between steps. Roots are
// the globals, every thread's stack and loop variables, and the entity fields. Natives never see a collection (it only runs
// at the end of dm_step), so a native may hold the strings it is working with.
#include "dm_internal.h"

#define GC_FIRST 64u * 1024

int dm_str_eq(const char* a, const char* b) { return !strcmp(a, b); }

// ---- strings --------------------------------------------------------------------------------------------------------

static struct dm_str* str_alloc(dm_vm* vm, const char* text, size_t len) {
    struct dm_str* s = (struct dm_str*)DM_ALLOC(sizeof *s + len);
    if (!s) return 0;
    s->len = (uint32_t)len;
    memcpy(s->text, text, len);
    s->text[len] = 0;
    s->next = vm->strs;
    vm->strs = s;
    vm->str_bytes += sizeof *s + len;
    return s;
}

dm_val dm_str_new(dm_vm* vm, const char* text) {
    if (!text) return dm_nil();
    struct dm_str* s = str_alloc(vm, text, strlen(text));
    if (!s) return dm_nil();
    dm_val v;
    v.t = DM_STR;
    v.u.s = s;
    return v;
}

static dm_val str_from(dm_vm* vm, const char* a, size_t na, const char* b, size_t nb) {
    struct dm_str* s = (struct dm_str*)DM_ALLOC(sizeof *s + na + nb);
    if (!s) return dm_nil();
    s->len = (uint32_t)(na + nb);
    memcpy(s->text, a, na);
    memcpy(s->text + na, b, nb);
    s->text[na + nb] = 0;
    s->next = vm->strs;
    vm->strs = s;
    vm->str_bytes += sizeof *s + na + nb;
    dm_val v;
    v.t = DM_STR;
    v.u.s = s;
    return v;
}

const char* dm_text(const dm_val* v) { return v->t == DM_STR ? v->u.s->text : ""; }

static void mark_val(const dm_val* v) { if (v->t == DM_STR) v->u.s->mark = 1; }

static void gc(dm_vm* vm) {
    for (struct dm_str* s = vm->strs; s; s = s->next) s->mark = s->pinned;
    for (int i = 0; i < vm->prog->nglob; i++) mark_val(&vm->globals[i]);
    for (int i = 0; i < DM_MAX_THREADS; i++) {
        struct dm_thread* t = &vm->th[i];
        if (!t->used) continue;
        for (int k = 0; k < t->sp; k++) mark_val(&t->st[k]);
        for (int k = 0; k < DM_MAX_LOCALS; k++) mark_val(&t->loc[k]);
    }
    for (int i = 0; i < DM_MAX_FIELDS; i++) if (vm->fields[i].used) mark_val(&vm->fields[i].v);
    struct dm_str** link = &vm->strs;
    size_t bytes = 0;
    while (*link) {
        struct dm_str* s = *link;
        if (s->mark) { bytes += sizeof *s + s->len; link = &s->next; }
        else { *link = s->next; DM_FREE(s); }
    }
    vm->str_bytes = bytes;
    vm->gc_at = bytes * 2 > GC_FIRST ? bytes * 2 : GC_FIRST;
}

// ---- numbers as text ------------------------------------------------------------------------------------------------

void dm_fmt_num(double n, char* out) {
    int k = 0;
    if (n != n) { strcpy(out, "nan"); return; }
    if (n > 1e15 || n < -1e15) { strcpy(out, n > 0 ? "huge" : "-huge"); return; }
    if (n < 0) { out[k++] = '-'; n = -n; }
    double ip = floor(n);
    double frac = n - ip;
    long long whole = (long long)ip;
    long long scaled = (long long)(frac * 1000000.0 + 0.5);       // six decimals, then trimmed
    if (scaled >= 1000000) { whole++; scaled -= 1000000; }
    char digits[24];
    int d = 0;
    do { digits[d++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (d) out[k++] = digits[--d];
    if (scaled) {
        out[k++] = '.';
        char frac_digits[6];
        for (int i = 5; i >= 0; i--) { frac_digits[i] = (char)('0' + scaled % 10); scaled /= 10; }
        int last = 5;
        while (last > 0 && frac_digits[last] == '0') last--;
        for (int i = 0; i <= last; i++) out[k++] = frac_digits[i];
    }
    out[k] = 0;
}

// Text of any value (for log:print and `+` with a string).
static const char* val_text(const dm_val* v, char* buf) {
    switch (v->t) {
    case DM_NUM: dm_fmt_num(v->u.n, buf); return buf;
    case DM_STR: return v->u.s->text;
    case DM_BOOL: return v->u.b ? "true" : "false";
    case DM_ENT: strcpy(buf, "<thing "); { char n[32]; dm_fmt_num(v->u.ent, n); strcat(buf, n); } strcat(buf, ">"); return buf;
    default: return "nil";
    }
}

// ---- the VM object --------------------------------------------------------------------------------------------------

dm_vm* dm_vm_new(dm_program* prog, const struct dm_host* host) {
    dm_vm* vm = (dm_vm*)DM_ALLOC(sizeof *vm);
    if (!vm) return 0;
    vm->prog = prog;
    if (host) vm->host = *host;
    vm->cur = -1;
    vm->gc_at = GC_FIRST;
    vm->globals = (dm_val*)DM_ALLOC((size_t)(prog->nglob + 1) * sizeof(dm_val));
    vm->call_fn = (dm_native*)DM_ALLOC((size_t)(prog->ncalls + 1) * sizeof(dm_native));
    vm->call_ctx = (void**)DM_ALLOC((size_t)(prog->ncalls + 1) * sizeof(void*));
    vm->th = (struct dm_thread*)DM_ALLOC(DM_MAX_THREADS * sizeof(struct dm_thread));
    vm->inst = (struct dm_inst*)DM_ALLOC(DM_MAX_INSTS * sizeof(struct dm_inst));
    vm->timers = (struct dm_timer*)DM_ALLOC(DM_MAX_TIMERS * sizeof(struct dm_timer));
    vm->fields = (struct dm_field*)DM_ALLOC(DM_MAX_FIELDS * sizeof(struct dm_field));
    vm->str_const = (dm_val*)DM_ALLOC((size_t)(prog->nstr + 1) * sizeof(dm_val));
    vm->tick_busy = (uint8_t*)DM_ALLOC((size_t)(prog->nhs + 1));
    if (!vm->globals || !vm->call_fn || !vm->call_ctx || !vm->th || !vm->inst || !vm->timers || !vm->fields || !vm->str_const || !vm->tick_busy) {
        dm_vm_free(vm);
        return 0;
    }
    for (int i = 0; i < prog->nstr; i++) {           // constants live as long as the VM
        vm->str_const[i] = dm_str_new(vm, prog->strs[i]);
        if (vm->str_const[i].t == DM_STR) vm->str_const[i].u.s->pinned = 1;
    }
    return vm;
}

void dm_vm_free(dm_vm* vm) {
    if (!vm) return;
    struct dm_str* s = vm->strs;
    while (s) { struct dm_str* n = s->next; DM_FREE(s); s = n; }
    DM_FREE(vm->globals); DM_FREE(vm->call_fn); DM_FREE(vm->call_ctx); DM_FREE(vm->th); DM_FREE(vm->inst);
    DM_FREE(vm->timers); DM_FREE(vm->fields); DM_FREE(vm->str_const); DM_FREE(vm->tick_busy); DM_FREE(vm->natives);
    DM_FREE(vm);
}

void dm_register(dm_vm* vm, const char* module, const char* name, dm_native fn, void* ctx) {
    if (vm->nnatives == vm->capnatives) {
        int cap = vm->capnatives ? vm->capnatives * 2 : 64;
        struct dm_native_entry* g = (struct dm_native_entry*)DM_REALLOC(vm->natives, (size_t)cap * sizeof *g);
        if (!g) return;
        vm->natives = g;
        vm->capnatives = cap;
    }
    struct dm_native_entry* e = &vm->natives[vm->nnatives++];
    strncpy(e->module, module, sizeof e->module - 1);
    e->module[sizeof e->module - 1] = 0;
    strncpy(e->name, name, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = 0;
    e->fn = fn;
    e->ctx = ctx;
}

int dm_fail(dm_vm* vm, const char* msg) {
    strncpy(vm->err, msg, sizeof vm->err - 1);
    vm->err[sizeof vm->err - 1] = 0;
    return -1;
}

static void cat(char* dst, size_t cap, const char* s) {
    size_t n = strlen(dst);
    while (*s && n + 1 < cap) dst[n++] = *s++;
    dst[n] = 0;
}

// Resolve every call in the program to a native, and check the `from dexlib import ...` lines.
int dm_link(dm_vm* vm, char* err, size_t err_cap) {
    dm_program* P = vm->prog;
    if (err_cap) err[0] = 0;
    // imports must name a module somebody registered
    for (int i = 0; i < P->nimports; i++) {
        int known = !strcmp(P->imports[i], "basics");
        for (int k = 0; k < vm->nnatives && !known; k++) known = !strcmp(vm->natives[k].module, P->imports[i]);
        if (!known) {
            if (err_cap) { err[0] = 0; cat(err, err_cap, "there is no module `dexlib."); cat(err, err_cap, P->imports[i]); cat(err, err_cap, "`"); }
            return -1;
        }
    }
    for (int c = 0; c < P->ncalls; c++) {
        const struct dm_call* call = &P->calls[c];
        int found = -1;
        for (int k = 0; k < vm->nnatives; k++) {
            const struct dm_native_entry* e = &vm->natives[k];
            if (strcmp(e->name, call->name)) continue;
            if (call->module[0]) { if (strcmp(e->module, call->module)) continue; }
            else {                                           // unqualified: any module the program imported (or basics)
                int ok = !strcmp(e->module, "basics") || !strcmp(e->module, "log");
                for (int i = 0; i < P->nimports && !ok; i++) ok = !strcmp(P->imports[i], e->module);
                if (!ok) continue;
            }
            found = k;
            break;
        }
        if (found < 0) {
            if (err_cap) {
                err[0] = 0;
                cat(err, err_cap, "unknown function ");
                if (call->module[0]) { cat(err, err_cap, call->module); cat(err, err_cap, ":"); }
                cat(err, err_cap, call->name);
                cat(err, err_cap, " (line ");
                char n[32];
                dm_fmt_num(call->line, n);
                cat(err, err_cap, n);
                cat(err, err_cap, ")");
            }
            return -1;
        }
        // a qualified call needs its module imported, except the built-in `log:`
        if (call->module[0] && strcmp(call->module, "log")) {
            int imported = 0;
            for (int i = 0; i < P->nimports; i++) if (!strcmp(P->imports[i], call->module)) imported = 1;
            if (!imported) {
                if (err_cap) {
                    err[0] = 0;
                    cat(err, err_cap, "line ");
                    char n[32];
                    dm_fmt_num(call->line, n);
                    cat(err, err_cap, n);
                    cat(err, err_cap, ": ");
                    cat(err, err_cap, call->module);
                    cat(err, err_cap, ":");
                    cat(err, err_cap, call->name);
                    cat(err, err_cap, " needs `from dexlib import ");
                    cat(err, err_cap, call->module);
                    cat(err, err_cap, "` first");
                }
                return -1;
            }
        }
        vm->call_fn[c] = vm->natives[found].fn;
        vm->call_ctx[c] = vm->natives[found].ctx;
    }
    return 0;
}

// ---- entity fields --------------------------------------------------------------------------------------------------

static struct dm_field* field_find(dm_vm* vm, int ent, const char* name) {
    for (int i = 0; i < DM_MAX_FIELDS; i++)
        if (vm->fields[i].used && vm->fields[i].ent == ent && vm->fields[i].name == name) return &vm->fields[i];
    return 0;
}

static int field_set(dm_vm* vm, int ent, const char* name, dm_val v) {
    struct dm_field* f = field_find(vm, ent, name);
    if (!f) for (int i = 0; i < DM_MAX_FIELDS; i++) if (!vm->fields[i].used) { f = &vm->fields[i]; break; }
    if (!f) return -1;
    f->used = 1;
    f->ent = ent;
    f->name = name;
    f->v = v;
    return 0;
}

// ---- threads --------------------------------------------------------------------------------------------------------

static struct dm_thread* thread_start(dm_vm* vm, int handler, int inst, int me, int who, int hit) {
    for (int i = 0; i < DM_MAX_THREADS; i++) {
        struct dm_thread* t = &vm->th[i];
        if (t->used) continue;
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->handler = handler;
        t->inst = inst;
        t->me = me;
        t->who = who;
        t->hit = hit;
        t->ip = vm->prog->hs[handler].entry;
        t->wake_at = vm->now_ms;
        return t;
    }
    return 0;           // no free thread: the event is dropped (a script that spawns thousands of waiting handlers)
}

static int inst_ent(dm_vm* vm, int inst) { return inst >= 0 ? vm->inst[inst].ent : -1; }

static void log_line(dm_vm* vm, const char* line) {
    if (vm->host.print) vm->host.print(vm->host.user, line);
}

static void runtime_error(dm_vm* vm, struct dm_thread* t, int ip, const char* msg) {
    char line[160] = "[dmccl] error at line ";
    char n[32];
    dm_fmt_num(vm->prog->code[ip].line, n);
    cat(line, sizeof line, n);
    cat(line, sizeof line, ": ");
    cat(line, sizeof line, msg);
    log_line(vm, line);
    t->used = 0;
}

static int truthy(dm_vm* vm, const dm_val* v) {
    switch (v->t) {
    case DM_NUM: return v->u.n != 0;
    case DM_STR: return v->u.s->len != 0;
    case DM_BOOL: return v->u.b;
    case DM_ENT: return v->u.ent >= 0 && (!vm->host.alive || vm->host.alive(vm, v->u.ent));
    default: return 0;
    }
}

static int equal(const dm_val* a, const dm_val* b) {
    if (a->t != b->t) return 0;
    switch (a->t) {
    case DM_NUM: return a->u.n == b->u.n;
    case DM_STR: return a->u.s == b->u.s || !strcmp(a->u.s->text, b->u.s->text);
    case DM_BOOL: return a->u.b == b->u.b;
    case DM_ENT: return a->u.ent == b->u.ent;
    default: return 1;
    }
}

static int global_by_name(dm_vm* vm, const char* name) {
    for (int i = 0; i < vm->prog->nglob; i++) if (!strcmp(vm->prog->globals[i], name)) return i;
    return -1;
}

static dm_val context(dm_vm* vm, struct dm_thread* t, int which) {
    switch (which) {
    case DM_CTX_ME: return t->me >= 0 ? dm_ent(t->me) : dm_nil();
    case DM_CTX_WHO: return t->who >= 0 ? dm_ent(t->who) : dm_nil();
    case DM_CTX_HIT: return t->hit >= 0 ? dm_ent(t->hit) : dm_nil();
    case DM_CTX_TIME: return dm_num(vm->now_ms / 1000.0);
    default: {
        dm_val out = dm_nil();
        if (vm->host.ctx && t->me >= 0) vm->host.ctx(vm, t->me, which == DM_CTX_CHILD ? "child" : "parent", &out);
        return out;
    }
    }
}

// Member of an entity: the game first, then the fields the script stored itself. Dead or missing things read as nil.
static dm_val member_get(dm_vm* vm, const dm_val* obj, const char* name) {
    dm_val out = dm_nil();
    if (obj->t != DM_ENT) return out;
    if (vm->host.get && vm->host.get(vm, obj->u.ent, name, &out)) return out;
    struct dm_field* f = field_find(vm, obj->u.ent, name);
    return f ? f->v : dm_nil();
}

enum { RUN_YIELD, RUN_DONE, RUN_ERROR };

// Runs thread `ti` until it yields or finishes. A thread slot may be freed while it runs (suicide), so state goes through
// the slot only while it still `used`.
static int run_thread(dm_vm* vm, int ti) {
    struct dm_thread* t = &vm->th[ti];
    dm_program* P = vm->prog;
    int slice = DM_SLICE;
    vm->cur = ti;
#define POP() (t->st[--t->sp])
#define PUSH(v) (t->st[t->sp++] = (v))
#define FAIL(msg) do { runtime_error(vm, t, ip, msg); vm->cur = -1; return RUN_ERROR; } while (0)
    for (;;) {
        if (--slice < 0 || --vm->step_left < 0) { t->wake_at = vm->now_ms; vm->cur = -1; return RUN_YIELD; }
        int ip = t->ip;
        const struct dm_ins* I = &P->code[ip];
        if (t->sp > DM_STACK - 10) FAIL("the script piled up too many values (an expression nested too deep)");
        t->ip = ip + 1;
        switch (I->op) {
        case OP_NIL: PUSH(dm_nil()); break;
        case OP_TRUE: PUSH(dm_bool(1)); break;
        case OP_FALSE: PUSH(dm_bool(0)); break;
        case OP_NUM: PUSH(dm_num(P->nums[I->a])); break;
        case OP_STR: PUSH(vm->str_const[I->a]); break;
        case OP_POP: t->sp--; break;
        case OP_DUP: { dm_val v = t->st[t->sp - 1]; PUSH(v); break; }
        case OP_NEG: {
            dm_val v = POP();
            if (v.t != DM_NUM) FAIL("`-` needs a number");
            PUSH(dm_num(-v.u.n));
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
            dm_val b = POP(), a = POP();
            if (I->op == OP_ADD && (a.t == DM_STR || b.t == DM_STR)) {          // "text" + anything = text
                char ba[40], bb[40];
                const char* x = val_text(&a, ba);
                const char* y = val_text(&b, bb);
                dm_val s = str_from(vm, x, strlen(x), y, strlen(y));
                PUSH(s);
                break;
            }
            if (a.t != DM_NUM || b.t != DM_NUM) FAIL("math needs numbers (use + to join text)");
            double r;
            switch (I->op) {
            case OP_ADD: r = a.u.n + b.u.n; break;
            case OP_SUB: r = a.u.n - b.u.n; break;
            case OP_MUL: r = a.u.n * b.u.n; break;
            case OP_DIV: if (b.u.n == 0) FAIL("division by zero"); r = a.u.n / b.u.n; break;
            default: if (b.u.n == 0) FAIL("modulo by zero"); r = fmod(a.u.n, b.u.n); break;
            }
            PUSH(dm_num(r));
            break;
        }
        case OP_EQ: case OP_NE: {
            dm_val b = POP(), a = POP();
            int e = equal(&a, &b);
            PUSH(dm_bool(I->op == OP_EQ ? e : !e));
            break;
        }
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
            dm_val b = POP(), a = POP();
            double x, y;
            if (a.t == DM_NUM && b.t == DM_NUM) { x = a.u.n; y = b.u.n; }
            else if (a.t == DM_STR && b.t == DM_STR) { x = strcmp(a.u.s->text, b.u.s->text); y = 0; }
            else FAIL("cannot compare these (numbers with numbers, text with text)");
            int r = I->op == OP_LT ? x < y : I->op == OP_GT ? x > y : I->op == OP_LE ? x <= y : x >= y;
            PUSH(dm_bool(r));
            break;
        }
        case OP_AND: case OP_OR: FAIL("internal: and/or are compiled to jumps");
        case OP_NOT: { dm_val v = POP(); PUSH(dm_bool(!truthy(vm, &v))); break; }
        case OP_LOADG: PUSH(vm->globals[I->a]); break;
        case OP_STOREG: vm->globals[I->a] = POP(); break;
        case OP_LOADL: PUSH(t->loc[I->a]); break;
        case OP_STOREL: t->loc[I->a] = POP(); break;
        case OP_CTX: PUSH(context(vm, t, I->a)); break;
        case OP_GETM: {
            dm_val obj = POP();
            PUSH(member_get(vm, &obj, P->strs[I->a]));
            break;
        }
        case OP_SETM: {
            dm_val v = POP(), obj = POP();
            if (obj.t != DM_ENT) break;                                         // setting a field of nothing does nothing
            const char* name = P->strs[I->a];
            if (vm->host.set && vm->host.set(vm, obj.u.ent, name, v)) break;
            if (field_set(vm, obj.u.ent, name, v)) FAIL("too many custom fields in the level");
            break;
        }
        case OP_CALLM: {
            int argc = I->n;
            dm_val args[8];
            for (int i = argc - 1; i >= 0; i--) args[i] = POP();
            dm_val obj = POP();
            dm_val ret = dm_nil();
            if (obj.t == DM_ENT) {
                vm->err[0] = 0;
                int ok = vm->host.act ? vm->host.act(vm, obj.u.ent, P->strs[I->a], argc, args, &ret) : 0;
                if (ok < 0) { vm->cur = -1; runtime_error(vm, t, ip, vm->err[0] ? vm->err : "that failed"); return RUN_ERROR; }
                if (!ok) {
                    char msg[96] = "this thing cannot `";
                    cat(msg, sizeof msg, P->strs[I->a]);
                    cat(msg, sizeof msg, "`");
                    FAIL(msg);
                }
            }
            if (!t->used) { vm->cur = -1; return RUN_DONE; }                    // the call killed this very thread
            PUSH(ret);
            break;
        }
        case OP_CALL: {
            int argc = I->n;
            dm_val args[8];
            for (int i = argc - 1; i >= 0; i--) args[i] = POP();
            dm_val ret = dm_nil();
            vm->err[0] = 0;
            int bad = vm->call_fn[I->a](vm, argc, args, &ret, vm->call_ctx[I->a]);
            if (bad) { vm->cur = -1; runtime_error(vm, t, ip, vm->err[0] ? vm->err : "a function failed"); return RUN_ERROR; }
            if (!t->used) { vm->cur = -1; return RUN_DONE; }
            PUSH(ret);
            break;
        }
        case OP_JMP: t->ip = ip + I->a; break;
        case OP_JMPF: { dm_val v = POP(); if (!truthy(vm, &v)) t->ip = ip + I->a; break; }
        case OP_WAIT: {
            dm_val v = POP();
            if (v.t != DM_NUM) FAIL("waitfor needs a number of seconds");
            t->wake_at = vm->now_ms + v.u.n * 1000.0;
            vm->cur = -1;
            return RUN_YIELD;
        }
        case OP_DEADLINE: {
            dm_val v = POP();
            if (v.t != DM_NUM) FAIL("checkfor needs a number of seconds");
            PUSH(dm_num(vm->now_ms + v.u.n * 1000.0));
            break;
        }
        case OP_CHECK: {
            dm_val cond = POP();
            int is_true;
            if (cond.t == DM_STR) {                                             // original dmccl: the NAME of a variable
                int g = global_by_name(vm, cond.u.s->text);
                is_true = g >= 0 && truthy(vm, &vm->globals[g]);
            } else is_true = truthy(vm, &cond);
            if (is_true) { t->sp--; t->ip = ip + I->a; break; }                 // happened in time: skip the body
            if (vm->now_ms >= t->st[t->sp - 1].u.n) { t->sp--; t->ip = ip + 2; break; }   // too late: run the body
            t->wake_at = vm->now_ms;                                            // look again next step
            vm->cur = -1;
            return RUN_YIELD;
        }
        case OP_REP_TEST: {
            dm_val i = t->st[t->sp - 1], limit = t->st[t->sp - 2];
            if (limit.t != DM_NUM) FAIL("repeat needs a number of times");
            if (i.u.n >= limit.u.n) t->ip = ip + I->a;
            break;
        }
        case OP_REP_VAR: t->loc[I->a] = t->st[t->sp - 1]; break;
        case OP_REP_NEXT: t->st[t->sp - 1].u.n += 1; t->ip = ip + I->a; break;
        case OP_SUICIDE:
            vm->cur = -1;
            if (t->me < 0) { vm->halted = 1; if (vm->host.suicide) vm->host.suicide(vm, -1); }
            else if (vm->host.suicide) vm->host.suicide(vm, t->me);
            else dm_ent_gone(vm, t->me);
            t->used = 0;
            return RUN_DONE;
        case OP_YIELD: t->wake_at = vm->now_ms; vm->cur = -1; return RUN_YIELD;
        case OP_END: default: t->used = 0; vm->cur = -1; return RUN_DONE;
        }
    }
#undef POP
#undef PUSH
#undef FAIL
}

// ---- events and time ------------------------------------------------------------------------------------------------

static void add_timer(dm_vm* vm, int handler, int inst) {
    for (int i = 0; i < DM_MAX_TIMERS; i++)
        if (!vm->timers[i].used) {
            vm->timers[i].used = 1;
            vm->timers[i].handler = handler;
            vm->timers[i].inst = inst;
            vm->timers[i].next_ms = vm->now_ms + vm->prog->hs[handler].every_ms;
            return;
        }
}

static void start_handlers_of(dm_vm* vm, int first, int count, int inst) {
    int me = inst_ent(vm, inst);
    for (int h = first; h < first + count; h++) {
        if (vm->prog->hs[h].kind == EV_RUN) thread_start(vm, h, inst, me, -1, -1);
        else if (vm->prog->hs[h].kind == EV_EVERY) add_timer(vm, h, inst);
    }
}

void dm_start(dm_vm* vm) {
    dm_program* P = vm->prog;
    vm->started = 1;
    for (int h = 0; h < P->nhs; h++) {
        if (P->hs[h].thing >= 0) continue;
        if (P->hs[h].kind == EV_RUN) thread_start(vm, h, -1, -1, -1, -1);
        else if (P->hs[h].kind == EV_EVERY) add_timer(vm, h, -1);
    }
}

int dm_spawn_thing(dm_vm* vm, const char* thing, int ent) {
    dm_program* P = vm->prog;
    for (int i = 0; i < P->nthings; i++) {
        if (strcmp(P->things[i].name, thing)) continue;
        for (int k = 0; k < DM_MAX_INSTS; k++) {
            if (vm->inst[k].used) continue;
            vm->inst[k].used = 1;
            vm->inst[k].ent = ent;
            vm->inst[k].thing = i;
            start_handlers_of(vm, P->things[i].first, P->things[i].count, k);
            return 1;
        }
        return 0;
    }
    return 0;
}

void dm_ent_gone(dm_vm* vm, int ent) {
    for (int i = 0; i < DM_MAX_THREADS; i++)
        if (vm->th[i].used && vm->th[i].me == ent && i != vm->cur) vm->th[i].used = 0;
    if (vm->cur >= 0 && vm->th[vm->cur].me == ent) vm->th[vm->cur].used = 0;
    for (int i = 0; i < DM_MAX_INSTS; i++)
        if (vm->inst[i].used && vm->inst[i].ent == ent) {
            for (int k = 0; k < DM_MAX_TIMERS; k++) if (vm->timers[k].used && vm->timers[k].inst == i) vm->timers[k].used = 0;
            vm->inst[i].used = 0;
        }
    for (int i = 0; i < DM_MAX_FIELDS; i++) if (vm->fields[i].used && vm->fields[i].ent == ent) vm->fields[i].used = 0;
}

// Does pattern (kind, arg) match entity `ent` carrying `tag`?
static int pattern_hits(dm_vm* vm, int kind, int arg, int inst, int ent, const char* tag) {
    switch (kind) {
    case PAT_VAR: { const dm_val* g = &vm->globals[arg]; return g->t == DM_ENT && g->u.ent == ent; }
    case PAT_TAG: return tag && !strcmp(tag, vm->prog->strs[arg]);
    case PAT_ME: return inst >= 0 && vm->inst[inst].ent == ent;
    default: return 0;
    }
}

static void fire_pair(dm_vm* vm, int ev, int a, const char* a_tag, int b, const char* b_tag) {
    dm_program* P = vm->prog;
    for (int h = 0; h < P->nhs; h++) {
        const struct dm_handler* H = &P->hs[h];
        if (H->kind != ev) continue;
        if (H->thing < 0) {
            if (pattern_hits(vm, H->a_kind, H->a_arg, -1, a, a_tag) && (ev == EV_DIES || pattern_hits(vm, H->b_kind, H->b_arg, -1, b, b_tag)))
                thread_start(vm, h, -1, -1, a, b);
            else if (ev == EV_TOUCH && pattern_hits(vm, H->a_kind, H->a_arg, -1, b, b_tag) && pattern_hits(vm, H->b_kind, H->b_arg, -1, a, a_tag))
                thread_start(vm, h, -1, -1, b, a);
            continue;
        }
        for (int k = 0; k < DM_MAX_INSTS; k++) {        // a thing's own handler: once per instance of that thing
            if (!vm->inst[k].used || vm->inst[k].thing != H->thing) continue;
            int me = vm->inst[k].ent;
            if (pattern_hits(vm, H->a_kind, H->a_arg, k, a, a_tag) && (ev == EV_DIES || pattern_hits(vm, H->b_kind, H->b_arg, k, b, b_tag)))
                thread_start(vm, h, k, me, a, b);
            else if (ev == EV_TOUCH && pattern_hits(vm, H->a_kind, H->a_arg, k, b, b_tag) && pattern_hits(vm, H->b_kind, H->b_arg, k, a, a_tag))
                thread_start(vm, h, k, me, b, a);
        }
    }
}

void dm_fire_touch(dm_vm* vm, int a, const char* a_tag, int b, const char* b_tag) { if (!vm->halted) fire_pair(vm, EV_TOUCH, a, a_tag, b, b_tag); }
void dm_fire_dies(dm_vm* vm, int ent, const char* tag) { if (!vm->halted) fire_pair(vm, EV_DIES, ent, tag, -1, 0); }

void dm_fire_key(dm_vm* vm, const char* key) {
    dm_program* P = vm->prog;
    if (vm->halted) return;
    for (int h = 0; h < P->nhs; h++) {
        if (P->hs[h].kind != EV_KEY || strcmp(P->strs[P->hs[h].key], key)) continue;
        if (P->hs[h].thing < 0) { thread_start(vm, h, -1, -1, -1, -1); continue; }
        for (int k = 0; k < DM_MAX_INSTS; k++)
            if (vm->inst[k].used && vm->inst[k].thing == P->hs[h].thing) thread_start(vm, h, k, vm->inst[k].ent, -1, -1);
    }
}

static int handler_busy(dm_vm* vm, int h, int inst) {
    for (int i = 0; i < DM_MAX_THREADS; i++) if (vm->th[i].used && vm->th[i].handler == h && vm->th[i].inst == inst) return 1;
    return 0;
}

void dm_step(dm_vm* vm, int dt_ms) {
    dm_program* P = vm->prog;
    if (!vm->started || vm->halted) return;
    vm->now_ms += dt_ms;
    // `every N`: fire when due, catching up at most once per step
    for (int i = 0; i < DM_MAX_TIMERS; i++) {
        struct dm_timer* tm = &vm->timers[i];
        if (!tm->used || vm->now_ms < tm->next_ms) continue;
        double every = P->hs[tm->handler].every_ms;
        tm->next_ms += every;
        if (tm->next_ms < vm->now_ms) tm->next_ms = vm->now_ms + every;
        thread_start(vm, tm->handler, tm->inst, inst_ent(vm, tm->inst), -1, -1);
    }
    // `tick`: once per step, unless the previous one is still running
    for (int h = 0; h < P->nhs; h++) {
        if (P->hs[h].kind != EV_TICK) continue;
        if (P->hs[h].thing < 0) { if (!handler_busy(vm, h, -1)) thread_start(vm, h, -1, -1, -1, -1); continue; }
        for (int k = 0; k < DM_MAX_INSTS; k++)
            if (vm->inst[k].used && vm->inst[k].thing == P->hs[h].thing && !handler_busy(vm, h, k)) thread_start(vm, h, k, vm->inst[k].ent, -1, -1);
    }
    vm->step_left = DM_STEP_BUDGET;
    for (int i = 0; i < DM_MAX_THREADS && !vm->halted; i++) {
        struct dm_thread* t = &vm->th[i];
        if (!t->used || t->wake_at > vm->now_ms) continue;
        if (vm->step_left <= 0) break;
        run_thread(vm, i);
    }
    if (vm->halted) for (int i = 0; i < DM_MAX_THREADS; i++) vm->th[i].used = 0;
    if (vm->str_bytes > vm->gc_at) gc(vm);
}

int dm_running(const dm_vm* vm) { return !vm->halted; }

int dm_me(const dm_vm* vm) { return vm->cur >= 0 ? vm->th[vm->cur].me : -1; }

// Nothing is running and nothing can start by itself (no timers, no `tick` handlers): a script that is only waiting for the
// game to give it events is idle too, which is how the command line tool knows a script has finished.
int dm_idle(const dm_vm* vm) {
    if (vm->halted) return 1;
    if (dm_thread_count(vm)) return 0;
    for (int i = 0; i < DM_MAX_TIMERS; i++) if (vm->timers[i].used) return 0;
    for (int h = 0; h < vm->prog->nhs; h++) if (vm->prog->hs[h].kind == EV_TICK) return 0;
    return 1;
}

int dm_thread_count(const dm_vm* vm) {
    int n = 0;
    for (int i = 0; i < DM_MAX_THREADS; i++) n += vm->th[i].used;
    return n;
}

dm_val dm_global_get(dm_vm* vm, const char* name) {
    int g = global_by_name(vm, name);
    return g >= 0 ? vm->globals[g] : dm_nil();
}

void dm_global_set(dm_vm* vm, const char* name, dm_val v) {
    int g = global_by_name(vm, name);
    if (g >= 0) vm->globals[g] = v;
}
