// run.c — host runner for the ojs engine.
//
//   ojs_run [-m] [-s] file.js ...     run scripts (-m: as modules, -s: strict)
//   ojs_run -e 'code'                 evaluate and print the completion value
//   ojs_run --test262 <harness dir>   read "flags path" lines from stdin and
//                                     print PASS / FAIL <reason> per line
//
// Provides print(), console.log() and the $262 host object test262 uses.

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "../../src/ojs/ojs.h"
#include "../../src/ojs/ojs_sys.h"

void* ojs_sys_malloc(size_t n) { return malloc(n); }
void* ojs_sys_realloc(void* p, size_t n) { return realloc(p, n); }
void ojs_sys_free(void* p) { free(p); }

double ojs_sys_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}

int ojs_sys_tz_offset(double utc_ms, int is_local) { (void)utc_ms; (void)is_local; return 0; }

static void set_x87(void) {
    // the kernel runs the x87 at 53-bit precision; do the same here
    unsigned short cw = 0x027F;
    __asm__ volatile("fldcw %0" :: "m"(cw));
}

static char* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    *len = r;
    return b;
}

static ojsv js_print(ojs* J, ojsv this_v, int argc, ojsv* argv) {
    for (int i = 0; i < argc; i++) {
        char* s = ojs_describe(J, argv[i]);
        printf("%s%s", i ? " " : "", s ? s : "?");
        free(s);
    }
    printf("\n");
    fflush(stdout);
    return OJS_UNDEFINED;
}

// ---------------------------------------------------------------- $262

static ojsv js_eval_script(ojs* J, ojsv this_v, int argc, ojsv* argv) {
    size_t len;
    char* s = ojs_to_cstring(J, argc ? argv[0] : OJS_UNDEFINED, &len);
    if (!s) return OJS_EXCEPTION;
    ojsv r = ojs_eval(J, s, len, "evalScript", OJS_EVAL_SCRIPT);
    ojs_free_cstring(J, s);
    return r;
}

int ojs_detach_buffer(ojs* J, ojsv v);
static ojsv js_detach(ojs* J, ojsv this_v, int argc, ojsv* argv) {
    if (ojs_detach_buffer(J, argc ? argv[0] : OJS_UNDEFINED) < 0) return ojs_throw_type_error(J, "not an ArrayBuffer");
    return OJS_NULL;
}

static ojsv js_gc(ojs* J, ojsv this_v, int argc, ojsv* argv) { ojs_gc(J); return OJS_UNDEFINED; }

// performance.now(): monotonic milliseconds with sub-ms resolution (benchmarks)
static ojsv js_perf_now(ojs* J, ojsv this_v, int argc, ojsv* argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ojs_number((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6);
}

// readFile(path): a file's text (debugging aid for the runner)
static ojsv js_read_file(ojs* J, ojsv this_v, int argc, ojsv* argv) {
    char* path = ojs_to_cstring(J, argc ? argv[0] : OJS_UNDEFINED, 0);
    if (!path) return OJS_EXCEPTION;
    size_t len;
    char* src = read_file(path, &len);
    ojs_free_cstring(J, path);
    if (!src) return ojs_throw_type_error(J, "cannot read file");
    ojsv r = ojs_string_len(J, src, len);
    free(src);
    return r;
}

static int install_host(ojs* J) {
    ojsv g = ojs_global(J);
    ojsv pf = ojs_function(J, js_print, "print", 1);
    if (ojs_define_hidden(J, g, "print", pf) < 0) return -1;
    if (ojs_define_hidden(J, g, "readFile", ojs_function(J, js_read_file, "readFile", 1)) < 0) return -1;
    ojsv perf = ojs_object(J);
    if (ojs_define_hidden(J, perf, "now", ojs_function(J, js_perf_now, "now", 0)) < 0 || ojs_define_hidden(J, g, "performance", perf) < 0) return -1;
    ojsv con = ojs_object(J);
    if (ojs_define_hidden(J, con, "log", pf) < 0 || ojs_define_hidden(J, g, "console", con) < 0) return -1;
    ojsv d = ojs_object(J);
    static const struct ojs_func_entry fns[] = {
        { "evalScript", js_eval_script, 1 },
        { "detachArrayBuffer", js_detach, 1 },
        { "gc", js_gc, 0 },
    };
    if (ojs_set_functions(J, d, fns, 3) < 0) return -1;
    if (ojs_define_hidden(J, d, "global", g) < 0) return -1;
    ojsv agent = ojs_object(J);
    if (ojs_define_hidden(J, d, "agent", agent) < 0) return -1;
    return ojs_define_hidden(J, g, "$262", d);
}

// ---------------------------------------------------------------- modules (files relative to the referrer)

static char* mod_resolve(ojs* J, const char* referrer, const char* spec, void* op) {
    (void)J; (void)op;
    size_t rl = strlen(referrer), sl = strlen(spec);
    char* out = (char*)malloc(rl + sl + 2);
    if (!out) return 0;
    if (spec[0] == '/' || !strchr(referrer, '/')) { memcpy(out, spec, sl + 1); return out; }
    const char* slash = strrchr(referrer, '/');
    size_t dl = (size_t)(slash - referrer) + 1;
    memcpy(out, referrer, dl);
    const char* s = spec;
    if (s[0] == '.' && s[1] == '/') s += 2;
    memcpy(out + dl, s, strlen(s) + 1);
    return out;
}

static ojsv mod_load(ojs* J, const char* name, void* op) {
    (void)op;
    size_t len;
    char* src = read_file(name, &len);
    if (!src) return ojs_throw_reference_error(J, "cannot load module %s", name);
    ojsv m = ojs_eval(J, src, len, name, OJS_EVAL_MODULE | OJS_EVAL_COMPILE_ONLY);
    free(src);
    return m;
}

// ---------------------------------------------------------------- running

static int report(ojs* J, const char* what) {
    ojsv e = ojs_take_exception(J);
    char* s = ojs_describe(J, e);
    printf("%s: %s\n", what, s ? s : "?");
    free(s);
    return 1;
}

static int drain_jobs(ojs* J) {
    for (;;) {
        int r = ojs_run_job(J);
        if (r == 0) return 0;
        if (r < 0) return -1;
    }
}

static int run_file(ojs* J, const char* path, int module, int strict) {
    size_t len;
    char* src = read_file(path, &len);
    if (!src) { printf("cannot read %s\n", path); return 1; }
    ojsv r = ojs_eval(J, src, len, path, (module ? OJS_EVAL_MODULE : OJS_EVAL_SCRIPT) | (strict ? OJS_EVAL_STRICT : 0));
    free(src);
    if (ojs_is_exception(r)) return report(J, "Uncaught");
    if (drain_jobs(J) < 0) return report(J, "Uncaught (job)");
    if (module) {
        ojsv res;
        if (ojs_promise_state(J, r, &res) == 2) {
            char* s = ojs_describe(J, res);
            printf("Uncaught (module): %s\n", s ? s : "?");
            free(s);
            return 1;
        }
    }
    return 0;
}

// test262: each stdin line "<flags> <test path>"; flags: s strict, m module, r raw, a async, n<type> negative
static int test262(const char* harness) {
    static char line[8192];
    size_t hl;
    char path[4096];
    snprintf(path, sizeof path, "%s/assert.js", harness);
    char* h_assert = read_file(path, &hl);
    snprintf(path, sizeof path, "%s/sta.js", harness);
    char* h_sta = read_file(path, &hl);
    snprintf(path, sizeof path, "%s/doneprintHandle.js", harness);
    char* h_done = read_file(path, &hl);
    if (!h_assert || !h_sta || !h_done) { printf("missing harness files in %s\n", harness); return 2; }
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char* sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char* flags = line;
        const char* file = sp + 1;
        // optional includes after the path: "flags path inc1,inc2"
        char* inc = strchr(file, ' ');
        if (inc) *inc++ = 0;
        int strict = strchr(flags, 's') != 0, module = strchr(flags, 'm') != 0, raw = strchr(flags, 'r') != 0;
        int async = strchr(flags, 'a') != 0;
        const char* neg = strchr(flags, 'n');
        ojs* J = ojs_new();
        if (!J) { printf("FAIL init\n"); fflush(stdout); continue; }
        ojs_enter(J);
        ojs_set_stack_size(J, 2 << 20);
        ojs_set_memory_limit(J, 512u << 20);
        if (getenv("OJS_GCSTRESS")) ojs_set_gc_stress(J, (uint32_t)atoi(getenv("OJS_GCSTRESS")));
        install_host(J);
        struct ojs_module_hooks hooks = { mod_resolve, mod_load, 0, 0 };
        ojs_set_module_hooks(J, &hooks);
        int ok = 1;
        char reason[600] = "";
        size_t len;
        char* src = read_file(file, &len);
        if (!src) { printf("FAIL cannot read\n"); fflush(stdout); ojs_leave(J); ojs_free(J); continue; }
        if (!raw) {
            const char* hs[3] = { h_assert, h_sta, async ? h_done : 0 };
            for (int k = 0; k < 3 && ok; k++) {
                if (!hs[k]) continue;
                ojsv r = ojs_eval(J, hs[k], strlen(hs[k]), "harness", OJS_EVAL_SCRIPT);
                if (ojs_is_exception(r)) { ok = 0; snprintf(reason, sizeof reason, "harness error"); ojs_take_exception(J); }
            }
            if (inc && ok) {
                char* save = 0;
                for (char* t = strtok_r(inc, ",", &save); t && ok; t = strtok_r(0, ",", &save)) {
                    snprintf(path, sizeof path, "%s/%s", harness, t);
                    size_t il;
                    char* is = read_file(path, &il);
                    if (!is) { ok = 0; snprintf(reason, sizeof reason, "missing include %s", t); break; }
                    ojsv r = ojs_eval(J, is, il, t, OJS_EVAL_SCRIPT);
                    free(is);
                    if (ojs_is_exception(r)) {
                        ojsv e = ojs_take_exception(J);
                        char* d = ojs_describe(J, e);
                        ok = 0;
                        snprintf(reason, sizeof reason, "include %s: %s", t, d ? d : "?");
                        free(d);
                    }
                }
            }
        }
        int threw = 0;
        char errdesc[512] = "";
        if (ok) {
            ojsv r = ojs_eval(J, src, len, file, (module ? OJS_EVAL_MODULE : OJS_EVAL_SCRIPT) | (strict ? OJS_EVAL_STRICT : 0));
            if (ojs_is_exception(r)) {
                threw = 1;
                ojsv e = ojs_take_exception(J);
                char* d = ojs_describe(J, e);
                snprintf(errdesc, sizeof errdesc, "%s", d ? d : "?");
                free(d);
            } else {
                if (drain_jobs(J) < 0) {
                    threw = 1;
                    ojsv e = ojs_take_exception(J);
                    char* d = ojs_describe(J, e);
                    snprintf(errdesc, sizeof errdesc, "(job) %s", d ? d : "?");
                    free(d);
                }
                ojsv res;
                if (!threw && module && ojs_promise_state(J, r, &res) == 2) {
                    threw = 1;
                    char* d = ojs_describe(J, res);
                    snprintf(errdesc, sizeof errdesc, "%s", d ? d : "?");
                    free(d);
                }
            }
            if (neg) {
                // expected error type follows 'n' up to the next flag (e.g. "nSyntaxError")
                const char* t = neg + 1;
                if (!threw) { ok = 0; snprintf(reason, sizeof reason, "expected %s, no exception", t); }
                else if (strncmp(errdesc, t, strlen(t)) != 0) { ok = 0; snprintf(reason, sizeof reason, "expected %s, got %s", t, errdesc); }
            } else if (threw) { ok = 0; snprintf(reason, sizeof reason, "%s", errdesc); }
        }
        free(src);
        ojs_leave(J);
        ojs_free(J);
        for (char* c = reason; *c; c++) if (*c == '\n') *c = ' ';
        if (ok) printf("PASS\n"); else printf("FAIL %s\n", reason);
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    set_x87();
    if (argc > 2 && !strcmp(argv[1], "--test262")) return test262(argv[2]);
    int module = 0, strict = 0, rc = 0;
    ojs* J = ojs_new();
    if (!J) { printf("ojs_new failed\n"); return 2; }
    ojs_enter(J);
    ojs_set_stack_size(J, 2 << 20);
    if (getenv("OJS_GCSTRESS")) ojs_set_gc_stress(J, (uint32_t)atoi(getenv("OJS_GCSTRESS")));
    if (getenv("OJS_MEMLIMIT")) ojs_set_memory_limit(J, (size_t)atoi(getenv("OJS_MEMLIMIT")) << 20);   // MB, like a page realm
    install_host(J);
    struct ojs_module_hooks hooks = { mod_resolve, mod_load, 0, 0 };
    ojs_set_module_hooks(J, &hooks);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) { module = 1; continue; }
        if (!strcmp(argv[i], "-s")) { strict = 1; continue; }
        if (!strcmp(argv[i], "-e") && i + 1 < argc) {
            const char* code = argv[++i];
            ojsv r = ojs_eval(J, code, strlen(code), "<cmdline>", OJS_EVAL_SCRIPT);
            if (ojs_is_exception(r)) { rc |= report(J, "Uncaught"); continue; }
            char* s = ojs_describe(J, r);
            printf("%s\n", s ? s : "?");
            free(s);
            if (drain_jobs(J) < 0) rc |= report(J, "Uncaught (job)");
            continue;
        }
        rc |= run_file(J, argv[i], module, strict);
    }
    if (getenv("OJS_GCSTATS")) {
        struct ojs_stats st;
        ojs_get_stats(J, &st);
        fprintf(stderr, "[gc] heap %zuKB live %zuKB collections %d (%.0fms) allocated %lluKB\n", st.heap_bytes >> 10, st.live_bytes >> 10, st.gc_count, st.gc_ms, st.total_alloc >> 10);
        const char* names = OJS_STATS_TYPES;
        for (int i = 0; i < 16 && *names; i++) {
            int l = 0;
            while (names[l] && names[l] != ' ') l++;
            if (st.live_by_type[i] >= 1024) fprintf(stderr, "  %.*s %zuKB\n", l, names, st.live_by_type[i] >> 10);
            names += l;
            while (*names == ' ') names++;
        }
    }
    ojs_leave(J);
    ojs_free(J);
    return rc;
}
