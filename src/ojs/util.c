// util.c — formatting, exception helpers, stack depth checks.

#include "ojs_int.h"
#include "atoms.h"

// ---------------------------------------------------------------- formatting

static int put(char* out, int cap, int n, char c) {
    if (n < cap - 1) out[n] = c;
    return n + 1;
}

int ojs_vsnprintf(char* out, int cap, const char* fmt, va_list ap) {
    int n = 0;
    for (const char* f = fmt; *f; f++) {
        if (*f != '%') { n = put(out, cap, n, *f); continue; }
        f++;
        int lng = 0;
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case '%': n = put(out, cap, n, '%'); break;
        case 'c': n = put(out, cap, n, (char)va_arg(ap, int)); break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) n = put(out, cap, n, *s++);
            break;
        }
        case 'S': {   // struct str* (may be NULL)
            const struct str* s = va_arg(ap, const struct str*);
            if (!s) { const char* t = "?"; while (*t) n = put(out, cap, n, *t++); break; }
            char tmp[4];
            for (uint32_t i = 0; i < str_len(s) && n < 4000; i++) {
                uint32_t c = str_at(s, i);
                if (c < 0x80) n = put(out, cap, n, (char)c);
                else {
                    // UTF-8 (surrogates as-is: messages only)
                    int k;
                    if (c < 0x800) { tmp[0] = (char)(0xC0 | (c >> 6)); tmp[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
                    else { tmp[0] = (char)(0xE0 | (c >> 12)); tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (c & 0x3F)); k = 3; }
                    for (int j = 0; j < k; j++) n = put(out, cap, n, tmp[j]);
                }
            }
            break;
        }
        case 'd': case 'u': case 'x': {
            uint64_t v;
            int neg = 0;
            if (*f == 'd') {
                int64_t x = lng >= 2 ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                if (x < 0) { neg = 1; v = (uint64_t)(-(x + 1)) + 1; } else v = (uint64_t)x;
            } else v = lng >= 2 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            char d[24];
            int dl = 0, base = *f == 'x' ? 16 : 10;
            do { d[dl++] = "0123456789abcdef"[v % (unsigned)base]; v /= (unsigned)base; } while (v);
            if (neg) n = put(out, cap, n, '-');
            while (dl) n = put(out, cap, n, d[--dl]);
            break;
        }
        case 'g': {   // double, ECMAScript Number::toString form
            char b[40];
            num_to_cstr(va_arg(ap, double), b);
            for (char* p = b; *p; p++) n = put(out, cap, n, *p);
            break;
        }
        default: n = put(out, cap, n, '%'); if (*f) n = put(out, cap, n, *f); else f--; break;
        }
    }
    if (cap > 0) out[n < cap ? n : cap - 1] = 0;
    return n;
}

int ojs_snprintf(char* out, int cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = ojs_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

// ---------------------------------------------------------------- exceptions

jv err_new(ojs* J, int ne, jv message);     // b_error.c (ne < 0: plain Error)

#if !defined(KERNEL) || !KERNEL
// host debugging: OJS_TRACE_THROW=1 prints every exception when it is thrown, caught or not (which web API did a script
// probe and find missing?); OJS_TRACE_THROW=2 also prints the first stack lines
#include <stdlib.h>
#include <stdio.h>
char* ojs_describe(ojs* J, jv v);
void ojs_free_cstring(ojs* J, char* s);
static void trace_throw(ojs* J, jv v) {
    static int mode = -1, busy;
    if (mode < 0) { const char* e = getenv("OJS_TRACE_THROW"); mode = e ? atoi(e) : 0; }
    if (!mode || busy) return;
    busy = 1;
    jv saved = J->exc; int had = J->has_exc;
    char* d = ojs_describe(J, v);
    J->exc = saved; J->has_exc = had;
    if (d) {
        int lines = 0;
        for (char* p = d; *p; p++) if (*p == '\n' && ++lines == (mode > 1 ? 4 : 1)) { *p = 0; break; }
        fprintf(stderr, "THROW %s\n", d);
        if (mode > 1 && d[0] == '<') {
            fprintf(stderr, "      raw value %016llx\n", (unsigned long long)v);
            jv m = ojs_get(J, v, "message");
            if (!ojs_is_exception(m)) { char* ms = ojs_to_cstring(J, m, 0); if (ms) { fprintf(stderr, "      message: %s\n", ms); ojs_free_cstring(J, ms); } }
            else J->has_exc = 0;
            jv nm = ojs_get(J, v, "name");
            if (!ojs_is_exception(nm)) { char* ms = ojs_to_cstring(J, nm, 0); if (ms) { fprintf(stderr, "      name: %s\n", ms); ojs_free_cstring(J, ms); } }
            else J->has_exc = 0;
        }
        ojs_free_cstring(J, d);
    }
    busy = 0;
}

// host debugging: OJS_TRACE_MISS=1 prints every distinct failed property lookup ("get") and failed `in` test ("has") with the
// receiver's constructor name - which browser features a page probes for and does not find. `typeof chrome` / `chrome` on the
// global object arrive with the receiver "window". Lines: "MISS get Navigator.userAgentData".
int ojs_trace_miss_on = -1;
#define MISS_SLOTS 262144
void ojs_trace_miss(ojs* J, struct obj* o, pkey k, const char* what) {
    static int busy, armed;
    static char* tab;                                   // hash set of the distinct lines already printed
    static int count;
    if (ojs_trace_miss_on < 0) { const char* e = getenv("OJS_TRACE_MISS"); ojs_trace_miss_on = e ? 1 : 0; armed = e && e[0] == '2'; }
    if (!ojs_trace_miss_on || busy || PK_IS_INDEX(k) || count >= MISS_SLOTS / 2) return;
    busy = 1;
    jv saved = J->exc; int had = J->has_exc;
    jv ks = pkey_to_string(J, k);
    char* kn = jv_is_str(ks) ? ojs_to_cstring(J, ks, 0) : 0;
    if (kn) {
        if (!armed) { if (!strcmp(kn, "__traceReady")) armed = 1; }       // OJS_TRACE_MISS=1: silent until the tracer's marker; =2: from the start
        else {
            const char* cn = "?";
            char* cs = 0;
            if (o == J->I.global) cn = "window";
            else {
                jv c = ord_get(J, o, A(constructor), jv_from_obj(o));
                if (c != JV_EXC && jv_is_obj(c)) {
                    jv n = ord_get(J, jv_obj(c), A(name), c);
                    if (n != JV_EXC && jv_is_str(n)) { cs = ojs_to_cstring(J, n, 0); if (cs) cn = cs; }
                }
            }
            char line[160];
            snprintf(line, sizeof line, "%s %s.%s", what, cn, kn);
            if (cs) ojs_free_cstring(J, cs);
            if (!tab) tab = (char*)calloc(MISS_SLOTS, 1);
            if (tab) {
                unsigned h = 2166136261u;
                for (const char* p = line; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
                unsigned slot = h & (MISS_SLOTS - 1);
                while (tab[slot] && tab[slot] != (char)(h >> 24 | 1)) slot = (slot + 1) & (MISS_SLOTS - 1);
                if (!tab[slot]) { tab[slot] = (char)(h >> 24 | 1); count++; fprintf(stderr, "MISS %s\n", line); }
            }
        }
        ojs_free_cstring(J, kn);
    }
    J->exc = saved; J->has_exc = had;
    busy = 0;
}
#endif

jv ojs_throw(ojs* J, jv v) {
#if !defined(KERNEL) || !KERNEL
    trace_throw(J, v);
#endif
    J->exc = v;
    J->has_exc = 1;
    return JV_EXC;
}

static jv throw_fmt(ojs* J, int ne, const char* fmt, va_list ap) {
    char buf[512];
    int n = ojs_vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    struct str* s = str_from_utf8(J, buf, (size_t)n);
    if (!s) return JV_EXC;   // OOM already thrown
    jv e = err_new(J, ne, jv_from_str(s));
    if (e == JV_EXC) return JV_EXC;
    return ojs_throw(J, e);
}

jv throw_error(ojs* J, int ne, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    jv r = throw_fmt(J, ne, fmt, ap);
    va_end(ap);
    return r;
}
#define THROW_FN(name, ne) \
    jv name(ojs* J, const char* fmt, ...) { va_list ap; va_start(ap, fmt); jv r = throw_fmt(J, ne, fmt, ap); va_end(ap); return r; }
THROW_FN(throw_type, NE_TYPE)
THROW_FN(throw_range, NE_RANGE)
THROW_FN(throw_ref, NE_REFERENCE)
THROW_FN(throw_syntax, NE_SYNTAX)

jv throw_oom(ojs* J) {
    // never allocate here: the exception is a preallocated value when
    // available, else a plain string-free marker the API reports as OOM
    J->exc = J->A ? jv_from_str(J->A->out_of_memory) : JV_NULL;
    J->has_exc = 1;
    J->oom = 1;
    return JV_EXC;
}

jv throw_stack_overflow(ojs* J) { return throw_range(J, "Maximum call stack size exceeded"); }

jv take_exc(ojs* J) {
    jv e = J->exc;
    J->exc = JV_UNDEFINED;
    J->has_exc = 0;
    J->oom = 0;
    return e;
}

int check_stack(ojs* J) {
    char probe;
    if (J->stack_top && J->stack_limit &&
        (uintptr_t)J->stack_top - (uintptr_t)&probe > J->stack_limit) {
        throw_stack_overflow(J);
        return -1;
    }
    return 0;
}
