// ablock.c — the built-in, always-on adblocker for okai (see ablock.h).
// Speaks the EasyList/uBlock filter syntax. The lists are data; everything
// here is ours, written from the syntax's documented semantics.
//
// Parse: one line at a time into compact records. Patterns and selectors stay
// zero-copy in the list text (the boot module memory); only lowercased domain
// names and hosts are copied into bump chunks. Case-insensitive matching for
// network filters (the URL is lowercased per check), case-sensitive names for
// cosmetic selectors (CSS class/id matching is case-sensitive).
//
// Index (built once, after the last line):
//   - a host hash set for the `||host^` block rules (parent domains checked),
//   - token buckets for the rest (uBlock's anchor-token scheme: every eligible
//     pattern carries its longest useful alphanumeric token, and any URL the
//     pattern matches must contain that token as a run — tokens adjacent to a
//     wildcard are excluded, so the guarantee holds),
//   - name-hash buckets for cosmetic rules, keyed on the rightmost compound's
//     class/id/tag/attribute name.
//
// ablock_allows() verdict order: $important blocks > exceptions > blocks.
// Untyped network filters never match a top-level document (uBlock
// semantics), or navigating to a listed host would brick the browser.
// Regex filters are unwrapped and matched as their literal inner text: plain
// "/ads/" behaves exactly like uBlock's regex /ads/ (a substring match), and
// regexes with metacharacters fail open (never match) rather than wrong.
//
// Main loop only, no locks. Parsing is sliced for the idle path.

#include <stdint.h>
#include "ablock.h"
#include "web/wcommon.h"
#include "web/wdom.h"
#include "web/css.h"

#if defined(KERNEL) && KERNEL
#include "memory.h"     // memory_boot_module
extern uint32_t tick_count;
#endif

#define AB_URL_MAX 8192

// ---- records ------------------------------------------------------------------------

// resource type bits (network filter options)
#define TB_MAIN   0x001
#define TB_FRAME  0x002
#define TB_CSS    0x004
#define TB_IMG    0x008
#define TB_SCRIPT 0x010
#define TB_XHR    0x020
#define TB_FONT   0x040
#define TB_MEDIA  0x080
#define TB_OTHER  0x100
#define TB_ALL    0x1FF

// filter flags
#define AF_EXC   0x0001  // @@ exception
#define AF_IMP   0x0002  // $important
#define AF_TP    0x0004  // third-party only
#define AF_FP    0x0008  // first-party only
#define AF_ENDA  0x0010  // pattern ended with '|'
#define AF_BF    0x0020  // $badfilter (disables the matching filter)
#define AF_DEAD  0x0040  // removed by a $badfilter
#define AF_HOST  0x0080  // lives in the host hash set (skipped by the token index)
#define AF_REDIR 0x0100  // $redirect=<resource>: block, but serve a built-in resource

// pattern anchors
#define AN_NONE  0
#define AN_HOST  1    // '||' — hostname start or a dot boundary inside it
#define AN_START 2    // '|' — absolute start of the URL

struct adom {             // one domain= / cosmetic-domain entry (lowercased copy)
    const char* s;
    uint16_t len;
    uint8_t  neg;         // '~'
};

struct afilter {          // one network filter
    const char* pat;      // as written (slice of the list text)
    uint16_t plen;
    uint16_t types;       // type bitmask, 0 = untyped
    uint16_t flags;
    uint8_t  anchor;
    uint8_t  lit0;        // lowercased first literal char for quick reject, 0 = none
    uint8_t  rres;        // $redirect= resource index, 0xff = none (see ares[])
    uint16_t ndom;        // domain= entries at dom[dombase..] (0 = none)
    uint32_t dombase;
    uint32_t thash;       // anchor-token hash (case-folded), 0 = tokenless
};

// built-in $redirect= resources (content ours; the names are uBlock's so its
// lists address them)
struct ares { const char* name; const char* ctype; const char* bytes; uint32_t len; };
static const struct ares ARES[] = {
    { "noopjs",   "application/javascript", "/* ablock noop */\n", 19 },
    { "nooptext", "text/plain",             "", 0 },
    { "noop.txt", "text/plain",             "", 0 },
    { "noopjson", "application/json",       "{}\n", 3 },
    // 1x1 transparent GIF (87a header + logical screen + color table + image)
    { "1x1.gif",  "image/gif",
      "\x47\x49\x46\x38\x37\x61\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00"
      "\xff\xff\xff\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44"
      "\x01\x00\x3b", 43 },
};
#define ARES_N 5
#define RRES_NONE 0xff

// one ##+js(...) scriptlet (uBlock syntax; our own implementations, see the
// JS library at the bottom of the file)
#define SC_MAX_ARGS 6
struct asc {
    const char* name;          // scriptlet name as written (alias or full)
    uint16_t nlen;
    const char* args[SC_MAX_ARGS];
    uint8_t alen[SC_MAX_ARGS];
    uint8_t nargs;
    uint8_t ndom;
    uint32_t dombase;
};

struct ahide {            // one cosmetic rule (## hide or #@# unhide)
    const char* sel;      // selector text as written
    uint16_t slen;
    uint8_t  unhide;
    uint8_t  ndom;
    uint32_t dombase;
    uint32_t key;         // name hash of the rightmost compound key, 0 = unkeyable
    const char* ht;       // :has-text(needle) argument, 0 = plain CSS rule
    uint16_t htl;
};

#define ABT_BITS 16                       // network token buckets
#define ABT_SIZE (1u << ABT_BITS)
#define CBT_BITS 13                       // cosmetic name buckets
#define CBT_SIZE (1u << CBT_BITS)

// skip counters (why a line was not loaded; reported once at load)
struct skips {
    int redirect, popup, regex, scriptlet, html, style, procedural, option;
};

// host hash set entry: lowercased host name hash -> its record (so a
// $badfilter can remove a host rule like any other)
struct hh_ent { uint64_t h; int32_t fi; };

static struct {
    int phase;                 // 0 empty, 1 parsing lines, 2 building, 3 ready
    const char* buf;
    uint32_t blen, pos;

    struct afilter* af; int naf, caf;
    struct ahide*  ah; int nah, cah;
    struct adom*   ad; int nad, cad;
    struct asc*    as; int nas, cas;
    int n_exc, n_host, n_domcos, n_sc;

    // lowercased-string bump chunks (stable pointers)
    struct achunk { struct achunk* next; uint32_t used, cap; char* p; }* cc;

    // host hash set: entries are struct hh_ent (file scope)
    struct hh_ent* hh; int hhcap, hhn;

    // network index
    uint32_t* boff;            // ABT_SIZE+1 bucket starts
    uint32_t* bitems;          // filter indices per bucket
    uint32_t* tokless; int ntokless;

    // cosmetic index
    uint32_t* coff;            // CBT_SIZE+1
    uint32_t* citems;
    uint32_t* unkey; int nunkey;
    int32_t*  seen;            // per-rule dedupe stamp for one doc build
    uint32_t  serial;
    struct { int rule, el; } htc[512];   // :has-text() candidates of one build
    int nhtc;

    struct skips sk;
    uint32_t bytes;
    uint32_t t_load;           // tick_count when ablock_load ran (kernel only)
} AB;

// ---- small helpers ------------------------------------------------------------------

static uint64_t now_cyc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t fnv32f(const char* s, int len) {   // case-folded (tokens, hosts)
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)w_lower((unsigned char)s[i])) * 16777619u;
    return h;
}

static uint32_t fnv32(const char* s, int len) {    // case-sensitive (cosmetic names)
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static uint64_t fnv64f(const char* s, int len) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < len; i++) { h ^= (uint8_t)w_lower((unsigned char)s[i]); h *= 1099511628211ull; }
    return h;
}

// bounded substring search (the list text is not NUL-terminated per line)
static int has_sub(const char* s, int len, const char* lit) {
    int nl = 0;
    while (lit[nl]) nl++;
    if (nl == 0 || nl > len) return 0;
    for (int i = 0; i + nl <= len; i++) {
        int k = 0;
        while (k < nl && s[i + k] == lit[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

// ABP separator: any char but letter, digit, '_', '-', '.', '%'.
static int sep_char(char c) {
    unsigned char u = (unsigned char)c;
    if ((u | 32) >= 'a' && (u | 32) <= 'z') return 0;
    if (u >= '0' && u <= '9') return 0;
    return !(u == '_' || u == '-' || u == '.' || u == '%');
}

// token runs: [0-9a-z%] after folding (both sides use the same rule)
static int tok_char(unsigned char c) {
    c = (unsigned char)w_lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '%';
}

static int is_hostchar(unsigned char c) {
    if ((c | 32) >= 'a' && (c | 32) <= 'z') return 1;
    return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

static void* grow(void* p, int* cap, int need, int esz) {
    if (need <= *cap) return p;
    int nc = *cap ? *cap * 2 : 1024;
    while (nc < need) nc *= 2;
    void* q = w_realloc(p, (uint32_t)nc * esz);
    if (q) { AB.bytes += (uint32_t)(nc - (*cap > 0 ? *cap : 0)) * esz; *cap = nc; }
    return q;
}

// bump-allocate a lowercased NUL-terminated copy of s[0..len). Selectors are
// capped at 4 KB and domain entries at 255 bytes, so 64 KB chunks suffice.
static char* lower_copy_a(const char* s, int len) {
    if (len <= 0) return 0;
    if (!AB.cc || AB.cc->used + (uint32_t)len + 1 > AB.cc->cap) {
        struct achunk* c = (struct achunk*)w_malloc(sizeof(struct achunk) + 65536);
        if (!c) return 0;
        c->p = (char*)(c + 1);
        c->cap = 65536;
        c->used = 0;
        c->next = AB.cc;
        AB.cc = c;
        AB.bytes += 65536 + (uint32_t)sizeof(struct achunk);
    }
    char* out = AB.cc->p + AB.cc->used;
    for (int i = 0; i < len; i++) out[i] = (char)w_lower((unsigned char)s[i]);
    out[len] = 0;
    AB.cc->used += (uint32_t)len + 1;
    return out;
}

// ---- host set -----------------------------------------------------------------------

static int hh_put(uint64_t h, int fi) {
    if ((AB.hhn + 1) * 10 >= AB.hhcap * 7) {          // load < 0.7
        int nc = AB.hhcap ? AB.hhcap * 2 : 1 << 13;
        struct hh_ent* t = (struct hh_ent*)w_calloc((uint32_t)nc, sizeof(struct hh_ent));
        if (!t) return 0;
        AB.bytes += (uint32_t)nc * sizeof(struct hh_ent);
        for (int i = 0; i < AB.hhcap; i++) {
            if (!AB.hh[i].h) continue;
            uint32_t j = (uint32_t)(AB.hh[i].h >> 33) & (uint32_t)(nc - 1);
            while (t[j].h) j = (j + 1) & (uint32_t)(nc - 1);
            t[j] = AB.hh[i];
        }
        w_free(AB.hh);
        AB.hh = t;
        AB.hhcap = nc;
    }
    uint32_t j = (uint32_t)(h >> 33) & (uint32_t)(AB.hhcap - 1);
    while (AB.hh[j].h) {
        if (AB.hh[j].h == h) return 1;                // first record wins (duplicate lines)
        j = (j + 1) & (uint32_t)(AB.hhcap - 1);
    }
    AB.hh[j].h = h;
    AB.hh[j].fi = fi;
    AB.hhn++;
    return 1;
}

// record index of a host rule, or -1
static int hh_has(uint64_t h) {
    if (!AB.hhcap) return -1;
    uint32_t j = (uint32_t)(h >> 33) & (uint32_t)(AB.hhcap - 1);
    while (AB.hh[j].h) {
        if (AB.hh[j].h == h) return AB.hh[j].fi;
        j = (j + 1) & (uint32_t)(AB.hhcap - 1);
    }
    return -1;
}

// ---- pattern matching ---------------------------------------------------------------

// Match p[0..plen) against u starting at ui (p has no anchors left; '*'
// wildcard, '^' separator-or-end, literals case-folded). Bounded backtracking.
static int match_at(const char* p, int plen, const char* u, int ulen, int ui, int enda) {
    int pi = 0, star = -1, st_ui = 0, steps = 0;
    for (;;) {
        if (++steps > 200000) return 0;
        if (pi >= plen) return !enda || ui == ulen;
        char pc = p[pi];
        if (pc == '*') { star = ++pi; st_ui = ui; continue; }
        int ok = 0, adv = 0;
        if (pc == '^') {
            if (ui >= ulen) { ok = 1; }                       // ^ also matches the end
            else if (sep_char(u[ui])) { ok = 1; adv = 1; }
        } else if (ui < ulen && w_lower((unsigned char)u[ui]) == w_lower((unsigned char)pc)) {
            ok = 1; adv = 1;
        }
        if (ok) { pi++; ui += adv; continue; }
        if (star < 0) return 0;
        st_ui++;
        if (st_ui > ulen) return 0;
        pi = star;
        ui = st_ui;
    }
}

static int pat_match(const struct afilter* f, const char* u, int ulen, int hs, int he) {
    // f->pat is stored with its anchor already stripped
    const char* p = f->pat;
    int plen = f->plen;
    if (f->anchor == AN_HOST) {
        if (plen <= 0) return 0;
        if (match_at(p, plen, u, ulen, hs, f->flags & AF_ENDA)) return 1;
        for (int i = hs + 1; i < he; i++)
            if (u[i - 1] == '.' && match_at(p, plen, u, ulen, i, f->flags & AF_ENDA)) return 1;
        return 0;
    }
    if (f->anchor == AN_START) {
        return plen > 0 && match_at(p, plen, u, ulen, 0, f->flags & AF_ENDA);
    }
    if (f->lit0) {
        for (int s = 0; s <= ulen; s++) {
            if (w_lower((unsigned char)u[s]) != f->lit0) continue;
            if (match_at(p, plen, u, ulen, s, f->flags & AF_ENDA)) return 1;
        }
        return 0;
    }
    for (int s = 0; s <= ulen; s++)
        if (match_at(p, plen, u, ulen, s, f->flags & AF_ENDA)) return 1;
    return 0;
}

// ---- option parsing -----------------------------------------------------------------

// Name index for the built-in redirect resources; "whatever.js" resolves to
// noopjs like uBlock's generic JS noops.
static int res_find(const char* s, int len) {
    for (int i = 0; i < ARES_N; i++)
        if ((int)strlen(ARES[i].name) == len && !memcmp(s, ARES[i].name, len)) return i;
    if (len > 3 && s[len - 3] == '.' && s[len - 2] == 'j' && s[len - 1] == 's') return 0;   // *.js -> noopjs
    return -1;
}

// One comma-separated option list (the part after the last '$'). Returns
// 1 = parsed (the pattern continues without the options), 0 = out of memory.
// Skippable options are counted and the line dropped via *drop.
static int parse_opts(const char* s, int len, struct afilter* f, int* drop) {
    uint16_t pos = 0, neg = 0;
    int have_pos = 0, types_given = 0;
    uint16_t flags = 0;
    uint32_t dombase = AB.nad;
    uint16_t ndom = 0;
    f->rres = RRES_NONE;
    int i = 0;
    while (i < len) {
        int j = i;
        while (j < len && s[j] != ',') j++;
        int tl = j - i;
        if (tl > 0) {
            const char* t = s + i;
            int negated = t[0] == '~';
            if (negated) { t++; tl--; }
            #define OPT_IS(lit) (tl == (int)sizeof(lit) - 1 && w_ieq(t, tl, lit))
            int kind = 0;                                  // 0 = type bit or special below
            if (OPT_IS("script")) kind = TB_SCRIPT;
            else if (OPT_IS("image")) kind = TB_IMG;
            else if (OPT_IS("stylesheet")) kind = TB_CSS;
            else if (OPT_IS("xmlhttprequest") || OPT_IS("xhr")) kind = TB_XHR;
            else if (OPT_IS("font")) kind = TB_FONT;
            else if (OPT_IS("subdocument") || OPT_IS("frame")) kind = TB_FRAME;
            else if (OPT_IS("document") || OPT_IS("main_frame")) kind = TB_MAIN;
            else if (OPT_IS("media")) kind = TB_MEDIA;
            else if (OPT_IS("other")) kind = TB_OTHER;
            else if (OPT_IS("all")) kind = TB_ALL;
            else if (OPT_IS("third-party") || OPT_IS("3p")) {
                if (negated) flags |= AF_FP; else flags |= AF_TP;
            }
            else if (OPT_IS("first-party") || OPT_IS("1p")) {
                if (negated) flags |= AF_TP; else flags |= AF_FP;
            }
            else if (OPT_IS("important")) flags |= AF_IMP;
            else if (OPT_IS("badfilter")) flags |= AF_BF;
            else if (OPT_IS("match-case")) { /* we match case-insensitively */ }
            else if (tl > 9 && w_ieq_prefix(t, tl, "redirect=")) {
                // serve a built-in resource instead of the request
                const char* rn = t + 9;
                int rl = tl - 9;
                int r = rl > 0 ? res_find(rn, rl) : -1;
                if (r < 0) { AB.sk.redirect++; *drop = 1; return 1; }
                flags |= AF_REDIR;
                f->rres = (uint8_t)r;
            }
            else if (tl > 7 && w_ieq_prefix(t, tl, "domain=")) { kind = -2; tl -= 7; t += 7; }
            else if (tl > 5 && w_ieq_prefix(t, tl, "from=")) { kind = -2; tl -= 5; t += 5; }
            else if (OPT_IS("csp") || (tl > 4 && w_ieq_prefix(t, tl, "csp=")) ||
                     (tl > 11 && w_ieq_prefix(t, tl, "removeparam=")) ||
                     (tl > 12 && w_ieq_prefix(t, tl, "permissions=")) ||
                     OPT_IS("uritransform") || OPT_IS("redirect-rule")) {
                AB.sk.redirect++;
                *drop = 1;
                return 1;
            }
            else if (OPT_IS("popup")) { AB.sk.popup++; *drop = 1; return 1; }
            else if (OPT_IS("ghide") || OPT_IS("generichide") || OPT_IS("ehide") ||
                     OPT_IS("elemhide") || OPT_IS("shide") || OPT_IS("specifichide") ||
                     OPT_IS("css-generic") || OPT_IS("css-specific")) {
                AB.sk.procedural++;
                *drop = 1;
                return 1;
            }
            else if (OPT_IS("method") || OPT_IS("dnstype") || OPT_IS("dnsrewrite") ||
                     OPT_IS("cname") || OPT_IS("ping") || OPT_IS("websocket") ||
                     OPT_IS("webrtc") || OPT_IS("object") || OPT_IS("object_subrequest") ||
                     OPT_IS("inline-script") || OPT_IS("inline-font")) {
                AB.sk.option++;
                *drop = 1;
                return 1;
            }
            else {
                AB.sk.option++;
                *drop = 1;
                return 1;
            }
            #undef OPT_IS
            if (kind == TB_ALL) {
                types_given = 1;
                if (!negated) { pos = TB_ALL; have_pos = 1; }
                else neg = TB_ALL;
            } else if (kind > 0) {
                types_given = 1;
                if (negated) neg |= (uint16_t)kind;
                else { pos |= (uint16_t)kind; have_pos = 1; }
            } else if (kind == -2) {                       // domain=/from= list (',' or '|')
                int k = 0;
                while (k <= tl) {
                    int e = k;
                    while (e < tl && t[e] != ',' && t[e] != '|') e++;
                    if (e > k) {
                        const char* de = t + k;
                        int dl = e - k;
                        int dneg = de[0] == '~';
                        if (dneg) { de++; dl--; }
                        int ok = dl > 0 && dl <= 255 && !(dl == 1 && de[0] == '*');
                        for (int z = 0; z < dl && ok; z++) ok = is_hostchar((unsigned char)de[z]);
                        if (!ok) { AB.sk.option++; *drop = 1; return 1; }
                        struct adom* dd = (struct adom*)grow(AB.ad, &AB.cad, AB.nad + 1, sizeof(struct adom));
                        if (!dd) return 0;
                        AB.ad = dd;
                        dd = &AB.ad[AB.nad];
                        dd->s = lower_copy_a(de, dl);
                        if (!dd->s) return 0;
                        dd->len = (uint16_t)dl;
                        dd->neg = (uint8_t)dneg;
                        AB.nad++;
                        ndom++;
                    }
                    k = e + 1;
                }
            }
        }
        i = j + 1;
    }
    if ((flags & AF_TP) && (flags & AF_FP)) { AB.sk.option++; *drop = 1; return 1; }
    // no type option at all: the filter stays untyped (matches everything
    // but a top-level document — and compares equal to its $badfilter)
    f->types = types_given ? (have_pos ? (uint16_t)(pos & ~neg) : (uint16_t)(TB_ALL & ~neg)) : 0;
    f->flags |= flags;
    f->ndom = ndom;
    f->dombase = dombase;
    return 1;
}

// ---- tokens -------------------------------------------------------------------------

static const char* const stop_tok[] = {
    "com", "net", "org", "www", "http", "https", "html", "htm", "php", "js",
    "css", "jpg", "jpeg", "png", "gif", "svg", "webp", "index", 0
};

// Pick the anchor token of the pattern: its longest [a-z0-9%]+ run that is
// not stopped, not short, and not adjacent to a '*' (adjacency would break
// the "the URL must contain this run" guarantee). Returns the hash or 0.
static uint32_t pick_token(const char* p, int plen) {
    int best = -1, bestlen = 0;
    for (int i = 0; i < plen; ) {
        if (!tok_char((unsigned char)p[i])) { i++; continue; }
        int j = i;
        while (j < plen && tok_char((unsigned char)p[j])) j++;
        int len = j - i;
        int adjacent_star = (i > 0 && p[i - 1] == '*') || (j < plen && p[j] == '*');
        if (len >= 3 && len > bestlen && !adjacent_star) {
            int stopped = 0;
            for (int k = 0; stop_tok[k]; k++) {
                if (len == (int)strlen(stop_tok[k]) && w_ieq(p + i, len, stop_tok[k])) { stopped = 1; break; }
            }
            if (!stopped) { best = i; bestlen = len; }
        }
        i = j;
    }
    if (best < 0) return 0;
    return fnv32f(p + best, bestlen) | 1u;   // never 0 for a real token
}

// ---- line parsing -------------------------------------------------------------------

static int cos_marker(const char* s, int len, int* mlen) {
    for (int i = 0; i < len - 2; i++) {
        if (s[i] != '#') continue;
        if (s[i + 1] == '@' && s[i + 2] == '#') { *mlen = 3; return i; }
        if (s[i + 1] == '$' && s[i + 2] == '#') { *mlen = 3; return i; }
        if (s[i + 1] == '?' && s[i + 2] == '#') { *mlen = 3; return i; }
        if (s[i + 1] == '#') { *mlen = 2; return i; }
    }
    return -1;
}

// Procedural / uBlock-only selector extensions we do not implement.
static int sel_procedural(const char* s, int len) {
    static const char* const marks[] = {
        ":has-text(", ":matches-path(", ":matches-css", ":matches-attr", ":style(",
        ":remove(", ":upward(", ":xpath(", ":nth-ancestor(", ":min-text-length(",
        ":others(", ":watch-attr(", ":if(", ":if-not(", "-abp-has(", "-abp-contains(",
        "-abp-properties(", 0
    };
    for (int k = 0; marks[k]; k++)
        if (has_sub(s, len, marks[k])) return 1;
    return 0;
}

// Name hash of the rightmost compound's key: the first class/id/tag/attribute
// name in it (the argument of :has()/:is()/:where()/:not() is searched when
// the compound starts with the pseudo). 0 = nothing to key on (the rule goes
// on the always-consider list).
static uint32_t sel_key(const char* s, int len) {
    // rightmost compound: after the last depth-0 combinator (incl. whitespace)
    int depth = 0, comp = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth) depth--; }
        else if (depth == 0 && (c == '>' || c == '+' || c == '~' || w_isspace((unsigned char)c))) comp = i + 1;
    }
    if (comp >= len) return 0;
    const char* c = s + comp;
    int cl = len - comp;
    int i = 0;
    while (i < cl && w_isspace((unsigned char)c[i])) i++;
    if (i >= cl) return 0;
    int extract_from = i;
    if (c[i] == ':') {
        int n = 0;
        static const char* const fn[] = { ":has(", ":is(", ":where(", ":not(", 0 };
        for (int k = 0; fn[k]; k++) {
            int fl = (int)strlen(fn[k]);
            if (i + fl <= cl && w_ieq_prefix(c + i, cl - i, fn[k])) { n = fl; break; }
        }
        if (!n) return 0;
        extract_from = i + n;
    }
    for (int k = extract_from; k < cl; k++) {
        char ch = c[k];
        if (ch == '#' || ch == '.') {
            int j = k + 1, nl = 0;
            char nm[128];
            while (j < cl && nl < 127) {
                char d = c[j];
                if (d == '\\' && j + 1 < cl) { nm[nl++] = c[j + 1]; j += 2; continue; }
                int idc = w_isalnum((unsigned char)d) || d == '-' || d == '_' || d == '\\' || (unsigned char)d >= 0x80;
                if (!idc) break;
                nm[nl++] = d;
                j++;
            }
            return nl ? fnv32(nm, nl) | 1u : 0;
        }
        if (ch == '[') {
            int j = k + 1, nl = 0;
            char nm[128];
            while (j < cl && (c[j] == ' ' || c[j] == '\t')) j++;
            while (j < cl && nl < 127) {
                char d = c[j];
                int idc = w_isalnum((unsigned char)d) || d == '-' || d == '_' || (unsigned char)d >= 0x80;
                if (!idc) break;
                nm[nl++] = d;
                j++;
            }
            return nl ? fnv32(nm, nl) | 1u : 0;
        }
        if (ch == ':') {
            int n = 0;
            static const char* const fn[] = { ":has(", ":is(", ":where(", ":not(", 0 };
            for (int q = 0; fn[q]; q++) {
                int fl = (int)strlen(fn[q]);
                if (k + fl <= cl && w_ieq_prefix(c + k, cl - k, fn[q])) { n = fl; break; }
            }
            if (!n) return 0;
            k += n;                       // resume inside the parens
            continue;
        }
        if (w_isalnum((unsigned char)ch)) {
            int j = k, nl = 0;
            char nm[128];
            while (j < cl && nl < 127) {
                char d = c[j];
                int idc = w_isalnum((unsigned char)d) || d == '-' || d == '_' || (unsigned char)d >= 0x80;
                if (!idc) break;
                nm[nl++] = d;
                j++;
            }
            if (j >= cl) return fnv32(nm, nl) | 1u;   // bare tag compound
            k = j - 1;                                 // "div.foo": keep scanning
            continue;
        }
        return 0;
    }
    return 0;
}

// ##+js(name, arg, ...): the domain list sits left of the '##', the rest is
// the scriptlet call. Args split on ',' and trimmed, exactly like uBlock's
// parser (no escaped commas).
static void add_scriptlet(const char* doms, int dlen, const char* call, int clen) {
    if (clen <= 0 || call[clen - 1] != ')') { AB.sk.scriptlet++; return; }
    clen--;                                   // drop the closing ')'
    struct asc* sc = (struct asc*)grow(AB.as, &AB.cas, AB.nas + 1, sizeof(struct asc));
    if (!sc) return;
    AB.as = sc;
    sc = &AB.as[AB.nas];
    memset(sc, 0, sizeof *sc);
    uint32_t base = AB.nad;
    uint16_t nd = 0;
    int i = 0;
    while (i < dlen) {
        int j = i;
        while (j < dlen && doms[j] != ',') j++;
        if (j > i) {
            const char* de = doms + i;
            int dl = j - i;
            int neg = de[0] == '~';
            if (neg) { de++; dl--; }
            if (!(dl == 1 && de[0] == '*' && !neg)) {
                int ok = dl > 0 && dl <= 255;
                for (int z = 0; z < dl && ok; z++) ok = is_hostchar((unsigned char)de[z]);
                if (!ok) { AB.sk.option++; return; }
                struct adom* dd = (struct adom*)grow(AB.ad, &AB.cad, AB.nad + 1, sizeof(struct adom));
                if (!dd) return;
                AB.ad = dd;
                dd = &AB.ad[AB.nad];
                dd->s = lower_copy_a(de, dl);
                if (!dd->s) return;
                dd->len = (uint16_t)dl;
                dd->neg = (uint8_t)neg;
                AB.nad++;
                nd++;
            }
        }
        i = j + 1;
    }
    // args
    int arg = 0;
    i = 0;
    while (i <= clen) {
        int j = i;
        while (j < clen && call[j] != ',') j++;
        const char* a = call + i;
        int al = j - i;
        while (al > 0 && (a[0] == ' ' || a[0] == '\t')) { a++; al--; }
        while (al > 0 && (a[al - 1] == ' ' || a[al - 1] == '\t')) al--;
        if (al > 0) {
            if (arg >= SC_MAX_ARGS + 1) { AB.sk.scriptlet++; return; }   // too many tokens
            if (arg == 0) {
                if (al > 64) { AB.sk.scriptlet++; return; }
                sc->name = a;
                sc->nlen = (uint16_t)al;
            } else {
                if (al > 512) { AB.sk.scriptlet++; return; }
                sc->args[arg - 1] = a;
                sc->alen[arg - 1] = (uint8_t)al;
                sc->nargs = (uint8_t)arg;
            }
            arg++;
        }
        i = j + 1;
    }
    if (!sc->nlen) { AB.sk.scriptlet++; return; }
    sc->ndom = (uint8_t)nd;
    sc->dombase = base;
    AB.nas++;
    AB.n_sc++;
}

static void add_hide(const char* doms, int dlen, const char* sel, int slen, int unhide) {
    if (slen <= 0 || slen > 4096) { AB.sk.option++; return; }
    for (int i = 0; i < slen; i++)
        if (sel[i] == '{' || sel[i] == '}' || sel[i] == ';' || sel[i] == '<') { AB.sk.option++; return; }
    // :has-text(needle): the one procedural operator we implement; it must
    // be the last thing in the selector, and the needle must not be a regex
    const char* ht = 0;
    int htl = 0;
    for (int i = 0; i + 10 <= slen; i++) {
        if (sel[i] != ':' || memcmp(sel + i, ":has-text(", 10)) continue;
        if (sel[slen - 1] != ')') { AB.sk.procedural++; return; }      // not trailing
        const char* a = sel + i + 10;
        int al = slen - i - 11;
        if (al <= 0 || al > 512) { AB.sk.procedural++; return; }
        if (a[0] == '/' && a[al - 1] == '/' && al >= 2) { AB.sk.procedural++; return; }  // regex needle: not yet
        for (int z = 0; z + 10 <= al; z++)                                               // exactly one, trailing
            if (a[z] == ':' && !memcmp(a + z, ":has-text(", 10)) { AB.sk.procedural++; return; }
        ht = a;
        htl = al;
        slen = i;                                                      // base selector
        while (slen > 0 && w_isspace((unsigned char)sel[slen - 1])) slen--;
        break;
    }
    if (slen <= 0) { AB.sk.procedural++; return; }
    if (sel_procedural(sel, slen)) { AB.sk.procedural++; return; }
    struct ahide* h = (struct ahide*)grow(AB.ah, &AB.cah, AB.nah + 1, sizeof(struct ahide));
    if (!h) return;
    AB.ah = h;
    h = &AB.ah[AB.nah];
    uint32_t base = AB.nad;
    uint16_t nd = 0;
    int i = 0;
    while (i < dlen) {
        int j = i;
        while (j < dlen && doms[j] != ',') j++;
        if (j > i) {
            const char* de = doms + i;
            int dl = j - i;
            int neg = de[0] == '~';
            if (neg) { de++; dl--; }
            if (!(dl == 1 && de[0] == '*' && !neg)) {      // '*' alone = generic
                int ok = dl > 0 && dl <= 255;
                for (int z = 0; z < dl && ok; z++) ok = is_hostchar((unsigned char)de[z]);
                if (!ok) { AB.sk.option++; return; }       // entity wildcards drop the rule
                struct adom* dd = (struct adom*)grow(AB.ad, &AB.cad, AB.nad + 1, sizeof(struct adom));
                if (!dd) return;
                AB.ad = dd;
                dd = &AB.ad[AB.nad];
                dd->s = lower_copy_a(de, dl);
                if (!dd->s) return;
                dd->len = (uint16_t)dl;
                dd->neg = (uint8_t)neg;
                AB.nad++;
                nd++;
            }
        }
        i = j + 1;
    }
    h->sel = sel;
    h->slen = (uint16_t)slen;
    h->unhide = (uint8_t)unhide;
    h->ndom = nd;
    h->dombase = base;
    // every rule is name-keyed so :has-text() rules can find their elements;
    // host rules only apply once the host scan marked them
    h->key = sel_key(sel, slen);
    h->ht = ht;
    h->htl = (uint16_t)htl;
    AB.nah++;
    if (nd) AB.n_domcos++;
    if (h->key == 0 && !unhide) AB.nunkey++;    // host rules land here too; the doc walk re-filters by host
}

static void parse_line(const char* s, int len) {
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len <= 0) return;
    if (s[0] == '!' || s[0] == '[') return;     // comments, list headers, !# directives

    // cosmetic?
    int mlen = 0;
    int mi = cos_marker(s, len, &mlen);
    if (mi >= 0) {
        const char* sel = s + mi + mlen;
        int slen = len - mi - mlen;
        if (s[mi + 1] == '$') { AB.sk.style++; return; }               // #$# style injection
        if (slen > 0 && sel[0] == '^') { AB.sk.html++; return; }       // ##^ HTML filter
        if (slen >= 5 && sel[0] == '+' && sel[1] == 'j' && sel[2] == 's' && sel[3] == '(') {
            add_scriptlet(s, mi, sel + 4, slen - 4);                   // ##+js(name, args...)
            return;
        }
        add_hide(s, mi, sel, slen, s[mi + 1] == '@');
        return;
    }

    // network filter
    int exc = 0;
    const char* p = s;
    int plen = len;
    if (plen >= 2 && p[0] == '@' && p[1] == '@') { exc = 1; p += 2; plen -= 2; }

    // options after the last '$' — only if the suffix reads as an option list
    struct afilter f;
    memset(&f, 0, sizeof f);
    f.rres = RRES_NONE;
    for (int i = plen - 1; i > 0; i--) {
        if (p[i] != '$') continue;
        int olen = plen - i - 1;
        if (olen <= 0) continue;                        // a bare trailing '$'
        int looks = 1;
        for (int k = 0; k < olen; k++) {
            char c = p[i + 1 + k];
            if (c == ',' || c == '=' || c == '|' || c == '~' || c == '-' || c == '_' || c == '.' ||
                w_isalnum((unsigned char)c)) continue;
            looks = 0;
            break;
        }
        if (!looks) continue;
        int drop = 0;
        struct afilter t;
        memset(&t, 0, sizeof t);
        if (!parse_opts(p + i + 1, olen, &t, &drop)) return;           // OOM
        if (drop) return;
        f = t;
        plen = i;
        break;
    }
    if (exc) f.flags |= AF_EXC;

    // regex filters are matched as their literal inner text
    if (plen >= 3 && p[0] == '/' && p[plen - 1] == '/') { p++; plen -= 2; }

    // anchors
    if (plen >= 2 && p[0] == '|' && p[1] == '|') f.anchor = AN_HOST;
    else if (plen >= 1 && p[0] == '|') f.anchor = AN_START;
    if (plen >= 1 && p[plen - 1] == '|') { f.flags |= AF_ENDA; plen--; }
    if (plen <= 0) return;
    if (f.anchor == AN_HOST) { p += 2; plen -= 2; }
    else if (f.anchor == AN_START) { p++; plen--; }
    if (plen <= 0) return;
    // a mid-pattern '|' is not a thing
    for (int i = 0; i < plen; i++)
        if (p[i] == '|') { AB.sk.option++; return; }

    f.pat = p;
    f.plen = (uint16_t)plen;

    // plain-host line ("peter lowe's" style) or ||host^ with no options:
    // these go into the O(1) host set as real records, so $badfilter can
    // still remove them.
    if (!f.types && !(f.flags & (AF_TP | AF_FP | AF_IMP | AF_BF)) && !f.ndom) {
        if (f.anchor == AN_HOST && !(f.flags & AF_ENDA) && !exc && plen >= 5 && p[plen - 1] == '^') {
            int ok = 1;
            for (int i = 0; i < plen - 1 && ok; i++) ok = is_hostchar((unsigned char)p[i]);
            if (ok) {
                f.flags |= AF_HOST;
                f.thash = 0;
                struct afilter* slot = (struct afilter*)grow(AB.af, &AB.caf, AB.naf + 1, sizeof(struct afilter));
                if (!slot) return;
                AB.af = slot;
                AB.af[AB.naf] = f;
                hh_put(fnv64f(p, plen - 1), AB.naf);
                AB.naf++;
                AB.n_host++;
                return;
            }
        }
        if (f.anchor == AN_NONE && !exc && plen >= 4 && plen <= 255) {
            int ok = 0;
            for (int i = 0; i < plen; i++) {
                if (!is_hostchar((unsigned char)p[i])) { ok = 0; break; }
                if (p[i] == '.') ok = 1;
            }
            if (ok) {
                f.flags |= AF_HOST;
                f.thash = 0;
                struct afilter* slot = (struct afilter*)grow(AB.af, &AB.caf, AB.naf + 1, sizeof(struct afilter));
                if (!slot) return;
                AB.af = slot;
                AB.af[AB.naf] = f;
                hh_put(fnv64f(p, plen), AB.naf);
                AB.naf++;
                AB.n_host++;
                return;
            }
        }
    }

    // first literal char for the quick reject (f.pat has its anchor stripped)
    {
        const char* q = f.pat;
        int ql = f.plen;
        while (ql > 0 && (*q == '*' || *q == '^')) { q++; ql--; }
        if (ql > 0 && *q != '*' && *q != '^' && *q != '|') f.lit0 = (uint8_t)w_lower((unsigned char)*q);
    }

    f.thash = pick_token(f.pat, f.plen);
    struct afilter* slot = (struct afilter*)grow(AB.af, &AB.caf, AB.naf + 1, sizeof(struct afilter));
    if (!slot) return;
    AB.af = slot;
    AB.af[AB.naf++] = f;
    if (exc) AB.n_exc++;
}

// ---- parse slicer -------------------------------------------------------------------

static int parse_lines(uint32_t budget) {
    uint64_t t0 = now_cyc();
    int n = 0;
    while (AB.pos < AB.blen) {
        const char* line = AB.buf + AB.pos;
        const char* end = line;
        while (end < AB.buf + AB.blen && *end != '\n') end++;
        parse_line(line, (int)(end - line));
        AB.pos = (uint32_t)(end - AB.buf) + 1;
        if (++n >= 128) {
            n = 0;
            if (budget && now_cyc() - t0 > budget) return 1;
        }
    }
    return 0;
}

static int bf_same(const struct afilter* a, const struct afilter* b) {
    if (a->plen != b->plen || memcmp(a->pat, b->pat, a->plen)) return 0;
    if (a->types != b->types || a->anchor != b->anchor) return 0;
    if ((a->flags & ~(AF_BF | AF_HOST)) != (b->flags & ~(AF_BF | AF_HOST))) return 0;
    if (a->ndom != b->ndom) return 0;
    for (int i = 0; i < a->ndom; i++) {
        const struct adom* x = &AB.ad[a->dombase + i];
        const struct adom* y = &AB.ad[b->dombase + i];
        if (x->len != y->len || x->neg != y->neg || memcmp(x->s, y->s, x->len)) return 0;
    }
    return 1;
}

static void build_indexes(void) {
    // $badfilter: drop the filters it names
    for (int i = 0; i < AB.naf; i++) {
        if (!(AB.af[i].flags & AF_BF)) continue;
        for (int j = 0; j < AB.naf; j++) {
            if (j == i || (AB.af[j].flags & (AF_BF | AF_DEAD))) continue;
            if (bf_same(&AB.af[i], &AB.af[j])) AB.af[j].flags |= AF_DEAD;
        }
    }
    // token buckets (two-pass counting fill; dead and tokenless filters excluded)
    AB.boff = (uint32_t*)w_calloc(ABT_SIZE + 1, sizeof(uint32_t));
    if (!AB.boff) return;
    AB.bytes += (ABT_SIZE + 1) * sizeof(uint32_t);
    int nitems = 0, ntokless = 0;
    for (int i = 0; i < AB.naf; i++) {
        const struct afilter* f = &AB.af[i];
        if (f->flags & (AF_DEAD | AF_BF | AF_HOST)) continue;   // host rules live in the host set
        if (!f->thash) ntokless++;
        else { AB.boff[(f->thash & (ABT_SIZE - 1)) + 1]++; nitems++; }
    }
    for (uint32_t k = 1; k <= ABT_SIZE; k++) AB.boff[k] += AB.boff[k - 1];
    AB.bitems = (uint32_t*)w_malloc((uint32_t)(nitems ? nitems : 1) * sizeof(uint32_t));
    AB.tokless = (uint32_t*)w_malloc((uint32_t)(ntokless ? ntokless : 1) * sizeof(uint32_t));
    if (!AB.bitems || !AB.tokless) return;
    AB.bytes += (uint32_t)(nitems + ntokless + 2) * sizeof(uint32_t);
    {   // fill per bucket: boff is the bucket's start, so place each filter at
        // its bucket's cursor (a copy of the prefix sums that advances)
        uint32_t* cur = (uint32_t*)w_malloc((ABT_SIZE + 1) * sizeof(uint32_t));
        if (!cur) return;
        memcpy(cur, AB.boff, (ABT_SIZE + 1) * sizeof(uint32_t));
        int tk2 = 0;
        for (int i = 0; i < AB.naf; i++) {
            const struct afilter* f = &AB.af[i];
            if (f->flags & (AF_DEAD | AF_BF | AF_HOST)) continue;
            if (!f->thash) AB.tokless[tk2++] = (uint32_t)i;
            else AB.bitems[cur[f->thash & (ABT_SIZE - 1)]++] = (uint32_t)i;
        }
        w_free(cur);
    }
    AB.ntokless = ntokless;
    // cosmetic name buckets
    AB.coff = (uint32_t*)w_calloc(CBT_SIZE + 1, sizeof(uint32_t));
    if (!AB.coff) return;
    AB.bytes += (CBT_SIZE + 1) * sizeof(uint32_t);
    int nc = 0;
    for (int i = 0; i < AB.nah; i++) {
        if (AB.ah[i].key) { AB.coff[(AB.ah[i].key & (CBT_SIZE - 1)) + 1]++; nc++; }
    }
    for (uint32_t k = 1; k <= CBT_SIZE; k++) AB.coff[k] += AB.coff[k - 1];
    AB.citems = (uint32_t*)w_malloc((uint32_t)(nc ? nc : 1) * sizeof(uint32_t));
    AB.unkey = (uint32_t*)w_malloc((uint32_t)(AB.nunkey ? AB.nunkey : 1) * sizeof(uint32_t));
    if (!AB.citems || !AB.unkey) return;
    AB.bytes += (uint32_t)(nc + AB.nunkey + 2) * sizeof(uint32_t);
    AB.seen = (int32_t*)w_calloc((uint32_t)(AB.nah ? AB.nah : 1), sizeof(int32_t));
    if (!AB.seen) return;
    AB.bytes += (uint32_t)(AB.nah + 1) * sizeof(int32_t);
    {   // per-bucket fill (see the network index above)
        uint32_t* cur = (uint32_t*)w_malloc((CBT_SIZE + 1) * sizeof(uint32_t));
        if (!cur) return;
        memcpy(cur, AB.coff, (CBT_SIZE + 1) * sizeof(uint32_t));
        int uk = 0;
        for (int i = 0; i < AB.nah; i++) {
            if (AB.ah[i].key) AB.citems[cur[AB.ah[i].key & (CBT_SIZE - 1)]++] = (uint32_t)i;
            else if (!AB.ah[i].unhide) AB.unkey[uk++] = (uint32_t)i;
        }
        w_free(cur);
    }
    AB.phase = 3;
    int skips = AB.sk.redirect + AB.sk.popup + AB.sk.regex + AB.sk.scriptlet + AB.sk.html +
                AB.sk.style + AB.sk.procedural + AB.sk.option;
#if defined(KERNEL) && KERNEL
    w_log("[ablk] %d network filters (%d exceptions, %d host-only, %d tokenless), %d cosmetic rules (%d host-keyed), %d scriptlets, %d skipped (redirect %d, popup %d, regex %d, +js %d, html %d, style %d, procedural %d, option %d) [%u KB, %u ms]\n",
          AB.naf, AB.n_exc, AB.n_host, ntokless, AB.nah, AB.n_domcos, AB.n_sc, skips,
          AB.sk.redirect, AB.sk.popup, AB.sk.regex, AB.sk.scriptlet, AB.sk.html, AB.sk.style,
          AB.sk.procedural, AB.sk.option, AB.bytes >> 10,
          (unsigned)((tick_count - AB.t_load) * 10u));
#else
    w_log("[ablk] %d network filters (%d exceptions, %d host-only, %d tokenless), %d cosmetic rules (%d host-keyed), %d scriptlets, %d skipped, %u KB\n",
          AB.naf, AB.n_exc, AB.n_host, ntokless, AB.nah, AB.n_domcos, AB.n_sc, skips, AB.bytes >> 10);
#endif
}

int ablock_parse_step(uint32_t budget) {
    if (AB.phase == 1) {
        if (parse_lines(budget)) return 1;
        AB.phase = 2;
        return 1;
    }
    if (AB.phase == 2) {
        build_indexes();
        return 0;
    }
    return 0;
}

static void drain(void) {
    if (AB.phase != 1 && AB.phase != 2) return;
    if (AB.blen) w_log("[ablk] finishing the list parse on first use\n");
    while (ablock_parse_step(0)) {}
}

int ablock_ready(void) {
    return AB.phase == 3;
}

void ablock_reset(void) {
    w_free(AB.af); w_free(AB.ah); w_free(AB.ad); w_free(AB.as);
    w_free(AB.hh);
    w_free(AB.boff); w_free(AB.bitems); w_free(AB.tokless);
    w_free(AB.coff); w_free(AB.citems); w_free(AB.unkey); w_free(AB.seen);
    struct achunk* c = AB.cc;
    while (c) {
        struct achunk* n = c->next;
        w_free(c);
        c = n;
    }
    memset(&AB, 0, sizeof AB);
}

int ablock_load(const char* buf, uint32_t len) {
    ablock_reset();
    AB.buf = buf;
    AB.blen = len;
    AB.pos = 0;
    AB.phase = 1;
#if defined(KERNEL) && KERNEL
    AB.t_load = tick_count;
#endif
    return 0;
}

#if defined(KERNEL) && KERNEL
void ablock_init(void) {
    for (int i = 0; i < 8; i++) {
        uint32_t len;
        const char* cmd;
        const uint8_t* m = memory_boot_module(i, &len, &cmd);
        if (!m) break;
        if (cmd && cmd[0] && !strcmp(cmd, "ablock")) {
            w_log("[ablk] filter module %u: %u bytes\n", i, len);
            ablock_load((const char*)m, len);
            return;
        }
    }
    w_log("[ablk] NO FILTER MODULE — ad lists missing from this image\n");
    AB.phase = 3;    // ready, empty: every request passes
}
#endif

// ---- the check ----------------------------------------------------------------------

static const char* site_tail2(const char* host, int len, int* outlen) {
    int last = -1, prev = -1;
    int digits = 1;
    for (int i = 0; i < len; i++) {
        char c = host[i];
        if (c == '.') { prev = last; last = i; }
        if (!(c >= '0' && c <= '9') && c != '.') digits = 0;
    }
    if (digits || prev < 0) { *outlen = len; return host; }
    *outlen = len - prev - 1;
    return host + prev + 1;
}

static int host_suffix(const char* host, int hlen, const char* e, int elen) {
    if (elen <= 0 || elen > hlen) return 0;
    if (elen == hlen) return !memcmp(host, e, elen);
    return host[hlen - elen - 1] == '.' && !memcmp(host + hlen - elen, e, elen);
}

static int dom_match(uint32_t base, uint16_t n, const char* host, int hlen) {
    int pos = 0, has_pos = 0;
    for (int i = 0; i < n; i++) {
        const struct adom* d = &AB.ad[base + i];
        int m = host_suffix(host, hlen, d->s, d->len);
        if (d->neg) {
            if (m) return 0;
        } else {
            has_pos = 1;
            if (m) pos = 1;
        }
    }
    return has_pos ? pos : 1;                        // negatives only: everywhere but those
}

// "http://host/path" -> host span [hs,he) of the lowercased URL (port included)
static void host_span(const char* u, int ulen, int* hs, int* he) {
    *hs = *he = 0;
    for (int i = 0; i + 2 < ulen; i++) {
        if (u[i] == ':' && u[i + 1] == '/' && u[i + 2] == '/') {
            int s = i + 3, e = s;
            while (e < ulen && u[e] != '/' && u[e] != '?' && u[e] != '#') e++;
            *hs = s;
            *he = e;
            return;
        }
    }
}

static int kind_bit(int kind) {
    switch (kind) {
    case ABK_MAIN:   return TB_MAIN;
    case ABK_FRAME:  return TB_FRAME;
    case ABK_CSS:    return TB_CSS;
    case ABK_IMG:    return TB_IMG;
    case ABK_SCRIPT: return TB_SCRIPT;
    case ABK_FONT:   return TB_FONT;
    default:         return TB_XHR;
    }
}

const char* ablock_kind_name(int kind) {
    switch (kind) {
    case ABK_MAIN:   return "DOC";
    case ABK_FRAME:  return "FRAME";
    case ABK_CSS:    return "CSS";
    case ABK_IMG:    return "IMG";
    case ABK_SCRIPT: return "JS";
    case ABK_FONT:   return "FONT";
    default:         return "XHR";
    }
}

static int eval_filter(const struct afilter* f, int kb, int three_p,
                       const char* phost, int phlen,
                       const char* u, int ulen, int hs, int he) {
    if (f->flags & (AF_DEAD | AF_BF)) return 0;
    if (f->types == 0) {
        if (kb == TB_MAIN) return 0;                 // untyped never blocks a navigation
    } else if (!(f->types & (uint16_t)kb)) return 0;
    if ((f->flags & AF_TP) && !three_p) return 0;
    if ((f->flags & AF_FP) && three_p) return 0;
    if (f->ndom && !dom_match(f->dombase, f->ndom, phost, phlen)) return 0;
    if (!pat_match(f, u, ulen, hs, he)) return 0;
    return f->flags & AF_EXC ? 2 : 1;
}

// Shared verdict scan; *redir receives a built-in resource index when a
// matching block carries $redirect= (never for a top-level navigation).
static int allows_int(const char* url, const char* top, int kind, int* redir) {
    if (AB.phase == 0 || !url) return 1;             // engine never loaded
    drain();
    if (!AB.naf && !AB.nah) return 1;                // empty engine (no module)
    if (!AB.boff || !AB.coff) return 1;              // the build ran out of memory: fail open
    if (redir) *redir = -1;

    static char lurl[AB_URL_MAX];
    int ulen = 0;
    for (; url[ulen] && ulen < AB_URL_MAX - 1; ulen++) lurl[ulen] = (char)w_lower((unsigned char)url[ulen]);
    lurl[ulen] = 0;
    int hs, he;
    host_span(lurl, ulen, &hs, &he);

    // page host + party
    char ph[256];
    int phlen = 0;
    if (top && top[0]) {
        static char ltop[AB_URL_MAX];
        int tl = 0;
        for (; top[tl] && tl < AB_URL_MAX - 1; tl++) ltop[tl] = (char)w_lower((unsigned char)top[tl]);
        ltop[tl] = 0;
        int ts, te;
        host_span(ltop, tl, &ts, &te);
        if (te > ts && te - ts < (int)sizeof ph) {
            memcpy(ph, ltop + ts, te - ts);
            phlen = te - ts;
        }
    }
    int three_p = 0;
    if (phlen) {
        int al, bl;
        const char* ta = site_tail2(lurl + hs, he - hs, &al);
        const char* tb = site_tail2(ph, phlen, &bl);
        three_p = !(al == bl && !memcmp(ta, tb, al));
    }

    int kb = kind_bit(kind);

    // host set: untyped ||host^ blocks (never for a top-level navigation).
    // No early return: an exception in the token index can still rescue it.
    int blocked = 0, allowed = 0;
    if (kind != ABK_MAIN) {
        for (int i = hs; i < he; i++) {
            if (i > hs && lurl[i - 1] != '.') continue;
            int fi = hh_has(fnv64f(lurl + i, he - i));
            if (fi >= 0 && !(AB.af[fi].flags & AF_DEAD)) { blocked = 1; break; }
        }
    }

    // token buckets
    int ntok = 0;
    for (int i = 0; i < ulen && ntok < 64; ) {
        if (!tok_char((unsigned char)lurl[i])) { i++; continue; }
        int j = i;
        while (j < ulen && tok_char((unsigned char)lurl[j])) j++;
        int len = j - i;
        i = j;
        if (len < 3 || ++ntok > 64) continue;
        uint32_t h = fnv32f(lurl + i - len, len) | 1u;   // same |1 the build side carries
        uint32_t b = h & (ABT_SIZE - 1);
        for (uint32_t k = AB.boff[b]; k < AB.boff[b + 1]; k++) {
            const struct afilter* f = &AB.af[AB.bitems[k]];
            int v = eval_filter(f, kb, three_p, ph, phlen, lurl, ulen, hs, he);
            if (!v) continue;
            if (v == 2) allowed = 1;
            else if (f->flags & AF_IMP) return 0;
            else {
                blocked = 1;
                if (redir && (f->flags & AF_REDIR) && f->rres != RRES_NONE && kind != ABK_MAIN) *redir = f->rres;
            }
        }
    }
    // tokenless (rare)
    for (int k = 0; k < AB.ntokless; k++) {
        const struct afilter* f = &AB.af[AB.tokless[k]];
        int v = eval_filter(f, kb, three_p, ph, phlen, lurl, ulen, hs, he);
        if (!v) continue;
        if (v == 2) allowed = 1;
        else if (f->flags & AF_IMP) return 0;
        else {
            blocked = 1;
            if (redir && (f->flags & AF_REDIR) && f->rres != RRES_NONE && kind != ABK_MAIN) *redir = f->rres;
        }
    }

    if (allowed) return 1;
    return blocked ? 0 : 1;
}

int ablock_allows(const char* url, const char* top, int kind) {
    return allows_int(url, top, kind, 0);
}

int ablock_allows2(const char* url, const char* top, int kind, int* redirect) {
    return allows_int(url, top, kind, redirect);
}

void ablock_resource(int idx, const char** bytes, int* len, const char** ctype) {
    if (idx < 0 || idx >= ARES_N) { if (bytes) *bytes = ""; if (len) *len = 0; if (ctype) *ctype = "text/plain"; return; }
    if (bytes) *bytes = ARES[idx].bytes;
    if (len) *len = (int)ARES[idx].len;
    if (ctype) *ctype = ARES[idx].ctype;
}

// ---- cosmetic -----------------------------------------------------------------------

// Mark a rule for this build (dedupe by stamp). Returns 1 if newly marked.
static int cos_mark(int ri) {
    if (AB.seen[ri] == (int32_t)AB.serial) return 0;
    AB.seen[ri] = (int32_t)AB.serial;
    return 1;
}

static void cos_lookup_bucket(const char* name, int nl, int el, uint32_t* unhides, int* nunh) {
    if (nl <= 0) return;
    uint32_t h = fnv32(name, nl) | 1u;               // same |1 sel_key() stores
    uint32_t b = h & (CBT_SIZE - 1);
    for (uint32_t k = AB.coff[b]; k < AB.coff[b + 1]; k++) {
        int ri = (int)AB.citems[k];
        if (AB.ah[ri].key != h) continue;
        if (AB.ah[ri].ndom && AB.seen[ri] != (int32_t)AB.serial)
            continue;                                // host rule for a different host
        if (AB.ah[ri].unhide) {
            if (*nunh < 128) unhides[(*nunh)++] = (uint32_t)ri;
        } else if (AB.ah[ri].ht) {
            // :has-text(): hiding is per element, decided after the walk
            if (!AB.ah[ri].ndom) cos_mark(ri);       // generic rules join via this hit
            if (AB.nhtc < 512) {
                AB.htc[AB.nhtc].rule = ri;
                AB.htc[AB.nhtc].el = el;
                AB.nhtc++;
            }
        } else {
            cos_mark(ri);
        }
    }
}

char* ablock_doc_css(const struct wdom* dom, const char* host) {
    if (AB.phase == 0 || !dom) return 0;
    drain();
    if (!AB.nah || !AB.coff) return 0;
    AB.serial++;

    char h[256];
    int hlen = 0;
    for (; host && host[hlen] && hlen < (int)sizeof h - 1; hlen++) h[hlen] = (char)w_lower((unsigned char)host[hlen]);
    h[hlen] = 0;

    static uint32_t unhides[128];
    int nunh = 0;

    // host-keyed rules (linear scan: a few thousand at most)
    for (int i = 0; i < AB.nah; i++) {
        if (!AB.ah[i].ndom) continue;
        if (!dom_match(AB.ah[i].dombase, AB.ah[i].ndom, h, hlen)) continue;
        if (AB.ah[i].unhide) {
            if (nunh < 128) unhides[nunh++] = (uint32_t)i;
        } else {
            cos_mark(i);
        }
    }

    // generic rules keyed on what this document's elements actually are
    AB.nhtc = 0;
    for (int el = dom->n[0].first; el >= 0; el = wdom_next(dom, el, 0)) {
        const struct wnode* n = &dom->n[el];
        if (n->type != WN_ELEM || (n->flags & WNF_REMOVED)) continue;
        int nl;
        if (n->tag) {
            const char* nm = watom_name(&dom->atoms, n->tag, &nl);
            if (nm) cos_lookup_bucket(nm, nl, el, unhides, &nunh);
        }
        if (n->id) {
            const char* nm = watom_name(&dom->atoms, n->id, &nl);
            if (nm) cos_lookup_bucket(nm, nl, el, unhides, &nunh);
        }
        for (int k = 0; k < n->nclass; k++) {
            uint16_t at = dom->classes[n->text + k];
            if (!at) continue;
            const char* nm = watom_name(&dom->atoms, at, &nl);
            if (nm) cos_lookup_bucket(nm, nl, el, unhides, &nunh);
        }
        for (int a = n->attr; a >= 0; a = dom->a[a].next) {
            const char* nm = watom_name(&dom->atoms, dom->a[a].name, &nl);
            if (nm) cos_lookup_bucket(nm, nl, el, unhides, &nunh);
        }
    }

    // :has-text() candidates: verify the base selector and the element text,
    // then hide the individual elements through an internal attribute
    int htmark[512], nhtmark = 0;
    for (int k = 0; k < AB.nhtc; k++) {
        int ri = AB.htc[k].rule, el = AB.htc[k].el;
        struct wselq* q = css_selq_compile((struct wdom*)dom, AB.ah[ri].sel, AB.ah[ri].slen);
        if (!q) continue;
        int ok = css_selq_match(q, dom, el);
        css_selq_free(q);
        if (!ok) continue;
        char txt[2048];
        int tl = wdom_text_content(dom, el, txt, (int)sizeof txt);
        if (tl < AB.ah[ri].htl) continue;
        int found = 0;
        for (int z = 0; z + AB.ah[ri].htl <= tl && !found; z++)
            if (!memcmp(txt + z, AB.ah[ri].ht, AB.ah[ri].htl)) found = 1;
        if (!found) continue;
        // tag the element (one value per rule) and remember the rule
        char val[16];
        int vl = 0;
        val[vl++] = 'r';
        int v = ri;
        char tmp[8];
        int tn = 0;
        do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn) val[vl++] = tmp[--tn];
        wdom_set_attr((struct wdom*)dom, el,
                      watom_intern((struct watoms*)&dom->atoms, "data-ablk-h", 11), val, vl);
        int dup = 0;
        for (int z = 0; z < nhtmark && !dup; z++) dup = htmark[z] == ri;
        if (!dup && nhtmark < 512) htmark[nhtmark++] = ri;
    }

    // unkeyable generics: always considered
    for (int k = 0; k < AB.nunkey; k++) {
        int ri = (int)AB.unkey[k];
        if (!AB.ah[ri].ndom) cos_mark(ri);
    }

    // emit, minus the unhides that matched this host (exact selector text)
    struct wbuf b;
    memset(&b, 0, sizeof b);
    for (int i = 0; i < AB.nah; i++) {
        if (AB.seen[i] != (int32_t)AB.serial) continue;
        if (AB.ah[i].ht) continue;                   // :has-text() rules emit via the attribute
        int canceled = 0;
        for (int k = 0; k < nunh && !canceled; k++) {
            const struct ahide* uh = &AB.ah[unhides[k]];
            if (uh->slen == AB.ah[i].slen && !memcmp(uh->sel, AB.ah[i].sel, uh->slen)) canceled = 1;
        }
        if (canceled) continue;
        if (b.len > 1 << 20) break;                  // sanity cap
        wbuf_put(&b, AB.ah[i].sel, AB.ah[i].slen);
        wbuf_put(&b, "{display:none!important}\n", 25);
    }
    for (int k = 0; k < nhtmark && b.len <= 1 << 20; k++) {
        int ri = htmark[k];
        wbuf_put(&b, "[data-ablk-h=\"r", 15);
        int v = ri;
        char tmp[8];
        int tn = 0;
        do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn) wbuf_putc(&b, tmp[--tn]);
        wbuf_put(&b, "\"]{display:none!important}\n", 27);
    }
    if (!b.len) {
        wbuf_free(&b);
        return 0;
    }
    if (!wbuf_reserve(&b, 1)) {                      // NUL-terminate: wdoc.c runs strlen() on it
        wbuf_free(&b);
        return 0;
    }
    b.p[b.len] = 0;
    w_log("[ablk] hide rules for this page: %u bytes\n", b.len);
    return b.p;                                      // engine-allocator buffer, caller frees
}


// ---- scriptlets ---------------------------------------------------------------------
// Our own implementations of the uBlock scriptlet names the lists use (the
// from-scratch rule: this library is written here, not copied). A realm gets
// the library plus one __ablk([[name, args...], ...]) call, injected by
// wdoc.c before any page script runs. Evaluated with OJS_EVAL_HIDE_SOURCE so
// the injected functions report as native.
static const char SCLIB[] =
"(function(){\n"
"if (window.__ablk) return;\n"
"var doc = document;\n"
"function owner(path) {\n"
"  var seg = path.split('.'), o = window, i, n;\n"
"  for (i = 0; i + 1 < seg.length; i++) {\n"
"    if (o == null || (typeof o !== 'object' && typeof o !== 'function')) return null;\n"
"    n = seg[i];\n"
"    if (o[n] == null) { try { o[n] = {}; } catch (e) { return null; } }\n"
"    o = o[n];\n"
"  }\n"
"  return o == null ? null : { o: o, p: seg[seg.length - 1] };\n"
"}\n"
"function val(v) {\n"
"  if (v === 'true' || v === 'yes') return true;\n"
"  if (v === 'false' || v === 'no') return false;\n"
"  if (v === 'undefined') return undefined;\n"
"  if (v === 'null') return null;\n"
"  if (v === 'noopFunc') return function () {};\n"
"  if (v === 'trueFunc') return function () { return true; };\n"
"  if (v === 'falseFunc') return function () { return false; };\n"
"  if (v === '{}' || v === '{ }') return {};\n"
"  if (v === '[]') return [];\n"
"  if (v.length >= 2 && v.charCodeAt(0) === 39 && v.charCodeAt(v.length - 1) === 39) return v.slice(1, -1);\n"
"  var n = Number(v);\n"
"  return String(n) === v ? n : v;\n"
"}\n"
"function pat(p) {\n"
"  if (!p) return null;\n"
"  if (p.length > 2 && p[0] === '/' && p[p.length - 1] === '/') {\n"
"    try { return new RegExp(p.slice(1, -1)); } catch (e) { return null; }\n"
"  }\n"
"  return p;\n"
"}\n"
"function hit(s, p) {\n"
"  if (p == null) return true;\n"
"  return typeof p === 'string' ? s.indexOf(p) >= 0 : p.test(s);\n"
"}\n"
"var H = {\n"
"'set-constant': function (a) {\n"
"  var t = owner(a[0] || '');\n"
"  if (!t) return;\n"
"  try { Object.defineProperty(t.o, t.p, { value: val(a[1]), writable: true, configurable: true }); } catch (e) {}\n"
"},\n"
"'abort-current-inline-script': function (a) {\n"
"  var t = owner(a[0] || '');\n"
"  if (!t) return;\n"
"  var needle = a[1] || '', orig = t.o[t.p];\n"
"  Object.defineProperty(t.o, t.p, {\n"
"    get: function () {\n"
"      var cs = doc.currentScript;\n"
"      if (cs && (!needle || String(cs.textContent || '').indexOf(needle) >= 0)) throw new Error('blocked');\n"
"      return orig;\n"
"    },\n"
"    set: function (v) { orig = v; },\n"
"    configurable: true\n"
"  });\n"
"},\n"
"'abort-on-property-write': function (a) {\n"
"  var t = owner(a[0] || '');\n"
"  if (!t) return;\n"
"  var re = pat(a[1] || ''), orig = t.o[t.p];\n"
"  Object.defineProperty(t.o, t.p, {\n"
"    get: function () { return orig; },\n"
"    set: function (v) {\n"
"      if (!re || hit(String(v), re)) throw new Error('blocked');\n"
"      orig = v;\n"
"    },\n"
"    configurable: true\n"
"  });\n"
"},\n"
"'abort-on-addEventListener': function (a) {\n"
"  var type = a[0] || '', name = a[1] || '', orig = EventTarget.prototype.addEventListener;\n"
"  EventTarget.prototype.addEventListener = function (t, l) {\n"
"    if (t === type && (!name || String((l && l.name) || '').indexOf(name) >= 0)) throw new Error('blocked');\n"
"    return orig.apply(this, arguments);\n"
"  };\n"
"},\n"
"'remove-node-text': function (a) {\n"
"  var tag = String(a[0] || '').toUpperCase(), text = a[1] || '';\n"
"  function kill(el) { if (el.parentNode) el.parentNode.removeChild(el); }\n"
"  function sweep(root) {\n"
"    if (!root.getElementsByTagName) return;\n"
"    var els = root.getElementsByTagName(tag);\n"
"    for (var i = els.length - 1; i >= 0; i--)\n"
"      if (String(els[i].textContent || '').indexOf(text) >= 0) kill(els[i]);\n"
"  }\n"
"  sweep(doc);\n"
"  new MutationObserver(function (ms) {\n"
"    for (var i = 0; i < ms.length; i++) {\n"
"      var ad = ms[i].addedNodes;\n"
"      for (var k = 0; k < ad.length; k++) {\n"
"        var el = ad[k];\n"
"        if (!el || el.nodeType !== 1) continue;\n"
"        if ((el.tagName || '') === tag && String(el.textContent || '').indexOf(text) >= 0) kill(el);\n"
"        else sweep(el);\n"
"      }\n"
"    }\n"
"  }).observe(doc.documentElement || doc, { childList: true, subtree: true });\n"
"},\n"
"'set-cookie': function (a) {\n"
"  doc.cookie = a[0] + '=' + (a[1] || '') + ';path=' + (a[2] || '/') + ';max-age=31540000';\n"
"  for (var i = 3; i < a.length; i++) if (a[i] === 'reload') W.location.reload();\n"
"},\n"
"'set-local-storage-item': function (a) {\n"
"  if (a[1] === '$remove$') W.localStorage.removeItem(a[0]);\n"
"  else W.localStorage.setItem(a[0], a[1]);\n"
"},\n"
"'cookie-remover': function (a) {\n"
"  var nd = a[0] || '', parts = String(doc.cookie).split(';');\n"
"  for (var i = 0; i < parts.length; i++) {\n"
"    var kv = parts[i], e = kv.indexOf('=');\n"
"    if (e < 0) continue;\n"
"    var nm = kv.charCodeAt(0) === 32 ? kv.slice(1, e) : kv.slice(0, e);\n"
"    if (nm.indexOf(nd) >= 0) doc.cookie = nm + '=;max-age=0;path=/';\n"
"  }\n"
"},\n"
"'no-fetch-if': function (a) {\n"
"  var p = pat(a[0] || ''), orig = W.fetch;\n"
"  if (!orig) return;\n"
"  W.fetch = function (u) {\n"
"    var s = typeof u === 'string' ? u : (u && u.url) || '';\n"
"    if (hit(s, p)) return Promise.resolve({ ok: true, status: 200, url: s,\n"
"      text: function () { return Promise.resolve(''); },\n"
"      json: function () { return Promise.resolve({}); },\n"
"      headers: { get: function () { return null; } } });\n"
"    return orig.apply(this, arguments);\n"
"  };\n"
"},\n"
"'no-xhr-if': function (a) {\n"
"  var p = pat(a[0] || ''), X = W.XMLHttpRequest;\n"
"  if (!X) return;\n"
"  var op = X.prototype.open, sd = X.prototype.send;\n"
"  X.prototype.open = function (m, u) { this.__abu = String(u || ''); return op.apply(this, arguments); };\n"
"  X.prototype.send = function () {\n"
"    var self = this;\n"
"    if (!hit(self.__abu, p)) return sd.apply(this, arguments);\n"
"    setTimeout(function () {\n"
"      try {\n"
"        Object.defineProperty(self, 'readyState', { value: 4, configurable: true });\n"
"        Object.defineProperty(self, 'status', { value: 200, configurable: true });\n"
"        Object.defineProperty(self, 'responseText', { value: '', configurable: true });\n"
"      } catch (e) {}\n"
"      if (self.onreadystatechange) self.onreadystatechange();\n"
"      if (self.onload) self.onload();\n"
"    }, 1);\n"
"  };\n"
"}\n"
"};\n"
"var AL = { set: 'set-constant', acs: 'abort-current-inline-script', rmnt: 'remove-node-text',\n"
"  aost: 'abort-on-property-write', aeld: 'abort-on-addEventListener' };\n"
"window.__ablk = function (list) {\n"
"  for (var i = 0; i < list.length; i++) {\n"
"    var e = list[i], fn = H[AL[e[0]] || e[0]];\n"
"    if (fn) { try { fn(e.slice(1)); } catch (x) {} }\n"
"  }\n"
"};\n"
"})();\n";

// Generated JS: the library plus one __ablk() call with this document's
// scriptlets. Engine-allocator buffer; wdoc.c evaluates it in the page realm
// before any of the page's own scripts run.
char* ablock_doc_scriptlets(const char* host) {
    if (AB.phase == 0) return 0;
    drain();
    if (!AB.nas) return 0;
    char h[256];
    int hlen = 0;
    for (; host && host[hlen] && hlen < (int)sizeof h - 1; hlen++) h[hlen] = (char)w_lower((unsigned char)host[hlen]);
    h[hlen] = 0;
    int match[128];
    int n = 0;
    for (int i = 0; i < AB.nas && n < 128; i++) {
        if (AB.as[i].ndom && !dom_match(AB.as[i].dombase, AB.as[i].ndom, h, hlen)) continue;
        match[n++] = i;
    }
    if (!n) return 0;
    struct wbuf b;
    memset(&b, 0, sizeof b);
    wbuf_put(&b, SCLIB, (int)strlen(SCLIB));
    wbuf_put(&b, "__ablk([", 8);
    for (int k = 0; k < n; k++) {
        const struct asc* sc = &AB.as[match[k]];
        if (k) wbuf_putc(&b, ',');
        wbuf_putc(&b, '[');
        // JSON-escaped strings (the args come verbatim from the list text)
        #define SCJSTR(txt, ln) do { \
            wbuf_putc(&b, '"'); \
            for (int z = 0; z < (ln); z++) { \
                unsigned char c = (unsigned char)(txt)[z]; \
                if (c == '"' || c == 92) { wbuf_putc(&b, '\\'); wbuf_putc(&b, (char)c); } \
                else if (c < 0x20) { \
                    wbuf_putc(&b, '\\'); wbuf_putc(&b, 'u'); \
                    wbuf_putc(&b, "0123456789abcdef"[(c >> 4) & 15]); \
                    wbuf_putc(&b, "0123456789abcdef"[c & 15]); \
                } else wbuf_putc(&b, (char)c); \
            } \
            wbuf_putc(&b, '"'); \
        } while (0)
        SCJSTR(sc->name, sc->nlen);
        for (int a = 0; a < sc->nargs; a++) {
            wbuf_putc(&b, ',');
            SCJSTR(sc->args[a], sc->alen[a]);
        }
        #undef SCJSTR
        wbuf_putc(&b, ']');
    }
    wbuf_put(&b, "]);\n", 4);
    if (!wbuf_reserve(&b, 1)) {
        wbuf_free(&b);
        return 0;
    }
    b.p[b.len] = 0;
    return b.p;
}
