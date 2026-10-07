#include "wdom.h"
#include "wcommon.h"

// ---- Atoms --------------------------------------------------------------

static uint32_t hash_name(const char* s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static int atoms_grow_hash(struct watoms* t) {
    int nc = t->hcap ? t->hcap * 2 : 1024;
    int32_t* nh = (int32_t*)w_malloc(nc * sizeof(int32_t));
    if (!nh) return 0;
    for (int i = 0; i < nc; i++) nh[i] = -1;
    for (int a = 1; a < t->count; a++) {
        uint32_t h = hash_name(t->names + t->off[a], t->len[a]) & (nc - 1);
        while (nh[h] >= 0) h = (h + 1) & (nc - 1);
        nh[h] = a;
    }
    w_free(t->hash);
    t->hash = nh;
    t->hcap = nc;
    return 1;
}

int watom_find(const struct watoms* t, const char* s, int len) {
    if (!t->hcap || len <= 0 || len > 255) return 0;
    uint32_t h = hash_name(s, len) & (t->hcap - 1);
    while (t->hash[h] >= 0) {
        int a = t->hash[h];
        if (t->len[a] == len && !memcmp(t->names + t->off[a], s, len)) return a;
        h = (h + 1) & (t->hcap - 1);
    }
    return 0;
}

int watom_intern(struct watoms* t, const char* s, int len) {
    if (len <= 0) return 0;
    if (len > 255) len = 255;
    int a = watom_find(t, s, len);
    if (a) return a;
    if (t->count >= 65535) return 0;
    if (t->count * 2 >= t->hcap && !atoms_grow_hash(t)) return 0;
    if (t->count == t->cap) {
        int nc = t->cap ? t->cap * 2 : 512;
        uint32_t* no = (uint32_t*)w_realloc(t->off, nc * sizeof(uint32_t));
        if (!no) return 0;
        t->off = no;
        uint16_t* nl = (uint16_t*)w_realloc(t->len, nc * sizeof(uint16_t));
        if (!nl) return 0;
        t->len = nl;
        t->cap = nc;
    }
    if (t->nlen + len + 1 > t->ncap) {
        uint32_t nc = t->ncap ? t->ncap * 2 : 8192;
        while (nc < t->nlen + len + 1) nc *= 2;
        char* nn = (char*)w_realloc(t->names, nc);
        if (!nn) return 0;
        t->names = nn;
        t->ncap = nc;
    }
    a = t->count++;
    t->off[a] = t->nlen;
    t->len[a] = (uint16_t)len;
    memcpy(t->names + t->nlen, s, len);
    t->names[t->nlen + len] = 0;
    t->nlen += len + 1;
    uint32_t h = hash_name(s, len) & (t->hcap - 1);
    while (t->hash[h] >= 0) h = (h + 1) & (t->hcap - 1);
    t->hash[h] = a;
    return a;
}

const char* watom_name(const struct watoms* t, int atom, int* len) {
    if (atom <= 0 || atom >= t->count) { if (len) *len = 0; return ""; }
    if (len) *len = t->len[atom];
    return t->names + t->off[atom];
}

static int atoms_init(struct watoms* t) {
    memset(t, 0, sizeof(*t));
    // atom 0 = empty name
    t->count = 1;
    if (!atoms_grow_hash(t)) return 0;
    t->cap = 512;
    t->off = (uint32_t*)w_malloc(t->cap * sizeof(uint32_t));
    t->len = (uint16_t*)w_malloc(t->cap * sizeof(uint16_t));
    t->ncap = 8192;
    t->names = (char*)w_malloc(t->ncap);
    if (!t->off || !t->len || !t->names) return 0;
    t->off[0] = 0; t->len[0] = 0; t->names[0] = 0; t->nlen = 1;
    for (int i = 1; i < ATOM_KNOWN_COUNT; i++) {
        const char* s = atom_known_names[i];
        int a = watom_intern(t, s, (int)strlen(s));
        if (a != i) return 0; // known ids must be stable
    }
    return 1;
}

static void atoms_free(struct watoms* t) {
    w_free(t->names); w_free(t->off); w_free(t->len); w_free(t->hash);
    memset(t, 0, sizeof(*t));
}

// ---- Document -------------------------------------------------------------

struct wdom* wdom_new(void) {
    struct wdom* d = (struct wdom*)w_calloc(1, sizeof(struct wdom));
    if (!d) return 0;
    if (!atoms_init(&d->atoms)) { atoms_free(&d->atoms); w_free(d); return 0; }
    d->ncap = 1024;
    d->n = (struct wnode*)w_malloc(d->ncap * sizeof(struct wnode));
    d->acap = 1024;
    d->a = (struct wattr*)w_malloc(d->acap * sizeof(struct wattr));
    d->tcap = 16384;
    d->text = (char*)w_malloc(d->tcap);
    d->clscap = 1024;
    d->classes = (uint16_t*)w_malloc(d->clscap * sizeof(uint16_t));
    if (!d->n || !d->a || !d->text || !d->classes) { wdom_free(d); return 0; }
    d->html = d->head = d->body = -1;
    // node 0: document
    struct wnode* r = &d->n[0];
    memset(r, 0, sizeof(*r));
    r->type = WN_DOC;
    r->parent = r->first = r->last = r->next = r->prev = r->attr = -1;
    r->aux = -1;
    d->nn = 1;
    d->root = 0;
    return d;
}

void wdom_free(struct wdom* d) {
    if (!d) return;
    w_free(d->n); w_free(d->a); w_free(d->text); w_free(d->classes);
    atoms_free(&d->atoms);
    w_free(d);
}

static int new_node(struct wdom* d, int type) {
    if (d->nn == d->ncap) {
        int nc = d->ncap * 2;
        struct wnode* nn = (struct wnode*)w_realloc(d->n, nc * sizeof(struct wnode));
        if (!nn) { d->oom = 1; return -1; }
        d->n = nn;
        d->ncap = nc;
    }
    int i = d->nn++;
    struct wnode* n = &d->n[i];
    memset(n, 0, sizeof(*n));
    n->type = (uint8_t)type;
    n->parent = n->first = n->last = n->next = n->prev = n->attr = -1;
    n->aux = -1;
    return i;
}

static int text_put(struct wdom* d, const char* s, int len, uint32_t* off) {
    if (d->tlen + (uint32_t)len > d->tcap) {
        uint32_t nc = d->tcap * 2;
        while (nc < d->tlen + (uint32_t)len) nc *= 2;
        char* nt = (char*)w_realloc(d->text, nc);
        if (!nt) { d->oom = 1; return 0; }
        d->text = nt;
        d->tcap = nc;
    }
    *off = d->tlen;
    if (len) memcpy(d->text + d->tlen, s, len);
    d->tlen += len;
    return 1;
}

int wdom_create_element(struct wdom* d, int ns, int tag_atom) {
    int i = new_node(d, WN_ELEM);
    if (i < 0) return -1;
    d->n[i].ns = (uint8_t)ns;
    d->n[i].tag = (uint16_t)tag_atom;
    return i;
}

static int is_ws_run(const char* s, int len) {
    for (int i = 0; i < len; i++) if (!w_isspace((unsigned char)s[i])) return 0;
    return 1;
}

int wdom_create_text(struct wdom* d, const char* s, int len) {
    int i = new_node(d, WN_TEXT);
    if (i < 0) return -1;
    uint32_t off;
    if (!text_put(d, s, len, &off)) return i;
    d->n[i].text = off;
    d->n[i].tlen = (uint32_t)len;
    if (is_ws_run(s, len)) d->n[i].flags |= WNF_WS;
    return i;
}

int wdom_create_comment(struct wdom* d, const char* s, int len) {
    int i = new_node(d, WN_COMMENT);
    if (i < 0) return -1;
    uint32_t off;
    if (!text_put(d, s, len, &off)) return i;
    d->n[i].text = off;
    d->n[i].tlen = (uint32_t)len;
    return i;
}

void wdom_remove(struct wdom* d, int c) {
    if (c < 0) return;
    struct wnode* n = &d->n[c];
    int p = n->parent;
    if (p < 0) return;
    if (n->prev >= 0) d->n[n->prev].next = n->next; else d->n[p].first = n->next;
    if (n->next >= 0) d->n[n->next].prev = n->prev; else d->n[p].last = n->prev;
    n->parent = n->prev = n->next = -1;
}

void wdom_insert_before(struct wdom* d, int p, int c, int ref) {
    if (p < 0 || c < 0 || p == c) return;
    if (d->n[c].parent >= 0) wdom_remove(d, c);
    struct wnode* n = &d->n[c];
    n->parent = p;
    if (ref < 0 || d->n[ref].parent != p) {
        n->prev = d->n[p].last;
        n->next = -1;
        if (n->prev >= 0) d->n[n->prev].next = c; else d->n[p].first = c;
        d->n[p].last = c;
    } else {
        n->next = ref;
        n->prev = d->n[ref].prev;
        if (n->prev >= 0) d->n[n->prev].next = c; else d->n[p].first = c;
        d->n[ref].prev = c;
    }
}

void wdom_append(struct wdom* d, int p, int c) { wdom_insert_before(d, p, c, -1); }

// Extend text node t with s (moving its data to the arena tail if needed).
static void text_extend(struct wdom* d, int t, const char* s, int len) {
    struct wnode* n = &d->n[t];
    int at_tail = (n->text + n->tlen == d->tlen);
    uint32_t need = (uint32_t)len + (at_tail ? 0 : n->tlen);
    if (d->tlen + need > d->tcap) {
        uint32_t nc = d->tcap * 2;
        while (nc < d->tlen + need) nc *= 2;
        char* nt = (char*)w_realloc(d->text, nc);
        if (!nt) { d->oom = 1; return; }
        d->text = nt;
        d->tcap = nc;
    }
    if (!at_tail) { // relocate the node's data to the arena tail
        memmove(d->text + d->tlen, d->text + n->text, n->tlen);
        n->text = d->tlen;
        d->tlen += n->tlen;
    }
    memcpy(d->text + d->tlen, s, len);
    d->tlen += (uint32_t)len;
    n->tlen += (uint32_t)len;
    if ((n->flags & WNF_WS) && !is_ws_run(s, len)) n->flags &= ~WNF_WS;
}

void wdom_append_text(struct wdom* d, int p, const char* s, int len) {
    if (len <= 0 || p < 0) return;
    int l = d->n[p].last;
    if (l >= 0 && d->n[l].type == WN_TEXT) { text_extend(d, l, s, len); return; }
    int t = wdom_create_text(d, s, len);
    if (t >= 0) wdom_append(d, p, t);
}

void wdom_insert_text_before(struct wdom* d, int p, int ref, const char* s, int len) {
    if (ref < 0) { wdom_append_text(d, p, s, len); return; }
    int pv = d->n[ref].prev;
    if (pv >= 0 && d->n[pv].type == WN_TEXT) { text_extend(d, pv, s, len); return; }
    int t = wdom_create_text(d, s, len);
    if (t >= 0) wdom_insert_before(d, p, t, ref);
}

// ---- Attributes -------------------------------------------------------------

static int find_attr(const struct wdom* d, int el, int name) {
    for (int a = d->n[el].attr; a >= 0; a = d->a[a].next)
        if (d->a[a].name == name) return a;
    return -1;
}

void wdom_set_attr(struct wdom* d, int el, int name, const char* v, int vlen) {
    if (el < 0 || !name) return;
    int a = find_attr(d, el, name);
    uint32_t off;
    if (a >= 0 && (uint32_t)vlen <= d->a[a].vlen) {
        memcpy(d->text + d->a[a].val, v, vlen);
        d->a[a].vlen = (uint32_t)vlen;
    } else {
        if (!text_put(d, v, vlen, &off)) return;
        if (a < 0) {
            if (d->na == d->acap) {
                int nc = d->acap * 2;
                struct wattr* na = (struct wattr*)w_realloc(d->a, nc * sizeof(struct wattr));
                if (!na) { d->oom = 1; return; }
                d->a = na;
                d->acap = nc;
            }
            a = d->na++;
            d->a[a].name = (uint16_t)name;
            d->a[a].next = -1;
            // append at the tail to keep source order
            int t = d->n[el].attr;
            if (t < 0) d->n[el].attr = a;
            else { while (d->a[t].next >= 0) t = d->a[t].next; d->a[t].next = a; }
        }
        d->a[a].val = off;
        d->a[a].vlen = (uint32_t)vlen;
    }
    if (name == A_class || name == A_id) wdom_index_attrs(d, el);
}

int wdom_has_attr(const struct wdom* d, int el, int name) {
    return el >= 0 && find_attr(d, el, name) >= 0;
}

const char* wdom_attr(const struct wdom* d, int el, int name, int* len) {
    if (el < 0 || d->n[el].type != WN_ELEM) return 0;
    int a = find_attr(d, el, name);
    if (a < 0) return 0;
    if (len) *len = (int)d->a[a].vlen;
    return d->text + d->a[a].val;
}

int wdom_attr_copy(const struct wdom* d, int el, int name, char* out, int cap) {
    int len;
    const char* v = wdom_attr(d, el, name, &len);
    if (!v) { if (cap > 0) out[0] = 0; return -1; }
    if (len > cap - 1) len = cap - 1;
    memcpy(out, v, len);
    out[len] = 0;
    return len;
}

const char* wdom_attr_s(const struct wdom* d, int el, const char* name, int* len) {
    char low[64];
    int n = 0;
    while (name[n] && n < 63) { low[n] = (char)w_lower((unsigned char)name[n]); n++; }
    int at = watom_find(&d->atoms, low, n);
    if (!at) return 0;
    return wdom_attr(d, el, at, len);
}

void wdom_index_attrs(struct wdom* d, int el) {
    struct wnode* n = &d->n[el];
    int len;
    const char* v = wdom_attr(d, el, A_id, &len);
    n->id = v && len ? (uint16_t)watom_intern(&d->atoms, v, len) : 0;
    n = &d->n[el];
    n->nclass = 0;
    v = wdom_attr(d, el, A_class, &len);
    if (!v) return;
    // copy the value: interning may realloc nothing in text, but be safe
    char buf[512];
    if (len > 511) len = 511;
    memcpy(buf, v, len);
    uint32_t start = d->ncls;
    int cnt = 0;
    for (int i = 0; i < len && cnt < 64;) {
        while (i < len && w_isspace((unsigned char)buf[i])) i++;
        int s = i;
        while (i < len && !w_isspace((unsigned char)buf[i])) i++;
        if (i > s) {
            int at = watom_intern(&d->atoms, buf + s, i - s);
            if (!at) continue;
            if (d->ncls == d->clscap) {
                uint32_t nc = d->clscap * 2;
                uint16_t* ncs = (uint16_t*)w_realloc(d->classes, nc * sizeof(uint16_t));
                if (!ncs) { d->oom = 1; break; }
                d->classes = ncs;
                d->clscap = nc;
            }
            d->classes[d->ncls++] = (uint16_t)at;
            cnt++;
        }
    }
    n = &d->n[el];
    n->text = start;
    n->nclass = (uint16_t)cnt;
}

int wdom_has_class(const struct wdom* d, int el, int cls) {
    const struct wnode* n = &d->n[el];
    const uint16_t* c = d->classes + n->text;
    for (int i = 0; i < n->nclass; i++) if (c[i] == cls) return 1;
    return 0;
}

// ---- Queries ----------------------------------------------------------------

const char* wdom_tag_name(const struct wdom* d, int el, int* len) {
    if (el < 0 || d->n[el].type != WN_ELEM) { if (len) *len = 0; return ""; }
    return watom_name(&d->atoms, d->n[el].tag, len);
}

int wdom_next(const struct wdom* d, int node, int scope) {
    if (d->n[node].first >= 0) return d->n[node].first;
    while (node >= 0 && node != scope) {
        if (d->n[node].next >= 0) return d->n[node].next;
        node = d->n[node].parent;
    }
    return -1;
}

int wdom_find_id(const struct wdom* d, const char* id) {
    int a = watom_find(&d->atoms, id, (int)strlen(id));
    if (!a) return -1;
    for (int i = d->n[0].first; i >= 0; i = wdom_next(d, i, 0))
        if (d->n[i].type == WN_ELEM && d->n[i].id == a) return i;
    return -1;
}

int wdom_first_tag(const struct wdom* d, int tag) {
    for (int i = d->n[0].first; i >= 0; i = wdom_next(d, i, 0))
        if (d->n[i].type == WN_ELEM && d->n[i].tag == tag) return i;
    return -1;
}

int wdom_text_content(const struct wdom* d, int node, char* out, int cap) {
    int o = 0;
    if (cap <= 0) return 0;
    if (d->n[node].type == WN_TEXT) {
        int l = (int)d->n[node].tlen;
        if (l > cap - 1) l = cap - 1;
        memcpy(out, d->text + d->n[node].text, l);
        out[l] = 0;
        return l;
    }
    for (int i = d->n[node].first; i >= 0; i = wdom_next(d, i, node)) {
        if (d->n[i].type != WN_TEXT) continue;
        int l = (int)d->n[i].tlen;
        if (o + l > cap - 1) l = cap - 1 - o;
        if (l <= 0) break;
        memcpy(out + o, d->text + d->n[i].text, l);
        o += l;
    }
    out[o] = 0;
    return o;
}

void wdom_set_text_content(struct wdom* d, int el, const char* s, int len) {
    while (d->n[el].first >= 0) {
        int c = d->n[el].first;
        wdom_remove(d, c);
        d->n[c].flags |= WNF_REMOVED;
    }
    if (len > 0) wdom_append_text(d, el, s, len);
}

// ---- Debug dump (html5lib tree-construction test format) --------------------

static void dump_rec(const struct wdom* d, int node, int depth, struct wbuf* b) {
    for (int c = d->n[node].first; c >= 0; c = d->n[c].next) {
        const struct wnode* n = &d->n[c];
        wbuf_put(b, "| ", 2);
        for (int i = 0; i < depth; i++) wbuf_put(b, "  ", 2);
        if (n->type == WN_ELEM) {
            int l;
            const char* nm = watom_name(&d->atoms, n->tag, &l);
            wbuf_putc(b, '<');
            if (n->ns == NS_SVG) wbuf_put(b, "svg ", 4);
            else if (n->ns == NS_MATH) wbuf_put(b, "math ", 5);
            wbuf_put(b, nm, l);
            wbuf_put(b, ">\n", 2);
            // attributes sorted by name (simple insertion order sort)
            int ids[128], na = 0;
            for (int a = n->attr; a >= 0 && na < 128; a = d->a[a].next) ids[na++] = a;
            for (int i = 1; i < na; i++) {
                int k = ids[i], j = i - 1;
                int kl; const char* kn = watom_name(&d->atoms, d->a[k].name, &kl);
                while (j >= 0) {
                    int jl; const char* jn = watom_name(&d->atoms, d->a[ids[j]].name, &jl);
                    int m = kl < jl ? kl : jl, cmp = memcmp(jn, kn, m);
                    if (cmp < 0 || (cmp == 0 && jl <= kl)) break;
                    ids[j + 1] = ids[j]; j--;
                }
                ids[j + 1] = k;
            }
            for (int i = 0; i < na; i++) {
                const struct wattr* at = &d->a[ids[i]];
                wbuf_put(b, "| ", 2);
                for (int k = 0; k <= depth; k++) wbuf_put(b, "  ", 2);
                int al; const char* an = watom_name(&d->atoms, at->name, &al);
                wbuf_put(b, an, al);
                wbuf_put(b, "=\"", 2);
                wbuf_put(b, d->text + at->val, (int)at->vlen);
                wbuf_put(b, "\"\n", 2);
            }
            // (template contents are ordinary children in this DOM; no
            // html5lib "content" wrapper line)
            dump_rec(d, c, depth + 1, b);
        } else if (n->type == WN_TEXT) {
            wbuf_putc(b, '"');
            wbuf_put(b, d->text + n->text, (int)n->tlen);
            wbuf_put(b, "\"\n", 2);
        } else if (n->type == WN_COMMENT) {
            wbuf_put(b, "<!-- ", 5);
            wbuf_put(b, d->text + n->text, (int)n->tlen);
            wbuf_put(b, " -->\n", 5);
        }
    }
}

char* wdom_dump(const struct wdom* d, int* out_len) {
    struct wbuf b = { 0, 0, 0 };
    dump_rec(d, 0, 0, &b);
    wbuf_putc(&b, 0);
    *out_len = b.len - 1;
    return b.p;
}

void wdom_remove_attr(struct wdom* d, int el, int name) {
    if (el < 0 || !name) return;
    int prev = -1;
    for (int a = d->n[el].attr; a >= 0; prev = a, a = d->a[a].next) {
        if (d->a[a].name != name) continue;
        if (prev < 0) d->n[el].attr = d->a[a].next;
        else d->a[prev].next = d->a[a].next;
        if (name == A_class || name == A_id) wdom_index_attrs(d, el);
        return;
    }
}
