// srctext.c - compact storage for the source text functions keep.
//
// Every function template keeps its script's source (Function.prototype.toString,
// line numbers in stack traces). A script with one character above U+00FF anywhere
// is a UTF-16 string: twice the size of the text. Once such a script is compiled its
// templates switch to a UTF-8 copy (WTF-8: lone surrogates as 3-byte sequences, so
// any code unit sequence round-trips) with the byte offset of every 1024th code
// unit, and the UTF-16 string can be collected. Readers walk it with a cursor.

#include "ojs_int.h"
#include "gc_int.h"
#include "vm.h"

#define CHECK_SHIFT 10   // a checkpoint every 1024 code units

struct srctext {
    uint32_t units;      // length in UTF-16 code units
    uint32_t nbytes;
    uint32_t ncheck;
    uint32_t pad;
    // uint32_t check[ncheck]; uint8_t utf8[nbytes];
};

static inline const uint32_t* checks(const struct srctext* t) { return (const uint32_t*)(t + 1); }
static inline const uint8_t* bytes(const struct srctext* t) { return (const uint8_t*)(checks(t) + t->ncheck); }

struct srctext* srctext_new(ojs* J, const struct str* s) {
    uint32_t n = str_len(s), nb = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = str_at(s, i);
        if (c < 0x80) nb += 1;
        else if (c < 0x800) nb += 2;
        else if ((c & 0xFC00) == 0xD800 && i + 1 < n && (str_at(s, i + 1) & 0xFC00) == 0xDC00) { nb += 4; i++; }
        else nb += 3;
    }
    uint32_t nc = (n >> CHECK_SHIFT) + 1;
    struct srctext* t = (struct srctext*)bytes_new(J, sizeof *t + (size_t)nc * 4 + nb);
    if (!t) return 0;
    t->units = n;
    t->nbytes = nb;
    t->ncheck = nc;
    uint32_t* ck = (uint32_t*)(t + 1);
    uint8_t* o = (uint8_t*)(ck + nc);
    uint32_t b = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!(i & ((1u << CHECK_SHIFT) - 1))) ck[i >> CHECK_SHIFT] = b;
        uint32_t c = str_at(s, i);
        if (c < 0x80) o[b++] = (uint8_t)c;
        else if (c < 0x800) { o[b++] = (uint8_t)(0xC0 | (c >> 6)); o[b++] = (uint8_t)(0x80 | (c & 0x3F)); }
        else if ((c & 0xFC00) == 0xD800 && i + 1 < n && (str_at(s, i + 1) & 0xFC00) == 0xDC00) {
            uint32_t cp = 0x10000 + ((c - 0xD800) << 10) + (str_at(s, i + 1) - 0xDC00);
            o[b++] = (uint8_t)(0xF0 | (cp >> 18));
            o[b++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            o[b++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
            o[b++] = (uint8_t)(0x80 | (cp & 0x3F));
            i++;
            // a pair never straddles a checkpoint: the low half's unit index is i
            if (!(i & ((1u << CHECK_SHIFT) - 1))) ck[i >> CHECK_SHIFT] = b - 4 + 0x80000000u;   // (marked: mid-pair)
        } else {
            o[b++] = (uint8_t)(0xE0 | (c >> 12));
            o[b++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            o[b++] = (uint8_t)(0x80 | (c & 0x3F));
        }
    }
    if (!(n & ((1u << CHECK_SHIFT) - 1))) ck[n >> CHECK_SHIFT] = b;   // the end, when it falls on one
    return t;
}

uint32_t srctext_len(const struct srctext* t) { return t->units; }

// cursor over code units
void srccur_init(struct srccur* c, const struct srctext* t, uint32_t pos) {
    c->t = t;
    if (pos > t->units) pos = t->units;
    uint32_t k = pos >> CHECK_SHIFT;
    uint32_t ck = checks(t)[k];
    c->unit = k << CHECK_SHIFT;
    c->low = 0;
    if (ck & 0x80000000u) {   // the checkpoint unit is the low half of a pair starting at ck
        c->byte = ck & 0x7FFFFFFFu;
        uint32_t hi = srccur_next(c);   // decodes the pair: returns the high half, keeps the low
        (void)hi;
        c->unit = k << CHECK_SHIFT;    // positioned at the low half
    } else c->byte = ck;
    while (c->unit < pos) srccur_next(c);
}

uint32_t srccur_next(struct srccur* c) {
    if (c->low) { uint32_t l = c->low; c->low = 0; c->unit++; return l; }
    const struct srctext* t = c->t;
    if (c->unit >= t->units || c->byte >= t->nbytes) return 0xFFFFFFFFu;
    const uint8_t* p = bytes(t) + c->byte;
    uint32_t b0 = p[0], cp;
    if (b0 < 0x80) { cp = b0; c->byte += 1; }
    else if (b0 < 0xE0) { cp = ((b0 & 0x1F) << 6) | (p[1] & 0x3F); c->byte += 2; }
    else if (b0 < 0xF0) { cp = ((b0 & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); c->byte += 3; }
    else {
        cp = ((b0 & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        c->byte += 4;
        cp -= 0x10000;
        c->low = 0xDC00 + (cp & 0x3FF);
        c->unit++;
        return 0xD800 + (cp >> 10);
    }
    c->unit++;
    return cp;
}

// the code units [from, to) as a string
jv srctext_slice(ojs* J, const struct srctext* t, uint32_t from, uint32_t to) {
    if (to > t->units) to = t->units;
    struct sbuf b;
    sb_init(J, &b);
    struct srccur c;
    srccur_init(&c, t, from);
    for (uint32_t i = from; i < to; i++) {
        uint32_t u = srccur_next(&c);
        if (u == 0xFFFFFFFFu) break;
        sb_putc(&b, u);
    }
    return sb_done(&b);
}

// switch every template of one compile (the top one and those nested in its
// constants) from the UTF-16 source to a compact copy
static void retarget(struct ftempl* t, const struct str* s, struct srctext* x, int depth) {
    if (!t || depth > 200) return;
    if (t->source == s) { t->source = 0; t->srct = x; }
    for (uint32_t i = 0; i < t->nconsts; i++) {
        jv v = t->consts[i];
        if (JV_TAG(v) == TAG_PTR && ((struct gch*)JV_PTR(v))->type == GT_FTEMPL) retarget((struct ftempl*)JV_PTR(v), s, x, depth + 1);
    }
}

void srctext_compact(ojs* J, struct ftempl* top, struct ftempl* top2, const struct str* s) {
    if (!top || !s || !str_wide(s) || str_len(s) < 16384) return;   // small or 8-bit: as is
    struct srctext* x = srctext_new(J, s);
    if (!x) { take_exc(J); return; }   // out of memory: keep the string
    retarget(top, s, x, 0);
    retarget(top2, s, x, 0);
}
