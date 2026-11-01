// Box tree construction + anonymous box fixups.
#include "lay_int.h"
#include "cstyles.h"

const struct wstyle* lay_anon_style(struct wlayout* L, const struct wstyle* parent, int display) {
    if (L->anon_used % 128 == 0) {
        if (L->nanon_chunk >= 512) return parent;
        struct wstyle* c = (struct wstyle*)w_malloc(128 * sizeof(struct wstyle));
        if (!c) return parent;
        L->anon_chunk[L->nanon_chunk++] = c;
    }
    struct wstyle* s = &L->anon_chunk[L->nanon_chunk - 1][L->anon_used % 128];
    L->anon_used++;
    css_anon_style(parent, display, s);
    return s;
}

const char* lay_box_raw(struct wlayout* L, int bi, int* len) {
    struct lbox* b = &L->b[bi];
    *len = (int)b->tl;
    if (b->flags & BF_RAWTEXT) return L->text.p + b->t0;
    if (b->node >= 0 && L->d->n[b->node].type == WN_TEXT) {
        *len = (int)L->d->n[b->node].tlen;
        return L->d->text + L->d->n[b->node].text;
    }
    *len = 0;
    return "";
}

static int is_block_level(const struct lbox* b) {
    if (b->flags & (BF_ABS | BF_FLOAT)) return 0;
    return b->kind == LB_BLOCK && !(b->flags & BF_INLINE_LEVEL) ? 1 :
           (b->kind == LB_REPLACED && !(b->flags & BF_INLINE_LEVEL));
}

static int is_inline_level(const struct lbox* b) {
    if (b->flags & (BF_ABS | BF_FLOAT)) return 0;
    return !is_block_level(b);
}

static int collapsible_ws_text(struct wlayout* L, int bi) {
    const struct lbox* b = &L->b[bi];
    if (b->kind != LB_TEXT) return 0;
    int ws = b->st ? b->st->white_space : WS_NORMAL;
    if (ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_BREAK_SPACES) return 0;
    int len;
    const char* s = lay_box_raw(L, bi, &len);
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return 0;
        if (c == '\n' && ws == WS_PRE_LINE) return 0;
    }
    return 1;
}

// detach child list of box into an array (returns count)
static int children_of(struct wlayout* L, int bi, int** out) {
    int n = 0;
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) n++;
    if (!n) { *out = 0; return 0; }
    int* a = (int*)w_malloc(n * sizeof(int));
    if (!a) { *out = 0; return 0; }
    n = 0;
    for (int c = L->b[bi].first; c >= 0; c = L->b[c].next) a[n++] = c;
    *out = a;
    return n;
}

static void clear_children(struct wlayout* L, int bi) { L->b[bi].first = L->b[bi].last = -1; }

static int anon_block(struct wlayout* L, int parent, int fc) {
    const struct wstyle* ps = L->b[parent].st;
    int disp = fc == FC_TABLE ? D_TABLE : fc == FC_ROWGROUP ? D_TABLE_ROW_GROUP :
               fc == FC_ROW ? D_TABLE_ROW : fc == FC_CELL ? D_TABLE_CELL : D_BLOCK;
    const struct wstyle* st = lay_anon_style(L, ps, disp);
    int a = lb_new(L, -1, st, LB_BLOCK);
    if (a < 0) return -1;
    L->b[a].fc = (uint8_t)fc;
    L->b[a].flags |= BF_ANON | (L->b[parent].flags & BF_FIXED);
    return a;
}

static int is_table_part(const struct lbox* b) {
    return b->kind == LB_BLOCK && !(b->flags & (BF_ABS | BF_FLOAT)) &&
           (b->fc == FC_ROWGROUP || b->fc == FC_ROW || b->fc == FC_CELL);
}

// Fix the children of block container bi.
static void fixup_flow(struct wlayout* L, int bi) {
    int* ch;
    int n = children_of(L, bi, &ch);
    if (!n) return;
    int any_block = 0, any_inline = 0, any_tpart = 0;
    for (int i = 0; i < n; i++) {
        struct lbox* c = &L->b[ch[i]];
        if (is_table_part(c)) any_tpart = 1;
        else if (is_block_level(c)) any_block = 1;
        else if (is_inline_level(c) && !collapsible_ws_text(L, ch[i])) any_inline = 1;
    }
    if (any_tpart) {
        // wrap runs of table parts into anonymous tables
        clear_children(L, bi);
        for (int i = 0; i < n;) {
            if (is_table_part(&L->b[ch[i]])) {
                int t = anon_block(L, bi, FC_TABLE);
                if (t < 0) break;
                while (i < n && (is_table_part(&L->b[ch[i]]) || collapsible_ws_text(L, ch[i]))) {
                    lb_append(L, t, ch[i]);
                    i++;
                }
                lb_append(L, bi, t);
                continue;
            }
            lb_append(L, bi, ch[i]);
            i++;
        }
        w_free(ch);
        n = children_of(L, bi, &ch);
        any_block = 1;
        any_inline = 0;
        for (int i = 0; i < n; i++)
            if (is_inline_level(&L->b[ch[i]]) && !collapsible_ws_text(L, ch[i])) any_inline = 1;
    }
    if (!any_block) {
        if (any_inline) L->b[bi].flags |= BF_IFC;
        w_free(ch);
        return;
    }
    // mixed (or block only): wrap inline runs, drop whitespace-only runs
    clear_children(L, bi);
    for (int i = 0; i < n;) {
        struct lbox* c = &L->b[ch[i]];
        if (is_block_level(c) || is_table_part(c)) { lb_append(L, bi, ch[i]); i++; continue; }
        // collect a run of non-block children
        int j = i, real = 0;
        while (j < n && !is_block_level(&L->b[ch[j]]) && !is_table_part(&L->b[ch[j]])) {
            if (is_inline_level(&L->b[ch[j]]) && !collapsible_ws_text(L, ch[j])) real = 1;
            j++;
        }
        if (real) {
            int a = anon_block(L, bi, FC_FLOW);
            if (a < 0) { w_free(ch); return; }
            L->b[a].flags |= BF_IFC;
            for (int k = i; k < j; k++) lb_append(L, a, ch[k]);
            lb_append(L, bi, a);
        } else {
            for (int k = i; k < j; k++)
                if (L->b[ch[k]].flags & (BF_ABS | BF_FLOAT)) lb_append(L, bi, ch[k]);
        }
        i = j;
    }
    w_free(ch);
}

// flex/grid containers: every in-flow child is an item; text runs become
// anonymous block items
static void fixup_items(struct wlayout* L, int bi) {
    int* ch;
    int n = children_of(L, bi, &ch);
    if (!n) return;
    clear_children(L, bi);
    for (int i = 0; i < n;) {
        struct lbox* c = &L->b[ch[i]];
        if ((c->flags & (BF_ABS)) || c->kind == LB_BLOCK || c->kind == LB_REPLACED) {
            if (c->kind == LB_BLOCK || c->kind == LB_REPLACED) {
                c->flags &= ~(BF_INLINE_LEVEL | BF_FLOAT);
            }
            lb_append(L, bi, ch[i]);
            i++;
            continue;
        }
        int j = i, real = 0;
        while (j < n && !(L->b[ch[j]].flags & BF_ABS) && L->b[ch[j]].kind != LB_BLOCK &&
               L->b[ch[j]].kind != LB_REPLACED) {
            if (!collapsible_ws_text(L, ch[j])) real = 1;
            j++;
        }
        if (real) {
            int a = anon_block(L, bi, FC_FLOW);
            if (a < 0) break;
            L->b[a].flags |= BF_IFC;
            for (int k = i; k < j; k++) lb_append(L, a, ch[k]);
            lb_append(L, bi, a);
        }
        i = j;
    }
    w_free(ch);
}

static void fixup_table(struct wlayout* L, int bi, int fc) {
    int* ch;
    int n = children_of(L, bi, &ch);
    if (!n) return;
    clear_children(L, bi);
    int want = fc == FC_TABLE ? FC_ROWGROUP : fc == FC_ROWGROUP ? FC_ROW : FC_CELL;
    for (int i = 0; i < n;) {
        struct lbox* c = &L->b[ch[i]];
        if (c->kind == LB_BLOCK && (c->fc == want || (fc == FC_TABLE && (c->fc == FC_CAPTION ||
            c->fc == FC_COLUMN)) || (c->flags & BF_ABS))) {
            lb_append(L, bi, ch[i]);
            i++;
            continue;
        }
        if (collapsible_ws_text(L, ch[i])) { i++; continue; }
        // wrap a run of misparented children
        int a = anon_block(L, bi, want);
        if (a < 0) break;
        int j = i;
        while (j < n) {
            struct lbox* d = &L->b[ch[j]];
            if (d->kind == LB_BLOCK && (d->fc == want || (fc == FC_TABLE && (d->fc == FC_CAPTION || d->fc == FC_COLUMN))))
                break;
            lb_append(L, a, ch[j]);
            j++;
        }
        lb_append(L, bi, a);
        if (want == FC_ROWGROUP) fixup_table(L, a, FC_ROWGROUP);
        else if (want == FC_ROW) fixup_table(L, a, FC_ROW);
        else fixup_flow(L, a);
        i = j;
    }
    w_free(ch);
}

static void fixup(struct wlayout* L, int bi) {
    struct lbox* b = &L->b[bi];
    if (b->kind != LB_BLOCK) return;
    switch (b->fc) {
    case FC_FLEX: case FC_GRID: fixup_items(L, bi); break;
    case FC_TABLE: case FC_ROWGROUP: case FC_ROW: fixup_table(L, bi, b->fc); break;
    case FC_COLUMN: clear_children(L, bi); break;
    default: fixup_flow(L, bi); break;
    }
}

// ---- list markers -------------------------------------------------------------

static int roman(int n, char* out, int upper) {
    static const int V[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char* const S[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    int o = 0;
    if (n <= 0 || n > 3999) return 0;
    for (int i = 0; i < 13; i++)
        while (n >= V[i]) {
            for (const char* p = S[i]; *p; p++) out[o++] = upper ? (char)(*p - 32) : *p;
            n -= V[i];
        }
    return o;
}

static int marker_text(const struct wstyle* st, int ord, char* out, const struct wstyleset* ss) {
    int o = 0;
    if (st->list_style_type >= LS_CSTYLE0 && st->cstyle_idx >= 0) {
        o = cstyle_format(st->cstyle_idx, ord, out);
        if (o > 0) { out[o++] = '.'; out[o++] = ' '; return o; }
        goto dec;
    }
    switch (st->list_style_type) {
    case LS_NONE: return 0;
    case LS_DISC: o = w_utf8_enc(0x2022, out); break;
    case LS_CIRCLE: o = w_utf8_enc(0x25E6, out); break;
    case LS_SQUARE: o = w_utf8_enc(0x25AA, out); break;
    case LS_DISCLOSURE_OPEN: o = w_utf8_enc(0x25BE, out); break;
    case LS_DISCLOSURE_CLOSED: o = w_utf8_enc(0x25B8, out); break;
    case LS_STRING: {
        const char* s = css_str(ss, st->list_marker, 0);
        int n = 0;
        while (s[n] && n < 60) { out[n] = s[n]; n++; }
        return n;
    }
    case LS_LOWER_ALPHA: case LS_UPPER_ALPHA: {
        char tmp[16]; int t = 0, v = ord;
        if (v <= 0) v = 1;
        while (v > 0 && t < 15) { v--; tmp[t++] = (char)((st->list_style_type == LS_UPPER_ALPHA ? 'A' : 'a') + v % 26); v /= 26; }
        while (t) out[o++] = tmp[--t];
        out[o++] = '.';
        break;
    }
    case LS_LOWER_ROMAN: case LS_UPPER_ROMAN:
        o = roman(ord, out, st->list_style_type == LS_UPPER_ROMAN);
        if (!o) goto dec;
        out[o++] = '.';
        break;
    case LS_LOWER_GREEK:
        if (ord >= 1 && ord <= 24) { o = w_utf8_enc(0x3B1 + ord - 1 + (ord > 17 ? 1 : 0), out); out[o++] = '.'; break; }
        goto dec;
    case LS_DECIMAL_LZ:
        if (ord >= 0 && ord < 10) out[o++] = '0';
        goto dec;
    default:
    dec: {
        char tmp[16]; int t = 0, v = ord < 0 ? -ord : ord;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 15);
        if (ord < 0) out[o++] = '-';
        while (t) out[o++] = tmp[--t];
        out[o++] = '.';
    }
    }
    out[o++] = ' ';
    return o;
}

// ---- CSS counters ---------------------------------------------------------------------
// CSS Lists 3 section 4: every element (and ::before/::after, which count as its first and last child) has a counter SET, a
// list of counter instances. It starts as a copy of the parent's set, then the preceding sibling's counters are appended
// unless the set already has a counter of that NAME; counter-reset instantiates (replacing a same-named counter that the
// element itself or a preceding sibling made), counter-increment/-set act on the innermost counter of the name. An instance
// has one value, changed in document order, so a set only needs to hold indices into L->ctr.

#define CTR_ITEMS 16
#define CSET_MAX 24

struct cset { int n; int ix[CSET_MAX]; };

struct ctr_item { const char* nm; int nl; int has_val; int32_t val; int rev; };

static int ctr_parse(const char* s, int len, struct ctr_item* it, int max) {
    int n = 0, i = 0;
    while (i < len && n < max) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
        if (i >= len) break;
        int st = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') { if (s[i] == '(') { while (i < len && s[i] != ')') i++; } i++; }
        if (i > len) i = len;
        const char* t = s + st; int tl = i - st;
        int is_num = (t[0] >= '0' && t[0] <= '9') || ((t[0] == '-' || t[0] == '+') && tl > 1 && t[1] >= '0' && t[1] <= '9');
        if (is_num) {
            if (n > 0 && !it[n - 1].has_val) {
                int neg = t[0] == '-', k = (t[0] == '-' || t[0] == '+') ? 1 : 0;
                int32_t v = 0;
                for (; k < tl && t[k] >= '0' && t[k] <= '9'; k++) v = v * 10 + (t[k] - '0');
                it[n - 1].val = neg ? -v : v; it[n - 1].has_val = 1;
            }
            continue;
        }
        it[n].has_val = 0; it[n].val = 0; it[n].rev = 0;
        if (tl > 9 && w_ieq_prefix(t, tl, "reversed(") && t[tl - 1] == ')') { it[n].nm = t + 9; it[n].nl = tl - 10; it[n].rev = 1; }
        else { it[n].nm = t; it[n].nl = tl; }
        if (it[n].nl <= 0 || it[n].nl > 27 || w_ieq(it[n].nm, it[n].nl, "none")) continue;
        n++;
    }
    return n;
}

static int ctr_name_eq(const struct lctr* c, const char* nm, int nl) {
    int i = 0;
    for (; i < nl; i++) if (c->name[i] != nm[i]) return 0;
    return c->name[i] == 0;
}

// position in `set` of the innermost counter called nm, -1 when none
static int cset_find(const struct wlayout* L, const struct cset* set, const char* nm, int nl) {
    for (int i = set->n - 1; i >= 0; i--) if (ctr_name_eq(&L->ctr[set->ix[i]], nm, nl)) return i;
    return -1;
}

static void cset_add(struct cset* set, int ix) {
    if (set->n >= CSET_MAX) { for (int i = 1; i < CSET_MAX; i++) set->ix[i - 1] = set->ix[i]; set->n--; }
    set->ix[set->n++] = ix;
}

// element id as a counter creator: elements are their node index, pseudo-elements -(el * 4 + which) - 1
static int ctr_id(int el, int which) { return which ? -(el * 4 + which) - 1 : el; }
static int ctr_par(const struct wlayout* L, int id) {
    return id >= 0 ? L->d->n[id].parent : (-id - 1) / 4;
}

static void cset_inherit(const struct wlayout* L, struct cset* mine, const struct cset* parent, const struct cset* sib) {
    *mine = *parent;
    for (int i = 0; i < sib->n; i++) {
        const struct lctr* c = &L->ctr[sib->ix[i]];
        if (cset_find(L, mine, c->name, (int)strlen(c->name)) < 0) cset_add(mine, sib->ix[i]);
    }
}

static int ctr_new(struct wlayout* L, struct cset* mine, int creator, const char* nm, int nl, int32_t val, int rev) {
    // the innermost counter of that name made by this element or a preceding sibling is replaced
    int p = cset_find(L, mine, nm, nl);
    if (p >= 0) {
        int cr = L->ctr[mine->ix[p]].creator;
        if (cr == creator || ctr_par(L, cr) == ctr_par(L, creator)) {
            for (int k = p; k + 1 < mine->n; k++) mine->ix[k] = mine->ix[k + 1];
            mine->n--;
        }
    }
    if (L->nctr >= L->capctr) {
        int nc = L->capctr ? L->capctr * 2 : 64;
        struct lctr* nn = (struct lctr*)w_realloc(L->ctr, (size_t)nc * sizeof(struct lctr));
        if (!nn) return -1;
        L->ctr = nn; L->capctr = nc;
    }
    struct lctr* c = &L->ctr[L->nctr];
    for (int i = 0; i < nl; i++) c->name[i] = nm[i];
    c->name[nl] = 0;
    c->val = val; c->rev = (uint8_t)rev; c->creator = creator;
    cset_add(mine, L->nctr);
    return L->nctr++;
}

static struct lctr* ctr_get(struct wlayout* L, struct cset* mine, int creator, const char* nm, int nl) {
    int p = cset_find(L, mine, nm, nl);
    if (p >= 0) return &L->ctr[mine->ix[p]];
    int ix = ctr_new(L, mine, creator, nm, nl, 0, 0);
    return ix >= 0 ? &L->ctr[ix] : 0;
}

static int ctr_find_item(const struct wlayout* L, int32_t off, int32_t len, const char* name, struct ctr_item* out) {
    if (!off) return 0;
    struct ctr_item it[CTR_ITEMS];
    int n = ctr_parse(css_str(L->ss, off, len), len, it, CTR_ITEMS), nl = (int)strlen(name);
    for (int i = 0; i < n; i++) if (it[i].nl == nl && !memcmp(it[i].nm, name, (size_t)nl)) { *out = it[i]; return 1; }
    return 0;
}

static int ctr_mentions(const struct wlayout* L, int32_t off, int32_t len, const char* name) {
    struct ctr_item x;
    return ctr_find_item(L, off, len, name, &x);
}

static int increments_item(const struct wlayout* L, const struct wstyle* st) {
    return st->display == D_LIST_ITEM && !ctr_mentions(L, st->ctr_inc, st->ctr_inc_len, "list-item");
}

struct rev_walk { int32_t sum, last; int any, found; int32_t v; };

// Initial value of a reversed counter. The counter is sized so that, with no counter-set, the value after the last incrementing
// element is minus its increment (1 with the default -1); a counter-set (an li value) in the list fixes it instead: the value there is
// what the elements before it lead up to.
static void ctr_rev_walk(struct wlayout* L, int el, const char* name, struct rev_walk* w) {
    struct wdom* d = L->d;
    int isli = !strcmp(name, "list-item");
    for (int c = d->n[el].first; c >= 0 && !w->found; c = d->n[c].next) {
        if (d->n[c].type != WN_ELEM) continue;
        const struct wstyle* cs = css_style_of(L->ss, c);
        if (!cs || cs->display == D_NONE) continue;
        int isol = isli && d->n[c].ns == NS_HTML && (d->n[c].tag == T_ol || d->n[c].tag == T_ul || d->n[c].tag == T_menu);
        if (isol || ctr_mentions(L, cs->ctr_reset, cs->ctr_reset_len, name)) continue;   // a nested list / counter of its own
        struct ctr_item si, ii;
        int has_inc = 0; int32_t iv = -1;
        if (ctr_find_item(L, cs->ctr_inc, cs->ctr_inc_len, name, &ii)) { has_inc = 1; iv = ii.has_val ? ii.val : -1; }
        else if (isli && increments_item(L, cs)) has_inc = 1;
        if (has_inc && iv != 0) { w->sum += iv; w->last = iv; w->any = 1; }     // a zero increment does not size the counter
        if (ctr_find_item(L, cs->ctr_set, cs->ctr_set_len, name, &si)) { w->found = 1; w->v = (si.has_val ? si.val : 0) - w->sum; return; }
        if (isli && d->n[c].ns == NS_HTML && d->n[c].tag == T_li && (cs->list_value_set & 1)) { w->found = 1; w->v = cs->list_value - w->sum; return; }
        ctr_rev_walk(L, c, name, w);
    }
}

static int32_t ctr_rev_start(struct wlayout* L, int el, const char* name) {
    struct rev_walk w = { 0, 0, 0, 0, 0 };
    ctr_rev_walk(L, el, name, &w);
    if (w.found) return w.v;
    return w.any ? -w.last - w.sum : 1;
}

// Counter work of one element or pseudo-element: counter-reset, then counter-increment, then counter-set (plus the list-item
// counter of lists and list items). `id` = ctr_id(); `el` < 0 for a pseudo-element.
static void ctr_apply(struct wlayout* L, struct cset* mine, int id, const struct wstyle* st, int el) {
    struct wdom* d = L->d;
    struct ctr_item it[CTR_ITEMS];
    int is_list = el >= 0 && d->n[el].ns == NS_HTML && (d->n[el].tag == T_ol || d->n[el].tag == T_ul || d->n[el].tag == T_menu);
    if (is_list && !ctr_mentions(L, st->ctr_reset, st->ctr_reset_len, "list-item")) {
        int32_t v = 0; int rev = 0;
        if (d->n[el].tag == T_ol) {
            int has = st->list_value_set & 1;
            rev = (st->list_value_set & 2) != 0;
            if (rev) v = has ? st->list_value + 1 : ctr_rev_start(L, el, "list-item");
            else v = has ? st->list_value - 1 : 0;
        }
        ctr_new(L, mine, id, "list-item", 9, v, rev);
    }
    if (st->ctr_reset) {
        int n = ctr_parse(css_str(L->ss, st->ctr_reset, st->ctr_reset_len), st->ctr_reset_len, it, CTR_ITEMS);
        for (int i = 0; i < n; i++) {
            char nm[32]; memcpy(nm, it[i].nm, (size_t)it[i].nl); nm[it[i].nl] = 0;
            int32_t v = it[i].has_val ? it[i].val : (it[i].rev && el >= 0 ? ctr_rev_start(L, el, nm) : 0);
            ctr_new(L, mine, id, it[i].nm, it[i].nl, v, it[i].rev);
        }
    }
    if (st->ctr_inc) {
        int n = ctr_parse(css_str(L->ss, st->ctr_inc, st->ctr_inc_len), st->ctr_inc_len, it, CTR_ITEMS);
        for (int i = 0; i < n; i++) {
            struct lctr* c = ctr_get(L, mine, id, it[i].nm, it[i].nl);
            if (c) c->val += it[i].has_val ? it[i].val : (c->rev ? -1 : 1);
        }
    }
    if (increments_item(L, st)) {
        struct lctr* c = ctr_get(L, mine, id, "list-item", 9);
        if (c) c->val += c->rev ? -1 : 1;
    }
    if (st->ctr_set) {
        int n = ctr_parse(css_str(L->ss, st->ctr_set, st->ctr_set_len), st->ctr_set_len, it, CTR_ITEMS);
        for (int i = 0; i < n; i++) {
            struct lctr* c = ctr_get(L, mine, id, it[i].nm, it[i].nl);
            if (c) c->val = it[i].has_val ? it[i].val : 0;
        }
    }
    if (el >= 0 && d->n[el].ns == NS_HTML && d->n[el].tag == T_li && (st->list_value_set & 1) &&
        !ctr_mentions(L, st->ctr_set, st->ctr_set_len, "list-item")) {
        struct lctr* c = ctr_get(L, mine, id, "list-item", 9);
        if (c) c->val = st->list_value;
    }
}

static int ctr_format(int32_t v, const char* sty, int sl, char* out) {
    int o = 0;
    if (sl == 4 && !memcmp(sty, "none", 4)) return 0;
    if (sl >= 4 && sty[0] >= 'a' && sty[0] <= 'z') {
        int ci = cstyle_find(sty, sl);
        if (ci >= 0) return cstyle_format(ci, (int)v, out);
    }
    if (sl == 4 && !memcmp(sty, "disc", 4)) return w_utf8_enc(0x2022, out);
    if (sl == 6 && !memcmp(sty, "circle", 6)) return w_utf8_enc(0x25E6, out);
    if (sl == 6 && !memcmp(sty, "square", 6)) return w_utf8_enc(0x25AA, out);
    int up = (sl > 6 && !memcmp(sty, "upper-", 6));
    int lo = (sl > 6 && !memcmp(sty, "lower-", 6));
    const char* base = sty + ((up || lo) ? 6 : 0);
    int bl = sl - (int)(base - sty);
    if (up || lo) {
        if (bl == 5 && !memcmp(base, "roman", 5)) { o = roman(v, out, up); if (o) return o; }
        else if (bl == 5 && (!memcmp(base, "alpha", 5) || !memcmp(base, "latin", 5))) {
            if (v >= 1) {
                char tmp[16]; int t = 0, x = v;
                while (x > 0 && t < 15) { x--; tmp[t++] = (char)((up ? 'A' : 'a') + x % 26); x /= 26; }
                while (t) out[o++] = tmp[--t];
                return o;
            }
        } else if (bl == 5 && !memcmp(base, "greek", 5) && v >= 1 && v <= 24)
            return w_utf8_enc(0x3B1 + v - 1 + (v > 17 ? 1 : 0), out);
    }
    if (sl == 20 && !memcmp(sty, "decimal-leading-zero", 20) && v >= 0 && v < 10) out[o++] = '0';
    char tmp[16]; int t = 0; uint32_t x = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do { tmp[t++] = (char)('0' + x % 10); x /= 10; } while (x && t < 15);
    if (v < 0) out[o++] = '-';
    while (t) out[o++] = tmp[--t];
    return o;
}

// `content` text with counter() placeholders (see css_style.c ctr_func) -> final text, counters of `set`
static int expand_content(struct wlayout* L, const struct cset* set, const char* s, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 72; i++) {
        if ((unsigned char)s[i] != 1 || i + 1 >= n) { out[o++] = s[i]; continue; }
        int plural = s[i + 1] == 'C';
        int a = i + 2, e = a;
        while (e < n && s[e] != 2) e++;
        const char* nm = s + a; int nl = e - a;
        int sa = e + 1, se = sa;
        while (se < n && s[se] != 2) se++;
        const char* sty = s + sa; int sl = se - sa;
        int pa = se + 1, pe = pa;
        while (pe < n && s[pe] != 3) pe++;
        const char* sep = s + pa; int sepl = pe - pa;
        i = pe;
        if (!plural) {
            int p = cset_find(L, set, nm, nl);
            o += ctr_format(p >= 0 ? L->ctr[set->ix[p]].val : 0, sty, sl, out + o);
        } else {
            int first = 1, any = 0;
            for (int k = 0; k < set->n; k++) {
                if (!ctr_name_eq(&L->ctr[set->ix[k]], nm, nl)) continue;
                if (!first) for (int q = 0; q < sepl && o < cap - 72; q++) out[o++] = sep[q];
                first = 0; any = 1;
                o += ctr_format(L->ctr[set->ix[k]].val, sty, sl, out + o);
            }
            if (!any) o += ctr_format(0, sty, sl, out + o);
        }
    }
    return o;
}

// ---- building ---------------------------------------------------------------------

static int rk_of(struct wlayout* L, int el, int* is_replaced) {
    struct wdom* d = L->d;
    const struct wnode* n = &d->n[el];
    *is_replaced = 0;
    if (n->ns == NS_SVG) {
        if (n->tag == T_svg) { *is_replaced = 1; return RK_SVG; }
        return -1;
    }
    if (n->ns != NS_HTML) return -1;
    switch (n->tag) {
    case T_img: *is_replaced = 1; return RK_IMG;
    case T_video: *is_replaced = 1; return RK_VIDEO;
    case T_canvas: *is_replaced = 1; return RK_CANVAS;
    case T_iframe: *is_replaced = 1; return RK_IFRAME;
    case T_embed: case T_object: *is_replaced = 1; return RK_OBJECT;
    case T_select: *is_replaced = 1; return RK_SELECT;
    case T_textarea: *is_replaced = 1; return RK_TEXTAREA;
    case T_meter: case T_progress: *is_replaced = 1; return RK_METER;
    case T_input: {
        int tl;
        const char* t = wdom_attr(d, el, A_type, &tl);
        *is_replaced = 1;
        if (!t) return RK_INPUT_TEXT;
        if (w_ieq(t, tl, "checkbox")) return RK_CHECKBOX;
        if (w_ieq(t, tl, "radio")) return RK_RADIO;
        if (w_ieq(t, tl, "submit") || w_ieq(t, tl, "button") || w_ieq(t, tl, "reset") ||
            w_ieq(t, tl, "file")) return RK_INPUT_BUTTON;
        if (w_ieq(t, tl, "image")) return RK_IMG;
        if (w_ieq(t, tl, "range") || w_ieq(t, tl, "color")) return RK_METER;
        return RK_INPUT_TEXT;
    }
    }
    return -1;
}

static int fc_of(int display) {
    switch (display) {
    case D_FLEX: case D_INLINE_FLEX: return FC_FLEX;
    case D_GRID: case D_INLINE_GRID: return FC_GRID;
    case D_TABLE: case D_INLINE_TABLE: return FC_TABLE;
    case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP: case D_TABLE_FOOTER_GROUP: return FC_ROWGROUP;
    case D_TABLE_ROW: return FC_ROW;
    case D_TABLE_CELL: return FC_CELL;
    case D_TABLE_CAPTION: return FC_CAPTION;
    case D_TABLE_COLUMN: case D_TABLE_COLUMN_GROUP: return FC_COLUMN;
    }
    return FC_FLOW;
}

static int is_inline_display(int d) {
    return d == D_INLINE_BLOCK || d == D_INLINE_FLEX || d == D_INLINE_GRID || d == D_INLINE_TABLE;
}

static int text_box(struct wlayout* L, int parent, int node, const struct wstyle* st) {
    int t = lb_new(L, node, st, LB_TEXT);
    if (t < 0) return -1;
    L->b[t].flags |= L->b[parent].flags & BF_FIXED;
    lb_append(L, parent, t);
    return t;
}

static void build_children(struct wlayout* L, int el, int box, const struct cset* mine, struct cset* cprev);

// Pseudo-element box (::before = 1, ::after = 2) for element el into box.
static void build_pseudo(struct wlayout* L, int el, int box, int which, const struct cset* pset, struct cset* prev) {
    const struct wstyle* ps = css_pseudo_of(L->ss, el, which);
    if (!ps || !ps->content || ps->display == D_NONE) return;
    struct cset mine;
    cset_inherit(L, &mine, pset, prev);
    ctr_apply(L, &mine, ctr_id(el, which), ps, -1);
    *prev = mine;
    char cbuf[1024];
    int clen = ps->content_len > 0 ? expand_content(L, &mine, css_str(L->ss, ps->content, ps->content_len), ps->content_len, cbuf, (int)sizeof cbuf) : 0;
    if (ps->display == D_CONTENTS) {
        // no box: the generated text joins the parent as inline content,
        // styled by the pseudo (so flex fixups merge it with adjacent runs)
        if (clen > 0) {
            uint32_t off = (uint32_t)L->text.len;
            wbuf_put(&L->text, cbuf, clen);
            int t = lb_new(L, -1, ps, LB_TEXT);
            if (t >= 0) {
                L->b[t].t0 = off;
                L->b[t].tl = (uint32_t)clen;
                L->b[t].flags |= BF_RAWTEXT | (L->b[box].flags & BF_FIXED);
                lb_append(L, box, t);
            }
        }
        return;
    }
    int kind = (ps->display == D_INLINE) ? LB_INLINE : LB_BLOCK;
    int pb = lb_new(L, -1, ps, kind);
    if (pb < 0) return;
    struct lbox* b = &L->b[pb];
    b->pseudo = (uint8_t)which;
    b->node = -1;
    b->fc = (uint8_t)fc_of(ps->display);
    if (kind == LB_BLOCK && is_inline_display(ps->display)) b->flags |= BF_INLINE_LEVEL;
    if (ps->position == POS_ABSOLUTE || ps->position == POS_FIXED) b->flags |= BF_ABS;
    else if (ps->float_ != FL_NONE) b->flags |= BF_FLOAT;
    if (ps->position != POS_STATIC) b->flags |= BF_POSITIONED;
    b->flags |= (L->b[box].flags & BF_FIXED) | (ps->position == POS_FIXED ? BF_FIXED : 0);
    b->list_ord = el; // owning element (hit testing / debugging)
    if (clen > 0) {
        uint32_t off = (uint32_t)L->text.len;
        wbuf_put(&L->text, cbuf, clen);
        int t = lb_new(L, -1, ps, LB_TEXT);
        if (t >= 0) {
            L->b[t].t0 = off;
            L->b[t].tl = (uint32_t)clen;
            L->b[t].flags |= BF_RAWTEXT | (b->flags & BF_FIXED);
            lb_append(L, pb, t);
        }
    }
    lb_append(L, box, pb);
    if (kind == LB_BLOCK) fixup(L, pb);
}

static void build_el(struct wlayout* L, int el, int parent_box, const struct cset* pset, struct cset* prev) {
    struct wdom* d = L->d;
    const struct wstyle* st = css_style_of(L->ss, el);
    if (!st || st->display == D_NONE) return;
    if (L->depth > 400) return; // pathological nesting
    struct cset mine, cprev;
    cset_inherit(L, &mine, pset, prev);
    ctr_apply(L, &mine, el, st, el);
    *prev = mine;
    cprev.n = 0;
    if (st->display == D_CONTENTS) {
        L->depth++;
        build_children(L, el, parent_box, &mine, &cprev);
        L->depth--;
        return;
    }
    const struct wnode* n = &d->n[el];
    int is_rep = 0;
    int rk = rk_of(L, el, &is_rep);
    int kind;
    if (n->ns == NS_HTML && n->tag == T_br) kind = LB_BR;
    else if (n->ns == NS_HTML && n->tag == T_wbr) kind = LB_WBR;
    else if (is_rep) kind = LB_REPLACED;
    else if (st->display == D_INLINE || st->display == D_RUBY || st->display == D_RUBY_TEXT) kind = LB_INLINE;
    else kind = LB_BLOCK;
    if (n->ns == NS_SVG && !is_rep) return; // svg internals are painted by the svg box
    if (n->ns == NS_MATH && n->tag != T_math && st->display == D_BLOCK) kind = LB_INLINE;
    int bi = lb_new(L, el, st, kind);
    if (bi < 0) return;
    struct lbox* b = &L->b[bi];
    b->rk = (uint8_t)(rk < 0 ? 0 : rk);
    b->fc = (uint8_t)fc_of(st->display);
    if (kind == LB_REPLACED || kind == LB_BLOCK) {
        if (is_inline_display(st->display) || (kind == LB_REPLACED && st->display == D_INLINE))
            b->flags |= BF_INLINE_LEVEL;
    }
    if (kind == LB_BR || kind == LB_WBR || kind == LB_INLINE) b->flags |= BF_INLINE_LEVEL;
    if (st->position == POS_ABSOLUTE || st->position == POS_FIXED) b->flags |= BF_ABS;
    else if (st->float_ != FL_NONE && kind != LB_INLINE) b->flags |= BF_FLOAT;
    if (st->position != POS_STATIC) b->flags |= BF_POSITIONED;
    else if (st->contain & (CT_PAINT | CT_LAYOUT)) b->flags |= BF_POSITIONED;   // paint/layout containment: containing block for abspos descendants
    b->flags |= (L->b[parent_box].flags & BF_FIXED) | (st->position == POS_FIXED ? BF_FIXED : 0);
    if (L->node_box[el] < 0) L->node_box[el] = bi;
    // list items: ordinal + marker
    if (st->display == D_LIST_ITEM) {
        int lp = cset_find(L, &mine, "list-item", 9);
        int ord = lp >= 0 ? L->ctr[mine.ix[lp]].val : 0;
        b->list_ord = ord;
        const struct wstyle* ms = css_pseudo_of(L->ss, el, 3);
        char mt[80];
        int ml;
        struct cset mset = mine;                      // ::marker is the list item's first child: its own counter properties apply
        if (ms) ctr_apply(L, &mset, ctr_id(el, 3), ms, -1);
        if (ms && ms->content) ml = expand_content(L, &mset, css_str(L->ss, ms->content, ms->content_len), ms->content_len, mt, (int)sizeof mt);
        else ml = marker_text(st, ord, mt, L->ss);
        if (ml > 0) {
            uint32_t off = (uint32_t)L->text.len;
            wbuf_put(&L->text, mt, ml);
            if (st->list_style_inside) {
                int m = lb_new(L, -1, ms ? ms : st, LB_MARKER);
                if (m >= 0) {
                    L->b[m].t0 = off;
                    L->b[m].tl = (uint32_t)ml;
                    L->b[m].flags |= BF_INLINE_LEVEL | BF_RAWTEXT | (b->flags & BF_FIXED);
                    lb_append(L, bi, m);
                }
            } else {
                b = &L->b[bi];
                b->flags |= BF_MARKER;
                b->t0 = off;
                b->tl = (uint32_t)ml;
            }
        }
    }
    lb_append(L, parent_box, bi);
    if (kind == LB_REPLACED || kind == LB_BR || kind == LB_WBR) {
        if (kind == LB_REPLACED && n->tag == T_button) build_children(L, el, bi, &mine, &cprev);
        return;
    }
    L->depth++;
    build_pseudo(L, el, bi, 1, &mine, &cprev);
    build_children(L, el, bi, &mine, &cprev);
    build_pseudo(L, el, bi, 2, &mine, &cprev);
    L->depth--;
    b = &L->b[bi];
    if (kind == LB_INLINE) {
        // block-in-inline: an inline containing block-level boxes becomes a block
        for (int c = b->first; c >= 0; c = L->b[c].next)
            if (is_block_level(&L->b[c]) || is_table_part(&L->b[c])) {
                b->kind = LB_BLOCK;
                b->fc = FC_FLOW;
                b->flags &= ~BF_INLINE_LEVEL;
                break;
            }
    }
    if (b->kind == LB_BLOCK) fixup(L, bi);
}

static void build_children(struct wlayout* L, int el, int box, const struct cset* mine, struct cset* cprev) {
    struct wdom* d = L->d;
    const struct wstyle* st = css_style_of(L->ss, el);
    const struct wstyle* bst = L->b[box].st;
    for (int c = d->n[el].first; c >= 0; c = d->n[c].next) {
        const struct wnode* n = &d->n[c];
        if (n->type == WN_TEXT) {
            // text inherits through the element whose children these are —
            // for display:contents that is the contents element, not the box
            if (n->tlen) text_box(L, box, c, st ? st : bst);
        } else if (n->type == WN_ELEM) {
            build_el(L, c, box, mine, cprev);
        }
    }
}

int lay_build_tree(struct wlayout* L) {
    struct wdom* d = L->d;
    int html = d->html >= 0 ? d->html : wdom_first_tag(d, T_html);
    // initial containing block holder (anonymous, not painted)
    const struct wstyle* rs = html >= 0 ? css_style_of(L->ss, html) : 0;
    struct wstyle* icb_style = (struct wstyle*)lay_anon_style(L, rs, D_BLOCK);
    int icb = lb_new(L, -1, icb_style, LB_BLOCK);
    if (icb < 0) return -1;
    L->b[icb].flags |= BF_ANON | BF_BFC | BF_POSITIONED;
    L->root = icb;
    L->nctr = 0;
    if (html < 0) return icb;
    struct cset none, none2;
    none.n = 0; none2.n = 0;
    build_el(L, html, icb, &none, &none2);
    fixup(L, icb);
    return icb;
}
