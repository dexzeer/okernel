// b_json.c — JSON.parse / JSON.stringify / JSON.rawJSON / JSON.isRawJSON
// (ECMA-262 §25.5 and the JSON.parse source text access extension).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

double num_from_decimal(const char* digits, int nd, int exp10);
jv bigint_to_string(ojs* J, jv v, int radix);

// ---------------------------------------------------------------- parse

struct jp {
    ojs* J;
    const struct str* s;
    uint32_t i, n;
    int depth;
    int keep_source;        // record source text of primitives (reviver present)
    struct obj* sources;    // value object -> source text (parallel arrays in an internal array)
};

static inline uint32_t PC(struct jp* p) { return p->i < p->n ? str_at(p->s, p->i) : 0xFFFFFFFFu; }

static void skip_ws(struct jp* p) {
    while (p->i < p->n) {
        uint32_t c = str_at(p->s, p->i);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static jv jp_error(struct jp* p, const char* what) {
    if (p->i >= p->n) return throw_syntax(p->J, "Unexpected end of JSON input");
    uint32_t c = str_at(p->s, p->i);
    if (c >= 32 && c < 127) return throw_syntax(p->J, "Unexpected token '%c', %s in JSON at position %d", (int)c, what, (int)p->i);
    return throw_syntax(p->J, "Unexpected character %s in JSON at position %d", what, (int)p->i);
}

static int hex4(struct jp* p, uint32_t* out) {
    if (p->i + 4 > p->n) return -1;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t c = str_at(p->s, p->i + (uint32_t)k), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') d = (c | 0x20) - 'a' + 10;
        else return -1;
        v = v * 16 + d;
    }
    p->i += 4;
    *out = v;
    return 0;
}

static jv jp_string(struct jp* p) {
    // p->i at the opening quote
    p->i++;
    uint32_t start = p->i;
    // fast scan: no escapes
    while (p->i < p->n) {
        uint32_t c = str_at(p->s, p->i);
        if (c == '"') {
            jv r = jstr_sub(p->J, jv_from_str((struct str*)p->s), start, p->i);
            p->i++;
            return r;
        }
        if (c == '\\' || c < 0x20) break;
        p->i++;
    }
    struct sbuf b;
    sb_init(p->J, &b);
    sb_put_sub(&b, p->s, start, p->i);
    for (;;) {
        if (p->i >= p->n) { sb_free(&b); return jp_error(p, "unterminated string"); }
        uint32_t c = str_at(p->s, p->i);
        if (c == '"') { p->i++; return sb_done(&b); }
        if (c < 0x20) { sb_free(&b); return jp_error(p, "bad control character in string literal"); }
        if (c != '\\') { sb_putc(&b, c); p->i++; continue; }
        p->i++;
        if (p->i >= p->n) { sb_free(&b); return jp_error(p, "bad escape"); }
        c = str_at(p->s, p->i++);
        switch (c) {
        case '"': sb_putc(&b, '"'); break;
        case '\\': sb_putc(&b, '\\'); break;
        case '/': sb_putc(&b, '/'); break;
        case 'b': sb_putc(&b, 8); break;
        case 'f': sb_putc(&b, 12); break;
        case 'n': sb_putc(&b, 10); break;
        case 'r': sb_putc(&b, 13); break;
        case 't': sb_putc(&b, 9); break;
        case 'u': {
            uint32_t u;
            if (hex4(p, &u) < 0) { sb_free(&b); return jp_error(p, "bad Unicode escape"); }
            sb_putc(&b, u);
            break;
        }
        default:
            p->i--;
            sb_free(&b);
            return jp_error(p, "bad escaped character");
        }
    }
}

static jv jp_number(struct jp* p) {
    uint32_t start = p->i;
    int neg = 0;
    if (PC(p) == '-') { neg = 1; p->i++; }
    char dig[800];
    int nd = 0, exp10 = 0, dropped = 0;
    uint32_t c = PC(p);
    if (c == '0') {
        p->i++;
    } else if (c >= '1' && c <= '9') {
        while ((c = PC(p)) >= '0' && c <= '9') {
            if (nd < 780) dig[nd++] = (char)c; else { exp10++; if (c != '0') dropped = 1; }
            p->i++;
        }
    } else return jp_error(p, "no number after minus sign");
    int is_int = 1;
    if (PC(p) == '.') {
        is_int = 0;
        p->i++;
        c = PC(p);
        if (!(c >= '0' && c <= '9')) return jp_error(p, "unterminated fractional number");
        while ((c = PC(p)) >= '0' && c <= '9') {
            if (nd == 0 && c == '0') { exp10--; p->i++; continue; }
            if (nd < 780) { dig[nd++] = (char)c; exp10--; } else if (c != '0') dropped = 1;
            p->i++;
        }
    }
    c = PC(p);
    if (c == 'e' || c == 'E') {
        is_int = 0;
        p->i++;
        int eneg = 0;
        c = PC(p);
        if (c == '+' || c == '-') { eneg = c == '-'; p->i++; }
        c = PC(p);
        if (!(c >= '0' && c <= '9')) return jp_error(p, "exponent part is missing a number");
        int e = 0;
        while ((c = PC(p)) >= '0' && c <= '9') { if (e < 100000) e = e * 10 + (int)(c - '0'); p->i++; }
        exp10 += eneg ? -e : e;
    }
    // small integers directly
    if (is_int && nd <= 9 && exp10 == 0) {
        int32_t v = 0;
        for (int k = 0; k < nd; k++) v = v * 10 + (dig[k] - '0');
        if (neg) return v == 0 ? jv_from_dbl(-0.0) : jv_from_int(-v);
        return jv_from_int(v);
    }
    (void)start;
    if (dropped && nd < 799) { dig[nd++] = '1'; exp10--; }
    double d = nd ? num_from_decimal(dig, nd, exp10) : 0.0;
    return jv_number(neg ? -d : d);
}

static int match_word(struct jp* p, const char* w) {
    uint32_t k = 0;
    for (; w[k]; k++) if (p->i + k >= p->n || str_at(p->s, p->i + k) != (uint8_t)w[k]) return 0;
    p->i += k;
    return 1;
}

static jv jp_value(struct jp* p);

// source text record: a parallel internal array [value, start, end] for primitives
static int note_source(struct jp* p, jv v, uint32_t start, uint32_t end) {
    if (!p->keep_source) return 0;
    struct obj* s = p->sources;
    if (obj_elems_reserve(p->J, s, s->elen + 3) < 0) return -1;
    s->elems[s->elen++] = v;
    s->elems[s->elen++] = jv_from_int((int32_t)start);
    s->elems[s->elen++] = jv_from_int((int32_t)end);
    return 0;
}

static jv jp_value(struct jp* p) {
    ojs* J = p->J;
    skip_ws(p);
    uint32_t c = PC(p);
    uint32_t start = p->i;
    jv v;
    switch (c) {
    case '{': {
        if (++p->depth > 2000 || check_stack(J)) { if (!J->has_exc) throw_range(J, "JSON nesting too deep"); return JV_EXC; }
        p->i++;
        struct obj* o = obj_new_plain(J);
        if (!o) return JV_EXC;
        skip_ws(p);
        if (PC(p) == '}') { p->i++; p->depth--; return jv_from_obj(o); }
        for (;;) {
            skip_ws(p);
            if (PC(p) != '"') return jp_error(p, "expected property name");
            jv k = jp_string(p);
            if (k == JV_EXC) return JV_EXC;
            skip_ws(p);
            if (PC(p) != ':') return jp_error(p, "expected ':' after property name");
            p->i++;
            jv val = jp_value(p);
            if (val == JV_EXC) return JV_EXC;
            pkey pk = pkey_from_value(J, k);
            if (!pk) return JV_EXC;
            if (obj_define_value(J, o, pk, val, PA_DEFAULT) < 0) return JV_EXC;
            skip_ws(p);
            c = PC(p);
            if (c == ',') { p->i++; continue; }
            if (c == '}') { p->i++; break; }
            return jp_error(p, "expected ',' or '}' after property value");
        }
        p->depth--;
        return jv_from_obj(o);
    }
    case '[': {
        if (++p->depth > 2000 || check_stack(J)) { if (!J->has_exc) throw_range(J, "JSON nesting too deep"); return JV_EXC; }
        p->i++;
        struct obj* a = obj_new_array(J, 0);
        if (!a) return JV_EXC;
        skip_ws(p);
        if (PC(p) == ']') { p->i++; p->depth--; return jv_from_obj(a); }
        for (;;) {
            jv val = jp_value(p);
            if (val == JV_EXC) return JV_EXC;
            if (a->elen == a->alen && obj_elems_reserve(J, a, a->elen + 1) < 0) return JV_EXC;
            a->elems[a->elen++] = val;
            a->alen = a->elen;
            skip_ws(p);
            c = PC(p);
            if (c == ',') { p->i++; continue; }
            if (c == ']') { p->i++; break; }
            return jp_error(p, "expected ',' or ']' after array element");
        }
        p->depth--;
        return jv_from_obj(a);
    }
    case '"': v = jp_string(p); break;
    case 't': if (!match_word(p, "true")) return jp_error(p, "is not valid JSON"); v = JV_TRUE; break;
    case 'f': if (!match_word(p, "false")) return jp_error(p, "is not valid JSON"); v = JV_FALSE; break;
    case 'n': if (!match_word(p, "null")) return jp_error(p, "is not valid JSON"); v = JV_NULL; break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) { v = jp_number(p); break; }
        return jp_error(p, "is not valid JSON");
    }
    if (v == JV_EXC) return JV_EXC;
    if (note_source(p, v, start, p->i) < 0) return JV_EXC;
    return v;
}

// InternalizeJSONProperty(holder, name, reviver)
static jv internalize(ojs* J, struct jp* p, jv holder, pkey name, jv reviver, uint32_t* srcpos) {
    if (check_stack(J)) return JV_EXC;
    jv val = obj_get(J, jv_obj(holder), name, holder);
    if (val == JV_EXC) return JV_EXC;
    if (jv_is_obj(val)) {
        int ia = is_array(J, val);
        if (ia < 0) return JV_EXC;
        if (ia) {
            int64_t len;
            if (length_of_array_like(J, jv_obj(val), &len) < 0) return JV_EXC;
            for (int64_t i = 0; i < len; i++) {
                pkey k = PK_FROM_INDEX((uint32_t)i);
                jv nv = internalize(J, p, val, k, reviver, srcpos);
                if (nv == JV_EXC) return JV_EXC;
                if (jv_is_undef(nv)) { if (obj_delete(J, jv_obj(val), k, 0) < 0) return JV_EXC; }
                else if (create_data_property(J, jv_obj(val), k, nv) < 0) return JV_EXC;
            }
        } else {
            jv keys = obj_own_keys(J, jv_obj(val), OWNKEYS_STRINGS | OWNKEYS_ENUM_ONLY);
            if (keys == JV_EXC) return JV_EXC;
            struct obj* ka = jv_obj(keys);
            for (uint32_t i = 0; i < ka->elen; i++) {
                pkey k = pkey_from_value(J, ka->elems[i]);
                if (!k) return JV_EXC;
                jv nv = internalize(J, p, val, k, reviver, srcpos);
                if (nv == JV_EXC) return JV_EXC;
                if (jv_is_undef(nv)) { if (obj_delete(J, jv_obj(val), k, 0) < 0) return JV_EXC; }
                else if (create_data_property(J, jv_obj(val), k, nv) < 0) return JV_EXC;
            }
        }
    }
    jv kv = pkey_to_value(J, name);
    if (kv == JV_EXC) return JV_EXC;
    // context: { source } for primitive values that are still the parsed ones
    struct obj* ctx = obj_new_plain(J);
    if (!ctx) return JV_EXC;
    if (!jv_is_obj(val) && p->sources) {
        struct obj* s = p->sources;
        for (uint32_t i = *srcpos; i < s->elen; i += 3) {
            if (strict_equals(J, s->elems[i], val) || (s->elems[i] == val)) {
                jv src = jstr_sub(J, jv_from_str((struct str*)p->s), (uint32_t)jv_int(s->elems[i + 1]), (uint32_t)jv_int(s->elems[i + 2]));
                if (src == JV_EXC) return JV_EXC;
                if (def_value(J, ctx, "source", src, PA_DEFAULT) < 0) return JV_EXC;
                *srcpos = i + 3;
                break;
            }
        }
    }
    jv args[3] = { kv, val, jv_from_obj(ctx) };
    return ojs_call_v(J, reviver, holder, 3, args);
}

jv json_parse(ojs* J, struct str* s, jv reviver) {
    struct jp p;
    memset(&p, 0, sizeof p);
    p.J = J;
    p.s = s;
    p.n = str_len(s);
    p.keep_source = is_callable(reviver);
    if (p.keep_source) {
        p.sources = obj_new_array(J, 0);
        if (!p.sources) return JV_EXC;
    }
    jv v = jp_value(&p);
    if (v == JV_EXC) return JV_EXC;
    skip_ws(&p);
    if (p.i < p.n) return jp_error(&p, "unexpected non-whitespace character after JSON data");
    if (!p.keep_source) return v;
    struct obj* root = obj_new_plain(J);
    if (!root) return JV_EXC;
    if (obj_define_value(J, root, A(empty), v, PA_DEFAULT) < 0) return JV_EXC;
    uint32_t pos = 0;
    return internalize(J, &p, jv_from_obj(root), A(empty), reviver, &pos);
}

static jv json_parse_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    return json_parse(J, s, argv[1]);
}

// ---------------------------------------------------------------- rawJSON

static jv json_raw(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    uint32_t n = str_len(s);
    if (!n) return throw_syntax(J, "Invalid value for JSON.rawJSON");
    uint32_t c0 = str_at(s, 0), c1 = str_at(s, n - 1);
    if (c0 == '\t' || c0 == '\n' || c0 == '\r' || c0 == ' ' || c1 == '\t' || c1 == '\n' || c1 == '\r' || c1 == ' ')
        return throw_syntax(J, "Invalid value for JSON.rawJSON");
    jv v = json_parse(J, s, JV_UNDEFINED);
    if (v == JV_EXC) return JV_EXC;
    if (jv_is_obj(v)) return throw_syntax(J, "Invalid value for JSON.rawJSON");
    struct obj* o = obj_new(J, 0, OC_RAW_JSON, 0);
    if (!o) return JV_EXC;
    if (def_value(J, o, "rawJSON", jv_from_str(s), PA_DEFAULT) < 0) return JV_EXC;
    if (set_integrity(J, o, 1) < 0) return JV_EXC;
    return jv_from_obj(o);
}

static int is_raw_json(jv v) { return jv_is_obj(v) && obj_class(jv_obj(v)) == OC_RAW_JSON; }

static jv json_is_raw(ojs* J, jv this_v, int argc, jv* argv, int magic) { return jv_bool(is_raw_json(argv[0])); }

// ---------------------------------------------------------------- stringify

struct js {
    ojs* J;
    struct sbuf b;
    jv replacer;            // function or undefined
    struct obj* plist;      // property list (array of keys) or NULL
    struct str* gap;        // indent unit or NULL
    struct obj* stack;      // objects being serialized (cycle detection)
    int depth;
};

static const char HEX[] = "0123456789abcdef";

static void quote(struct sbuf* b, const struct str* s) {
    sb_putc(b, '"');
    uint32_t n = str_len(s);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        switch (c) {
        case '"': sb_puts(b, "\\\""); continue;
        case '\\': sb_puts(b, "\\\\"); continue;
        case 8: sb_puts(b, "\\b"); continue;
        case 12: sb_puts(b, "\\f"); continue;
        case 10: sb_puts(b, "\\n"); continue;
        case 13: sb_puts(b, "\\r"); continue;
        case 9: sb_puts(b, "\\t"); continue;
        }
        int lone = 0;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 < n && str_at(s, i + 1) >= 0xDC00 && str_at(s, i + 1) <= 0xDFFF) {
                sb_putc(b, c);
                sb_putc(b, str_at(s, ++i));
                continue;
            }
            lone = 1;
        } else if (c >= 0xDC00 && c <= 0xDFFF) lone = 1;
        if (c < 0x20 || lone) {
            sb_puts(b, "\\u");
            sb_putc(b, HEX[(c >> 12) & 15]);
            sb_putc(b, HEX[(c >> 8) & 15]);
            sb_putc(b, HEX[(c >> 4) & 15]);
            sb_putc(b, HEX[c & 15]);
            continue;
        }
        sb_putc(b, c);
    }
    sb_putc(b, '"');
}

static void newline_indent(struct js* x) {
    if (!x->gap) return;
    sb_putc(&x->b, '\n');
    for (int i = 0; i < x->depth; i++) sb_put_str(&x->b, x->gap);
}

static int ser_value(struct js* x, jv holder, jv key, jv value);

static int stack_push(struct js* x, jv v) {
    struct obj* s = x->stack;
    for (uint32_t i = 0; i < s->elen; i++) if (s->elems[i] == v) { throw_type(x->J, "Converting circular structure to JSON"); return -1; }
    if (obj_elems_reserve(x->J, s, s->elen + 1) < 0) return -1;
    s->elems[s->elen++] = v;
    return 0;
}

static int ser_object(struct js* x, jv v) {
    ojs* J = x->J;
    if (stack_push(x, v) < 0) return -1;
    if (check_stack(J)) return -1;
    struct obj* o = jv_obj(v);
    jv keys;
    if (x->plist) keys = jv_from_obj(x->plist);
    else {
        keys = obj_own_keys(J, o, OWNKEYS_STRINGS | OWNKEYS_ENUM_ONLY);
        if (keys == JV_EXC) return -1;
    }
    struct obj* ka = jv_obj(keys);
    sb_putc(&x->b, '{');
    x->depth++;
    int any = 0;
    for (uint32_t i = 0; i < ka->elen; i++) {
        jv k = ka->elems[i];
        uint32_t mark = x->b.len;
        if (any) sb_putc(&x->b, ',');
        newline_indent(x);
        struct str* ks = str_flat(J, k);
        if (!ks) return -1;
        quote(&x->b, ks);
        sb_putc(&x->b, ':');
        if (x->gap) sb_putc(&x->b, ' ');
        int r = ser_value(x, v, k, JV_HOLE);
        if (r < 0) return -1;
        if (r == 0) { x->b.len = mark; continue; }   // undefined: drop the member
        any = 1;
    }
    x->depth--;
    if (any) newline_indent(x);
    sb_putc(&x->b, '}');
    x->stack->elen--;
    return 1;
}

static int ser_array(struct js* x, jv v) {
    ojs* J = x->J;
    if (stack_push(x, v) < 0) return -1;
    if (check_stack(J)) return -1;
    int64_t len;
    if (length_of_array_like(J, jv_obj(v), &len) < 0) return -1;
    sb_putc(&x->b, '[');
    x->depth++;
    for (int64_t i = 0; i < len; i++) {
        if (i) sb_putc(&x->b, ',');
        newline_indent(x);
        jv k = jv_number((double)i);
        int r = ser_value(x, v, k, JV_HOLE);
        if (r < 0) return -1;
        if (r == 0) sb_puts(&x->b, "null");
        if (x->b.oom) { throw_oom(J); return -1; }
    }
    x->depth--;
    if (len) newline_indent(x);
    sb_putc(&x->b, ']');
    x->stack->elen--;
    return 1;
}

// SerializeJSONProperty: 1 written, 0 undefined (nothing written), -1 exception.
// value JV_HOLE: read holder[key]
static int ser_value(struct js* x, jv holder, jv key, jv value) {
    ojs* J = x->J;
    if (value == JV_HOLE) {
        pkey k = pkey_from_value(J, key);
        if (!k) return -1;
        value = obj_get_v(J, holder, k);
        if (value == JV_EXC) return -1;
    }
    if (jv_is_obj(value) || jv_is_big(value)) {
        jv tj = obj_get_v(J, value, A(toJSON));
        if (tj == JV_EXC) return -1;
        if (is_callable(tj)) {
            jv ks = jv_is_str(key) ? key : to_string(J, key);
            if (ks == JV_EXC) return -1;
            value = ojs_call_v(J, tj, value, 1, &ks);
            if (value == JV_EXC) return -1;
        }
    }
    if (!jv_is_undef(x->replacer)) {
        jv ks = jv_is_str(key) ? key : to_string(J, key);
        if (ks == JV_EXC) return -1;
        jv args[2] = { ks, value };
        value = ojs_call_v(J, x->replacer, holder, 2, args);
        if (value == JV_EXC) return -1;
    }
    if (jv_is_obj(value)) {
        struct obj* o = jv_obj(value);
        if (is_raw_json(value)) {
            struct pdesc d;
            pkey rk = pkey_from_cstr(J, "rawJSON");
            if (rk && ord_get_own(J, o, rk, &d) > 0 && jv_is_str(d.value)) {
                struct str* s = str_flat(J, d.value);
                if (!s) return -1;
                sb_put_str(&x->b, s);
                return 1;
            }
        }
        switch (obj_class(o)) {
        case OC_NUMBER: {
            jv n = to_number(J, value);
            if (n == JV_EXC) return -1;
            value = n;
            break;
        }
        case OC_STRING: {
            jv s = to_string(J, value);
            if (s == JV_EXC) return -1;
            value = s;
            break;
        }
        case OC_BOOLEAN: case OC_BIGINT: value = ((struct prim*)o)->v; break;
        default: break;
        }
    }
    switch (JV_TAG(value)) {
    case TAG_SPECIAL:
        if (value == JV_NULL) { sb_puts(&x->b, "null"); return 1; }
        if (value == JV_TRUE) { sb_puts(&x->b, "true"); return 1; }
        if (value == JV_FALSE) { sb_puts(&x->b, "false"); return 1; }
        return 0;
    case TAG_STR: {
        struct str* s = str_flat(J, value);
        if (!s) return -1;
        quote(&x->b, s);
        return 1;
    }
    case TAG_SYM: return 0;
    case TAG_BIG: throw_type(J, "Do not know how to serialize a BigInt"); return -1;
    case TAG_OBJ: {
        if (is_callable(value)) return 0;
        int ia = is_array(J, value);
        if (ia < 0) return -1;
        return ia ? ser_array(x, value) : ser_object(x, value);
    }
    default: {
        double d = jv_num(value);
        if (d != d || d == 1.0 / 0.0 || d == -1.0 / 0.0) { sb_puts(&x->b, "null"); return 1; }
        if (jv_is_int(value)) {
            char buf[16];
            ojs_snprintf(buf, sizeof buf, "%d", jv_int(value));
            sb_puts(&x->b, buf);
            return 1;
        }
        char buf[40];
        num_to_cstr(d, buf);
        sb_puts(&x->b, buf);
        return 1;
    }
    }
}

jv json_stringify(ojs* J, jv value, jv replacer, jv space) {
    struct js x;
    memset(&x, 0, sizeof x);
    x.J = J;
    x.replacer = JV_UNDEFINED;
    x.stack = obj_new_array(J, 0);
    if (!x.stack) return JV_EXC;
    if (jv_is_obj(replacer)) {
        if (is_callable(replacer)) x.replacer = replacer;
        else {
            int ia = is_array(J, replacer);
            if (ia < 0) return JV_EXC;
            if (ia) {
                struct obj* pl = obj_new_array(J, 0);
                struct obj* seen = obj_new(J, 0, OC_OBJECT, 0);
                if (!pl || !seen) return JV_EXC;
                int64_t len;
                if (length_of_array_like(J, jv_obj(replacer), &len) < 0) return JV_EXC;
                for (int64_t i = 0; i < len; i++) {
                    jv v = obj_get(J, jv_obj(replacer), PK_FROM_INDEX((uint32_t)i), replacer);
                    if (v == JV_EXC) return JV_EXC;
                    jv item = JV_UNDEFINED;
                    if (jv_is_str(v)) item = v;
                    else if (jv_is_number(v)) item = to_string(J, v);
                    else if (jv_is_obj(v) && (obj_class(jv_obj(v)) == OC_STRING || obj_class(jv_obj(v)) == OC_NUMBER)) item = to_string(J, v);
                    if (item == JV_EXC) return JV_EXC;
                    if (jv_is_undef(item)) continue;
                    pkey k = pkey_from_value(J, item);
                    if (!k) return JV_EXC;
                    if (ord_get_own(J, seen, k, 0)) continue;
                    if (obj_define_value(J, seen, k, JV_TRUE, PA_DEFAULT) < 0) return JV_EXC;
                    if (obj_elems_reserve(J, pl, pl->elen + 1) < 0) return JV_EXC;
                    pl->elems[pl->elen++] = item;
                    pl->alen = pl->elen;
                }
                x.plist = pl;
            }
        }
    }
    if (jv_is_obj(space)) {
        int c = obj_class(jv_obj(space));
        if (c == OC_NUMBER) { space = to_number(J, space); if (space == JV_EXC) return JV_EXC; }
        else if (c == OC_STRING) { space = to_string(J, space); if (space == JV_EXC) return JV_EXC; }
    }
    if (jv_is_number(space)) {
        double n;
        if (to_integer_or_inf(J, space, &n) < 0) return JV_EXC;
        if (n > 10) n = 10;
        if (n >= 1) {
            char sp[11];
            int k = (int)n;
            memset(sp, ' ', (size_t)k);
            x.gap = str_new8(J, (const uint8_t*)sp, (uint32_t)k);
            if (!x.gap) return JV_EXC;
        }
    } else if (jv_is_str(space)) {
        struct str* s = str_flat(J, space);
        if (!s) return JV_EXC;
        if (str_len(s) > 10) {
            jv t = jstr_sub(J, jv_from_str(s), 0, 10);
            if (t == JV_EXC) return JV_EXC;
            s = jv_str(t);
        }
        if (str_len(s)) x.gap = s;
    }
    struct obj* wrapper = obj_new_plain(J);
    if (!wrapper) return JV_EXC;
    if (obj_define_value(J, wrapper, A(empty), value, PA_DEFAULT) < 0) return JV_EXC;
    sb_init(J, &x.b);
    int r = ser_value(&x, jv_from_obj(wrapper), jv_from_str(J->A->empty), value);
    if (r < 0) { sb_free(&x.b); return JV_EXC; }
    if (r == 0) { sb_free(&x.b); return JV_UNDEFINED; }
    return sb_done(&x.b);
}

static jv json_stringify_fn(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return json_stringify(J, argv[0], argv[1], argv[2]);
}

static const struct bdef json_fns[] = {
    FN("parse", json_parse_fn, 2, 0),
    FN("stringify", json_stringify_fn, 3, 0),
    FN("rawJSON", json_raw, 1, 0),
    FN("isRawJSON", json_is_raw, 1, 0),
};

int b_json_init(ojs* J) {
    struct obj* o = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!o || DEF_FNS(o, json_fns) < 0) return -1;
    if (def_value(J, o, "@@toStringTag", str_value(J, "JSON"), PA_CONFIGURABLE) < 0) return -1;
    return def_global(J, "JSON", jv_from_obj(o));
}
