// b_intl.c — the Intl namespace (ECMA-402). The locale/plural/rounding/
// pattern machinery lives in intl.c over the CLDR tables (intl_data.c);
// this file is the JS surface: constructors, option handling, resolvedOptions
// and format/formatToParts, plus the toLocale* prototype methods.

#include "ojs_int.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "intl.h"

extern double ojs_sys_time_ms(void);            // embedder hook (b_date.c keeps the weak default)

// service kinds
enum { SV_NF = 1, SV_DT, SV_PR, SV_LOCALE, SV_LF, SV_RTF, SV_COLLATOR };

struct intlsvc {
    struct obj base;
    unsigned char kind;
    unsigned char pad[3];
    jv tag;                     // canonical locale string (traced)
    struct intl_nfmt nf;
    char curbuf[8];             // currency code text when not in the tables
    struct intl_dfmt df;
    struct intl_dtcomp comp;
    signed char date_style, time_style;
    unsigned char lf_type, lf_style;
    unsigned char rtf_field, rtf_width, rtf_numeric;
    unsigned char pr_type;
};

static struct obj* nf_proto, *dt_proto, *pr_proto, *loc_proto, *lf_proto, *rtf_proto, *col_proto;

static int dt_time_arg(ojs* J, jv v, double* out);

static jv cstr_jv(ojs* J, const char* s) {
    struct str* r = str_from_cstr(J, s);
    return r ? jv_from_str(r) : JV_EXC;
}

extern double km_floor(double);   // src/kmath.c

// intl_opt_num into a short field
static int opt_num_s(ojs* J, struct obj* o, const char* name, double lo, double hi, short* out) {
    int v = -1;
    if (intl_opt_num(J, o, name, lo, hi, &v) < 0) return -1;
    *out = (short)v;
    return 0;
}

// intl_opt_enum into a signed char field
static int opt_enum_c(ojs* J, struct obj* o, const char* name, const char* const* vals,
                      int nvals, int def, signed char* out) {
    int v = def;
    if (intl_opt_enum(J, o, name, vals, nvals, def, &v) < 0) return -1;
    *out = (signed char)v;
    return 0;
}

static void intl_trace(ojs* J, struct obj* o) {
    struct intlsvc* s = (struct intlsvc*)o;
    if (s->tag && !jv_is_undef(s->tag)) gc_mark_value(J, s->tag);
}

static struct intlsvc* this_svc(ojs* J, jv t, int kind, const char* what) {
    if (jv_is_obj(t) && obj_class(jv_obj(t)) == OC_INTL) {
        struct intlsvc* s = (struct intlsvc*)jv_obj(t);
        if (s->kind == kind) return s;
    }
    throw_type(J, "%s called on incompatible receiver", what);
    return 0;
}

static struct intlsvc* svc_of(jv made) { return (struct intlsvc*)jv_obj(made); }

// fresh service object through OrdinaryCreateFromConstructor (subclass-safe)
static struct intlsvc* svc_create(ojs* J, jv new_target, struct obj* proto, int kind) {
    jv made = ordinary_create_from_ctor(J, new_target, proto, OC_INTL, sizeof(struct intlsvc));
    if (made == JV_EXC) return 0;
    struct intlsvc* s = (struct intlsvc*)jv_obj(made);
    memset((char*)s + sizeof(struct obj), 0, sizeof(struct intlsvc) - sizeof(struct obj));
    s->kind = (unsigned char)kind;
    s->tag = JV_UNDEFINED;
    s->nf.cur_entry = 0xFFFF;
    s->nf.unit_entry = 0xFFFF;
    s->nf.cur_ci = -1;
    s->nf.unit_ui = -1;
    s->nf.min_int = 1;
    s->nf.min_frac = -1;
    s->nf.max_frac = -1;
    s->nf.min_sig = -1;
    s->nf.max_sig = -1;
    s->nf.mode = RM_HALF_EXPAND;
    s->nf.sign_disp = 0;
    s->date_style = -1;
    s->time_style = -1;
    s->pr_type = 0;
    return s;
}

static jv svc_tag(ojs* J, struct intlsvc* s, const char* ctag) {
    jv tag = cstr_jv(J, ctag);
    if (!tag) return JV_EXC;
    s->tag = tag;
    return tag;
}

// the tag of a service as a C string
static size_t svc_tag_buf(struct intlsvc* s, char* buf, unsigned cap) {
    if (jv_is_undef(s->tag)) { buf[0] = 0; return 0; }
    size_t n = str_to_utf8_buf(jv_str(s->tag), buf, cap - 1);
    buf[n] = 0;
    return n;
}

// ---------------------------------------------------------------- locale lists

// CanonicalizeLocaleList: iterable of strings/objects -> array of canonical tags
static jv canonical_locale_list(ojs* J, jv locales) {
    struct obj* arr = obj_new_array(J, 0);
    if (!arr) return JV_EXC;
    if (jv_is_undef(locales)) return jv_from_obj(arr);
    uint32_t count = 0;
    jv single = JV_UNDEFINED;
    if (jv_is_obj(locales)) {
        jv meth = obj_get(J, jv_obj(locales), bname_key(J, "@@iterator"), locales);
        if (jv_is_exc(meth)) return JV_EXC;
        if (jv_is_obj(meth)) {
            jv it = ojs_call_v(J, meth, locales, 0, 0);   // GetIterator
            if (jv_is_exc(it)) return JV_EXC;
            if (!jv_is_obj(it)) {
                throw_type(J, "locales iterator is not an object");
                return JV_EXC;
            }
            jv next_fn = obj_get(J, jv_obj(it), bname_key(J, "next"), it);
            if (jv_is_exc(next_fn)) return JV_EXC;
            for (;;) {
                jv res = ojs_call_v(J, next_fn, it, 0, 0);
                if (jv_is_exc(res)) return JV_EXC;
                jv done = obj_get(J, jv_obj(res), bname_key(J, "done"), res);
                if (jv_is_exc(done)) return JV_EXC;
                if (to_boolean(done)) break;
                jv v = obj_get(J, jv_obj(res), bname_key(J, "value"), res);
                if (jv_is_exc(v)) return JV_EXC;
                jv s;
                if (jv_is_obj(v)) {
                    jv ts = obj_get(J, jv_obj(v), bname_key(J, "toString"), v);
                    if (jv_is_exc(ts)) return JV_EXC;
                    s = (jv_is_obj(ts) && obj_class(jv_obj(ts)) == OC_NATIVE)
                            ? ojs_call_v(J, ts, v, 0, 0)
                            : to_string(J, v);
                } else {
                    s = to_string(J, v);
                }
                if (jv_is_exc(s)) return JV_EXC;
                if (jv_is_undef(s) || jv_is_nullish(s)) {
                    throw_type(J, "locales list entries must be strings or objects");
                    return JV_EXC;
                }
                single = s;
                struct str* str = jv_str(s);
                char buf[128];
                if (str_utf8_len(str) >= sizeof buf) { throw_range(J, "locale tag is too long"); return JV_EXC; }
                size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
                buf[bl] = 0;
                char canon[128];
                if (intl_canonicalize(buf, (unsigned)bl, canon, sizeof canon) < 0) {
                    throw_range(J, "invalid language tag: %S", jv_str(s));
                    return JV_EXC;
                }
                jv cs = cstr_jv(J, canon);
                if (!cs) return JV_EXC;
                for (uint32_t i = 0; i < count; i++) {
                    jv ev = obj_get(J, arr, PK_FROM_INDEX(i), jv_from_obj(arr));
                    if (jv_is_exc(ev)) return JV_EXC;
                    if (jv_str(ev) == jv_str(cs)) {
                        throw_range(J, "duplicate locale in the list");
                        return JV_EXC;
                    }
                }
                if (obj_define_value(J, arr, PK_FROM_INDEX(count++), cs, PA_DEFAULT) < 0) return JV_EXC;
            }
            return jv_from_obj(arr);
        }
    }
    // a plain string (or a non-iterable object: ToString of it)
    jv s = jv_is_undef(single) ? to_string(J, locales) : single;
    if (jv_is_exc(s)) return JV_EXC;
    struct str* str = jv_str(s);
    char buf[128];
    if (str_utf8_len(str) >= sizeof buf) { throw_range(J, "locale tag is too long"); return JV_EXC; }
    size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
    buf[bl] = 0;
    char canon[128];
    if (intl_canonicalize(buf, (unsigned)bl, canon, sizeof canon) < 0) {
        throw_range(J, "invalid language tag: %S", jv_str(s));
        return JV_EXC;
    }
    jv cs = cstr_jv(J, canon);
    if (!cs) return JV_EXC;
    if (obj_define_value(J, arr, PK_FROM_INDEX(0), cs, PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(arr);
}

// resolve the best covered locale; writes the matched request tag into out
static int resolve_locales(ojs* J, jv locales, char* out, unsigned cap) {
    jv list = canonical_locale_list(J, locales);
    if (jv_is_exc(list)) return -1;
    struct obj* arr = jv_obj(list);
    for (uint32_t i = 0; i < arr->alen; i++) {
        jv ev = obj_get(J, arr, PK_FROM_INDEX(i), jv_from_obj(arr));
        if (jv_is_exc(ev)) return -1;
        struct str* s = jv_str(ev);
        char buf[128];
        size_t bl = str_to_utf8_buf(s, buf, sizeof buf - 1);
        buf[bl] = 0;
        if (intl_resolve(buf, (unsigned)bl) >= 0) {
            if (bl >= cap) { throw_range(J, "locale tag is too long"); return -1; }
            memcpy(out, buf, bl + 1);
            return 0;
        }
    }
    ojs_snprintf(out, cap, "en");
    return 0;
}

// ---------------------------------------------------------------- shared option helpers

// numbering system index from the resolved tag's -u-nu, else the locale default
static int nu_of(const char* tag, unsigned li) {
    char kw[24];
    if (intl_tag_keyword(tag, "nu", kw, sizeof kw) > 0) {
        for (unsigned i = 0; i < intl_nsys_n; i++)
            if (strcmp(intl_nsys[i].id, kw) == 0) return (int)i;
    }
    return intl_locales[li].nu;
}

static int nu_by_id(const char* id) {
    for (unsigned i = 0; i < intl_nsys_n; i++)
        if (strcmp(intl_nsys[i].id, id) == 0) return (int)i;
    return -1;
}

static int hc_resolve(ojs* J, struct obj* opts, unsigned li, int* hour12_out) {
    static const char* const HCS[] = { "h11", "h12", "h23", "h24" };
    int hc = intl_locales[li].hc;
    int hc_opt = -1;
    if (intl_opt_enum(J, opts, "hourCycle", HCS, 4, -1, &hc_opt) < 0) return -1;
    if (hc_opt >= 0) {
        hc = hc_opt;
    } else if (opts) {
        int h12 = -1;
        if (intl_opt_bool(J, opts, "hour12", -1, &h12) < 0) return -1;
        if (h12 >= 0) hc = h12 ? 1 : 2;
    }
    if (hour12_out) *hour12_out = hc == 0 || hc == 1;
    return hc;
}

// ---------------------------------------------------------------- getCanonicalLocales / supportedValuesOf

static jv intl_get_canonical_locales(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)t; (void)magic;
    return canonical_locale_list(J, argc ? argv[0] : JV_UNDEFINED);
}

static jv intl_supported_values(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)t; (void)magic;
    if (argc == 0) { throw_type(J, "supportedValuesOf requires a type argument"); return JV_EXC; }
    jv s = to_string(J, argv[0]);
    if (jv_is_exc(s)) return JV_EXC;
    struct str* str = jv_str(s);
    char buf[24];
    size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
    buf[bl] = 0;
    struct obj* arr = obj_new_array(J, 0);
    if (!arr) return JV_EXC;
    uint32_t n = 0;
    if (strcmp(buf, "calendar") == 0) {
        jv v = str_value(J, "gregory");
        if (obj_define_value(J, arr, PK_FROM_INDEX(n++), v, PA_DEFAULT) < 0) return JV_EXC;
    } else if (strcmp(buf, "numberingSystem") == 0) {
        for (unsigned i = 0; i < intl_nsys_n; i++) {
            jv v = cstr_jv(J, intl_nsys[i].id);
            if (!v) return JV_EXC;
            if (obj_define_value(J, arr, PK_FROM_INDEX(n++), v, PA_DEFAULT) < 0) return JV_EXC;
        }
    } else if (strcmp(buf, "currency") == 0) {
        for (unsigned i = 0; i < intl_ncurfrac; i++) {
            jv v = cstr_jv(J, intl_curfrac[i].code);
            if (!v) return JV_EXC;
            if (obj_define_value(J, arr, PK_FROM_INDEX(n++), v, PA_DEFAULT) < 0) return JV_EXC;
        }
    } else if (strcmp(buf, "timeZone") == 0) {
        jv v = str_value(J, "UTC");
        if (obj_define_value(J, arr, PK_FROM_INDEX(n++), v, PA_DEFAULT) < 0) return JV_EXC;
    } else if (strcmp(buf, "unit") == 0) {
        for (unsigned i = 0; i < intl_nunit_n; i++) {
            jv v = cstr_jv(J, intl_unit_id[i]);
            if (!v) return JV_EXC;
            if (obj_define_value(J, arr, PK_FROM_INDEX(n++), v, PA_DEFAULT) < 0) return JV_EXC;
        }
    } else if (strcmp(buf, "collation") != 0) {
        throw_range(J, "invalid type \"%s\"", buf);
        return JV_EXC;
    }
    return jv_from_obj(arr);
}

// ---------------------------------------------------------------- Intl.Locale

// set/replace (val 0 or "": remove) a -u- keyword in a canonical tag
static int loc_set_kw(char* tag, size_t cap, const char* key, const char* val) {
    char base[96];
    ojs_snprintf(base, sizeof base, "%s", tag);
    char* u0 = strstr(base, "-u-");
    if (u0) *u0 = 0;
    char uext[96] = "";
    char* uo = strstr(tag, "-u-");
    if (uo) ojs_snprintf(uext, sizeof uext, "%s", uo + 3);
    char out[160];
    ojs_snprintf(out, sizeof out, "%s", base);
    size_t kl = strlen(key);
    int wrote_u = 0;
    #define KW_PUT(first, second) do { \
        const char* f_ = (first); \
        const char* s_ = (second); \
        if (strlen(out) + 4 + (f_ ? strlen(f_) : 0) + (s_ ? strlen(s_) + 1 : 0) + 1 >= sizeof out) return -1; \
        strcat(out, wrote_u ? "-" : "-u-"); \
        wrote_u = 1; \
        if (f_) { strcat(out, f_); if (s_) { strcat(out, "-"); strcat(out, s_); } } \
    } while (0)
    char* p = uext;
    int replaced = 0;
    while (p && *p) {
        char* dash = strchr(p, '-');
        size_t l = dash ? (size_t)(dash - p) : strlen(p);
        if (l == kl && memcmp(p, key, kl) == 0) {
            // skip its value subtags
            const char* q = dash ? dash + 1 : p + l;
            while (*q) {
                const char* d2 = strchr(q, '-');
                size_t vl = d2 ? (size_t)(d2 - q) : strlen(q);
                if (vl == 2) break;
                if (d2) q = d2 + 1;
                else { q += vl; break; }
            }
            p = (char*)q;
            if (val && val[0] && val[0] != '-') KW_PUT(key, val);
            replaced = 1;
            continue;
        }
        // keep this subtag; a keyword keeps key + value subtags
        const char* q;
        if (l == 2) {
            q = dash ? dash + 1 : p + l;
            while (*q) {
                const char* d2 = strchr(q, '-');
                size_t vl = d2 ? (size_t)(d2 - q) : strlen(q);
                if (vl == 2) break;
                if (d2) q = d2 + 1;
                else { q += vl; break; }
            }
        } else {
            q = dash ? dash + 1 : p + l;
        }
        size_t keep = (size_t)(q - p);
        char kept[48];
        if (keep >= sizeof kept) return -1;
        memcpy(kept, p, keep);
        kept[keep] = 0;
        KW_PUT(kept, 0);
        p += keep;
    }
    if (!replaced && val && val[0] && val[0] != '-') KW_PUT(key, val);
    if (strlen(out) >= cap) return -1;
    memcpy(tag, out, strlen(out) + 1);
    return 0;
    #undef KW_PUT
}

static jv locale_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    if (argc == 0) {
        throw_type(J, "Intl.Locale requires a language tag argument");
        return JV_EXC;
    }
    jv s = to_string(J, argv[0]);
    if (jv_is_exc(s)) return JV_EXC;
    struct str* str = jv_str(s);
    char buf[128];
    if (str_utf8_len(str) >= sizeof buf) { throw_range(J, "locale tag is too long"); return JV_EXC; }
    size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
    buf[bl] = 0;
    struct obj* opts = 0;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;

    char canon[128];
    if (intl_canonicalize(buf, (unsigned)bl, canon, sizeof canon) < 0) {
        throw_range(J, "invalid language tag: %S", jv_str(s));
        return JV_EXC;
    }
    // subtag overrides: rebuild "language(-script)?(-region)?" from the base
    char lang[16] = "", script[8] = "", reg[8] = "";
    if (opts) {
        if (intl_opt_cstr(J, opts, "language", lang, sizeof lang) < 0) return JV_EXC;
        if (intl_opt_cstr(J, opts, "script", script, sizeof script) < 0) return JV_EXC;
        if (intl_opt_cstr(J, opts, "region", reg, sizeof reg) < 0) return JV_EXC;
        if (lang[0] || script[0] || reg[0]) {
            char base[96];
            ojs_snprintf(base, sizeof base, "%s", canon);
            char* u = strstr(base, "-u-");
            if (u) *u = 0;
            // split the base manually
            char b2[96];
            ojs_snprintf(b2, sizeof b2, "%s", base);
            char* segs[8];
            int ns = 0;
            char* seg = b2;
            while (seg && ns < 8) {
                segs[ns++] = seg;
                char* d = strchr(seg, '-');
                if (!d) break;
                *d = 0;
                seg = d + 1;
            }
            const char* l2 = lang[0] ? lang : segs[0];
            const char* s2 = script[0] ? script : (ns > 1 && strlen(segs[1]) == 4 ? segs[1] : 0);
            const char* r2 = reg[0] ? reg : 0;
            if (!r2 && ns > 1) {
                const char* cand = s2 ? (ns > 2 ? segs[2] : 0) : segs[1];
                size_t cl = cand ? strlen(cand) : 0;
                if (cand && cl <= 3 && (cl == 3 || ((cand[0] | 32) >= 'a' && (cand[0] | 32) <= 'z'))) r2 = cand;
            }
            char merged[128];
            int ml = ojs_snprintf(merged, sizeof merged, "%s", l2);
            if (s2) ml += ojs_snprintf(merged + ml, (int)(sizeof merged - ml), "-%s", s2);
            if (r2) ml += ojs_snprintf(merged + ml, (int)(sizeof merged - ml), "-%s", r2);
            char* u2 = strstr(canon, "-u-");
            char* x2 = strstr(canon, "-x-");
            char* cut = 0;
            if (u2) cut = u2;
            if (x2 && (!cut || x2 < cut)) cut = x2;
            if (cut) ojs_snprintf(merged + ml, (int)(sizeof merged - ml), "%s", cut);
            if (intl_canonicalize(merged, (unsigned)strlen(merged), canon, sizeof canon) < 0) {
                throw_range(J, "invalid locale options");
                return JV_EXC;
            }
        }
        // keyword options
        static const char* const KEY[] = { "calendar", "caseFirst", "collation", "hourCycle", "numberingSystem" };
        static const char* const KW[] = { "ca", "kf", "co", "hc", "nu" };
        for (int i = 0; i < 5; i++) {
            char kv[24];
            if (intl_opt_cstr(J, opts, KEY[i], kv, sizeof kv) < 0) return JV_EXC;
            if (kv[0] && loc_set_kw(canon, sizeof canon, KW[i], kv) < 0) { throw_range(J, "locale is too long"); return JV_EXC; }
        }
        int numeric = 0;
        if (intl_opt_bool(J, opts, "numeric", -1, &numeric) < 0) return JV_EXC;
        if (numeric >= 0 && loc_set_kw(canon, sizeof canon, "kn", numeric ? "true" : "false") < 0) {
            throw_range(J, "locale is too long");
            return JV_EXC;
        }
        // re-canonicalize: sort the u keywords, drop "true" values, case fixes
        char rec[128];
        if (intl_canonicalize(canon, (unsigned)strlen(canon), rec, sizeof rec) == 0) {
            memcpy(canon, rec, sizeof rec);
        }
    }
    struct intlsvc* r = svc_create(J, J->native_new_target, loc_proto, SV_LOCALE);
    if (!r) return JV_EXC;
    if (jv_is_exc(svc_tag(J, r, canon))) return JV_EXC;
    return jv_from_obj((struct obj*)r);
}

// the base name (tag without extensions) as a C string
static size_t loc_base_buf(struct intlsvc* s, char* buf, unsigned cap) {
    size_t n = svc_tag_buf(s, buf, cap);
    char* c1 = strstr(buf, "-u-");
    char* c2 = strstr(buf, "-t-");
    char* c3 = strstr(buf, "-x-");
    char* end = buf + n;
    if (c1 && c1 < end) end = c1;
    if (c2 && c2 < end) end = c2;
    if (c3 && c3 < end) end = c3;
    *end = 0;
    return (size_t)(end - buf);
}

static jv loc_base_name(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_LOCALE, "Intl.Locale.prototype.baseName");
    if (!s) return JV_EXC;
    char buf[128];
    size_t n = loc_base_buf(s, buf, sizeof buf);
    struct str* r = str_from_utf8(J, buf, n);
    return r ? jv_from_str(r) : JV_EXC;
}

// split a base name into subtag pointers (in place)
static int loc_split(char* base, char* segs[], int max) {
    int ns = 0;
    char* seg = base;
    while (seg && ns < max) {
        segs[ns++] = seg;
        char* d = strchr(seg, '-');
        if (!d) break;
        *d = 0;
        seg = d + 1;
    }
    return ns;
}

static int is_region_seg(const char* s) {
    size_t l = strlen(s);
    if (l == 2) return (s[0] | 32) >= 'a' && (s[0] | 32) <= 'z';
    if (l == 3) return s[0] >= '0' && s[0] <= '9';
    return 0;
}

static jv loc_simple_get(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv;
    struct intlsvc* s = this_svc(J, t, SV_LOCALE, "Intl.Locale.prototype getter");
    if (!s) return JV_EXC;
    char buf[96];
    size_t n = loc_base_buf(s, buf, sizeof buf);
    char* segs[8];
    int ns = loc_split(buf, segs, 8);
    (void)n;
    // variant segments start after lang[-script][-region]
    int vstart = 1;
    if (ns > 1 && strlen(segs[1]) == 4) vstart = 2;
    if (ns > vstart && is_region_seg(segs[vstart])) vstart++;
    switch (magic) {
    case 0: return str_value(J, segs[0]);   // language
    case 1:                                  // script
        if (ns > 1 && strlen(segs[1]) == 4) return str_value(J, segs[1]);
        return JV_UNDEFINED;
    case 2:                                  // region
        if (ns > vstart - 1 && vstart > 1) return str_value(J, segs[vstart - 1]);
        if (ns > 1 && is_region_seg(segs[1]) && strlen(segs[1]) != 4) return str_value(J, segs[1]);
        return JV_UNDEFINED;
    case 3: {                                // variants
        if (vstart >= ns) return JV_UNDEFINED;
        struct sbuf sb;
        sb_init(J, &sb);
        for (int i = vstart; i < ns; i++) {
            if (i > vstart) sb_putc(&sb, '-');
            sb_puts(&sb, segs[i]);
        }
        return sb_done(&sb);
    }
    default: return JV_UNDEFINED;
    }
}

static jv loc_kw_get(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv;
    struct intlsvc* s = this_svc(J, t, SV_LOCALE, "Intl.Locale.prototype getter");
    if (!s) return JV_EXC;
    char buf[128];
    size_t bl = svc_tag_buf(s, buf, sizeof buf);
    static const char* const KW[] = { "ca", "kf", "co", "nu" };
    if (magic == 4) {   // hourCycle
        char kw[12];
        if (intl_tag_keyword(buf, "hc", kw, sizeof kw) > 0) return str_value(J, kw);
        int li = intl_resolve(buf, (unsigned)bl);
        if (li < 0) return JV_UNDEFINED;
        int hc = intl_locales[li].hc;
        return str_value(J, hc == 0 ? "h11" : hc == 1 ? "h12" : hc == 2 ? "h23" : "h24");
    }
    if (magic == 5) {   // hour12
        char kw[12];
        if (intl_tag_keyword(buf, "hc", kw, sizeof kw) > 0)
            return jv_bool(strcmp(kw, "h11") == 0 || strcmp(kw, "h12") == 0);
        int li = intl_resolve(buf, (unsigned)bl);
        if (li < 0) return JV_UNDEFINED;
        return jv_bool(intl_locales[li].hc == 0 || intl_locales[li].hc == 1);
    }
    if (magic == 6) {   // numeric
        char kw[12];
        int r = intl_tag_keyword(buf, "kn", kw, sizeof kw);
        if (r > 0) return jv_bool(strcmp(kw, "false") != 0);
        if (r < 0) return JV_TRUE;   // bare kn
        return JV_FALSE;
    }
    if (magic >= 7) return JV_UNDEFINED;
    char kw[20];
    if (intl_tag_keyword(buf, KW[magic], kw, sizeof kw) > 0) return str_value(J, kw);
    return JV_UNDEFINED;
}

static jv loc_to_string(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_LOCALE, "Intl.Locale.prototype.toString");
    if (!s) return JV_EXC;
    return s->tag;
}

// maximize/minimize share the base-transform-reattach flow
static jv loc_transform(ojs* J, jv t, int maximize, const char* what) {
    struct intlsvc* s = this_svc(J, t, SV_LOCALE, what);
    if (!s) return JV_EXC;
    char buf[128];
    svc_tag_buf(s, buf, sizeof buf);
    char ext[96] = "";
    char* u = strstr(buf, "-u-");
    char* x = strstr(buf, "-x-");
    char* cut = 0;
    if (u) cut = u;
    if (x && (!cut || x < cut)) cut = x;
    if (cut) {
        ojs_snprintf(ext, sizeof ext, "%s", cut);
        *cut = 0;
    }
    char out[128];
    int r = maximize ? intl_maximize(buf, (unsigned)strlen(buf), out, sizeof out)
                     : intl_minimize(buf, (unsigned)strlen(buf), out, sizeof out);
    if (r < 0) return s->tag;
    if (ext[0] && strlen(out) + strlen(ext) + 1 < sizeof out) strcat(out, ext);
    struct intlsvc* nr = svc_create(J, J->native_new_target, loc_proto, SV_LOCALE);
    if (!nr) return JV_EXC;
    if (jv_is_exc(svc_tag(J, nr, out))) return JV_EXC;
    return jv_from_obj((struct obj*)nr);
}

static jv loc_maximize(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return loc_transform(J, t, 1, "Intl.Locale.prototype.maximize");
}

static jv loc_minimize(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    return loc_transform(J, t, 0, "Intl.Locale.prototype.minimize");
}

// ---------------------------------------------------------------- Intl.PluralRules

static jv pr_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    struct obj* opts = 0;
    char loc[128];
    if (resolve_locales(J, argc > 0 ? argv[0] : JV_UNDEFINED, loc, sizeof loc) < 0) return JV_EXC;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;
    int li = intl_resolve(loc, (unsigned)strlen(loc));
    if (li < 0) return throw_range(J, "no locale data");
    struct intlsvc* s = svc_create(J, J->native_new_target, pr_proto, SV_PR);
    if (!s) return JV_EXC;
    if (jv_is_exc(svc_tag(J, s, intl_locales[li].tag))) return JV_EXC;
    static const char* const TYPES[] = { "cardinal", "ordinal" };
    int ty = 0;
    if (intl_opt_enum(J, opts, "type", TYPES, 2, 0, &ty) < 0) return JV_EXC;
    s->pr_type = (unsigned char)ty;
    s->nf.card = ty == 1 ? intl_locales[li].ord : intl_locales[li].card;
    if (opt_num_s(J, opts, "minimumIntegerDigits", 1, 21, &s->nf.min_int) < 0) return JV_EXC;
    if (s->nf.min_int < 0) s->nf.min_int = 1;
    if (opt_num_s(J, opts, "minimumFractionDigits", 0, 20, &s->nf.min_frac) < 0) return JV_EXC;
    if (opt_num_s(J, opts, "maximumFractionDigits", 0, 20, &s->nf.max_frac) < 0) return JV_EXC;
    if (s->nf.max_frac < 0) s->nf.max_frac = s->nf.min_frac > 3 ? s->nf.min_frac : 3;
    if (s->nf.min_frac < 0) s->nf.min_frac = 0;
    if (s->nf.min_frac > s->nf.max_frac) s->nf.max_frac = s->nf.min_frac;
    if (opt_num_s(J, opts, "minimumSignificantDigits", 1, 21, &s->nf.min_sig) < 0) return JV_EXC;
    if (opt_num_s(J, opts, "maximumSignificantDigits", 1, 21, &s->nf.max_sig) < 0) return JV_EXC;
    if (s->nf.max_sig < 0 && s->nf.min_sig >= 0) s->nf.max_sig = 21;
    if (s->nf.min_sig < 0) s->nf.min_sig = 1;
    if (s->nf.min_sig > s->nf.max_sig) s->nf.min_sig = s->nf.max_sig;
    s->nf.nu = (unsigned char)nu_of(loc, (unsigned)li);
    return jv_from_obj((struct obj*)s);
}

static jv pr_select(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_PR, "Intl.PluralRules.prototype.select");
    if (!s) return JV_EXC;
    double x = 0;
    if (argc > 0 && to_number_d(J, argv[0], &x) < 0) return JV_EXC;
    double v;
    if (s->nf.max_sig >= 0) v = intl_round_number(x, 0, s->nf.max_sig);
    else v = intl_round_number(x, s->nf.min_frac, s->nf.max_frac);
    int cat = intl_plural_select(s->nf.card, v);
    return str_value(J, intl_plural_cat_name(cat));
}

static jv pr_resolved(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_PR, "Intl.PluralRules.prototype.resolvedOptions");
    if (!s) return JV_EXC;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "locale"), s->tag, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "type"), str_value(J, s->pr_type ? "ordinal" : "cardinal"), PA_DEFAULT) < 0) return JV_EXC;
    struct obj* ca = obj_new_array(J, 0);
    if (!ca) return JV_EXC;
    uint32_t n = 0;
    unsigned rs = s->nf.card;
    if (rs < intl_nrs_n) {
        for (int i = 0; i < 5; i++) {
            if (intl_rs[rs].mask & (1u << i)) {
                jv v2 = str_value(J, intl_plural_cat_name(i));
                if (obj_define_value(J, ca, PK_FROM_INDEX(n++), v2, PA_DEFAULT) < 0) return JV_EXC;
            }
        }
    }
    jv v3 = str_value(J, "other");
    if (obj_define_value(J, ca, PK_FROM_INDEX(n++), v3, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "pluralCategories"), jv_from_obj(ca), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "minimumIntegerDigits"), jv_number(s->nf.min_int), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "minimumFractionDigits"), jv_number(s->nf.min_frac), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "maximumFractionDigits"), jv_number(s->nf.max_frac), PA_DEFAULT) < 0) return JV_EXC;
    if (s->nf.max_sig >= 0) {
        if (obj_define_value(J, o, bname_key(J, "minimumSignificantDigits"), jv_number(s->nf.min_sig), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "maximumSignificantDigits"), jv_number(s->nf.max_sig), PA_DEFAULT) < 0) return JV_EXC;
    }
    if (obj_define_value(J, o, bname_key(J, "roundingMode"), str_value(J, "halfExpand"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "roundingPriority"), str_value(J, "all"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numberingSystem"), cstr_jv(J, intl_nsys[s->nf.nu].id), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- Intl.NumberFormat

// the per-locale currency entry by ISO code (packed 3 chars in code4)
static int lcur_find(unsigned li, const char* code) {
    unsigned key = ((unsigned)(unsigned char)code[0] << 24) | ((unsigned)(unsigned char)code[1] << 16) |
                   ((unsigned)(unsigned char)code[2] << 8);
    const struct intl_loc* loc = &intl_locales[li];
    for (unsigned i = 0; i < loc->cur_n; i++)
        if (intl_lcur[loc->cur_off + i].code4 == key) return (int)(loc->cur_off + i);
    return -1;
}

static int lunit_find(unsigned li, int ui) {
    const struct intl_loc* loc = &intl_locales[li];
    for (unsigned i = 0; i < loc->unit_n; i++)
        if (intl_lunit[loc->unit_off + i].ui == ui) return (int)(loc->unit_off + i);
    return -1;
}

static int ci_by_code(const char* code) {
    for (unsigned i = 0; i < intl_ncurfrac; i++)
        if (strcmp(intl_curfrac[i].code, code) == 0) return (int)i;
    return -1;
}

static int ui_by_id(const char* id) {
    for (unsigned i = 0; i < intl_nunit_n; i++)
        if (strcmp(intl_unit_id[i], id) == 0) return (int)i;
    return -1;
}

static jv nf_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    struct obj* opts = 0;
    char loc[128];
    if (resolve_locales(J, argc > 0 ? argv[0] : JV_UNDEFINED, loc, sizeof loc) < 0) return JV_EXC;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;
    int li = intl_resolve(loc, (unsigned)strlen(loc));
    if (li < 0) return throw_range(J, "no locale data");
    struct intlsvc* s = svc_create(J, J->native_new_target, nf_proto, SV_NF);
    if (!s) return JV_EXC;
    if (jv_is_exc(svc_tag(J, s, intl_locales[li].tag))) return JV_EXC;
    struct intl_nfmt* f = &s->nf;
    const struct intl_loc* dl = &intl_locales[li];
    f->pat_dec = IP(dl->dec);
    f->pat_pct = IP(dl->pct);
    f->pat_cur = IP(dl->cur);
    for (int i = 0; i < 8; i++) f->sym[i] = dl->sym[i];
    f->use_grp = 1;
    f->nu = (unsigned char)nu_of(loc, (unsigned)li);
    f->card = dl->card;
    f->comp_base = dl->comp_s;
    f->cur_code = s->curbuf;

    static const char* const STYLES[] = { "decimal", "percent", "currency", "unit" };
    int style = 0;
    if (intl_opt_enum(J, opts, "style", STYLES, 4, 0, &style) < 0) return JV_EXC;
    f->style = (unsigned char)style;

    static const char* const CDISPS[] = { "code", "symbol", "narrowSymbol", "name" };
    int cdisp = 1;
    if (intl_opt_enum(J, opts, "currencyDisplay", CDISPS, 4, 1, &cdisp) < 0) return JV_EXC;
    f->cur_disp = (unsigned char)cdisp;

    static const char* const NOTATIONS[] = { "standard", "scientific", "engineering", "compact" };
    int notation = 0;
    if (intl_opt_enum(J, opts, "notation", NOTATIONS, 4, 0, &notation) < 0) return JV_EXC;
    f->notation = (unsigned char)notation;

    static const char* const CD2[] = { "short", "long" };
    int cd2 = 0;
    if (intl_opt_enum(J, opts, "compactDisplay", CD2, 2, 0, &cd2) < 0) return JV_EXC;
    if (cd2 == 1) f->comp_base = dl->comp_l;
    f->compact_long = (unsigned char)cd2;

    static const char* const MODES[] = { "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" };
    int mode = RM_HALF_EXPAND;
    if (intl_opt_enum(J, opts, "roundingMode", MODES, 9, RM_HALF_EXPAND, &mode) < 0) return JV_EXC;
    f->mode = (unsigned char)mode;

    static const char* const SIGNS[] = { "auto", "never", "always", "exceptZero", "negative" };
    int sd = 0;
    if (intl_opt_enum(J, opts, "signDisplay", SIGNS, 5, 0, &sd) < 0) return JV_EXC;
    f->sign_disp = (unsigned char)sd;

    static const char* const GRP[] = { "always", "auto", "false", "min2", "true" };
    int grp = 1;
    if (intl_opt_enum(J, opts, "useGrouping", GRP, 5, 1, &grp) < 0) return JV_EXC;
    f->use_grp = (unsigned char)(grp == 2 ? 0 : 1);

    if (opt_num_s(J, opts, "minimumIntegerDigits", 1, 21, &f->min_int) < 0) return JV_EXC;
    if (f->min_int < 0) f->min_int = 1;
    if (opt_num_s(J, opts, "minimumFractionDigits", 0, 100, &f->min_frac) < 0) return JV_EXC;
    if (opt_num_s(J, opts, "maximumFractionDigits", 0, 100, &f->max_frac) < 0) return JV_EXC;
    if (opt_num_s(J, opts, "minimumSignificantDigits", 1, 21, &f->min_sig) < 0) return JV_EXC;
    if (opt_num_s(J, opts, "maximumSignificantDigits", 1, 21, &f->max_sig) < 0) return JV_EXC;

    if (style == 2) {
        char cur[8];
        if (intl_opt_cstr(J, opts, "currency", cur, sizeof cur) < 0) return JV_EXC;
        if (!cur[0]) { throw_type(J, "currency is required with the currency style"); return JV_EXC; }
        for (char* p = cur; *p; p++) *p = (char)(*p & ~32);
        if (strlen(cur) != 3) { throw_range(J, "invalid currency code"); return JV_EXC; }
        ojs_snprintf(s->curbuf, sizeof s->curbuf, "%s", cur);
        int ci = ci_by_code(cur);
        f->cur_ci = (short)ci;
        int digits = ci >= 0 ? intl_curfrac[ci].digits : INTL_CURFRAC_DEFAULT;
        if (f->min_frac < 0) f->min_frac = (short)digits;
        if (f->max_frac < 0) f->max_frac = (short)digits;
        {
            // the symbol table is independent of the digits table (CLDR only
            // lists currencies with non-default digits)
            int e = lcur_find((unsigned)li, cur);
            if (e >= 0) f->cur_entry = (unsigned short)e;
        }
    } else if (style == 3) {
        char unit[24];
        if (intl_opt_cstr(J, opts, "unit", unit, sizeof unit) < 0) return JV_EXC;
        if (!unit[0]) { throw_type(J, "unit is required with the unit style"); return JV_EXC; }
        for (char* p = unit; *p; p++) *p = (char)(*p | 32);
        int ui = ui_by_id(unit);
        if (ui < 0) { throw_range(J, "invalid unit: %s", unit); return JV_EXC; }
        f->unit_ui = (short)ui;
        static const char* const UDISP[] = { "long", "short", "narrow" };
        int udisp = 1;
        if (intl_opt_enum(J, opts, "unitDisplay", UDISP, 3, 1, &udisp) < 0) return JV_EXC;
        f->unit_disp = (unsigned char)udisp;
        int e = lunit_find((unsigned)li, ui);
        if (e >= 0) f->unit_entry = (unsigned short)e;
        if (f->min_frac < 0) f->min_frac = 0;
        if (f->max_frac < 0) f->max_frac = 0;
    } else if (style == 1) {
        if (f->min_frac < 0) f->min_frac = 0;
        if (f->max_frac < 0) f->max_frac = 0;
    }
    if (f->min_frac >= 0 && f->max_frac >= 0 && f->min_frac > f->max_frac) {
        throw_range(J, "maximumFractionDigits must be greater than or equal to minimumFractionDigits");
        return JV_EXC;
    }
    if (f->min_sig >= 0 && f->max_sig < 0) f->max_sig = 21;
    if (f->min_sig < 0) f->min_sig = 1;
    if (f->max_sig >= 0 && f->min_sig > f->max_sig) f->min_sig = f->max_sig;
    return jv_from_obj((struct obj*)s);
}

static jv nf_format(ojs* J, jv t, int argc, jv* argv, int magic) {
    struct intlsvc* s = this_svc(J, t, SV_NF, magic ? "Intl.NumberFormat.prototype.formatToParts" : "Intl.NumberFormat.prototype.format");
    if (!s) return JV_EXC;
    double x = 0;
    if (argc > 0 && to_number_d(J, argv[0], &x) < 0) return JV_EXC;
    return intl_nf_format(J, &s->nf, x, magic);
}

static jv nf_resolved(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_NF, "Intl.NumberFormat.prototype.resolvedOptions");
    if (!s) return JV_EXC;
    struct intl_nfmt* f = &s->nf;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    static const char* const STYLES[] = { "decimal", "percent", "currency", "unit" };
    static const char* const NOTATIONS[] = { "standard", "scientific", "engineering", "compact" };
    static const char* const CDISPS[] = { "code", "symbol", "narrowSymbol", "name" };
    static const char* const SIGNS[] = { "auto", "never", "always", "exceptZero", "negative" };
    static const char* const MODES[] = { "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" };
    if (obj_define_value(J, o, bname_key(J, "locale"), s->tag, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numberingSystem"), cstr_jv(J, intl_nsys[f->nu].id), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "style"), str_value(J, STYLES[f->style]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "notation"), str_value(J, NOTATIONS[f->notation]), PA_DEFAULT) < 0) return JV_EXC;
    if (f->style == 2) {
        if (obj_define_value(J, o, bname_key(J, "currency"), cstr_jv(J, s->curbuf), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "currencyDisplay"), str_value(J, CDISPS[f->cur_disp]), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "currencySign"), str_value(J, "standard"), PA_DEFAULT) < 0) return JV_EXC;
    } else if (f->style == 3 && f->unit_ui >= 0) {
        if (obj_define_value(J, o, bname_key(J, "unit"), cstr_jv(J, intl_unit_id[f->unit_ui]), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "unitDisplay"), str_value(J, "long"), PA_DEFAULT) < 0) return JV_EXC;
    }
    if (f->notation == 3) {
        if (obj_define_value(J, o, bname_key(J, "compactDisplay"), str_value(J, f->compact_long ? "long" : "short"), PA_DEFAULT) < 0) return JV_EXC;
    }
    if (obj_define_value(J, o, bname_key(J, "minimumIntegerDigits"), jv_number(f->min_int), PA_DEFAULT) < 0) return JV_EXC;
    if (f->max_sig >= 0) {
        if (obj_define_value(J, o, bname_key(J, "minimumSignificantDigits"), jv_number(f->min_sig), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "maximumSignificantDigits"), jv_number(f->max_sig), PA_DEFAULT) < 0) return JV_EXC;
    } else {
        if (obj_define_value(J, o, bname_key(J, "minimumFractionDigits"), jv_number(f->min_frac < 0 ? 0 : f->min_frac), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "maximumFractionDigits"), jv_number(f->max_frac < 0 ? 3 : f->max_frac), PA_DEFAULT) < 0) return JV_EXC;
    }
    if (obj_define_value(J, o, bname_key(J, "roundingMode"), str_value(J, MODES[f->mode]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "roundingPriority"), str_value(J, "all"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "trailingZeroDisplay"), str_value(J, "auto"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "useGrouping"), jv_bool(f->use_grp == 1), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "signDisplay"), str_value(J, SIGNS[f->sign_disp]), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- Intl.DateTimeFormat

static int comp_code(ojs* J, struct obj* opts, const char* name, unsigned char* out) {
    static const char* const VALS[] = { "narrow", "short", "long", "numeric", "2-digit" };
    jv v;
    if (intl_opt_str(J, opts, name, &v) < 0) return -1;
    *out = 0;
    if (jv_is_undef(v)) return 0;
    jv s = to_string(J, v);
    if (jv_is_exc(s)) return -1;
    struct str* str = jv_str(s);
    char buf[12];
    size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
    buf[bl] = 0;
    for (int i = 0; i < 5; i++)
        if (strcmp(buf, VALS[i]) == 0) {
            *out = (unsigned char)(i == 0 ? 5 : i == 1 ? 4 : i == 2 ? 3 : i == 3 ? 1 : 2);
            return 0;
        }
    throw_range(J, "invalid value for %s", name);
    return -1;
}

static jv dt_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    struct obj* opts = 0;
    char loc[128];
    if (resolve_locales(J, argc > 0 ? argv[0] : JV_UNDEFINED, loc, sizeof loc) < 0) return JV_EXC;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;
    int li = intl_resolve(loc, (unsigned)strlen(loc));
    if (li < 0) return throw_range(J, "no locale data");
    struct intlsvc* s = svc_create(J, J->native_new_target, dt_proto, SV_DT);
    if (!s) return JV_EXC;
    if (jv_is_exc(svc_tag(J, s, intl_locales[li].tag))) return JV_EXC;
    static const char* const DSTYLES[] = { "full", "long", "medium", "short" };
    if (opt_enum_c(J, opts, "dateStyle", DSTYLES, 4, -1, &s->date_style) < 0) return JV_EXC;
    if (opt_enum_c(J, opts, "timeStyle", DSTYLES, 4, -1, &s->time_style) < 0) return JV_EXC;

    struct intl_dtcomp* c = &s->comp;
    if (s->date_style < 0 && s->time_style < 0) {
        if (comp_code(J, opts, "weekday", &c->weekday) < 0) return JV_EXC;
        if (comp_code(J, opts, "era", &c->era) < 0) return JV_EXC;
        if (comp_code(J, opts, "year", &c->year) < 0) return JV_EXC;
        if (comp_code(J, opts, "month", &c->month) < 0) return JV_EXC;
        if (comp_code(J, opts, "day", &c->day) < 0) return JV_EXC;
        if (comp_code(J, opts, "dayPeriod", &c->dayperiod) < 0) return JV_EXC;
        if (comp_code(J, opts, "hour", &c->hour) < 0) return JV_EXC;
        if (comp_code(J, opts, "minute", &c->minute) < 0) return JV_EXC;
        if (comp_code(J, opts, "second", &c->second) < 0) return JV_EXC;
        short frac = 0;
        if (opt_num_s(J, opts, "fractionalSecondDigits", 1, 3, &frac) < 0) return JV_EXC;
        c->frac = (unsigned char)(frac > 0 ? frac : 0);
        static const char* const TZN[] = { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric" };
        int tz = -1;
        if (intl_opt_enum(J, opts, "timeZoneName", TZN, 6, -1, &tz) < 0) return JV_EXC;
        c->tzname = (unsigned char)(tz >= 0 ? tz + 1 : 0);   // 0 = absent, "short" = 1
        int hc = hc_resolve(J, opts, (unsigned)li, 0);
        if (hc < 0) return JV_EXC;
        c->hc = (unsigned char)hc;
    } else {
        int hc = hc_resolve(J, opts, (unsigned)li, 0);
        if (hc < 0) return JV_EXC;
        c->hc = (unsigned char)hc;
    }
    // time zone: only UTC and the process zone (the kernel runs in UTC)
    jv tzv;
    if (intl_opt_str(J, opts, "timeZone", &tzv) < 0) return JV_EXC;
    int tz_local = 1;
    if (!jv_is_undef(tzv)) {
        struct str* str = jv_str(tzv);
        char buf[48];
        size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
        buf[bl] = 0;
        for (char* p = buf; *p; p++) *p = (char)(*p | 32);
        if (strcmp(buf, "utc") == 0 || strcmp(buf, "etc/utc") == 0 || strcmp(buf, "gmt") == 0 ||
            strcmp(buf, "etc/gmt") == 0 || strcmp(buf, "z") == 0) {
            tz_local = 0;
        } else {
            throw_range(J, "unsupported time zone: %S", jv_str(tzv));
            return JV_EXC;
        }
    }
    char nuopt[24];
    if (intl_opt_cstr(J, opts, "numberingSystem", nuopt, sizeof nuopt) < 0) return JV_EXC;
    int nu = nuopt[0] ? nu_by_id(nuopt) : -1;
    if (nu < 0) nu = nu_of(loc, (unsigned)li);
    s->df.li = (unsigned short)li;
    s->df.tz = (unsigned char)(tz_local ? 1 : 0);
    s->df.nu = (unsigned char)nu;
    const char* dp = 0;
    const char* tp = 0;
    const char* glue = 0;
    if (intl_dt_pick((unsigned short)li, s->date_style, s->time_style,
                     (s->date_style < 0 && s->time_style < 0) ? c : 0, &dp, &tp, &glue) < 0) {
        return JV_EXC;
    }
    s->df.date_pat = dp;
    s->df.time_pat = tp;
    s->df.glue = glue;
    // the matched pattern may lack the requested fractional seconds / zone /
    // weekday width — the formatter appends them (ICU's behavior)
    s->df.wk_len = 0;
    s->df.frac_add = 0;
    s->df.tz_add = 0;
    s->df.tz_len = 0;
    if (s->date_style < 0 && s->time_style < 0) {
        static const unsigned char WKMAP[] = { 0, 0, 0, 4, 1, 5 };   // comp code -> pattern len
        s->df.wk_len = c->weekday ? WKMAP[c->weekday] : 0;
        if (c->frac) {
            int has_s = 0;
            for (const char* t = tp; t && *t; t++) if (*t == 'S') { has_s = 1; break; }
            if (!has_s) s->df.frac_add = c->frac;
        }
        if (c->tzname) {
            int has_z = 0;
            for (const char* t = tp; t && *t; t++) if (*t == 'z') { has_z = 1; break; }
            if (!has_z) {
                s->df.tz_add = 1;
                s->df.tz_len = (c->tzname == 2 || c->tzname == 4 || c->tzname == 6) ? 4 : 1;
            }
        }
    }
    return jv_from_obj((struct obj*)s);
}

static jv dt_format(ojs* J, jv t, int argc, jv* argv, int magic) {
    struct intlsvc* s = this_svc(J, t, SV_DT, magic ? "Intl.DateTimeFormat.prototype.formatToParts" : "Intl.DateTimeFormat.prototype.format");
    if (!s) return JV_EXC;
    double x;
    if (argc == 0 || jv_is_undef(argv[0])) x = km_floor(ojs_sys_time_ms());
    else if (dt_time_arg(J, argv[0], &x) < 0) return JV_EXC;
    return intl_df_format(J, &s->df, x, magic);
}

static jv dt_resolved(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_DT, "Intl.DateTimeFormat.prototype.resolvedOptions");
    if (!s) return JV_EXC;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    struct intl_dtcomp* c = &s->comp;
    static const char* const HC[] = { "h11", "h12", "h23", "h24" };
    if (obj_define_value(J, o, bname_key(J, "locale"), s->tag, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "calendar"), str_value(J, "gregory"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numberingSystem"), cstr_jv(J, intl_nsys[s->df.nu].id), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "timeZone"), str_value(J, "UTC"), PA_DEFAULT) < 0) return JV_EXC;
    if (s->date_style < 0 && s->time_style < 0) {
        if (obj_define_value(J, o, bname_key(J, "hourCycle"), str_value(J, HC[c->hc]), PA_DEFAULT) < 0) return JV_EXC;
        if (obj_define_value(J, o, bname_key(J, "hour12"), jv_bool(c->hc == 0 || c->hc == 1), PA_DEFAULT) < 0) return JV_EXC;
    }
    struct { const char* name; unsigned char v; } FLD[9] = {
        { "weekday", c->weekday }, { "era", c->era }, { "year", c->year },
        { "month", c->month }, { "day", c->day }, { "dayPeriod", c->dayperiod },
        { "hour", c->hour }, { "minute", c->minute }, { "second", c->second },
    };
    static const char* const CODES[] = { "", "numeric", "2-digit", "long", "short", "narrow" };
    if (s->date_style < 0 && s->time_style < 0) {
        for (unsigned i = 0; i < 9; i++) {
            if (!FLD[i].v) continue;
            if (obj_define_value(J, o, bname_key(J, FLD[i].name), str_value(J, CODES[FLD[i].v]), PA_DEFAULT) < 0) return JV_EXC;
        }
        if (c->frac && obj_define_value(J, o, bname_key(J, "fractionalSecondDigits"), jv_number(c->frac), PA_DEFAULT) < 0) return JV_EXC;
        if (c->tzname) {
            static const char* const TZN[] = { "", "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric" };
            if (obj_define_value(J, o, bname_key(J, "timeZoneName"), str_value(J, TZN[c->tzname]), PA_DEFAULT) < 0) return JV_EXC;
        }
    }
    if (s->date_style >= 0) {
        static const char* const ST[] = { "full", "long", "medium", "short" };
        if (obj_define_value(J, o, bname_key(J, "dateStyle"), str_value(J, ST[s->date_style]), PA_DEFAULT) < 0) return JV_EXC;
    }
    if (s->time_style >= 0) {
        static const char* const ST[] = { "full", "long", "medium", "short" };
        if (obj_define_value(J, o, bname_key(J, "timeStyle"), str_value(J, ST[s->time_style]), PA_DEFAULT) < 0) return JV_EXC;
    }
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- Intl.ListFormat

static jv lf_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    struct obj* opts = 0;
    char loc[128];
    if (resolve_locales(J, argc > 0 ? argv[0] : JV_UNDEFINED, loc, sizeof loc) < 0) return JV_EXC;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;
    int li = intl_resolve(loc, (unsigned)strlen(loc));
    if (li < 0) return throw_range(J, "no locale data");
    if (intl_locales[li].lp == 0xFFFF) return throw_range(J, "list patterns unavailable for this locale");
    struct intlsvc* s = svc_create(J, J->native_new_target, lf_proto, SV_LF);
    if (!s) return JV_EXC;
    if (jv_is_exc(svc_tag(J, s, intl_locales[li].tag))) return JV_EXC;
    static const char* const TYPES[] = { "conjunction", "disjunction", "unit" };
    int ty = 0;
    if (intl_opt_enum(J, opts, "type", TYPES, 3, 0, &ty) < 0) return JV_EXC;
    static const char* const STYLES[] = { "long", "short", "narrow" };
    int st = 0;
    if (intl_opt_enum(J, opts, "style", STYLES, 3, 0, &st) < 0) return JV_EXC;
    s->lf_type = (unsigned char)ty;
    s->lf_style = (unsigned char)st;
    s->df.li = (unsigned short)li;
    return jv_from_obj((struct obj*)s);
}

// the four list patterns of a group: intl_lp holds one pool offset per
// group (5 per locale: standard, or, unit, unit-short, unit-narrow), each
// pointing at a start/middle/end/pair run
static void lf_group(const struct intlsvc* s, const char** start, const char** middle,
                     const char** end, const char** pair) {
    const struct intl_loc* loc = &intl_locales[s->df.li];
    int gidx = s->lf_type + (s->lf_type == 2 ? s->lf_style : 0);
    const char* p = IP(intl_lp[loc->lp + gidx]);
    *start = p;
    *middle = p + strlen(p) + 1;
    *end = *middle + strlen(*middle) + 1;
    *pair = *end + strlen(*end) + 1;
}

// ListFormat emitter: builds the string always and the parts array when asked
struct lfem {
    ojs* J;
    struct sbuf sb;
    struct obj* parts;      // 0 in string mode
    uint32_t n;
    int err;
};

static void lfem_lit(struct lfem* e, const char* s, size_t n) {
    sb_put_utf8(&e->sb, s, n);
    if (e->parts && n) {
        struct obj* o = obj_new_plain(e->J);
        if (!o) { e->err = 1; return; }
        struct str* sv = str_from_utf8(e->J, s, n);
        if (!sv || obj_define_value(e->J, o, bname_key(e->J, "type"), str_value(e->J, "literal"), PA_DEFAULT) < 0 ||
            obj_define_value(e->J, o, bname_key(e->J, "value"), jv_from_str(sv), PA_DEFAULT) < 0 ||
            obj_define_value(e->J, e->parts, PK_FROM_INDEX(e->n++), jv_from_obj(o), PA_DEFAULT) < 0) {
            e->err = 1;
        }
    }
}

static void lfem_el(struct lfem* e, const struct str* s) {
    sb_put_str(&e->sb, s);
    if (e->parts) {
        struct obj* o = obj_new_plain(e->J);
        if (!o) { e->err = 1; return; }
        if (obj_define_value(e->J, o, bname_key(e->J, "type"), str_value(e->J, "element"), PA_DEFAULT) < 0 ||
            obj_define_value(e->J, o, bname_key(e->J, "value"), jv_from_str((struct str*)s), PA_DEFAULT) < 0 ||
            obj_define_value(e->J, e->parts, PK_FROM_INDEX(e->n++), jv_from_obj(o), PA_DEFAULT) < 0) {
            e->err = 1;
        }
    }
}

// one join pattern: literals around {0} and {1}; e1 undefined when the
// pattern carries only {0} (the second element is emitted by the caller)
static void lfem_join(struct lfem* e, const char* pat, const struct str* e0, const struct str* e1) {
    const char* h0 = strstr(pat, "{0}");
    if (!h0) {
        lfem_lit(e, pat, strlen(pat));
        return;
    }
    lfem_lit(e, pat, (size_t)(h0 - pat));
    if (e0) lfem_el(e, e0);
    const char* h1 = e1 ? strstr(h0 + 3, "{1}") : 0;
    if (h1) {
        if (h1 > h0 + 3) lfem_lit(e, h0 + 3, (size_t)(h1 - (h0 + 3)));
        lfem_el(e, e1);
        if (h1[3]) lfem_lit(e, h1 + 3, strlen(h1 + 3));
    } else if (h0[3]) {
        lfem_lit(e, h0 + 3, strlen(h0 + 3));
    }
}

static jv lf_format(ojs* J, jv t, int argc, jv* argv, int magic) {
    struct intlsvc* s = this_svc(J, t, SV_LF, magic ? "Intl.ListFormat.prototype.formatToParts" : "Intl.ListFormat.prototype.format");
    if (!s) return JV_EXC;
    jv items = iter_to_list(J, argc > 0 ? argv[0] : JV_UNDEFINED);
    if (jv_is_exc(items)) return JV_EXC;
    struct obj* arr = jv_obj(items);
    uint32_t n = arr->alen;
    struct lfem e;
    e.J = J;
    sb_init(J, &e.sb);
    e.parts = 0;
    e.n = 0;
    e.err = 0;
    if (magic) {
        struct obj* p = obj_new_array(J, 0);
        if (!p) return JV_EXC;
        e.parts = p;
    }
    const char* lstart, *lmiddle, *lend, *lpair;
    lf_group(s, &lstart, &lmiddle, &lend, &lpair);
    if (n == 1) {
        jv e0 = obj_get(J, arr, PK_FROM_INDEX(0), jv_from_obj(arr));
        if (jv_is_exc(e0)) return JV_EXC;
        jv s0 = to_string(J, e0);
        if (jv_is_exc(s0)) return JV_EXC;
        lfem_el(&e, jv_str(s0));
    } else if (n >= 2) {
        jv e0 = obj_get(J, arr, PK_FROM_INDEX(0), jv_from_obj(arr));
        if (jv_is_exc(e0)) return JV_EXC;
        jv s0 = to_string(J, e0);
        if (jv_is_exc(s0)) return JV_EXC;
        jv el = obj_get(J, arr, PK_FROM_INDEX(n - 1), jv_from_obj(arr));
        if (jv_is_exc(el)) return JV_EXC;
        jv sl = to_string(J, el);
        if (jv_is_exc(sl)) return JV_EXC;
        if (n == 2) {
            lfem_join(&e, lpair, jv_str(s0), jv_str(sl));   // pair
        } else {
            // [start-before-{0}] el0 [start-between] el1 [middle-between] ...
            // elN-2 [end-between] elLast [end-after-{1}]
            const char* start = lstart;
            const char* h0 = strstr(start, "{0}");
            const char* h1 = h0 ? strstr(h0 + 3, "{1}") : 0;
            if (h0) {
                lfem_lit(&e, start, (size_t)(h0 - start));
                lfem_el(&e, jv_str(s0));
                if (h1) lfem_lit(&e, h0 + 3, (size_t)(h1 - (h0 + 3)));
                else if (h0[3]) lfem_lit(&e, h0 + 3, strlen(h0 + 3));
            }
            const char* mid = lmiddle;
            const char* m0 = strstr(mid, "{0}");
            const char* m1 = m0 ? strstr(m0 + 3, "{1}") : 0;
            const char* endp = lend;
            const char* e0p = strstr(endp, "{0}");
            const char* e1p = e0p ? strstr(e0p + 3, "{1}") : 0;
            for (uint32_t i = 1; i < n - 1; i++) {
                jv em = obj_get(J, arr, PK_FROM_INDEX(i), jv_from_obj(arr));
                if (jv_is_exc(em)) return JV_EXC;
                jv sm = to_string(J, em);
                if (jv_is_exc(sm)) return JV_EXC;
                lfem_el(&e, jv_str(sm));
                if (i + 1 < n - 1) {
                    if (m1) lfem_lit(&e, m0 + 3, (size_t)(m1 - (m0 + 3)));
                    else if (m0 && m0[3]) lfem_lit(&e, m0 + 3, strlen(m0 + 3));
                } else {
                    if (e1p) lfem_lit(&e, e0p + 3, (size_t)(e1p - (e0p + 3)));
                    else if (e0p && e0p[3]) lfem_lit(&e, e0p + 3, strlen(e0p + 3));
                }
            }
            lfem_el(&e, jv_str(sl));
            if (e1p && e1p[3]) lfem_lit(&e, e1p + 3, strlen(e1p + 3));
        }
    }
    if (e.err) return JV_EXC;
    if (e.parts) {
        jv strv = sb_done(&e.sb);
        if (jv_is_exc(strv)) return JV_EXC;
        // CLDR list patterns carry no trailing {1} for "start"/"middle": the
        // last join already appended the final element
        return jv_from_obj(e.parts);
    }
    return sb_done(&e.sb);
}

static jv lf_resolved(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_LF, "Intl.ListFormat.prototype.resolvedOptions");
    if (!s) return JV_EXC;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    static const char* const TYPES[] = { "conjunction", "disjunction", "unit" };
    static const char* const STYLES[] = { "long", "short", "narrow" };
    if (obj_define_value(J, o, bname_key(J, "locale"), s->tag, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "type"), str_value(J, TYPES[s->lf_type]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "style"), str_value(J, STYLES[s->lf_style]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numeric"), jv_bool(1), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- Intl.RelativeTimeFormat

static const char* const RTF_UNITS[] = { "year", "quarter", "month", "week", "day", "hour", "minute", "second" };

static jv rtf_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    struct obj* opts = 0;
    char loc[128];
    if (resolve_locales(J, argc > 0 ? argv[0] : JV_UNDEFINED, loc, sizeof loc) < 0) return JV_EXC;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;
    int li = intl_resolve(loc, (unsigned)strlen(loc));
    if (li < 0) return throw_range(J, "no locale data");
    if (intl_locales[li].fld == 0xFFFF) return throw_range(J, "relative time data unavailable for this locale");
    struct intlsvc* s = svc_create(J, J->native_new_target, rtf_proto, SV_RTF);
    if (!s) return JV_EXC;
    if (jv_is_exc(svc_tag(J, s, intl_locales[li].tag))) return JV_EXC;
    s->df.li = (unsigned short)li;
    static const char* const STYLES[] = { "long", "short", "narrow" };
    int st = 0;
    if (intl_opt_enum(J, opts, "style", STYLES, 3, 0, &st) < 0) return JV_EXC;
    s->rtf_width = (unsigned char)st;
    static const char* const NUMS[] = { "always", "auto" };
    int nm = 0;
    if (intl_opt_enum(J, opts, "numeric", NUMS, 2, 0, &nm) < 0) return JV_EXC;
    s->rtf_numeric = (unsigned char)nm;   // 0 = always
    return jv_from_obj((struct obj*)s);
}

static jv rtf_format(ojs* J, jv t, int argc, jv* argv, int magic) {
    struct intlsvc* s = this_svc(J, t, SV_RTF, magic ? "Intl.RelativeTimeFormat.prototype.formatToParts" : "Intl.RelativeTimeFormat.prototype.format");
    if (!s) return JV_EXC;
    double x = 0;
    if (argc > 0 && to_number_d(J, argv[0], &x) < 0) return JV_EXC;
    if (argc < 2) { throw_type(J, "unit is required"); return JV_EXC; }
    jv us = to_string(J, argv[1]);
    if (jv_is_exc(us)) return JV_EXC;
    char unit[16];
    size_t bl = str_to_utf8_buf(jv_str(us), unit, sizeof unit - 1);
    unit[bl] = 0;
    for (char* p = unit; *p; p++) *p = (char)(*p | 32);
    int field = -1;
    for (int i = 0; i < 8; i++) if (strcmp(unit, RTF_UNITS[i]) == 0) field = i;
    if (field < 0) { throw_range(J, "invalid unit: %S", jv_str(us)); return JV_EXC; }
    return intl_rtf_format(J, s->df.li, field, s->rtf_width, s->rtf_numeric == 0, x, magic);
}

static jv rtf_resolved(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_RTF, "Intl.RelativeTimeFormat.prototype.resolvedOptions");
    if (!s) return JV_EXC;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    static const char* const STYLES[] = { "long", "short", "narrow" };
    static const char* const NUMS[] = { "always", "auto" };
    if (obj_define_value(J, o, bname_key(J, "locale"), s->tag, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "style"), str_value(J, STYLES[s->rtf_width]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numeric"), str_value(J, NUMS[s->rtf_numeric]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numberingSystem"), cstr_jv(J, intl_nsys[intl_locales[s->df.li].nu].id), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- Intl.Collator (codepoint order; no CLDR collation tables)

static jv col_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    (void)this_v; (void)magic;
    struct obj* opts = 0;
    char loc[128];
    if (resolve_locales(J, argc > 0 ? argv[0] : JV_UNDEFINED, loc, sizeof loc) < 0) return JV_EXC;
    if (argc > 1 && intl_opts_obj(J, argv[1], &opts) < 0) return JV_EXC;
    int li = intl_resolve(loc, (unsigned)strlen(loc));
    if (li < 0) return throw_range(J, "no locale data");
    static const char* const CF[] = { "upper", "lower", "false" };
    int casefirst = 2;
    if (intl_opt_enum(J, opts, "caseFirst", CF, 3, 2, &casefirst) < 0) return JV_EXC;
    static const char* const SENS[] = { "base", "accent", "case", "variant" };
    int sens = 3;
    if (intl_opt_enum(J, opts, "sensitivity", SENS, 4, 3, &sens) < 0) return JV_EXC;
    struct intlsvc* s = svc_create(J, J->native_new_target, col_proto, SV_COLLATOR);
    if (!s) return JV_EXC;
    if (jv_is_exc(svc_tag(J, s, intl_locales[li].tag))) return JV_EXC;
    s->lf_type = (unsigned char)casefirst;
    s->lf_style = (unsigned char)sens;
    return jv_from_obj((struct obj*)s);
}

static jv col_compare(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_COLLATOR, "Intl.Collator.prototype.compare");
    if (!s) return JV_EXC;
    jv a = argc > 0 ? to_string(J, argv[0]) : JV_UNDEFINED;
    if (jv_is_exc(a)) return JV_EXC;
    jv b = argc > 1 ? to_string(J, argv[1]) : JV_UNDEFINED;
    if (jv_is_exc(b)) return JV_EXC;
    struct str* sa = jv_str(a);
    struct str* sb2 = jv_str(b);
    uint32_t la = str_len(sa), lb = str_len(sb2);
    uint32_t n = la < lb ? la : lb;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ca = str_at(sa, i), cb = str_at(sb2, i);
        if (ca != cb) return jv_from_int(ca < cb ? -1 : 1);
    }
    return jv_from_int(la == lb ? 0 : (la < lb ? -1 : 1));
}

static jv col_resolved(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    struct intlsvc* s = this_svc(J, t, SV_COLLATOR, "Intl.Collator.prototype.resolvedOptions");
    if (!s) return JV_EXC;
    struct obj* o = obj_new_plain(J);
    if (!o) return JV_EXC;
    static const char* const CF[] = { "upper", "lower", "false" };
    static const char* const SENS[] = { "base", "accent", "case", "variant" };
    if (obj_define_value(J, o, bname_key(J, "locale"), s->tag, PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "usage"), str_value(J, "sort"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "sensitivity"), str_value(J, SENS[s->lf_style]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "ignorePunctuation"), jv_bool(0), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "caseFirst"), str_value(J, CF[s->lf_type]), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "collation"), str_value(J, "default"), PA_DEFAULT) < 0) return JV_EXC;
    if (obj_define_value(J, o, bname_key(J, "numeric"), jv_bool(0), PA_DEFAULT) < 0) return JV_EXC;
    return jv_from_obj(o);
}

// ---------------------------------------------------------------- toLocale* prototype methods

static jv number_to_locale_string(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)magic;
    double x;
    jv prim = to_number(J, t);
    if (jv_is_exc(prim)) return JV_EXC;
    if (to_number_d(J, prim, &x) < 0) return JV_EXC;
    jv made = nf_ctor(J, t, argc, argv, 0);
    if (jv_is_exc(made)) return JV_EXC;
    return intl_nf_format(J, &svc_of(made)->nf, x, 0);
}

// BigInt.prototype.toLocaleString: the digits go through the exact decimal
// conversion (nearest double; full arbitrary precision is a follow-up)
static jv bigint_to_locale_string(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)magic;
    if (!jv_is_big(t)) { throw_type(J, "toLocaleString called on incompatible receiver"); return JV_EXC; }
    jv s = to_string(J, t);
    if (jv_is_exc(s)) return JV_EXC;
    struct str* str = jv_str(s);
    char buf[80];
    size_t bl = str_to_utf8_buf(str, buf, sizeof buf - 1);
    buf[bl] = 0;
    int neg = buf[0] == '-';
    const char* dg = neg ? buf + 1 : buf;
    int nd = 0;
    while (dg[nd] >= '0' && dg[nd] <= '9') nd++;
    double x = num_from_decimal(dg, nd, 0);
    if (neg) x = -x;
    jv made = nf_ctor(J, t, argc, argv, 0);
    if (jv_is_exc(made)) return JV_EXC;
    return intl_nf_format(J, &svc_of(made)->nf, x, 0);
}

// Array.prototype.toLocaleString: the elements' toLocaleString, ","-joined
static jv array_to_locale_string(ojs* J, jv t, int argc, jv* argv, int magic) {
    (void)magic;
    if (!jv_is_obj(t) || obj_class(jv_obj(t)) != OC_ARRAY) {
        throw_type(J, "toLocaleString called on incompatible receiver");
        return JV_EXC;
    }
    struct obj* arr = jv_obj(t);
    struct sbuf sb;
    sb_init(J, &sb);
    jv sep = str_value(J, ",");
    for (uint32_t i = 0; i < arr->alen; i++) {
        if (i) {
            sb_put_str(&sb, jv_str(sep));
        }
        jv ev = obj_get(J, arr, PK_FROM_INDEX(i), jv_from_obj(arr));
        if (jv_is_exc(ev)) return JV_EXC;
        if (jv_is_undef(ev) || jv_is_nullish(ev) || ev == JV_HOLE) continue;
        jv meth;
        if (jv_is_obj(ev)) {
            meth = obj_get(J, jv_obj(ev), bname_key(J, "toLocaleString"), ev);
        } else if (jv_is_number(ev)) {
            meth = obj_get(J, J->I.number_proto, bname_key(J, "toLocaleString"), ev);
        } else if (jv_is_big(ev)) {
            meth = obj_get(J, J->I.bigint_proto, bname_key(J, "toLocaleString"), ev);
        } else {
            jv wrapped = wrap_primitive(J, ev);
            if (jv_is_exc(wrapped)) return JV_EXC;
            meth = obj_get(J, jv_obj(wrapped), bname_key(J, "toLocaleString"), ev);
        }
        if (jv_is_exc(meth)) return JV_EXC;
        if (!jv_is_obj(meth)) {
            throw_type(J, "element toLocaleString is not a function");
            return JV_EXC;
        }
        jv args[2] = { argc > 0 ? argv[0] : JV_UNDEFINED, argc > 1 ? argv[1] : JV_UNDEFINED };
        jv r = ojs_call_v(J, meth, ev, argc > 0 ? 2 : 0, args);
        if (jv_is_exc(r)) return JV_EXC;
        jv s = to_string(J, r);
        if (jv_is_exc(s)) return JV_EXC;
        sb_put_str(&sb, jv_str(s));
    }
    return sb_done(&sb);
}

// the time value of a format input (Date -> valueOf, anything else ToNumber)
static int dt_time_arg(ojs* J, jv v, double* out) {
    if (jv_is_obj(v) && obj_class(jv_obj(v)) == OC_DATE) {
        jv pv = obj_get(J, jv_obj(v), bname_key(J, "valueOf"), v);
        if (jv_is_exc(pv)) return -1;
        jv nv = ojs_call_v(J, pv, v, 0, 0);
        if (jv_is_exc(nv)) return -1;
        return to_number_d(J, nv, out);
    }
    return to_number_d(J, v, out);
}

static jv date_to_locale(ojs* J, jv t, int argc, jv* argv, int magic) {
    // magic: 0 = date + time, 1 = date only, 2 = time only
    double x;
    if (dt_time_arg(J, t, &x) < 0) return JV_EXC;
    struct obj* merged = obj_new_plain(J);
    if (!merged) return JV_EXC;
    if (argc > 1 && jv_is_obj(argv[1])) {
        static const char* const KEYS[] = { "localeMatcher", "calendar", "numberingSystem", "timeZone", "hour12", "hourCycle" };
        for (unsigned k = 0; k < 6; k++) {
            jv v2 = obj_get(J, jv_obj(argv[1]), bname_key(J, KEYS[k]), argv[1]);
            if (jv_is_exc(v2)) return JV_EXC;
            if (!jv_is_undef(v2) && obj_define_value(J, merged, bname_key(J, KEYS[k]), v2, PA_DEFAULT) < 0) return JV_EXC;
        }
    }
    if (obj_define_value(J, merged, bname_key(J, magic == 2 ? "timeStyle" : "dateStyle"), str_value(J, "long"), PA_DEFAULT) < 0) return JV_EXC;
    if (magic == 0 && obj_define_value(J, merged, bname_key(J, "timeStyle"), str_value(J, "long"), PA_DEFAULT) < 0) return JV_EXC;
    jv margs[2] = { argc > 0 ? argv[0] : JV_UNDEFINED, jv_from_obj(merged) };
    jv made = dt_ctor(J, t, 2, margs, 0);
    if (jv_is_exc(made)) return JV_EXC;
    jv dargs[1] = { jv_number(x) };
    return dt_format(J, made, 1, dargs, 0);
}

// ---------------------------------------------------------------- init

static const struct bdef loc_proto_defs[] = {
    GETTER("baseName", loc_base_name, 0),
    GETTER("language", loc_simple_get, 0),
    GETTER("script", loc_simple_get, 1),
    GETTER("region", loc_simple_get, 2),
    GETTER("variants", loc_simple_get, 3),
    GETTER("calendar", loc_kw_get, 0),
    GETTER("caseFirst", loc_kw_get, 1),
    GETTER("collation", loc_kw_get, 2),
    GETTER("numberingSystem", loc_kw_get, 3),
    GETTER("hourCycle", loc_kw_get, 4),
    GETTER("hour12", loc_kw_get, 5),
    GETTER("numeric", loc_kw_get, 6),
    FN("toString", loc_to_string, 0, 0),
    FN("maximize", loc_maximize, 0, 0),
    FN("minimize", loc_minimize, 0, 0),
};

static const struct bdef pr_proto_defs[] = {
    FN("select", pr_select, 1, 0),
    FN("resolvedOptions", pr_resolved, 0, 0),
};

static const struct bdef nf_proto_defs[] = {
    FN("format", nf_format, 1, 0),
    FN("formatToParts", nf_format, 1, 1),
    FN("resolvedOptions", nf_resolved, 0, 0),
};

static const struct bdef dt_proto_defs[] = {
    FN("format", dt_format, 1, 0),
    FN("formatToParts", dt_format, 1, 1),
    FN("resolvedOptions", dt_resolved, 0, 0),
};

static const struct bdef lf_proto_defs[] = {
    FN("format", lf_format, 1, 0),
    FN("formatToParts", lf_format, 1, 1),
    FN("resolvedOptions", lf_resolved, 0, 0),
};

static const struct bdef rtf_proto_defs[] = {
    FN("format", rtf_format, 2, 0),
    FN("formatToParts", rtf_format, 2, 1),
    FN("resolvedOptions", rtf_resolved, 0, 0),
};

static const struct bdef col_proto_defs[] = {
    FN("compare", col_compare, 2, 0),
    FN("resolvedOptions", col_resolved, 0, 0),
};

int b_intl_init(ojs* J) {
    struct obj* intl = obj_new_plain(J);
    if (!intl) return -1;
    if (def_value(J, intl, "@@toStringTag", str_value(J, "Intl"), PA_CONFIGURABLE) < 0) return -1;
    if (def_global(J, "Intl", jv_from_obj(intl)) < 0) return -1;

    struct obj* c;
    #define ADD_CTOR(name, ctorfn, proto, field) do { \
        proto = obj_new(J, J->I.object_proto, OC_OBJECT, 0); \
        if (!proto) return -1; \
        c = new_native_ctor(J, ctorfn, name, 0, 0, proto); \
        if (!c) return -1; \
        if (def_value(J, intl, name + 5, jv_from_obj(c), PA_DEFAULT) < 0) return -1; \
    } while (0)

    ADD_CTOR("Intl.Locale", locale_ctor, loc_proto, loc);
    if (DEF_FNS(loc_proto, loc_proto_defs) < 0) return -1;
    if (def_value(J, loc_proto, "@@toStringTag", str_value(J, "Intl.Locale"), PA_CONFIGURABLE) < 0) return -1;

    ADD_CTOR("Intl.PluralRules", pr_ctor, pr_proto, pr);
    if (DEF_FNS(pr_proto, pr_proto_defs) < 0) return -1;
    if (def_value(J, pr_proto, "@@toStringTag", str_value(J, "Intl.PluralRules"), PA_CONFIGURABLE) < 0) return -1;

    ADD_CTOR("Intl.NumberFormat", nf_ctor, nf_proto, nf);
    if (DEF_FNS(nf_proto, nf_proto_defs) < 0) return -1;
    if (def_value(J, nf_proto, "@@toStringTag", str_value(J, "Intl.NumberFormat"), PA_CONFIGURABLE) < 0) return -1;

    ADD_CTOR("Intl.DateTimeFormat", dt_ctor, dt_proto, dt);
    if (DEF_FNS(dt_proto, dt_proto_defs) < 0) return -1;
    if (def_value(J, dt_proto, "@@toStringTag", str_value(J, "Intl.DateTimeFormat"), PA_CONFIGURABLE) < 0) return -1;

    ADD_CTOR("Intl.ListFormat", lf_ctor, lf_proto, lf);
    if (DEF_FNS(lf_proto, lf_proto_defs) < 0) return -1;
    if (def_value(J, lf_proto, "@@toStringTag", str_value(J, "Intl.ListFormat"), PA_CONFIGURABLE) < 0) return -1;

    ADD_CTOR("Intl.RelativeTimeFormat", rtf_ctor, rtf_proto, rtf);
    if (DEF_FNS(rtf_proto, rtf_proto_defs) < 0) return -1;
    if (def_value(J, rtf_proto, "@@toStringTag", str_value(J, "Intl.RelativeTimeFormat"), PA_CONFIGURABLE) < 0) return -1;

    ADD_CTOR("Intl.Collator", col_ctor, col_proto, col);
    if (DEF_FNS(col_proto, col_proto_defs) < 0) return -1;
    if (def_value(J, col_proto, "@@toStringTag", str_value(J, "Intl.Collator"), PA_CONFIGURABLE) < 0) return -1;
    #undef ADD_CTOR

    if (def_value(J, intl, "getCanonicalLocales", jv_from_obj(new_native(J, intl_get_canonical_locales, "getCanonicalLocales", 1, 0)), PA_DEFAULT) < 0) return -1;
    if (def_value(J, intl, "supportedValuesOf", jv_from_obj(new_native(J, intl_supported_values, "supportedValuesOf", 1, 0)), PA_DEFAULT) < 0) return -1;

    // the toLocale* methods route through Intl
    {
        static const struct bdef np_defs[] = {
            FN("toLocaleString", number_to_locale_string, 0, 0),
        };
        if (DEF_FNS(J->I.number_proto, np_defs) < 0) return -1;
    }
    {
        static const struct bdef dp_defs[] = {
            FN("toLocaleString", date_to_locale, 0, 0),
            FN("toLocaleDateString", date_to_locale, 0, 1),
            FN("toLocaleTimeString", date_to_locale, 0, 2),
        };
        if (DEF_FNS(J->I.date_proto, dp_defs) < 0) return -1;
    }
    {
        static const struct bdef bp_defs[] = {
            FN("toLocaleString", bigint_to_locale_string, 0, 0),
        };
        if (DEF_FNS(J->I.bigint_proto, bp_defs) < 0) return -1;
    }
    {
        static const struct bdef ap_defs[] = {
            FN("toLocaleString", array_to_locale_string, 0, 0),
        };
        if (DEF_FNS(J->I.array_proto, ap_defs) < 0) return -1;
    }
    return 0;
}

void intl_classes(void) {
    class_ops[OC_INTL].trace = intl_trace;
}
