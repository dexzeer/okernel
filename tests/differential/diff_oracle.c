// Differential-test oracle for okernel crypto primitives.
//
// Line protocol on stdin/stdout (all blobs hex, space-separated):
//   X25519 <scalar32> <u32>          -> <rc> <out32>      (rc 1=ok 0=reject)
//   SHA256 <msg>                     -> <digest32>
//   HMAC <key> <msg>                 -> <mac32>
//   HKDF <salt> <ikm> <info> <olen>  -> <prk32> <okm>     (extract+expand)
//   AEAD_ENC <key32> <nonce12> <aad> <pt> -> <ct> <tag16>
//   AEAD_DEC <key32> <nonce12> <aad> <ct> <tag16> -> <rc> <pt>
// Empty blobs are written as "-". Anything malformed -> "ERROR" line.
// Exit 0 on EOF.
//
// Compile (host, system libc — note: do NOT add -Isrc; the kernel's
// freestanding string.h would shadow the system one):
//   gcc -m32 -O2 -Isrc/crypto -o build-host/diff_oracle \
//     tests/differential/diff_oracle.c <crypto sources...>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"
#include "hmac.h"
#include "hkdf.h"
#include "aead.h"
#include "x25519.h"

#define MAXB 1024

static int unhex(const char* s, uint8_t* out, uint32_t cap, uint32_t* len) {
    if (s[0] == '-' && s[1] == 0) { *len = 0; return 0; }
    uint32_t n = 0;
    while (s[n]) n++;
    if (n % 2 || n / 2 > cap) return -1;
    for (uint32_t i = 0; i < n / 2; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return -1;
        }
        out[i] = (uint8_t)v;
    }
    *len = n / 2;
    return 0;
}

static void puthex(const uint8_t* p, uint32_t n) {
    if (n == 0) { printf("-"); return; }
    for (uint32_t i = 0; i < n; i++) printf("%02x", p[i]);
}

static char line[8192];

int main(void) {
    static uint8_t a[MAXB], b[MAXB], c[MAXB], d[MAXB], out[MAXB + 64];
    while (fgets(line, sizeof(line), stdin)) {
        char* tok = strtok(line, " \t\r\n");
        if (!tok) continue;
        if (!strcmp(tok, "X25519")) {
            char *ss = strtok(NULL, " \t\r\n"), *su = strtok(NULL, " \t\r\n");
            uint32_t ls, lu;
            if (!ss || !su || unhex(ss, a, 32, &ls) || ls != 32 ||
                unhex(su, b, 32, &lu) || lu != 32) { printf("ERROR\n"); continue; }
            int rc = x25519_shared_secret(out, a, b);
            printf("%d ", rc);
            puthex(out, 32);
            printf("\n");
        } else if (!strcmp(tok, "SHA256")) {
            char* sm = strtok(NULL, " \t\r\n");
            uint32_t lm;
            if (!sm || unhex(sm, a, MAXB, &lm)) { printf("ERROR\n"); continue; }
            sha256(a, lm, out);
            puthex(out, 32);
            printf("\n");
        } else if (!strcmp(tok, "HMAC")) {
            char *sk = strtok(NULL, " \t\r\n"), *sm = strtok(NULL, " \t\r\n");
            uint32_t lk, lm;
            if (!sk || !sm || unhex(sk, a, MAXB, &lk) || unhex(sm, b, MAXB, &lm)) {
                printf("ERROR\n"); continue;
            }
            hmac_sha256(a, lk, b, lm, out);
            puthex(out, 32);
            printf("\n");
        } else if (!strcmp(tok, "HKDF")) {
            char *ss = strtok(NULL, " \t\r\n"), *si = strtok(NULL, " \t\r\n");
            char *sf = strtok(NULL, " \t\r\n"), *so = strtok(NULL, " \t\r\n");
            uint32_t ls, li, lf;
            unsigned long olen;
            if (!ss || !si || !sf || !so || unhex(ss, a, MAXB, &ls) ||
                unhex(si, b, MAXB, &li) || unhex(sf, c, MAXB, &lf) ||
                (olen = strtoul(so, NULL, 10), olen > MAXB)) {
                printf("ERROR\n"); continue;
            }
            uint8_t prk[32];
            hkdf_extract(ls ? a : NULL, ls, b, li, prk);
            if (hkdf_expand(prk, lf ? c : NULL, lf, out, (uint32_t)olen) != 0) {
                printf("ERROR\n"); continue;
            }
            puthex(prk, 32);
            printf(" ");
            puthex(out, (uint32_t)olen);
            printf("\n");
        } else if (!strcmp(tok, "AEAD_ENC")) {
            char *sk = strtok(NULL, " \t\r\n"), *sn = strtok(NULL, " \t\r\n");
            char *sa = strtok(NULL, " \t\r\n"), *sp = strtok(NULL, " \t\r\n");
            uint32_t lk, ln, la, lp;
            if (!sk || !sn || !sa || !sp || unhex(sk, a, 32, &lk) || lk != 32 ||
                unhex(sn, b, 12, &ln) || ln != 12 || unhex(sa, c, MAXB, &la) ||
                unhex(sp, d, MAXB, &lp)) { printf("ERROR\n"); continue; }
            uint8_t tag[16];
            if (aead_chacha20_poly1305_encrypt(a, b, la ? c : NULL, la,
                                              d, lp, out, tag) != 0) {
                printf("ERROR\n"); continue;
            }
            puthex(out, lp);
            printf(" ");
            puthex(tag, 16);
            printf("\n");
        } else if (!strcmp(tok, "AEAD_DEC")) {
            char *sk = strtok(NULL, " \t\r\n"), *sn = strtok(NULL, " \t\r\n");
            char *sa = strtok(NULL, " \t\r\n"), *sc = strtok(NULL, " \t\r\n");
            char *st = strtok(NULL, " \t\r\n");
            uint32_t lk, ln, la, lc, lt;
            static uint8_t tag[16];
            if (!sk || !sn || !sa || !sc || !st || unhex(sk, a, 32, &lk) || lk != 32 ||
                unhex(sn, b, 12, &ln) || ln != 12 || unhex(sa, c, MAXB, &la) ||
                unhex(sc, d, MAXB, &lc) || unhex(st, tag, 16, &lt) || lt != 16) {
                printf("ERROR\n"); continue;
            }
            int rc = aead_chacha20_poly1305_decrypt(a, b, la ? c : NULL, la,
                                                   d, lc, tag, out);
            printf("%d ", rc);
            puthex(out, rc == 0 ? lc : 0);
            printf("\n");
        } else {
            printf("ERROR\n");
        }
        fflush(stdout);
    }
    return 0;
}
