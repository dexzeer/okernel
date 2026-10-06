// Selector matching (right-to-left with backtracking for descendant /
// subsequent-sibling combinators).
#include "css_int.h"

#define MAXCOMP 32

static int prev_el(const struct wdom* d, int n) {
    for (n = d->n[n].prev; n >= 0; n = d->n[n].prev) if (d->n[n].type == WN_ELEM) return n;
    return -1;
}
static int next_el(const struct wdom* d, int n) {
    for (n = d->n[n].next; n >= 0; n = d->n[n].next) if (d->n[n].type == WN_ELEM) return n;
    return -1;
}
static int parent_el(const struct wdom* d, int n) {
    int p = d->n[n].parent;
    return (p >= 0 && d->n[p].type == WN_ELEM) ? p : -1;
}

static int match_list(const struct wdom* d, const struct wsheet* sh, int first, int count, int el);

static int attr_match(const struct wdom* d, const struct wsheet* sh, const struct cpart* p, int el) {
    int vl;
    const char* v = wdom_attr(d, el, p->atom, &vl);
    if (!v) return 0;
    if (p->op == AO_EXISTS) return 1;
    const char* want = sh->pool.p + p->a;
    int wl = p->b;
    #define EQN(a, b, n) (p->ci ? w_ieq_n((a), (b), (n)) : !memcmp((a), (b), (n)))
    switch (p->op) {
    case AO_EQ: return vl == wl && EQN(v, want, wl);
    case AO_PREFIX: return wl > 0 && vl >= wl && EQN(v, want, wl);
    case AO_SUFFIX: return wl > 0 && vl >= wl && EQN(v + vl - wl, want, wl);
    case AO_SUBSTR:
        if (wl <= 0) return 0;
        for (int i = 0; i + wl <= vl; i++) if (EQN(v + i, want, wl)) return 1;
        return 0;
    case AO_DASH: return (vl == wl && EQN(v, want, wl)) || (vl > wl && EQN(v, want, wl) && v[wl] == '-');
    case AO_INCLUDES:
        if (wl <= 0) return 0;
        for (int i = 0; i < vl;) {
            while (i < vl && w_isspace((unsigned char)v[i])) i++;
            int s = i;
            while (i < vl && !w_isspace((unsigned char)v[i])) i++;
            if (i - s == wl && EQN(v + s, want, wl)) return 1;
        }
        return 0;
    }
    #undef EQN
    return 0;
}

static int nth_ok(int32_t a, int32_t b, int idx) {
    if (a == 0) return idx == b;
    int32_t diff = idx - b;
    if (a > 0) return diff >= 0 && diff % a == 0;
    return diff <= 0 && (-diff) % (-a) == 0;
}

static int same_type(const struct wdom* d, int a, int b) {
    return d->n[a].tag == d->n[b].tag && d->n[a].ns == d->n[b].ns;
}

static int is_form_control(const struct wdom* d, int el) {
    int t = d->n[el].tag;
    return d->n[el].ns == NS_HTML && (t == T_input || t == T_button || t == T_select ||
                                      t == T_textarea || t == T_optgroup || t == T_option ||
                                      t == T_fieldset);
}

static int text_input(const struct wdom* d, int el) {
    if (wdom_is(d, el, T_textarea)) return 1;
    if (!wdom_is(d, el, T_input)) return 0;
    int tl;
    const char* t = wdom_attr(d, el, A_type, &tl);
    if (!t) return 1;
    static const char* const TXT[] = { "text", "search", "email", "url", "tel", "password",
                                       "number", "date", "time", "datetime-local", "month", "week", 0 };
    for (int i = 0; TXT[i]; i++) if (w_ieq(t, tl, TXT[i])) return 1;
    return 0;
}

static int has_rel_match(const struct wdom* d, const struct wsheet* sh, int first, int count, int anchor);

static int pseudo_match(const struct wdom* d, const struct wsheet* sh, const struct cpart* p, int el) {
    switch (p->op) {
    case PC_FIRST_CHILD: return prev_el(d, el) < 0;
    case PC_LAST_CHILD: return next_el(d, el) < 0;
    case PC_ONLY_CHILD: return prev_el(d, el) < 0 && next_el(d, el) < 0;
    case PC_NTH_CHILD: case PC_NTH_LAST_CHILD: {
        if (p->sub >= 0 && !match_list(d, sh, p->sub, p->nsub, el)) return 0;
        int idx = 1;
        for (int s = (p->op == PC_NTH_CHILD) ? prev_el(d, el) : next_el(d, el); s >= 0;
             s = (p->op == PC_NTH_CHILD) ? prev_el(d, s) : next_el(d, s))
            if (p->sub < 0 || match_list(d, sh, p->sub, p->nsub, s)) idx++;
        return nth_ok(p->a, p->b, idx);
    }
    case PC_FIRST_OF_TYPE:
        for (int s = prev_el(d, el); s >= 0; s = prev_el(d, s)) if (same_type(d, s, el)) return 0;
        return 1;
    case PC_LAST_OF_TYPE:
        for (int s = next_el(d, el); s >= 0; s = next_el(d, s)) if (same_type(d, s, el)) return 0;
        return 1;
    case PC_ONLY_OF_TYPE:
        for (int s = prev_el(d, el); s >= 0; s = prev_el(d, s)) if (same_type(d, s, el)) return 0;
        for (int s = next_el(d, el); s >= 0; s = next_el(d, s)) if (same_type(d, s, el)) return 0;
        return 1;
    case PC_NTH_OF_TYPE: case PC_NTH_LAST_OF_TYPE: {
        int idx = 1;
        for (int s = (p->op == PC_NTH_OF_TYPE) ? prev_el(d, el) : next_el(d, el); s >= 0;
             s = (p->op == PC_NTH_OF_TYPE) ? prev_el(d, s) : next_el(d, s))
            if (same_type(d, s, el)) idx++;
        return nth_ok(p->a, p->b, idx);
    }
    case PC_NOT: return !match_list(d, sh, p->sub, p->nsub, el);
    case PC_IS: case PC_WHERE: return match_list(d, sh, p->sub, p->nsub, el);
    case PC_HAS: return has_rel_match(d, sh, p->sub, p->nsub, el);
    case PC_ROOT: return d->n[el].parent == 0;
    case PC_EMPTY:
        for (int c = d->n[el].first; c >= 0; c = d->n[c].next) {
            if (d->n[c].type == WN_ELEM) return 0;
            if (d->n[c].type == WN_TEXT && d->n[c].tlen) return 0;
        }
        return 1;
    case PC_LINK:
        return (wdom_is(d, el, T_a) || wdom_is(d, el, T_area)) && wdom_has_attr(d, el, A_href);
    case PC_VISITED: return 0;
    case PC_HOVER: case PC_ACTIVE: return (d->n[el].flags & WNF_HOVER) != 0;
    case PC_FOCUS: case PC_FOCUS_VISIBLE: return (d->n[el].flags & WNF_FOCUSED) != 0;
    case PC_FOCUS_WITHIN: return (d->n[el].flags & (WNF_FOCUSED | WNF_FOCUS_WITHIN)) != 0;
    case PC_CHECKED:
        if (wdom_is(d, el, T_input)) return wdom_has_attr(d, el, A_checked);
        if (wdom_is(d, el, T_option)) return wdom_has_attr(d, el, A_selected);
        return 0;
    case PC_DISABLED: return is_form_control(d, el) && wdom_has_attr(d, el, A_disabled);
    case PC_ENABLED: return is_form_control(d, el) && !wdom_has_attr(d, el, A_disabled);
    case PC_REQUIRED: case PC_OPTIONAL: {
        int t = d->n[el].tag;
        if (d->n[el].ns != NS_HTML || !(t == T_input || t == T_select || t == T_textarea)) return 0;
        int req = wdom_attr_s(d, el, "required", 0) != 0;
        return p->op == PC_REQUIRED ? req : !req;
    }
    case PC_READ_WRITE: case PC_READ_ONLY: {
        int rw = text_input(d, el) && !wdom_has_attr(d, el, A_readonly) &&
                 !wdom_has_attr(d, el, A_disabled);
        if (!rw && wdom_attr_s(d, el, "contenteditable", 0)) rw = 1;
        return p->op == PC_READ_WRITE ? rw : !rw;
    }
    case PC_PLACEHOLDER_SHOWN: {
        if (!text_input(d, el) || !wdom_has_attr(d, el, A_placeholder)) return 0;
        int vl = 0;
        const char* v = wdom_attr(d, el, A_value, &vl);
        return !v || vl == 0;
    }
    case PC_LANG: {
        const char* want = sh->pool.p + p->a;
        int wl = p->b;
        for (int e = el; e >= 0; e = parent_el(d, e)) {
            int vl;
            const char* v = wdom_attr(d, e, A_lang, &vl);
            if (!v) continue;
            if (vl < wl) return 0;
            if (!w_ieq_n(v, want, wl)) return 0;
            return vl == wl || v[wl] == '-';
        }
        return 0;
    }
    case PC_OPEN: return wdom_has_attr(d, el, A_open);
    case PC_ALWAYS: return 1;
    case PC_NEVER: case PC_TARGET: case PC_DEFAULT: case PC_INDETERMINATE: return 0;
    }
    return 0;
}

static int match_compound(const struct wdom* d, const struct wsheet* sh, int s, int e, int el,
                          int anchor) {
    const struct wnode* n = &d->n[el];
    for (int i = s; i < e; i++) {
        const struct cpart* p = &sh->parts[i];
        switch (p->kind) {
        case SK_UNIV: break;
        case SK_TAG: if (n->tag != p->atom) return 0; break;
        case SK_ID: if (n->id != p->atom || !p->atom) return 0; break;
        case SK_CLASS: if (!wdom_has_class(d, el, p->atom)) return 0; break;
        case SK_ATTR: if (!attr_match(d, sh, p, el)) return 0; break;
        case SK_PSEUDO: if (!pseudo_match(d, sh, p, el)) return 0; break;
        case SK_END: return 1;
        default: break;
        }
    }
    (void)anchor;
    return 1;
}

struct comps {
    int s[MAXCOMP], e[MAXCOMP];   // compound part ranges (left to right)
    int comb[MAXCOMP];            // combinator BEFORE compound k (k >= 1)
    int n;
    int anchor_first;             // relative selector: compound 0 is the :has anchor
};

static void split(const struct wsheet* sh, int first, struct comps* c) {
    c->n = 0;
    c->anchor_first = 0;
    int i = first;
    if (sh->parts[i].kind == SK_COMB) { // leading combinator (relative selector)
        c->s[0] = c->e[0] = -1;
        c->comb[0] = 0;
        c->n = 1;
        c->anchor_first = 1;
        c->comb[1] = sh->parts[i].op;
        i++;
    }
    while (c->n < MAXCOMP) {
        int st = i;
        while (sh->parts[i].kind != SK_COMB && sh->parts[i].kind != SK_END) i++;
        c->s[c->n] = st;
        c->e[c->n] = i;
        c->n++;
        if (sh->parts[i].kind == SK_END) break;
        c->comb[c->n] = sh->parts[i].op;
        i++;
    }
}

static int match_at(const struct wdom* d, const struct wsheet* sh, const struct comps* c, int k,
                    int el, int anchor) {
    if (c->anchor_first && k == 0) return el == anchor;
    if (!match_compound(d, sh, c->s[k], c->e[k], el, anchor)) return 0;
    if (k == 0) return 1;
    switch (c->comb[k]) {
    case CB_CHILD: {
        int p = c->anchor_first && k == 1 ? d->n[el].parent : parent_el(d, el);
        return p >= 0 && match_at(d, sh, c, k - 1, p, anchor);
    }
    case CB_DESC:
        for (int p = d->n[el].parent; p >= 0; p = d->n[p].parent) {
            if (d->n[p].type != WN_ELEM) break;
            if (match_at(d, sh, c, k - 1, p, anchor)) return 1;
        }
        return 0;
    case CB_ADJ: {
        int p = prev_el(d, el);
        return p >= 0 && match_at(d, sh, c, k - 1, p, anchor);
    }
    case CB_SIB:
        for (int p = prev_el(d, el); p >= 0; p = prev_el(d, p))
            if (match_at(d, sh, c, k - 1, p, anchor)) return 1;
        return 0;
    }
    return 0;
}

int csel_match(const struct wdom* d, const struct wsheet* sh, int sel, int el) {
    struct comps c;
    split(sh, sh->sels[sel].first, &c);
    return match_at(d, sh, &c, c.n - 1, el, -1);
}

static int match_list(const struct wdom* d, const struct wsheet* sh, int first, int count, int el) {
    for (int k = 0; k < count; k++) if (csel_match(d, sh, first + k, el)) return 1;
    return 0;
}

// :has(<relative selectors>): candidates are descendants (> and descendant
// combinators) or following siblings and their subtrees (+ and ~).
static int has_rel_match(const struct wdom* d, const struct wsheet* sh, int first, int count, int anchor) {
    for (int k = 0; k < count; k++) {
        struct comps c;
        split(sh, sh->sels[first + k].first, &c);
        if (!c.anchor_first) continue;
        int lead = c.comb[1];
        if (lead == CB_CHILD || lead == CB_DESC) {
            int budget = 20000;
            for (int x = d->n[anchor].first; x >= 0 && budget-- > 0; x = wdom_next(d, x, anchor))
                if (d->n[x].type == WN_ELEM && match_at(d, sh, &c, c.n - 1, x, anchor)) return 1;
        } else {
            for (int s = next_el(d, anchor); s >= 0; s = next_el(d, s)) {
                if (match_at(d, sh, &c, c.n - 1, s, anchor)) return 1;
                if (c.n > 2) {
                    int budget = 5000;
                    for (int x = d->n[s].first; x >= 0 && budget-- > 0; x = wdom_next(d, x, s))
                        if (d->n[x].type == WN_ELEM && match_at(d, sh, &c, c.n - 1, x, anchor)) return 1;
                }
                if (lead == CB_ADJ && c.n == 2) break;
            }
        }
    }
    return 0;
}
