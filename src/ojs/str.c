// str.c — JS strings: flat Latin-1 / UTF-16 strings, ropes, the atom
// (intern) table, the string builder and UTF-8 conversion.

#include "ojs_int.h"
#include "gc_int.h"
#include "atoms.h"

// ---------------------------------------------------------------- allocation

struct str* str_alloc(ojs* J, uint32_t len, int wide) {
    if (len > (1u << 29)) { throw_range(J, "Invalid string length"); return 0; }
    size_t bytes = offsetof(struct str, u) + (size_t)len * (wide ? 2 : 1) + 2;
    struct str* s = (struct str*)gc_alloc(J, GT_STR, bytes);
    if (!s) return 0;
    s->h.aux = wide ? SF_WIDE : 0;
    s->h.aux32 = len;
    return s;
}

struct str* str_new8(ojs* J, const uint8_t* p, uint32_t len) {
    struct str* s = str_alloc(J, len, 0);
    if (s && len) memcpy(s->u.c8, p, len);
    return s;
}

struct str* str_new16(ojs* J, const uint16_t* p, uint32_t len) {
    int wide = 0;
    for (uint32_t i = 0; i < len; i++) if (p[i] > 0xFF) { wide = 1; break; }
    struct str* s = str_alloc(J, len, wide);
    if (!s) return 0;
    if (wide) memcpy(s->u.c16, p, (size_t)len * 2);
    else for (uint32_t i = 0; i < len; i++) s->u.c8[i] = (uint8_t)p[i];
    return s;
}

struct str* str_from_cstr(ojs* J, const char* s) { return str_from_utf8(J, s, strlen(s)); }

// UTF-8 (lenient: WTF-8 surrogates accepted, malformed bytes -> U+FFFD)
static uint32_t utf8_next(const uint8_t* s, size_t n, size_t* i) {
    uint8_t c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    int k; uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; min = 0x10000; }
    else { (*i)++; return 0xFFFD; }
    for (int j = 1; j <= k; j++) {
        if (*i + (size_t)j >= n || (s[*i + j] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (s[*i + j] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF) { (*i)++; return 0xFFFD; }
    *i += (size_t)k + 1;
    return cp;
}

struct str* str_from_utf8(ojs* J, const char* src, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    // pass 1: length in UTF-16 units and width
    uint32_t len = 0;
    int wide = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp = utf8_next(s, n, &i);
        if (cp > 0xFF) wide = 1;
        len += cp >= 0x10000 ? 2 : 1;
    }
    struct str* r = str_alloc(J, len, wide);
    if (!r) return 0;
    uint32_t k = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp = utf8_next(s, n, &i);
        if (!wide) r->u.c8[k++] = (uint8_t)cp;
        else if (cp >= 0x10000) {
            cp -= 0x10000;
            r->u.c16[k++] = (uint16_t)(0xD800 + (cp >> 10));
            r->u.c16[k++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else r->u.c16[k++] = (uint16_t)cp;
    }
    return r;
}

// UTF-16 -> UTF-8; lone surrogates become U+FFFD
static size_t put_utf8(uint32_t cp, char* o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static uint32_t cp_at(const struct str* s, uint32_t* i) {
    uint32_t n = str_len(s), c = str_at(s, *i);
    (*i)++;
    if (c >= 0xD800 && c <= 0xDBFF && *i < n) {
        uint32_t d = str_at(s, *i);
        if (d >= 0xDC00 && d <= 0xDFFF) { (*i)++; return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); }
    }
    if (c >= 0xD800 && c <= 0xDFFF) return 0xFFFD;
    return c;
}

size_t str_utf8_len(const struct str* s) {
    size_t n = 0;
    char tmp[4];
    for (uint32_t i = 0; i < str_len(s);) n += put_utf8(cp_at(s, &i), tmp);
    return n;
}

size_t str_to_utf8_buf(const struct str* s, char* out, size_t cap) {
    size_t n = 0;
    char tmp[4];
    for (uint32_t i = 0; i < str_len(s);) {
        size_t k = put_utf8(cp_at(s, &i), tmp);
        if (out && n + k < cap) memcpy(out + n, tmp, k);
        n += k;
    }
    if (out && cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

char* str_to_utf8(ojs* J, const struct str* s, size_t* len) {
    size_t n = str_utf8_len(s);
    char* o = (char*)ojs_sys_malloc(n + 1);
    if (!o) { throw_oom(J); return 0; }
    str_to_utf8_buf(s, o, n + 1);
    if (len) *len = n;
    return o;
}

// ---------------------------------------------------------------- ropes

#define ROPE_MIN 256u          // shorter concatenations are copied flat
#define ROPE_MAX_DEPTH 2000u

static uint32_t rope_depth(jv v) {
    struct gch* g = (struct gch*)JV_PTR(v);
    return g->type == GT_ROPE ? ((struct rope*)g)->depth : 0;
}

static void copy_units(ojs* J, jv v, struct str* dst, uint32_t* at);

struct str* str_flat(ojs* J, jv v) {
    struct gch* g = (struct gch*)JV_PTR(v);
    if (g->type == GT_STR) return (struct str*)g;
    struct rope* r = (struct rope*)g;
    if (r->flat) return r->flat;
    // wide if any leaf is wide
    int wide = 0;
    {
        // iterative leaf scan with an explicit stack
        jv stack[64]; int sp = 0;
        jv* big = 0; int bigcap = 0;
        jv* st = stack; int cap = 64;
        st[sp++] = v;
        while (sp && !wide) {
            jv x = st[--sp];
            struct gch* h = (struct gch*)JV_PTR(x);
            if (h->type == GT_STR) { if (h->aux & SF_WIDE) wide = 1; continue; }
            struct rope* q = (struct rope*)h;
            if (q->flat) { if (q->flat->h.aux & SF_WIDE) wide = 1; continue; }
            if (sp + 2 > cap) {
                int nc = cap * 2;
                jv* t = (jv*)ojs_sys_realloc(big, (size_t)nc * sizeof(jv));
                if (!t) { ojs_sys_free(big); throw_oom(J); return 0; }
                if (!big) memcpy(t, stack, (size_t)sp * sizeof(jv));
                big = t; bigcap = nc; st = big; cap = nc;
            }
            st[sp++] = q->right;
            st[sp++] = q->left;
        }
        (void)bigcap;
        ojs_sys_free(big);
    }
    struct str* s = str_alloc(J, r->h.aux32, wide);
    if (!s) return 0;
    uint32_t at = 0;
    copy_units(J, v, s, &at);
    r->flat = s;
    // drop the halves: the rope is now an indirection to the flat string
    r->left = r->right = jv_from_str(s);
    r->depth = 0;
    return s;
}

static void copy_flat(const struct str* f, struct str* dst, uint32_t* at) {
    uint32_t n = str_len(f);
    if (str_wide(dst)) {
        if (str_wide(f)) memcpy(dst->u.c16 + *at, f->u.c16, (size_t)n * 2);
        else for (uint32_t i = 0; i < n; i++) dst->u.c16[*at + i] = f->u.c8[i];
    } else memcpy(dst->u.c8 + *at, f->u.c8, n);
    *at += n;
}

static void copy_units(ojs* J, jv v, struct str* dst, uint32_t* at) {
    // left spine iteratively, right children recursively-by-stack
    jv stack[64]; int sp = 0;
    jv* st = stack; int cap = 64; jv* big = 0;
    st[sp++] = v;
    while (sp) {
        jv x = st[--sp];
        struct gch* h = (struct gch*)JV_PTR(x);
        if (h->type == GT_STR) { copy_flat((struct str*)h, dst, at); continue; }
        struct rope* q = (struct rope*)h;
        if (q->flat) { copy_flat(q->flat, dst, at); continue; }
        if (sp + 2 > cap) {
            int nc = cap * 2;
            jv* t = (jv*)ojs_sys_realloc(big, (size_t)nc * sizeof(jv));
            if (!t) break;   // OOM: leaves the tail unfilled (string length stays valid)
            if (!big) memcpy(t, stack, (size_t)sp * sizeof(jv));
            big = t; st = big; cap = nc;
        }
        st[sp++] = q->right;
        st[sp++] = q->left;
    }
    ojs_sys_free(big);
    (void)J;
}

jv jstr_concat(ojs* J, jv a, jv b) {
    uint32_t la = jstr_len(a), lb = jstr_len(b);
    if (!la) return b;
    if (!lb) return a;
    if ((uint64_t)la + lb > (1u << 29)) return throw_range(J, "Invalid string length");
    uint32_t n = la + lb;
    uint32_t depth = rope_depth(a) > rope_depth(b) ? rope_depth(a) : rope_depth(b);
    if (n < ROPE_MIN || depth >= ROPE_MAX_DEPTH) {
        struct str* fa = str_flat(J, a);
        if (!fa) return JV_EXC;
        struct str* fb = str_flat(J, b);
        if (!fb) return JV_EXC;
        struct str* s = str_alloc(J, n, str_wide(fa) || str_wide(fb));
        if (!s) return JV_EXC;
        uint32_t at = 0;
        copy_flat(fa, s, &at);
        copy_flat(fb, s, &at);
        return jv_from_str(s);
    }
    struct rope* r = (struct rope*)gc_alloc(J, GT_ROPE, sizeof(struct rope));
    if (!r) return JV_EXC;
    r->h.aux32 = n;
    r->left = a;
    r->right = b;
    r->depth = depth + 1;
    return jv_from_str((struct str*)r);
}

jv jstr_sub(ojs* J, jv v, uint32_t start, uint32_t end) {
    struct str* s = str_flat(J, v);
    if (!s) return JV_EXC;
    if (start == 0 && end == str_len(s)) return jv_from_str(s);
    if (end <= start) return jv_from_str(J->A->empty);
    struct str* r;
    if (str_wide(s)) r = str_new16(J, s->u.c16 + start, end - start);
    else r = str_new8(J, s->u.c8 + start, end - start);
    return r ? jv_from_str(r) : JV_EXC;
}

// ---------------------------------------------------------------- comparison, hashing

int str_eq(const struct str* a, const struct str* b) {
    if (a == b) return 1;
    uint32_t n = str_len(a);
    if (n != str_len(b)) return 0;
    if ((a->h.aux & SF_HASHED) && (b->h.aux & SF_HASHED) && a->hash != b->hash) return 0;
    if (!str_wide(a) && !str_wide(b)) return memcmp(a->u.c8, b->u.c8, n) == 0;
    if (str_wide(a) && str_wide(b)) return memcmp(a->u.c16, b->u.c16, (size_t)n * 2) == 0;
    for (uint32_t i = 0; i < n; i++) if (str_at(a, i) != str_at(b, i)) return 0;
    return 1;
}

int str_cmp(const struct str* a, const struct str* b) {
    uint32_t na = str_len(a), nb = str_len(b), n = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x = str_at(a, i), y = str_at(b, i);
        if (x != y) return x < y ? -1 : 1;
    }
    return na < nb ? -1 : na > nb ? 1 : 0;
}

int str_eq_ascii(const struct str* a, const char* lit) {
    uint32_t n = str_len(a), i = 0;
    for (; i < n && lit[i]; i++) if (str_at(a, i) != (uint8_t)lit[i]) return 0;
    return i == n && !lit[i];
}

static uint32_t hash_units(const struct str* s) {
    uint32_t h = 2166136261u, n = str_len(s);
    if (str_wide(s)) for (uint32_t i = 0; i < n; i++) h = (h ^ s->u.c16[i]) * 16777619u;
    else for (uint32_t i = 0; i < n; i++) h = (h ^ s->u.c8[i]) * 16777619u;
    h ^= h >> 15;
    return h ? h : 1;
}

uint32_t str_hash(struct str* s) {
    if (!(s->h.aux & SF_HASHED)) { s->hash = hash_units(s); s->h.aux |= SF_HASHED; }
    return s->hash;
}

int32_t str_index_of(const struct str* h, const struct str* n, int32_t from) {
    int32_t hl = (int32_t)str_len(h), nl = (int32_t)str_len(n);
    if (from < 0) from = 0;
    if (nl == 0) return from <= hl ? from : hl;
    if (nl > hl) return -1;
    uint32_t c0 = str_at(n, 0);
    for (int32_t i = from; i <= hl - nl; i++) {
        if (str_at(h, (uint32_t)i) != c0) continue;
        int32_t k = 1;
        while (k < nl && str_at(h, (uint32_t)(i + k)) == str_at(n, (uint32_t)k)) k++;
        if (k == nl) return i;
    }
    return -1;
}

int32_t str_last_index_of(const struct str* h, const struct str* n, int32_t from) {
    int32_t hl = (int32_t)str_len(h), nl = (int32_t)str_len(n);
    if (nl > hl) return -1;
    if (from > hl - nl) from = hl - nl;
    for (int32_t i = from; i >= 0; i--) {
        int32_t k = 0;
        while (k < nl && str_at(h, (uint32_t)(i + k)) == str_at(n, (uint32_t)k)) k++;
        if (k == nl) return i;
    }
    return -1;
}

// ---------------------------------------------------------------- atoms

static int atom_slot(ojs* J, const struct str* s, uint32_t h) {
    uint32_t mask = J->atom_cap - 1, i = h & mask;
    for (;;) {
        struct str* a = J->atoms[i];
        if (!a) return (int)i;
        if (a != (struct str*)1 && a->hash == h && str_eq(a, s)) return (int)i;
        i = (i + 1) & mask;
    }
}

static int atoms_rehash(ojs* J, uint32_t ncap) {
    struct str** old = J->atoms;
    uint32_t ocap = J->atom_cap;
    struct str** t = (struct str**)ojs_sys_malloc((size_t)ncap * sizeof(struct str*));
    if (!t) return -1;
    memset(t, 0, (size_t)ncap * sizeof(struct str*));
    J->atoms = t;
    J->atom_cap = ncap;
    J->natoms = 0;
    for (uint32_t i = 0; i < ocap; i++) {
        struct str* a = old[i];
        if (!a || a == (struct str*)1) continue;
        t[atom_slot(J, a, a->hash)] = a;
        J->natoms++;
    }
    ojs_sys_free(old);
    return 0;
}

struct str* atom_str(ojs* J, struct str* s) {
    if (s->h.aux & SF_ATOM) return s;
    if ((J->natoms + 1) * 4 >= J->atom_cap * 3 && atoms_rehash(J, J->atom_cap ? J->atom_cap * 2 : 1024) < 0) {
        throw_oom(J);
        return 0;
    }
    uint32_t h = str_hash(s);
    int i = atom_slot(J, s, h);
    if (J->atoms[i]) return J->atoms[i];
    // the atom must be a flat GT_STR the table owns: s is one
    s->h.aux |= SF_ATOM;
    J->atoms[i] = s;
    J->natoms++;
    return s;
}

struct str* atom_cstr(ojs* J, const char* lit) {
    // probe without allocating when the atom exists
    size_t n = strlen(lit);
    int ascii = 1;
    for (size_t i = 0; i < n; i++) if ((uint8_t)lit[i] >= 0x80) { ascii = 0; break; }
    if (ascii && J->atom_cap) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)lit[i]) * 16777619u;
        h ^= h >> 15;
        if (!h) h = 1;
        uint32_t mask = J->atom_cap - 1, i = h & mask;
        for (;;) {
            struct str* a = J->atoms[i];
            if (!a) break;
            if (a != (struct str*)1 && a->hash == h && !str_wide(a) && str_len(a) == n && !memcmp(a->u.c8, lit, n)) return a;
            i = (i + 1) & mask;
        }
    }
    struct str* s = str_from_utf8(J, lit, n);
    return s ? atom_str(J, s) : 0;
}

void atoms_sweep(ojs* J) {
    // weak table: drop atoms nothing references any more
    int dead = 0;
    for (uint32_t i = 0; i < J->atom_cap; i++) {
        struct str* a = J->atoms[i];
        if (!a || a == (struct str*)1) continue;
        if (!gc_is_marked(a)) { J->atoms[i] = (struct str*)1; J->natoms--; dead++; }
    }
    if (dead) atoms_rehash(J, J->atom_cap);   // drop tombstones
}

void atoms_mark_common(ojs* J) {
    if (!J->A) return;
    struct str** p = (struct str**)J->A;
    for (size_t i = 0; i < sizeof(struct atoms_common) / sizeof(struct str*); i++) gc_mark_ptr(J, p[i]);
}

int atoms_init(ojs* J) {
    if (atoms_rehash(J, 2048) < 0) return -1;
    J->A = (struct atoms_common*)ojs_sys_malloc(sizeof(struct atoms_common));
    if (!J->A) return -1;
#define ATOM_INIT(id, s) if (!(J->A->id = atom_cstr(J, s))) return -1;
    ATOM_LIST(ATOM_INIT)
#undef ATOM_INIT
    return 0;
}

// ---------------------------------------------------------------- property keys

// canonical array index "0".."2147483647" (no leading zeros, no sign)
static int str_index(const struct str* s, uint32_t* out) {
    uint32_t n = str_len(s);
    if (n == 0 || n > 10) return 0;
    uint32_t c = str_at(s, 0);
    if (c < '0' || c > '9' || (c == '0' && n > 1)) return 0;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        c = str_at(s, i);
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    if (v > 0x7FFFFFFFu) return 0;
    *out = (uint32_t)v;
    return 1;
}

pkey pkey_from_str(ojs* J, struct str* s) {
    uint32_t idx;
    if (!(s->h.aux & SF_NOTINDEX)) {
        if (str_index(s, &idx)) return PK_FROM_INDEX(idx);
        s->h.aux |= SF_NOTINDEX;
    }
    struct str* a = atom_str(J, s);
    if (!a) return PK_NONE;
    a->h.aux |= SF_NOTINDEX;
    return pk_from_atom(a);
}

pkey pkey_from_cstr(ojs* J, const char* s) {
    // indices written as literals are rare here; go through the general path
    struct str* a = atom_cstr(J, s);
    if (!a) return PK_NONE;
    uint32_t idx;
    if (!(a->h.aux & SF_NOTINDEX)) {
        if (str_index(a, &idx)) return PK_FROM_INDEX(idx);
        a->h.aux |= SF_NOTINDEX;
    }
    return pk_from_atom(a);
}

pkey pkey_from_value(ojs* J, jv v) {
    if (jv_is_int(v) && jv_int(v) >= 0) return PK_FROM_INDEX((uint32_t)jv_int(v));
    if (jv_is_num(v)) {
        double d = jv_dbl(v);
        if (d >= 0 && d <= 2147483647.0 && d == (double)d2u32(d) && !(d == 0 && (jv_from_dbl_raw(d) >> 63)))
            return PK_FROM_INDEX(d2u32(d));
    }
    if (jv_is_sym(v)) return pk_from_sym(jv_sym(v));
    if (jv_is_str(v)) {
        struct str* s = str_flat(J, v);
        return s ? pkey_from_str(J, s) : PK_NONE;
    }
    jv p = to_primitive(J, v, 2);
    if (p == JV_EXC) return PK_NONE;
    if (jv_is_sym(p)) return pk_from_sym(jv_sym(p));
    struct str* s = to_str(J, p);
    return s ? pkey_from_str(J, s) : PK_NONE;
}

jv pkey_to_value(ojs* J, pkey k) {
    if (PK_IS_INDEX(k)) {
        char buf[16];
        int n = ojs_snprintf(buf, sizeof buf, "%u", PK_INDEX(k));
        struct str* s = str_new8(J, (const uint8_t*)buf, (uint32_t)n);
        return s ? jv_from_str(s) : JV_EXC;
    }
    if (pk_is_sym(k)) return jv_from_sym(pk_sym(k));
    return jv_from_str(pk_str(k));
}

jv pkey_to_string(ojs* J, pkey k) {
    if (pk_is_sym(k)) {
        struct sym* s = pk_sym(k);
        if (jv_is_undef(s->desc)) return jv_from_str(J->A->empty);
        struct sbuf b;
        sb_init(J, &b);
        sb_putc(&b, '[');
        sb_put_str(&b, str_flat(J, s->desc));
        sb_putc(&b, ']');
        return sb_done(&b);
    }
    return pkey_to_value(J, k);
}

// ---------------------------------------------------------------- string builder

void sb_init(ojs* J, struct sbuf* b) { memset(b, 0, sizeof *b); b->J = J; }

static int sb_reserve(struct sbuf* b, uint32_t extra) {
    if (b->oom) return 0;
    uint64_t need = (uint64_t)b->len + extra;
    if (need <= b->cap) return 1;
    if (need > (1u << 29)) { b->oom = 2; return 0; }
    uint32_t nc = b->cap ? b->cap : 32;
    while (nc < need) nc *= 2;
    if (b->wide) {
        uint16_t* t = (uint16_t*)ojs_sys_realloc(b->b16, (size_t)nc * 2);
        if (!t) { b->oom = 1; return 0; }
        b->b16 = t;
    } else {
        uint8_t* t = (uint8_t*)ojs_sys_realloc(b->b8, nc);
        if (!t) { b->oom = 1; return 0; }
        b->b8 = t;
    }
    b->cap = nc;
    return 1;
}

static int sb_widen(struct sbuf* b) {
    uint32_t cap = b->cap ? b->cap : 32;
    uint16_t* t = (uint16_t*)ojs_sys_malloc((size_t)cap * 2);
    if (!t) { b->oom = 1; return 0; }
    for (uint32_t i = 0; i < b->len; i++) t[i] = b->b8[i];
    ojs_sys_free(b->b8);
    b->b8 = 0;
    b->b16 = t;
    b->cap = cap;
    b->wide = 1;
    return 1;
}

void sb_putc(struct sbuf* b, uint32_t cu) {
    if (cu > 0xFF && !b->wide && !sb_widen(b)) return;
    if (!sb_reserve(b, 1)) return;
    if (b->wide) b->b16[b->len++] = (uint16_t)cu;
    else b->b8[b->len++] = (uint8_t)cu;
}

void sb_put_cp(struct sbuf* b, uint32_t cp) {
    if (cp >= 0x10000) {
        cp -= 0x10000;
        sb_putc(b, 0xD800 + (cp >> 10));
        sb_putc(b, 0xDC00 + (cp & 0x3FF));
    } else sb_putc(b, cp);
}

void sb_puts(struct sbuf* b, const char* s) {
    size_t n = strlen(s);
    if (!sb_reserve(b, (uint32_t)n)) return;
    for (size_t i = 0; i < n; i++) sb_putc(b, (uint8_t)s[i]);
}

void sb_put_sub(struct sbuf* b, const struct str* s, uint32_t from, uint32_t to) {
    if (!s || to <= from) return;
    if (str_wide(s) && !b->wide) {
        for (uint32_t i = from; i < to; i++) if (s->u.c16[i] > 0xFF) { if (!sb_widen(b)) return; break; }
    }
    if (!sb_reserve(b, to - from)) return;
    if (b->wide) {
        if (str_wide(s)) memcpy(b->b16 + b->len, s->u.c16 + from, (size_t)(to - from) * 2);
        else for (uint32_t i = from; i < to; i++) b->b16[b->len + i - from] = s->u.c8[i];
    } else {
        if (str_wide(s)) for (uint32_t i = from; i < to; i++) b->b8[b->len + i - from] = (uint8_t)s->u.c16[i];
        else memcpy(b->b8 + b->len, s->u.c8 + from, to - from);
    }
    b->len += to - from;
}

void sb_put_str(struct sbuf* b, const struct str* s) { if (s) sb_put_sub(b, s, 0, str_len(s)); }

void sb_put_utf8(struct sbuf* b, const char* s, size_t n) {
    for (size_t i = 0; i < n;) sb_put_cp(b, utf8_next((const uint8_t*)s, n, &i));
}

jv sb_done(struct sbuf* b) {
    ojs* J = b->J;
    if (b->oom) {
        int range = b->oom == 2;
        sb_free(b);
        return range ? throw_range(J, "Invalid string length") : throw_oom(J);
    }
    struct str* s = b->wide ? str_new16(J, b->b16, b->len) : str_new8(J, b->b8, b->len);
    sb_free(b);
    return s ? jv_from_str(s) : JV_EXC;
}

void sb_free(struct sbuf* b) {
    ojs_sys_free(b->b8);
    ojs_sys_free(b->b16);
    b->b8 = 0;
    b->b16 = 0;
    b->len = b->cap = 0;
}
