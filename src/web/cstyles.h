// Predefined CSS counter styles (see cstyles.c / cstyles_data.h) plus the
// dynamic @counter-style rules parsed from a stylesheet (wsheet->dyns).
#ifndef CSTYLES_H
#define CSTYLES_H

// ---- predefined ------------------------------------------------------------

int cstyle_find(const char* s, int len);
int cstyle_format(int idx, int value, char* out);

// ---- dynamic (@counter-style) ----------------------------------------------

// systems (shared by the predefined table and @counter-style rules)
enum { CS_NUMERIC, CS_ALPHABETIC, CS_SYMBOLIC, CS_ADDITIVE, CS_FIXED, CS_HEBREW,
       CS_ETHIOPIC, CS_CJK, CS_CYCLIC };
#define CS_DYN_EXTENDS 100   // system: extends <name>

// One dynamic style. Strings live in one NUL-terminated malloc'd block
// (pool); the *off fields are byte offsets into it. 0 is a legal offset,
// 0xFFFF marks "descriptor not given".
struct cstyle_dyn {
    char* name;                  // malloc'd style name (exact case)
    int system;                  // CS_* (cstyles_data.h) or CS_DYN_EXTENDS
    char* pool;                  // malloc'd, NUL-terminated strings
    int pool_len, pool_cap;
    uint16_t sym_off[8];         // symbols descriptor
    int nsym;
    struct { int32_t v; uint16_t off; } add[24];   // additive-symbols
    int nadd;
    uint16_t prefix_off, suffix_off, pad_off, neg_off;   // 0xFFFF unset
    int pad_n;                   // pad minimum length
    int neg_pre_len, neg_suf_len;   // negative descriptor byte lens (into neg_off)
    int rmin, rmax;              // range descriptor (defaults come from system)
    int has_range, has_pad;
    int fixed_start;             // system: fixed <start> (1 default)
    int alias_decimal;           // system: decimal — the predefined style by another name
    char* fallback;              // malloc'd fallback style name (exact case), 0 none
    char* extends_nm;            // malloc'd base name for CS_DYN_EXTENDS
};

struct cstyle_dyn* cstyle_dyn_new(const char* name, int len);
void cstyle_dyn_free(struct cstyle_dyn* d);
// append a NUL-terminated string to the style's pool, returning its offset
uint16_t cstyle_dyn_put(struct cstyle_dyn* d, const char* s, int len);

// Name lookup: returns the dynamic style with this name (exact bytes), or 0.
// `out_at` (optional) receives its index.
struct cstyle_dyn* cstyle_dyn_find(struct cstyle_dyn** list, int n, const char* s, int len, int* out_at);

// Format a dynamic style. `resolve` looks a style name up (dynamic list first,
// then the predefined table): *dyn_out = dynamic style or 0, *idx_out =
// predefined index or -1. Returns the byte length written, 0 = cannot format
// (caller falls back). `depth` guards extends chains.
int cstyle_dyn_format(const struct cstyle_dyn* d, int value, char* out, int cap,
                      void* rctx,
                      void (*resolve)(void* rctx, const char* s, int len,
                                      const struct cstyle_dyn** dyn_out, int* idx_out),
                      int depth);

#endif
