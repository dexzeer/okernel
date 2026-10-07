// HTML5 parser: WHATWG tokenizer + tree construction.
//
// Follows the HTML Living Standard's parsing algorithm: insertion modes,
// the stack of open elements, the list of active formatting elements (with
// markers and the Noah's Ark clause), the adoption agency algorithm, foster
// parenting, implied end tags, scope rules, foreign content (SVG/MathML) and
// RCDATA/RAWTEXT/script/plaintext tokenizer states. The scripting flag is
// OFF (external scripts never run), so <noscript> content is parsed as
// markup and rendered, exactly like a browser with JavaScript disabled.
//
// Simplifications (documented, deliberate): script data escape states are
// folded into plain script data; <template> contents are ordinary children
// (hidden by the UA stylesheet); doctype quirks detection uses the common
// subset of the legacy public-id list.

#include "wdom.h"
#include "wcommon.h"
#include "entities.h"

#define MAXSTACK 1024
#define MAXAFE   256
#define MAXATTR  64

enum { TK_START, TK_END, TK_CHARS, TK_COMMENT, TK_DOCTYPE, TK_EOF };
enum { TS_DATA, TS_RCDATA, TS_RAWTEXT, TS_SCRIPT, TS_PLAINTEXT };
enum {
    M_INITIAL, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_IN_HEAD_NOSCRIPT,
    M_AFTER_HEAD, M_IN_BODY, M_TEXT, M_IN_TABLE, M_IN_TABLE_TEXT, M_IN_CAPTION,
    M_IN_COLGROUP, M_IN_TBODY, M_IN_ROW, M_IN_CELL, M_IN_SELECT,
    M_IN_SELECT_IN_TABLE, M_AFTER_BODY, M_IN_FRAMESET, M_AFTER_FRAMESET,
    M_AFTER_AFTER_BODY, M_AFTER_AFTER_FRAMESET
};

struct tattr { int name; uint32_t voff; int vlen; char raw[96]; int rawlen; };

struct tok {
    int type;
    int tag;                 // atom (start/end)
    char name[64];           // lowercase tag name (start/end)
    int nlen;
    int self_closing;
    struct tattr* attrs;     // -> hp.attr_store (kept off the stack)
    int nattr;
    const char* chars;       // TK_CHARS / TK_COMMENT data
    int clen;
    int force_quirks;        // doctype
    int dt_quirk;            // 0 none, 1 limited, 2 full
};

struct hp {
    struct wdom* d;
    const char* in;
    int len, pos;
    int tstate;
    int raw_tag;             // atom whose end tag ends RCDATA/RAWTEXT/script
    struct wbuf chars;       // pending character data
    struct wbuf vals;        // attribute values of the current token
    struct wbuf misc;        // comment/doctype scratch
    int stack[MAXSTACK];
    int sp;
    int overflow;            // elements dropped from the stack (too deep)
    int afe[MAXAFE];
    int nafe;
    int head, form;
    int mode, orig_mode;
    int frameset_ok;
    int foster;
    struct wbuf ptt;         // pending table character tokens
    int ptt_nonws;
    int skip_lf;             // drop a leading LF (pre/listing/textarea)
    int stopped;
    struct tattr attr_store[MAXATTR];
};

#define D (p->d)
#define N(i) (p->d->n[i])

// ---- element classification ---------------------------------------------

static int is_html(struct hp* p, int el, int tag) {
    return el >= 0 && N(el).type == WN_ELEM && N(el).ns == NS_HTML && N(el).tag == tag;
}

static int tag_in(int tag, const int* list) {
    for (; *list; list++) if (*list == tag) return 1;
    return 0;
}

static const int SPECIAL[] = {
    T_address, T_applet, T_area, T_article, T_aside, T_base, T_basefont, T_bgsound,
    T_blockquote, T_body, T_br, T_button, T_caption, T_center, T_col, T_colgroup,
    T_dd, T_details, T_dir, T_div, T_dl, T_dt, T_embed, T_fieldset, T_figcaption,
    T_figure, T_footer, T_form, T_frame, T_frameset, T_h1, T_h2, T_h3, T_h4, T_h5,
    T_h6, T_head, T_header, T_hgroup, T_hr, T_html, T_iframe, T_img, T_input,
    T_keygen, T_li, T_link, T_listing, T_main, T_marquee, T_menu, T_meta, T_nav,
    T_noembed, T_noframes, T_noscript, T_object, T_ol, T_p, T_param, T_plaintext,
    T_pre, T_script, T_search, T_section, T_select, T_source, T_style, T_summary,
    T_table, T_tbody, T_td, T_template, T_textarea, T_tfoot, T_th, T_thead,
    T_title, T_tr, T_track, T_ul, T_wbr, T_xmp, 0
};
static const int FORMATTING[] = {
    T_a, T_b, T_big, T_code, T_em, T_font, T_i, T_nobr, T_s, T_small, T_strike,
    T_strong, T_tt, T_u, 0
};
static const int HEADINGS[] = { T_h1, T_h2, T_h3, T_h4, T_h5, T_h6, 0 };

static int is_special(struct hp* p, int el) {
    const struct wnode* n = &N(el);
    if (n->ns == NS_HTML) return tag_in(n->tag, SPECIAL);
    if (n->ns == NS_MATH)
        return n->tag == T_mi || n->tag == T_mo || n->tag == T_mn || n->tag == T_ms ||
               n->tag == T_mtext || n->tag == T_annotation_xml;
    if (n->ns == NS_SVG)
        return n->tag == T_foreignobject || n->tag == T_desc || n->tag == T_title;
    return 0;
}

static int cur(struct hp* p) { return p->sp ? p->stack[p->sp - 1] : -1; }

static int is_cur(struct hp* p, int tag) { return is_html(p, cur(p), tag); }

// ---- scopes ------------------------------------------------------------------

enum { SC_DEFAULT, SC_LIST, SC_BUTTON, SC_TABLE, SC_SELECT };

static int scope_boundary(struct hp* p, int el, int kind) {
    const struct wnode* n = &N(el);
    if (kind == SC_SELECT) // everything except optgroup/option bounds select scope
        return !(n->ns == NS_HTML && (n->tag == T_optgroup || n->tag == T_option));
    if (n->ns == NS_HTML) {
        if (kind == SC_TABLE) return n->tag == T_html || n->tag == T_table || n->tag == T_template;
        if (n->tag == T_applet || n->tag == T_caption || n->tag == T_html || n->tag == T_table ||
            n->tag == T_td || n->tag == T_th || n->tag == T_marquee || n->tag == T_object ||
            n->tag == T_template)
            return 1;
        if (kind == SC_LIST && (n->tag == T_ol || n->tag == T_ul)) return 1;
        if (kind == SC_BUTTON && n->tag == T_button) return 1;
        return 0;
    }
    if (kind == SC_TABLE) return 0;
    if (n->ns == NS_MATH)
        return n->tag == T_mi || n->tag == T_mo || n->tag == T_mn || n->tag == T_ms ||
               n->tag == T_mtext || n->tag == T_annotation_xml;
    return n->tag == T_foreignobject || n->tag == T_desc || n->tag == T_title;
}

static int in_scope(struct hp* p, int tag, int kind) {
    for (int i = p->sp - 1; i >= 0; i--) {
        int el = p->stack[i];
        if (is_html(p, el, tag)) return 1;
        if (scope_boundary(p, el, kind)) return 0;
    }
    return 0;
}

static int in_scope_el(struct hp* p, int target, int kind) {
    for (int i = p->sp - 1; i >= 0; i--) {
        int el = p->stack[i];
        if (el == target) return 1;
        if (scope_boundary(p, el, kind)) return 0;
    }
    return 0;
}

static int heading_in_scope(struct hp* p) {
    for (int i = p->sp - 1; i >= 0; i--) {
        int el = p->stack[i];
        if (N(el).ns == NS_HTML && tag_in(N(el).tag, HEADINGS)) return 1;
        if (scope_boundary(p, el, SC_DEFAULT)) return 0;
    }
    return 0;
}

// ---- stack ops -----------------------------------------------------------------

static void push(struct hp* p, int el) {
    if (p->sp < MAXSTACK) p->stack[p->sp++] = el;
    else p->overflow++;
}

static void pop(struct hp* p) {
    if (p->overflow) { p->overflow--; return; }
    if (p->sp > 0) p->sp--;
}

static void pop_until(struct hp* p, int tag) {
    while (p->sp > 0) {
        int el = cur(p);
        pop(p);
        if (is_html(p, el, tag)) return;
    }
}

static void pop_until_el(struct hp* p, int target) {
    while (p->sp > 0) {
        int el = cur(p);
        pop(p);
        if (el == target) return;
    }
}

static void remove_from_stack(struct hp* p, int el) {
    for (int i = p->sp - 1; i >= 0; i--)
        if (p->stack[i] == el) {
            for (int j = i; j < p->sp - 1; j++) p->stack[j] = p->stack[j + 1];
            p->sp--;
            return;
        }
}

static int stack_index(struct hp* p, int el) {
    for (int i = p->sp - 1; i >= 0; i--) if (p->stack[i] == el) return i;
    return -1;
}

static void gen_implied(struct hp* p, int except) {
    for (;;) {
        int c = cur(p);
        if (c < 0 || N(c).ns != NS_HTML) return;
        int t = N(c).tag;
        if (t == except) return;
        if (t == T_dd || t == T_dt || t == T_li || t == T_optgroup || t == T_option ||
            t == T_p || t == T_rb || t == T_rp || t == T_rt || t == T_rtc)
            pop(p);
        else return;
    }
}

static void gen_implied_all(struct hp* p) {
    for (;;) {
        int c = cur(p);
        if (c < 0 || N(c).ns != NS_HTML) return;
        int t = N(c).tag;
        if (t == T_dd || t == T_dt || t == T_li || t == T_optgroup || t == T_option ||
            t == T_p || t == T_rb || t == T_rp || t == T_rt || t == T_rtc || t == T_caption ||
            t == T_colgroup || t == T_tbody || t == T_td || t == T_tfoot || t == T_th ||
            t == T_thead || t == T_tr)
            pop(p);
        else return;
    }
}

static void close_p(struct hp* p) {
    gen_implied(p, T_p);
    pop_until(p, T_p);
}

// ---- insertion ---------------------------------------------------------------

static int last_table(struct hp* p) {
    for (int i = p->sp - 1; i >= 0; i--) if (is_html(p, p->stack[i], T_table)) return i;
    return -1;
}

// Appropriate place for inserting a node (foster parenting aware).
static void place(struct hp* p, int* parent, int* before) {
    int target = cur(p);
    *before = -1;
    if (p->foster && target >= 0 && N(target).ns == NS_HTML &&
        (N(target).tag == T_table || N(target).tag == T_tbody || N(target).tag == T_tfoot ||
         N(target).tag == T_thead || N(target).tag == T_tr)) {
        int ti = last_table(p);
        if (ti < 0) { *parent = p->stack[0]; return; }
        int table = p->stack[ti];
        if (N(table).parent >= 0) { *parent = N(table).parent; *before = table; return; }
        *parent = p->stack[ti - 1];
        return;
    }
    *parent = target >= 0 ? target : 0;
}

static void set_attrs(struct hp* p, int el, struct tok* t) {
    for (int i = 0; i < t->nattr; i++) {
        if (!t->attrs[i].name) continue;
        wdom_set_attr(D, el, t->attrs[i].name, p->vals.p + t->attrs[i].voff, t->attrs[i].vlen);
    }
}

static int create_for_token(struct hp* p, struct tok* t, int ns) {
    int el = wdom_create_element(D, ns, t->tag);
    if (el < 0) return -1;
    set_attrs(p, el, t);
    return el;
}

static int insert_element(struct hp* p, struct tok* t, int ns) {
    int parent, before;
    place(p, &parent, &before);
    int el = create_for_token(p, t, ns);
    if (el < 0) return -1;
    wdom_insert_before(D, parent, el, before);
    push(p, el);
    return el;
}

static int insert_html(struct hp* p, int tag) {
    struct tok t;
    t.tag = tag; t.nattr = 0;
    return insert_element(p, &t, NS_HTML);
}

static void insert_chars(struct hp* p, const char* s, int len) {
    if (len <= 0) return;
    int parent, before;
    place(p, &parent, &before);
    if (parent == 0) return; // never text under the document node
    wdom_insert_text_before(D, parent, before, s, len);
}

static void insert_comment(struct hp* p, struct tok* t, int parent) {
    int c = wdom_create_comment(D, t->chars, t->clen);
    if (c < 0) return;
    if (parent >= 0) { wdom_append(D, parent, c); return; }
    int pp, before;
    place(p, &pp, &before);
    wdom_insert_before(D, pp, c, before);
}

// ---- active formatting elements -------------------------------------------------

#define MARKER (-1)

static int same_attrs(struct hp* p, int a, int b) {
    int ca = 0, cb = 0;
    for (int x = N(a).attr; x >= 0; x = D->a[x].next) ca++;
    for (int x = N(b).attr; x >= 0; x = D->a[x].next) cb++;
    if (ca != cb) return 0;
    for (int x = N(a).attr; x >= 0; x = D->a[x].next) {
        int l;
        const char* v = wdom_attr(D, b, D->a[x].name, &l);
        if (!v || (uint32_t)l != D->a[x].vlen || memcmp(v, D->text + D->a[x].val, l)) return 0;
    }
    return 1;
}

static void afe_push(struct hp* p, int el) {
    int count = 0, earliest = -1;
    for (int i = p->nafe - 1; i >= 0 && p->afe[i] != MARKER; i--) {
        int e = p->afe[i];
        if (N(e).tag == N(el).tag && N(e).ns == N(el).ns && same_attrs(p, e, el)) {
            count++;
            earliest = i;
        }
    }
    if (count >= 3) {
        for (int j = earliest; j < p->nafe - 1; j++) p->afe[j] = p->afe[j + 1];
        p->nafe--;
    }
    if (p->nafe < MAXAFE) p->afe[p->nafe++] = el;
}

static void afe_marker(struct hp* p) { if (p->nafe < MAXAFE) p->afe[p->nafe++] = MARKER; }

static void afe_clear_to_marker(struct hp* p) {
    while (p->nafe > 0) {
        int e = p->afe[--p->nafe];
        if (e == MARKER) return;
    }
}

static int afe_index(struct hp* p, int el) {
    for (int i = p->nafe - 1; i >= 0; i--) if (p->afe[i] == el) return i;
    return -1;
}

static void afe_remove(struct hp* p, int el) {
    int i = afe_index(p, el);
    if (i < 0) return;
    for (int j = i; j < p->nafe - 1; j++) p->afe[j] = p->afe[j + 1];
    p->nafe--;
}

static int clone_el(struct hp* p, int el) {
    int c = wdom_create_element(D, N(el).ns, N(el).tag);
    if (c < 0) return -1;
    for (int x = N(el).attr; x >= 0; x = D->a[x].next) {
        // copy value out first: set_attr may grow the text arena
        int vl = (int)D->a[x].vlen;
        char tmp[256];
        char* v = vl <= 256 ? tmp : (char*)w_malloc(vl);
        if (!v) continue;
        memcpy(v, D->text + D->a[x].val, vl);
        wdom_set_attr(D, c, D->a[x].name, v, vl);
        if (v != tmp) w_free(v);
    }
    return c;
}

static void reconstruct_afe(struct hp* p) {
    if (p->nafe == 0) return;
    int i = p->nafe - 1;
    int e = p->afe[i];
    if (e == MARKER || stack_index(p, e) >= 0) return;
    while (i > 0) {
        i--;
        e = p->afe[i];
        if (e == MARKER || stack_index(p, e) >= 0) { i++; break; }
    }
    for (; i < p->nafe; i++) {
        int src = p->afe[i];
        int parent, before;
        place(p, &parent, &before);
        int c = clone_el(p, src);
        if (c < 0) return;
        wdom_insert_before(D, parent, c, before);
        push(p, c);
        p->afe[i] = c;
    }
}

// Adoption agency algorithm. Returns 0 when the token must be handled as
// "any other end tag" instead.
static int adoption_agency(struct hp* p, int tag) {
    int c = cur(p);
    if (is_html(p, c, tag) && afe_index(p, c) < 0) { pop(p); return 1; }
    for (int outer = 0; outer < 8; outer++) {
        int fi = -1;
        for (int i = p->nafe - 1; i >= 0 && p->afe[i] != MARKER; i--)
            if (is_html(p, p->afe[i], tag)) { fi = i; break; }
        if (fi < 0) return 0;
        int fe = p->afe[fi];
        int fsi = stack_index(p, fe);
        if (fsi < 0) { afe_remove(p, fe); return 1; }
        if (!in_scope_el(p, fe, SC_DEFAULT)) return 1;
        int furthest = -1, fbi = -1;
        for (int i = fsi + 1; i < p->sp; i++)
            if (is_special(p, p->stack[i])) { furthest = p->stack[i]; fbi = i; break; }
        if (furthest < 0) {
            while (p->sp > fsi) pop(p);
            afe_remove(p, fe);
            return 1;
        }
        int common = p->stack[fsi - 1];
        int bookmark = afe_index(p, fe);
        int node = furthest, last = furthest;
        int ni = fbi;
        for (int inner = 1;; inner++) {
            ni--;
            node = p->stack[ni];
            if (node == fe) break;
            int ai = afe_index(p, node);
            if (inner > 3 && ai >= 0) {
                afe_remove(p, node);
                if (ai < bookmark) bookmark--;
                ai = -1;
            }
            if (ai < 0) {
                remove_from_stack(p, node);
                continue;
            }
            int nc = clone_el(p, node);
            if (nc < 0) return 1;
            p->afe[ai] = nc;
            p->stack[ni] = nc;
            node = nc;
            if (last == furthest) bookmark = ai + 1;
            wdom_append(D, node, last);
            last = node;
        }
        // insert last at the appropriate place for common
        if (N(common).ns == NS_HTML &&
            (N(common).tag == T_table || N(common).tag == T_tbody || N(common).tag == T_tfoot ||
             N(common).tag == T_thead || N(common).tag == T_tr)) {
            int ti = last_table(p);
            if (ti >= 0 && N(p->stack[ti]).parent >= 0)
                wdom_insert_before(D, N(p->stack[ti]).parent, last, p->stack[ti]);
            else if (ti > 0) wdom_append(D, p->stack[ti - 1], last);
            else wdom_append(D, common, last);
        } else {
            wdom_append(D, common, last);
        }
        int ne = clone_el(p, fe);
        if (ne < 0) return 1;
        while (N(furthest).first >= 0) wdom_append(D, ne, N(furthest).first);
        wdom_append(D, furthest, ne);
        int oldi = afe_index(p, fe);
        if (oldi >= 0) {
            for (int j = oldi; j < p->nafe - 1; j++) p->afe[j] = p->afe[j + 1];
            p->nafe--;
            if (oldi < bookmark) bookmark--;
        }
        if (bookmark < 0) bookmark = 0;
        if (bookmark > p->nafe) bookmark = p->nafe;
        if (p->nafe < MAXAFE) {
            for (int j = p->nafe; j > bookmark; j--) p->afe[j] = p->afe[j - 1];
            p->afe[bookmark] = ne;
            p->nafe++;
        }
        remove_from_stack(p, fe);
        int fbpos = stack_index(p, furthest);
        if (fbpos >= 0 && p->sp < MAXSTACK) {
            for (int j = p->sp; j > fbpos + 1; j--) p->stack[j] = p->stack[j - 1];
            p->stack[fbpos + 1] = ne;
            p->sp++;
        }
    }
    return 1;
}

// ---- insertion mode reset ----------------------------------------------------

static void reset_mode(struct hp* p) {
    for (int i = p->sp - 1; i >= 0; i--) {
        int el = p->stack[i];
        int last = (i == 0);
        if (N(el).ns != NS_HTML) { if (last) { p->mode = M_IN_BODY; return; } continue; }
        int t = N(el).tag;
        if (t == T_select) {
            for (int j = i - 1; j > 0; j--) {
                if (is_html(p, p->stack[j], T_template)) break;
                if (is_html(p, p->stack[j], T_table)) { p->mode = M_IN_SELECT_IN_TABLE; return; }
            }
            p->mode = M_IN_SELECT;
            return;
        }
        if ((t == T_td || t == T_th) && !last) { p->mode = M_IN_CELL; return; }
        if (t == T_tr) { p->mode = M_IN_ROW; return; }
        if (t == T_tbody || t == T_thead || t == T_tfoot) { p->mode = M_IN_TBODY; return; }
        if (t == T_caption) { p->mode = M_IN_CAPTION; return; }
        if (t == T_colgroup) { p->mode = M_IN_COLGROUP; return; }
        if (t == T_table) { p->mode = M_IN_TABLE; return; }
        if (t == T_template) { p->mode = M_IN_BODY; return; }
        if (t == T_head && !last) { p->mode = M_IN_HEAD; return; }
        if (t == T_body) { p->mode = M_IN_BODY; return; }
        if (t == T_frameset) { p->mode = M_IN_FRAMESET; return; }
        if (t == T_html) { p->mode = p->head < 0 ? M_BEFORE_HEAD : M_AFTER_HEAD; return; }
        if (last) { p->mode = M_IN_BODY; return; }
    }
    p->mode = M_IN_BODY;
}

// ---- tokenizer helpers ----------------------------------------------------------

static int went_cmp(const char* a, int alen, const char* b) {
    int i = 0;
    for (; i < alen && b[i]; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return x < y ? -1 : 1;
    }
    if (i == alen && !b[i]) return 0;
    return i == alen ? -1 : 1;
}

static int went_find(const char* s, int len) {
    int lo = 0, hi = WENT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = went_cmp(s, len, went_table[mid].name);
        if (!c) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static const uint16_t C1_FIX[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
};

// Decode a character reference at in[pos] == '&'. Appends the result to out
// and returns the number of input bytes consumed (1 = literal '&').
static int charref(struct hp* p, int pos, int in_attr, struct wbuf* out) {
    const char* s = p->in + pos;
    int n = p->len - pos;
    if (n < 2) { wbuf_putc(out, '&'); return 1; }
    char c = s[1];
    if (c == '#') {
        int i = 2, hex = 0;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        int st = i;
        uint32_t v = 0;
        int big = 0;
        while (i < n && (hex ? w_ishex((unsigned char)s[i]) : w_isdigit((unsigned char)s[i]))) {
            v = hex ? v * 16 + (uint32_t)w_hexval((unsigned char)s[i]) : v * 10 + (uint32_t)(s[i] - '0');
            if (v > 0x10FFFF) big = 1;
            i++;
        }
        if (i == st) { wbuf_putc(out, '&'); return 1; }
        if (i < n && s[i] == ';') i++;
        if (big || v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
        else if (v >= 0x80 && v <= 0x9F) v = C1_FIX[v - 0x80];
        char u[4];
        wbuf_put(out, u, w_utf8_enc(v, u));
        return i;
    }
    if (!w_isalnum((unsigned char)c)) { wbuf_putc(out, '&'); return 1; }
    int maxl = n - 1 < WENT_MAXLEN ? n - 1 : WENT_MAXLEN;
    // the longest run of name characters (+ ';')
    int run = 0;
    while (run < maxl && w_isalnum((unsigned char)s[1 + run])) run++;
    if (run < maxl && s[1 + run] == ';') run++;
    for (int l = run; l >= 1; l--) {
        int e = went_find(s + 1, l);
        if (e < 0) continue;
        int semi = s[l] == ';';
        if (!semi && in_attr) {
            char nx = (1 + l < n) ? s[1 + l] : 0;
            if (nx == '=' || w_isalnum((unsigned char)nx)) { wbuf_putc(out, '&'); return 1; }
        }
        const char* u = went_table[e].utf8;
        // entity values may contain NUL bytes only for none; use strlen
        wbuf_put(out, u, (int)strlen(u));
        return 1 + l;
    }
    wbuf_putc(out, '&');
    return 1;
}

static void flush_chars(struct hp* p);
static void process(struct hp* p, struct tok* t);

static void emit(struct hp* p, struct tok* t) {
    if (t->type != TK_CHARS) flush_chars(p);
    process(p, t);
}

static void flush_chars(struct hp* p) {
    if (!p->chars.len) return;
    struct tok t;
    t.type = TK_CHARS;
    // process() may append new chars (never), but copy defensively by
    // detaching the buffer for the duration.
    struct wbuf b = p->chars;
    p->chars.p = 0; p->chars.len = p->chars.cap = 0;
    t.chars = b.p;
    t.clen = b.len;
    process(p, &t);
    // reuse the storage
    if (!p->chars.p) { p->chars = b; p->chars.len = 0; }
    else wbuf_free(&b);
}

// Read a tag starting at '<'. Returns 0 when the bytes are not a tag (so the
// caller emits '<' as text).
static int read_tag(struct hp* p) {
    const char* s = p->in;
    int n = p->len;
    int i = p->pos + 1;
    int end = 0;
    if (i < n && s[i] == '/') { end = 1; i++; }
    if (i >= n || !w_isalpha((unsigned char)s[i])) return 0;
    struct tok t;
    t.type = end ? TK_END : TK_START;
    t.attrs = p->attr_store;
    t.self_closing = 0;
    t.nattr = 0;
    t.nlen = 0;
    p->vals.len = 0;
    while (i < n && !w_isspace((unsigned char)s[i]) && s[i] != '/' && s[i] != '>') {
        if (t.nlen < 63) t.name[t.nlen++] = (char)w_lower((unsigned char)s[i]);
        i++;
    }
    t.name[t.nlen] = 0;
    for (;;) {
        while (i < n && w_isspace((unsigned char)s[i])) i++;
        if (i >= n) { p->pos = n; return 1; } // EOF in tag: dropped
        if (s[i] == '>') { i++; break; }
        if (s[i] == '/') {
            i++;
            if (i < n && s[i] == '>') { t.self_closing = 1; i++; break; }
            continue;
        }
        // attribute name
        int ns = i;
        char nm[96];
        int nl = 0;
        do {
            if (nl < 95) nm[nl++] = (char)w_lower((unsigned char)s[i]);
            i++;
        } while (i < n && !w_isspace((unsigned char)s[i]) && s[i] != '/' && s[i] != '>' && s[i] != '=');
        (void)ns;
        nm[nl] = 0;
        int j = i;
        while (j < n && w_isspace((unsigned char)s[j])) j++;
        uint32_t voff = (uint32_t)p->vals.len;
        if (j < n && s[j] == '=') {
            i = j + 1;
            while (i < n && w_isspace((unsigned char)s[i])) i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                while (i < n && s[i] != q) {
                    if (s[i] == '&') { i += charref(p, i, 1, &p->vals); continue; }
                    if (s[i] == 0) { wbuf_put(&p->vals, "\xEF\xBF\xBD", 3); i++; continue; }
                    int k = i;
                    while (k < n && s[k] != q && s[k] != '&' && s[k] != 0) k++;
                    wbuf_put(&p->vals, s + i, k - i);
                    i = k;
                }
                if (i < n) i++;
            } else {
                while (i < n && !w_isspace((unsigned char)s[i]) && s[i] != '>') {
                    if (s[i] == '&') { i += charref(p, i, 1, &p->vals); continue; }
                    wbuf_putc(&p->vals, s[i] ? s[i] : '?');
                    i++;
                }
            }
        }
        int vlen = p->vals.len - (int)voff;
        // duplicate attributes: first wins
        int dup = 0;
        for (int a = 0; a < t.nattr; a++)
            if (t.attrs[a].rawlen == nl && !memcmp(t.attrs[a].raw, nm, nl)) dup = 1;
        if (!dup && t.nattr < MAXATTR) {
            struct tattr* ta = &t.attrs[t.nattr++];
            ta->rawlen = nl;
            memcpy(ta->raw, nm, ta->rawlen);
            ta->voff = voff;
            ta->vlen = vlen;
            ta->name = 0; // interned lazily (end tags discard attributes)
        }
    }
    p->pos = i;
    t.tag = watom_intern(&D->atoms, t.name, t.nlen);
    if (t.type == TK_START)
        for (int a = 0; a < t.nattr; a++)
            t.attrs[a].name = watom_intern(&D->atoms, t.attrs[a].raw, t.attrs[a].rawlen);
    emit(p, &t);
    return 1;
}

static void emit_comment(struct hp* p, const char* s, int len) {
    struct tok t;
    t.type = TK_COMMENT;
    t.chars = s;
    t.clen = len;
    emit(p, &t);
}

static const char* const QUIRK_PREFIX[] = {
    "+//silmaril//dtd html pro v0r11 19970101//", "-//as//dtd html 3.0 aswedit + extensions//",
    "-//advasoft ltd//dtd html 3.0 aswedit + extensions//", "-//ietf//dtd html 2.0",
    "-//ietf//dtd html 3", "-//ietf//dtd html level", "-//ietf//dtd html strict",
    "-//ietf//dtd html//", "-//metrius//dtd metrius presentational//",
    "-//microsoft//dtd internet explorer", "-//netscape comm. corp.//dtd",
    "-//o'reilly and associates//dtd html", "-//softquad", "-//spyglass//dtd html 2.0",
    "-//sq//dtd html 2.0", "-//sun microsystems corp.//dtd hotjava", "-//w3c//dtd html 3",
    "-//w3c//dtd html 4.0 frameset//", "-//w3c//dtd html 4.0 transitional//",
    "-//w3c//dtd html experimental", "-//w3c//dtd w3 html//", "-//w3o//dtd w3 html",
    "-//webtechs//dtd mozilla html", 0
};

static int starts_ci(const char* s, int n, const char* lit) {
    return w_ieq_prefix(s, n, lit);
}

static void read_doctype(struct hp* p, int i) {
    const char* s = p->in;
    int n = p->len;
    int end = i;
    while (end < n && s[end] != '>') end++;
    // name
    int k = i;
    while (k < end && w_isspace((unsigned char)s[k])) k++;
    int ns = k;
    while (k < end && !w_isspace((unsigned char)s[k])) k++;
    struct tok t;
    t.type = TK_DOCTYPE;
    t.dt_quirk = 0;
    if (!w_ieq(s + ns, k - ns, "html")) t.dt_quirk = 2;
    // public / system ids
    char pub[128]; int pl = -1, has_sys = 0;
    while (k < end && w_isspace((unsigned char)s[k])) k++;
    if (k + 6 <= end && w_ieq_prefix(s + k, end - k, "public")) {
        k += 6;
        while (k < end && w_isspace((unsigned char)s[k])) k++;
        if (k < end && (s[k] == '"' || s[k] == '\'')) {
            char q = s[k++];
            pl = 0;
            while (k < end && s[k] != q) { if (pl < 127) pub[pl++] = (char)w_lower((unsigned char)s[k]); k++; }
            pub[pl] = 0;
            if (k < end) k++;
            while (k < end && w_isspace((unsigned char)s[k])) k++;
            if (k < end && (s[k] == '"' || s[k] == '\'')) has_sys = 1;
        }
    } else if (k + 6 <= end && w_ieq_prefix(s + k, end - k, "system")) {
        has_sys = 1;
    }
    if (pl >= 0) {
        for (int q = 0; QUIRK_PREFIX[q]; q++)
            if (starts_ci(pub, pl, QUIRK_PREFIX[q])) t.dt_quirk = 2;
        if (w_ieq(pub, pl, "-//w3o//dtd w3 html strict 3.0//en//") ||
            w_ieq(pub, pl, "-/w3c/dtd html 4.0 transitional/en") || w_ieq(pub, pl, "html"))
            t.dt_quirk = 2;
        if (starts_ci(pub, pl, "-//w3c//dtd html 4.01 frameset//") ||
            starts_ci(pub, pl, "-//w3c//dtd html 4.01 transitional//"))
            t.dt_quirk = has_sys ? (t.dt_quirk ? t.dt_quirk : 1) : 2;
        if (starts_ci(pub, pl, "-//w3c//dtd xhtml 1.0 frameset//") ||
            starts_ci(pub, pl, "-//w3c//dtd xhtml 1.0 transitional//"))
            if (!t.dt_quirk) t.dt_quirk = 1;
    }
    p->pos = end < n ? end + 1 : n;
    emit(p, &t);
}

static int foreign_current(struct hp* p) {
    int c = cur(p);
    return c >= 0 && N(c).ns != NS_HTML;
}

// '<!' constructs at p->pos.
static void read_markup_decl(struct hp* p) {
    const char* s = p->in;
    int n = p->len;
    int i = p->pos + 2;
    if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
        i += 2;
        if (i < n && s[i] == '>') { p->pos = i + 1; emit_comment(p, "", 0); return; }
        if (i + 1 < n && s[i] == '-' && s[i + 1] == '>') { p->pos = i + 2; emit_comment(p, "", 0); return; }
        int st = i;
        while (i < n) {
            if (s[i] == '-' && i + 2 < n && s[i + 1] == '-' && s[i + 2] == '>') {
                p->pos = i + 3; emit_comment(p, s + st, i - st); return;
            }
            if (s[i] == '-' && i + 3 < n && s[i + 1] == '-' && s[i + 2] == '!' && s[i + 3] == '>') {
                p->pos = i + 4; emit_comment(p, s + st, i - st); return;
            }
            i++;
        }
        p->pos = n;
        emit_comment(p, s + st, n - st);
        return;
    }
    if (i + 7 <= n && w_ieq_prefix(s + i, n - i, "doctype")) { read_doctype(p, i + 7); return; }
    if (i + 7 <= n && !memcmp(s + i, "[CDATA[", 7) && foreign_current(p)) {
        i += 7;
        int st = i;
        while (i + 2 < n && !(s[i] == ']' && s[i + 1] == ']' && s[i + 2] == '>')) i++;
        int e = i + 2 < n ? i : n;
        wbuf_put(&p->chars, s + st, e - st);
        p->pos = e < n ? e + 3 : n;
        return;
    }
    // bogus comment
    int st = i;
    while (i < n && s[i] != '>') i++;
    p->pos = i < n ? i + 1 : n;
    emit_comment(p, s + st, i - st);
}

static void data_state(struct hp* p) {
    const char* s = p->in;
    int n = p->len;
    while (p->pos < n && p->tstate == TS_DATA && !p->stopped) {
        int i = p->pos;
        int k = i;
        while (k < n && s[k] != '<' && s[k] != '&' && s[k] != 0) k++;
        if (k > i) wbuf_put(&p->chars, s + i, k - i);
        p->pos = k;
        if (k >= n) break;
        if (s[k] == 0) { p->pos++; continue; } // NUL in data: dropped
        if (s[k] == '&') { p->pos += charref(p, k, 0, &p->chars); continue; }
        // '<'
        if (k + 1 < n && s[k + 1] == '!') { read_markup_decl(p); continue; }
        if (k + 1 < n && s[k + 1] == '?') { // bogus comment
            int j = k + 1;
            while (j < n && s[j] != '>') j++;
            p->pos = j < n ? j + 1 : n;
            emit_comment(p, s + k + 1, j - k - 1);
            continue;
        }
        if (k + 2 < n && s[k + 1] == '/' && s[k + 2] == '>') { p->pos = k + 3; continue; }
        if (k + 1 < n && s[k + 1] == '/' && !w_isalpha((unsigned char)s[k + 2])) {
            int j = k + 2;
            while (j < n && s[j] != '>') j++;
            p->pos = j < n ? j + 1 : n;
            emit_comment(p, s + k + 2, j - k - 2);
            continue;
        }
        if (!read_tag(p)) { wbuf_putc(&p->chars, '<'); p->pos = k + 1; }
    }
}

// RCDATA / RAWTEXT / script data: everything up to the appropriate end tag.
static void raw_state(struct hp* p) {
    const char* s = p->in;
    int n = p->len;
    int nl;
    const char* name = watom_name(&D->atoms, p->raw_tag, &nl);
    int i = p->pos;
    while (i < n) {
        if (s[i] == '<' && i + 1 < n && s[i + 1] == '/' && i + 2 + nl <= n &&
            w_ieq_prefix(s + i + 2, n - i - 2, name)) {
            char after = i + 2 + nl < n ? s[i + 2 + nl] : '>';
            if (w_isspace((unsigned char)after) || after == '/' || after == '>') break;
        }
        if (p->tstate == TS_RCDATA && s[i] == '&') {
            if (i > p->pos) wbuf_put(&p->chars, s + p->pos, i - p->pos);
            i += charref(p, i, 0, &p->chars);
            p->pos = i;
            continue;
        }
        if (s[i] == 0) {
            if (i > p->pos) wbuf_put(&p->chars, s + p->pos, i - p->pos);
            wbuf_put(&p->chars, "\xEF\xBF\xBD", 3);
            p->pos = ++i;
            continue;
        }
        i++;
    }
    if (i > p->pos) wbuf_put(&p->chars, s + p->pos, i - p->pos);
    p->pos = i;
    if (i < n) {
        p->tstate = TS_DATA;
        read_tag(p);
    }
}

// ---- tree construction ---------------------------------------------------------

static int is_ws_str(const char* s, int n) {
    for (int i = 0; i < n; i++)
        if (!(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) return 0;
    return 1;
}

static int ws_prefix(const char* s, int n) {
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\f' || s[i] == '\r')) i++;
    return i;
}

static void start_raw(struct hp* p, struct tok* t, int state) {
    insert_element(p, t, NS_HTML);
    p->tstate = state;
    p->raw_tag = t->tag;
    p->orig_mode = p->mode;
    p->mode = M_TEXT;
}

static void in_body(struct hp* p, struct tok* t);
static void in_head(struct hp* p, struct tok* t);
static void in_table(struct hp* p, struct tok* t);
static void in_select(struct hp* p, struct tok* t);

static void merge_attrs(struct hp* p, int el, struct tok* t) {
    if (el < 0) return;
    for (int i = 0; i < t->nattr; i++)
        if (t->attrs[i].name && !wdom_has_attr(D, el, t->attrs[i].name))
            wdom_set_attr(D, el, t->attrs[i].name, p->vals.p + t->attrs[i].voff, t->attrs[i].vlen);
}

static void any_other_end(struct hp* p, int tag) {
    for (int i = p->sp - 1; i >= 0; i--) {
        int el = p->stack[i];
        if (is_html(p, el, tag)) {
            gen_implied(p, tag);
            pop_until_el(p, el);
            return;
        }
        if (is_special(p, el)) return;
    }
}

static void body_chars(struct hp* p, const char* s, int n) {
    if (p->skip_lf) {
        p->skip_lf = 0;
        if (n > 0 && s[0] == '\n') { s++; n--; }
    }
    if (n <= 0) return;
    reconstruct_afe(p);
    insert_chars(p, s, n);
    if (!is_ws_str(s, n)) p->frameset_ok = 0;
}

static void close_cell(struct hp* p) {
    gen_implied(p, 0);
    for (int i = p->sp - 1; i >= 0; i--) {
        int el = p->stack[i];
        if (is_html(p, el, T_td) || is_html(p, el, T_th)) { pop_until_el(p, el); break; }
    }
    afe_clear_to_marker(p);
    p->mode = M_IN_ROW;
}

static void clear_to_table_ctx(struct hp* p) {
    while (p->sp > 1) {
        int c = cur(p);
        if (is_html(p, c, T_table) || is_html(p, c, T_template) || is_html(p, c, T_html)) return;
        pop(p);
    }
}
static void clear_to_tbody_ctx(struct hp* p) {
    while (p->sp > 1) {
        int c = cur(p);
        if (is_html(p, c, T_tbody) || is_html(p, c, T_tfoot) || is_html(p, c, T_thead) ||
            is_html(p, c, T_template) || is_html(p, c, T_html)) return;
        pop(p);
    }
}
static void clear_to_row_ctx(struct hp* p) {
    while (p->sp > 1) {
        int c = cur(p);
        if (is_html(p, c, T_tr) || is_html(p, c, T_template) || is_html(p, c, T_html)) return;
        pop(p);
    }
}

static int has_attr_val(struct hp* p, struct tok* t, int name, const char* val) {
    for (int i = 0; i < t->nattr; i++)
        if (t->attrs[i].name == name)
            return w_ieq(p->vals.p + t->attrs[i].voff, t->attrs[i].vlen, val);
    return 0;
}

static void in_body(struct hp* p, struct tok* t) {
    int tg = t->tag;
    switch (t->type) {
    case TK_CHARS:
        body_chars(p, t->chars, t->clen);
        return;
    case TK_COMMENT:
        insert_comment(p, t, -1);
        return;
    case TK_DOCTYPE:
        return;
    case TK_EOF:
        p->stopped = 1;
        return;
    case TK_START:
        if (tg == T_html) { merge_attrs(p, p->stack[0], t); return; }
        if (tg == T_base || tg == T_basefont || tg == T_bgsound || tg == T_link ||
            tg == T_meta || tg == T_noframes || tg == T_script || tg == T_style ||
            tg == T_template || tg == T_title) { in_head(p, t); return; }
        if (tg == T_body) {
            if (p->sp > 1 && is_html(p, p->stack[1], T_body)) {
                p->frameset_ok = 0;
                merge_attrs(p, p->stack[1], t);
            }
            return;
        }
        if (tg == T_frameset) {
            if (!p->frameset_ok || p->sp < 2 || !is_html(p, p->stack[1], T_body)) return;
            wdom_remove(D, p->stack[1]);
            while (p->sp > 1) pop(p);
            insert_element(p, t, NS_HTML);
            p->mode = M_IN_FRAMESET;
            return;
        }
        if (tg == T_address || tg == T_article || tg == T_aside || tg == T_blockquote ||
            tg == T_center || tg == T_details || tg == T_dialog || tg == T_dir ||
            tg == T_div || tg == T_dl || tg == T_fieldset || tg == T_figcaption ||
            tg == T_figure || tg == T_footer || tg == T_header || tg == T_hgroup ||
            tg == T_main || tg == T_menu || tg == T_nav || tg == T_ol || tg == T_p ||
            tg == T_search || tg == T_section || tg == T_summary || tg == T_ul) {
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tag_in(tg, HEADINGS)) {
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            int c = cur(p);
            if (c >= 0 && N(c).ns == NS_HTML && tag_in(N(c).tag, HEADINGS)) pop(p);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_pre || tg == T_listing) {
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            insert_element(p, t, NS_HTML);
            p->skip_lf = 1;
            p->frameset_ok = 0;
            return;
        }
        if (tg == T_form) {
            if (p->form >= 0) return;
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            p->form = insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_li || tg == T_dd || tg == T_dt) {
            p->frameset_ok = 0;
            for (int i = p->sp - 1; i >= 0; i--) {
                int el = p->stack[i];
                if (tg == T_li ? is_html(p, el, T_li)
                               : (is_html(p, el, T_dd) || is_html(p, el, T_dt))) {
                    gen_implied(p, N(el).tag);
                    pop_until_el(p, el);
                    break;
                }
                if (is_special(p, el) && !is_html(p, el, T_address) && !is_html(p, el, T_div) &&
                    !is_html(p, el, T_p))
                    break;
            }
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_plaintext) {
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            insert_element(p, t, NS_HTML);
            p->tstate = TS_PLAINTEXT;
            return;
        }
        if (tg == T_button) {
            if (in_scope(p, T_button, SC_DEFAULT)) {
                gen_implied(p, 0);
                pop_until(p, T_button);
            }
            reconstruct_afe(p);
            insert_element(p, t, NS_HTML);
            p->frameset_ok = 0;
            return;
        }
        if (tg == T_a) {
            for (int i = p->nafe - 1; i >= 0 && p->afe[i] != MARKER; i--) {
                if (is_html(p, p->afe[i], T_a)) {
                    int a = p->afe[i];
                    adoption_agency(p, T_a);
                    afe_remove(p, a);
                    remove_from_stack(p, a);
                    break;
                }
            }
            reconstruct_afe(p);
            afe_push(p, insert_element(p, t, NS_HTML));
            return;
        }
        if (tg == T_b || tg == T_big || tg == T_code || tg == T_em || tg == T_font ||
            tg == T_i || tg == T_s || tg == T_small || tg == T_strike || tg == T_strong ||
            tg == T_tt || tg == T_u) {
            reconstruct_afe(p);
            int el = insert_element(p, t, NS_HTML);
            if (el >= 0) afe_push(p, el);
            return;
        }
        if (tg == T_nobr) {
            reconstruct_afe(p);
            if (in_scope(p, T_nobr, SC_DEFAULT)) { adoption_agency(p, T_nobr); reconstruct_afe(p); }
            int el = insert_element(p, t, NS_HTML);
            if (el >= 0) afe_push(p, el);
            return;
        }
        if (tg == T_applet || tg == T_marquee || tg == T_object) {
            reconstruct_afe(p);
            insert_element(p, t, NS_HTML);
            afe_marker(p);
            p->frameset_ok = 0;
            return;
        }
        if (tg == T_table) {
            if (!D->quirks && in_scope(p, T_p, SC_BUTTON)) close_p(p);
            insert_element(p, t, NS_HTML);
            p->frameset_ok = 0;
            p->mode = M_IN_TABLE;
            return;
        }
        if (tg == T_area || tg == T_br || tg == T_embed || tg == T_img || tg == T_keygen ||
            tg == T_wbr || tg == T_image) {
            if (tg == T_image) t->tag = tg = T_img;
            reconstruct_afe(p);
            insert_element(p, t, NS_HTML);
            pop(p);
            p->frameset_ok = 0;
            return;
        }
        if (tg == T_input) {
            reconstruct_afe(p);
            insert_element(p, t, NS_HTML);
            pop(p);
            if (!has_attr_val(p, t, A_type, "hidden")) p->frameset_ok = 0;
            return;
        }
        if (tg == T_param || tg == T_source || tg == T_track) {
            insert_element(p, t, NS_HTML);
            pop(p);
            return;
        }
        if (tg == T_hr) {
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            insert_element(p, t, NS_HTML);
            pop(p);
            p->frameset_ok = 0;
            return;
        }
        if (tg == T_textarea) {
            insert_element(p, t, NS_HTML);
            p->skip_lf = 1;
            p->tstate = TS_RCDATA;
            p->raw_tag = T_textarea;
            p->orig_mode = p->mode;
            p->frameset_ok = 0;
            p->mode = M_TEXT;
            return;
        }
        if (tg == T_xmp) {
            if (in_scope(p, T_p, SC_BUTTON)) close_p(p);
            reconstruct_afe(p);
            p->frameset_ok = 0;
            start_raw(p, t, TS_RAWTEXT);
            return;
        }
        if (tg == T_iframe) { p->frameset_ok = 0; start_raw(p, t, TS_RAWTEXT); return; }
        if (tg == T_noembed || (tg == T_noscript && D->scripting)) { start_raw(p, t, TS_RAWTEXT); return; }
        if (tg == T_select) {
            reconstruct_afe(p);
            insert_element(p, t, NS_HTML);
            p->frameset_ok = 0;
            if (p->mode == M_IN_TABLE || p->mode == M_IN_CAPTION || p->mode == M_IN_TBODY ||
                p->mode == M_IN_ROW || p->mode == M_IN_CELL)
                p->mode = M_IN_SELECT_IN_TABLE;
            else p->mode = M_IN_SELECT;
            return;
        }
        if (tg == T_optgroup || tg == T_option) {
            if (is_cur(p, T_option)) pop(p);
            reconstruct_afe(p);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_rb || tg == T_rtc) {
            if (in_scope(p, T_ruby, SC_DEFAULT)) gen_implied(p, 0);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_rp || tg == T_rt) {
            if (in_scope(p, T_ruby, SC_DEFAULT)) gen_implied(p, T_rtc);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_math || tg == T_svg) {
            reconstruct_afe(p);
            insert_element(p, t, tg == T_svg ? NS_SVG : NS_MATH);
            if (t->self_closing) pop(p);
            return;
        }
        if (tg == T_caption || tg == T_col || tg == T_colgroup || tg == T_frame ||
            tg == T_head || tg == T_tbody || tg == T_td || tg == T_tfoot || tg == T_th ||
            tg == T_thead || tg == T_tr)
            return; // parse error, ignored
        reconstruct_afe(p);
        insert_element(p, t, NS_HTML);
        return;
    case TK_END:
        if (tg == T_template) { in_head(p, t); return; }
        if (tg == T_body || tg == T_html) {
            if (!in_scope(p, T_body, SC_DEFAULT)) return;
            p->mode = M_AFTER_BODY;
            if (tg == T_html) process(p, t);
            return;
        }
        if (tg == T_address || tg == T_article || tg == T_aside || tg == T_blockquote ||
            tg == T_button || tg == T_center || tg == T_details || tg == T_dialog ||
            tg == T_dir || tg == T_div || tg == T_dl || tg == T_fieldset ||
            tg == T_figcaption || tg == T_figure || tg == T_footer || tg == T_header ||
            tg == T_hgroup || tg == T_listing || tg == T_main || tg == T_menu ||
            tg == T_nav || tg == T_ol || tg == T_pre || tg == T_search || tg == T_section ||
            tg == T_summary || tg == T_ul) {
            if (!in_scope(p, tg, SC_DEFAULT)) return;
            gen_implied(p, 0);
            pop_until(p, tg);
            return;
        }
        if (tg == T_form) {
            int node = p->form;
            p->form = -1;
            if (node < 0 || !in_scope_el(p, node, SC_DEFAULT)) return;
            gen_implied(p, 0);
            remove_from_stack(p, node);
            return;
        }
        if (tg == T_p) {
            if (!in_scope(p, T_p, SC_BUTTON)) insert_html(p, T_p);
            close_p(p);
            return;
        }
        if (tg == T_li) {
            if (!in_scope(p, T_li, SC_LIST)) return;
            gen_implied(p, T_li);
            pop_until(p, T_li);
            return;
        }
        if (tg == T_dd || tg == T_dt) {
            if (!in_scope(p, tg, SC_DEFAULT)) return;
            gen_implied(p, tg);
            pop_until(p, tg);
            return;
        }
        if (tag_in(tg, HEADINGS)) {
            if (!heading_in_scope(p)) return;
            gen_implied(p, 0);
            while (p->sp > 0) {
                int c = cur(p);
                pop(p);
                if (N(c).ns == NS_HTML && tag_in(N(c).tag, HEADINGS)) break;
            }
            return;
        }
        if (tag_in(tg, FORMATTING)) {
            if (!adoption_agency(p, tg)) any_other_end(p, tg);
            return;
        }
        if (tg == T_applet || tg == T_marquee || tg == T_object) {
            if (!in_scope(p, tg, SC_DEFAULT)) return;
            gen_implied(p, 0);
            pop_until(p, tg);
            afe_clear_to_marker(p);
            return;
        }
        if (tg == T_br) {
            t->type = TK_START;
            t->nattr = 0;
            in_body(p, t);
            return;
        }
        any_other_end(p, tg);
        return;
    }
}

static void in_head(struct hp* p, struct tok* t) {
    int tg = t->tag;
    if (t->type == TK_CHARS) {
        int w = ws_prefix(t->chars, t->clen);
        insert_chars(p, t->chars, w);
        if (w == t->clen) return;
        t->chars += w; t->clen -= w;
    } else if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
    else if (t->type == TK_DOCTYPE) return;
    else if (t->type == TK_START) {
        if (tg == T_html) { in_body(p, t); return; }
        if (tg == T_base || tg == T_basefont || tg == T_bgsound || tg == T_link || tg == T_meta) {
            insert_element(p, t, NS_HTML);
            pop(p);
            return;
        }
        if (tg == T_title) { start_raw(p, t, TS_RCDATA); return; }
        if (tg == T_noframes || tg == T_style || (tg == T_noscript && D->scripting)) {
            start_raw(p, t, TS_RAWTEXT);
            return;
        }
        if (tg == T_noscript) { insert_element(p, t, NS_HTML); p->mode = M_IN_HEAD_NOSCRIPT; return; }
        if (tg == T_script) { start_raw(p, t, TS_SCRIPT); return; }
        if (tg == T_template) {
            insert_element(p, t, NS_HTML);
            afe_marker(p);
            p->frameset_ok = 0;
            p->mode = M_IN_BODY; // simplified template handling
            return;
        }
        if (tg == T_head) return;
    } else if (t->type == TK_END) {
        if (tg == T_head) { pop(p); p->mode = M_AFTER_HEAD; return; }
        if (tg == T_template) {
            int found = 0;
            for (int i = p->sp - 1; i >= 0; i--) if (is_html(p, p->stack[i], T_template)) found = 1;
            if (!found) return;
            gen_implied_all(p);
            pop_until(p, T_template);
            afe_clear_to_marker(p);
            reset_mode(p);
            return;
        }
        if (tg != T_body && tg != T_html && tg != T_br) return;
    }
    // anything else
    pop(p); // head
    p->mode = M_AFTER_HEAD;
    process(p, t);
}

static void in_table_anything_else(struct hp* p, struct tok* t) {
    p->foster = 1;
    in_body(p, t);
    p->foster = 0;
}

static void in_table(struct hp* p, struct tok* t) {
    int tg = t->tag;
    int c = cur(p);
    if (t->type == TK_CHARS && c >= 0 && N(c).ns == NS_HTML &&
        (N(c).tag == T_table || N(c).tag == T_tbody || N(c).tag == T_template ||
         N(c).tag == T_tfoot || N(c).tag == T_thead || N(c).tag == T_tr)) {
        p->ptt.len = 0;
        p->ptt_nonws = 0;
        p->orig_mode = p->mode;
        p->mode = M_IN_TABLE_TEXT;
        process(p, t);
        return;
    }
    if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
    if (t->type == TK_DOCTYPE) return;
    if (t->type == TK_START) {
        if (tg == T_caption) {
            clear_to_table_ctx(p);
            afe_marker(p);
            insert_element(p, t, NS_HTML);
            p->mode = M_IN_CAPTION;
            return;
        }
        if (tg == T_colgroup) {
            clear_to_table_ctx(p);
            insert_element(p, t, NS_HTML);
            p->mode = M_IN_COLGROUP;
            return;
        }
        if (tg == T_col) {
            clear_to_table_ctx(p);
            insert_html(p, T_colgroup);
            p->mode = M_IN_COLGROUP;
            process(p, t);
            return;
        }
        if (tg == T_tbody || tg == T_tfoot || tg == T_thead) {
            clear_to_table_ctx(p);
            insert_element(p, t, NS_HTML);
            p->mode = M_IN_TBODY;
            return;
        }
        if (tg == T_td || tg == T_th || tg == T_tr) {
            clear_to_table_ctx(p);
            insert_html(p, T_tbody);
            p->mode = M_IN_TBODY;
            process(p, t);
            return;
        }
        if (tg == T_table) {
            if (!in_scope(p, T_table, SC_TABLE)) return;
            pop_until(p, T_table);
            reset_mode(p);
            process(p, t);
            return;
        }
        if (tg == T_style || tg == T_script || tg == T_template) { in_head(p, t); return; }
        if (tg == T_input && has_attr_val(p, t, A_type, "hidden")) {
            insert_element(p, t, NS_HTML);
            pop(p);
            return;
        }
        if (tg == T_form) {
            if (p->form >= 0) return;
            p->form = insert_element(p, t, NS_HTML);
            pop(p);
            return;
        }
    } else if (t->type == TK_END) {
        if (tg == T_table) {
            if (!in_scope(p, T_table, SC_TABLE)) return;
            pop_until(p, T_table);
            reset_mode(p);
            return;
        }
        if (tg == T_body || tg == T_caption || tg == T_col || tg == T_colgroup ||
            tg == T_html || tg == T_tbody || tg == T_td || tg == T_tfoot || tg == T_th ||
            tg == T_thead || tg == T_tr)
            return;
        if (tg == T_template) { in_head(p, t); return; }
    } else if (t->type == TK_EOF) {
        in_body(p, t);
        return;
    }
    in_table_anything_else(p, t);
}

static void in_table_text(struct hp* p, struct tok* t) {
    if (t->type == TK_CHARS) {
        wbuf_put(&p->ptt, t->chars, t->clen);
        if (!is_ws_str(t->chars, t->clen)) p->ptt_nonws = 1;
        return;
    }
    if (p->ptt.len) {
        if (p->ptt_nonws) {
            p->foster = 1;
            reconstruct_afe(p);
            insert_chars(p, p->ptt.p, p->ptt.len);
            p->frameset_ok = 0;
            p->foster = 0;
        } else {
            insert_chars(p, p->ptt.p, p->ptt.len);
        }
        p->ptt.len = 0;
    }
    p->mode = p->orig_mode;
    process(p, t);
}

static void in_caption(struct hp* p, struct tok* t) {
    int tg = t->tag;
    if ((t->type == TK_END && tg == T_caption) ||
        (t->type == TK_START && (tg == T_caption || tg == T_col || tg == T_colgroup ||
                                 tg == T_tbody || tg == T_td || tg == T_tfoot || tg == T_th ||
                                 tg == T_thead || tg == T_tr)) ||
        (t->type == TK_END && tg == T_table)) {
        if (!in_scope(p, T_caption, SC_TABLE)) return;
        gen_implied(p, 0);
        pop_until(p, T_caption);
        afe_clear_to_marker(p);
        p->mode = M_IN_TABLE;
        if (!(t->type == TK_END && tg == T_caption)) process(p, t);
        return;
    }
    if (t->type == TK_END && (tg == T_body || tg == T_col || tg == T_colgroup || tg == T_html ||
                              tg == T_tbody || tg == T_td || tg == T_tfoot || tg == T_th ||
                              tg == T_thead || tg == T_tr))
        return;
    in_body(p, t);
}

static void in_colgroup(struct hp* p, struct tok* t) {
    int tg = t->tag;
    if (t->type == TK_CHARS) {
        int w = ws_prefix(t->chars, t->clen);
        insert_chars(p, t->chars, w);
        if (w == t->clen) return;
        t->chars += w; t->clen -= w;
    } else if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
    else if (t->type == TK_DOCTYPE) return;
    else if (t->type == TK_START && tg == T_html) { in_body(p, t); return; }
    else if (t->type == TK_START && tg == T_col) { insert_element(p, t, NS_HTML); pop(p); return; }
    else if (t->type == TK_END && tg == T_colgroup) {
        if (is_cur(p, T_colgroup)) { pop(p); p->mode = M_IN_TABLE; }
        return;
    } else if (t->type == TK_END && tg == T_col) return;
    else if ((t->type == TK_START || t->type == TK_END) && tg == T_template) { in_head(p, t); return; }
    else if (t->type == TK_EOF) { in_body(p, t); return; }
    if (!is_cur(p, T_colgroup)) return;
    pop(p);
    p->mode = M_IN_TABLE;
    process(p, t);
}

static void in_tbody(struct hp* p, struct tok* t) {
    int tg = t->tag;
    if (t->type == TK_START && tg == T_tr) {
        clear_to_tbody_ctx(p);
        insert_element(p, t, NS_HTML);
        p->mode = M_IN_ROW;
        return;
    }
    if (t->type == TK_START && (tg == T_th || tg == T_td)) {
        clear_to_tbody_ctx(p);
        insert_html(p, T_tr);
        p->mode = M_IN_ROW;
        process(p, t);
        return;
    }
    if (t->type == TK_END && (tg == T_tbody || tg == T_tfoot || tg == T_thead)) {
        if (!in_scope(p, tg, SC_TABLE)) return;
        clear_to_tbody_ctx(p);
        pop(p);
        p->mode = M_IN_TABLE;
        return;
    }
    if ((t->type == TK_START && (tg == T_caption || tg == T_col || tg == T_colgroup ||
                                 tg == T_tbody || tg == T_tfoot || tg == T_thead)) ||
        (t->type == TK_END && tg == T_table)) {
        if (!in_scope(p, T_tbody, SC_TABLE) && !in_scope(p, T_thead, SC_TABLE) &&
            !in_scope(p, T_tfoot, SC_TABLE))
            return;
        clear_to_tbody_ctx(p);
        pop(p);
        p->mode = M_IN_TABLE;
        process(p, t);
        return;
    }
    if (t->type == TK_END && (tg == T_body || tg == T_caption || tg == T_col ||
                              tg == T_colgroup || tg == T_html || tg == T_td || tg == T_th ||
                              tg == T_tr))
        return;
    in_table(p, t);
}

static void in_row(struct hp* p, struct tok* t) {
    int tg = t->tag;
    if (t->type == TK_START && (tg == T_th || tg == T_td)) {
        clear_to_row_ctx(p);
        insert_element(p, t, NS_HTML);
        p->mode = M_IN_CELL;
        afe_marker(p);
        return;
    }
    if (t->type == TK_END && tg == T_tr) {
        if (!in_scope(p, T_tr, SC_TABLE)) return;
        clear_to_row_ctx(p);
        pop(p);
        p->mode = M_IN_TBODY;
        return;
    }
    if ((t->type == TK_START && (tg == T_caption || tg == T_col || tg == T_colgroup ||
                                 tg == T_tbody || tg == T_tfoot || tg == T_thead || tg == T_tr)) ||
        (t->type == TK_END && tg == T_table)) {
        if (!in_scope(p, T_tr, SC_TABLE)) return;
        clear_to_row_ctx(p);
        pop(p);
        p->mode = M_IN_TBODY;
        process(p, t);
        return;
    }
    if (t->type == TK_END && (tg == T_tbody || tg == T_tfoot || tg == T_thead)) {
        if (!in_scope(p, tg, SC_TABLE) || !in_scope(p, T_tr, SC_TABLE)) return;
        clear_to_row_ctx(p);
        pop(p);
        p->mode = M_IN_TBODY;
        process(p, t);
        return;
    }
    if (t->type == TK_END && (tg == T_body || tg == T_caption || tg == T_col ||
                              tg == T_colgroup || tg == T_html || tg == T_td || tg == T_th))
        return;
    in_table(p, t);
}

static void in_cell(struct hp* p, struct tok* t) {
    int tg = t->tag;
    if (t->type == TK_END && (tg == T_td || tg == T_th)) {
        if (!in_scope(p, tg, SC_TABLE)) return;
        gen_implied(p, 0);
        pop_until(p, tg);
        afe_clear_to_marker(p);
        p->mode = M_IN_ROW;
        return;
    }
    if (t->type == TK_START && (tg == T_caption || tg == T_col || tg == T_colgroup ||
                                tg == T_tbody || tg == T_td || tg == T_tfoot || tg == T_th ||
                                tg == T_thead || tg == T_tr)) {
        if (!in_scope(p, T_td, SC_TABLE) && !in_scope(p, T_th, SC_TABLE)) return;
        close_cell(p);
        process(p, t);
        return;
    }
    if (t->type == TK_END && (tg == T_body || tg == T_caption || tg == T_col ||
                              tg == T_colgroup || tg == T_html))
        return;
    if (t->type == TK_END && (tg == T_table || tg == T_tbody || tg == T_tfoot ||
                              tg == T_thead || tg == T_tr)) {
        if (!in_scope(p, tg, SC_TABLE)) return;
        close_cell(p);
        process(p, t);
        return;
    }
    in_body(p, t);
}

static void in_select(struct hp* p, struct tok* t) {
    int tg = t->tag;
    switch (t->type) {
    case TK_CHARS: insert_chars(p, t->chars, t->clen); return;
    case TK_COMMENT: insert_comment(p, t, -1); return;
    case TK_DOCTYPE: return;
    case TK_EOF: in_body(p, t); return;
    case TK_START:
        if (tg == T_html) { in_body(p, t); return; }
        if (tg == T_option) {
            if (is_cur(p, T_option)) pop(p);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_optgroup) {
            if (is_cur(p, T_option)) pop(p);
            if (is_cur(p, T_optgroup)) pop(p);
            insert_element(p, t, NS_HTML);
            return;
        }
        if (tg == T_hr) {
            if (is_cur(p, T_option)) pop(p);
            if (is_cur(p, T_optgroup)) pop(p);
            insert_element(p, t, NS_HTML);
            pop(p);
            return;
        }
        if (tg == T_select) {
            if (!in_scope(p, T_select, SC_SELECT)) return;
            pop_until(p, T_select);
            reset_mode(p);
            return;
        }
        if (tg == T_input || tg == T_keygen || tg == T_textarea) {
            if (!in_scope(p, T_select, SC_SELECT)) return;
            pop_until(p, T_select);
            reset_mode(p);
            process(p, t);
            return;
        }
        if (tg == T_script || tg == T_template) { in_head(p, t); return; }
        return;
    case TK_END:
        if (tg == T_optgroup) {
            if (is_cur(p, T_option) && p->sp > 1 && is_html(p, p->stack[p->sp - 2], T_optgroup)) pop(p);
            if (is_cur(p, T_optgroup)) pop(p);
            return;
        }
        if (tg == T_option) { if (is_cur(p, T_option)) pop(p); return; }
        if (tg == T_select) {
            if (!in_scope(p, T_select, SC_SELECT)) return;
            pop_until(p, T_select);
            reset_mode(p);
            return;
        }
        if (tg == T_template) { in_head(p, t); return; }
        return;
    }
}

static void in_select_in_table(struct hp* p, struct tok* t) {
    int tg = t->tag;
    int tbl = tg == T_caption || tg == T_table || tg == T_tbody || tg == T_tfoot ||
              tg == T_thead || tg == T_tr || tg == T_td || tg == T_th;
    if (t->type == TK_START && tbl) {
        pop_until(p, T_select);
        reset_mode(p);
        process(p, t);
        return;
    }
    if (t->type == TK_END && tbl) {
        if (!in_scope(p, tg, SC_TABLE)) return;
        pop_until(p, T_select);
        reset_mode(p);
        process(p, t);
        return;
    }
    in_select(p, t);
}

// Foreign content: HTML "breakout" start tags end the SVG/MathML subtree.
static const int BREAKOUT[] = {
    T_b, T_big, T_blockquote, T_body, T_br, T_center, T_code, T_dd, T_div, T_dl, T_dt,
    T_em, T_embed, T_h1, T_h2, T_h3, T_h4, T_h5, T_h6, T_head, T_hr, T_i, T_img, T_li,
    T_listing, T_menu, T_meta, T_nobr, T_ol, T_p, T_pre, T_ruby, T_s, T_small, T_span,
    T_strong, T_strike, T_sub, T_sup, T_table, T_tt, T_u, T_ul, T_var, 0
};

static int html_integration(struct hp* p, int el) {
    const struct wnode* n = &N(el);
    if (n->ns == NS_SVG) return n->tag == T_foreignobject || n->tag == T_desc || n->tag == T_title;
    return 0;
}
static int mathml_text_integration(struct hp* p, int el) {
    const struct wnode* n = &N(el);
    return n->ns == NS_MATH && (n->tag == T_mi || n->tag == T_mo || n->tag == T_mn ||
                                n->tag == T_ms || n->tag == T_mtext);
}

static void in_foreign(struct hp* p, struct tok* t) {
    if (t->type == TK_CHARS) {
        insert_chars(p, t->chars, t->clen);
        if (!is_ws_str(t->chars, t->clen)) p->frameset_ok = 0;
        return;
    }
    if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
    if (t->type == TK_DOCTYPE) return;
    if (t->type == TK_START) {
        int bo = tag_in(t->tag, BREAKOUT);
        if (t->tag == T_font) {
            for (int i = 0; i < t->nattr; i++)
                if (t->attrs[i].name == A_color || t->attrs[i].name == A_face ||
                    t->attrs[i].name == A_size) bo = 1;
        }
        if (bo) {
            while (p->sp > 1) {
                int c = cur(p);
                if (N(c).ns == NS_HTML || mathml_text_integration(p, c) || html_integration(p, c)) break;
                pop(p);
            }
            int saved = p->mode;
            (void)saved;
            process(p, t);
            return;
        }
        int ns = N(cur(p)).ns;
        insert_element(p, t, ns);
        if (t->self_closing) pop(p);
        return;
    }
    // end tags are resolved in process() (matching foreign element, else
    // the current HTML insertion mode)
}

static void process(struct hp* p, struct tok* t) {
    if (p->stopped) return;
    // tree construction dispatcher: foreign content first
    int c = cur(p);
    if (c >= 0 && N(c).ns != NS_HTML && t->type != TK_EOF) {
        int use_html = 0;
        if (mathml_text_integration(p, c) && (t->type == TK_CHARS || t->type == TK_START))
            use_html = 1;
        if (html_integration(p, c) && (t->type == TK_START || t->type == TK_CHARS)) use_html = 1;
        if (N(c).ns == NS_MATH && N(c).tag == T_annotation_xml && t->type == TK_START && t->tag == T_svg)
            use_html = 0;
        if (!use_html) {
            if (t->type == TK_END) {
                // matching foreign end tag?
                for (int i = p->sp - 1; i > 0; i--) {
                    int el = p->stack[i];
                    if (N(el).ns == NS_HTML) break;
                    int l;
                    const char* nm = watom_name(&D->atoms, N(el).tag, &l);
                    if (l == t->nlen && w_ieq(t->name, t->nlen, nm)) { pop_until_el(p, el); return; }
                }
                // otherwise process using the current insertion mode
            } else {
                in_foreign(p, t);
                return;
            }
        }
    }
    switch (p->mode) {
    case M_INITIAL:
        if (t->type == TK_CHARS) {
            int w = ws_prefix(t->chars, t->clen);
            if (w == t->clen) return;
            t->chars += w; t->clen -= w;
        } else if (t->type == TK_COMMENT) { insert_comment(p, t, 0); return; }
        else if (t->type == TK_DOCTYPE) {
            if (t->dt_quirk == 2) D->quirks = 1;
            p->mode = M_BEFORE_HTML;
            return;
        }
        D->quirks = 1; // no doctype
        p->mode = M_BEFORE_HTML;
        process(p, t);
        return;
    case M_BEFORE_HTML:
        if (t->type == TK_DOCTYPE) return;
        if (t->type == TK_COMMENT) { insert_comment(p, t, 0); return; }
        if (t->type == TK_CHARS) {
            int w = ws_prefix(t->chars, t->clen);
            if (w == t->clen) return;
            t->chars += w; t->clen -= w;
        }
        if (t->type == TK_START && t->tag == T_html) {
            int el = create_for_token(p, t, NS_HTML);
            wdom_append(D, 0, el);
            push(p, el);
            D->html = el;
            p->mode = M_BEFORE_HEAD;
            return;
        }
        if (t->type == TK_END && t->tag != T_head && t->tag != T_body && t->tag != T_html &&
            t->tag != T_br)
            return;
        {
            int el = wdom_create_element(D, NS_HTML, T_html);
            wdom_append(D, 0, el);
            push(p, el);
            D->html = el;
            p->mode = M_BEFORE_HEAD;
            process(p, t);
        }
        return;
    case M_BEFORE_HEAD:
        if (t->type == TK_CHARS) {
            int w = ws_prefix(t->chars, t->clen);
            if (w == t->clen) return;
            t->chars += w; t->clen -= w;
        } else if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
        else if (t->type == TK_DOCTYPE) return;
        else if (t->type == TK_START && t->tag == T_html) { in_body(p, t); return; }
        else if (t->type == TK_START && t->tag == T_head) {
            p->head = insert_element(p, t, NS_HTML);
            D->head = p->head;
            p->mode = M_IN_HEAD;
            return;
        } else if (t->type == TK_END && t->tag != T_head && t->tag != T_body &&
                   t->tag != T_html && t->tag != T_br)
            return;
        p->head = insert_html(p, T_head);
        D->head = p->head;
        p->mode = M_IN_HEAD;
        process(p, t);
        return;
    case M_IN_HEAD:
        in_head(p, t);
        return;
    case M_IN_HEAD_NOSCRIPT:
        if (t->type == TK_END && t->tag == T_noscript) { pop(p); p->mode = M_IN_HEAD; return; }
        if (t->type == TK_DOCTYPE) return;
        if ((t->type == TK_CHARS && is_ws_str(t->chars, t->clen)) || t->type == TK_COMMENT ||
            (t->type == TK_START && (t->tag == T_basefont || t->tag == T_bgsound ||
                                     t->tag == T_link || t->tag == T_meta ||
                                     t->tag == T_noframes || t->tag == T_style))) {
            in_head(p, t);
            return;
        }
        if (t->type == TK_START && t->tag == T_html) { in_body(p, t); return; }
        if (t->type == TK_START && (t->tag == T_head || t->tag == T_noscript)) return;
        if (t->type == TK_END && t->tag != T_br) return;
        pop(p);
        p->mode = M_IN_HEAD;
        process(p, t);
        return;
    case M_AFTER_HEAD:
        if (t->type == TK_CHARS) {
            int w = ws_prefix(t->chars, t->clen);
            insert_chars(p, t->chars, w);
            if (w == t->clen) return;
            t->chars += w; t->clen -= w;
        } else if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
        else if (t->type == TK_DOCTYPE) return;
        else if (t->type == TK_START) {
            if (t->tag == T_html) { in_body(p, t); return; }
            if (t->tag == T_body) {
                D->body = insert_element(p, t, NS_HTML);
                p->frameset_ok = 0;
                p->mode = M_IN_BODY;
                return;
            }
            if (t->tag == T_frameset) {
                insert_element(p, t, NS_HTML);
                p->mode = M_IN_FRAMESET;
                return;
            }
            if (t->tag == T_base || t->tag == T_basefont || t->tag == T_bgsound ||
                t->tag == T_link || t->tag == T_meta || t->tag == T_noframes ||
                t->tag == T_script || t->tag == T_style || t->tag == T_template ||
                t->tag == T_title) {
                if (p->head >= 0) push(p, p->head);
                in_head(p, t);
                if (p->head >= 0) remove_from_stack(p, p->head);
                return;
            }
            if (t->tag == T_head) return;
        } else if (t->type == TK_END) {
            if (t->tag == T_template) { in_head(p, t); return; }
            if (t->tag != T_body && t->tag != T_html && t->tag != T_br) return;
        }
        D->body = insert_html(p, T_body);
        p->mode = M_IN_BODY;
        process(p, t);
        return;
    case M_IN_BODY:
        in_body(p, t);
        return;
    case M_TEXT:
        if (t->type == TK_CHARS) {
            const char* s = t->chars;
            int n = t->clen;
            if (p->skip_lf) { p->skip_lf = 0; if (n > 0 && s[0] == '\n') { s++; n--; } }
            insert_chars(p, s, n);
            return;
        }
        if (t->type == TK_EOF) {
            pop(p);
            p->mode = p->orig_mode;
            process(p, t);
            return;
        }
        if (t->type == TK_END) {
            pop(p);
            p->mode = p->orig_mode;
            p->tstate = TS_DATA;
            return;
        }
        return;
    case M_IN_TABLE: in_table(p, t); return;
    case M_IN_TABLE_TEXT: in_table_text(p, t); return;
    case M_IN_CAPTION: in_caption(p, t); return;
    case M_IN_COLGROUP: in_colgroup(p, t); return;
    case M_IN_TBODY: in_tbody(p, t); return;
    case M_IN_ROW: in_row(p, t); return;
    case M_IN_CELL: in_cell(p, t); return;
    case M_IN_SELECT: in_select(p, t); return;
    case M_IN_SELECT_IN_TABLE: in_select_in_table(p, t); return;
    case M_AFTER_BODY:
        if (t->type == TK_CHARS && is_ws_str(t->chars, t->clen)) { in_body(p, t); return; }
        if (t->type == TK_COMMENT) { insert_comment(p, t, p->stack[0]); return; }
        if (t->type == TK_DOCTYPE) return;
        if (t->type == TK_START && t->tag == T_html) { in_body(p, t); return; }
        if (t->type == TK_END && t->tag == T_html) { p->mode = M_AFTER_AFTER_BODY; return; }
        if (t->type == TK_EOF) { p->stopped = 1; return; }
        p->mode = M_IN_BODY;
        process(p, t);
        return;
    case M_IN_FRAMESET:
    case M_AFTER_FRAMESET:
        if (t->type == TK_CHARS) {
            // keep only whitespace
            char buf[256];
            int o = 0;
            for (int i = 0; i < t->clen && o < 256; i++)
                if (w_isspace((unsigned char)t->chars[i])) buf[o++] = t->chars[i];
            insert_chars(p, buf, o);
            return;
        }
        if (t->type == TK_COMMENT) { insert_comment(p, t, -1); return; }
        if (t->type == TK_START && t->tag == T_html) { in_body(p, t); return; }
        if (p->mode == M_IN_FRAMESET) {
            if (t->type == TK_START && t->tag == T_frameset) { insert_element(p, t, NS_HTML); return; }
            if (t->type == TK_END && t->tag == T_frameset) {
                if (p->sp > 1) pop(p);
                if (!is_cur(p, T_frameset)) p->mode = M_AFTER_FRAMESET;
                return;
            }
            if (t->type == TK_START && t->tag == T_frame) { insert_element(p, t, NS_HTML); pop(p); return; }
        } else if (t->type == TK_END && t->tag == T_html) {
            p->mode = M_AFTER_AFTER_FRAMESET;
            return;
        }
        if (t->type == TK_START && t->tag == T_noframes) { in_head(p, t); return; }
        if (t->type == TK_EOF) p->stopped = 1;
        return;
    case M_AFTER_AFTER_BODY:
        if (t->type == TK_COMMENT) { insert_comment(p, t, 0); return; }
        if (t->type == TK_DOCTYPE || (t->type == TK_CHARS && is_ws_str(t->chars, t->clen)) ||
            (t->type == TK_START && t->tag == T_html)) { in_body(p, t); return; }
        if (t->type == TK_EOF) { p->stopped = 1; return; }
        p->mode = M_IN_BODY;
        process(p, t);
        return;
    case M_AFTER_AFTER_FRAMESET:
        if (t->type == TK_COMMENT) { insert_comment(p, t, 0); return; }
        if (t->type == TK_EOF) p->stopped = 1;
        return;
    }
}

static void run(struct hp* p) {
    while (p->pos < p->len && !p->stopped) {
        switch (p->tstate) {
        case TS_DATA: data_state(p); break;
        case TS_RCDATA:
        case TS_RAWTEXT:
        case TS_SCRIPT: raw_state(p); break;
        case TS_PLAINTEXT:
            wbuf_put(&p->chars, p->in + p->pos, p->len - p->pos);
            p->pos = p->len;
            break;
        }
    }
    struct tok t;
    t.type = TK_EOF;
    t.tag = 0;
    emit(p, &t);
}

static void hp_init(struct hp* p, struct wdom* d, const char* in, int len) {
    memset(p, 0, sizeof(*p));
    p->d = d;
    p->in = in;
    p->len = len;
    p->head = p->form = -1;
    p->frameset_ok = 1;
    p->mode = M_INITIAL;
}

static void hp_free(struct hp* p) {
    wbuf_free(&p->chars);
    wbuf_free(&p->vals);
    wbuf_free(&p->misc);
    wbuf_free(&p->ptt);
}

static void extract_title(struct wdom* d) {
    int t = wdom_first_tag(d, T_title);
    d->title[0] = 0;
    if (t < 0) return;
    char buf[512];
    int n = wdom_text_content(d, t, buf, sizeof buf);
    int o = 0, sp = 0;
    for (int i = 0; i < n && o < (int)sizeof(d->title) - 4; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (w_isspace(c)) { sp = 1; continue; }
        if (sp && o) d->title[o++] = ' ';
        sp = 0;
        d->title[o++] = (char)c;
    }
    // never end on a cut UTF-8 sequence: drop a trailing partial character
    int k = o;
    while (k > 0 && ((unsigned char)d->title[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0 && (unsigned char)d->title[k - 1] >= 0xC0) {
        uint32_t cp;
        if (w_utf8_dec(d->title + k - 1, o - k + 1, &cp) != o - k + 1) o = k - 1;
    }
    d->title[o] = 0;
}

struct wdom* whtml_parse(const char* bytes, int len, const char* charset_hint) {
    return whtml_parse_ex(bytes, len, charset_hint, 0);
}

struct wdom* whtml_parse_ex(const char* bytes, int len, const char* charset_hint, int scripting) {
    struct wdom* d = wdom_new();
    if (!d) return 0;
    d->scripting = scripting;
    char sniffed[32];
    const char* label = charset_hint;
    if (!label || !label[0]) {
        if (wcharset_sniff(bytes, len, sniffed, sizeof sniffed)) label = sniffed;
    }
    int ulen = 0;
    char* utf8 = wcharset_decode(bytes, len, label, &ulen, d->charset, sizeof d->charset);
    if (!utf8) { d->oom = 1; return d; }
    struct hp* p = (struct hp*)w_malloc(sizeof(struct hp));
    if (!p) { w_free(utf8); d->oom = 1; return d; }
    hp_init(p, d, utf8, ulen);
    run(p);
    hp_free(p);
    w_free(p);
    w_free(utf8);
    if (d->html < 0) d->html = wdom_first_tag(d, T_html);
    if (d->body < 0) {
        for (int c = d->html >= 0 ? d->n[d->html].first : -1; c >= 0; c = d->n[c].next)
            if (wdom_is(d, c, T_body) || wdom_is(d, c, T_frameset)) { d->body = c; break; }
    }
    extract_title(d);
    return d;
}

void whtml_parse_fragment(struct wdom* d, int parent, const char* utf8, int len) {
    whtml_parse_fragment_ctx(d, parent, T_body, utf8, len);
}

void whtml_parse_fragment_ctx(struct wdom* d, int parent, int ctx_tag, const char* utf8, int len) {
    struct hp* p = (struct hp*)w_malloc(sizeof(struct hp));
    if (!p) return;
    hp_init(p, d, utf8, len);
    int ctx = wdom_create_element(d, NS_HTML, T_html);
    if (ctx < 0) { w_free(p); return; }
    push(p, ctx);
    // "reset the insertion mode appropriately" for the context element
    switch (ctx_tag) {
    case T_table: p->mode = M_IN_TABLE; break;
    case T_tbody: case T_thead: case T_tfoot: p->mode = M_IN_TBODY; break;
    case T_tr: p->mode = M_IN_ROW; break;
    case T_td: case T_th: p->mode = M_IN_CELL; break;
    case T_select: p->mode = M_IN_SELECT; break;
    case T_colgroup: p->mode = M_IN_COLGROUP; break;
    case T_caption: p->mode = M_IN_CAPTION; break;
    default: p->mode = M_IN_BODY; break;
    }
    if (ctx_tag == T_title || ctx_tag == T_textarea) { p->tstate = TS_RCDATA; p->raw_tag = ctx_tag; }
    else if (ctx_tag == T_style || ctx_tag == T_xmp || ctx_tag == T_iframe || ctx_tag == T_noembed ||
             ctx_tag == T_noframes || (ctx_tag == T_noscript && d->scripting)) {
        p->tstate = TS_RAWTEXT; p->raw_tag = ctx_tag;
    } else if (ctx_tag == T_script) { p->tstate = TS_SCRIPT; p->raw_tag = ctx_tag; }
    else if (ctx_tag == T_plaintext) p->tstate = TS_PLAINTEXT;
    p->head = -2; // "head" already seen
    p->frameset_ok = 0;
    run(p);
    hp_free(p);
    w_free(p);
    while (d->n[ctx].first >= 0) wdom_append(d, parent, d->n[ctx].first);
}
