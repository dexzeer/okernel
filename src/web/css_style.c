// Cascade + computed values.
#include "css_int.h"
#include "wurl.h"

// ---- UA stylesheet ------------------------------------------------------------

static const char UA_CSS[] =
"html,address,blockquote,body,center,dialog,div,figure,figcaption,footer,form,header,hr,"
"legend,listing,main,p,plaintext,pre,search,xmp,details,summary,article,aside,h1,h2,h3,h4,h5,h6,"
"hgroup,nav,section,dir,dd,dl,dt,menu,ol,ul,fieldset,optgroup,frameset,frame{display:block}"
"head,link,meta,script,style,title,template,base,basefont,datalist,noembed,noframes,param,rp,"
"area,[hidden],input[type=hidden],option,embed[hidden],source,track{display:none}"
"dialog:not([open]){display:none}"
"li{display:list-item}"
"table{display:table}caption{display:table-caption}colgroup{display:table-column-group}"
"col{display:table-column}thead{display:table-header-group}tbody{display:table-row-group}"
"tfoot{display:table-footer-group}tr{display:table-row}td,th{display:table-cell}"
"img,video,canvas,iframe,embed,object,svg,input,button,select,textarea,meter,progress{display:inline-block}"
"html{color:canvastext;font-family:serif;font-size:medium;line-height:normal}"
"body{margin:8px}"
"p,blockquote,figure,dl,ol,ul,menu,dir,pre,listing,xmp,plaintext{margin-top:1em;margin-bottom:1em}"
"blockquote,figure{margin-left:40px;margin-right:40px}"
"dd{margin-left:40px}"
"ol,ul,menu,dir{padding-left:40px}"
"ol{list-style-type:decimal}ul,menu,dir{list-style-type:disc}"
"ul ul,ol ul,ul ol,ol ol,ul menu,ol menu,menu ul,menu ol,dl dl{margin-top:0;margin-bottom:0}"
"ul ul,ol ul,menu ul,ul menu{list-style-type:circle}"
"ul ul ul,ul ol ul,ol ul ul,ol ol ul{list-style-type:square}"
"h1{font-size:2em;margin-top:.67em;margin-bottom:.67em}"
"h2{font-size:1.5em;margin-top:.83em;margin-bottom:.83em}"
"h3{font-size:1.17em;margin-top:1em;margin-bottom:1em}"
"h4{margin-top:1.33em;margin-bottom:1.33em}"
"h5{font-size:.83em;margin-top:1.67em;margin-bottom:1.67em}"
"h6{font-size:.67em;margin-top:2.33em;margin-bottom:2.33em}"
"h1,h2,h3,h4,h5,h6,th,b,strong{font-weight:bold}"
"article h1,aside h1,nav h1,section h1{font-size:1.5em;margin-top:.83em;margin-bottom:.83em}"
"article article h1,article aside h1,article nav h1,article section h1,aside article h1,"
"aside aside h1,aside nav h1,aside section h1,nav article h1,nav aside h1,nav nav h1,"
"nav section h1,section article h1,section aside h1,section nav h1,section section h1"
"{font-size:1.17em;margin-top:1em;margin-bottom:1em}"
"b,strong{font-weight:bolder}"
"i,cite,em,var,dfn,address{font-style:italic}"
"code,kbd,samp,tt,pre,listing,xmp,plaintext{font-family:monospace}"
"pre,listing,xmp,plaintext{white-space:pre}"
"textarea{white-space:pre-wrap}"
"nobr{white-space:nowrap}"
"small{font-size:smaller}big{font-size:larger}"
"sub{vertical-align:sub;font-size:smaller}sup{vertical-align:super;font-size:smaller}"
"u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
"abbr[title],acronym[title]{text-decoration:underline dotted}"
"a:link{color:#0000EE;text-decoration:underline;cursor:pointer}"
"mark{background-color:yellow;color:black}"
"center{text-align:center}"
"hr{color:gray;border-style:inset;border-width:1px;margin-top:.5em;margin-bottom:.5em;"
"margin-left:auto;margin-right:auto;overflow:hidden}"
"table{border-spacing:2px;border-collapse:separate;box-sizing:border-box;text-indent:0}"
"td,th{padding:1px}"
"thead,tbody,tfoot,tr{vertical-align:middle}"
"td,th{vertical-align:inherit}"
"th{text-align:center}"
"caption{text-align:center}"
"fieldset{margin-left:2px;margin-right:2px;padding:.35em .75em .625em;border:2px groove #c0c0c0}"
"legend{padding-left:2px;padding-right:2px}"
"iframe{border:2px inset #767676}"
"input,select,button,textarea{font-family:sans-serif;font-size:13.333px;color:black;"
"letter-spacing:normal;word-spacing:normal;line-height:normal;text-transform:none;"
"text-indent:0;text-align:start;font-weight:normal;font-style:normal}"
"input,textarea,select{border:1px solid #767676;border-radius:2px;background-color:white;"
"padding:1px 2px}"
"textarea{font-family:monospace}"
"select{padding:0 0 0 4px}"
"button,input[type=submit],input[type=button],input[type=reset]{border:1px solid #767676;"
"border-radius:3px;background-color:#efefef;padding:1px 6px;text-align:center}"
"input[type=checkbox],input[type=radio]{border:1px solid #767676;width:13px;height:13px;"
"padding:0;margin:3px 3px 3px 4px}"
"input[type=radio]{border-radius:50%}"
"input[type=image]{border:0;padding:0;background-color:transparent}"
"details>summary:first-of-type{display:list-item;list-style-type:disclosure-closed;list-style-position:inside}"
"details[open]>summary:first-of-type{list-style-type:disclosure-open}"
"rt{font-size:50%}"
"q::before{content:open-quote}q::after{content:close-quote}"
"video{object-fit:contain}"
"svg:not(:root){overflow:hidden}"
"marquee{display:inline-block}"
"meter,progress{width:10em;height:1em;vertical-align:-0.2em}"
;

// Quirks-mode extras (HTML spec rendering section, tables)
static const char UA_QUIRKS_CSS[] =
"table{font-weight:normal;font-style:normal;font-variant:normal;font-size:medium;"
"line-height:normal;white-space:normal;text-align:start}"
"li{list-style-position:inside}";

static struct wsheet ua_sheet, ua_quirks;
static int ua_ready;

static void ua_init(void) {
    if (ua_ready) return;
    memset(&ua_sheet, 0, sizeof ua_sheet);
    memset(&ua_quirks, 0, sizeof ua_quirks);
    ua_sheet.origin = ua_quirks.origin = CSS_ORIGIN_UA;
    ua_sheet.outer_mq = ua_quirks.outer_mq = -1;
    csheet_parse(&ua_sheet, 0, UA_CSS, (int)sizeof(UA_CSS) - 1);
    csheet_parse(&ua_quirks, 0, UA_QUIRKS_CSS, (int)sizeof(UA_QUIRKS_CSS) - 1);
    ua_ready = 1;
}

// ---- structures -----------------------------------------------------------------

struct wcvar { uint32_t hash, name, nlen, val, vlen; };
struct wcvars { struct wcvars* parent; int n; struct wcvar v[]; };

struct ruleref {
    const struct wsheet* sh;
    int rule;
    uint32_t key;        // bucket key
    uint32_t order;      // global cascade order (sheet position, rule order)
    uint8_t origin;
    uint8_t nanc;        // required-ancestor hashes (ancestor Bloom filter)
    uint32_t anc[4];
};

// Ancestor Bloom filter: counts of (kind,atom) hashes of the elements on
// the current DFS path. A rule whose selector requires an ancestor with an
// id/class/tag absent from the filter cannot match.
#define BLOOM_BITS 4096
static uint8_t bloom[BLOOM_BITS];
static inline uint32_t bloom_hash(uint32_t key) { return key * 2654435761u; }
static inline void bloom_add(uint32_t h, int d) {
    bloom[h & (BLOOM_BITS - 1)] += (uint8_t)d;
    bloom[(h >> 12) & (BLOOM_BITS - 1)] += (uint8_t)d;
}
static inline int bloom_has(uint32_t h) {
    return bloom[h & (BLOOM_BITS - 1)] && bloom[(h >> 12) & (BLOOM_BITS - 1)];
}

#define MAX_SHEETS 64

struct wstyleset {
    struct wdom* d;
    struct wsheet* sheets[MAX_SHEETS];
    int nsheets;
    char doc_base[256];
    struct wsheet inl;       // inline style="" declarations (per pass)
    struct wsheet hints;     // presentational hints (per element)
    struct wsheet var_sh;    // var() substitution output (per element)
    // computed styles
    struct wstyle* styles;
    int nstyles, capstyles;
    int32_t* shash;
    int shcap;
    int32_t* node_style;
    int32_t* node_pseudo;    // 3 per node: before, after, marker
    int node_cap;
    struct wbuf strpool;
    int32_t* strhash;
    int strhcap, nstr;
    struct wgrad* grads;
    int ngrad, capgrad;
    // custom property blocks (freed with the set)
    struct wcvars** vblocks;
    int nvb, capvb;
    // rule index
    struct ruleref* refs;
    int nrefs, caprefs;
    uint32_t* bkeys;          // open addressing: key -> (start, count)
    int32_t* bstart;
    int32_t* bcount;
    int bcap;
    int index_dirty;
    int univ_start, univ_count;
    // media query results per sheet (index aligned with sheets[] + 2 UA)
    uint8_t* mqres[MAX_SHEETS + 2];
    int vw, vh;
    // imports bookkeeping (URLs already satisfied)
    struct wbuf imports_done;
    // scratch
    struct ruleref** match;
    int nmatch, capmatch;
};

static const struct wsheet* sheet_at(const struct wstyleset* ss, int i, int* mqi) {
    // 0 = UA, 1 = UA quirks, 2.. = author sheets
    *mqi = i;
    if (i == 0) return &ua_sheet;
    if (i == 1) return &ua_quirks;
    return ss->sheets[i - 2];
}

// ---- string pool ----------------------------------------------------------------

static uint32_t sh_hash(const char* s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

// Intern s into the computed-string pool; returns offset+1 (0 on failure).
static int32_t str_intern(struct wstyleset* ss, const char* s, int n) {
    if (n < 0) n = 0;
    if (ss->nstr * 2 >= ss->strhcap) {
        int nc = ss->strhcap ? ss->strhcap * 2 : 256;
        int32_t* nh = (int32_t*)w_malloc(nc * sizeof(int32_t));
        if (!nh) return 0;
        for (int i = 0; i < nc; i++) nh[i] = -1;
        for (int i = 0; i < ss->strhcap; i++) {
            int32_t o = ss->strhash[i];
            if (o < 0) continue;
            uint32_t len;
            memcpy(&len, ss->strpool.p + o, 4);
            uint32_t h = sh_hash(ss->strpool.p + o + 4, (int)len) & (nc - 1);
            while (nh[h] >= 0) h = (h + 1) & (nc - 1);
            nh[h] = o;
        }
        w_free(ss->strhash);
        ss->strhash = nh;
        ss->strhcap = nc;
    }
    uint32_t h = sh_hash(s, n) & (ss->strhcap - 1);
    while (ss->strhash[h] >= 0) {
        int32_t o = ss->strhash[h];
        uint32_t len;
        memcpy(&len, ss->strpool.p + o, 4);
        if ((int)len == n && !memcmp(ss->strpool.p + o + 4, s, n)) return o + 4 + 1;
        h = (h + 1) & (ss->strhcap - 1);
    }
    int32_t o = ss->strpool.len;
    uint32_t len = (uint32_t)n;
    wbuf_put(&ss->strpool, (const char*)&len, 4);
    wbuf_put(&ss->strpool, s, n);
    wbuf_putc(&ss->strpool, 0);
    ss->strhash[h] = o;
    ss->nstr++;
    return o + 4 + 1;
}

const char* css_str(const struct wstyleset* ss, int32_t ref, int32_t len) {
    (void)len;
    if (ref <= 0 || ref > ss->strpool.len) return "";
    return ss->strpool.p + ref - 1;
}

const struct wgrad* css_grad(const struct wstyleset* ss, int idx) {
    if (idx < 0 || idx >= ss->ngrad) return 0;
    return &ss->grads[idx];
}

void css_resolve_url(const char* base, const char* rel, int rlen, char* out, int cap) {
    if (wurl_resolve(base, rel, rlen, out, cap) < 0) {
        int n = rlen < cap - 1 ? rlen : cap - 1;
        if (n < 0) n = 0;
        memcpy(out, rel, n);
        out[n] = 0;
    }
}

// ---- set lifecycle ------------------------------------------------------------------

struct wstyleset* css_set_new(struct wdom* d) {
    ua_init();
    struct wstyleset* ss = (struct wstyleset*)w_calloc(1, sizeof(struct wstyleset));
    if (!ss) return 0;
    ss->d = d;
    ss->index_dirty = 1;
    ss->inl.origin = CSS_ORIGIN_AUTHOR;
    ss->hints.origin = CSS_ORIGIN_AUTHOR;
    ss->var_sh.origin = CSS_ORIGIN_AUTHOR;
    ss->inl.outer_mq = ss->hints.outer_mq = ss->var_sh.outer_mq = -1;
    return ss;
}

void css_set_doc_base(struct wstyleset* ss, const char* url) {
    int n = 0;
    while (url[n] && n < 255) { ss->doc_base[n] = url[n]; n++; }
    ss->doc_base[n] = 0;
    memcpy(ss->inl.base, ss->doc_base, sizeof ss->inl.base);
    memcpy(ss->hints.base, ss->doc_base, sizeof ss->hints.base);
}

int css_set_sheet_count(const struct wstyleset* ss) { return ss->nsheets; }

void css_set_free(struct wstyleset* ss) {
    if (!ss) return;
    for (int i = 0; i < ss->nsheets; i++) { csheet_free(ss->sheets[i]); w_free(ss->sheets[i]); }
    csheet_free(&ss->inl);
    csheet_free(&ss->hints);
    csheet_free(&ss->var_sh);
    w_free(ss->styles); w_free(ss->shash); w_free(ss->node_style); w_free(ss->node_pseudo);
    wbuf_free(&ss->strpool); w_free(ss->strhash); w_free(ss->grads);
    for (int i = 0; i < ss->nvb; i++) w_free(ss->vblocks[i]);
    w_free(ss->vblocks);
    w_free(ss->refs); w_free(ss->bkeys); w_free(ss->bstart); w_free(ss->bcount);
    for (int i = 0; i < MAX_SHEETS + 2; i++) w_free(ss->mqres[i]);
    wbuf_free(&ss->imports_done);
    w_free(ss->match);
    w_free(ss);
}

int css_set_add_sheet(struct wstyleset* ss, const char* text, int len, const char* base_url,
                      int before_idx) {
    return css_set_add_sheet_ex(ss, text, len, base_url, before_idx, 0, 0);
}

int css_set_add_sheet_ex(struct wstyleset* ss, const char* text, int len, const char* base_url,
                         int before_idx, const char* media, int media_len) {
    if (ss->nsheets >= MAX_SHEETS) return -1;
    struct wsheet* sh = (struct wsheet*)w_calloc(1, sizeof(struct wsheet));
    if (!sh) return -1;
    sh->origin = CSS_ORIGIN_AUTHOR;
    sh->outer_mq = -1;
    if (media && media_len > 0) csheet_set_media(sh, media, media_len);
    const char* b = base_url && base_url[0] ? base_url : ss->doc_base;
    int n = 0;
    while (b[n] && n < 255) { sh->base[n] = b[n]; n++; }
    sh->base[n] = 0;
    csheet_parse(sh, &ss->d->atoms, text, len);
    int at = (before_idx < 0 || before_idx > ss->nsheets) ? ss->nsheets : before_idx;
    for (int i = ss->nsheets; i > at; i--) ss->sheets[i] = ss->sheets[i - 1];
    ss->sheets[at] = sh;
    ss->nsheets++;
    ss->index_dirty = 1;
    return at;
}

int css_set_pending_imports(struct wstyleset* ss, char urls[][256], int* before, int max) {
    int n = 0;
    for (int i = 0; i < ss->nsheets && n < max; i++) {
        const struct wsheet* sh = ss->sheets[i];
        const char* p = sh->imports.p;
        for (int k = 0; k < sh->nimports && n < max; k++) {
            int l = (int)strlen(p);
            // already done?
            int done = 0;
            for (int q = 0; q < ss->imports_done.len;) {
                const char* u = ss->imports_done.p + q;
                int ul = (int)strlen(u);
                if (ul == l && !memcmp(u, p, l)) { done = 1; break; }
                q += ul + 1;
            }
            if (!done) {
                int c = l < 255 ? l : 255;
                memcpy(urls[n], p, c);
                urls[n][c] = 0;
                before[n] = i;
                n++;
            }
            p += l + 1;
        }
    }
    return n;
}

void css_set_mark_import_done(struct wstyleset* ss, const char* url) {
    wbuf_put(&ss->imports_done, url, (int)strlen(url) + 1);
}

int css_collect_inline(struct wdom* d, char* out, int cap) {
    int o = 0;
    for (int i = d->n[0].first; i >= 0; i = wdom_next(d, i, 0)) {
        if (!wdom_is(d, i, T_style)) continue;
        int ml;
        const char* media = wdom_attr(d, i, A_media, &ml);
        (void)media;
        o += wdom_text_content(d, i, out + o, cap - o);
        if (o < cap - 1) out[o++] = '\n';
        out[o] = 0;
    }
    return o;
}

void css_stats(const struct wstyleset* ss, int* rules, int* styles, int* sheets) {
    int r = 0;
    for (int i = 0; i < ss->nsheets; i++) r += ss->sheets[i]->nrule;
    *rules = r;
    *styles = ss->nstyles;
    *sheets = ss->nsheets;
}

// ---- rule index -----------------------------------------------------------------------

static uint32_t rule_key(const struct wsheet* sh, int sel) {
    int i = sh->sels[sel].first;
    // find the last compound
    int last = i;
    for (int k = i; sh->parts[k].kind != SK_END; k++)
        if (sh->parts[k].kind == SK_COMB) last = k + 1;
    uint32_t cls = 0, tag = 0;
    for (int k = last; sh->parts[k].kind != SK_END && sh->parts[k].kind != SK_COMB; k++) {
        const struct cpart* p = &sh->parts[k];
        if (p->kind == SK_ID && p->atom) return (1u << 16) | p->atom;
        if (p->kind == SK_CLASS && !cls) cls = (2u << 16) | p->atom;
        if (p->kind == SK_TAG && !tag) tag = (3u << 16) | p->atom;
    }
    if (cls) return cls;
    if (tag) return tag;
    for (int k = last; sh->parts[k].kind != SK_END && sh->parts[k].kind != SK_COMB; k++)
        if (sh->parts[k].kind == SK_ATTR && sh->parts[k].atom) return (4u << 16) | sh->parts[k].atom;
    return 0;
}

// Hashes of ids/classes/tags that must appear on some ancestor: compounds
// whose right-hand combinator is descendant or child.
static int rule_ancestors(const struct wsheet* sh, int sel, uint32_t* out) {
    int n = 0;
    int i = sh->sels[sel].first;
    int st = i;
    for (int k = i;; k++) {
        int kind = sh->parts[k].kind;
        if (kind == SK_COMB || kind == SK_END) {
            if (kind == SK_COMB && (sh->parts[k].op == CB_DESC || sh->parts[k].op == CB_CHILD)) {
                for (int q = st; q < k && n < 4; q++) {
                    const struct cpart* p = &sh->parts[q];
                    if (p->kind == SK_ID && p->atom) out[n++] = bloom_hash((1u << 16) | p->atom);
                    else if (p->kind == SK_CLASS && p->atom) out[n++] = bloom_hash((2u << 16) | p->atom);
                    else if (p->kind == SK_TAG && p->atom) out[n++] = bloom_hash((3u << 16) | p->atom);
                }
            }
            if (kind == SK_END) break;
            st = k + 1;
        }
    }
    return n;
}

static int ref_cmp_key(const struct ruleref* a, const struct ruleref* b) {
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static void sort_refs(struct ruleref* r, int n) {
    // heap sort (n can be tens of thousands)
    for (int start = n / 2 - 1; start >= 0; start--) {
        int root = start;
        for (;;) {
            int child = 2 * root + 1;
            if (child >= n) break;
            if (child + 1 < n && ref_cmp_key(&r[child], &r[child + 1]) < 0) child++;
            if (ref_cmp_key(&r[root], &r[child]) >= 0) break;
            struct ruleref t = r[root]; r[root] = r[child]; r[child] = t;
            root = child;
        }
    }
    for (int end = n - 1; end > 0; end--) {
        struct ruleref t = r[0]; r[0] = r[end]; r[end] = t;
        int root = 0;
        for (;;) {
            int child = 2 * root + 1;
            if (child >= end) break;
            if (child + 1 < end && ref_cmp_key(&r[child], &r[child + 1]) < 0) child++;
            if (ref_cmp_key(&r[root], &r[child]) >= 0) break;
            struct ruleref t2 = r[root]; r[root] = r[child]; r[child] = t2;
            root = child;
        }
    }
}

static void build_index(struct wstyleset* ss) {
    int total = 0;
    int nsrc = ss->nsheets + 2;
    for (int i = 0; i < nsrc; i++) { int m; total += sheet_at(ss, i, &m)->nrule; }
    if (total > ss->caprefs) {
        w_free(ss->refs);
        ss->refs = (struct ruleref*)w_malloc((total + 1) * sizeof(struct ruleref));
        ss->caprefs = ss->refs ? total : 0;
        if (!ss->refs) { ss->nrefs = 0; return; }
    }
    int n = 0;
    for (int i = 0; i < nsrc; i++) {
        if (i == 1 && !ss->d->quirks) continue;
        int m;
        const struct wsheet* sh = sheet_at(ss, i, &m);
        for (int r = 0; r < sh->nrule; r++) {
            struct ruleref* rr = &ss->refs[n++];
            rr->sh = sh;
            rr->rule = r;
            rr->key = rule_key(sh, sh->rules[r].sel);
            rr->order = ((uint32_t)i << 22) | (sh->rules[r].order & 0x3FFFFF);
            rr->origin = (uint8_t)sh->origin;
            rr->nanc = (uint8_t)(sh->sels[sh->rules[r].sel].has_rel ? 0 :
                                 rule_ancestors(sh, sh->rules[r].sel, rr->anc));
        }
    }
    ss->nrefs = n;
    sort_refs(ss->refs, n);
    // bucket table
    int nb = 0;
    for (int k = 0; k < n; k++) if (k == 0 || ss->refs[k].key != ss->refs[k - 1].key) nb++;
    int cap = 64;
    while (cap < nb * 2) cap *= 2;
    w_free(ss->bkeys); w_free(ss->bstart); w_free(ss->bcount);
    ss->bkeys = (uint32_t*)w_malloc(cap * sizeof(uint32_t));
    ss->bstart = (int32_t*)w_malloc(cap * sizeof(int32_t));
    ss->bcount = (int32_t*)w_malloc(cap * sizeof(int32_t));
    ss->bcap = cap;
    if (!ss->bkeys || !ss->bstart || !ss->bcount) { ss->bcap = 0; return; }
    for (int i = 0; i < cap; i++) ss->bkeys[i] = 0xFFFFFFFFu;
    ss->univ_start = 0; ss->univ_count = 0;
    for (int k = 0; k < n;) {
        int e = k;
        while (e < n && ss->refs[e].key == ss->refs[k].key) e++;
        uint32_t key = ss->refs[k].key;
        if (key == 0) { ss->univ_start = k; ss->univ_count = e - k; }
        else {
            uint32_t h = (key * 2654435761u) & (cap - 1);
            while (ss->bkeys[h] != 0xFFFFFFFFu) h = (h + 1) & (cap - 1);
            ss->bkeys[h] = key;
            ss->bstart[h] = k;
            ss->bcount[h] = e - k;
        }
        k = e;
    }
    ss->index_dirty = 0;
}

static int bucket(const struct wstyleset* ss, uint32_t key, int* start) {
    if (!ss->bcap) return 0;
    uint32_t h = (key * 2654435761u) & (ss->bcap - 1);
    while (ss->bkeys[h] != 0xFFFFFFFFu) {
        if (ss->bkeys[h] == key) { *start = ss->bstart[h]; return ss->bcount[h]; }
        h = (h + 1) & (ss->bcap - 1);
    }
    return 0;
}

// ---- media queries -----------------------------------------------------------------------

static int mq_len_px(const char* s, int len, int32_t* px_milli) {
    int32_t m; int u;
    cv_trim(&s, &len);
    if (!cv_number(s, len, &m, &u)) return 0;
    if (u == len) { *px_milli = m; return 1; }
    int un = cv_unit(s + u, len - u);
    if (un == U_PX) *px_milli = m;
    else if (un == U_EM || un == U_REM) *px_milli = m * 16;
    else if (un == U_PT) *px_milli = m * 4 / 3;
    else if (un == U_DPPX || un == U_X) *px_milli = m;
    else return 0;
    return 1;
}

static int mq_feature(const char* s, int len, int vw, int vh) {
    cv_trim(&s, &len);
    if (len >= 2 && s[0] == '(' && s[len - 1] == ')') { s++; len -= 2; cv_trim(&s, &len); }
    // range syntax: "width >= 600px", "600px <= width <= 900px"
    int op_at = -1;
    for (int i = 0; i < len; i++) if (s[i] == '<' || s[i] == '>' || (s[i] == '=' && (i == 0 || (s[i-1] != '<' && s[i-1] != '>')))) { op_at = i; break; }
    int colon = -1;
    for (int i = 0; i < len; i++) if (s[i] == ':') { colon = i; break; }
    if (op_at >= 0 && colon < 0) {
        // evaluate each comparison segment pairwise
        const char* toks[5]; int tl[5]; int nt = 0;
        int i = 0;
        while (i < len && nt < 5) {
            while (i < len && w_isspace((unsigned char)s[i])) i++;
            int st = i;
            if (s[i] == '<' || s[i] == '>' || s[i] == '=') {
                i++;
                if (i < len && s[i] == '=') i++;
            } else {
                while (i < len && !w_isspace((unsigned char)s[i]) && s[i] != '<' && s[i] != '>' && s[i] != '=') i++;
            }
            toks[nt] = s + st; tl[nt] = i - st; nt++;
        }
        int ok = 1;
        for (int k = 0; k + 2 < nt; k += 2) {
            const char* a = toks[k]; int al = tl[k];
            const char* op = toks[k + 1]; int ol = tl[k + 1];
            const char* b = toks[k + 2]; int bl = tl[k + 2];
            int32_t av, bv;
            int32_t W = vw * 1000, H = vh * 1000;
            if (w_ieq(a, al, "width")) av = W; else if (w_ieq(a, al, "height")) av = H;
            else if (!mq_len_px(a, al, &av)) return 0;
            if (w_ieq(b, bl, "width")) bv = W; else if (w_ieq(b, bl, "height")) bv = H;
            else if (!mq_len_px(b, bl, &bv)) return 0;
            if (ol == 1 && op[0] == '<') ok &= av < bv;
            else if (ol == 1 && op[0] == '>') ok &= av > bv;
            else if (ol == 2 && op[0] == '<') ok &= av <= bv;
            else if (ol == 2 && op[0] == '>') ok &= av >= bv;
            else if (op[0] == '=') ok &= av == bv;
        }
        return ok;
    }
    const char* name = s; int nl = colon >= 0 ? colon : len;
    cv_trim(&name, &nl);
    const char* val = colon >= 0 ? s + colon + 1 : 0;
    int vl = colon >= 0 ? len - colon - 1 : 0;
    if (val) cv_trim(&val, &vl);
    int32_t px;
    if (w_ieq(name, nl, "min-width")) return val && mq_len_px(val, vl, &px) && vw * 1000 >= px;
    if (w_ieq(name, nl, "max-width")) return val && mq_len_px(val, vl, &px) && vw * 1000 <= px;
    if (w_ieq(name, nl, "width")) return val && mq_len_px(val, vl, &px) && vw * 1000 == px;
    if (w_ieq(name, nl, "min-height")) return val && mq_len_px(val, vl, &px) && vh * 1000 >= px;
    if (w_ieq(name, nl, "max-height")) return val && mq_len_px(val, vl, &px) && vh * 1000 <= px;
    if (w_ieq(name, nl, "min-device-width")) return val && mq_len_px(val, vl, &px) && 1920000 >= px;
    if (w_ieq(name, nl, "max-device-width")) return val && mq_len_px(val, vl, &px) && 1920000 <= px;
    if (w_ieq(name, nl, "orientation"))
        return val && (w_ieq(val, vl, "landscape") ? vw >= vh : vw < vh);
    if (w_ieq(name, nl, "prefers-color-scheme")) return val && w_ieq(val, vl, "light");
    if (w_ieq(name, nl, "prefers-reduced-motion")) return !val || w_ieq(val, vl, "no-preference");
    if (w_ieq(name, nl, "prefers-reduced-transparency") || w_ieq(name, nl, "prefers-contrast") ||
        w_ieq(name, nl, "forced-colors") || w_ieq(name, nl, "inverted-colors"))
        return val && (w_ieq(val, vl, "no-preference") || w_ieq(val, vl, "none"));
    if (w_ieq(name, nl, "hover") || w_ieq(name, nl, "any-hover")) return !val || w_ieq(val, vl, "hover");
    if (w_ieq(name, nl, "pointer") || w_ieq(name, nl, "any-pointer")) return !val || w_ieq(val, vl, "fine");
    if (w_ieq(name, nl, "color")) return 1;
    if (w_ieq(name, nl, "min-color")) return 1;
    if (w_ieq(name, nl, "monochrome") || w_ieq(name, nl, "grid")) return val && w_ieq(val, vl, "0");
    if (w_ieq(name, nl, "scripting")) return val && w_ieq(val, vl, "none");
    if (w_ieq(name, nl, "display-mode")) return val && w_ieq(val, vl, "browser");
    if (w_ieq(name, nl, "update")) return val && w_ieq(val, vl, "fast");
    if (w_ieq(name, nl, "-webkit-min-device-pixel-ratio") || w_ieq(name, nl, "min-resolution") ||
        w_ieq(name, nl, "min--moz-device-pixel-ratio")) {
        int32_t r;
        if (!val) return 0;
        if (w_ieq_prefix(val + vl - 3 > val ? val + vl - 3 : val, 3, "dpi")) {
            int32_t m; int u;
            return cv_number(val, vl, &m, &u) && m <= 96000;
        }
        return mq_len_px(val, vl, &r) && r <= 1000;
    }
    if (w_ieq(name, nl, "-webkit-max-device-pixel-ratio") || w_ieq(name, nl, "max-resolution")) return 1;
    if (w_ieq(name, nl, "min-aspect-ratio") || w_ieq(name, nl, "max-aspect-ratio") ||
        w_ieq(name, nl, "aspect-ratio")) {
        if (!val) return 0;
        int32_t a, b; int u, u2;
        int slash = -1;
        for (int i = 0; i < vl; i++) if (val[i] == '/') { slash = i; break; }
        if (!cv_number(val, slash >= 0 ? slash : vl, &a, &u)) return 0;
        b = 1000;
        if (slash >= 0) { const char* q = val + slash + 1; int ql = vl - slash - 1; cv_trim(&q, &ql); if (!cv_number(q, ql, &b, &u2)) return 0; }
        int64_t lhs = (int64_t)vw * b, rhs = (int64_t)vh * a;
        if (name[1] == 'i') return lhs >= rhs;
        if (name[1] == 'a') return lhs <= rhs;
        return lhs == rhs;
    }
    return 0;
}

static int mq_condition(const char* s, int len, int vw, int vh);

// one media query: [not|only] [type] [and (cond)]*  |  (cond) [and|or ...]
static int mq_query(const char* s, int len, int vw, int vh) {
    cv_trim(&s, &len);
    if (len <= 0) return 1;
    int neg = 0;
    if (w_ieq_prefix(s, len, "not ") && s[4 <= len ? 4 : 0] != '(') { neg = 1; s += 4; len -= 4; cv_trim(&s, &len); }
    else if (w_ieq_prefix(s, len, "only ")) { s += 5; len -= 5; cv_trim(&s, &len); }
    int r;
    if (s[0] != '(' && !(w_ieq_prefix(s, len, "not ") || w_ieq_prefix(s, len, "not("))) {
        // media type
        int i = 0;
        while (i < len && !w_isspace((unsigned char)s[i])) i++;
        int type_ok = w_ieq(s, i, "all") || w_ieq(s, i, "screen");
        if (!w_ieq(s, i, "all") && !w_ieq(s, i, "screen") && !w_ieq(s, i, "print") &&
            !w_ieq(s, i, "speech") && !w_ieq(s, i, "tv") && !w_ieq(s, i, "handheld") &&
            !w_ieq(s, i, "projection") && !w_ieq(s, i, "tty") && !w_ieq(s, i, "braille") &&
            !w_ieq(s, i, "embossed") && !w_ieq(s, i, "aural"))
            return 0; // unknown type: never matches (even with "not")
        const char* rest = s + i; int rl = len - i;
        cv_trim(&rest, &rl);
        if (rl > 0) {
            if (!w_ieq_prefix(rest, rl, "and")) return 0;
            rest += 3; rl -= 3;
            r = type_ok && mq_condition(rest, rl, vw, vh);
        } else r = type_ok;
    } else {
        r = mq_condition(s, len, vw, vh);
    }
    return neg ? !r : r;
}

static int mq_condition(const char* s, int len, int vw, int vh) {
    cv_trim(&s, &len);
    if (len <= 0) return 1;
    if (w_ieq_prefix(s, len, "not ") || w_ieq_prefix(s, len, "not(")) return !mq_condition(s + 3, len - 3, vw, vh);
    int depth = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (!depth && i > 0 && w_isspace((unsigned char)s[i - 1])) {
            if (w_ieq_prefix(s + i, len - i, "and ") || w_ieq_prefix(s + i, len - i, "and("))
                return mq_condition(s, i, vw, vh) && mq_condition(s + i + 3, len - i - 3, vw, vh);
            if (w_ieq_prefix(s + i, len - i, "or ") || w_ieq_prefix(s + i, len - i, "or("))
                return mq_condition(s, i, vw, vh) || mq_condition(s + i + 2, len - i - 2, vw, vh);
        }
    }
    if (s[0] == '(' && s[len - 1] == ')') {
        const char* in = s + 1; int il = len - 2;
        cv_trim(&in, &il);
        if (il && (in[0] == '(' || w_ieq_prefix(in, il, "not"))) return mq_condition(in, il, vw, vh);
        return mq_feature(s, len, vw, vh);
    }
    return mq_feature(s, len, vw, vh);
}

static int mq_eval(const char* s, int len, int vw, int vh) {
    // \x01 joins nested conditions (all must hold)
    for (int i = 0; i < len; i++)
        if (s[i] == 1) return mq_eval(s, i, vw, vh) && mq_eval(s + i + 1, len - i - 1, vw, vh);
    int pos = 0; const char* q; int ql;
    int any = 0, count = 0;
    while (cv_next_comma(s, len, &pos, &q, &ql)) {
        count++;
        if (mq_query(q, ql, vw, vh)) any = 1;
    }
    return count ? any : 1;
}

static void eval_mqs(struct wstyleset* ss) {
    for (int i = 0; i < ss->nsheets + 2; i++) {
        int m;
        const struct wsheet* sh = sheet_at(ss, i, &m);
        w_free(ss->mqres[i]);
        ss->mqres[i] = 0;
        if (!sh->nmq) continue;
        ss->mqres[i] = (uint8_t*)w_malloc(sh->nmq);
        if (!ss->mqres[i]) continue;
        for (int k = 0; k < sh->nmq; k++)
            ss->mqres[i][k] = (uint8_t)mq_eval(sh->pool.p + sh->mqs[k].raw, (int)sh->mqs[k].rawlen, ss->vw, ss->vh);
    }
}

static int rule_live(const struct wstyleset* ss, const struct ruleref* rr) {
    const struct crule* r = &rr->sh->rules[rr->rule];
    if (r->mq < 0) return 1;
    int si = (int)(rr->order >> 22);
    const uint8_t* res = si < MAX_SHEETS + 2 ? ss->mqres[si] : 0;
    return res ? res[r->mq] : 0;
}

// ---- initial style ------------------------------------------------------------------

#define CUR_COLOR 0x01000000u   // currentColor sentinel (resolved after compute)

static struct wstyle initial;
static int initial_ready;

static void initial_init(void) {
    memset(&initial, 0, sizeof initial);
    struct wstyle* s = &initial;
    s->display = D_INLINE;
    s->opacity = 255;
    s->z_auto = 1;
    s->width = s->height = s->min_w = s->min_h = wl_auto();
    s->max_w.t = s->max_h.t = WL_NONE;
    for (int i = 0; i < 4; i++) {
        s->margin[i] = wl_px(0);
        s->padding[i] = wl_px(0);
        s->inset[i] = wl_auto();
        s->bw[i] = 0;
        s->bs[i] = BS_NONE;
        s->bc[i] = CUR_COLOR;
        s->radius[i] = wl_px(0);
    }
    s->color = 0xFF000000;
    s->bg_grad = -1;
    s->bg_size[0] = s->bg_size[1] = wl_auto();
    s->bg_pos[0] = s->bg_pos[1] = wl_px(0);
    s->font_family = FAM_SERIF;
    s->font_weight = 400;
    s->font_size = PX(16);
    s->fs_default = 1;
    s->deco_color = CUR_COLOR;
    s->text_indent = wl_px(0);
    s->list_style_type = LS_DISC;
    s->flex_shrink = 1000;
    s->flex_basis = wl_auto();
    s->align_self = AL_AUTO;
    s->justify_self = AL_AUTO;
    s->row_gap = s->column_gap = wl_px(0);
    s->fill = 0xFF000000;
    s->stroke_none = 1;
    s->stroke_width = PX(1);
    s->outline_color = CUR_COLOR;
    s->translate_x = s->translate_y = wl_px(0);
    s->vertical_align = VA_BASELINE;
    initial_ready = 1;
}

// ---- computed value helpers -------------------------------------------------------------

struct cctx {
    struct wstyleset* ss;
    const struct wstyle* parent;
    struct wstyle* s;
    int el;
    int32_t root_fs;     // LU
    const struct wsheet* sh;  // sheet of the decl being computed (base URL)
};

static int32_t unit_lu(struct cctx* c, int32_t milli, int unit, int32_t fs) {
    int32_t vw = c->ss->vw, vh = c->ss->vh;
    switch (unit) {
    case U_PX: return w_muldiv(milli, 64, 1000);
    case U_EM: return w_muldiv(milli, fs, 1000);
    case U_REM: return w_muldiv(milli, c->root_fs, 1000);
    case U_EX: return w_muldiv(milli, fs / 2, 1000);
    case U_CH: return w_muldiv(milli, fs * 572 / 1000, 1000);
    case U_LH: return w_muldiv(milli, fs * 12 / 10, 1000);
    case U_VW: return w_muldiv(milli, vw * 64, 100000);
    case U_VH: return w_muldiv(milli, vh * 64, 100000);
    case U_VMIN: return w_muldiv(milli, (vw < vh ? vw : vh) * 64, 100000);
    case U_VMAX: return w_muldiv(milli, (vw > vh ? vw : vh) * 64, 100000);
    case U_PT: return w_muldiv(milli, 256, 3000);
    case U_PC: return w_muldiv(milli, 1024, 1000);
    case U_IN: return w_muldiv(milli, 6144, 1000);
    case U_CM: return w_muldiv(milli, 6144 * 100, 254000);
    case U_MM: return w_muldiv(milli, 6144 * 10, 254000);
    case U_Q: return w_muldiv(milli, 6144 * 10, 1016000);
    }
    return 0;
}

// --- calc() ---
struct cv { int32_t px, pct, num; int is_len; int ok; };

struct calcp {
    struct cctx* c;
    const char* s;
    int len, i;
    int32_t fs;
    int32_t pct_basis;   // LU basis used to compare mixed min()/max() arguments
};

static struct cv calc_sum(struct calcp* p);

static void cp_ws(struct calcp* p) { while (p->i < p->len && w_isspace((unsigned char)p->s[p->i])) p->i++; }

static struct cv cv_err(void) { struct cv v = { 0, 0, 0, 0, 0 }; return v; }

static int32_t cv_flat(const struct calcp* p, struct cv v) {
    return v.px + (v.pct ? w_muldiv(p->pct_basis, v.pct, 10000) : 0);
}

static struct cv calc_args_minmax(struct calcp* p, int kind) {
    // kind: 0 min, 1 max, 2 clamp
    struct cv args[8];
    int n = 0;
    for (;;) {
        cp_ws(p);
        struct cv v = calc_sum(p);
        if (!v.ok) return cv_err();
        if (n < 8) args[n++] = v;
        cp_ws(p);
        if (p->i < p->len && p->s[p->i] == ',') { p->i++; continue; }
        break;
    }
    if (p->i >= p->len || p->s[p->i] != ')') return cv_err();
    p->i++;
    if (kind == 2) {
        if (n != 3) return cv_err();
        // clamp(min, val, max) = max(min, min(val, max))
        struct cv lo = args[0], v = args[1], hi = args[2];
        if (!v.is_len) {
            int32_t r = v.num;
            if (r > hi.num) r = hi.num;
            if (r < lo.num) r = lo.num;
            v.num = r;
            return v;
        }
        struct cv r = v;
        if (cv_flat(p, r) > cv_flat(p, hi)) r = hi;
        if (cv_flat(p, r) < cv_flat(p, lo)) r = lo;
        return r;
    }
    struct cv best = args[0];
    for (int k = 1; k < n; k++) {
        if (!best.is_len) {
            if ((kind == 0) ? args[k].num < best.num : args[k].num > best.num) best = args[k];
            continue;
        }
        int32_t a = cv_flat(p, args[k]), b = cv_flat(p, best);
        // same-unit comparison when both are pure percentages
        if (!args[k].px && !best.px && (args[k].pct || best.pct)) { a = args[k].pct; b = best.pct; }
        if ((kind == 0) ? a < b : a > b) best = args[k];
    }
    return best;
}

static struct cv calc_factor(struct calcp* p) {
    cp_ws(p);
    if (p->i >= p->len) return cv_err();
    const char* s = p->s + p->i;
    int rem = p->len - p->i;
    if (s[0] == '(') {
        p->i++;
        struct cv v = calc_sum(p);
        cp_ws(p);
        if (p->i < p->len && p->s[p->i] == ')') p->i++;
        return v;
    }
    if (w_ieq_prefix(s, rem, "calc(") || w_ieq_prefix(s, rem, "-webkit-calc(")) {
        p->i += s[0] == '-' ? 13 : 5;
        struct cv v = calc_sum(p);
        cp_ws(p);
        if (p->i < p->len && p->s[p->i] == ')') p->i++;
        return v;
    }
    if (w_ieq_prefix(s, rem, "min(")) { p->i += 4; return calc_args_minmax(p, 0); }
    if (w_ieq_prefix(s, rem, "max(")) { p->i += 4; return calc_args_minmax(p, 1); }
    if (w_ieq_prefix(s, rem, "clamp(")) { p->i += 6; return calc_args_minmax(p, 2); }
    int32_t m; int u;
    if (cv_number(s, rem, &m, &u)) {
        p->i += u;
        struct cv v = { 0, 0, 0, 0, 1 };
        if (p->i < p->len && p->s[p->i] == '%') { p->i++; v.pct = m / 10; v.is_len = 1; return v; }
        int ue = p->i;
        while (ue < p->len && w_isalpha((unsigned char)p->s[ue])) ue++;
        if (ue > p->i) {
            int un = cv_unit(p->s + p->i, ue - p->i);
            p->i = ue;
            if (un < 0 || un >= U_DEG) return cv_err();
            v.px = unit_lu(p->c, m, un, p->fs);
            v.is_len = 1;
            return v;
        }
        v.num = m;
        return v;
    }
    if (w_ieq_prefix(s, rem, "infinity")) { p->i += 8; struct cv v = { 0, 0, 0x3FFFFFFF, 0, 1 }; return v; }
    if (w_ieq_prefix(s, rem, "pi")) { p->i += 2; struct cv v = { 0, 0, 3142, 0, 1 }; return v; }
    if (s[0] == '-' && rem > 1 && !w_isdigit((unsigned char)s[1]) && s[1] != '.') {
        p->i++;
        struct cv v = calc_factor(p);
        v.px = -v.px; v.pct = -v.pct; v.num = -v.num;
        return v;
    }
    return cv_err();
}

static struct cv calc_prod(struct calcp* p) {
    struct cv a = calc_factor(p);
    for (;;) {
        if (!a.ok) return a;
        cp_ws(p);
        if (p->i >= p->len) return a;
        char op = p->s[p->i];
        if (op != '*' && op != '/') return a;
        p->i++;
        struct cv b = calc_factor(p);
        if (!b.ok) return cv_err();
        if (op == '*') {
            if (a.is_len && b.is_len) return cv_err();
            if (b.is_len) { struct cv t = a; a = b; b = t; }
            if (a.is_len) { a.px = w_muldiv(a.px, b.num, 1000); a.pct = w_muldiv(a.pct, b.num, 1000); }
            else a.num = w_muldiv(a.num, b.num, 1000);
        } else {
            if (b.is_len || b.num == 0) return cv_err();
            int32_t dv = b.num < 0 ? -b.num : b.num;
            int sg = b.num < 0 ? -1 : 1;
            if (a.is_len) { a.px = sg * w_muldiv(a.px, 1000, dv); a.pct = sg * w_muldiv(a.pct, 1000, dv); }
            else a.num = sg * w_muldiv(a.num, 1000, dv);
        }
    }
}

static struct cv calc_sum(struct calcp* p) {
    struct cv a = calc_prod(p);
    for (;;) {
        if (!a.ok) return a;
        cp_ws(p);
        if (p->i >= p->len) return a;
        char op = p->s[p->i];
        if (op != '+' && op != '-') return a;
        p->i++;
        struct cv b = calc_prod(p);
        if (!b.ok) return cv_err();
        int sg = op == '-' ? -1 : 1;
        if (a.is_len || b.is_len) {
            // number 0 + length is tolerated
            a.px += sg * b.px;
            a.pct += sg * b.pct;
            a.is_len = 1;
        } else a.num += sg * b.num;
    }
}

// Evaluate a calc()-family expression. Lengths -> (px LU, pct 1/100%);
// numbers -> *num (milli). Returns 1 on success.
static int calc_eval(struct cctx* c, const char* s, int len, int32_t fs, struct wlen* out,
                     int32_t* num) {
    struct calcp p;
    p.c = c; p.s = s; p.len = len; p.i = 0; p.fs = fs;
    p.pct_basis = c->ss->vw * 64; // heuristic basis for mixed min/max comparisons
    struct cv v = calc_factor(&p);
    if (!v.ok) return 0;
    if (out) { out->px = v.px; out->pct = v.pct; out->t = WL_LEN; if (!v.is_len) out->px = w_muldiv(v.num, 64, 1000); }
    if (num) *num = v.num;
    return 1;
}

// Resolve a length-ish decl into a wlen. fs = font size for em.
static int dlen(struct cctx* c, const struct cdecl* d, int32_t fs, struct wlen* out) {
    switch (d->kind) {
    case VK_AUTO: *out = wl_auto(); return 1;
    case VK_KW: out->px = 0; out->pct = 0; out->t = (uint8_t)d->a; return d->a <= WL_CONTENT;
    case VK_LEN: *out = wl_px(unit_lu(c, d->a, d->unit, fs)); return 1;
    case VK_PCT: out->px = 0; out->pct = d->a / 10; out->t = WL_LEN; return 1;
    case VK_NUM: *out = wl_px(w_muldiv(d->a, 64, 1000)); return 1;
    case VK_CALC: {
        const struct wsheet* sh = c->sh;
        return calc_eval(c, sh->pool.p + d->raw, (int)d->rawlen, fs, out, 0);
    }
    }
    return 0;
}

static const char* draw_of(struct cctx* c, const struct cdecl* d, int* len) {
    *len = (int)d->rawlen;
    return c->sh->pool.p + d->raw;
}

// ---- complex values ----------------------------------------------------------------

static int family_of(const char* s, int len, int* generic_mono) {
    int pos = 0; const char* f; int fl;
    *generic_mono = 0;
    while (cv_next_comma(s, len, &pos, &f, &fl)) {
        if (fl && (f[0] == '"' || f[0] == '\'')) { f++; fl -= 2; if (fl < 0) fl = 0; }
        char low[64];
        int n = 0;
        for (int i = 0; i < fl && n < 63; i++) low[n++] = (char)w_lower((unsigned char)f[i]);
        low[n] = 0;
        if (!strcmp(low, "monospace")) { *generic_mono = 1; return FAM_MONO; }
        if (!strcmp(low, "ui-monospace") || w_strstr(low, "mono") || w_strstr(low, "courier") ||
            w_strstr(low, "consol") || w_strstr(low, "menlo") || w_strstr(low, "monaco") ||
            w_strstr(low, "code") || !strcmp(low, "fixed") || w_strstr(low, "terminal") ||
            w_strstr(low, "lucida console")) return FAM_MONO;
        if (!strcmp(low, "serif") || !strcmp(low, "ui-serif") || w_strstr(low, "times") ||
            w_strstr(low, "georgia") || w_strstr(low, "garamond") || w_strstr(low, "libertine") ||
            w_strstr(low, "palatino") || w_strstr(low, "cambria") || w_strstr(low, "baskerville") ||
            w_strstr(low, "noto serif") || w_strstr(low, "merriweather") || w_strstr(low, "charter") ||
            w_strstr(low, "book antiqua") || w_strstr(low, "linux libertine") || w_strstr(low, "lora") ||
            w_strstr(low, "source serif") || w_strstr(low, "pt serif") || w_strstr(low, "droid serif") ||
            w_strstr(low, "dejavu serif") || w_strstr(low, "liberation serif") || w_strstr(low, "iowan"))
            return FAM_SERIF;
        if (!strcmp(low, "sans-serif") || !strcmp(low, "system-ui") || !strcmp(low, "ui-sans-serif") ||
            !strcmp(low, "-apple-system") || !strcmp(low, "blinkmacsystemfont") ||
            w_strstr(low, "arial") || w_strstr(low, "helvetica") || w_strstr(low, "roboto") ||
            w_strstr(low, "segoe") || w_strstr(low, "verdana") || w_strstr(low, "sans") ||
            w_strstr(low, "inter") || w_strstr(low, "tahoma") || w_strstr(low, "trebuchet") ||
            w_strstr(low, "ubuntu") || w_strstr(low, "lato") || w_strstr(low, "montserrat") ||
            w_strstr(low, "lucida") || w_strstr(low, "calibri") || w_strstr(low, "geneva") ||
            w_strstr(low, "avenir") || w_strstr(low, "futura") || w_strstr(low, "poppins") ||
            w_strstr(low, "nunito") || w_strstr(low, "raleway") || w_strstr(low, "graphik") ||
            w_strstr(low, "sf pro") || w_strstr(low, "cantarell") || w_strstr(low, "oxygen") ||
            w_strstr(low, "fira") || w_strstr(low, "work") || w_strstr(low, "manrope") ||
            w_strstr(low, "public") || w_strstr(low, "ibm plex") || w_strstr(low, "reith") ||
            w_strstr(low, "geist") || w_strstr(low, "mona"))
            return FAM_SANS;
        if (!strcmp(low, "cursive") || !strcmp(low, "fantasy") || !strcmp(low, "emoji") ||
            !strcmp(low, "math") || !strcmp(low, "fangsong")) return FAM_SANS;
        // unknown family name: keep looking for a generic fallback
    }
    return -1;
}

static int32_t grad_add(struct wstyleset* ss, const struct wgrad* g) {
    for (int i = ss->ngrad - 1; i >= 0 && i >= ss->ngrad - 64; i--)
        if (!memcmp(&ss->grads[i], g, sizeof *g)) return i;
    if (ss->ngrad >= ss->capgrad) {
        int nc = ss->capgrad ? ss->capgrad * 2 : 32;
        struct wgrad* ng = (struct wgrad*)w_realloc(ss->grads, nc * sizeof(struct wgrad));
        if (!ng) return -1;
        ss->grads = ng;
        ss->capgrad = nc;
    }
    ss->grads[ss->ngrad] = *g;
    return ss->ngrad++;
}

static int32_t angle_milli(const char* s, int len) {
    int32_t m; int u;
    if (!cv_number(s, len, &m, &u)) return 180000;
    int un = cv_unit(s + u, len - u);
    if (un == U_RAD) return w_div64((int64_t)m * 57296, 1000);
    if (un == U_TURN) return m * 360;
    if (un == U_GRAD) return m * 9 / 10;
    return m;
}

static int parse_gradient(struct cctx* c, const char* s, int len, struct wgrad* g) {
    memset(g, 0, sizeof *g);
    int p = 0;
    while (p < len && s[p] != '(') p++;
    if (p >= len || s[len - 1] != ')') return 0;
    const char* fn = s; int fl = p;
    g->repeating = w_ieq_prefix(fn, fl, "repeating");
    g->radial = (w_ieq(fn, fl, "radial-gradient") || w_ieq(fn, fl, "repeating-radial-gradient"));
    if (w_ieq(fn, fl, "conic-gradient")) g->radial = 1;
    if (!(w_ieq(fn, fl, "linear-gradient") || w_ieq(fn, fl, "-webkit-linear-gradient") ||
          w_ieq(fn, fl, "-moz-linear-gradient") || w_ieq(fn, fl, "repeating-linear-gradient") ||
          g->radial))
        return 0;
    int legacy = w_ieq(fn, fl, "-webkit-linear-gradient") || w_ieq(fn, fl, "-moz-linear-gradient");
    const char* a = s + p + 1;
    int al = len - p - 2;
    int pos = 0; const char* part; int pl;
    int first = 1;
    g->angle = 180000;
    while (cv_next_comma(a, al, &pos, &part, &pl)) {
        if (first) {
            first = 0;
            if (w_ieq_prefix(part, pl, "to ")) {
                const char* d = part + 3; int dl = pl - 3; cv_trim(&d, &dl);
                int top = 0, bottom = 0, left = 0, right = 0;
                int q = 0; const char* t; int tl;
                while (cv_next(d, dl, &q, &t, &tl)) {
                    if (w_ieq(t, tl, "top")) top = 1;
                    else if (w_ieq(t, tl, "bottom")) bottom = 1;
                    else if (w_ieq(t, tl, "left")) left = 1;
                    else if (w_ieq(t, tl, "right")) right = 1;
                }
                if (top && right) g->angle = 45000;
                else if (bottom && right) g->angle = 135000;
                else if (bottom && left) g->angle = 225000;
                else if (top && left) g->angle = 315000;
                else if (top) g->angle = 0;
                else if (right) g->angle = 90000;
                else if (left) g->angle = 270000;
                else g->angle = 180000;
                continue;
            }
            int32_t m; int u;
            if (cv_number(part, pl, &m, &u) && u < pl && w_isalpha((unsigned char)part[u])) {
                g->angle = angle_milli(part, pl);
                if (legacy) g->angle = 90000 - g->angle;
                continue;
            }
            if (legacy && (w_ieq(part, pl, "top") || w_ieq(part, pl, "left") ||
                           w_ieq(part, pl, "right") || w_ieq(part, pl, "bottom"))) {
                g->angle = w_ieq(part, pl, "top") ? 180000 : w_ieq(part, pl, "left") ? 90000 :
                           w_ieq(part, pl, "right") ? 270000 : 0;
                continue;
            }
            if (g->radial) {
                // shape/size/position prelude: skip if it contains no color
                uint32_t col; int cur;
                int q = 0; const char* t; int tl;
                int has_color = 0;
                if (cv_next(part, pl, &q, &t, &tl) && cv_color(t, tl, &col, &cur)) has_color = 1;
                if (!has_color) continue;
            }
        }
        if (g->nstops >= CSS_MAX_STOPS) break;
        // "color [pos [pos]]" or "pos color"
        int q = 0; const char* t; int tl;
        uint32_t col = 0; int cur = 0, have_col = 0;
        int32_t sp[2]; int nsp = 0;
        while (cv_next(part, pl, &q, &t, &tl)) {
            uint32_t cc; int ccur;
            if (!have_col && cv_color(t, tl, &cc, &ccur)) { col = cc; cur = ccur; have_col = 1; continue; }
            int32_t m; int u;
            if (nsp < 2 && cv_number(t, tl, &m, &u)) {
                if (u < tl && t[u] == '%') sp[nsp++] = m / 10;
                else sp[nsp++] = -2 - unit_lu(c, m, u < tl ? (cv_unit(t + u, tl - u) < 0 ? U_PX : cv_unit(t + u, tl - u)) : U_PX, c->s->font_size); // absolute: encoded negative
            }
        }
        if (!have_col) continue; // color hint: ignored
        if (cur) col = CUR_COLOR;
        g->color[g->nstops] = col;
        g->pos[g->nstops] = nsp ? sp[0] : -1;
        g->nstops++;
        if (nsp == 2 && g->nstops < CSS_MAX_STOPS) {
            g->color[g->nstops] = col;
            g->pos[g->nstops] = sp[1];
            g->nstops++;
        }
    }
    return g->nstops >= 1;
}

static void bg_image(struct cctx* c, const char* s, int len) {
    struct wstyle* st = c->s;
    cv_trim(&s, &len);
    st->bg_image = 0;
    st->bg_image_len = 0;
    st->bg_grad = -1;
    if (w_ieq(s, len, "none")) return;
    // first layer only
    int pos = 0; const char* layer; int ll;
    if (!cv_next_comma(s, len, &pos, &layer, &ll)) return;
    if (w_ieq_prefix(layer, ll, "image-set(") || w_ieq_prefix(layer, ll, "-webkit-image-set(")) {
        int k = 0;
        while (k < ll && layer[k] != '(') k++;
        const char* in = layer + k + 1; int il = ll - k - 2;
        int p2 = 0; const char* opt; int ol;
        if (!cv_next_comma(in, il, &p2, &opt, &ol)) return;
        int q = 0; const char* t; int tl;
        if (!cv_next(opt, ol, &q, &t, &tl)) return;
        if (t[0] == '"' || t[0] == '\'') {
            char buf[512];
            int n = 0;
            for (int i = 1; i < tl - 1 && n < 511; i++) buf[n++] = t[i];
            char abs[512];
            css_resolve_url(c->sh->base, buf, n, abs, sizeof abs);
            st->bg_image = str_intern(c->ss, abs, (int)strlen(abs));
            st->bg_image_len = (int32_t)strlen(abs);
            return;
        }
        layer = t; ll = tl;
    }
    if (w_ieq_prefix(layer, ll, "url(")) {
        const char* u = layer + 4;
        int ul = ll - 5;
        cv_trim(&u, &ul);
        if (ul >= 2 && (u[0] == '"' || u[0] == '\'')) { u++; ul -= 2; }
        if (ul <= 0) return;
        char abs[1024];
        if (w_ieq_prefix(u, ul, "data:")) {
            if (ul > 1023) return; // large inline images: not supported here
            memcpy(abs, u, ul);
            abs[ul] = 0;
        } else css_resolve_url(c->sh->base, u, ul, abs, sizeof abs);
        st->bg_image = str_intern(c->ss, abs, (int)strlen(abs));
        st->bg_image_len = (int32_t)strlen(abs);
        return;
    }
    struct wgrad g;
    if (parse_gradient(c, layer, ll, &g)) st->bg_grad = (int16_t)grad_add(c->ss, &g);
}

static void bg_pos_one(struct cctx* c, const char* s, int len, int axis) {
    cv_trim(&s, &len);
    struct wlen* o = &c->s->bg_pos[axis];
    // last component wins for 3/4-value syntax approximations
    int pos = 0; const char* t; int tl;
    const char* first = 0; int fl = 0;
    while (cv_next(s, len, &pos, &t, &tl)) { if (!first) { first = t; fl = tl; } }
    if (!first) return;
    if (w_ieq(first, fl, "left") || w_ieq(first, fl, "top")) *o = wl_px(0);
    else if (w_ieq(first, fl, "center")) { o->px = 0; o->pct = 5000; o->t = WL_LEN; }
    else if (w_ieq(first, fl, "right") || w_ieq(first, fl, "bottom")) { o->px = 0; o->pct = 10000; o->t = WL_LEN; }
    else {
        int32_t m; int u;
        if (cv_number(first, fl, &m, &u)) {
            if (u < fl && first[u] == '%') { o->px = 0; o->pct = m / 10; o->t = WL_LEN; }
            else {
                int un = u < fl ? cv_unit(first + u, fl - u) : U_PX;
                *o = wl_px(unit_lu(c, m, un < 0 ? U_PX : un, c->s->font_size));
            }
        } else if (w_ieq_prefix(first, fl, "calc(")) {
            calc_eval(c, first, fl, c->s->font_size, o, 0);
        }
    }
}

static void bg_size(struct cctx* c, const char* s, int len) {
    cv_trim(&s, &len);
    struct wstyle* st = c->s;
    st->bg_size_kind = BGS_AUTO;
    st->bg_size[0] = st->bg_size[1] = wl_auto();
    if (w_ieq(s, len, "cover")) { st->bg_size_kind = BGS_COVER; return; }
    if (w_ieq(s, len, "contain")) { st->bg_size_kind = BGS_CONTAIN; return; }
    int pos = 0; const char* t; int tl; int n = 0;
    while (n < 2 && cv_next(s, len, &pos, &t, &tl)) {
        struct cdecl d; memset(&d, 0, sizeof d);
        if (!w_ieq(t, tl, "auto")) {
            int32_t m; int u;
            if (cv_number(t, tl, &m, &u)) {
                if (u < tl && t[u] == '%') { st->bg_size[n].px = 0; st->bg_size[n].pct = m / 10; st->bg_size[n].t = WL_LEN; }
                else { int un = u < tl ? cv_unit(t + u, tl - u) : U_PX; st->bg_size[n] = wl_px(unit_lu(c, m, un < 0 ? U_PX : un, st->font_size)); }
                st->bg_size_kind = BGS_LEN;
            }
        }
        n++;
    }
}

static void box_shadow(struct cctx* c, const char* s, int len) {
    struct wstyle* st = c->s;
    st->has_shadow = 0;
    cv_trim(&s, &len);
    if (w_ieq(s, len, "none")) return;
    // use the first non-inset shadow (paint supports outer shadows)
    int pos = 0; const char* sh; int shl;
    while (cv_next_comma(s, len, &pos, &sh, &shl)) {
        int q = 0; const char* t; int tl;
        int32_t v[4]; int nv = 0;
        uint32_t col = 0xFF000000; int cur = 1, inset = 0;
        while (cv_next(sh, shl, &q, &t, &tl)) {
            if (w_ieq(t, tl, "inset")) { inset = 1; continue; }
            uint32_t cc; int ccur;
            int32_t m; int u;
            if (nv < 4 && cv_number(t, tl, &m, &u)) {
                int un = u < tl ? cv_unit(t + u, tl - u) : U_PX;
                v[nv++] = unit_lu(c, m, un < 0 ? U_PX : un, st->font_size);
                continue;
            }
            if (cv_color(t, tl, &cc, &ccur)) { col = cc; cur = ccur; }
        }
        if (nv < 2 || inset) continue;
        st->shadow_x = v[0];
        st->shadow_y = v[1];
        st->shadow_blur = nv > 2 ? v[2] : 0;
        st->shadow_spread = nv > 3 ? v[3] : 0;
        st->shadow_color = cur ? CUR_COLOR : col;
        st->shadow_inset = 0;
        st->has_shadow = 1;
        return;
    }
}

static void transform(struct cctx* c, const char* s, int len) {
    struct wstyle* st = c->s;
    st->translate_x = st->translate_y = wl_px(0);
    cv_trim(&s, &len);
    if (w_ieq(s, len, "none")) return;
    // "translate" property form: "x [y]"
    if (len && s[0] != 't' && s[0] != 'T' && s[0] != 'm' && s[0] != 's' && s[0] != 'r' && s[0] != 'p') {
        int q = 0; const char* t; int tl; int n = 0;
        while (n < 2 && cv_next(s, len, &q, &t, &tl)) {
            struct cdecl d; memset(&d, 0, sizeof d);
            struct wlen w;
            int32_t m; int u;
            if (cv_number(t, tl, &m, &u)) {
                if (u < tl && t[u] == '%') { w.px = 0; w.pct = m / 10; w.t = WL_LEN; }
                else { int un = u < tl ? cv_unit(t + u, tl - u) : U_PX; w = wl_px(unit_lu(c, m, un < 0 ? U_PX : un, st->font_size)); }
                if (n == 0) st->translate_x = w; else st->translate_y = w;
            }
            n++;
        }
        return;
    }
    int pos = 0; const char* fn; int fl;
    while (cv_next(s, len, &pos, &fn, &fl)) {
        int p = 0;
        while (p < fl && fn[p] != '(') p++;
        if (p >= fl) continue;
        const char* name = fn; int nl = p;
        const char* args = fn + p + 1; int al = fl - p - 2;
        if (al < 0) continue;
        struct wlen a[3]; int na = 0;
        int q = 0; const char* t; int tl;
        while (na < 3 && cv_next_comma(args, al, &q, &t, &tl)) {
            int32_t m; int u;
            a[na] = wl_px(0);
            if (w_ieq_prefix(t, tl, "calc(")) calc_eval(c, t, tl, st->font_size, &a[na], 0);
            else if (cv_number(t, tl, &m, &u)) {
                if (u < tl && t[u] == '%') { a[na].px = 0; a[na].pct = m / 10; a[na].t = WL_LEN; }
                else { int un = u < tl ? cv_unit(t + u, tl - u) : U_PX; a[na] = wl_px(unit_lu(c, m, un < 0 ? U_PX : un, st->font_size)); }
            }
            na++;
        }
        if (w_ieq(name, nl, "translate") || w_ieq(name, nl, "translate3d")) {
            if (na >= 1) { st->translate_x.px += a[0].px; st->translate_x.pct += a[0].pct; }
            if (na >= 2) { st->translate_y.px += a[1].px; st->translate_y.pct += a[1].pct; }
        } else if (w_ieq(name, nl, "translatex")) {
            if (na >= 1) { st->translate_x.px += a[0].px; st->translate_x.pct += a[0].pct; }
        } else if (w_ieq(name, nl, "translatey")) {
            if (na >= 1) { st->translate_y.px += a[0].px; st->translate_y.pct += a[0].pct; }
        }
    }
}

static int16_t grid_line(struct cctx* c, const char* s, int len, int32_t* name, int32_t* nlen) {
    cv_trim(&s, &len);
    if (w_ieq(s, len, "auto") || !len) return 0;
    int span = 0;
    int32_t num = 0; int have = 0;
    int q = 0; const char* t; int tl;
    while (cv_next(s, len, &q, &t, &tl)) {
        if (w_ieq(t, tl, "span")) { span = 1; continue; }
        int32_t m; int u;
        if (cv_number(t, tl, &m, &u) && u == tl) { num = m / 1000; have = 1; continue; }
        // named line/area
        if (name && !*name) { *name = str_intern(c->ss, t, tl); *nlen = tl; }
    }
    if (span) return (int16_t)(1000 + (have ? (num > 0 ? num : 1) : 1));
    if (have) return (int16_t)(num > 999 ? 999 : num < -999 ? -999 : num);
    return 0;
}

// content: build the generated text
static void gen_content(struct cctx* c, const char* s, int len) {
    struct wstyle* st = c->s;
    cv_trim(&s, &len);
    st->content = 0;
    st->content_len = 0;
    if (w_ieq(s, len, "none") || w_ieq(s, len, "normal")) return;
    char buf[512];
    int o = 0;
    int pos = 0; const char* t; int tl;
    int any = 0;
    while (cv_next(s, len, &pos, &t, &tl)) {
        if (tl == 1 && t[0] == '/') break; // alt text follows
        if (t[0] == '"' || t[0] == '\'') {
            any = 1;
            for (int i = 1; i < tl - 1 && o < 500; i++) {
                if (t[i] == '\\' && i + 1 < tl - 1) {
                    i++;
                    if (w_ishex((unsigned char)t[i])) {
                        uint32_t cp = 0;
                        int k = 0;
                        while (k < 6 && i < tl - 1 && w_ishex((unsigned char)t[i])) { cp = cp * 16 + w_hexval((unsigned char)t[i]); i++; k++; }
                        if (i < tl - 1 && t[i] == ' ') i++;
                        i--;
                        o += w_utf8_enc(cp == 0xA ? '\n' : cp, buf + o);
                    } else buf[o++] = t[i];
                    continue;
                }
                buf[o++] = t[i];
            }
            continue;
        }
        if (w_ieq_prefix(t, tl, "attr(")) {
            any = 1;
            const char* an = t + 5; int anl = tl - 6;
            cv_trim(&an, &anl);
            int vl;
            const char* v;
            char nm[64]; int nn = 0;
            for (int i = 0; i < anl && nn < 63 && an[i] != ',' && an[i] != ' '; i++) nm[nn++] = an[i];
            nm[nn] = 0;
            v = wdom_attr_s(c->ss->d, c->el, nm, &vl);
            if (v) for (int i = 0; i < vl && o < 500; i++) buf[o++] = v[i];
            continue;
        }
        if (w_ieq(t, tl, "open-quote")) { any = 1; o += w_utf8_enc(0x201C, buf + o); continue; }
        if (w_ieq(t, tl, "close-quote")) { any = 1; o += w_utf8_enc(0x201D, buf + o); continue; }
        if (w_ieq(t, tl, "no-open-quote") || w_ieq(t, tl, "no-close-quote")) { any = 1; continue; }
        if (w_ieq_prefix(t, tl, "counter(") || w_ieq_prefix(t, tl, "counters(")) { any = 1; continue; }
        if (w_ieq_prefix(t, tl, "url(") || w_ieq_prefix(t, tl, "linear-gradient(")) { any = 1; continue; }
    }
    if (!any) return;
    st->content = str_intern(c->ss, buf, o);
    st->content_len = o;
}

static void aspect_ratio(struct cctx* c, const char* s, int len) {
    c->s->aspect_ratio = 0;
    cv_trim(&s, &len);
    if (w_ieq_prefix(s, len, "auto")) { s += 4; len -= 4; cv_trim(&s, &len); }
    if (!len) return;
    int slash = -1;
    for (int i = 0; i < len; i++) if (s[i] == '/') { slash = i; break; }
    int32_t a, b = 1000; int u;
    if (!cv_number(s, slash >= 0 ? slash : len, &a, &u)) return;
    if (slash >= 0) {
        const char* q = s + slash + 1; int ql = len - slash - 1; cv_trim(&q, &ql);
        if (!cv_number(q, ql, &b, &u) || b <= 0) return;
    }
    c->s->aspect_ratio = w_muldiv(a, 1000, b);
}

static uint32_t paint_color(const char* s, int len, int* none) {
    cv_trim(&s, &len);
    *none = 0;
    if (w_ieq(s, len, "none") || w_ieq_prefix(s, len, "url(")) { *none = 1; return 0; }
    uint32_t col; int cur;
    if (cv_color(s, len, &col, &cur)) return cur ? CUR_COLOR : col;
    if (w_ieq(s, len, "context-fill") || w_ieq(s, len, "context-stroke")) return CUR_COLOR;
    *none = 1;
    return 0;
}

// ---- custom properties -----------------------------------------------------------------

static const struct wcvar* var_find(const struct wcvars* v, const char* name, int nl) {
    uint32_t h = sh_hash(name, nl);
    for (; v; v = v->parent)
        for (int i = v->n - 1; i >= 0; i--)
            if (v->v[i].hash == h && (int)v->v[i].nlen == nl) return &v->v[i];
    return 0;
}

static int is_name_char_c(char c) {
    unsigned char u = (unsigned char)c;
    return w_isalnum(u) || c == '-' || c == '_' || u >= 0x80;
}

// Substitute var()/env() in s into out; returns 0 if a var is missing with
// no fallback (declaration invalid at computed-value time).
static int var_subst(struct wstyleset* ss, const struct wcvars* vars, const char* s, int len,
                     struct wbuf* out, int depth) {
    if (depth > 12) return 0;
    int i = 0;
    while (i < len) {
        int isvar = (i + 4 <= len && w_ieq_prefix(s + i, len - i, "var(") &&
                     (i == 0 || !is_name_char_c(s[i - 1])));
        int isenv = (i + 4 <= len && w_ieq_prefix(s + i, len - i, "env(") &&
                     (i == 0 || !is_name_char_c(s[i - 1])));
        if (!isvar && !isenv) { wbuf_putc(out, s[i]); i++; continue; }
        int st = i + 4, depth2 = 1, j = st;
        while (j < len && depth2) {
            if (s[j] == '(') depth2++;
            else if (s[j] == ')') { if (--depth2 == 0) break; }
            j++;
        }
        // args [st, j)
        int comma = -1, d3 = 0;
        for (int k = st; k < j; k++) {
            if (s[k] == '(') d3++;
            else if (s[k] == ')') d3--;
            else if (s[k] == ',' && !d3) { comma = k; break; }
        }
        const char* nm = s + st; int nl = (comma >= 0 ? comma : j) - st;
        cv_trim(&nm, &nl);
        const struct wcvar* v = isvar ? var_find(vars, nm, nl) : 0;
        if (v && v->vlen > 0) {
            if (!var_subst(ss, vars, ss->strpool.p + v->val, (int)v->vlen, out, depth + 1)) return 0;
        } else if (comma >= 0) {
            const char* fb = s + comma + 1; int fbl = j - comma - 1;
            cv_trim(&fb, &fbl);
            if (!var_subst(ss, vars, fb, fbl, out, depth + 1)) return 0;
        } else if (isenv) {
            wbuf_put(out, "0px", 3);
        } else {
            return 0;
        }
        i = j + 1;
    }
    return 1;
}

static struct wcvars* vars_block(struct wstyleset* ss, struct wcvars* parent, int n) {
    struct wcvars* b = (struct wcvars*)w_malloc(sizeof(struct wcvars) + n * sizeof(struct wcvar));
    if (!b) return 0;
    if (ss->nvb >= ss->capvb) {
        int nc = ss->capvb ? ss->capvb * 2 : 64;
        struct wcvars** nv = (struct wcvars**)w_realloc(ss->vblocks, nc * sizeof(*nv));
        if (!nv) { w_free(b); return 0; }
        ss->vblocks = nv;
        ss->capvb = nc;
    }
    ss->vblocks[ss->nvb++] = b;
    b->parent = parent;
    b->n = 0;
    return b;
}

// ---- cascade --------------------------------------------------------------------------

struct slot { const struct cdecl* d; const struct wsheet* sh; };

static int match_cmp(const struct ruleref* a, const struct ruleref* b) {
    if (a->origin != b->origin) return a->origin < b->origin ? -1 : 1;
    uint32_t sa = a->sh->rules[a->rule].spec, sb = b->sh->rules[b->rule].spec;
    if (sa != sb) return sa < sb ? -1 : 1;
    return a->order < b->order ? -1 : a->order > b->order;
}

static void push_match(struct wstyleset* ss, struct ruleref* r) {
    if (ss->nmatch >= ss->capmatch) {
        int nc = ss->capmatch ? ss->capmatch * 2 : 256;
        struct ruleref** nm = (struct ruleref**)w_realloc(ss->match, nc * sizeof(*nm));
        if (!nm) return;
        ss->match = nm;
        ss->capmatch = nc;
    }
    ss->match[ss->nmatch++] = r;
}

static void collect_range(struct wstyleset* ss, int start, int count, int el) {
    for (int k = start; k < start + count; k++) {
        struct ruleref* rr = &ss->refs[k];
        int ok = 1;
        for (int a = 0; a < rr->nanc; a++) if (!bloom_has(rr->anc[a])) { ok = 0; break; }
        if (!ok) continue;
        if (!rule_live(ss, rr)) continue;
        const struct crule* r = &rr->sh->rules[rr->rule];
        if (r->pseudo == PE_OTHER || r->pseudo == PE_PLACEHOLDER) continue;
        if (csel_match(ss->d, rr->sh, r->sel, el)) push_match(ss, rr);
    }
}

// Gather rules matching el (all pseudo kinds); sorted in cascade order.
static void gather(struct wstyleset* ss, int el) {
    ss->nmatch = 0;
    const struct wnode* n = &ss->d->n[el];
    int st, cnt;
    if (ss->univ_count) collect_range(ss, ss->univ_start, ss->univ_count, el);
    if ((cnt = bucket(ss, (3u << 16) | n->tag, &st))) collect_range(ss, st, cnt, el);
    if (n->id && (cnt = bucket(ss, (1u << 16) | n->id, &st))) collect_range(ss, st, cnt, el);
    const uint16_t* cl = ss->d->classes + n->text;
    for (int i = 0; i < n->nclass; i++) {
        // skip duplicate class names on the element
        int dup = 0;
        for (int j = 0; j < i; j++) if (cl[j] == cl[i]) dup = 1;
        if (dup) continue;
        if ((cnt = bucket(ss, (2u << 16) | cl[i], &st))) collect_range(ss, st, cnt, el);
    }
    for (int a = n->attr; a >= 0; a = ss->d->a[a].next) {
        int an = ss->d->a[a].name;
        int dup = 0;
        for (int b = n->attr; b != a; b = ss->d->a[b].next) if (ss->d->a[b].name == an) dup = 1;
        if (dup) continue;
        if ((cnt = bucket(ss, (4u << 16) | an, &st))) collect_range(ss, st, cnt, el);
    }
    // insertion sort
    for (int i = 1; i < ss->nmatch; i++) {
        struct ruleref* k = ss->match[i];
        int j = i - 1;
        while (j >= 0 && match_cmp(ss->match[j], k) > 0) { ss->match[j + 1] = ss->match[j]; j--; }
        ss->match[j + 1] = k;
    }
}

// Apply declarations of matched rules for pseudo `pe` into slots, in
// cascade order: UA normal, hints, author normal, inline normal, author
// important, inline important, UA important.
static void cascade(struct wstyleset* ss, int pe, struct slot* slots,
                    const struct cdecl** customs, const struct wsheet** custom_sh, int* ncustom,
                    int hint_first, int hint_count, int inl_first, int inl_count) {
    for (int i = 0; i < P_COUNT; i++) slots[i].d = 0;
    *ncustom = 0;
    #define APPLY(dd, shp) do { \
        const struct cdecl* d_ = (dd); \
        if (d_->prop == P_CUSTOM) { if (*ncustom < 512) { customs[*ncustom] = d_; custom_sh[*ncustom] = (shp); (*ncustom)++; } } \
        else if (d_->prop < P_COUNT) { slots[d_->prop].d = d_; slots[d_->prop].sh = (shp); } \
    } while (0)
    for (int pass = 0; pass < 2; pass++) {
        int imp = pass;
        // UA (normal pass only; UA !important comes last)
        if (!imp)
            for (int m = 0; m < ss->nmatch; m++) {
                struct ruleref* rr = ss->match[m];
                if (rr->origin != CSS_ORIGIN_UA) continue;
                const struct crule* r = &rr->sh->rules[rr->rule];
                if (r->pseudo != pe) continue;
                for (int k = 0; k < r->ndecl; k++) {
                    const struct cdecl* d = &rr->sh->decls[r->decl + k];
                    if (!d->important) APPLY(d, rr->sh);
                }
            }
        if (!imp && pe == PE_NONE)
            for (int k = 0; k < hint_count; k++) APPLY(&ss->hints.decls[hint_first + k], &ss->hints);
        for (int m = 0; m < ss->nmatch; m++) {
            struct ruleref* rr = ss->match[m];
            if (rr->origin != CSS_ORIGIN_AUTHOR) continue;
            const struct crule* r = &rr->sh->rules[rr->rule];
            if (r->pseudo != pe) continue;
            for (int k = 0; k < r->ndecl; k++) {
                const struct cdecl* d = &rr->sh->decls[r->decl + k];
                if (d->important == imp) APPLY(d, rr->sh);
            }
        }
        if (pe == PE_NONE)
            for (int k = 0; k < inl_count; k++) {
                const struct cdecl* d = &ss->inl.decls[inl_first + k];
                if (d->important == imp) APPLY(d, &ss->inl);
            }
    }
    for (int m = 0; m < ss->nmatch; m++) {
        struct ruleref* rr = ss->match[m];
        if (rr->origin != CSS_ORIGIN_UA) continue;
        const struct crule* r = &rr->sh->rules[rr->rule];
        if (r->pseudo != pe) continue;
        for (int k = 0; k < r->ndecl; k++) {
            const struct cdecl* d = &rr->sh->decls[r->decl + k];
            if (d->important) APPLY(d, rr->sh);
        }
    }
    #undef APPLY
}

// ---- presentational hints --------------------------------------------------------------

static void hint(struct wstyleset* ss, const char* prop, const char* val, int vl) {
    if (vl < 0) vl = (int)strlen(val);
    cparse_value(&ss->hints, prop, (int)strlen(prop), val, vl, 0);
}

// "100" -> "100px", "50%" -> "50%"
static int dim_attr(const char* v, int vl, char* out, int cap) {
    int i = 0;
    while (i < vl && w_isspace((unsigned char)v[i])) i++;
    int st = i;
    while (i < vl && (w_isdigit((unsigned char)v[i]) || v[i] == '.')) i++;
    if (i == st || i - st > 12) return 0;
    int n = i - st;
    memcpy(out, v + st, n);
    if (i < vl && v[i] == '%') out[n++] = '%';
    else { out[n++] = 'p'; out[n++] = 'x'; }
    out[n] = 0;
    (void)cap;
    return n;
}

static int table_ancestor(const struct wdom* d, int el) {
    for (int p = d->n[el].parent; p >= 0 && d->n[p].type == WN_ELEM; p = d->n[p].parent)
        if (wdom_is(d, p, T_table)) return p;
    return -1;
}

static void gen_hints(struct wstyleset* ss, int el, int* first, int* count) {
    struct wdom* d = ss->d;
    *first = ss->hints.ndecl;
    const struct wnode* n = &d->n[el];
    if (n->ns != NS_HTML) {
        // SVG presentation attributes
        int vl; const char* v;
        if ((v = wdom_attr(d, el, A_fill, &vl))) hint(ss, "fill", v, vl);
        if ((v = wdom_attr(d, el, A_stroke, &vl))) hint(ss, "stroke", v, vl);
        if ((v = wdom_attr(d, el, A_stroke_width, &vl))) hint(ss, "stroke-width", v, vl);
        if (n->tag == T_svg) {
            char b[32];
            if ((v = wdom_attr(d, el, A_width, &vl)) && dim_attr(v, vl, b, 32)) hint(ss, "width", b, -1);
            if ((v = wdom_attr(d, el, A_height, &vl)) && dim_attr(v, vl, b, 32)) hint(ss, "height", b, -1);
        }
        *count = ss->hints.ndecl - *first;
        return;
    }
    int t = n->tag;
    int vl;
    const char* v;
    char b[64];
    if ((v = wdom_attr(d, el, A_bgcolor, &vl)) && (t == T_body || t == T_table || t == T_td ||
        t == T_th || t == T_tr || t == T_tbody || t == T_thead || t == T_tfoot)) {
        uint32_t col; int cur;
        if (cv_color(v, vl, &col, &cur)) hint(ss, "background-color", v, vl);
        else if (vl == 6 && w_ishex((unsigned char)v[0])) { // legacy "ffffff"
            b[0] = '#'; memcpy(b + 1, v, 6); b[7] = 0;
            hint(ss, "background-color", b, 7);
        }
    }
    if (t == T_body) {
        if ((v = wdom_attr(d, el, A_text, &vl))) hint(ss, "color", v, vl);
        if ((v = wdom_attr(d, el, A_background, &vl))) {
            char u[300]; int ul = 0;
            u[ul++] = 'u'; u[ul++] = 'r'; u[ul++] = 'l'; u[ul++] = '(';
            for (int i = 0; i < vl && ul < 296; i++) u[ul++] = v[i];
            u[ul++] = ')';
            hint(ss, "background-image", u, ul);
        }
    }
    if (t == T_img || t == T_table || t == T_td || t == T_th || t == T_iframe || t == T_video ||
        t == T_canvas || t == T_object || t == T_embed || t == T_col || t == T_hr || t == T_input ||
        t == T_tr || t == T_svg) {
        if ((v = wdom_attr(d, el, A_width, &vl)) && dim_attr(v, vl, b, 64) && !(t == T_tr))
            hint(ss, "width", b, -1);
        if ((v = wdom_attr(d, el, A_height, &vl)) && dim_attr(v, vl, b, 64) && t != T_col)
            hint(ss, "height", b, -1);
    }
    if ((v = wdom_attr(d, el, A_align, &vl))) {
        if (t == T_img || t == T_table || t == T_iframe || t == T_object || t == T_embed) {
            if (w_ieq(v, vl, "left")) hint(ss, "float", "left", 4);
            else if (w_ieq(v, vl, "right")) hint(ss, "float", "right", 5);
            else if (w_ieq(v, vl, "center") && t == T_table) {
                hint(ss, "margin-left", "auto", 4); hint(ss, "margin-right", "auto", 4);
            } else if (w_ieq(v, vl, "middle") || w_ieq(v, vl, "absmiddle")) hint(ss, "vertical-align", "middle", 6);
            else if (w_ieq(v, vl, "top")) hint(ss, "vertical-align", "top", 3);
            else if (w_ieq(v, vl, "bottom")) hint(ss, "vertical-align", "bottom", 6);
        } else if (t == T_hr) {
            if (w_ieq(v, vl, "left")) { hint(ss, "margin-left", "0", 1); hint(ss, "margin-right", "auto", 4); }
            else if (w_ieq(v, vl, "right")) { hint(ss, "margin-left", "auto", 4); hint(ss, "margin-right", "0", 1); }
        } else if (t == T_caption) {
            // align=bottom on caption
            if (w_ieq(v, vl, "bottom")) hint(ss, "caption-side", "bottom", 6);
        } else {
            if (w_ieq(v, vl, "center") || w_ieq(v, vl, "middle")) hint(ss, "text-align", "center", 6);
            else if (w_ieq(v, vl, "left")) hint(ss, "text-align", "left", 4);
            else if (w_ieq(v, vl, "right")) hint(ss, "text-align", "right", 5);
            else if (w_ieq(v, vl, "justify")) hint(ss, "text-align", "justify", 7);
        }
    }
    if ((v = wdom_attr(d, el, A_valign, &vl)) && (t == T_td || t == T_th || t == T_tr ||
        t == T_tbody || t == T_thead || t == T_tfoot || t == T_col)) {
        if (w_ieq(v, vl, "top") || w_ieq(v, vl, "middle") || w_ieq(v, vl, "bottom") || w_ieq(v, vl, "baseline"))
            hint(ss, "vertical-align", v, vl);
    }
    if ((t == T_td || t == T_th) && wdom_has_attr(d, el, A_nowrap)) hint(ss, "white-space", "nowrap", 6);
    if (t == T_table) {
        if ((v = wdom_attr(d, el, A_cellspacing, &vl)) && dim_attr(v, vl, b, 64)) hint(ss, "border-spacing", b, -1);
        if ((v = wdom_attr(d, el, A_border, &vl))) {
            int bw = 0;
            for (int i = 0; i < vl && w_isdigit((unsigned char)v[i]); i++) bw = bw * 10 + (v[i] - '0');
            if (vl == 0) bw = 1;
            if (bw > 0) {
                int o = 0;
                o += (bw >= 10 ? 2 : 1);
                char bb[40];
                int k = 0;
                if (bw >= 10) bb[k++] = (char)('0' + bw / 10 % 10);
                bb[k++] = (char)('0' + bw % 10);
                memcpy(bb + k, "px outset gray", 14); k += 14;
                hint(ss, "border", bb, k);
            }
        }
    }
    if (t == T_td || t == T_th) {
        int tb = table_ancestor(d, el);
        if (tb >= 0) {
            if ((v = wdom_attr(d, tb, A_cellpadding, &vl)) && dim_attr(v, vl, b, 64)) hint(ss, "padding", b, -1);
            if ((v = wdom_attr(d, tb, A_border, &vl))) {
                int bw = 0;
                for (int i = 0; i < vl && w_isdigit((unsigned char)v[i]); i++) bw = bw * 10 + (v[i] - '0');
                if (vl == 0 || bw > 0) hint(ss, "border", "1px inset gray", 14);
            }
        }
    }
    if (t == T_img || t == T_object) {
        if ((v = wdom_attr(d, el, A_border, &vl)) && dim_attr(v, vl, b, 64)) {
            hint(ss, "border-width", b, -1);
            hint(ss, "border-style", "solid", 5);
        }
    }
    if (t == T_font) {
        if ((v = wdom_attr(d, el, A_color, &vl))) hint(ss, "color", v, vl);
        if ((v = wdom_attr(d, el, A_face, &vl))) hint(ss, "font-family", v, vl);
        if ((v = wdom_attr(d, el, A_size, &vl)) && vl > 0) {
            int rel = 0, k = 0, num = 0;
            if (v[0] == '+') { rel = 1; k = 1; } else if (v[0] == '-') { rel = -1; k = 1; }
            while (k < vl && w_isdigit((unsigned char)v[k])) num = num * 10 + (v[k++] - '0');
            int sz = rel == 0 ? num : 3 + rel * num;
            if (sz < 1) sz = 1;
            if (sz > 7) sz = 7;
            static const char* const FS[8] = { "", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large" };
            hint(ss, "font-size", FS[sz], -1);
        }
    }
    if (t == T_hr) {
        if ((v = wdom_attr(d, el, A_size, &vl)) && dim_attr(v, vl, b, 64)) hint(ss, "height", b, -1);
        if ((v = wdom_attr(d, el, A_color, &vl))) { hint(ss, "background-color", v, vl); hint(ss, "border-color", v, vl); }
        if (wdom_has_attr(d, el, A_noshade)) hint(ss, "border-style", "solid", 5);
    }
    if ((t == T_ol || t == T_ul || t == T_li) && (v = wdom_attr(d, el, A_type, &vl)) && vl > 0) {
        const char* ty = 0;
        if (vl == 1) {
            switch (v[0]) {
            case '1': ty = "decimal"; break;
            case 'a': ty = "lower-alpha"; break;
            case 'A': ty = "upper-alpha"; break;
            case 'i': ty = "lower-roman"; break;
            case 'I': ty = "upper-roman"; break;
            }
        } else if (w_ieq(v, vl, "disc") || w_ieq(v, vl, "circle") || w_ieq(v, vl, "square") ||
                   w_ieq(v, vl, "none")) ty = 0, hint(ss, "list-style-type", v, vl);
        if (ty) hint(ss, "list-style-type", ty, -1);
    }
    if ((t == T_div || t == T_p || t == T_h1 || t == T_h2 || t == T_h3 || t == T_h4 ||
         t == T_h5 || t == T_h6) && 0) { /* align handled above */ }
    if (t == T_iframe && (v = wdom_attr(d, el, (int)watom_find(&d->atoms, "frameborder", 11), &vl)) &&
        vl == 1 && v[0] == '0')
        hint(ss, "border-style", "none", 4);
    if (t == T_textarea || t == T_select || t == T_input) {
        // size/cols/rows are used by the layout directly
    }
    *count = ss->hints.ndecl - *first;
}

// ---- compute -------------------------------------------------------------------------

static int inherited_prop(int p) {
    switch (p) {
    case P_VISIBILITY: case P_COLOR: case P_FONT_FAMILY: case P_FONT_SIZE: case P_FONT_WEIGHT:
    case P_FONT_STYLE: case P_FONT_VARIANT: case P_LINE_HEIGHT: case P_TEXT_ALIGN:
    case P_TEXT_TRANSFORM: case P_TEXT_INDENT: case P_LETTER_SPACING: case P_WORD_SPACING:
    case P_WHITE_SPACE: case P_WORD_BREAK: case P_OVERFLOW_WRAP: case P_DIRECTION:
    case P_LIST_STYLE_TYPE: case P_LIST_STYLE_POSITION: case P_BORDER_COLLAPSE:
    case P_BORDER_SPACING: case P_CAPTION_SIDE: case P_CURSOR: case P_POINTER_EVENTS:
    case P_FILL: case P_STROKE: case P_STROKE_WIDTH:
        return 1;
    }
    return 0;
}

// copy one property's computed value from src into dst
static void copy_prop(struct wstyle* dst, const struct wstyle* src, int p) {
    switch (p) {
    case P_DISPLAY: dst->display = src->display; break;
    case P_POSITION: dst->position = src->position; break;
    case P_FLOAT: dst->float_ = src->float_; break;
    case P_CLEAR: dst->clear = src->clear; break;
    case P_BOX_SIZING: dst->box_sizing = src->box_sizing; break;
    case P_VISIBILITY: dst->visibility = src->visibility; break;
    case P_OVERFLOW_X: dst->overflow_x = src->overflow_x; break;
    case P_OVERFLOW_Y: dst->overflow_y = src->overflow_y; break;
    case P_Z_INDEX: dst->z_index = src->z_index; dst->z_auto = src->z_auto; break;
    case P_OPACITY: dst->opacity = src->opacity; break;
    case P_WIDTH: dst->width = src->width; break;
    case P_HEIGHT: dst->height = src->height; break;
    case P_MIN_WIDTH: dst->min_w = src->min_w; break;
    case P_MIN_HEIGHT: dst->min_h = src->min_h; break;
    case P_MAX_WIDTH: dst->max_w = src->max_w; break;
    case P_MAX_HEIGHT: dst->max_h = src->max_h; break;
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        dst->margin[p - P_MARGIN_TOP] = src->margin[p - P_MARGIN_TOP]; break;
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        dst->padding[p - P_PADDING_TOP] = src->padding[p - P_PADDING_TOP]; break;
    case P_TOP: case P_RIGHT: case P_BOTTOM: case P_LEFT:
        dst->inset[p - P_TOP] = src->inset[p - P_TOP]; break;
    case P_BTW: case P_BRW: case P_BBW: case P_BLW: dst->bw[p - P_BTW] = src->bw[p - P_BTW]; break;
    case P_BTS: case P_BRS: case P_BBS: case P_BLS: dst->bs[p - P_BTS] = src->bs[p - P_BTS]; break;
    case P_BTC: case P_BRC: case P_BBC: case P_BLC: dst->bc[p - P_BTC] = src->bc[p - P_BTC]; break;
    case P_RTL: case P_RTR: case P_RBR: case P_RBL: dst->radius[p - P_RTL] = src->radius[p - P_RTL]; break;
    case P_COLOR: dst->color = src->color; break;
    case P_BG_COLOR: dst->bg_color = src->bg_color; break;
    case P_BG_IMAGE: dst->bg_image = src->bg_image; dst->bg_image_len = src->bg_image_len; dst->bg_grad = src->bg_grad; break;
    case P_BG_REPEAT: dst->bg_repeat = src->bg_repeat; break;
    case P_BG_SIZE: dst->bg_size_kind = src->bg_size_kind; dst->bg_size[0] = src->bg_size[0]; dst->bg_size[1] = src->bg_size[1]; break;
    case P_BG_POS_X: dst->bg_pos[0] = src->bg_pos[0]; break;
    case P_BG_POS_Y: dst->bg_pos[1] = src->bg_pos[1]; break;
    case P_FONT_FAMILY: dst->font_family = src->font_family; break;
    case P_FONT_SIZE: dst->font_size = src->font_size; dst->fs_default = src->fs_default; break;
    case P_FONT_WEIGHT: dst->font_weight = src->font_weight; break;
    case P_FONT_STYLE: dst->font_style = src->font_style; break;
    case P_FONT_VARIANT: dst->font_small_caps = src->font_small_caps; break;
    case P_LINE_HEIGHT: dst->line_height = src->line_height; dst->lh_factor = src->lh_factor; break;
    case P_TEXT_ALIGN: dst->text_align = src->text_align; break;
    case P_DECO_LINE: dst->deco_line = src->deco_line; break;
    case P_DECO_COLOR: dst->deco_color = src->deco_color; break;
    case P_DECO_STYLE: dst->deco_style = src->deco_style; break;
    case P_TEXT_TRANSFORM: dst->text_transform = src->text_transform; break;
    case P_TEXT_INDENT: dst->text_indent = src->text_indent; break;
    case P_TEXT_OVERFLOW: dst->text_overflow_ellipsis = src->text_overflow_ellipsis; break;
    case P_LETTER_SPACING: dst->letter_spacing = src->letter_spacing; break;
    case P_WORD_SPACING: dst->word_spacing = src->word_spacing; break;
    case P_WHITE_SPACE: dst->white_space = src->white_space; break;
    case P_WORD_BREAK: dst->word_break = src->word_break; break;
    case P_OVERFLOW_WRAP: dst->overflow_wrap = src->overflow_wrap; break;
    case P_VERTICAL_ALIGN: dst->vertical_align = src->vertical_align; dst->va_len = src->va_len; break;
    case P_DIRECTION: dst->direction_rtl = src->direction_rtl; break;
    case P_LIST_STYLE_TYPE: dst->list_style_type = src->list_style_type; dst->list_marker = src->list_marker; break;
    case P_LIST_STYLE_POSITION: dst->list_style_inside = src->list_style_inside; break;
    case P_FLEX_DIRECTION: dst->flex_direction = src->flex_direction; break;
    case P_FLEX_WRAP: dst->flex_wrap = src->flex_wrap; break;
    case P_JUSTIFY_CONTENT: dst->justify_content = src->justify_content; break;
    case P_ALIGN_ITEMS: dst->align_items = src->align_items; break;
    case P_ALIGN_SELF: dst->align_self = src->align_self; break;
    case P_ALIGN_CONTENT: dst->align_content = src->align_content; break;
    case P_JUSTIFY_ITEMS: dst->justify_items = src->justify_items; break;
    case P_JUSTIFY_SELF: dst->justify_self = src->justify_self; break;
    case P_FLEX_GROW: dst->flex_grow = src->flex_grow; break;
    case P_FLEX_SHRINK: dst->flex_shrink = src->flex_shrink; break;
    case P_FLEX_BASIS: dst->flex_basis = src->flex_basis; break;
    case P_ORDER: dst->order = src->order; break;
    case P_ROW_GAP: dst->row_gap = src->row_gap; break;
    case P_COLUMN_GAP: dst->column_gap = src->column_gap; break;
    case P_GRID_TEMPLATE_COLUMNS: dst->grid_cols = src->grid_cols; dst->grid_cols_len = src->grid_cols_len; break;
    case P_GRID_TEMPLATE_ROWS: dst->grid_rows = src->grid_rows; dst->grid_rows_len = src->grid_rows_len; break;
    case P_GRID_TEMPLATE_AREAS: dst->grid_areas = src->grid_areas; dst->grid_areas_len = src->grid_areas_len; break;
    case P_GRID_AUTO_FLOW: dst->grid_auto_flow_col = src->grid_auto_flow_col; dst->grid_auto_flow_dense = src->grid_auto_flow_dense; break;
    case P_GRID_AUTO_COLUMNS: dst->grid_auto_cols = src->grid_auto_cols; dst->grid_auto_cols_len = src->grid_auto_cols_len; break;
    case P_GRID_AUTO_ROWS: dst->grid_auto_rows = src->grid_auto_rows; dst->grid_auto_rows_len = src->grid_auto_rows_len; break;
    case P_GRID_COLUMN_START: dst->gc_start = src->gc_start; break;
    case P_GRID_COLUMN_END: dst->gc_end = src->gc_end; break;
    case P_GRID_ROW_START: dst->gr_start = src->gr_start; dst->grid_area_name = src->grid_area_name; dst->grid_area_name_len = src->grid_area_name_len; break;
    case P_GRID_ROW_END: dst->gr_end = src->gr_end; break;
    case P_BORDER_COLLAPSE: dst->border_collapse = src->border_collapse; break;
    case P_BORDER_SPACING: dst->border_spacing_h = src->border_spacing_h; dst->border_spacing_v = src->border_spacing_v; break;
    case P_TABLE_LAYOUT: dst->table_layout_fixed = src->table_layout_fixed; break;
    case P_CAPTION_SIDE: dst->caption_bottom = src->caption_bottom; break;
    case P_CONTENT: dst->content = src->content; dst->content_len = src->content_len; break;
    case P_BOX_SHADOW:
        dst->has_shadow = src->has_shadow; dst->shadow_x = src->shadow_x; dst->shadow_y = src->shadow_y;
        dst->shadow_blur = src->shadow_blur; dst->shadow_spread = src->shadow_spread;
        dst->shadow_color = src->shadow_color; dst->shadow_inset = src->shadow_inset; break;
    case P_TRANSFORM: dst->translate_x = src->translate_x; dst->translate_y = src->translate_y; break;
    case P_ASPECT_RATIO: dst->aspect_ratio = src->aspect_ratio; break;
    case P_OBJECT_FIT: dst->object_fit = src->object_fit; break;
    case P_POINTER_EVENTS: dst->pointer_events_none = src->pointer_events_none; break;
    case P_CURSOR: dst->cursor_pointer = src->cursor_pointer; break;
    case P_OUTLINE_WIDTH: dst->outline_width = src->outline_width; break;
    case P_OUTLINE_STYLE: dst->outline_style = src->outline_style; break;
    case P_OUTLINE_COLOR: dst->outline_color = src->outline_color; break;
    case P_FILL: dst->fill = src->fill; dst->fill_none = src->fill_none; break;
    case P_STROKE: dst->stroke = src->stroke; dst->stroke_none = src->stroke_none; break;
    case P_STROKE_WIDTH: dst->stroke_width = src->stroke_width; break;
    }
}

static const int32_t FS_KW[] = { 0, 9, 10, 13, 16, 18, 24, 32, 48 };

static void compute_font_size(struct cctx* c, const struct cdecl* d) {
    struct wstyle* s = c->s;
    int32_t pfs = c->parent->font_size;
    s->fs_default = 0;
    if (d->kind == VK_KW) {
        int k = d->a;
        if (k >= 1 && k <= 8) {
            s->font_size = PX(FS_KW[k]);
            if (k == 4) s->fs_default = 1;
        } else if (k == 9) s->font_size = w_muldiv(pfs, 1000, 1200);
        else if (k == 10) s->font_size = w_muldiv(pfs, 1200, 1000);
        return;
    }
    if (d->kind == VK_LEN) {
        if (d->unit == U_EM) s->font_size = w_muldiv(d->a, pfs, 1000);
        else if (d->unit == U_EX) s->font_size = w_muldiv(d->a, pfs / 2, 1000);
        else if (d->unit == U_CH) s->font_size = w_muldiv(d->a, pfs * 572 / 1000, 1000);
        else s->font_size = unit_lu(c, d->a, d->unit, pfs);
    } else if (d->kind == VK_PCT) {
        s->font_size = w_muldiv(pfs, d->a, 100000);
    } else if (d->kind == VK_CALC) {
        struct wlen w;
        if (calc_eval(c, c->sh->pool.p + d->raw, (int)d->rawlen, pfs, &w, 0))
            s->font_size = w.px + w_muldiv(pfs, w.pct, 10000);
    }
    if (s->font_size < 0) s->font_size = 0;
}

// Apply one longhand decl (after var substitution) to the style.
static void apply_decl(struct cctx* c, int p, const struct cdecl* d) {
    struct wstyle* s = c->s;
    int32_t fs = s->font_size;
    struct wlen w;
    int len;
    const char* raw;
    if (d->kind == VK_KW && d->a >= KW_INHERIT) {
        int k = d->a;
        if (k == KW_INHERIT || (k == KW_UNSET && inherited_prop(p)) || (k == KW_REVERT && inherited_prop(p)))
            copy_prop(s, c->parent, p);
        else copy_prop(s, &initial, p);
        return;
    }
    switch (p) {
    case P_DISPLAY: if (d->kind == VK_KW) s->display = (uint8_t)d->a; break;
    case P_POSITION: if (d->kind == VK_KW) s->position = (uint8_t)d->a; break;
    case P_FLOAT: if (d->kind == VK_KW) s->float_ = (uint8_t)d->a; break;
    case P_CLEAR: if (d->kind == VK_KW) s->clear = (uint8_t)d->a; break;
    case P_BOX_SIZING: if (d->kind == VK_KW) s->box_sizing = (uint8_t)d->a; break;
    case P_VISIBILITY: if (d->kind == VK_KW) s->visibility = (uint8_t)d->a; break;
    case P_OVERFLOW_X: if (d->kind == VK_KW) s->overflow_x = (uint8_t)d->a; break;
    case P_OVERFLOW_Y: if (d->kind == VK_KW) s->overflow_y = (uint8_t)d->a; break;
    case P_Z_INDEX:
        if (d->kind == VK_AUTO) { s->z_auto = 1; s->z_index = 0; }
        else if (d->kind == VK_NUM) { s->z_auto = 0; s->z_index = d->a / 1000; }
        break;
    case P_OPACITY:
        if (d->kind == VK_NUM) s->opacity = (uint8_t)W_CLAMP((d->a * 255 + 500) / 1000, 0, 255);
        break;
    case P_WIDTH: if (dlen(c, d, fs, &w)) s->width = w; break;
    case P_HEIGHT: if (dlen(c, d, fs, &w)) s->height = w; break;
    case P_MIN_WIDTH: if (dlen(c, d, fs, &w)) s->min_w = w; break;
    case P_MIN_HEIGHT: if (dlen(c, d, fs, &w)) s->min_h = w; break;
    case P_MAX_WIDTH: if (dlen(c, d, fs, &w)) s->max_w = w; break;
    case P_MAX_HEIGHT: if (dlen(c, d, fs, &w)) s->max_h = w; break;
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        if (dlen(c, d, fs, &w)) s->margin[p - P_MARGIN_TOP] = w;
        break;
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        if (dlen(c, d, fs, &w) && w.t == WL_LEN) s->padding[p - P_PADDING_TOP] = w;
        break;
    case P_TOP: case P_RIGHT: case P_BOTTOM: case P_LEFT:
        if (dlen(c, d, fs, &w)) s->inset[p - P_TOP] = w;
        break;
    case P_BTW: case P_BRW: case P_BBW: case P_BLW:
        if (dlen(c, d, fs, &w) && w.t == WL_LEN) {
            int32_t v = w.px;
            // widths snap to device pixels (>= 1px when non-zero)
            if (v > 0 && v < LU) v = LU;
            else v = (v + LU / 2) & ~(LU - 1);
            s->bw[p - P_BTW] = v;
        }
        break;
    case P_BTS: case P_BRS: case P_BBS: case P_BLS:
        if (d->kind == VK_KW) s->bs[p - P_BTS] = (uint8_t)d->a;
        break;
    case P_BTC: case P_BRC: case P_BBC: case P_BLC:
        if (d->kind == VK_COLOR) s->bc[p - P_BTC] = d->b ? CUR_COLOR : (uint32_t)d->a;
        break;
    case P_RTL: case P_RTR: case P_RBR: case P_RBL:
        if (dlen(c, d, fs, &w) && w.t == WL_LEN) s->radius[p - P_RTL] = w;
        break;
    case P_COLOR:
        if (d->kind == VK_COLOR) s->color = d->b ? c->parent->color : (uint32_t)d->a;
        break;
    case P_BG_COLOR:
        if (d->kind == VK_COLOR) s->bg_color = d->b ? CUR_COLOR : (uint32_t)d->a;
        break;
    case P_BG_IMAGE: raw = draw_of(c, d, &len); bg_image(c, raw, len); break;
    case P_BG_REPEAT: if (d->kind == VK_KW) s->bg_repeat = (uint8_t)d->a; break;
    case P_BG_SIZE: raw = draw_of(c, d, &len); bg_size(c, raw, len); break;
    case P_BG_POS_X: {
        raw = draw_of(c, d, &len);
        // "background-position: x y" arrives here as one raw value
        int q = 0; const char* t1; int t1l; const char* t2; int t2l;
        if (cv_next(raw, len, &q, &t1, &t1l) && cv_next(raw, len, &q, &t2, &t2l)) {
            int swap = w_ieq(t1, t1l, "top") || w_ieq(t1, t1l, "bottom") ||
                       w_ieq(t2, t2l, "left") || w_ieq(t2, t2l, "right");
            if (swap) { bg_pos_one(c, t2, t2l, 0); bg_pos_one(c, t1, t1l, 1); }
            else { bg_pos_one(c, t1, t1l, 0); bg_pos_one(c, t2, t2l, 1); }
        } else {
            if (len && (w_ieq(raw, len, "top") || w_ieq(raw, len, "bottom"))) {
                bg_pos_one(c, "center", 6, 0); bg_pos_one(c, raw, len, 1);
            } else {
                bg_pos_one(c, raw, len, 0);
                if (w_ieq(raw, len, "left") || w_ieq(raw, len, "right") || w_ieq(raw, len, "center"))
                    bg_pos_one(c, "center", 6, 1);
            }
        }
        break;
    }
    case P_BG_POS_Y: raw = draw_of(c, d, &len); bg_pos_one(c, raw, len, 1); break;
    case P_FONT_FAMILY: {
        raw = draw_of(c, d, &len);
        int gm;
        int f = family_of(raw, len, &gm);
        if (f >= 0) s->font_family = (uint8_t)f;
        break;
    }
    case P_FONT_SIZE: break; // computed earlier
    case P_FONT_WEIGHT:
        if (d->kind == VK_KW) {
            int v = d->a;
            int pw = c->parent->font_weight;
            if (v == -2) v = pw < 350 ? 400 : pw < 550 ? 700 : 900;
            else if (v == -3) v = pw < 550 ? 100 : pw < 750 ? 400 : 700;
            s->font_weight = (uint16_t)v;
        }
        break;
    case P_FONT_STYLE: if (d->kind == VK_KW) s->font_style = (uint8_t)d->a; break;
    case P_FONT_VARIANT: if (d->kind == VK_KW) s->font_small_caps = (uint8_t)d->a; break;
    case P_LINE_HEIGHT:
        if (d->kind == VK_KW && d->a == 0) { s->line_height = 0; s->lh_factor = 0; }
        else if (d->kind == VK_NUM) { s->lh_factor = d->a; s->line_height = w_muldiv(fs, d->a, 1000); }
        else if (dlen(c, d, fs, &w) && w.t == WL_LEN) {
            s->lh_factor = 0;
            s->line_height = w.px + w_muldiv(fs, w.pct, 10000);
        }
        break;
    case P_TEXT_ALIGN: if (d->kind == VK_KW) s->text_align = (uint8_t)d->a; break;
    case P_DECO_LINE: if (d->kind == VK_KW) s->deco_line = (uint8_t)d->a; break;
    case P_DECO_COLOR: if (d->kind == VK_COLOR) s->deco_color = d->b ? CUR_COLOR : (uint32_t)d->a; break;
    case P_DECO_STYLE: if (d->kind == VK_KW) s->deco_style = (uint8_t)d->a; break;
    case P_TEXT_TRANSFORM: if (d->kind == VK_KW) s->text_transform = (uint8_t)d->a; break;
    case P_TEXT_INDENT: if (dlen(c, d, fs, &w)) s->text_indent = w; break;
    case P_TEXT_OVERFLOW: if (d->kind == VK_KW) s->text_overflow_ellipsis = (uint8_t)d->a; break;
    case P_LETTER_SPACING:
        if (d->kind == VK_LEN || d->kind == VK_CALC) { if (dlen(c, d, fs, &w)) s->letter_spacing = w.px; }
        else s->letter_spacing = 0;
        break;
    case P_WORD_SPACING:
        if (d->kind == VK_LEN || d->kind == VK_CALC) { if (dlen(c, d, fs, &w)) s->word_spacing = w.px; }
        else s->word_spacing = 0;
        break;
    case P_WHITE_SPACE: if (d->kind == VK_KW) s->white_space = (uint8_t)d->a; break;
    case P_WORD_BREAK:
        if (d->kind == VK_KW) {
            if (d->a == WB_BREAK_WORD) { s->word_break = WB_NORMAL; s->overflow_wrap = 1; }
            else s->word_break = (uint8_t)d->a;
        }
        break;
    case P_OVERFLOW_WRAP: if (d->kind == VK_KW) s->overflow_wrap = (uint8_t)d->a; break;
    case P_VERTICAL_ALIGN:
        if (d->kind == VK_KW) s->vertical_align = (uint8_t)d->a;
        else if (dlen(c, d, fs, &w) && w.t == WL_LEN) {
            s->vertical_align = VA_LEN;
            int32_t lh = s->line_height ? s->line_height : fs * 12 / 10;
            s->va_len = w.px + w_muldiv(lh, w.pct, 10000);
        }
        break;
    case P_DIRECTION: if (d->kind == VK_KW) s->direction_rtl = (uint8_t)d->a; break;
    case P_LIST_STYLE_TYPE:
        if (d->kind == VK_KW) { s->list_style_type = (uint8_t)d->a; s->list_marker = 0; }
        else if (d->kind == VK_RAW) {
            raw = draw_of(c, d, &len);
            if (len >= 2 && (raw[0] == '"' || raw[0] == '\'')) {
                s->list_style_type = LS_STRING;
                s->list_marker = str_intern(c->ss, raw + 1, len - 2);
            }
        }
        break;
    case P_LIST_STYLE_POSITION: if (d->kind == VK_KW) s->list_style_inside = (uint8_t)d->a; break;
    case P_FLEX_DIRECTION: if (d->kind == VK_KW) s->flex_direction = (uint8_t)d->a; break;
    case P_FLEX_WRAP: if (d->kind == VK_KW) s->flex_wrap = (uint8_t)d->a; break;
    case P_JUSTIFY_CONTENT: if (d->kind == VK_KW) s->justify_content = (uint8_t)d->a; break;
    case P_ALIGN_ITEMS: if (d->kind == VK_KW) s->align_items = (uint8_t)d->a; break;
    case P_ALIGN_SELF: if (d->kind == VK_KW) s->align_self = (uint8_t)d->a; break;
    case P_ALIGN_CONTENT: if (d->kind == VK_KW) s->align_content = (uint8_t)d->a; break;
    case P_JUSTIFY_ITEMS: if (d->kind == VK_KW) s->justify_items = (uint8_t)d->a; break;
    case P_JUSTIFY_SELF: if (d->kind == VK_KW) s->justify_self = (uint8_t)d->a; break;
    case P_FLEX_GROW: if (d->kind == VK_NUM && d->a >= 0) s->flex_grow = d->a; break;
    case P_FLEX_SHRINK: if (d->kind == VK_NUM && d->a >= 0) s->flex_shrink = d->a; break;
    case P_FLEX_BASIS: if (dlen(c, d, fs, &w)) s->flex_basis = w; break;
    case P_ORDER: if (d->kind == VK_NUM) s->order = d->a / 1000; break;
    case P_ROW_GAP:
        if (d->kind == VK_KW || d->kind == VK_AUTO) s->row_gap = wl_px(0);
        else if (dlen(c, d, fs, &w)) s->row_gap = w;
        break;
    case P_COLUMN_GAP:
        if (d->kind == VK_KW || d->kind == VK_AUTO) s->column_gap = wl_px(0);
        else if (dlen(c, d, fs, &w)) s->column_gap = w;
        break;
    case P_GRID_TEMPLATE_COLUMNS: case P_GRID_TEMPLATE_ROWS: case P_GRID_TEMPLATE_AREAS:
    case P_GRID_AUTO_COLUMNS: case P_GRID_AUTO_ROWS: {
        raw = draw_of(c, d, &len);
        cv_trim(&raw, &len);
        int32_t ref = (len && !w_ieq(raw, len, "none") && !w_ieq(raw, len, "auto")) ? str_intern(c->ss, raw, len) : 0;
        int32_t rl = ref ? len : 0;
        if (p == P_GRID_TEMPLATE_COLUMNS) { s->grid_cols = ref; s->grid_cols_len = rl; }
        else if (p == P_GRID_TEMPLATE_ROWS) { s->grid_rows = ref; s->grid_rows_len = rl; }
        else if (p == P_GRID_TEMPLATE_AREAS) { s->grid_areas = ref; s->grid_areas_len = rl; }
        else if (p == P_GRID_AUTO_COLUMNS) { s->grid_auto_cols = ref; s->grid_auto_cols_len = rl; }
        else { s->grid_auto_rows = ref; s->grid_auto_rows_len = rl; }
        break;
    }
    case P_GRID_AUTO_FLOW: {
        raw = draw_of(c, d, &len);
        s->grid_auto_flow_col = 0; s->grid_auto_flow_dense = 0;
        int q = 0; const char* t; int tl;
        while (cv_next(raw, len, &q, &t, &tl)) {
            if (w_ieq(t, tl, "column")) s->grid_auto_flow_col = 1;
            else if (w_ieq(t, tl, "dense")) s->grid_auto_flow_dense = 1;
        }
        break;
    }
    case P_GRID_COLUMN_START: raw = draw_of(c, d, &len); s->gc_start = grid_line(c, raw, len, 0, 0); break;
    case P_GRID_COLUMN_END: raw = draw_of(c, d, &len); s->gc_end = grid_line(c, raw, len, 0, 0); break;
    case P_GRID_ROW_START:
        raw = draw_of(c, d, &len);
        s->grid_area_name = 0; s->grid_area_name_len = 0;
        s->gr_start = grid_line(c, raw, len, &s->grid_area_name, &s->grid_area_name_len);
        break;
    case P_GRID_ROW_END: raw = draw_of(c, d, &len); s->gr_end = grid_line(c, raw, len, 0, 0); break;
    case P_BORDER_COLLAPSE: if (d->kind == VK_KW) s->border_collapse = (uint8_t)d->a; break;
    case P_BORDER_SPACING:
        if (d->kind == VK_LEN2) {
            s->border_spacing_h = unit_lu(c, d->a, d->unit, fs);
            s->border_spacing_v = unit_lu(c, d->b, d->unit2, fs);
        }
        break;
    case P_TABLE_LAYOUT: if (d->kind == VK_KW) s->table_layout_fixed = (uint8_t)d->a; break;
    case P_CAPTION_SIDE: if (d->kind == VK_KW) s->caption_bottom = (uint8_t)d->a; break;
    case P_CONTENT: raw = draw_of(c, d, &len); gen_content(c, raw, len); break;
    case P_BOX_SHADOW: raw = draw_of(c, d, &len); box_shadow(c, raw, len); break;
    case P_TRANSFORM: raw = draw_of(c, d, &len); transform(c, raw, len); break;
    case P_ASPECT_RATIO: raw = draw_of(c, d, &len); aspect_ratio(c, raw, len); break;
    case P_OBJECT_FIT: if (d->kind == VK_KW) s->object_fit = (uint8_t)d->a; break;
    case P_POINTER_EVENTS: if (d->kind == VK_KW) s->pointer_events_none = (uint8_t)d->a; break;
    case P_CURSOR: raw = draw_of(c, d, &len); s->cursor_pointer = w_ieq_prefix(raw, len, "pointer") ? 1 : 0; break;
    case P_OUTLINE_WIDTH: if (dlen(c, d, fs, &w) && w.t == WL_LEN) s->outline_width = w.px; break;
    case P_OUTLINE_STYLE: if (d->kind == VK_KW) s->outline_style = (uint8_t)d->a; break;
    case P_OUTLINE_COLOR: if (d->kind == VK_COLOR) s->outline_color = d->b ? CUR_COLOR : (uint32_t)d->a; break;
    case P_FILL: { raw = draw_of(c, d, &len); int none; s->fill = paint_color(raw, len, &none); s->fill_none = (uint8_t)none; break; }
    case P_STROKE: { raw = draw_of(c, d, &len); int none; s->stroke = paint_color(raw, len, &none); s->stroke_none = (uint8_t)none; break; }
    case P_STROKE_WIDTH: if (dlen(c, d, fs, &w) && w.t == WL_LEN) s->stroke_width = w.px; break;
    }
}

static uint32_t style_hash(const struct wstyle* s) {
    const unsigned char* b = (const unsigned char*)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *s; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static int32_t intern_style(struct wstyleset* ss, const struct wstyle* s) {
    if (ss->nstyles * 2 >= ss->shcap) {
        int nc = ss->shcap ? ss->shcap * 2 : 1024;
        int32_t* nh = (int32_t*)w_malloc(nc * sizeof(int32_t));
        if (!nh) return -1;
        for (int i = 0; i < nc; i++) nh[i] = -1;
        for (int i = 0; i < ss->nstyles; i++) {
            uint32_t h = style_hash(&ss->styles[i]) & (nc - 1);
            while (nh[h] >= 0) h = (h + 1) & (nc - 1);
            nh[h] = i;
        }
        w_free(ss->shash);
        ss->shash = nh;
        ss->shcap = nc;
    }
    uint32_t h = style_hash(s) & (ss->shcap - 1);
    while (ss->shash[h] >= 0) {
        int32_t i = ss->shash[h];
        if (!memcmp(&ss->styles[i], s, sizeof *s)) return i;
        h = (h + 1) & (ss->shcap - 1);
    }
    if (ss->nstyles >= ss->capstyles) {
        int nc = ss->capstyles ? ss->capstyles * 2 : 256;
        struct wstyle* n = (struct wstyle*)w_realloc(ss->styles, nc * sizeof(struct wstyle));
        if (!n) return -1;
        ss->styles = n;
        ss->capstyles = nc;
    }
    ss->styles[ss->nstyles] = *s;
    ss->shash[h] = ss->nstyles;
    return ss->nstyles++;
}

static int is_atomic_or_abs(const struct wstyle* s) {
    return s->display == D_INLINE_BLOCK || s->display == D_INLINE_FLEX ||
           s->display == D_INLINE_GRID || s->display == D_INLINE_TABLE ||
           s->float_ != FL_NONE || s->position == POS_ABSOLUTE || s->position == POS_FIXED;
}

static uint8_t blockify(uint8_t d) {
    switch (d) {
    case D_INLINE: case D_INLINE_BLOCK: case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP:
    case D_TABLE_FOOTER_GROUP: case D_TABLE_ROW: case D_TABLE_CELL: case D_TABLE_COLUMN:
    case D_TABLE_COLUMN_GROUP: case D_TABLE_CAPTION:
        return D_BLOCK;
    case D_INLINE_FLEX: return D_FLEX;
    case D_INLINE_GRID: return D_GRID;
    case D_INLINE_TABLE: return D_TABLE;
    }
    return d;
}

// Compute the style of element el (pe = PE_NONE) or one of its
// pseudo-elements. parent: the parent's computed style. Returns style index.
static int32_t compute_one(struct wstyleset* ss, int el, int pe, const struct wstyle* parent,
                           const struct wstyle* root, int hint_first, int hint_count,
                           int inl_first, int inl_count) {
    static struct slot slots[P_COUNT];
    static const struct cdecl* customs[512];
    static const struct wsheet* custom_sh[512];
    int ncustom;
    cascade(ss, pe, slots, customs, custom_sh, &ncustom, hint_first, hint_count, inl_first, inl_count);
    if (pe != PE_NONE) {
        // pseudo-element exists only with content
        if (!slots[P_CONTENT].d && pe != PE_MARKER) return -1;
    }
    struct wstyle s;
    memcpy(&s, &initial, sizeof s);
    // inherited properties from the parent
    for (int p = 1; p < P_COUNT; p++) if (inherited_prop(p)) copy_prop(&s, parent, p);
    s.deco_inh = 0;
    s.deco_inh_color = 0;
    s.lh_factor = parent->lh_factor;
    s.vars = parent->vars;
    // custom properties
    if (ncustom) {
        struct wcvars* vb = vars_block(ss, parent->vars, ncustom);
        if (vb) {
            for (int i = 0; i < ncustom; i++) {
                const struct cdecl* d = customs[i];
                const struct wsheet* sh = custom_sh[i];
                const char* nm = sh->pool.p + d->name;
                const char* vv = sh->pool.p + d->raw;
                int vl = (int)d->rawlen;
                // custom values may reference other vars: substitute now
                struct wbuf sb = { 0, 0, 0 };
                int ok = 1;
                for (int k = 0; k + 4 <= vl; k++) if (w_ieq_prefix(vv + k, vl - k, "var(")) { ok = 0; break; }
                uint32_t voff, vlen;
                if (ok) {
                    int32_t r = str_intern(ss, vv, vl);
                    voff = (uint32_t)(r - 1); vlen = (uint32_t)vl;
                } else {
                    if (var_subst(ss, vb, vv, vl, &sb, 0)) {
                        int32_t r = str_intern(ss, sb.p ? sb.p : "", sb.len);
                        voff = (uint32_t)(r - 1); vlen = (uint32_t)sb.len;
                    } else { voff = 0; vlen = 0; }
                    wbuf_free(&sb);
                }
                // intern name too
                int32_t nr = str_intern(ss, nm, (int)d->namelen);
                struct wcvar* cv = &vb->v[vb->n++];
                cv->hash = sh_hash(nm, (int)d->namelen);
                cv->name = (uint32_t)(nr - 1);
                cv->nlen = d->namelen;
                cv->val = voff;
                cv->vlen = vlen;
            }
            s.vars = vb;
        }
    }
    struct cctx c;
    c.ss = ss;
    c.parent = parent;
    c.s = &s;
    c.el = el;
    c.root_fs = root ? root->font_size : PX(16);
    // var() substitution: each VAR slot re-parses its (shorthand) property
    // with substituted text and takes the longhand it stands for.
    ss->var_sh.ndecl = 0;
    ss->var_sh.pool.len = 0;
    static int vidx[P_COUNT];
    for (int p = 1; p < P_COUNT; p++) {
        const struct cdecl* d = slots[p].d;
        if (!d || d->kind != VK_VAR) continue;
        const struct wsheet* sh = slots[p].sh;
        struct wbuf out = { 0, 0, 0 };
        int ok = var_subst(ss, s.vars, sh->pool.p + d->raw, (int)d->rawlen, &out, 0);
        slots[p].d = 0;
        if (ok) {
            int before = ss->var_sh.ndecl;
            memcpy(ss->var_sh.base, sh->base, sizeof ss->var_sh.base);
            cparse_value(&ss->var_sh, sh->pool.p + d->name, (int)d->namelen,
                         out.p ? out.p : "", out.len, d->important);
            for (int k = ss->var_sh.ndecl - 1; k >= before; k--)
                if (ss->var_sh.decls[k].prop == p) { vidx[p] = k + 1; break; }
        }
        wbuf_free(&out);
    }
    // var_sh.decls may have been reallocated while expanding: resolve the
    // recorded indices to pointers only now
    for (int p = 1; p < P_COUNT; p++) {
        if (!vidx[p]) continue;
        slots[p].d = &ss->var_sh.decls[vidx[p] - 1];
        slots[p].sh = &ss->var_sh;
        vidx[p] = 0;
    }
    // font-size first (em units depend on it)
    if (slots[P_FONT_SIZE].d) {
        c.sh = slots[P_FONT_SIZE].sh;
        const struct cdecl* d = slots[P_FONT_SIZE].d;
        if (d->kind == VK_KW && d->a >= KW_INHERIT) {
            if (d->a == KW_INITIAL) { s.font_size = PX(16); s.fs_default = 1; }
        } else compute_font_size(&c, d);
    }
    // font-family next (monospace default size quirk depends on it)
    if (slots[P_FONT_FAMILY].d) {
        c.sh = slots[P_FONT_FAMILY].sh;
        apply_decl(&c, P_FONT_FAMILY, slots[P_FONT_FAMILY].d);
    }
    if (s.fs_default && s.font_family == FAM_MONO && slots[P_FONT_FAMILY].d) {
        int gm = 0;
        const struct cdecl* fd = slots[P_FONT_FAMILY].d;
        if (fd->kind == VK_RAW) {
            family_of(slots[P_FONT_FAMILY].sh->pool.p + fd->raw, (int)fd->rawlen, &gm);
            if (gm) s.font_size = PX(13);
        }
    }
    // line-height inherited as a number scales with the new font size
    if (!slots[P_LINE_HEIGHT].d) {
        if (parent->lh_factor) s.line_height = w_muldiv(s.font_size, parent->lh_factor, 1000);
    }
    // color before currentColor users
    if (slots[P_COLOR].d) { c.sh = slots[P_COLOR].sh; apply_decl(&c, P_COLOR, slots[P_COLOR].d); }
    for (int p = 1; p < P_COUNT; p++) {
        if (p == P_FONT_SIZE || p == P_FONT_FAMILY || p == P_COLOR) continue;
        if (!slots[p].d) continue;
        c.sh = slots[p].sh;
        apply_decl(&c, p, slots[p].d);
    }
    // resolve currentColor sentinels
    for (int i = 0; i < 4; i++) if (s.bc[i] == CUR_COLOR) s.bc[i] = s.color;
    if (s.bg_color == CUR_COLOR) s.bg_color = s.color;
    if (s.deco_color == CUR_COLOR) s.deco_color = s.color;
    if (s.outline_color == CUR_COLOR) s.outline_color = s.color;
    if (s.shadow_color == CUR_COLOR) s.shadow_color = s.color;
    if (s.fill == CUR_COLOR) s.fill = s.color;
    if (s.stroke == CUR_COLOR) s.stroke = s.color;
    if (s.bg_grad >= 0) {
        struct wgrad g = ss->grads[s.bg_grad];
        int changed = 0;
        for (int i = 0; i < g.nstops; i++) if (g.color[i] == CUR_COLOR) { g.color[i] = s.color; changed = 1; }
        if (changed) s.bg_grad = (int16_t)grad_add(ss, &g);
    }
    // used border widths: none/hidden -> 0
    for (int i = 0; i < 4; i++) if (s.bs[i] == BS_NONE || s.bs[i] == BS_HIDDEN) s.bw[i] = 0;
    if (s.outline_style == BS_NONE) s.outline_width = 0;
    // blockification
    if (pe == PE_NONE) {
        int pd = parent->display;
        int parent_fg = (pd == D_FLEX || pd == D_INLINE_FLEX || pd == D_GRID || pd == D_INLINE_GRID);
        if (el >= 0 && ss->d->n[el].parent == 0) s.display = blockify(s.display);
        else if (s.position == POS_ABSOLUTE || s.position == POS_FIXED) { s.display = blockify(s.display); s.float_ = FL_NONE; }
        else if (s.float_ != FL_NONE) s.display = blockify(s.display);
        if (parent_fg && el >= 0 && s.position != POS_ABSOLUTE && s.position != POS_FIXED) {
            s.display = blockify(s.display);
            s.float_ = FL_NONE;
        }
    } else if (pe == PE_MARKER) {
        s.display = D_INLINE;
    } else {
        if (s.position == POS_ABSOLUTE || s.position == POS_FIXED || s.float_ != FL_NONE)
            s.display = blockify(s.display);
        int pd = parent->display;
        if (pd == D_FLEX || pd == D_INLINE_FLEX || pd == D_GRID || pd == D_INLINE_GRID)
            s.display = blockify(s.display);
    }
    // text-decoration propagation
    if (!is_atomic_or_abs(&s)) {
        s.deco_inh = parent->deco_inh | parent->deco_line;
        s.deco_inh_color = parent->deco_line ? parent->deco_color : parent->deco_inh_color;
    }
    // <li value>
    if (pe == PE_NONE && el >= 0 && wdom_is(ss->d, el, T_li)) {
        int vl;
        const char* v = wdom_attr(ss->d, el, A_value, &vl);
        if (v) {
            int32_t m; int u;
            if (cv_number(v, vl, &m, &u)) { s.list_value_set = 1; s.list_value = m / 1000; }
        }
    }
    if (pe == PE_NONE && el >= 0 && wdom_is(ss->d, el, T_ol)) {
        int vl;
        const char* v = wdom_attr(ss->d, el, A_start, &vl);
        if (v) {
            int32_t m; int u;
            if (cv_number(v, vl, &m, &u)) { s.list_value_set = 1; s.list_value = m / 1000; }
        }
        if (wdom_has_attr(ss->d, el, A_reversed)) s.list_value_set |= 2;
    }
    return intern_style(ss, &s);
}

static void bloom_el(const struct wdom* d, int el, int delta) {
    const struct wnode* n = &d->n[el];
    bloom_add(bloom_hash((3u << 16) | n->tag), delta);
    if (n->id) bloom_add(bloom_hash((1u << 16) | n->id), delta);
    const uint16_t* cl = d->classes + n->text;
    for (int i = 0; i < n->nclass; i++) bloom_add(bloom_hash((2u << 16) | cl[i]), delta);
}

static int ensure_nodes(struct wstyleset* ss) {
    int need = ss->d->nn;
    if (need <= ss->node_cap) return 1;
    int nc = need + 256;
    int32_t* a = (int32_t*)w_realloc(ss->node_style, nc * sizeof(int32_t));
    if (!a) return 0;
    ss->node_style = a;
    int32_t* b = (int32_t*)w_realloc(ss->node_pseudo, nc * 3 * sizeof(int32_t));
    if (!b) return 0;
    ss->node_pseudo = b;
    ss->node_cap = nc;
    return 1;
}

void css_compute_all(struct wstyleset* ss, int vw, int vh) {
    if (!initial_ready) initial_init();
    ua_init();
    struct wdom* d = ss->d;
    if (!ensure_nodes(ss)) return;
    if (ss->index_dirty) build_index(ss);
    ss->vw = vw;
    ss->vh = vh;
    eval_mqs(ss);
    // reset computed data (styles are re-interned each pass)
    ss->nstyles = 0;
    for (int i = 0; i < ss->shcap; i++) ss->shash[i] = -1;
    for (int i = 0; i < ss->nvb; i++) w_free(ss->vblocks[i]);
    ss->nvb = 0;
    ss->ngrad = 0;
    for (int i = 0; i < d->nn; i++) {
        ss->node_style[i] = -1;
        ss->node_pseudo[i * 3] = ss->node_pseudo[i * 3 + 1] = ss->node_pseudo[i * 3 + 2] = -1;
    }
    ss->inl.ndecl = 0;
    ss->inl.pool.len = 0;
    // explicit DFS with a parent-style stack
    struct frame { int el; int32_t style; };
    int cap = 1024;
    struct frame* stack = (struct frame*)w_malloc(cap * sizeof(struct frame));
    if (!stack) return;
    memset(bloom, 0, sizeof bloom);
    struct wstyle root_parent;
    memcpy(&root_parent, &initial, sizeof root_parent);
    root_parent.display = D_BLOCK;
    int32_t root_idx = -1;
    int sp = 0;
    // iterate children of the document
    int node = d->n[0].first;
    while (node >= 0) {
        int descend = 0;
        if (d->n[node].type == WN_ELEM) {
            // parent style: nearest element frame
            while (sp > 0 && d->n[node].parent != stack[sp - 1].el) { sp--; bloom_el(d, stack[sp].el, -1); }
            const struct wstyle* ps = sp ? &ss->styles[stack[sp - 1].style] : &root_parent;
            const struct wstyle* rs = root_idx >= 0 ? &ss->styles[root_idx] : 0;
            gather(ss, node);
            ss->hints.ndecl = 0;
            ss->hints.pool.len = 0;
            int hf, hc;
            gen_hints(ss, node, &hf, &hc);
            int inf = 0, inc = 0;
            int sl;
            const char* sv = wdom_attr(d, node, A_style, &sl);
            if (sv && sl) csheet_parse_decls(&ss->inl, &d->atoms, sv, sl, &inf, &inc);
            // ps may be invalidated by intern_style realloc: copy it
            struct wstyle pcopy;
            memcpy(&pcopy, ps, sizeof pcopy);
            int32_t si = compute_one(ss, node, PE_NONE, &pcopy, rs, hf, hc, inf, inc);
            if (si < 0) { node = wdom_next(d, node, 0); continue; }
            ss->node_style[node] = si;
            if (d->n[node].parent == 0) root_idx = si;
            struct wstyle me;
            memcpy(&me, &ss->styles[si], sizeof me);
            if (me.display != D_NONE) {
                const struct wstyle* rs2 = root_idx >= 0 ? &ss->styles[root_idx] : 0;
                struct wstyle rcopy;
                if (rs2) memcpy(&rcopy, rs2, sizeof rcopy);
                // pseudo-elements: only when some matched rule targets them
                int has_b = 0, has_a = 0, has_m = 0;
                for (int m = 0; m < ss->nmatch; m++) {
                    int pe = ss->match[m]->sh->rules[ss->match[m]->rule].pseudo;
                    if (pe == PE_BEFORE) has_b = 1;
                    else if (pe == PE_AFTER) has_a = 1;
                    else if (pe == PE_MARKER) has_m = 1;
                }
                if (has_b) ss->node_pseudo[node * 3] = compute_one(ss, node, PE_BEFORE, &me, rs2 ? &rcopy : 0, 0, 0, 0, 0);
                if (has_a) ss->node_pseudo[node * 3 + 1] = compute_one(ss, node, PE_AFTER, &me, rs2 ? &rcopy : 0, 0, 0, 0, 0);
                if (has_m) ss->node_pseudo[node * 3 + 2] = compute_one(ss, node, PE_MARKER, &me, rs2 ? &rcopy : 0, 0, 0, 0, 0);
                descend = d->n[node].first >= 0;
                if (descend) {
                    if (sp >= cap) {
                        int nc = cap * 2;
                        struct frame* ns = (struct frame*)w_realloc(stack, nc * sizeof(struct frame));
                        if (!ns) break;
                        stack = ns;
                        cap = nc;
                    }
                    stack[sp].el = node;
                    stack[sp].style = si;
                    sp++;
                    bloom_el(d, node, 1);
                }
            }
        }
        if (descend) { node = d->n[node].first; continue; }
        // next sibling or climb
        while (node >= 0 && d->n[node].next < 0) {
            node = d->n[node].parent;
            if (node <= 0) { node = -1; break; }
        }
        if (node >= 0) node = d->n[node].next;
    }
    while (sp > 0) { sp--; bloom_el(d, stack[sp].el, -1); }
    w_free(stack);
    for (int i = 0; i < d->nn; i++) d->n[i].aux = ss->node_style[i];
}

const struct wstyle* css_style_of(const struct wstyleset* ss, int node) {
    if (node < 0 || node >= ss->node_cap || node >= ss->d->nn) return 0;
    int32_t i = ss->node_style[node];
    return i >= 0 ? &ss->styles[i] : 0;
}

const struct wstyle* css_pseudo_of(const struct wstyleset* ss, int node, int which) {
    if (node < 0 || node >= ss->node_cap || which < 1 || which > 3) return 0;
    int32_t i = ss->node_pseudo[node * 3 + which - 1];
    return i >= 0 ? &ss->styles[i] : 0;
}
