#include "serial.h"
#include "dom.h"
#include "html.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int tag_eq_len(const char* a, int n, const char* b) {
    int i = 0;
    while (b[i]) {
        if (i >= n) return 0;
        if (lc((unsigned char)a[i]) != b[i]) return 0;
        i++;
    }
    return i == n;
}

// ---------------------------------------------------------------------------
// Arena helpers
// ---------------------------------------------------------------------------

static int name_store(struct dom* d, const char* s, int n) {
    if (d->name_len + n + 1 > DOM_MAX_NAMES) { d->truncated = 1; d->trunc_why = 1; return -1; }
    int off = d->name_len;
    for (int i = 0; i < n; i++) d->names[off + i] = (char)lc((unsigned char)s[i]);
    d->names[off + n] = 0;
    d->name_len += n + 1;
    return off;
}

static void node_tag(struct dom* d, int node, int off, int len) {
    d->nodes[node].tag_off = (uint32_t)off;
    d->nodes[node].tag_len = (uint8_t)len;
}

static int new_node(struct dom* d, uint8_t type) {
    if (d->node_count >= DOM_MAX_NODES) { d->truncated = 1; d->trunc_why = 2; return -1; }
    int n = d->node_count++;
    struct dom_node* p = &d->nodes[n];
    p->type = type;
    p->tag_off = 0; p->tag_len = 0;
    p->attr_head = DOM_NONE;
    p->parent = p->first_child = p->last_child = p->next_sib = DOM_NONE;
    p->text_off = 0; p->text_len = 0; p->flags = 0;
    return n;
}

static void append_child(struct dom* d, int parent, int child) {
    if (parent < 0 || child < 0) return;
    d->nodes[child].parent = (uint16_t)parent;
    d->nodes[child].next_sib = DOM_NONE;
    if (d->nodes[parent].last_child == DOM_NONE) {
        d->nodes[parent].first_child = (uint16_t)child;
    } else {
        d->nodes[d->nodes[parent].last_child].next_sib = (uint16_t)child;
    }
    d->nodes[parent].last_child = (uint16_t)child;
}

static int text_is_ws(const char* s, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c > ' ') return 0;
    }
    return 1;
}

// Decode a raw source span (charset + entities) directly into the text arena
// and append it to `node` when contiguous, else create a new text node.
// Returns the text node index, or -1 when the arena is full.
static int text_add(struct dom* d, int parent, int* last_text,
                    const char* raw, int len) {
    if (len <= 0) return -1;
    if (len > DOM_MAX_TEXT - d->text_len - 1) {
        len = DOM_MAX_TEXT - d->text_len - 1;
        d->truncated = 1; d->trunc_why = 3;
        if (len <= 0) return -1;
    }
    int node = -1;
    if (last_text && *last_text >= 0) {
        struct dom_node* t = &d->nodes[*last_text];
        // Merge only when it is the SAME parent's immediately preceding text
        // run (arena contiguity alone would glue text across element
        // boundaries whenever offsets happen to line up).
        if (t->parent == (uint16_t)parent &&
            (int)t->text_off + (int)t->text_len == d->text_len)
            node = *last_text;
    }
    if (node < 0) {
        node = new_node(d, DOM_NODE_TEXT);
        if (node < 0) return -1;
        d->nodes[node].text_off = (uint32_t)d->text_len;
        append_child(d, parent, node);
    }
    char* dst = d->text + d->text_len;
    memcpy(dst, raw, (size_t)len);
    dst[len] = 0;
    html_decode_entities(dst);
    int newlen = 0;
    while (dst[newlen]) newlen++;
    d->nodes[node].text_len = (uint16_t)(d->nodes[node].text_len + newlen);
    d->text_len += newlen;
    if (text_is_ws(d->text + d->nodes[node].text_off, d->nodes[node].text_len))
        d->nodes[node].flags |= DOM_F_WS;
    if (last_text) *last_text = node;
    return node;
}

// Store one attribute on `node` (decoded value in the text arena).
static void attr_add(struct dom* d, int node, const char* name, int nlen,
                     const char* rawval, int vlen) {
    if (d->attr_count >= DOM_MAX_ATTRS) { d->truncated = 1; d->trunc_why = 4; return; }
    if (nlen <= 0 || nlen > 255) return;
    int noff = name_store(d, name, nlen);
    if (noff < 0) return;
    if (vlen > DOM_MAX_TEXT - d->text_len - 1) {
        vlen = DOM_MAX_TEXT - d->text_len - 1;
        d->truncated = 1; d->trunc_why = 3;
    }
    if (vlen < 0) vlen = 0;
    int voff = d->text_len;
    if (vlen > 0) {
        memcpy(d->text + voff, rawval, (size_t)vlen);
        d->text[voff + vlen] = 0;
        html_decode_entities(d->text + voff);
        int nl = 0;
        while (d->text[voff + nl]) nl++;
        vlen = nl;
        d->text_len += vlen;
    }
    int a = d->attr_count++;
    struct dom_attr* at = &d->attrs[a];
    at->name_off = (uint32_t)noff;
    at->name_len = (uint8_t)nlen;
    at->val_off = (uint32_t)voff;
    at->val_len = (uint16_t)vlen;
    at->next = d->nodes[node].attr_head;
    d->nodes[node].attr_head = (uint16_t)a;
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

#define TOK_TEXT  0
#define TOK_START 1
#define TOK_END   2
#define TOK_EOF   3

#define TOK_MAX_ATTRS 16
#define TOK_ATTR_NAME 24
#define TOK_ATTR_VAL  192

struct tok_attr {
    char name[TOK_ATTR_NAME];
    uint8_t nl;
    char val[TOK_ATTR_VAL];
    int vl;
};

struct tok {
    int type;
    char name[32];
    int nl;
    const char* text;
    int text_len;      // TOK_TEXT: raw span length (0 for an all-consumed span)
    int self_close;
    struct tok_attr attrs[TOK_MAX_ATTRS];
    int nattrs;
    int dropped_attrs;
};

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_name_end(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '>' || c == '/';
}

// One token from `s` at *ppos. Text tokens span raw input. Comments/doctype
// are consumed and reported as an empty TOK_TEXT so callers can ignore them.
static void tok_next(const char* s, int len, int* ppos, struct tok* t) {
    int p = *ppos;
    t->type = TOK_TEXT;
    t->nl = 0;
    t->text = 0; t->text_len = 0;
    t->self_close = 0;
    t->nattrs = 0; t->dropped_attrs = 0;

    if (p >= len) { t->type = TOK_EOF; return; }
    if (s[p] != '<') {
        int start = p;
        while (p < len && s[p] != '<') p++;
        t->text = s + start; t->text_len = p - start;
        *ppos = p;
        return;
    }
    if (p + 1 >= len) { // lone '<' at EOF is text
        t->text = s + p; t->text_len = 1; *ppos = p + 1; return;
    }
    int c = (unsigned char)s[p + 1];
    if (c == '!') {
        if (p + 3 < len && s[p + 2] == '-' && s[p + 3] == '-') {
            int q = p + 4;
            while (q + 2 < len && !(s[q] == '-' && s[q + 1] == '-' && s[q + 2] == '>')) q++;
            *ppos = (q + 2 < len) ? q + 3 : len;
            return; // comment: empty text token
        }
        if (p + 8 < len && s[p + 2] == '[' && s[p + 3] == 'C' && s[p + 4] == 'D' &&
            s[p + 5] == 'A' && s[p + 6] == 'T' && s[p + 7] == 'A' && s[p + 8] == '[') {
            int q = p + 9;
            while (q + 2 < len && !(s[q] == ']' && s[q + 1] == ']' && s[q + 2] == '>')) q++;
            t->text = s + p + 9;
            t->text_len = (q + 2 < len) ? q - (p + 9) : len - (p + 9);
            *ppos = (q + 2 < len) ? q + 3 : len;
            return;
        }
        // doctype / bogus declaration: skip to '>'
        while (p < len && s[p] != '>') p++;
        *ppos = (p < len) ? p + 1 : len;
        return;
    }
    if (c == '?') {
        // processing instruction / bogus: skip to '>'
        while (p < len && s[p] != '>') p++;
        *ppos = (p < len) ? p + 1 : len;
        return;
    }

    int is_end = 0;
    p++; // consume '<'
    if (s[p] == '/') { is_end = 1; p++; }

    // Tag name
    int ns = p;
    while (p < len && !is_name_end((unsigned char)s[p])) p++;
    int nlen = p - ns;
    if (nlen > 31) nlen = 31;
    for (int i = 0; i < nlen; i++) t->name[i] = (char)lc((unsigned char)s[ns + i]);
    t->name[nlen] = 0;
    t->nl = nlen;
    t->type = is_end ? TOK_END : TOK_START;
    if (nlen == 0 && !is_end) { // "<>" or "< x": not a tag, emit '<' as text
        t->type = TOK_TEXT;
        t->text = s + (p - nlen - 1);
        t->text_len = 1;
        *ppos = (p - nlen - 1) + 1;
        return;
    }
    if (is_end) {
        while (p < len && s[p] != '>') p++;
        *ppos = (p < len) ? p + 1 : len;
        return;
    }

    // Attributes
    while (p < len) {
        while (p < len && is_ws((unsigned char)s[p])) p++;
        if (p >= len) break;
        if (s[p] == '>') { p++; break; }
        if (s[p] == '/' ) {
            if (p + 1 < len && s[p + 1] == '>') { t->self_close = 1; p += 2; break; }
            p++; continue;
        }
        int ans = p;
        while (p < len && !is_name_end((unsigned char)s[p]) && s[p] != '=') p++;
        int anl = p - ans;
        if (anl <= 0) { p++; continue; }
        int keep = t->nattrs < TOK_MAX_ATTRS;
        struct tok_attr* a = keep ? &t->attrs[t->nattrs] : 0;
        int an = anl > TOK_ATTR_NAME - 1 ? TOK_ATTR_NAME - 1 : anl;
        if (keep) {
            for (int i = 0; i < an; i++) a->name[i] = (char)lc((unsigned char)s[ans + i]);
            a->name[an] = 0; a->nl = (uint8_t)an; a->vl = 0; a->val[0] = 0;
        }
        while (p < len && is_ws((unsigned char)s[p])) p++;
        if (p < len && s[p] == '=') {
            p++;
            while (p < len && is_ws((unsigned char)s[p])) p++;
            int vs = p;
            if (p < len && (s[p] == '"' || s[p] == '\'')) {
                int q = (unsigned char)s[p++];
                vs = p;
                while (p < len && (unsigned char)s[p] != q) p++;
                if (keep) {
                    int vn = p - vs;
                    if (vn > TOK_ATTR_VAL - 1) vn = TOK_ATTR_VAL - 1;
                    for (int i = 0; i < vn; i++) a->val[i] = s[vs + i];
                    a->val[vn] = 0; a->vl = vn;
                }
                if (p < len) p++; // closing quote
            } else {
                while (p < len && !is_name_end((unsigned char)s[p])) p++;
                if (keep) {
                    int vn = p - vs;
                    if (vn > TOK_ATTR_VAL - 1) vn = TOK_ATTR_VAL - 1;
                    for (int i = 0; i < vn; i++) a->val[i] = s[vs + i];
                    a->val[vn] = 0; a->vl = vn;
                }
            }
        }
        if (keep) t->nattrs++;
        else t->dropped_attrs++;
    }
    *ppos = p;
}

// Raw-text element body: find the matching close tag starting at `p`. On
// success returns the close tag start and sets *close_end past its '>' and
// *text/*tlen to the raw body span. Returns -1 when unterminated.
static int raw_scan(const char* s, int len, int p, const char* name,
                    const char** text, int* tlen, int* close_end) {
    int nl = 0;
    while (name[nl]) nl++;
    int i = p;
    while (i < len) {
        if (s[i] == '<' && i + 1 < len && s[i + 1] == '/') {
            int j = i + 2;
            int k = 0;
            while (k < nl && j + k < len && lc((unsigned char)s[j + k]) == name[k]) k++;
            if (k == nl) {
                int e = j + k;
                if (e >= len || is_ws((unsigned char)s[e]) || s[e] == '>' || s[e] == '/') {
                    while (e < len && s[e] != '>') e++;
                    *text = s + p;
                    *tlen = i - p;
                    *close_end = (e < len) ? e + 1 : len;
                    return i;
                }
            }
        }
        i++;
    }
    *text = s + p;
    *tlen = len - p;
    *close_end = len;
    return -1;
}

// ---------------------------------------------------------------------------
// Tree builder
// ---------------------------------------------------------------------------

#define MAX_OPEN 2048

static int find_open(const struct dom* d, const int* st, int sp, const char* tag) {
    for (int i = sp - 1; i >= 0; i--) {
        int n = st[i];
        if (n >= 0 && dom_tag_is(d, n, tag)) return i;
    }
    return -1;
}

// Like find_open, but stops (returning -1) as soon as an element matching one
// of `stop` is crossed. Table implied-end-tag searches MUST be scoped this
// way: without it, an inner <tr> finds an OUTER <td> and closes through the
// enclosing <table>, destroying nested-table structure (Hacker News, most
// email layouts, Wikipedia infoboxes).
static int find_open_scoped(const struct dom* d, const int* st, int sp,
                            const char* tag, const char* stop1, const char* stop2) {
    for (int i = sp - 1; i >= 0; i--) {
        int n = st[i];
        if (n < 0) continue;
        if (dom_tag_is(d, n, tag)) return i;
        if ((stop1 && dom_tag_is(d, n, stop1)) ||
            (stop2 && dom_tag_is(d, n, stop2))) return -1;
    }
    return -1;
}

// Pop everything above index `idx` inclusive.
static void close_through(int* st, int* sp, int idx) {
    (void)st;
    if (idx < 0) return;
    *sp = idx;
}

int dom_build(struct dom* d, const char* html, int len) {
    dom_reset(d);
    html_sniff_charset(html, len);

    int root = new_node(d, DOM_NODE_ROOT);
    if (root < 0) return -1;
    d->root = root;
    if (len <= 0) return root;

    int st[MAX_OPEN];
    int sp = 0;
    st[sp++] = root;
    int last_text = -1;

    int p = 0;
    struct tok tk;
    // Skip HTTP headers (before \r\n\r\n) like the old parser did.
    for (int i = 0; i + 3 < len; i++) {
        if (html[i] == '\r' && html[i+1] == '\n' && html[i+2] == '\r' && html[i+3] == '\n') {
            p = i + 4;
            break;
        }
    }

    while (p < len && !d->truncated) {
        tok_next(html, len, &p, &tk);
        int cur = st[sp - 1];

        if (tk.type == TOK_TEXT) {
            if (tk.text_len > 0) text_add(d, cur, &last_text, tk.text, tk.text_len);
            continue;
        }
        if (tk.type == TOK_EOF) break;

        if (tk.type == TOK_START) {
            const char* tag = tk.name;
            int tl = tk.nl;
            if (tl == 0) continue;

            // Implied end tags. Table-scoped searches stop at the enclosing
            // <table> so nested tables never collapse (see find_open_scoped).
            if (tag_eq_len(tag, tl, "li")) {
                int i = find_open_scoped(d, st, sp, "li", "ul", "ol");
                if (i >= 0) close_through(st, &sp, i);
            } else if (tag_eq_len(tag, tl, "dt") ||
                       tag_eq_len(tag, tl, "dd")) {
                int i = find_open_scoped(d, st, sp, "dt", "dl", "table");
                int j = find_open_scoped(d, st, sp, "dd", "dl", "table");
                int m = i > j ? i : j;
                if (m >= 0) close_through(st, &sp, m);
            } else if (tag_eq_len(tag, tl, "td") ||
                       tag_eq_len(tag, tl, "th")) {
                int i = find_open_scoped(d, st, sp, "td", "tr", "table");
                int j = find_open_scoped(d, st, sp, "th", "tr", "table");
                int m = i > j ? i : j;
                if (m >= 0) close_through(st, &sp, m);
            } else if (tag_eq_len(tag, tl, "tr")) {
                int i = find_open_scoped(d, st, sp, "td", "table", 0);
                int j = find_open_scoped(d, st, sp, "th", "table", 0);
                int k = find_open_scoped(d, st, sp, "tr", "table", 0);
                int m = i > j ? i : j;
                if (k > m) m = k;
                if (m >= 0) close_through(st, &sp, m);
            } else if (tag_eq_len(tag, tl, "a")) {
                int i = find_open(d, st, sp, "a");
                if (i >= 0) close_through(st, &sp, i);
            } else if (tag_eq_len(tag, tl, "button")) {
                int i = find_open(d, st, sp, "button");
                if (i >= 0) close_through(st, &sp, i);
            } else if (tag_eq_len(tag, tl, "option")) {
                int i = find_open(d, st, sp, "option");
                if (i >= 0) close_through(st, &sp, i);
            }
            // A block start closes an open <p>.
            if (dom_is_block(tag, tl)) {
                int i = find_open(d, st, sp, "p");
                if (i >= 0) close_through(st, &sp, i);
            }
            if (tag[0] == 'h' && tl == 2 && tag[1] >= '1' && tag[1] <= '6') {
                for (int lvl = 1; lvl <= 6; lvl++) {
                    char hn[3] = { 'h', (char)('0' + lvl), 0 };
                    int i = find_open(d, st, sp, hn);
                    if (i >= 0) { close_through(st, &sp, i); break; }
                }
            }

            cur = st[sp - 1];
            int noff = name_store(d, tag, tl);
            int node = new_node(d, DOM_NODE_ELEMENT);
            if (node < 0) break;
            node_tag(d, node, noff, tl);
            append_child(d, cur, node);
            last_text = -1;
            for (int i = 0; i < tk.nattrs; i++) {
                struct tok_attr* a = &tk.attrs[i];
                attr_add(d, node, a->name, a->nl, a->val, a->vl);
            }

            if (tk.self_close || dom_is_void(tag, tl)) {
                continue;
            }
            if (dom_is_rawtext(tag, tl)) {
                const char* rtext = 0; int rlen = 0, cend = p;
                raw_scan(html, len, p, tag, &rtext, &rlen, &cend);
                if (rlen > 0) text_add(d, node, &last_text, rtext, rlen);
                p = cend;
                last_text = -1;
                continue; // element stays open in the source but has no content
            }
            if (sp < MAX_OPEN) st[sp++] = node;
            else { d->truncated = 1; d->trunc_why = 5; }
            continue;
        }

        // TOK_END
        if (tk.nl == 0) continue;
        if (tag_eq_len(tk.name, tk.nl, "br")) {
            // </br> acts as <br>
            int noff = name_store(d, "br", 2);
            int node = new_node(d, DOM_NODE_ELEMENT);
            if (node < 0) break;
            node_tag(d, node, noff, 2);
            append_child(d, cur, node);
            last_text = -1;
            continue;
        }
        int idx = find_open(d, st, sp, tk.name);
        if (idx >= 0) close_through(st, &sp, idx);
        last_text = -1;
    }

#if defined(KERNEL) && KERNEL
    serial_printf("[dom] end p=%d/%d nodes=%d attrs=%d text=%d names=%d trunc=%d why=%d\n",
                  p, len, d->node_count, d->attr_count, d->text_len, d->name_len, d->truncated, d->trunc_why);
#endif
    return root;
}

// ---------------------------------------------------------------------------
// Queries / classification
// ---------------------------------------------------------------------------

void dom_reset(struct dom* d) {
    d->node_count = 0;
    d->attr_count = 0;
    d->text_len = 0;
    d->name_len = 0;
    d->root = -1;
    d->truncated = 0;
}

int dom_tag_is(const struct dom* d, int node, const char* tag) {
    if (node < 0 || node >= d->node_count) return 0;
    const struct dom_node* n = &d->nodes[node];
    if (n->type != DOM_NODE_ELEMENT) return 0;
    return tag_eq_len(d->names + n->tag_off, n->tag_len, tag);
}

void dom_tag_copy(const struct dom* d, int node, char* out, int cap) {
    out[0] = 0;
    if (node < 0 || node >= d->node_count || cap <= 0) return;
    const struct dom_node* n = &d->nodes[node];
    if (n->type != DOM_NODE_ELEMENT) return;
    int l = n->tag_len < cap - 1 ? n->tag_len : cap - 1;
    memcpy(out, d->names + n->tag_off, (size_t)l);
    out[l] = 0;
}

int dom_attr_get(const struct dom* d, int node, const char* name, char* out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (node < 0 || node >= d->node_count) return -1;
    int nl = 0;
    while (name[nl]) nl++;
    for (int a = d->nodes[node].attr_head; a != DOM_NONE; a = d->attrs[a].next) {
        const struct dom_attr* at = &d->attrs[a];
        if (at->name_len != nl) continue;
        int ok = 1;
        for (int i = 0; i < nl; i++)
            if (d->names[at->name_off + i] != name[i]) { ok = 0; break; }
        if (!ok) continue;
        if (out && cap > 0) {
            int l = at->val_len < cap - 1 ? at->val_len : cap - 1;
            memcpy(out, d->text + at->val_off, (size_t)l);
            out[l] = 0;
        }
        return at->val_len;
    }
    return -1;
}

int dom_attr_set(struct dom* d, int node, const char* name, const char* val) {
    if (!d || !name || !val) return -1;
    if (node < 0 || node >= d->node_count) return -1;
    if (d->nodes[node].type != DOM_NODE_ELEMENT) return -1;
    int nl = 0;
    while (name[nl]) nl++;
    if (nl <= 0 || nl > 64) return -1;
    int vl = 0;
    while (val[vl]) vl++;
    if (vl > 60000) return -1; // val_len is uint16_t
    // Find existing attribute (names stored lowercase at parse).
    int found = -1;
    for (int a = d->nodes[node].attr_head; a != DOM_NONE; a = d->attrs[a].next) {
        const struct dom_attr* at = &d->attrs[a];
        if (at->name_len != nl) continue;
        int ok = 1;
        for (int i = 0; i < nl; i++)
            if (d->names[at->name_off + i] != name[i]) { ok = 0; break; }
        if (ok) { found = a; break; }
    }
    if (found >= 0) {
        struct dom_attr* at = &d->attrs[found];
        if (vl <= (int)at->val_len) {
            // Fits the old slot: overwrite in place (arena hole is fine).
            for (int i = 0; i < vl; i++) d->text[at->val_off + i] = val[i];
            d->text[at->val_off + vl] = 0;
            at->val_len = (uint16_t)vl;
            return 0;
        }
        if (d->text_len + vl + 1 > DOM_MAX_TEXT) return -1;
        at->val_off = (uint32_t)d->text_len;
        for (int i = 0; i < vl; i++) d->text[d->text_len++] = val[i];
        d->text[d->text_len++] = 0;
        at->val_len = (uint16_t)vl;
        return 0;
    }
    // Brand-new attribute: need a table slot, a names-arena slot and room
    // in the text arena. Prepended to the head so it wins name lookups.
    if (d->attr_count >= DOM_MAX_ATTRS) return -1;
    if (d->name_len + nl + 1 > DOM_MAX_NAMES) return -1;
    if (d->text_len + vl + 1 > DOM_MAX_TEXT) return -1;
    int a = d->attr_count++;
    struct dom_attr* at = &d->attrs[a];
    at->name_off = (uint32_t)d->name_len;
    for (int i = 0; i < nl; i++) d->names[d->name_len++] = name[i];
    d->names[d->name_len++] = 0;
    at->name_len = (uint8_t)nl;
    at->val_off = (uint32_t)d->text_len;
    for (int i = 0; i < vl; i++) d->text[d->text_len++] = val[i];
    d->text[d->text_len++] = 0;
    at->val_len = (uint16_t)vl;
    at->next = d->nodes[node].attr_head;
    d->nodes[node].attr_head = (uint16_t)a;
    return 0;
}

const char* dom_text(const struct dom* d, int node, int* len) {
    if (len) *len = 0;
    if (node < 0 || node >= d->node_count) return 0;
    const struct dom_node* n = &d->nodes[node];
    if (n->type != DOM_NODE_TEXT) return 0;
    if (len) *len = n->text_len;
    return d->text + n->text_off;
}

void dom_text_copy(const struct dom* d, int node, char* out, int cap) {
    out[0] = 0;
    if (node < 0 || node >= d->node_count || cap <= 0) return;
    const struct dom_node* n = &d->nodes[node];
    if (n->type != DOM_NODE_TEXT) return;
    int l = n->text_len < cap - 1 ? n->text_len : cap - 1;
    memcpy(out, d->text + n->text_off, (size_t)l);
    out[l] = 0;
}

// Depth-first document-order search for the first element satisfying `want`.
static int find_element(const struct dom* d, int node, int (*want)(const struct dom*, int, const char*), const char* arg) {
    if (node < 0 || node >= d->node_count) return -1;
    if (d->nodes[node].type == DOM_NODE_ELEMENT && want(d, node, arg)) return node;
    for (int c = d->nodes[node].first_child; c != DOM_NONE; c = d->nodes[c].next_sib) {
        int r = find_element(d, c, want, arg);
        if (r >= 0) return r;
    }
    return -1;
}

static int want_tag(const struct dom* d, int node, const char* tag) {
    return dom_tag_is(d, node, tag);
}
static int want_id(const struct dom* d, int node, const char* id) {
    char buf[64];
    int l = dom_attr_get(d, node, "id", buf, sizeof(buf));
    if (l < 0) return 0;
    int i = 0;
    while (id[i] && buf[i] == id[i]) i++;
    return id[i] == 0 && buf[i] == 0;
}
static int want_tag_class(const struct dom* d, int node, const char* arg) {
    // arg = "tag\0class"
    const char* tag = arg;
    const char* cls = arg;
    while (*cls) cls++;
    cls++;
    if (!dom_tag_is(d, node, tag)) return 0;
    char buf[128];
    int l = dom_attr_get(d, node, "class", buf, sizeof(buf));
    if (l < 0) return 0;
    int n = 0;
    while (cls[n]) n++;
    for (int i = 0; buf[i]; ) {
        while (buf[i] == ' ' || buf[i] == '\t') i++;
        int j = i;
        while (buf[j] && buf[j] != ' ' && buf[j] != '\t') j++;
        if (j - i == n) {
            int k = 0;
            while (k < n && buf[i + k] == cls[k]) k++;
            if (k == n) return 1;
        }
        i = j;
    }
    return 0;
}

int dom_first_tag(const struct dom* d, const char* tag) {
    return find_element(d, d->root, want_tag, tag);
}
int dom_find_id(const struct dom* d, const char* id) {
    return find_element(d, d->root, want_id, id);
}
int dom_first_tag_class(const struct dom* d, const char* tag, const char* cls) {
    char arg[128];
    int i = 0;
    while (tag[i] && i < 100) { arg[i] = tag[i]; i++; }
    arg[i++] = 0;
    int j = 0;
    while (cls[j] && i < 126) arg[i++] = cls[j++];
    arg[i] = 0;
    return find_element(d, d->root, want_tag_class, arg);
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

static const char* void_tags[] = {
    "area","base","br","col","embed","hr","img","input","link","meta",
    "param","source","track","wbr",0
};
static const char* block_tags[] = {
    "address","article","aside","blockquote","body","dd","div","dl","dt",
    "fieldset","figcaption","figure","footer","form","h1","h2","h3","h4",
    "h5","h6","header","hr","html","li","main","nav","ol","p","pre",
    "section","table","tbody","td","tfoot","th","thead","tr","ul",0
};
static const char* rawtext_tags[] = {
    "script","style","title","textarea","xmp","iframe","noembed","noframes",0
};

static int in_list(const char* tag, int len, const char** list) {
    for (int i = 0; list[i]; i++) {
        int j = 0;
        int ok = 1;
        while (list[i][j]) {
            if (j >= len || lc((unsigned char)tag[j]) != list[i][j]) { ok = 0; break; }
            j++;
        }
        if (ok && j == len) return 1;
    }
    return 0;
}

int dom_is_void(const char* tag, int len)    { return in_list(tag, len, void_tags); }
int dom_is_block(const char* tag, int len)   { return in_list(tag, len, block_tags); }
int dom_is_rawtext(const char* tag, int len) { return in_list(tag, len, rawtext_tags); }