// In-memory HTTP response cache, see fcache.h.
#include "fcache.h"

#ifdef FCACHE_HOST
#include <stdlib.h>
#include <string.h>
#define fc_alloc(n) malloc(n)
#define fc_free(p) free(p)
static uint32_t fc_heap_total(void) { return 256u << 20; }
#else
#include "../memory.h"
#include "../string.h"
#define fc_alloc(n) kmalloc(n)
#define fc_free(p) kfree(p)
static uint32_t fc_heap_total(void) { uint32_t t = 0, u, p, l; heap_stats(&t, &u, &p, &l); return t; }
#endif

#define FC_MAX_ENTRIES 192
#define FC_MAX_ONE (4u << 20)        // one response
#define FC_MAX_TOTAL (32u << 20)     // everything (and never more than an eighth of the heap)
#define FC_MAX_FRESH_S 86400u
#define FC_PREFETCH_S 30u

// ---- header parsing ---------------------------------------------------------------------------------------------

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int hdr_end(const char* r, uint32_t n) {
    for (uint32_t i = 0; i + 3 < n; i++)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') return (int)i + 4;
    return -1;
}

// Value of header `name` (lowercase, no colon) in the header block r[0..he): pointer + length, trimmed; NULL if absent.
static const char* hdr_value(const char* r, int he, const char* name, int* vl) {
    int nl = (int)strlen(name);
    for (int i = 0; i < he; i++) {
        if (i > 0 && r[i - 1] != '\n') continue;
        int k = 0;
        while (k < nl && i + k < he && lc(r[i + k]) == name[k]) k++;
        if (k != nl || i + k >= he || r[i + k] != ':') continue;
        int s = i + k + 1, e = s;
        while (e < he && r[e] != '\r' && r[e] != '\n') e++;
        while (s < e && (r[s] == ' ' || r[s] == '\t')) s++;
        while (e > s && (r[e - 1] == ' ' || r[e - 1] == '\t')) e--;
        *vl = e - s;
        return r + s;
    }
    return 0;
}

// Does the comma separated list v contain the token (lowercase)? If it is "name=NUMBER" style, the number goes to *num.
static int list_token(const char* v, int vl, const char* tok, int32_t* num) {
    int tl = (int)strlen(tok);
    int i = 0;
    while (i < vl) {
        while (i < vl && (v[i] == ' ' || v[i] == ',' || v[i] == '\t')) i++;
        int s = i;
        while (i < vl && v[i] != ',') i++;
        int e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e - s < tl) continue;
        int k = 0;
        while (k < tl && lc(v[s + k]) == tok[k]) k++;
        if (k != tl) continue;
        if (s + tl == e) { if (num) *num = -1; return 1; }
        if (v[s + tl] == '=') {
            int32_t x = 0;
            int p = s + tl + 1;
            if (p < e && v[p] == '"') p++;
            int any = 0;
            while (p < e && v[p] >= '0' && v[p] <= '9' && x < 100000000) { x = x * 10 + (v[p] - '0'); p++; any = 1; }
            if (num) *num = any ? x : -1;
            return 1;
        }
    }
    return 0;
}

static int month_of(const char* s) {
    static const char* const m[12] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    for (int i = 0; i < 12; i++)
        if (lc(s[0]) == m[i][0] && lc(s[1]) == m[i][1] && lc(s[2]) == m[i][2]) return i + 1;
    return 0;
}

// "Wed, 21 Oct 2015 07:28:00 GMT" -> seconds since 1970 (0 = unparsable). Seconds fit 32 bits until 2106.
static uint32_t http_date(const char* v, int vl) {
    int i = 0;
    while (i < vl && v[i] != ',') i++;
    if (i >= vl) return 0;
    i++;
    while (i < vl && v[i] == ' ') i++;
    int d = 0, digits = 0;
    while (i < vl && v[i] >= '0' && v[i] <= '9') { d = d * 10 + (v[i++] - '0'); digits++; }
    if (!digits || d < 1 || d > 31) return 0;
    while (i < vl && (v[i] == ' ' || v[i] == '-')) i++;
    if (i + 3 > vl) return 0;
    int mo = month_of(v + i);
    if (!mo) return 0;
    i += 3;
    while (i < vl && (v[i] == ' ' || v[i] == '-')) i++;
    int y = 0;
    digits = 0;
    while (i < vl && v[i] >= '0' && v[i] <= '9') { y = y * 10 + (v[i++] - '0'); digits++; }
    if (digits == 2) y += y < 70 ? 2000 : 1900;
    if (y < 1970 || y > 2105) return 0;
    while (i < vl && v[i] == ' ') i++;
    int hh = 0, mm = 0, ss = 0;
    if (i + 8 > vl) return 0;
    hh = (v[i] - '0') * 10 + (v[i + 1] - '0');
    mm = (v[i + 3] - '0') * 10 + (v[i + 4] - '0');
    ss = (v[i + 6] - '0') * 10 + (v[i + 7] - '0');
    // days from civil (Howard Hinnant), 1970-01-01 = 0
    int yy = mo <= 2 ? y - 1 : y;
    int era = yy / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
    return days * 86400u + (uint32_t)(hh * 3600 + mm * 60 + ss);
}

static int32_t atoi64(const char* v, int vl) {
    int32_t x = 0;
    int any = 0;
    for (int i = 0; i < vl && v[i] >= '0' && v[i] <= '9' && x < 1000000000; i++) { x = x * 10 + (v[i] - '0'); any = 1; }
    return any ? x : -1;
}

int fcache_freshness(const char* r, uint32_t len, uint32_t* fresh_ticks, int prefetch) {
    int he = hdr_end(r, len);
    if (he < 0 || len < 12 || r[0] != 'H' || r[8] != ' ') return 0;
    if (!(r[9] == '2' && r[10] == '0' && r[11] == '0')) return 0;
    int vl;
    const char* v;
    if (hdr_value(r, he, "set-cookie", &vl)) return 0;
    if ((v = hdr_value(r, he, "vary", &vl))) {
        // anything the request does not vary (or varies identically every time) is fine; the rest is not cacheable
        int i = 0;
        while (i < vl) {
            while (i < vl && (v[i] == ' ' || v[i] == ',')) i++;
            int s = i;
            while (i < vl && v[i] != ',') i++;
            int e = i;
            while (e > s && v[e - 1] == ' ') e--;
            if (e == s) continue;
            int n = e - s, ok = 0;
            static const char* const okv[] = { "accept-encoding", "accept", "origin", "user-agent" };
            for (int q = 0; q < 4 && !ok; q++) {
                int ql = (int)strlen(okv[q]);
                if (ql != n) continue;
                int k = 0;
                while (k < n && lc(v[s + k]) == okv[q][k]) k++;
                ok = k == n;
            }
            if (!ok) return 0;
        }
    }
    int32_t maxage = -1;
    int immutable = 0;
    if ((v = hdr_value(r, he, "cache-control", &vl))) {
        if (list_token(v, vl, "no-store", 0)) return 0;
        if (list_token(v, vl, "no-cache", 0) && !prefetch) return 0;
        list_token(v, vl, "max-age", &maxage);
        immutable = list_token(v, vl, "immutable", 0);
    }
    int32_t fresh = 0;
    if (maxage >= 0) {
        fresh = maxage;
        if ((v = hdr_value(r, he, "age", &vl))) { int32_t a = atoi64(v, vl); if (a > 0) fresh -= a; }
    } else {
        uint32_t date = 0, exp = 0, lm = 0;
        if ((v = hdr_value(r, he, "date", &vl))) date = http_date(v, vl);
        if ((v = hdr_value(r, he, "expires", &vl))) exp = http_date(v, vl);
        if ((v = hdr_value(r, he, "last-modified", &vl))) lm = http_date(v, vl);
        if (exp && date) fresh = exp > date ? (int32_t)(exp - date) : 0;
        else if (lm && date && date > lm) fresh = (int32_t)((date - lm) / 10);
    }
    if (immutable && fresh < (int32_t)FC_MAX_FRESH_S) fresh = FC_MAX_FRESH_S;
    if (fresh > (int32_t)FC_MAX_FRESH_S) fresh = FC_MAX_FRESH_S;
    if (prefetch && fresh < (int32_t)FC_PREFETCH_S) fresh = FC_PREFETCH_S;
    if (fresh <= 0) return 0;
    *fresh_ticks = (uint32_t)fresh * 100u;
    return 1;
}

// ---- the store --------------------------------------------------------------------------------------------------

struct fc_ent {
    struct fcache_key k;
    char* data;
    uint32_t len;
    uint32_t stored, fresh;      // tick stored, lifetime in ticks
    uint32_t used;               // LRU stamp
};

static struct fc_ent ents[FC_MAX_ENTRIES];
static uint32_t total_bytes, use_clock, st_hits, st_misses;

void fcache_key_init(struct fcache_key* k) { k->h1 = 2166136261u; k->h2 = 0x9E3779B9u; }
void fcache_key_add(struct fcache_key* k, const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        k->h1 = (k->h1 ^ c) * 16777619u;
        k->h2 = ((k->h2 + c + 1) * 0x01000193u) ^ (k->h2 >> 13);
    }
    k->h1 = (k->h1 ^ 0xFF) * 16777619u;
    k->h2 = ((k->h2 + 0x55) * 0x01000193u) ^ (k->h2 >> 13);
}

static void drop(struct fc_ent* e) {
    if (!e->data) return;
    fc_free(e->data);
    total_bytes -= e->len;
    e->data = 0;
    e->len = 0;
}

static int expired(const struct fc_ent* e, uint32_t now) { return (uint32_t)(now - e->stored) >= e->fresh; }

int fcache_lookup(const struct fcache_key* k, uint32_t now, const char** data, uint32_t* len) {
    for (int i = 0; i < FC_MAX_ENTRIES; i++) {
        struct fc_ent* e = &ents[i];
        if (!e->data || e->k.h1 != k->h1 || e->k.h2 != k->h2) continue;
        if (expired(e, now)) { drop(e); break; }
        e->used = ++use_clock;
        st_hits++;
        *data = e->data;
        *len = e->len;
        return 1;
    }
    st_misses++;
    return 0;
}

int fcache_store(const struct fcache_key* k, const char* resp, uint32_t len, uint32_t now, int prefetch) {
    uint32_t fresh;
    if (len > FC_MAX_ONE || !fcache_freshness(resp, len, &fresh, prefetch)) return 0;
    uint32_t cap = FC_MAX_TOTAL, ht = fc_heap_total() / 8;
    if (ht < cap) cap = ht;
    if (len > cap) return 0;
    int slot = -1;
    for (int i = 0; i < FC_MAX_ENTRIES; i++)            // same key: replace
        if (ents[i].data && ents[i].k.h1 == k->h1 && ents[i].k.h2 == k->h2) { drop(&ents[i]); slot = i; break; }
    for (int i = 0; i < FC_MAX_ENTRIES; i++)            // drop what already expired
        if (ents[i].data && expired(&ents[i], now)) drop(&ents[i]);
    while (total_bytes + len > cap) {                   // evict least recently used until it fits
        int old = -1;
        for (int i = 0; i < FC_MAX_ENTRIES; i++)
            if (ents[i].data && (old < 0 || (int32_t)(ents[i].used - ents[old].used) < 0)) old = i;
        if (old < 0) return 0;
        drop(&ents[old]);
    }
    if (slot < 0)
        for (int i = 0; i < FC_MAX_ENTRIES; i++) if (!ents[i].data) { slot = i; break; }
    if (slot < 0) {                                      // table full: evict the least recently used entry
        for (int i = 0; i < FC_MAX_ENTRIES; i++)
            if (slot < 0 || (int32_t)(ents[i].used - ents[slot].used) < 0) slot = i;
        drop(&ents[slot]);
    }
    char* d = (char*)fc_alloc(len + 1);
    if (!d) return 0;
    memcpy(d, resp, len);
    d[len] = 0;
    struct fc_ent* e = &ents[slot];
    e->k = *k;
    e->data = d;
    e->len = len;
    e->stored = now;
    e->fresh = fresh;
    e->used = ++use_clock;
    total_bytes += len;
    return 1;
}

void fcache_clear(void) {
    for (int i = 0; i < FC_MAX_ENTRIES; i++) drop(&ents[i]);
}

void fcache_stats(int* entries, uint32_t* bytes, uint32_t* hits, uint32_t* misses) {
    int n = 0;
    for (int i = 0; i < FC_MAX_ENTRIES; i++) if (ents[i].data) n++;
    if (entries) *entries = n;
    if (bytes) *bytes = total_bytes;
    if (hits) *hits = st_hits;
    if (misses) *misses = st_misses;
}
