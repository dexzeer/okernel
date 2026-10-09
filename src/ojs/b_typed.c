// b_typed.c — ArrayBuffer, SharedArrayBuffer, the TypedArray family,
// DataView and Atomics (ECMA-262 §25, §23.2).
//
// Buffer storage comes from the system allocator (freed by a
// finalizer); the engine heap only holds the small buffer objects.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

jv bigint_from_i64(ojs* J, int64_t v);
jv bigint_from_u64(ojs* J, uint64_t v);
int to_bigint64(ojs* J, jv v, uint64_t* out);   // ToBigInt then mod 2^64
jv to_bigint(ojs* J, jv v);
double km_trunc(double);
jv array_iter_new(ojs* J, jv target, int kind);
jv get_substitution(ojs* J, struct str* matched, struct str* s, uint32_t pos, struct obj* captures, jv named, struct str* repl);

#define WK(i) pk_from_sym(J->wk[i])
#define MAX_BYTES 0x7FFFFFFFu   // 2 GiB - 1: larger buffers are a RangeError

// ---------------------------------------------------------------- buffers

struct abuf {
    struct obj base;
    uint8_t* data;          // NULL when detached (or length 0)
    uint32_t len;
    uint32_t max;           // resizable / growable maximum
    int detached;
    int resizable;
    int shared;
    int pad;
};

static void abuf_finalize(ojs* J, struct obj* o) {
    struct abuf* b = (struct abuf*)o;
    gc_ext_free(J, b->data, b->max);
    b->data = 0;
}

static struct abuf* abuf_new(ojs* J, struct obj* proto, uint32_t len, uint32_t max, int resizable, int shared) {
    struct abuf* b = (struct abuf*)obj_new(J, proto, shared ? OC_SHAREDARRAYBUFFER : OC_ARRAYBUFFER, sizeof(struct abuf));
    if (!b) return 0;
    uint32_t alloc = resizable ? max : len;
    if (alloc) {
        b->data = (uint8_t*)gc_ext_alloc(J, alloc);
        if (!b->data) { throw_range(J, "Array buffer allocation failed"); return 0; }
    }
    b->len = len;
    b->max = resizable ? max : len;
    b->resizable = resizable;
    b->shared = shared;
    return b;
}

static int to_byte_len(ojs* J, jv v, uint32_t* out) {
    uint64_t n;
    if (to_index(J, v, &n) < 0) return -1;
    if (n > MAX_BYTES) { throw_range(J, "Array buffer allocation failed"); return -1; }
    *out = (uint32_t)n;
    return 0;
}

// magic: 0 ArrayBuffer, 1 SharedArrayBuffer
static jv abuf_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Constructor %s requires 'new'", magic ? "SharedArrayBuffer" : "ArrayBuffer");
    uint32_t len, max = 0;
    if (to_byte_len(J, argv[0], &len) < 0) return JV_EXC;
    int resizable = 0;
    if (jv_is_obj(argv[1])) {
        jv m = obj_get(J, jv_obj(argv[1]), A(maxByteLength), argv[1]);
        if (m == JV_EXC) return JV_EXC;
        if (!jv_is_undef(m)) {
            if (to_byte_len(J, m, &max) < 0) return JV_EXC;
            if (len > max) return throw_range(J, "byteLength exceeds maxByteLength");
            resizable = 1;
        }
    }
    struct obj* proto = get_proto_from_ctor(J, nt, magic ? J->I.sharedarraybuffer_proto : J->I.arraybuffer_proto);
    if (!proto) return JV_EXC;
    struct abuf* b = abuf_new(J, proto, len, max, resizable, magic);
    return b ? jv_from_obj(&b->base) : JV_EXC;
}

static struct abuf* this_buf(ojs* J, jv t, int shared, const char* m) {
    int cls = shared ? OC_SHAREDARRAYBUFFER : OC_ARRAYBUFFER;
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == cls) return (struct abuf*)jv_obj(t);
    throw_type(J, "%s called on incompatible receiver", m);
    return 0;
}

// getters: magic = what | shared<<4 (0 byteLength, 1 maxByteLength, 2 resizable/growable, 3 detached)
static jv abuf_getter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct abuf* b = this_buf(J, this_v, magic >> 4, "ArrayBuffer getter");
    if (!b) return JV_EXC;
    switch (magic & 15) {
    case 0: return jv_number(b->detached ? 0 : b->len);
    case 1: return jv_number(b->detached ? 0 : b->max);
    case 2: return jv_bool(b->resizable);
    default: return jv_bool(b->detached);
    }
}

static jv abuf_is_view(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(argv[0])) return JV_FALSE;
    int c = obj_class(jv_obj(argv[0]));
    return jv_bool(c == OC_TYPEDARRAY || c == OC_DATAVIEW);
}

static jv abuf_species(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

static jv abuf_slice(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct abuf* b = this_buf(J, this_v, magic, "ArrayBuffer.prototype.slice");
    if (!b) return JV_EXC;
    if (b->detached) return throw_type(J, "ArrayBuffer is detached");
    double len = b->len, first, fin;
    if (to_integer_or_inf(J, argv[0], &first) < 0) return JV_EXC;
    if (first < 0) { first += len; if (first < 0) first = 0; } else if (first > len) first = len;
    if (jv_is_undef(argv[1])) fin = len;
    else {
        if (to_integer_or_inf(J, argv[1], &fin) < 0) return JV_EXC;
        if (fin < 0) { fin += len; if (fin < 0) fin = 0; } else if (fin > len) fin = len;
    }
    double newlen = fin - first > 0 ? fin - first : 0;
    jv dflt = jv_from_obj(magic ? J->I.sharedarraybuffer_ctor : J->I.arraybuffer_ctor);
    jv c = species_constructor(J, &b->base, dflt);
    if (c == JV_EXC) return JV_EXC;
    jv n = jv_number(newlen);
    jv nv = ojs_construct_v(J, c, 1, &n, c);
    if (nv == JV_EXC) return JV_EXC;
    struct abuf* nb = jv_is_obj(nv) && obj_class(jv_obj(nv)) == (magic ? OC_SHAREDARRAYBUFFER : OC_ARRAYBUFFER) ? (struct abuf*)jv_obj(nv) : 0;
    if (!nb) return throw_type(J, "Species constructor did not return an ArrayBuffer");
    if (nb->detached) return throw_type(J, "New ArrayBuffer is detached");
    if (nb == b) return throw_type(J, "ArrayBuffer subclass returned this from species constructor");
    if (nb->len < newlen) return throw_type(J, "Species constructor returned a too small buffer");
    if (b->detached) return throw_type(J, "ArrayBuffer is detached");
    // the source may have shrunk
    double cur = b->len;
    if (first < cur) {
        double cnt = newlen;
        if (first + cnt > cur) cnt = cur - first;
        if (cnt > 0) memmove(nb->data, b->data + (uint32_t)first, (size_t)cnt);
    }
    return nv;
}

static jv abuf_resize(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct abuf* b = this_buf(J, this_v, magic, magic ? "SharedArrayBuffer.prototype.grow" : "ArrayBuffer.prototype.resize");
    if (!b) return JV_EXC;
    if (!b->resizable) return throw_type(J, "%s is not %s", magic ? "SharedArrayBuffer" : "ArrayBuffer", magic ? "growable" : "resizable");
    uint64_t n;
    if (to_index(J, argv[0], &n) < 0) return JV_EXC;
    if (b->detached) return throw_type(J, "ArrayBuffer is detached");
    if (n > b->max) return throw_range(J, "Invalid length parameter");
    if (magic && n < b->len) return throw_range(J, "SharedArrayBuffer cannot shrink");
    if (n < b->len) memset(b->data + n, 0, b->len - (uint32_t)n);
    b->len = (uint32_t)n;
    return JV_UNDEFINED;
}

// transfer (magic 0) / transferToFixedLength (magic 1)
static jv abuf_transfer(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct abuf* b = this_buf(J, this_v, 0, "ArrayBuffer.prototype.transfer");
    if (!b) return JV_EXC;
    uint32_t newlen;
    if (jv_is_undef(argv[0])) newlen = b->detached ? 0 : b->len;
    else if (to_byte_len(J, argv[0], &newlen) < 0) return JV_EXC;
    if (b->detached) return throw_type(J, "ArrayBuffer is detached");
    int resizable = !magic && b->resizable;
    uint32_t max = resizable ? b->max : newlen;
    if (resizable && newlen > max) return throw_range(J, "Invalid length parameter");
    struct abuf* nb = abuf_new(J, J->I.arraybuffer_proto, newlen, max, resizable, 0);
    if (!nb) return JV_EXC;
    uint32_t n = newlen < b->len ? newlen : b->len;
    if (n) memcpy(nb->data, b->data, n);
    gc_ext_free(J, b->data, b->max);
    b->data = 0;
    b->len = 0;
    b->max = 0;
    b->detached = 1;
    return jv_from_obj(&nb->base);
}

int ojs_detach_buffer(ojs* J, jv v) {
    if (!jv_is_obj(v) || obj_class(jv_obj(v)) != OC_ARRAYBUFFER) return -1;
    struct abuf* b = (struct abuf*)jv_obj(v);
    gc_ext_free(J, b->data, b->max);
    b->data = 0;
    b->len = 0;
    b->max = 0;
    b->detached = 1;
    return 0;
}

// ---------------------------------------------------------------- element types

enum { T_I8, T_U8, T_U8C, T_I16, T_U16, T_I32, T_U32, T_F16, T_F32, T_F64, T_BI64, T_BU64, T_COUNT };
static const uint8_t TSIZE[T_COUNT] = { 1, 1, 1, 2, 2, 4, 4, 2, 4, 8, 8, 8 };
static const char* const TNAME[T_COUNT] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
    "Float16Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array",
};
static int is_bigint_type(int t) { return t == T_BI64 || t == T_BU64; }

// binary16 <-> double
static double f16_to_double(uint16_t h) {
    int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    double v;
    if (e == 0) v = m / 16777216.0;                     // m * 2^-24
    else if (e == 31) v = m ? 0.0 / 0.0 : 1.0 / 0.0;
    else {
        v = (1024 + m) / 1024.0;
        int x = e - 15;
        while (x > 0) { v *= 2; x--; }
        while (x < 0) { v /= 2; x++; }
    }
    return s ? -v : v;
}

static uint16_t double_to_f16(double d) {
    uint16_t sign = 0;
    if (d != d) return 0x7E00;
    if (d < 0 || (d == 0 && 1 / d < 0)) { sign = 0x8000; d = -d; }
    if (d == 1.0 / 0.0) return sign | 0x7C00;
    if (d == 0) return sign;
    if (d >= 65520.0) return sign | 0x7C00;
    if (d < 6.103515625e-05) {
        // subnormal: round(d * 2^24) ties to even
        double q = d * 16777216.0;
        double fl = (double)(uint32_t)q, diff = q - fl;
        uint32_t m = (uint32_t)fl;
        if (diff > 0.5 || (diff == 0.5 && (m & 1))) m++;
        return (uint16_t)(sign | m);   // m == 1024 becomes the smallest normal
    }
    int e = 0;
    double m = d;
    while (m >= 2) { m /= 2; e++; }
    while (m < 1) { m *= 2; e--; }
    double q = (m - 1) * 1024.0;
    double fl = (double)(uint32_t)q, diff = q - fl;
    uint32_t f = (uint32_t)fl;
    if (diff > 0.5 || (diff == 0.5 && (f & 1))) f++;
    if (f == 1024) { f = 0; e++; }
    if (e + 15 >= 31) return sign | 0x7C00;
    return (uint16_t)(sign | ((e + 15) << 10) | f);
}

static jv read_elem(ojs* J, int type, const uint8_t* p) {
    switch (type) {
    case T_I8: return jv_from_int(*(const int8_t*)p);
    case T_U8: case T_U8C: return jv_from_int(*p);
    case T_I16: { int16_t v; memcpy(&v, p, 2); return jv_from_int(v); }
    case T_U16: { uint16_t v; memcpy(&v, p, 2); return jv_from_int(v); }
    case T_I32: { int32_t v; memcpy(&v, p, 4); return jv_from_int(v); }
    case T_U32: { uint32_t v; memcpy(&v, p, 4); return jv_number((double)v); }
    case T_F16: { uint16_t v; memcpy(&v, p, 2); return jv_number(f16_to_double(v)); }
    case T_F32: { float v; memcpy(&v, p, 4); return jv_from_dbl((double)v); }
    case T_F64: { double v; memcpy(&v, p, 8); return jv_from_dbl(v); }
    case T_BI64: { int64_t v; memcpy(&v, p, 8); return bigint_from_i64(J, v); }
    default: { uint64_t v; memcpy(&v, p, 8); return bigint_from_u64(J, v); }
    }
}

// numeric value already converted (number or bigint) -> bytes
static void write_conv(int type, uint8_t* p, double d, uint64_t big) {
    switch (type) {
    case T_I8: case T_U8: { uint8_t v = (uint8_t)dtoi32(d); *p = v; break; }
    case T_U8C: {
        uint8_t v;
        if (!(d > 0)) v = 0;
        else if (d >= 255) v = 255;
        else {
            double f = (double)(uint32_t)d, r = d - f;
            uint32_t n = (uint32_t)f;
            if (r > 0.5 || (r == 0.5 && (n & 1))) n++;
            v = (uint8_t)n;
        }
        *p = v;
        break;
    }
    case T_I16: case T_U16: { uint16_t v = (uint16_t)dtoi32(d); memcpy(p, &v, 2); break; }
    case T_I32: case T_U32: { uint32_t v = (uint32_t)dtoi32(d); memcpy(p, &v, 4); break; }
    case T_F16: { uint16_t v = double_to_f16(d); memcpy(p, &v, 2); break; }
    case T_F32: { volatile float v = (float)d; float w = v; memcpy(p, &w, 4); break; }
    case T_F64: memcpy(p, &d, 8); break;
    default: memcpy(p, &big, 8); break;
    }
}

// ToNumber / ToBigInt for an element type
static int conv_value(ojs* J, int type, jv v, double* d, uint64_t* big) {
    if (is_bigint_type(type)) return to_bigint64(J, v, big);
    if (jv_is_int(v)) { *d = jv_int(v); return 0; }
    return to_number_d(J, v, d);
}

// ---------------------------------------------------------------- typed arrays

struct tarr {
    struct obj base;
    struct abuf* buf;
    uint32_t offset;
    uint32_t length;        // element count (unless length tracking)
    int type;
    int tracking;           // length follows a resizable buffer
};

static void tarr_trace(ojs* J, struct obj* o) { gc_mark_ptr(J, ((struct tarr*)o)->buf); }

// IsTypedArrayOutOfBounds + current length; 1 in bounds (len set), 0 out of bounds / detached
static int ta_len(struct tarr* t, uint32_t* len) {
    struct abuf* b = t->buf;
    if (b->detached) { *len = 0; return 0; }
    uint32_t bl = b->len;
    if (t->offset > bl) { *len = 0; return 0; }
    if (t->tracking) { *len = (bl - t->offset) / TSIZE[t->type]; return 1; }
    if ((uint64_t)t->offset + (uint64_t)t->length * TSIZE[t->type] > bl) { *len = 0; return 0; }
    *len = t->length;
    return 1;
}

int typed_array_length(ojs* J, struct obj* o, uint32_t* len) {
    if (!ta_len((struct tarr*)o, len)) { throw_type(J, "TypedArray is detached or out of bounds"); return -1; }
    return 0;
}

static uint8_t* ta_ptr(struct tarr* t, uint32_t i) { return t->buf->data + t->offset + (size_t)i * TSIZE[t->type]; }

// CanonicalNumericIndexString: 1 numeric (value in *out), 0 not numeric
static int canonical_numeric(ojs* J, pkey k, double* out) {
    if (PK_IS_INDEX(k)) { *out = PK_INDEX(k); return 1; }
    if (!pk_is_str(k)) return 0;
    struct str* s = pk_str(k);
    uint32_t n = str_len(s);
    if (!n) return 0;
    uint32_t c = str_at(s, 0);
    if (!((c >= '0' && c <= '9') || c == '-' || c == 'I' || c == 'N')) return 0;
    if (n == 2 && c == '-' && str_at(s, 1) == '0') { *out = -0.0; return 1; }
    int ok;
    double d = num_from_str(s, &ok);
    jv back = num_to_string(J, d, 10);
    if (back == JV_EXC) { take_exc(J); return 0; }
    if (!str_eq(jv_str(back), s)) return 0;
    *out = d;
    return 1;
}

// IsValidIntegerIndex
static int valid_index(struct tarr* t, double d, uint32_t* idx) {
    if (d != d || d != km_trunc(d) || (d == 0 && 1 / d < 0)) return 0;
    uint32_t len;
    if (!ta_len(t, &len)) return 0;
    if (d < 0 || d >= len) return 0;
    *idx = (uint32_t)d;
    return 1;
}

// interpreter fast paths for an integer index (1 handled, 0 take the generic path)
int typed_get_fast(ojs* J, struct obj* o, uint32_t i, jv* out) {
    struct tarr* t = (struct tarr*)o;
    uint32_t len;
    if (t->type >= T_BI64) return 0;   // BigInt elements allocate
    if (!ta_len(t, &len) || i >= len) { *out = JV_UNDEFINED; return 1; }
    *out = read_elem(J, t->type, ta_ptr(t, i));
    return 1;
}

int typed_set_fast(ojs* J, struct obj* o, uint32_t i, jv v) {
    struct tarr* t = (struct tarr*)o;
    uint32_t len;
    if (t->type >= T_BI64 || !jv_is_number(v)) return 0;   // conversions may run code / throw
    if (ta_len(t, &len) && i < len) write_conv(t->type, ta_ptr(t, i), jv_num(v), 0);
    return 1;   // out of bounds: TypedArraySetElement does nothing
}

static int ta_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d) {
    double n;
    if (!canonical_numeric(J, k, &n)) return ord_get_own(J, o, k, d);
    uint32_t i;
    struct tarr* t = (struct tarr*)o;
    if (!valid_index(t, n, &i)) return 0;
    if (d) {
        jv v = read_elem(J, t->type, ta_ptr(t, i));
        if (v == JV_EXC) return -1;
        d->has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
        d->attrs = PA_DEFAULT;
        d->value = v;
        d->get = d->set = JV_UNDEFINED;
    }
    return 1;
}

static int ta_has(ojs* J, struct obj* o, pkey k) {
    double n;
    if (!canonical_numeric(J, k, &n)) {
        int r = ord_get_own(J, o, k, 0);
        if (r) return r;
        int err = 0;
        struct obj* p = obj_get_proto(J, o, &err);
        if (err) return -1;
        return p ? obj_has(J, p, k) : 0;
    }
    uint32_t i;
    return valid_index((struct tarr*)o, n, &i);
}

static int ta_write(ojs* J, struct tarr* t, double index, jv v) {
    double d = 0;
    uint64_t big = 0;
    if (conv_value(J, t->type, v, &d, &big) < 0) return -1;
    uint32_t i;
    if (valid_index(t, index, &i)) write_conv(t->type, ta_ptr(t, i), d, big);
    return 0;
}

static int ta_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d) {
    double n;
    if (!canonical_numeric(J, k, &n)) return ord_define_own(J, o, k, d);
    uint32_t i;
    struct tarr* t = (struct tarr*)o;
    if (!valid_index(t, n, &i)) return 0;
    if ((d->has & PD_CONFIGURABLE) && !(d->attrs & PA_CONFIGURABLE)) return 0;
    if ((d->has & PD_ENUMERABLE) && !(d->attrs & PA_ENUMERABLE)) return 0;
    if (PD_IS_ACCESSOR(d)) return 0;
    if ((d->has & PD_WRITABLE) && !(d->attrs & PA_WRITABLE)) return 0;
    if (d->has & PD_VALUE) { if (ta_write(J, t, n, d->value) < 0) return -1; }
    return 1;
}

static jv ta_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    double n;
    if (!canonical_numeric(J, k, &n)) {
        struct pdesc d;
        int r = ord_get_own(J, o, k, &d);
        if (r) {
            if (PD_IS_ACCESSOR(&d)) return jv_is_undef(d.get) ? JV_UNDEFINED : ojs_call_v(J, d.get, receiver, 0, 0);
            return d.value;
        }
        int err = 0;
        struct obj* p = obj_get_proto(J, o, &err);
        if (err) return JV_EXC;
        return p ? obj_get(J, p, k, receiver) : JV_UNDEFINED;
    }
    uint32_t i;
    struct tarr* t = (struct tarr*)o;
    if (!valid_index(t, n, &i)) return JV_UNDEFINED;
    return read_elem(J, t->type, ta_ptr(t, i));
}

static int ta_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver) {
    double n;
    if (canonical_numeric(J, k, &n)) {
        struct tarr* t = (struct tarr*)o;
        if (jv_is_obj(receiver) && jv_obj(receiver) == o) {
            if (ta_write(J, t, n, v) < 0) return -1;
            return 1;
        }
        uint32_t i;
        if (!valid_index(t, n, &i)) return 1;
    }
    return ord_set(J, o, k, v, receiver);
}

static int ta_del(ojs* J, struct obj* o, pkey k) {
    double n;
    if (!canonical_numeric(J, k, &n)) return ord_del(J, o, k);
    uint32_t i;
    return !valid_index((struct tarr*)o, n, &i);
}

static jv ta_own_keys(ojs* J, struct obj* o) {
    struct tarr* t = (struct tarr*)o;
    uint32_t len;
    ta_len(t, &len);
    jv rest = ord_own_keys(J, o);
    if (rest == JV_EXC) return JV_EXC;
    struct obj* r = jv_obj(rest);
    struct obj* a = obj_new_array(J, 0);
    if (!a || obj_elems_reserve(J, a, len + r->elen + 1) < 0) return JV_EXC;
    for (uint32_t i = 0; i < len; i++) {
        jv kv = pkey_to_value(J, PK_FROM_INDEX(i));
        if (kv == JV_EXC) return JV_EXC;
        a->elems[i] = kv;
    }
    memcpy(a->elems + len, r->elems, (size_t)r->elen * sizeof(jv));
    a->elen = a->alen = len + r->elen;
    return jv_from_obj(a);
}

// ---------------------------------------------------------------- creation

static struct obj* ta_proto_of(ojs* J, int type) { return J->I.ta_proto[type]; }

static struct tarr* ta_alloc(ojs* J, struct obj* proto, int type) {
    struct tarr* t = (struct tarr*)obj_new(J, proto, OC_TYPEDARRAY, sizeof(struct tarr));
    if (!t) return 0;
    t->type = type;
    t->base.flags &= ~OF_ARRAY_FAST;
    return t;
}

static jv ta_create_len(ojs* J, struct obj* proto, int type, double len) {
    if (len * TSIZE[type] > MAX_BYTES) return throw_range(J, "Invalid typed array length: %d", (int)len);
    struct tarr* t = ta_alloc(J, proto, type);
    if (!t) return JV_EXC;
    struct abuf* b = abuf_new(J, J->I.arraybuffer_proto, (uint32_t)len * TSIZE[type], 0, 0, 0);
    if (!b) return JV_EXC;
    t->buf = b;
    t->length = (uint32_t)len;
    return jv_from_obj(&t->base);
}

static struct tarr* this_ta(ojs* J, jv v, const char* m) {
    if (jv_is_obj(v) && obj_class(jv_obj(v)) == OC_TYPEDARRAY) return (struct tarr*)jv_obj(v);
    throw_type(J, "%s: this is not a typed array", m);
    return 0;
}

// ValidateTypedArray: in bounds, returns length
static struct tarr* valid_ta(ojs* J, jv v, uint32_t* len, const char* m) {
    struct tarr* t = this_ta(J, v, m);
    if (!t) return 0;
    if (!ta_len(t, len)) { throw_type(J, "%s: typed array is detached or out of bounds", m); return 0; }
    return t;
}

// TypedArrayCreateFromConstructor(C, args) + validation
static jv ta_create_from(ojs* J, jv c, int argc, jv* args, double min_len) {
    jv nv = ojs_construct_v(J, c, argc, args, c);
    if (nv == JV_EXC) return JV_EXC;
    uint32_t len;
    struct tarr* t = valid_ta(J, nv, &len, "TypedArray species");
    if (!t) return JV_EXC;
    if (argc == 1 && jv_is_number(args[0]) && len < min_len) return throw_type(J, "TypedArray species constructor returned a too short array");
    return nv;
}

// TypedArraySpeciesCreate(exemplar, args)
static jv ta_species_create(ojs* J, struct tarr* ex, int argc, jv* args, double min_len) {
    jv dflt = jv_from_obj(J->I.ta_ctor[ex->type]);
    jv c = species_constructor(J, &ex->base, dflt);
    if (c == JV_EXC) return JV_EXC;
    jv r = ta_create_from(J, c, argc, args, min_len);
    if (r == JV_EXC) return JV_EXC;
    if (is_bigint_type(((struct tarr*)jv_obj(r))->type) != is_bigint_type(ex->type))
        return throw_type(J, "TypedArray species: content type mismatch");
    return r;
}

static jv ta_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int type = magic;
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Constructor %s requires 'new'", TNAME[type]);
    struct obj* proto = get_proto_from_ctor(J, nt, ta_proto_of(J, type));
    if (!proto) return JV_EXC;
    jv a0 = argv[0];
    if (!jv_is_obj(a0)) {
        uint64_t n;
        if (to_index(J, a0, &n) < 0) return JV_EXC;
        return ta_create_len(J, proto, type, (double)n);
    }
    struct obj* o = jv_obj(a0);
    int cls = obj_class(o);
    if (cls == OC_ARRAYBUFFER || cls == OC_SHAREDARRAYBUFFER) {
        struct abuf* b = (struct abuf*)o;
        uint64_t off;
        if (to_index(J, argv[1], &off) < 0) return JV_EXC;
        if (off % TSIZE[type]) return throw_range(J, "start offset of %s should be a multiple of %d", TNAME[type], TSIZE[type]);
        uint64_t newlen = 0;
        int has_len = !jv_is_undef(argv[2]);
        if (has_len && to_index(J, argv[2], &newlen) < 0) return JV_EXC;
        if (b->detached) return throw_type(J, "ArrayBuffer is detached");
        struct tarr* t = ta_alloc(J, proto, type);
        if (!t) return JV_EXC;
        t->buf = b;
        t->offset = (uint32_t)off;
        if (!has_len && b->resizable) {
            if (off > b->len) return throw_range(J, "Start offset is outside the bounds of the buffer");
            t->tracking = 1;
        } else if (!has_len) {
            if (b->len % TSIZE[type]) return throw_range(J, "byte length of %s should be a multiple of %d", TNAME[type], TSIZE[type]);
            if (off > b->len) return throw_range(J, "Start offset is outside the bounds of the buffer");
            t->length = (b->len - (uint32_t)off) / TSIZE[type];
        } else {
            if (off + newlen * TSIZE[type] > b->len) return throw_range(J, "Invalid typed array length: %d", (int)newlen);
            t->length = (uint32_t)newlen;
        }
        return jv_from_obj(&t->base);
    }
    if (cls == OC_TYPEDARRAY) {
        struct tarr* src = (struct tarr*)o;
        uint32_t len;
        if (!ta_len(src, &len)) return throw_type(J, "source typed array is detached or out of bounds");
        if (is_bigint_type(src->type) != is_bigint_type(type)) return throw_type(J, "Content type mismatch");
        jv r = ta_create_len(J, proto, type, len);
        if (r == JV_EXC) return JV_EXC;
        struct tarr* t = (struct tarr*)jv_obj(r);
        if (src->type == type) memcpy(ta_ptr(t, 0), ta_ptr(src, 0), (size_t)len * TSIZE[type]);
        else for (uint32_t i = 0; i < len; i++) {
            jv v = read_elem(J, src->type, ta_ptr(src, i));
            if (v == JV_EXC || ta_write(J, t, i, v) < 0) return JV_EXC;
        }
        return r;
    }
    // iterable or array-like object
    jv using = get_method(J, a0, WK(WK_ITERATOR));
    if (using == JV_EXC) return JV_EXC;
    jv src = a0;
    if (!jv_is_undef(using)) {
        jv it = ojs_call_v(J, using, a0, 0, 0);
        if (it == JV_EXC) return JV_EXC;
        if (!jv_is_obj(it)) return throw_type(J, "Result of the Symbol.iterator method is not an object");
        jv next = obj_get(J, jv_obj(it), A(next), it);
        if (next == JV_EXC) return JV_EXC;
        struct iterrec r;
        memset(&r, 0, sizeof r);
        r.iter = it;
        r.next = next;
        struct obj* list = obj_new_array(J, 0);
        if (!list) return JV_EXC;
        for (;;) {
            jv v = iter_step_value(J, &r);
            if (v == JV_EXC) return JV_EXC;
            if (v == JV_HOLE) break;
            if (obj_elems_reserve(J, list, list->elen + 1) < 0) return JV_EXC;
            list->elems[list->elen++] = v;
            list->alen = list->elen;
        }
        src = jv_from_obj(list);
    }
    int64_t len;
    if (length_of_array_like(J, jv_obj(src), &len) < 0) return JV_EXC;
    jv r = ta_create_len(J, proto, type, (double)len);
    if (r == JV_EXC) return JV_EXC;
    struct tarr* t = (struct tarr*)jv_obj(r);
    for (int64_t i = 0; i < len; i++) {
        jv v = obj_get(J, jv_obj(src), PK_FROM_INDEX((uint32_t)i), src);
        if (v == JV_EXC || ta_write(J, t, (double)i, v) < 0) return JV_EXC;
    }
    return r;
}

static jv ta_abstract_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    return throw_type(J, "Abstract class TypedArray not directly constructable");
}

// ---------------------------------------------------------------- %TypedArray% statics

static jv ta_from(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv c = this_v;
    if (!is_constructor(c)) return throw_type(J, "TypedArray.from: this is not a constructor");
    jv mapfn = argv[1];
    if (!jv_is_undef(mapfn) && !is_callable(mapfn)) return throw_type(J, "TypedArray.from: mapper is not a function");
    jv using = get_method(J, argv[0], WK(WK_ITERATOR));
    if (using == JV_EXC) return JV_EXC;
    jv src;
    if (!jv_is_undef(using)) {
        jv it = ojs_call_v(J, using, argv[0], 0, 0);
        if (it == JV_EXC) return JV_EXC;
        if (!jv_is_obj(it)) return throw_type(J, "Result of the Symbol.iterator method is not an object");
        jv next = obj_get(J, jv_obj(it), A(next), it);
        if (next == JV_EXC) return JV_EXC;
        struct iterrec r;
        memset(&r, 0, sizeof r);
        r.iter = it;
        r.next = next;
        struct obj* list = obj_new_array(J, 0);
        if (!list) return JV_EXC;
        for (;;) {
            jv v = iter_step_value(J, &r);
            if (v == JV_EXC) return JV_EXC;
            if (v == JV_HOLE) break;
            if (obj_elems_reserve(J, list, list->elen + 1) < 0) return JV_EXC;
            list->elems[list->elen++] = v;
            list->alen = list->elen;
        }
        src = jv_from_obj(list);
    } else {
        src = to_object(J, argv[0]);
        if (src == JV_EXC) return JV_EXC;
    }
    int64_t len;
    if (length_of_array_like(J, jv_obj(src), &len) < 0) return JV_EXC;
    jv n = jv_number((double)len);
    jv tv = ta_create_from(J, c, 1, &n, (double)len);
    if (tv == JV_EXC) return JV_EXC;
    for (int64_t k = 0; k < len; k++) {
        jv v = obj_get(J, jv_obj(src), PK_FROM_INDEX((uint32_t)k), src);
        if (v == JV_EXC) return JV_EXC;
        if (!jv_is_undef(mapfn)) {
            jv args[2] = { v, jv_number((double)k) };
            v = ojs_call_v(J, mapfn, argv[2], 2, args);
            if (v == JV_EXC) return JV_EXC;
        }
        if (obj_set(J, jv_obj(tv), PK_FROM_INDEX((uint32_t)k), v, tv, 1) < 0) return JV_EXC;
    }
    return tv;
}

static jv ta_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!is_constructor(this_v)) return throw_type(J, "TypedArray.of: this is not a constructor");
    jv n = jv_from_int(argc);
    jv tv = ta_create_from(J, this_v, 1, &n, argc);
    if (tv == JV_EXC) return JV_EXC;
    for (int k = 0; k < argc; k++)
        if (obj_set(J, jv_obj(tv), PK_FROM_INDEX((uint32_t)k), argv[k], tv, 1) < 0) return JV_EXC;
    return tv;
}

static jv ta_species(ojs* J, jv this_v, int argc, jv* argv, int magic) { return this_v; }

// ---------------------------------------------------------------- %TypedArray%.prototype

// getters: 0 buffer, 1 byteLength, 2 byteOffset, 3 length, 4 @@toStringTag
static jv ta_getter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (magic == 4) {
        if (!jv_is_obj(this_v) || obj_class(jv_obj(this_v)) != OC_TYPEDARRAY) return JV_UNDEFINED;
        return str_value(J, TNAME[((struct tarr*)jv_obj(this_v))->type]);
    }
    struct tarr* t = this_ta(J, this_v, "TypedArray getter");
    if (!t) return JV_EXC;
    if (magic == 0) return jv_from_obj(&t->buf->base);
    uint32_t len;
    int in = ta_len(t, &len);
    if (magic == 1) return jv_number(in ? (double)len * TSIZE[t->type] : 0);
    if (magic == 2) return jv_number(in ? t->offset : 0);
    return jv_number(in ? len : 0);
}

static jv tp_iter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    if (!valid_ta(J, this_v, &len, "TypedArray iterator")) return JV_EXC;
    return array_iter_new(J, this_v, magic);
}

static jv tp_at(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "at");
    if (!t) return JV_EXC;
    double k;
    if (to_integer_or_inf(J, argv[0], &k) < 0) return JV_EXC;
    if (k < 0) k += len;
    if (k < 0 || k >= len) return JV_UNDEFINED;
    return obj_get(J, &t->base, PK_FROM_INDEX((uint32_t)k), this_v);
}

static int rel(ojs* J, jv v, double len, double dflt, double* out) {
    if (jv_is_undef(v)) { *out = dflt; return 0; }
    double r;
    if (to_integer_or_inf(J, v, &r) < 0) return -1;
    if (r < 0) { r += len; if (r < 0) r = 0; } else if (r > len) r = len;
    *out = r;
    return 0;
}

static jv tp_fill(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "fill");
    if (!t) return JV_EXC;
    double d = 0;
    uint64_t big = 0;
    if (conv_value(J, t->type, argv[0], &d, &big) < 0) return JV_EXC;
    double k, fin;
    if (rel(J, argv[1], len, 0, &k) < 0 || rel(J, argv[2], len, len, &fin) < 0) return JV_EXC;
    if (!ta_len(t, &len)) return throw_type(J, "fill: typed array is detached or out of bounds");
    if (fin > len) fin = len;
    for (; k < fin; k++) write_conv(t->type, ta_ptr(t, (uint32_t)k), d, big);
    return this_v;
}

static jv tp_copy_within(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "copyWithin");
    if (!t) return JV_EXC;
    double to, from, fin;
    if (rel(J, argv[0], len, 0, &to) < 0 || rel(J, argv[1], len, 0, &from) < 0 || rel(J, argv[2], len, len, &fin) < 0) return JV_EXC;
    double count = fin - from < len - to ? fin - from : len - to;
    if (count > 0) {
        if (!ta_len(t, &len)) return throw_type(J, "copyWithin: typed array is detached or out of bounds");
        uint32_t sz = TSIZE[t->type];
        double bytelen = (double)len * sz;
        double toB = to * sz, fromB = from * sz, cntB = count * sz;
        if (fromB + cntB > bytelen) cntB = bytelen - fromB;
        if (toB + cntB > bytelen) cntB = bytelen - toB;
        if (cntB > 0) memmove(ta_ptr(t, 0) + (uint32_t)toB, ta_ptr(t, 0) + (uint32_t)fromB, (size_t)cntB);
    }
    return this_v;
}

static jv tp_reverse(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "reverse");
    if (!t) return JV_EXC;
    uint32_t sz = TSIZE[t->type];
    uint8_t tmp[8];
    for (uint32_t lo = 0, hi = len ? len - 1 : 0; lo < hi; lo++, hi--) {
        memcpy(tmp, ta_ptr(t, lo), sz);
        memcpy(ta_ptr(t, lo), ta_ptr(t, hi), sz);
        memcpy(ta_ptr(t, hi), tmp, sz);
    }
    return this_v;
}

static jv tp_index_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, magic == 2 ? "includes" : "indexOf");
    if (!t) return JV_EXC;
    if (len == 0) return magic == 2 ? JV_FALSE : jv_from_int(-1);
    double k;
    if (magic == 1) {
        if (argc > 1) {
            if (to_integer_or_inf(J, argv[1], &k) < 0) return JV_EXC;
            if (k == -1.0 / 0.0) return jv_from_int(-1);
            if (k >= 0) { if (k > len - 1.0) k = len - 1.0; } else k += len;
        } else k = len - 1.0;
        for (; k >= 0; k--) {
            uint32_t cur;
            if (!ta_len(t, &cur) || k >= cur) continue;
            jv v = read_elem(J, t->type, ta_ptr(t, (uint32_t)k));
            if (v == JV_EXC) return JV_EXC;
            if (strict_equals(J, v, argv[0])) return jv_number(k);
        }
        return jv_from_int(-1);
    }
    if (to_integer_or_inf(J, argv[1], &k) < 0) return JV_EXC;
    if (k == 1.0 / 0.0) return magic == 2 ? JV_FALSE : jv_from_int(-1);
    if (k < 0) { k += len; if (k < 0) k = 0; }
    for (; k < len; k++) {
        uint32_t cur;
        jv v;
        if (!ta_len(t, &cur) || k >= cur) {
            if (magic != 2) continue;
            v = JV_UNDEFINED;
        } else {
            v = read_elem(J, t->type, ta_ptr(t, (uint32_t)k));
            if (v == JV_EXC) return JV_EXC;
        }
        if (magic == 2 ? same_value_zero(J, v, argv[0]) : strict_equals(J, v, argv[0])) return magic == 2 ? JV_TRUE : jv_number(k);
    }
    return magic == 2 ? JV_FALSE : jv_from_int(-1);
}

static jv tp_join(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "join");
    if (!t) return JV_EXC;
    struct str* sep = 0;
    if (!jv_is_undef(argv[0])) { sep = to_str(J, argv[0]); if (!sep) return JV_EXC; }
    struct sbuf b;
    sb_init(J, &b);
    for (uint32_t k = 0; k < len; k++) {
        if (k) { if (sep) sb_put_str(&b, sep); else sb_putc(&b, ','); }
        jv v = obj_get(J, &t->base, PK_FROM_INDEX(k), this_v);
        if (v == JV_EXC) { sb_free(&b); return JV_EXC; }
        if (jv_is_undef(v)) continue;
        if (magic) {
            v = invoke(J, v, A(toLocaleString), 0, 0);
            if (v == JV_EXC) { sb_free(&b); return JV_EXC; }
        }
        struct str* s = to_str(J, v);
        if (!s) { sb_free(&b); return JV_EXC; }
        sb_put_str(&b, s);
    }
    return sb_done(&b);
}

static jv tp_to_locale_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv a[8] = { JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED, JV_UNDEFINED };
    return tp_join(J, this_v, 0, a, 1);
}

enum { TCB_EVERY, TCB_SOME, TCB_FOREACH, TCB_MAP, TCB_FILTER, TCB_FIND, TCB_FINDINDEX, TCB_FINDLAST, TCB_FINDLASTINDEX };

static jv tp_iterate(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "TypedArray method");
    if (!t) return JV_EXC;
    jv fn = argv[0];
    if (!is_callable(fn)) return throw_type(J, "%S is not a function", jv_str(typeof_value(J, fn)));
    jv out = JV_UNDEFINED;
    struct obj* kept = 0;
    if (magic == TCB_MAP) {
        jv n = jv_number(len);
        out = ta_species_create(J, t, 1, &n, len);
        if (out == JV_EXC) return JV_EXC;
    } else if (magic == TCB_FILTER) {
        kept = obj_new_array(J, 0);
        if (!kept) return JV_EXC;
    }
    int last = magic == TCB_FINDLAST || magic == TCB_FINDLASTINDEX;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t k = last ? len - 1 - i : i;
        jv v = obj_get(J, &t->base, PK_FROM_INDEX(k), this_v);
        if (v == JV_EXC) return JV_EXC;
        jv args[3] = { v, jv_number(k), this_v };
        jv r = ojs_call_v(J, fn, argv[1], 3, args);
        if (r == JV_EXC) return JV_EXC;
        switch (magic) {
        case TCB_EVERY: if (!to_boolean(r)) return JV_FALSE; break;
        case TCB_SOME: if (to_boolean(r)) return JV_TRUE; break;
        case TCB_MAP: if (obj_set(J, jv_obj(out), PK_FROM_INDEX(k), r, out, 1) < 0) return JV_EXC; break;
        case TCB_FILTER:
            if (to_boolean(r)) {
                if (obj_elems_reserve(J, kept, kept->elen + 1) < 0) return JV_EXC;
                kept->elems[kept->elen++] = v;
                kept->alen = kept->elen;
            }
            break;
        case TCB_FIND: case TCB_FINDLAST: if (to_boolean(r)) return v; break;
        case TCB_FINDINDEX: case TCB_FINDLASTINDEX: if (to_boolean(r)) return jv_number(k); break;
        default: break;
        }
    }
    switch (magic) {
    case TCB_EVERY: return JV_TRUE;
    case TCB_SOME: return JV_FALSE;
    case TCB_MAP: return out;
    case TCB_FILTER: {
        jv n = jv_number(kept->elen);
        jv a = ta_species_create(J, t, 1, &n, kept->elen);
        if (a == JV_EXC) return JV_EXC;
        for (uint32_t i = 0; i < kept->elen; i++)
            if (obj_set(J, jv_obj(a), PK_FROM_INDEX(i), kept->elems[i], a, 1) < 0) return JV_EXC;
        return a;
    }
    case TCB_FINDINDEX: case TCB_FINDLASTINDEX: return jv_from_int(-1);
    }
    return JV_UNDEFINED;
}

static jv tp_reduce(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, magic ? "reduceRight" : "reduce");
    if (!t) return JV_EXC;
    jv fn = argv[0];
    if (!is_callable(fn)) return throw_type(J, "%S is not a function", jv_str(typeof_value(J, fn)));
    uint32_t i = 0;
    jv acc;
    if (argc >= 2) acc = argv[1];
    else {
        if (!len) return throw_type(J, "Reduce of empty array with no initial value");
        acc = obj_get(J, &t->base, PK_FROM_INDEX(magic ? len - 1 : 0), this_v);
        if (acc == JV_EXC) return JV_EXC;
        i = 1;
    }
    for (; i < len; i++) {
        uint32_t k = magic ? len - 1 - i : i;
        jv v = obj_get(J, &t->base, PK_FROM_INDEX(k), this_v);
        if (v == JV_EXC) return JV_EXC;
        jv args[4] = { acc, v, jv_number(k), this_v };
        acc = ojs_call_v(J, fn, JV_UNDEFINED, 4, args);
        if (acc == JV_EXC) return JV_EXC;
    }
    return acc;
}

static jv tp_set(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct tarr* t = this_ta(J, this_v, "set");
    if (!t) return JV_EXC;
    double off;
    if (to_integer_or_inf(J, argv[1], &off) < 0) return JV_EXC;
    if (off < 0) return throw_range(J, "offset is out of bounds");
    uint32_t len;
    if (!ta_len(t, &len)) return throw_type(J, "set: typed array is detached or out of bounds");
    jv src = argv[0];
    if (jv_is_obj(src) && obj_class(jv_obj(src)) == OC_TYPEDARRAY) {
        struct tarr* s = (struct tarr*)jv_obj(src);
        uint32_t slen;
        if (!ta_len(s, &slen)) return throw_type(J, "set: source typed array is detached or out of bounds");
        if (is_bigint_type(s->type) != is_bigint_type(t->type)) return throw_type(J, "Content type mismatch");
        if (off == 1.0 / 0.0 || slen + off > len) return throw_range(J, "offset is out of bounds");
        if (s->type == t->type) {
            memmove(ta_ptr(t, (uint32_t)off), ta_ptr(s, 0), (size_t)slen * TSIZE[t->type]);
            return JV_UNDEFINED;
        }
        // overlapping buffers: copy the source bytes first
        size_t nb = (size_t)slen * TSIZE[s->type];
        uint8_t* tmp = (uint8_t*)ojs_sys_malloc(nb ? nb : 1);
        if (!tmp) return throw_oom(J);
        memcpy(tmp, ta_ptr(s, 0), nb);
        for (uint32_t i = 0; i < slen; i++) {
            jv v = read_elem(J, s->type, tmp + (size_t)i * TSIZE[s->type]);
            if (v == JV_EXC) { ojs_sys_free(tmp); return JV_EXC; }
            double d = 0;
            uint64_t big = 0;
            if (conv_value(J, t->type, v, &d, &big) < 0) { ojs_sys_free(tmp); return JV_EXC; }
            write_conv(t->type, ta_ptr(t, (uint32_t)off + i), d, big);
        }
        ojs_sys_free(tmp);
        return JV_UNDEFINED;
    }
    jv so = to_object(J, src);
    if (so == JV_EXC) return JV_EXC;
    int64_t slen;
    if (length_of_array_like(J, jv_obj(so), &slen) < 0) return JV_EXC;
    if (off == 1.0 / 0.0 || slen + off > len) return throw_range(J, "offset is out of bounds");
    for (int64_t i = 0; i < slen; i++) {
        jv v = obj_get(J, jv_obj(so), PK_FROM_INDEX((uint32_t)i), so);
        if (v == JV_EXC) return JV_EXC;
        if (ta_write(J, t, off + (double)i, v) < 0) return JV_EXC;
    }
    return JV_UNDEFINED;
}

static jv tp_slice(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "slice");
    if (!t) return JV_EXC;
    double k, fin;
    if (rel(J, argv[0], len, 0, &k) < 0 || rel(J, argv[1], len, len, &fin) < 0) return JV_EXC;
    double count = fin - k > 0 ? fin - k : 0;
    jv n = jv_number(count);
    jv av = ta_species_create(J, t, 1, &n, count);
    if (av == JV_EXC) return JV_EXC;
    if (count > 0) {
        if (!ta_len(t, &len)) return throw_type(J, "slice: typed array is detached or out of bounds");
        if (fin > len) fin = len;
        count = fin - k > 0 ? fin - k : 0;
        struct tarr* a = (struct tarr*)jv_obj(av);
        if (a->type == t->type) {
            uint32_t sz = TSIZE[t->type];
            // byte-wise copy (the target may share the buffer)
            uint8_t* src = ta_ptr(t, (uint32_t)k);
            uint8_t* dst = ta_ptr(a, 0);
            for (size_t i = 0; i < (size_t)count * sz; i++) dst[i] = src[i];
        } else {
            for (uint32_t n2 = 0; k < fin; k++, n2++) {
                jv v = obj_get(J, &t->base, PK_FROM_INDEX((uint32_t)k), this_v);
                if (v == JV_EXC) return JV_EXC;
                if (obj_set(J, &a->base, PK_FROM_INDEX(n2), v, av, 1) < 0) return JV_EXC;
            }
        }
    }
    return av;
}

static jv tp_subarray(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct tarr* t = this_ta(J, this_v, "subarray");
    if (!t) return JV_EXC;
    uint32_t len;
    ta_len(t, &len);
    double b, e;
    if (rel(J, argv[0], len, 0, &b) < 0) return JV_EXC;
    int tracking = t->tracking && jv_is_undef(argv[1]);
    if (!tracking && rel(J, argv[1], len, len, &e) < 0) return JV_EXC;
    jv args[3];
    args[0] = jv_from_obj(&t->buf->base);
    args[1] = jv_number((double)t->offset + b * TSIZE[t->type]);
    int n = 2;
    if (!tracking) { args[2] = jv_number(e - b > 0 ? e - b : 0); n = 3; }
    jv dflt = jv_from_obj(J->I.ta_ctor[t->type]);
    jv c = species_constructor(J, &t->base, dflt);
    if (c == JV_EXC) return JV_EXC;
    jv r = ta_create_from(J, c, n, args, 0);
    if (r == JV_EXC) return JV_EXC;
    if (is_bigint_type(((struct tarr*)jv_obj(r))->type) != is_bigint_type(t->type)) return throw_type(J, "Content type mismatch");
    return r;
}

// numeric sort for typed arrays (default comparator): stable merge on values
static int ta_cmp_default(ojs* J, jv x, jv y) {
    if (jv_is_big(x)) {
        int bigint_cmp(jv a, jv b);
        return bigint_cmp(x, y);
    }
    double a = jv_num(x), b = jv_num(y);
    if (a != a) return b != b ? 0 : 1;
    if (b != b) return -1;
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == 0 && b == 0) {
        int sa = 1 / a < 0, sb = 1 / b < 0;
        return sa == sb ? 0 : sa ? -1 : 1;
    }
    return 0;
}

static int ta_sort_values(ojs* J, jv* v, jv* tmp, uint32_t n, jv cmp) {
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int c;
                if (jv_is_undef(cmp)) c = ta_cmp_default(J, v[i], v[j]);
                else {
                    jv args[2] = { v[i], v[j] };
                    jv r = ojs_call_v(J, cmp, JV_UNDEFINED, 2, args);
                    if (r == JV_EXC) return -1;
                    double d;
                    if (to_number_d(J, r, &d) < 0) return -1;
                    c = d > 0 ? 1 : d < 0 ? -1 : 0;
                }
                tmp[k++] = c <= 0 ? v[i++] : v[j++];
            }
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
            memcpy(v + lo, tmp + lo, (size_t)(hi - lo) * sizeof(jv));
        }
    }
    return 0;
}

static jv tp_sort(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv cmp = argv[0];
    if (!jv_is_undef(cmp) && !is_callable(cmp)) return throw_type(J, "The comparison function must be either a function or undefined");
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, magic ? "toSorted" : "sort");
    if (!t) return JV_EXC;
    struct tarr* dst = t;
    jv result = this_v;
    if (magic) {
        result = ta_create_len(J, ta_proto_of(J, t->type), t->type, len);
        if (result == JV_EXC) return JV_EXC;
        dst = (struct tarr*)jv_obj(result);
    }
    jv* v = valarr_new(J, len ? len : 1);
    jv* tmp = valarr_new(J, len ? len : 1);
    if (!v || !tmp) return JV_EXC;
    struct obj* hold = obj_new_array(J, 0);   // keeps v reachable
    if (!hold) return JV_EXC;
    hold->elems = v;
    for (uint32_t i = 0; i < len; i++) {
        v[i] = read_elem(J, t->type, ta_ptr(t, i));
        if (v[i] == JV_EXC) return JV_EXC;
    }
    if (ta_sort_values(J, v, tmp, len, cmp) < 0) return JV_EXC;
    uint32_t cur;
    if (!ta_len(dst, &cur)) return result;
    for (uint32_t i = 0; i < len && i < cur; i++) {
        double d = 0;
        uint64_t big = 0;
        if (conv_value(J, dst->type, v[i], &d, &big) < 0) return JV_EXC;
        write_conv(dst->type, ta_ptr(dst, i), d, big);
    }
    return result;
}

static jv tp_to_reversed(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "toReversed");
    if (!t) return JV_EXC;
    jv r = ta_create_len(J, ta_proto_of(J, t->type), t->type, len);
    if (r == JV_EXC) return JV_EXC;
    struct tarr* a = (struct tarr*)jv_obj(r);
    uint32_t sz = TSIZE[t->type];
    for (uint32_t i = 0; i < len; i++) memcpy(ta_ptr(a, i), ta_ptr(t, len - 1 - i), sz);
    return r;
}

static jv tp_with(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = valid_ta(J, this_v, &len, "with");
    if (!t) return JV_EXC;
    double idx;
    if (to_integer_or_inf(J, argv[0], &idx) < 0) return JV_EXC;
    if (idx < 0) idx += len;
    double d = 0;
    uint64_t big = 0;
    if (conv_value(J, t->type, argv[1], &d, &big) < 0) return JV_EXC;
    uint32_t cur;
    if (!ta_len(t, &cur) || idx < 0 || idx >= cur) return throw_range(J, "Invalid typed array index");
    jv r = ta_create_len(J, ta_proto_of(J, t->type), t->type, len);
    if (r == JV_EXC) return JV_EXC;
    struct tarr* a = (struct tarr*)jv_obj(r);
    for (uint32_t i = 0; i < len; i++) {
        if (i == (uint32_t)idx) write_conv(a->type, ta_ptr(a, i), d, big);
        else if (i < cur) memcpy(ta_ptr(a, i), ta_ptr(t, i), TSIZE[t->type]);
    }
    return r;
}

// ---------------------------------------------------------------- DataView

struct dview {
    struct obj base;
    struct abuf* buf;
    uint32_t offset;
    uint32_t length;
    int tracking;
};

static void dview_trace(ojs* J, struct obj* o) { gc_mark_ptr(J, ((struct dview*)o)->buf); }

static int dv_len(struct dview* v, uint32_t* len) {
    struct abuf* b = v->buf;
    if (b->detached || v->offset > b->len) { *len = 0; return 0; }
    if (v->tracking) { *len = b->len - v->offset; return 1; }
    if ((uint64_t)v->offset + v->length > b->len) { *len = 0; return 0; }
    *len = v->length;
    return 1;
}

static jv dataview_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return throw_type(J, "Constructor DataView requires 'new'");
    jv bv = argv[0];
    if (!jv_is_obj(bv) || (obj_class(jv_obj(bv)) != OC_ARRAYBUFFER && obj_class(jv_obj(bv)) != OC_SHAREDARRAYBUFFER))
        return throw_type(J, "First argument to DataView constructor must be an ArrayBuffer");
    struct abuf* b = (struct abuf*)jv_obj(bv);
    uint64_t off;
    if (to_index(J, argv[1], &off) < 0) return JV_EXC;
    if (b->detached) return throw_type(J, "ArrayBuffer is detached");
    if (off > b->len) return throw_range(J, "Start offset %d is outside the bounds of the buffer", (int)off);
    int tracking = 0;
    uint64_t vlen = 0;
    if (jv_is_undef(argv[2])) {
        if (b->resizable) tracking = 1;
        else vlen = b->len - off;
    } else {
        if (to_index(J, argv[2], &vlen) < 0) return JV_EXC;
        if (off + vlen > b->len) return throw_range(J, "Invalid DataView length %d", (int)vlen);
    }
    struct obj* proto = get_proto_from_ctor(J, nt, J->I.dataview_proto);
    if (!proto) return JV_EXC;
    if (b->detached) return throw_type(J, "ArrayBuffer is detached");
    if (off > b->len || (!tracking && off + vlen > b->len)) return throw_range(J, "Invalid DataView length");
    struct dview* v = (struct dview*)obj_new(J, proto, OC_DATAVIEW, sizeof(struct dview));
    if (!v) return JV_EXC;
    v->buf = b;
    v->offset = (uint32_t)off;
    v->length = (uint32_t)vlen;
    v->tracking = tracking;
    return jv_from_obj(&v->base);
}

static struct dview* this_dv(ojs* J, jv t, const char* m) {
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == OC_DATAVIEW) return (struct dview*)jv_obj(t);
    throw_type(J, "DataView.prototype.%s called on incompatible receiver", m);
    return 0;
}

// getters: 0 buffer, 1 byteLength, 2 byteOffset
static jv dv_getter(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct dview* v = this_dv(J, this_v, "getter");
    if (!v) return JV_EXC;
    if (magic == 0) return jv_from_obj(&v->buf->base);
    uint32_t len;
    if (!dv_len(v, &len)) return throw_type(J, "DataView is detached or out of bounds");
    return jv_number(magic == 1 ? len : v->offset);
}

static void swap_bytes(uint8_t* p, int n) {
    for (int i = 0; i < n / 2; i++) { uint8_t t = p[i]; p[i] = p[n - 1 - i]; p[n - 1 - i] = t; }
}

// get/set: magic = element type | 0x100 for set
static jv dv_access(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    int set = magic & 0x100, type = magic & 0xFF;
    struct dview* v = this_dv(J, this_v, set ? "set" : "get");
    if (!v) return JV_EXC;
    uint64_t idx;
    if (to_index(J, argv[0], &idx) < 0) return JV_EXC;
    double d = 0;
    uint64_t big = 0;
    if (set && conv_value(J, type, argv[1], &d, &big) < 0) return JV_EXC;
    int little = to_boolean(argv[set ? 2 : 1]);
    uint32_t len;
    if (!dv_len(v, &len)) return throw_type(J, "DataView is detached or out of bounds");
    int sz = TSIZE[type];
    if (idx + (uint64_t)sz > len) return throw_range(J, "Offset is outside the bounds of the DataView");
    uint8_t* p = v->buf->data + v->offset + (uint32_t)idx;
    uint8_t tmp[8];
    if (set) {
        write_conv(type, tmp, d, big);
        if (!little) swap_bytes(tmp, sz);
        memcpy(p, tmp, (size_t)sz);
        return JV_UNDEFINED;
    }
    memcpy(tmp, p, (size_t)sz);
    if (!little) swap_bytes(tmp, sz);
    return read_elem(J, type, tmp);
}

// ---------------------------------------------------------------- Atomics

static struct tarr* atomic_ta(ojs* J, jv v, int waitable, uint32_t* len) {
    struct tarr* t = valid_ta(J, v, len, "Atomics");
    if (!t) return 0;
    int ty = t->type;
    int ok = waitable ? (ty == T_I32 || ty == T_BI64) :
             (ty == T_I8 || ty == T_U8 || ty == T_I16 || ty == T_U16 || ty == T_I32 || ty == T_U32 || ty == T_BI64 || ty == T_BU64);
    if (!ok) { throw_type(J, "Atomics: invalid typed array type"); return 0; }
    return t;
}

enum { AT_ADD, AT_AND, AT_EXCHANGE, AT_OR, AT_SUB, AT_XOR, AT_LOAD, AT_STORE, AT_CAS };

static jv atomics_op(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = atomic_ta(J, argv[0], 0, &len);
    if (!t) return JV_EXC;
    uint64_t idx;
    if (to_index(J, argv[1], &idx) < 0) return JV_EXC;
    if (idx >= len) return throw_range(J, "Invalid atomic access index");
    int big = is_bigint_type(t->type);
    double dv = 0, ev = 0;
    uint64_t bv = 0, be = 0;
    jv stored = JV_UNDEFINED;
    if (magic == AT_CAS) {
        if (conv_value(J, t->type, argv[2], &ev, &be) < 0) return JV_EXC;
        if (conv_value(J, t->type, argv[3], &dv, &bv) < 0) return JV_EXC;
    } else if (magic != AT_LOAD) {
        if (big) {
            if (to_bigint64(J, argv[2], &bv) < 0) return JV_EXC;
            if (magic == AT_STORE) { stored = to_bigint(J, argv[2]); if (stored == JV_EXC) return JV_EXC; }
        } else {
            if (to_integer_or_inf(J, argv[2], &dv) < 0) return JV_EXC;
            if (dv == 0) dv = 0;   // -0 -> +0
            stored = jv_number(dv);
        }
    }
    if (!ta_len(t, &len) || idx >= len) return throw_type(J, "TypedArray is detached or out of bounds");
    uint8_t* p = ta_ptr(t, (uint32_t)idx);
    jv old = read_elem(J, t->type, p);
    if (old == JV_EXC) return JV_EXC;
    if (magic == AT_LOAD) return old;
    if (magic == AT_STORE) { write_conv(t->type, p, dv, bv); return stored; }
    if (magic == AT_CAS) {
        uint8_t tmp[8];
        write_conv(t->type, tmp, ev, be);
        if (!memcmp(tmp, p, TSIZE[t->type])) write_conv(t->type, p, dv, bv);
        return old;
    }
    // integer arithmetic on the raw element
    uint64_t a = 0;
    memcpy(&a, p, TSIZE[t->type]);
    uint64_t b;
    if (big) b = bv;
    else {
        uint8_t tmp[8] = { 0 };
        write_conv(t->type, tmp, dv, 0);
        b = 0;
        memcpy(&b, tmp, TSIZE[t->type]);
    }
    uint64_t r;
    switch (magic) {
    case AT_ADD: r = a + b; break;
    case AT_AND: r = a & b; break;
    case AT_EXCHANGE: r = b; break;
    case AT_OR: r = a | b; break;
    case AT_SUB: r = a - b; break;
    default: r = a ^ b; break;
    }
    memcpy(p, &r, TSIZE[t->type]);
    return old;
}

static jv atomics_is_lock_free(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double n;
    if (to_integer_or_inf(J, argv[0], &n) < 0) return JV_EXC;
    return jv_bool(n == 1 || n == 2 || n == 4 || n == 8);
}

static jv atomics_wait(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = atomic_ta(J, argv[0], 1, &len);
    if (!t) return JV_EXC;
    if (!t->buf->shared) return throw_type(J, "Atomics.wait requires a shared typed array");
    uint64_t idx;
    if (to_index(J, argv[1], &idx) < 0) return JV_EXC;
    if (idx >= len) return throw_range(J, "Invalid atomic access index");
    double d = 0;
    uint64_t big = 0;
    if (conv_value(J, t->type, argv[2], &d, &big) < 0) return JV_EXC;
    double timeout;
    if (to_number_d(J, argv[3], &timeout) < 0) return JV_EXC;
    uint8_t tmp[8];
    write_conv(t->type, tmp, d, big);
    if (memcmp(tmp, ta_ptr(t, (uint32_t)idx), TSIZE[t->type])) return str_value(J, "not-equal");
    // a single agent: nobody can notify us
    return str_value(J, "timed-out");
}

static jv atomics_notify(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    uint32_t len;
    struct tarr* t = atomic_ta(J, argv[0], 1, &len);
    if (!t) return JV_EXC;
    uint64_t idx;
    if (to_index(J, argv[1], &idx) < 0) return JV_EXC;
    if (!jv_is_undef(argv[2])) {
        double c;
        if (to_integer_or_inf(J, argv[2], &c) < 0) return JV_EXC;
    }
    if (idx >= len) return throw_range(J, "Invalid atomic access index");
    return jv_from_int(0);
}

static jv atomics_pause(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv n = argv[0];
    if (!jv_is_undef(n)) {
        if (!jv_is_number(n)) return throw_type(J, "Atomics.pause: argument must be an integral number");
        double d = jv_num(n);
        if (d != km_trunc(d) || d != d) return throw_type(J, "Atomics.pause: argument must be an integral number");
    }
    return JV_UNDEFINED;
}

// ---------------------------------------------------------------- init

static const struct class_ops ta_ops = {
    .get_own = ta_get_own, .define_own = ta_define_own, .has = ta_has, .get = ta_get, .set = ta_set,
    .del = ta_del, .own_keys = ta_own_keys, .trace = tarr_trace,
};

void b_typed_classes(void) {
    class_ops[OC_ARRAYBUFFER].finalize = abuf_finalize;
    class_ops[OC_SHAREDARRAYBUFFER].finalize = abuf_finalize;
    class_ops[OC_TYPEDARRAY] = ta_ops;
    class_ops[OC_DATAVIEW].trace = dview_trace;
}

static const struct bdef ab_proto_fns[] = {
    GETTER("byteLength", abuf_getter, 0),
    GETTER("maxByteLength", abuf_getter, 1),
    GETTER("resizable", abuf_getter, 2),
    GETTER("detached", abuf_getter, 3),
    FN("resize", abuf_resize, 1, 0),
    FN("slice", abuf_slice, 2, 0),
    FN("transfer", abuf_transfer, 0, 0),
    FN("transferToFixedLength", abuf_transfer, 0, 1),
};

static const struct bdef sab_proto_fns[] = {
    GETTER("byteLength", abuf_getter, 0x10),
    GETTER("maxByteLength", abuf_getter, 0x11),
    GETTER("growable", abuf_getter, 0x12),
    FN("grow", abuf_resize, 1, 1),
    FN("slice", abuf_slice, 2, 1),
};

static const struct bdef ta_statics[] = {
    FN("from", ta_from, 1, 0),
    FN("of", ta_of, 0, 0),
    GETTER("@@species", ta_species, 0),
};

static const struct bdef ta_proto_fns[] = {
    GETTER("buffer", ta_getter, 0),
    GETTER("byteLength", ta_getter, 1),
    GETTER("byteOffset", ta_getter, 2),
    GETTER("length", ta_getter, 3),
    GETTER("@@toStringTag", ta_getter, 4),
    FN("at", tp_at, 1, 0),
    FN("copyWithin", tp_copy_within, 2, 0),
    FN("entries", tp_iter, 0, 2),
    FN("every", tp_iterate, 1, TCB_EVERY),
    FN("fill", tp_fill, 1, 0),
    FN("filter", tp_iterate, 1, TCB_FILTER),
    FN("find", tp_iterate, 1, TCB_FIND),
    FN("findIndex", tp_iterate, 1, TCB_FINDINDEX),
    FN("findLast", tp_iterate, 1, TCB_FINDLAST),
    FN("findLastIndex", tp_iterate, 1, TCB_FINDLASTINDEX),
    FN("forEach", tp_iterate, 1, TCB_FOREACH),
    FN("includes", tp_index_of, 1, 2),
    FN("indexOf", tp_index_of, 1, 0),
    FN("join", tp_join, 1, 0),
    FN("keys", tp_iter, 0, 0),
    FN("lastIndexOf", tp_index_of, 1, 1),
    FN("map", tp_iterate, 1, TCB_MAP),
    FN("reduce", tp_reduce, 1, 0),
    FN("reduceRight", tp_reduce, 1, 1),
    FN("reverse", tp_reverse, 0, 0),
    FN("set", tp_set, 1, 0),
    FN("slice", tp_slice, 2, 0),
    FN("some", tp_iterate, 1, TCB_SOME),
    FN("sort", tp_sort, 1, 0),
    FN("subarray", tp_subarray, 2, 0),
    FN("toLocaleString", tp_to_locale_string, 0, 0),
    FN("toReversed", tp_to_reversed, 0, 0),
    FN("toSorted", tp_sort, 1, 1),
    FN("with", tp_with, 2, 0),
};

static const struct bdef dv_proto_fns[] = {
    GETTER("buffer", dv_getter, 0),
    GETTER("byteLength", dv_getter, 1),
    GETTER("byteOffset", dv_getter, 2),
    FN("getInt8", dv_access, 1, T_I8), FN("getUint8", dv_access, 1, T_U8),
    FN("getInt16", dv_access, 1, T_I16), FN("getUint16", dv_access, 1, T_U16),
    FN("getInt32", dv_access, 1, T_I32), FN("getUint32", dv_access, 1, T_U32),
    FN("getFloat16", dv_access, 1, T_F16), FN("getFloat32", dv_access, 1, T_F32), FN("getFloat64", dv_access, 1, T_F64),
    FN("getBigInt64", dv_access, 1, T_BI64), FN("getBigUint64", dv_access, 1, T_BU64),
    FN("setInt8", dv_access, 2, 0x100 | T_I8), FN("setUint8", dv_access, 2, 0x100 | T_U8),
    FN("setInt16", dv_access, 2, 0x100 | T_I16), FN("setUint16", dv_access, 2, 0x100 | T_U16),
    FN("setInt32", dv_access, 2, 0x100 | T_I32), FN("setUint32", dv_access, 2, 0x100 | T_U32),
    FN("setFloat16", dv_access, 2, 0x100 | T_F16), FN("setFloat32", dv_access, 2, 0x100 | T_F32),
    FN("setFloat64", dv_access, 2, 0x100 | T_F64),
    FN("setBigInt64", dv_access, 2, 0x100 | T_BI64), FN("setBigUint64", dv_access, 2, 0x100 | T_BU64),
};

static const struct bdef atomics_fns[] = {
    FN("add", atomics_op, 3, AT_ADD), FN("and", atomics_op, 3, AT_AND),
    FN("compareExchange", atomics_op, 4, AT_CAS), FN("exchange", atomics_op, 3, AT_EXCHANGE),
    FN("isLockFree", atomics_is_lock_free, 1, 0), FN("load", atomics_op, 2, AT_LOAD),
    FN("notify", atomics_notify, 3, 0), FN("or", atomics_op, 3, AT_OR), FN("pause", atomics_pause, 0, 0),
    FN("store", atomics_op, 3, AT_STORE), FN("sub", atomics_op, 3, AT_SUB), FN("wait", atomics_wait, 4, 0),
    FN("xor", atomics_op, 3, AT_XOR),
};

int b_typed_init(ojs* J) {
    struct intrinsics* I = &J->I;
    // ArrayBuffer
    I->arraybuffer_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->arraybuffer_proto) return -1;
    struct obj* abc = def_ctor(J, abuf_ctor, "ArrayBuffer", 1, 0, I->arraybuffer_proto);
    if (!abc) return -1;
    I->arraybuffer_ctor = abc;
    if (DEF_FNS(I->arraybuffer_proto, ab_proto_fns) < 0) return -1;
    struct bdef ab_st[] = { FN("isView", abuf_is_view, 1, 0), GETTER("@@species", abuf_species, 0) };
    if (def_fns(J, abc, ab_st, 2) < 0) return -1;
    if (def_value(J, I->arraybuffer_proto, "@@toStringTag", str_value(J, "ArrayBuffer"), PA_CONFIGURABLE) < 0) return -1;
    // SharedArrayBuffer
    I->sharedarraybuffer_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->sharedarraybuffer_proto) return -1;
    struct obj* sabc = def_ctor(J, abuf_ctor, "SharedArrayBuffer", 1, 1, I->sharedarraybuffer_proto);
    if (!sabc) return -1;
    I->sharedarraybuffer_ctor = sabc;
    if (DEF_FNS(I->sharedarraybuffer_proto, sab_proto_fns) < 0) return -1;
    if (def_fns(J, sabc, &ab_st[1], 1) < 0) return -1;
    if (def_value(J, I->sharedarraybuffer_proto, "@@toStringTag", str_value(J, "SharedArrayBuffer"), PA_CONFIGURABLE) < 0) return -1;
    // %TypedArray%
    I->typedarray_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->typedarray_proto) return -1;
    struct obj* tac = new_native_ctor(J, ta_abstract_ctor, "TypedArray", 0, 0, I->typedarray_proto);
    if (!tac) return -1;
    I->typedarray_ctor = tac;
    if (DEF_FNS(tac, ta_statics) < 0 || DEF_FNS(I->typedarray_proto, ta_proto_fns) < 0) return -1;
    struct obj* values = new_native(J, tp_iter, "values", 0, 1);
    if (!values) return -1;
    if (obj_define_value(J, I->typedarray_proto, A(values), jv_from_obj(values), PA_HIDDEN) < 0) return -1;
    if (obj_define_value(J, I->typedarray_proto, WK(WK_ITERATOR), jv_from_obj(values), PA_HIDDEN) < 0) return -1;
    if (obj_define_value(J, I->typedarray_proto, A(toString), jv_from_obj(I->array_proto_to_string), PA_HIDDEN) < 0) return -1;
    for (int t = 0; t < T_COUNT; t++) {
        struct obj* p = obj_new(J, I->typedarray_proto, OC_OBJECT, 0);
        if (!p) return -1;
        struct obj* c = def_ctor(J, ta_ctor, TNAME[t], 3, t, p);
        if (!c) return -1;
        c->proto = tac;
        I->ta_proto[t] = p;
        I->ta_ctor[t] = c;
        jv bpe = jv_from_int(TSIZE[t]);
        if (def_value(J, c, "BYTES_PER_ELEMENT", bpe, 0) < 0 || def_value(J, p, "BYTES_PER_ELEMENT", bpe, 0) < 0) return -1;
    }
    // DataView
    I->dataview_proto = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!I->dataview_proto) return -1;
    struct obj* dvc = def_ctor(J, dataview_ctor, "DataView", 1, 0, I->dataview_proto);
    if (!dvc) return -1;
    I->dataview_ctor = dvc;
    if (DEF_FNS(I->dataview_proto, dv_proto_fns) < 0) return -1;
    if (def_value(J, I->dataview_proto, "@@toStringTag", str_value(J, "DataView"), PA_CONFIGURABLE) < 0) return -1;
    // Atomics
    struct obj* at = obj_new(J, I->object_proto, OC_OBJECT, 0);
    if (!at || DEF_FNS(at, atomics_fns) < 0) return -1;
    if (def_value(J, at, "@@toStringTag", str_value(J, "Atomics"), PA_CONFIGURABLE) < 0) return -1;
    return def_global(J, "Atomics", jv_from_obj(at));
}

// ---------------------------------------------------------------- embedding helpers

uint8_t* typed_bytes(jv v, size_t* len) {
    if (!jv_is_obj(v)) return 0;
    struct obj* o = jv_obj(v);
    switch (obj_class(o)) {
    case OC_ARRAYBUFFER: case OC_SHAREDARRAYBUFFER: {
        struct abuf* b = (struct abuf*)o;
        if (b->detached) return 0;
        *len = b->len;
        return b->data;
    }
    case OC_TYPEDARRAY: {
        struct tarr* t = (struct tarr*)o;
        uint32_t n;
        if (!ta_len(t, &n)) return 0;
        *len = (size_t)n * TSIZE[t->type];
        return ta_ptr(t, 0);
    }
    case OC_DATAVIEW: {
        struct dview* d = (struct dview*)o;
        uint32_t n;
        if (!dv_len(d, &n)) return 0;
        *len = n;
        return d->buf->data + d->offset;
    }
    }
    return 0;
}

jv arraybuffer_copy(ojs* J, const void* data, size_t len) {
    if (len > MAX_BYTES) return throw_range(J, "Array buffer allocation failed");
    struct abuf* b = abuf_new(J, J->I.arraybuffer_proto, (uint32_t)len, 0, 0, 0);
    if (!b) return JV_EXC;
    if (len) memcpy(b->data, data, len);
    return jv_from_obj(&b->base);
}

jv uint8array_copy(ojs* J, const void* data, size_t len) {
    jv r = ta_create_len(J, J->I.ta_proto[T_U8], T_U8, (double)len);
    if (r == JV_EXC) return JV_EXC;
    if (len) memcpy(ta_ptr((struct tarr*)jv_obj(r), 0), data, len);
    return r;
}
