// dmccl standard functions: module `basics` (math(), floor(), random() ...) and `log` (log:print).
// The game adds its own modules (world, fx, ...) with dm_register(); these are the ones every script gets.
#include "dm_internal.h"

double sqrt(double);
double sin(double);
double cos(double);

// ---- math("10 + 5 * 2") -- the original dmccl's expression evaluator, as a string -----------------------------------------

struct calc { const char* s; int bad; const char* why; };

static void calc_ws(struct calc* c) { while (*c->s == ' ' || *c->s == '\t') c->s++; }

static double calc_add(struct calc* c);

static double calc_primary(struct calc* c) {
    calc_ws(c);
    if (*c->s == '(') {
        c->s++;
        double v = calc_add(c);
        calc_ws(c);
        if (*c->s != ')') { c->bad = 1; c->why = "expected `)`"; return 0; }
        c->s++;
        return v;
    }
    if (*c->s == '-') { c->s++; return -calc_primary(c); }
    if ((*c->s >= '0' && *c->s <= '9') || *c->s == '.') {
        double v = 0, scale = 1;
        int dot = 0;
        while ((*c->s >= '0' && *c->s <= '9') || (*c->s == '.' && !dot)) {
            if (*c->s == '.') dot = 1;
            else if (!dot) v = v * 10 + (*c->s - '0');
            else { scale /= 10; v += (*c->s - '0') * scale; }
            c->s++;
        }
        return v;
    }
    c->bad = 1;
    c->why = *c->s ? "unexpected character in math()" : "math() ended too early";
    return 0;
}

static double calc_mul(struct calc* c) {
    double v = calc_primary(c);
    for (;;) {
        calc_ws(c);
        if (c->bad) return 0;
        if (*c->s == '*') { c->s++; v *= calc_primary(c); }
        else if (*c->s == '/') {
            c->s++;
            double d = calc_primary(c);
            if (d == 0) { c->bad = 1; c->why = "division by zero"; return 0; }
            v /= d;
        } else return v;
    }
}

static double calc_add(struct calc* c) {
    double v = calc_mul(c);
    for (;;) {
        calc_ws(c);
        if (c->bad) return 0;
        if (*c->s == '+') { c->s++; v += calc_mul(c); }
        else if (*c->s == '-') { c->s++; v -= calc_mul(c); }
        else return v;
    }
}

static int f_math(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) {
    (void)ctx;
    if (argc != 1 || argv[0].t != DM_STR) return dm_fail(vm, "math() takes one text expression, like math(\"1 + 2\")");
    struct calc c = { argv[0].u.s->text, 0, 0 };
    double v = calc_add(&c);
    calc_ws(&c);
    if (!c.bad && *c.s) { c.bad = 1; c.why = "unexpected character in math()"; }
    if (c.bad) return dm_fail(vm, c.why);
    *ret = dm_num(v);
    return 0;
}

// ---- log:print ---------------------------------------------------------------------------------------------------------

static int f_print(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) {
    (void)ctx;
    char line[512];
    size_t n = 0;
    line[0] = 0;
    for (int i = 0; i < argc; i++) {
        char buf[40];
        const char* s;
        switch (argv[i].t) {
        case DM_NUM: dm_fmt_num(argv[i].u.n, buf); s = buf; break;
        case DM_STR: s = argv[i].u.s->text; break;
        case DM_BOOL: s = argv[i].u.b ? "true" : "false"; break;
        case DM_ENT: strcpy(buf, "<thing "); { char k[32]; dm_fmt_num(argv[i].u.ent, k); strcat(buf, k); } strcat(buf, ">"); s = buf; break;
        default: s = "nil"; break;
        }
        if (i && n + 1 < sizeof line) line[n++] = ' ';
        while (*s && n + 1 < sizeof line) line[n++] = *s++;
        line[n] = 0;
    }
    if (vm->host.print) vm->host.print(vm->host.user, line);
    *ret = dm_nil();
    return 0;
}

// ---- small numeric helpers ---------------------------------------------------------------------------------------------

#define NUM_ARG(i) (argv[i].u.n)
#define NEED(n, what) do { if (argc != (n)) return dm_fail(vm, what); for (int q = 0; q < (n); q++) if (argv[q].t != DM_NUM) return dm_fail(vm, what); } while (0)

static int f_floor(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(1, "floor(x) takes one number"); *ret = dm_num(floor(NUM_ARG(0))); return 0; }
static int f_abs(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(1, "abs(x) takes one number"); *ret = dm_num(NUM_ARG(0) < 0 ? -NUM_ARG(0) : NUM_ARG(0)); return 0; }
static int f_sqrt(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(1, "sqrt(x) takes one number"); *ret = dm_num(NUM_ARG(0) < 0 ? 0 : sqrt(NUM_ARG(0))); return 0; }
static int f_sin(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(1, "sin(degrees) takes one number"); *ret = dm_num(sin(NUM_ARG(0) * 3.14159265358979323846 / 180.0)); return 0; }
static int f_cos(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(1, "cos(degrees) takes one number"); *ret = dm_num(cos(NUM_ARG(0) * 3.14159265358979323846 / 180.0)); return 0; }
static int f_min(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(2, "min(a, b) takes two numbers"); *ret = dm_num(NUM_ARG(0) < NUM_ARG(1) ? NUM_ARG(0) : NUM_ARG(1)); return 0; }
static int f_max(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) { (void)ctx; NEED(2, "max(a, b) takes two numbers"); *ret = dm_num(NUM_ARG(0) > NUM_ARG(1) ? NUM_ARG(0) : NUM_ARG(1)); return 0; }

static uint32_t next_random(dm_vm* vm) {
    uint32_t x = vm->rng ? vm->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;      // xorshift32: the same sequence everywhere, replays stay replays
    vm->rng = x;
    return x;
}

// random(n): a whole number from 0 to n-1;   random(a, b): a number from a up to (not including) b
static int f_random(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) {
    (void)ctx;
    double unit = (double)(next_random(vm) >> 8) / 16777216.0;
    if (argc == 1 && argv[0].t == DM_NUM) { *ret = dm_num(floor(unit * argv[0].u.n)); return 0; }
    if (argc == 2 && argv[0].t == DM_NUM && argv[1].t == DM_NUM) { *ret = dm_num(argv[0].u.n + unit * (argv[1].u.n - argv[0].u.n)); return 0; }
    return dm_fail(vm, "random(n) or random(a, b) with numbers");
}

static int f_str(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) {       // str(x): any value as text
    (void)ctx;
    if (argc != 1) return dm_fail(vm, "str(x) takes one value");
    char buf[40];
    switch (argv[0].t) {
    case DM_NUM: dm_fmt_num(argv[0].u.n, buf); *ret = dm_str_new(vm, buf); break;
    case DM_STR: *ret = argv[0]; break;
    case DM_BOOL: *ret = dm_str_new(vm, argv[0].u.b ? "true" : "false"); break;
    default: *ret = dm_str_new(vm, "nil"); break;
    }
    return 0;
}

void dm_register_basics(dm_vm* vm) {
    dm_register(vm, "log", "print", f_print, 0);
    dm_register(vm, "basics", "math", f_math, 0);
    dm_register(vm, "basics", "floor", f_floor, 0);
    dm_register(vm, "basics", "abs", f_abs, 0);
    dm_register(vm, "basics", "sqrt", f_sqrt, 0);
    dm_register(vm, "basics", "sin", f_sin, 0);
    dm_register(vm, "basics", "cos", f_cos, 0);
    dm_register(vm, "basics", "min", f_min, 0);
    dm_register(vm, "basics", "max", f_max, 0);
    dm_register(vm, "basics", "random", f_random, 0);
    dm_register(vm, "basics", "str", f_str, 0);
}
