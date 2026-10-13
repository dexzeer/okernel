// dmccl tests: language semantics against a tiny fake world, errors, limits, memory, and hostile input.
// build/run: sh tests/dmccl/run.sh
#include "../../src/dmccl/dm.h"
#include "../../src/dmccl/dm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char out[8192];
static void say(void* user, const char* line) { (void)user; strcat(out, line); strcat(out, "\n"); }

// ---- a tiny fake world: entities with x/y, a parent/child link, tags -------------------------------------------------

struct ent { int alive; double x, y; int parent, child; char tag[16]; };
static struct ent W[64];
static int nW;

static int w_get(dm_vm* vm, int e, const char* name, dm_val* out_v) {
    (void)vm;
    if (e < 0 || e >= nW) return 0;
    if (!strcmp(name, "x")) { *out_v = dm_num(W[e].x); return 1; }
    if (!strcmp(name, "y")) { *out_v = dm_num(W[e].y); return 1; }
    if (!strcmp(name, "alive")) { *out_v = dm_bool(W[e].alive); return 1; }
    return 0;
}
static int w_set(dm_vm* vm, int e, const char* name, dm_val v) {
    (void)vm;
    if (e < 0 || e >= nW || v.t != DM_NUM) return 0;
    if (!strcmp(name, "x")) { W[e].x = v.u.n; return 1; }
    if (!strcmp(name, "y")) { W[e].y = v.u.n; return 1; }
    return 0;
}
static int w_act(dm_vm* vm, int e, const char* verb, int argc, const dm_val* argv, dm_val* ret) {
    (void)argc; (void)argv; (void)ret;
    if (e < 0 || e >= nW) return 0;
    if (!W[e].alive) return 1;                                  // verbs on the dead are ignored
    if (!strcmp(verb, "abort") || !strcmp(verb, "kill")) {
        W[e].alive = 0;
        if (!strcmp(verb, "kill")) dm_fire_dies(vm, e, W[e].tag);
        dm_ent_gone(vm, e);
        return 1;
    }
    return 0;
}
static int w_ctx(dm_vm* vm, int me, const char* name, dm_val* o) {
    (void)vm;
    int v = !strcmp(name, "child") ? W[me].child : W[me].parent;
    *o = v >= 0 ? dm_ent(v) : dm_nil();
    return 1;
}
static int w_alive(dm_vm* vm, int e) { (void)vm; return e >= 0 && e < nW && W[e].alive; }
static void w_suicide(dm_vm* vm, int e) { if (e >= 0) { W[e].alive = 0; dm_ent_gone(vm, e); } }

// test:spawn("tag" [, "thing"]) makes an entity that is a child of the caller
static int f_spawn(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) {
    (void)ctx;
    if (argc < 1 || argv[0].t != DM_STR) return dm_fail(vm, "spawn(tag [, thing])");
    int e = nW++;
    memset(&W[e], 0, sizeof W[e]);
    W[e].alive = 1;
    W[e].parent = dm_me(vm);
    W[e].child = -1;
    strncpy(W[e].tag, dm_text(&argv[0]), 15);
    if (W[e].parent >= 0) W[W[e].parent].child = e;
    if (argc > 1) dm_spawn_thing(vm, dm_text(&argv[1]), e);
    *ret = dm_ent(e);
    return 0;
}

// test:touch(a, b): the game says two things touched
static int f_touch(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx) {
    (void)ctx; (void)ret;
    if (argc != 2 || argv[0].t != DM_ENT || argv[1].t != DM_ENT) return dm_fail(vm, "touch(a, b)");
    dm_fire_touch(vm, argv[0].u.ent, W[argv[0].u.ent].tag, argv[1].u.ent, W[argv[1].u.ent].tag);
    return 0;
}

// ---- harness ----------------------------------------------------------------------------------------------------------

static int failures, checks;

static void check(int ok, const char* name, const char* detail) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n%s\n", name, detail ? detail : ""); }
}

// run `src` for `seconds` of game time; returns the VM (caller frees) with `out` holding what it printed
static dm_vm* run(const char* src, double seconds, char* err) {
    out[0] = 0;
    nW = 0;
    dm_program* P = dm_compile(src, strlen(src), err, 256);
    if (!P) return 0;
    struct dm_host h;
    memset(&h, 0, sizeof h);
    h.print = say; h.get = w_get; h.set = w_set; h.act = w_act; h.ctx = w_ctx; h.alive = w_alive; h.suicide = w_suicide;
    dm_vm* vm = dm_vm_new(P, &h);
    dm_register_basics(vm);
    dm_register(vm, "test", "spawn", f_spawn, 0);
    dm_register(vm, "test", "touch", f_touch, 0);
    if (dm_link(vm, err, 256)) { dm_vm_free(vm); dm_program_free(P); return 0; }
    dm_start(vm);
    for (double t = 0; t < seconds * 1000 && !dm_idle(vm); t += 10) dm_step(vm, 10);
    return vm;
}

static void finish(dm_vm* vm) { dm_program* P = vm->prog; dm_vm_free(vm); dm_program_free(P); }

static void expect(const char* name, const char* src, double seconds, const char* want) {
    char err[256] = "";
    dm_vm* vm = run(src, seconds, err);
    if (!vm) { char d[300]; strcpy(d, "did not run: "); strcat(d, err); check(0, name, d); return; }
    char d[9000];
    strcpy(d, "want:\n"); strcat(d, want); strcat(d, "got:\n"); strcat(d, out);
    check(!strcmp(out, want), name, d);
    finish(vm);
}

static void expect_compile_error(const char* name, const char* src, const char* want) {
    char err[256] = "";
    dm_program* P = dm_compile(src, strlen(src), err, sizeof err);
    char d[400];
    strcpy(d, "message was: "); strcat(d, err);
    check(!P && strstr(err, want), name, d);
    if (P) dm_program_free(P);
}

static void expect_link_error(const char* name, const char* src, const char* want) {
    char err[256] = "";
    dm_vm* vm = run(src, 1, err);
    char d[400];
    strcpy(d, "message was: "); strcat(d, err);
    check(!vm && strstr(err, want), name, d);
    if (vm) finish(vm);
}

int main(void) {
    // -- the original language --
    expect("hello", "from dexlib import basics\nwhen run do\n    log:print(\"hi\", 1 + 2 * 3)\nendifchain\n", 1, "hi 7\n");
    expect("if chain", "x = 10\nwhen run do\n if x == 42 do log:print(\"a\")\n elseif x == 10 do log:print(\"b\")\n else do log:print(\"c\")\n endifchain\nendifchain\n", 1, "b\n");
    expect("else branch", "x = 1\nwhen run do\n if x == 2 do log:print(\"a\")\n else do log:print(\"c\")\n endifchain\nendifchain\n", 1, "c\n");
    expect("strings and math()", "from dexlib import basics\nwhen run do\n r = math(\"10 + 5 * 2\")\n log:print(\"r=\" + r, \"Hello, \" + \"you\" + \"!\")\nendifchain\n", 1, "r=20 Hello, you!\n");
    expect("waitfor", "when run do\n log:print(\"a\")\n waitfor:2s\n log:print(\"b\")\nendifchain\n", 1.9, "a\n");
    expect("waitfor done", "when run do\n log:print(\"a\")\n waitfor:2s\n log:print(\"b\")\nendifchain\n", 2.1, "a\nb\n");
    expect("milliseconds", "when run do\n waitfor:250ms\n log:print(\"x\")\nendifchain\n", 0.3, "x\n");
    expect("suicide ends the level", "when run do\n log:print(\"a\")\n suicide\n log:print(\"never\")\nendifchain\n", 1, "a\n");
    expect("top-level statements run", "log:print(\"loose\")\nx = 3\nlog:print(x)\n", 1, "loose\n3\n");
    expect("above(1) repeats the statement", "when run do\n log:print(\"hey\")\n above(1)\n above(1)\nendifchain\n", 1, "hey\nhey\nhey\n");
    expect("above(2)", "when run do\n log:print(\"a\")\n log:print(\"b\")\n above(2)\nendifchain\n", 1, "a\nb\na\n");
    expect("comparison, not, and, or", "when run do\n log:print(1 < 2 & 2 < 3, not 1 == 1, 1 > 2 | 5 > 3, 3 >= 3, 2 != 2)\nendifchain\n", 1, "true false true true false\n");
    expect("and/or are short circuits", "when run do\n x = nil\n log:print(x & x.y > 1, nil | 7)\nendifchain\n", 1, "false true\n");
    expect("numbers print like numbers", "when run do\n log:print(1 / 4, 10 / 4, 100, 0.1 + 0.2, -3, 7 % 4)\nendifchain\n", 1, "0.25 2.5 100 0.3 -3 3\n");
    expect("unary minus and precedence", "when run do\n log:print(-2 * 3 + 10, 2 + -3, (2 + 3) * 4)\nendifchain\n", 1, "4 -1 20\n");
    expect("compound assignment", "when run do\n n = 5\n n += 3\n n -= 1\n log:print(n)\nendifchain\n", 1, "7\n");
    // -- repeat / forever / checkfor --
    expect("repeat", "when run do\n repeat 3 do log:print(\"x\")\n endifchain\nendifchain\n", 1, "x\nx\nx\n");
    expect("repeat as i", "when run do\n repeat 3 as i do\n log:print(\"i =\", i)\n endifchain\nendifchain\n", 1, "i = 0\ni = 1\ni = 2\n");
    expect("nested repeat", "when run do\n repeat 2 as a do\n repeat 2 as b do\n log:print(a, b)\n endifchain\n endifchain\nendifchain\n", 1, "0 0\n0 1\n1 0\n1 1\n");
    expect("repeat zero", "when run do\n repeat 0 do log:print(\"no\")\n endifchain\n log:print(\"done\")\nendifchain\n", 1, "done\n");
    expect("forever with waitfor", "when run do\n n = 0\n forever do\n n += 1\n log:print(n)\n waitfor:1s\n endifchain\nendifchain\n", 3.5, "1\n2\n3\n4\n");
    expect("checkfor timeout runs the body", "when run do\n checkfor ready 2s do log:print(\"too slow\")\n endifchain\n log:print(\"after\")\nendifchain\n", 3, "too slow\nafter\n");
    expect("checkfor condition met skips the body", "when run do\n ready = false\n checkfor \"ready\" 5s do log:print(\"too slow\")\n endifchain\n log:print(\"after\")\nendifchain\n"
           "when every 1s do\n ready = true\nendifchain\n", 4, "after\n");
    // -- handlers, things, members --
    expect("when every", "when every 1s do\n log:print(\"tick\", time)\nendifchain\n", 3.05, "tick 1\ntick 2\ntick 3\n");
    expect("when tick is once per step", "n = 0\nwhen tick do\n n += 1\nendifchain\nwhen every 1s do\n log:print(n)\nendifchain\n", 1.0, "99\n");     // 99: this step's `tick` thread starts after the `every` one that prints
    expect("one-liner when with then chain", "when run do log:print(\"a\") then log:print(\"b\")\nwhen run do log:print(\"c\")\n", 1, "a\nb\nc\n");
    expect("members and fields", "from dexlib import test\nwhen run do\n g = test:spawn(\"guy\")\n g.x = 5\n g.x += 2\n g.coins = 3\n g.coins += 1\n log:print(g.x, g.coins, g.nothing)\nendifchain\n", 1, "7 4 nil\n");
    expect("child.abort then suicide",
           "from dexlib import test\n"
           "thing bomb is \"ball\" do\n"
           " when run do\n"
           "  test:spawn(\"spark\")\n"
           "  log:print(\"child alive:\", child.alive)\n"
           "  child.abort then suicide\n"
           " endifchain\n"
           "endifchain\n"
           "when run do\n"
           " b = test:spawn(\"bomb\", \"bomb\")\n"
           " waitfor:1s\n"
           " log:print(\"bomb gone:\", not b)\n"
           "endifchain\n", 2, "child alive: true\nbomb gone: true\n");
    expect("verbs on dead things are harmless", "from dexlib import test\nwhen run do\n a = test:spawn(\"a\")\n a.abort\n a.abort\n a.kill\n log:print(\"ok\")\nendifchain\n", 1, "ok\n");
    expect("when touches (both directions)",
           "from dexlib import test\n"
           "when run do\n guy = test:spawn(\"guy\")\n spikes = test:spawn(\"spikes\")\n test:touch(guy, spikes)\n test:touch(spikes, guy)\nendifchain\n"
           "when guy touches \"spikes\" do\n log:print(\"ouch\", who == guy, hit.alive)\nendifchain\n", 1, "ouch true true\nouch true true\n");
    expect("dies event", "from dexlib import test\nwhen run do\n a = test:spawn(\"a\")\n a.kill\nendifchain\nwhen \"a\" dies do\n log:print(\"a died\")\nendifchain\n", 1, "a died\n");
    expect("thing handlers see me",
           "from dexlib import test\n"
           "thing blob do\n when run do\n me.hp = 3\n waitfor:1s\n me.hp -= 1\n log:print(\"hp\", me.hp)\n endifchain\nendifchain\n"
           "when run do\n a = test:spawn(\"blob\", \"blob\")\n b = test:spawn(\"blob\", \"blob\")\n waitfor:0.5s\n b.hp += 7\nendifchain\n", 1.5, "hp 2\nhp 9\n");
    // -- errors --
    expect_compile_error("missing endifchain", "when run do\n log:print(1)\n", "expected `endifchain`");
    expect_compile_error("bad character", "when run do\n x = $\nendifchain\n", "unexpected character '$'");
    expect_compile_error("stray endifchain", "when run do log:print(1)\nendifchain\n", "stray");
    expect_compile_error("assign to a value", "when run do\n 5 = 3\nendifchain\n", "assign");
    expect_compile_error("bad above", "when run do\n above(1)\nendifchain\n", "above");
    expect_compile_error("error line numbers", "x = 1\ny = 2\nwhen run do\n z = (\nendifchain\n", "line 4");
    expect_link_error("module not imported", "from dexlib import basics\nwhen run do\n test:spawn(\"x\")\nendifchain\n", "from dexlib import test");
    expect_link_error("unknown function", "when run do\n nothing(1)\nendifchain\n", "unknown function nothing");
    expect_link_error("unknown module", "from dexlib import nothere\nwhen run do\n log:print(1)\nendifchain\n", "dexlib.nothere");
    expect("runtime error is reported, others go on", "when run do\n log:print(1 / 0)\nendifchain\nwhen run do\n log:print(\"fine\")\nendifchain\n", 1, "[dmccl] error at line 2: division by zero\nfine\n");
    expect("type error", "when run do\n log:print(\"a\" - 1)\nendifchain\n", 1, "[dmccl] error at line 2: math needs numbers (use + to join text)\n");
    // -- limits --
    {
        char err[256];
        dm_vm* vm = run("when run do\n x = 0\n forever do\n  repeat 100000 do x += 1\n  endifchain\n endifchain\nendifchain\n", 0.5, err);
        check(vm != 0, "runaway loop still returns", err);
        if (vm) { check(dm_global_get(vm, "x").t == DM_NUM, "runaway loop made progress", 0); finish(vm); }
    }
    {   // thousands of strings: the collector keeps memory bounded
        char err[256];
        dm_vm* vm = run("when run do\n s = \"\"\n forever do\n  s = \"abc\" + 1 + \"def\"\n  t = s + s + s\n endifchain\nendifchain\n", 2, err);
        check(vm != 0, "string churn runs", err);
        if (vm) { check(vm->str_bytes < 4u * 1024 * 1024, "string memory stays bounded", 0); finish(vm); }
    }
    // -- hostile input: mutated programs must neither crash nor hang --
    {
#include "fuzz_block.inc"
    }
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
