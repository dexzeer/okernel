// b_error.c — Error, the NativeError constructors, AggregateError and
// stack traces.
//
// An error records the call stack (function template + bytecode offset
// per frame) when it is created; Error.prototype.stack is an accessor
// that formats it on first use ("    at f (file:line:col)" lines, the
// format most site code parses).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

#define STACK_MAX 16

struct errobj {
    struct obj base;
    jv frames;          // internal valarr pointer as jv_from_ptr (ftempl, offset pairs), or undefined
    uint32_t nframes;
    uint32_t pad;
    jv stack;           // formatted text (cached), or undefined
};

static void err_trace(ojs* J, struct obj* o) {
    struct errobj* e = (struct errobj*)o;
    if (e->frames != JV_UNDEFINED) gc_mark_valarr(J, (jv*)JV_PTR(e->frames));
    gc_mark_value(J, e->stack);
}

static const char* const NE_NAMES[NE_COUNT] = {
    "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", "URIError", "AggregateError",
};

void err_capture_stack(ojs* J, struct obj* o) {
    if (obj_class(o) != OC_ERROR) return;
    struct errobj* e = (struct errobj*)o;
    uint32_t n = 0;
    for (struct ojs_frame* f = J->frame; f && n < STACK_MAX; f = f->prev) n++;
    e->stack = JV_UNDEFINED;
    e->frames = JV_UNDEFINED;
    e->nframes = 0;
    if (!n) return;
    jv* v = valarr_new(J, n * 2);
    if (!v) { take_exc(J); return; }   // a missing trace is not an error
    uint32_t i = 0;
    for (struct ojs_frame* f = J->frame; f && i < n; f = f->prev, i++) {
        v[i * 2] = jv_from_ptr(f->t);
        v[i * 2 + 1] = jv_from_int((int32_t)(f->pc - f->t->code));
    }
    e->frames = jv_from_ptr(v);
    e->nframes = n;
}

// source position of the instruction a frame is executing: `off` is its saved pc,
// which already points past that instruction's opcode. POS_CALLEE marks a call
// whose callee is a plain dotted name starting at that position.
static uint32_t varint(const uint8_t** pp) {
    const uint8_t* p = *pp;
    uint32_t v = 0;
    int sh = 0;
    do { v |= (uint32_t)(*p & 0x7F) << sh; sh += 7; } while (*p++ & 0x80);
    *pp = p;
    return v;
}

static uint32_t pos_raw(struct ftempl* t, uint32_t off) {
    uint32_t best = t->src_start;
    if (off) off--;
    const uint8_t* p = t->lines;
    const uint8_t* e = p ? p + t->nlines : p;
    uint32_t pc = 0, pos = 0;
    while (p < e) {
        uint32_t a = varint(&p), b = varint(&p);
        pc += a >> 1;
        pos += (uint32_t)((int32_t)(b >> 1) ^ -(int32_t)(b & 1));
        if (pc > off) break;
        best = pos | ((a & 1) ? POS_CALLEE : 0);
    }
    return best;
}
static uint32_t pos_of(struct ftempl* t, uint32_t off) { return pos_raw(t, off) & ~POS_CALLEE; }

static int name_char(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

// "x is not a function": the callee of the call the current frame is executing, as
// written in the source, when it is a plain dotted name (a, a.b.c, this.f, a?.b);
// calls mark their own position right before the call instruction
static int callee_text(ojs* J, int ctor, char* out, int cap) {
    struct ojs_frame* f = J->frame;
    if (!f || !f->t || !(f->t->source || f->t->srct)) return 0;
    struct ftempl* t = f->t;
    uint32_t p0 = pos_raw(t, (uint32_t)(f->pc - t->code));
    if (!(p0 & POS_CALLEE)) return 0;
    p0 &= ~POS_CALLEE;
    // the source from the call's position on (a window is all the name can span)
    uint32_t w[256], n = 0;
    if (t->source) {
        uint32_t len = str_len(t->source);
        while (n < 256 && p0 + n < len) { w[n] = str_at(t->source, p0 + n); n++; }
    } else {
        struct srccur cur;
        srccur_init(&cur, t->srct, p0);
        while (n < 256) { uint32_t u = srccur_next(&cur); if (u == 0xFFFFFFFFu) break; w[n++] = u; }
    }
    uint32_t p = 0;
    if (ctor && p + 3 < n && w[p] == 'n' && w[p + 1] == 'e' && w[p + 2] == 'w' && !name_char(w[p + 3])) {
        p += 3;
        while (p < n && (w[p] == ' ' || w[p] == '\t')) p++;
    }
    int k = 0;
    while (p < n && k < cap - 1) {
        uint32_t c = w[p];
        if (name_char(c) || c == '#') out[k++] = (char)c;
        else if (c == '.' && k && out[k - 1] != '.') out[k++] = '.';
        else if (c == '?' && p + 1 < n && w[p + 1] == '.' && k) { out[k++] = '?'; out[k++] = '.'; p++; if (k >= cap - 1) return 0; }
        else break;
        p++;
    }
    if (!k || out[k - 1] == '.' || (out[0] >= '0' && out[0] <= '9')) return 0;
    while (p < n && (w[p] == ' ' || w[p] == '\t')) p++;
    uint32_t next = p < n ? w[p] : 0;
    if (!(next == '(' || next == '`' || (ctor && (next == 0 || next == ';' || next == ')' || next == ',' || next == '\n')))) return 0;
    out[k] = 0;
    return 1;
}

static void line_col(struct str* src, uint32_t pos, int* line, int* col);
static void t_line_col(struct ftempl* t, uint32_t pos, int* line, int* col);
static int t_has_source(struct ftempl* t) { return t->source || t->srct; }

// the innermost running function as "name file:line:col" (host profiling aid)
int ojs_where(ojs* J, char* out, int cap) {
    struct ojs_frame* f = J->frame;
    while (f && !f->t) f = f->prev;
    if (!f || cap < 8) return 0;
    struct ftempl* t = f->t;
    int line = 0, col = 0;
    if (t_has_source(t)) t_line_col(t, pos_of(t, (uint32_t)(f->pc - t->code)), &line, &col);
    struct sbuf b;
    sb_init(J, &b);
    if (t->name && str_len(t->name)) sb_put_str(&b, t->name); else sb_puts(&b, "<anon>");
    sb_puts(&b, " ");
    if (t->filename) {
        uint32_t n = str_len(t->filename), from = n > 48 ? n - 48 : 0;
        for (uint32_t i = from; i < n; i++) sb_putc(&b, str_at(t->filename, i));
    }
    char nb[24];
    ojs_snprintf(nb, sizeof nb, ":%d:%d", line, col);
    sb_puts(&b, nb);
    jv s = sb_done(&b);
    if (s == JV_EXC) { take_exc(J); return 0; }
    struct str* st = jv_str(s);
    int k = 0;
    for (uint32_t i = 0; i < str_len(st) && k < cap - 1; i++) { uint32_t c = str_at(st, i); out[k++] = c < 128 ? (char)c : '?'; }
    out[k] = 0;
    return k;
}

jv throw_not_callable(ojs* J, jv v, int ctor) {
    char name[96];
    if (callee_text(J, ctor, name, (int)sizeof name)) return throw_type(J, ctor ? "%s is not a constructor" : "%s is not a function", name);
    jv tv = typeof_value(J, v);
    if (ctor) return throw_type(J, "%S is not a constructor", jv_is_str(tv) ? jv_str(tv) : 0);
    return throw_type(J, "%S is not a function", jv_is_str(tv) ? jv_str(tv) : 0);
}

static void line_col(struct str* src, uint32_t pos, int* line, int* col) {
    int l = 1, c = 1;
    uint32_t n = str_len(src);
    if (pos > n) pos = n;
    for (uint32_t i = 0; i < pos; i++) {
        if (str_at(src, i) == '\n') { l++; c = 1; } else c++;
    }
    *line = l;
    *col = c;
}

// line_col over a template's source, whichever form it has
static void t_line_col(struct ftempl* t, uint32_t pos, int* line, int* col) {
    if (t->source) { line_col(t->source, pos, line, col); return; }
    int l = 1, c = 1;
    struct srccur cur;
    srccur_init(&cur, t->srct, 0);
    for (uint32_t i = 0; i < pos; i++) {
        uint32_t u = srccur_next(&cur);
        if (u == 0xFFFFFFFFu) break;
        if (u == '\n') { l++; c = 1; } else c++;
    }
    *line = l;
    *col = c;
}

// "Name: message" header (Error.prototype.toString semantics, without calls)
static void put_header(ojs* J, struct sbuf* b, struct obj* o) {
    jv nm = JV_UNDEFINED, msg = JV_UNDEFINED;
    struct pdesc d;
    for (struct obj* p = o; p; p = p->proto) {
        if ((p->flags & OF_EXOTIC)) break;
        if (jv_is_undef(nm) && ord_get_own(J, p, A(name), &d) > 0 && (d.has & PD_VALUE)) nm = d.value;
        if (jv_is_undef(msg) && ord_get_own(J, p, A(message), &d) > 0 && (d.has & PD_VALUE)) msg = d.value;
    }
    struct str* ns = jv_is_str(nm) ? str_flat(J, nm) : 0;
    struct str* ms = jv_is_str(msg) ? str_flat(J, msg) : 0;
    if (ns) sb_put_str(b, ns); else sb_puts(b, "Error");
    if (ms && str_len(ms)) {
        sb_puts(b, ": ");
        sb_put_str(b, ms);
    }
}

static jv format_stack(ojs* J, struct errobj* e) {
    struct sbuf b;
    sb_init(J, &b);
    put_header(J, &b, &e->base);
    jv* v = e->frames == JV_UNDEFINED ? 0 : (jv*)JV_PTR(e->frames);
    for (uint32_t i = 0; v && i < e->nframes; i++) {
        struct ftempl* t = (struct ftempl*)JV_PTR(v[i * 2]);
        uint32_t off = (uint32_t)jv_int(v[i * 2 + 1]);
        sb_puts(&b, "\n    at ");
        int named = t->name && str_len(t->name) > 0;
        if (named) { sb_put_str(&b, t->name); sb_puts(&b, " ("); }
        if (t->filename && str_len(t->filename)) sb_put_str(&b, t->filename);
        else sb_puts(&b, "<anonymous>");
        if (t_has_source(t)) {
            int line, col;
            t_line_col(t, pos_of(t, off), &line, &col);
            char buf[32];
            ojs_snprintf(buf, sizeof buf, ":%d:%d", line, col);
            sb_puts(&b, buf);
        }
        if (named) sb_putc(&b, ')');
    }
    return sb_done(&b);
}

static jv error_stack_get(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Error.prototype.stack getter called on non-object");
    struct obj* o = jv_obj(this_v);
    if (obj_class(o) != OC_ERROR) return JV_UNDEFINED;
    struct errobj* e = (struct errobj*)o;
    if (jv_is_undef(e->stack)) {
        jv s = format_stack(J, e);
        if (s == JV_EXC) return JV_EXC;
        e->stack = s;
    }
    return e->stack;
}

// SetterThatIgnoresPrototypeProperties(this, %Error.prototype%, "stack", v)
static jv error_stack_set(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Error.prototype.stack setter called on non-object");
    if (argc < 1) return throw_type(J, "Error.prototype.stack setter requires an argument");
    struct obj* o = jv_obj(this_v);
    if (o == J->I.error_proto) return throw_type(J, "Cannot assign to read only property 'stack' of Error.prototype");
    int h = obj_get_own(J, o, A(stack), 0);
    if (h < 0) return JV_EXC;
    if (!h) { if (create_data_property_or_throw(J, o, A(stack), argv[0]) < 0) return JV_EXC; }
    else if (obj_set(J, o, A(stack), argv[0], this_v, 1) < 0) return JV_EXC;
    return JV_UNDEFINED;
}

// ---------------------------------------------------------------- construction

static struct errobj* err_alloc(ojs* J, struct obj* proto) {
    struct errobj* e = (struct errobj*)obj_new(J, proto, OC_ERROR, sizeof(struct errobj));
    if (!e) return 0;
    e->base.flags |= OF_IS_ERROR;
    e->frames = JV_UNDEFINED;
    e->stack = JV_UNDEFINED;
    return e;
}

jv err_new(ojs* J, int ne, jv message) {
    struct obj* proto = ne >= 0 && ne < NE_COUNT ? J->I.native_error_proto[ne] : J->I.error_proto;
    struct errobj* e = err_alloc(J, proto);
    if (!e) return JV_EXC;
    if (!jv_is_undef(message) && obj_define_value(J, &e->base, A(message), message, PA_HIDDEN) < 0) return JV_EXC;
    err_capture_stack(J, &e->base);
    return jv_from_obj(&e->base);
}

// InstallErrorCause
static int install_cause(ojs* J, struct obj* o, jv options) {
    if (!jv_is_obj(options)) return 0;
    int h = obj_has(J, jv_obj(options), A(cause));
    if (h <= 0) return h;
    jv c = obj_get(J, jv_obj(options), A(cause), options);
    if (c == JV_EXC) return -1;
    return obj_define_value(J, o, A(cause), c, PA_HIDDEN);
}

// magic: -1 Error, NE_* native errors
static jv error_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    struct obj* fallback = magic < 0 ? J->I.error_proto : J->I.native_error_proto[magic];
    if (jv_is_undef(nt)) nt = jv_from_obj(magic < 0 ? J->I.error_ctor : J->I.native_error_ctor[magic]);
    struct obj* proto = get_proto_from_ctor(J, nt, fallback);
    if (!proto) return JV_EXC;
    struct errobj* e = err_alloc(J, proto);
    if (!e) return JV_EXC;
    jv msg = magic == NE_AGGREGATE ? argv[1] : argv[0];
    jv opts = magic == NE_AGGREGATE ? argv[2] : argv[1];
    if (!jv_is_undef(msg)) {
        jv s = to_string(J, msg);
        if (s == JV_EXC) return JV_EXC;
        if (obj_define_value(J, &e->base, A(message), s, PA_HIDDEN) < 0) return JV_EXC;
    }
    if (install_cause(J, &e->base, opts) < 0) return JV_EXC;
    if (magic == NE_AGGREGATE) {
        jv list = iter_to_list(J, argv[0]);
        if (list == JV_EXC) return JV_EXC;
        if (obj_define_value(J, &e->base, A(errors), list, PA_HIDDEN) < 0) return JV_EXC;
    }
    err_capture_stack(J, &e->base);
    return jv_from_obj(&e->base);
}

static jv error_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Error.prototype.toString called on non-object");
    struct obj* o = jv_obj(this_v);
    jv nm = obj_get(J, o, A(name), this_v);
    if (nm == JV_EXC) return JV_EXC;
    if (jv_is_undef(nm)) nm = str_value(J, "Error");
    else { nm = to_string(J, nm); if (nm == JV_EXC) return JV_EXC; }
    jv msg = obj_get(J, o, A(message), this_v);
    if (msg == JV_EXC) return JV_EXC;
    if (jv_is_undef(msg)) msg = jv_from_str(J->A->empty);
    else { msg = to_string(J, msg); if (msg == JV_EXC) return JV_EXC; }
    if (!jstr_len(nm)) return msg;
    if (!jstr_len(msg)) return nm;
    jv sep = str_value(J, ": ");
    jv a = jstr_concat(J, nm, sep);
    if (a == JV_EXC) return JV_EXC;
    return jstr_concat(J, a, msg);
}

// Error.captureStackTrace(obj): a V8 extension many libraries call
static jv error_capture(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(argv[0])) return throw_type(J, "Invalid argument");
    struct obj* o = jv_obj(argv[0]);
    struct errobj tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.base = *o;
    tmp.base.h.aux = OC_ERROR;
    tmp.frames = JV_UNDEFINED;
    tmp.stack = JV_UNDEFINED;
    err_capture_stack(J, &tmp.base);
    jv s = format_stack(J, &tmp);
    if (s == JV_EXC) return JV_EXC;
    if (obj_define_value(J, o, A(stack), s, PA_HIDDEN) < 0) return JV_EXC;
    return JV_UNDEFINED;
}

static jv error_is_error(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return jv_bool(jv_is_obj(argv[0]) && obj_class(jv_obj(argv[0])) == OC_ERROR);
}

static const struct bdef error_proto_fns[] = {
    FN("toString", error_to_string, 0, 0),
    GETTER("stack", error_stack_get, 0),
    SETTER("stack", error_stack_set, 0),
};

static const struct bdef error_statics[] = {
    FN("captureStackTrace", error_capture, 1, 0),
    FN("isError", error_is_error, 1, 0),
};

void b_error_classes(void) { class_ops[OC_ERROR].trace = err_trace; }

int b_error_init(ojs* J) {
    b_error_classes();
    struct obj* ep = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!ep) return -1;
    J->I.error_proto = ep;
    struct obj* ec = def_ctor(J, error_ctor, "Error", 1, -1, ep);
    if (!ec) return -1;
    J->I.error_ctor = ec;
    if (DEF_FNS(ec, error_statics) < 0) return -1;
    if (def_value(J, ec, "stackTraceLimit", jv_from_int(10), PA_DEFAULT) < 0) return -1;
    if (DEF_FNS(ep, error_proto_fns) < 0) return -1;
    if (obj_define_value(J, ep, A(name), str_value(J, "Error"), PA_HIDDEN) < 0) return -1;
    if (obj_define_value(J, ep, A(message), jv_from_str(J->A->empty), PA_HIDDEN) < 0) return -1;
    for (int i = 0; i < NE_COUNT; i++) {
        struct obj* p = obj_new(J, ep, OC_OBJECT, 0);
        if (!p) return -1;
        struct obj* c = def_ctor(J, error_ctor, NE_NAMES[i], i == NE_AGGREGATE ? 2 : 1, i, p);
        if (!c) return -1;
        c->proto = ec;   // NativeError constructors inherit from Error
        J->I.native_error_proto[i] = p;
        J->I.native_error_ctor[i] = c;
        if (obj_define_value(J, p, A(name), str_value(J, NE_NAMES[i]), PA_HIDDEN) < 0) return -1;
        if (obj_define_value(J, p, A(message), jv_from_str(J->A->empty), PA_HIDDEN) < 0) return -1;
    }
    return 0;
}
