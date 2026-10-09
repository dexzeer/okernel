// Host test for src/ojs/num.c against V8-generated vectors.
//   build: see tests/ojs/build.sh
// Includes num.c directly (static functions) with tiny stand-ins for the
// few engine services it touches.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ojs/ojs_int.h"

void* ojs_sys_malloc(size_t n) { return malloc(n); }
void* ojs_sys_realloc(void* p, size_t n) { return realloc(p, n); }
void ojs_sys_free(void* p) { free(p); }
struct str* str_new8(ojs* J, const uint8_t* s, uint32_t len) { (void)J; (void)s; (void)len; return 0; }

#include "../../src/ojs/num.c"

static double from_hex(const char* h) { uint64_t u = strtoull(h, 0, 16); double d; memcpy(&d, &u, 8); return d; }
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

// JSON string decoding (vectors use JSON.stringify for N lines)
static int json_str(const char* p, char* out, const char** end) {
    if (*p != '"') return -1;
    p++;
    int n = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                unsigned cp = (unsigned)strtoul((char[]){p[1], p[2], p[3], p[4], 0}, 0, 16);
                p += 5;
                if (cp < 0x80) out[n++] = (char)cp;
                else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
                else { out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[n++] = (char)(0x80 | (cp & 0x3F)); }
                continue;
            }
            out[n++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            p++;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    *end = p + 1;
    return n;
}

// a flat 16-bit struct str from UTF-8 (BMP only, enough for the vectors)
static struct str* mkstr(const char* u8) {
    uint16_t units[4096];
    int n = 0;
    const unsigned char* s = (const unsigned char*)u8;
    while (*s) {
        unsigned c = *s++;
        if (c >= 0xE0) { c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
        else if (c >= 0xC0) { c = ((c & 0x1F) << 6) | (s[0] & 0x3F); s += 1; }
        units[n++] = (uint16_t)c;
    }
    struct str* st = (struct str*)calloc(1, sizeof(struct str) + (size_t)n * 2 + 2);
    st->h.aux = SF_WIDE;
    st->h.aux32 = (uint32_t)n;
    memcpy(st->u.c16, units, (size_t)n * 2);
    return st;
}

int main(int argc, char** argv) {
    // IEEE double semantics as in the engine: x87 at 53-bit precision
    unsigned short cw = 0x27F;
    __asm__ volatile("fldcw %0" :: "m"(cw));
    FILE* f = fopen(argc > 1 ? argv[1] : "build-host/num_vectors.txt", "r");
    if (!f) { printf("no vectors\n"); return 2; }
    static char line[70000];
    long n = 0, bad = 0, per[8] = {0}, perbad[8] = {0};
    const char* kinds = "SFEPRN";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char kind = line[0];
        int ki = (int)(strchr(kinds, kind) - kinds);
        char got[2000], want[2000];
        got[0] = 0;
        if (kind == 'N') {
            char s[70000];
            const char* end;
            if (json_str(line + 2, s, &end) < 0) continue;
            double w = from_hex(end + 1);
            struct str* st = mkstr(s);
            double g = num_from_str(st, 0);
            free(st);
            n++; per[ki]++;
            if (bits(g) != bits(w) && !(g != g && w != w)) {
                bad++; perbad[ki]++;
                if (perbad[ki] <= 5) printf("N %s: got %016llx want %016llx\n", s, (unsigned long long)bits(g), (unsigned long long)bits(w));
            }
            continue;
        }
        char hx[17];
        memcpy(hx, line + 2, 16); hx[16] = 0;
        double x = from_hex(hx);
        const char* rest = line + 19;
        if (kind == 'S') {
            num_to_cstr(x, got);
            strcpy(want, rest);
        } else {
            int arg = atoi(rest);
            const char* sp = strchr(rest, ' ');
            strcpy(want, sp ? sp + 1 : "");
            if (kind == 'F') num_to_fixed(x, arg, got);
            else if (kind == 'R') {
                // radix toString via shortest(): format like num_to_string
                char d[1200]; int k, p = 0;
                double ax = x < 0 ? -x : x;
                if (x < 0) got[p++] = '-';
                if (ax == 0) { got[p++] = '0'; }
                else {
                    int nd = shortest(ax, arg, d, &k);
                    if (k <= 0) { got[p++] = '0'; got[p++] = '.'; for (int i = 0; i < -k; i++) got[p++] = '0'; for (int i = 0; i < nd; i++) got[p++] = d[i]; }
                    else if (k >= nd) { for (int i = 0; i < nd; i++) got[p++] = d[i]; for (int i = 0; i < k - nd; i++) got[p++] = '0'; }
                    else { for (int i = 0; i < k; i++) got[p++] = d[i]; got[p++] = '.'; for (int i = k; i < nd; i++) got[p++] = d[i]; }
                }
                got[p] = 0;
            } else {   // E, P: format here from num_sig_digits (as the builtins will)
                char d[1300];
                int e10, p = 0;
                double ax = x < 0 ? -x : x;
                if (x < 0) got[p++] = '-';
                if (kind == 'E') {
                    int digits = arg < 0 ? 0 : arg + 1;
                    int nd;
                    if (ax == 0) { nd = digits ? digits : 1; for (int i = 0; i < nd; i++) d[i] = '0'; e10 = 0; }
                    else nd = num_sig_digits(ax, digits, d, &e10);
                    got[p++] = d[0];
                    if (nd > 1) { got[p++] = '.'; for (int i = 1; i < nd; i++) got[p++] = d[i]; }
                    p += sprintf(got + p, "e%c%d", e10 < 0 ? '-' : '+', e10 < 0 ? -e10 : e10);
                } else {
                    int prec = arg;
                    if (ax == 0) { for (int i = 0; i < prec; i++) d[i] = '0'; e10 = 0; }
                    else num_sig_digits(ax, prec, d, &e10);
                    if (e10 < -6 || e10 >= prec) {
                        got[p++] = d[0];
                        if (prec > 1) { got[p++] = '.'; for (int i = 1; i < prec; i++) got[p++] = d[i]; }
                        p += sprintf(got + p, "e%c%d", e10 < 0 ? '-' : '+', e10 < 0 ? -e10 : e10);
                    } else if (e10 >= 0) {
                        for (int i = 0; i <= e10; i++) got[p++] = d[i];
                        if (prec > e10 + 1) { got[p++] = '.'; for (int i = e10 + 1; i < prec; i++) got[p++] = d[i]; }
                    } else {
                        got[p++] = '0'; got[p++] = '.';
                        for (int i = 0; i < -(e10 + 1); i++) got[p++] = '0';
                        for (int i = 0; i < prec; i++) got[p++] = d[i];
                    }
                    got[p] = 0;
                }
                got[p] = 0;
            }
        }
        n++; per[ki]++;
        if (strcmp(got, want)) {
            bad++; perbad[ki]++;
            if (perbad[ki] <= 8) printf("%c %s (%.17g) arg=%s: got '%s' want '%s'\n", kind, hx, x, rest, got, want);
        }
    }
    for (int i = 0; i < 6; i++) printf("  %c: %ld checks, %ld mismatches\n", kinds[i], per[i], perbad[i]);
    printf("NUM %s (%ld/%ld)\n", bad ? "FAIL" : "PASS", n - bad, n);
    return bad != 0;
}
