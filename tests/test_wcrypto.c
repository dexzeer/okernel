// Host test for src/rtw/wcrypto.c against vectors printed by tests/wcrypto_vectors.py (stdin).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/rtw/wcrypto.h"

static int fails, checked;
static char line[8192];
static int unhex(const char* s, uint8_t* out) {
    if (!strcmp(s, "-")) return 0;
    int n = (int)strlen(s) / 2;
    for (int i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
    return n;
}
#define CHK(c, what) do { checked++; if (!(c)) { fails++; if (fails < 10) printf("FAIL %s: %.100s\n", what, line); } } while (0)

int main(void) {
    static uint8_t a[4096], b[4096], c[4096], o[4096], p[4096];
    while (fgets(line, sizeof line, stdin)) {
        char copy[8192]; strcpy(copy, line);
        char* tok[10]; int nt = 0;
        for (char* t = strtok(copy, " \r\n"); t && nt < 10; t = strtok(0, " \r\n")) tok[nt++] = t;
        if (!nt) continue;
        if (!strcmp(tok[0], "hmac")) {
            int kl = unhex(tok[1], a), ml = unhex(tok[2], b), el = unhex(tok[3], c);
            wc_hmac_sha1(a, (uint32_t)kl, b, (uint32_t)ml, o);
            CHK(el == 20 && !memcmp(o, c, 20), "hmac");
        } else if (!strcmp(tok[0], "pbkdf2")) {
            int pl = unhex(tok[1], a), sl = unhex(tok[2], b), n = atoi(tok[4]), el = unhex(tok[5], c);
            wc_pbkdf2_sha1(a, (uint32_t)pl, b, (uint32_t)sl, (uint32_t)atoi(tok[3]), o, (uint32_t)n);
            CHK(el == n && !memcmp(o, c, (size_t)n), "pbkdf2");
        } else if (!strcmp(tok[0], "psk")) {
            wc_wpa_psk(tok[1], (const uint8_t*)tok[2], (uint32_t)strlen(tok[2]), o);
            int el = unhex(tok[3], c);
            CHK(el == 32 && !memcmp(o, c, 32), "psk");
        } else if (!strcmp(tok[0], "prf")) {
            char label[64]; strcpy(label, tok[2]);
            for (char* q = label; *q; q++) if (*q == '_') *q = ' ';
            int kl = unhex(tok[1], a), dl = unhex(tok[3], b), n = atoi(tok[4]), el = unhex(tok[5], c);
            wc_prf_sha1(a, (uint32_t)kl, label, b, (uint32_t)dl, o, (uint32_t)n);
            CHK(el == n && !memcmp(o, c, (size_t)n), "prf");
        } else if (!strcmp(tok[0], "dec")) {
            unhex(tok[1], a); unhex(tok[2], b); unhex(tok[3], c);
            wc_aes128_decrypt_block(a, b, o);
            CHK(!memcmp(o, c, 16), "aes decrypt");
        } else if (!strcmp(tok[0], "unwrap")) {
            unhex(tok[1], a); int wl = unhex(tok[2], b), pl = unhex(tok[3], c);
            int r = wc_aes_unwrap(a, b, (uint32_t)(wl / 8 - 1), o);
            CHK(r == 0 && pl == wl - 8 && !memcmp(o, c, (size_t)pl), "unwrap");
            b[wl - 1] ^= 1;                                                                  // a damaged blob must be refused
            CHK(wc_aes_unwrap(a, b, (uint32_t)(wl / 8 - 1), o) != 0, "unwrap integrity");
        } else if (!strcmp(tok[0], "ptk")) {
            static uint8_t aa[8], spa[8], an[40], sn[40], want[64];
            unhex(tok[1], a); unhex(tok[2], aa); unhex(tok[3], spa); unhex(tok[4], an); unhex(tok[5], sn); unhex(tok[6], want);
            wc_ptk(a, aa, spa, an, sn, o);
            CHK(!memcmp(o, want, 48), "ptk");
        } else if (!strcmp(tok[0], "wrap")) {
            int pl = unhex(tok[2], b), wl = unhex(tok[3], c);
            unhex(tok[1], a);
            wc_aes_wrap(a, b, (uint32_t)(pl / 8), o);
            CHK(wl == pl + 8 && !memcmp(o, c, (size_t)wl), "wrap");
        } else if (!strcmp(tok[0], "ccm")) {
            static uint8_t nonce[16], aad[512], ct[4096], mic[16];
            unhex(tok[1], a); unhex(tok[2], nonce); int al = unhex(tok[3], aad), pl = unhex(tok[4], p), cl = unhex(tok[5], ct); unhex(tok[6], mic);
            wc_ccm_encrypt(a, nonce, aad, (uint32_t)al, p, (uint32_t)pl, o, c);
            CHK(cl == pl && !memcmp(o, ct, (size_t)pl) && !memcmp(c, mic, 8), "ccm encrypt");
            int r = wc_ccm_decrypt(a, nonce, aad, (uint32_t)al, ct, (uint32_t)cl, mic, o);
            CHK(r == 0 && !memcmp(o, p, (size_t)pl), "ccm decrypt");
            mic[3] ^= 0x10;
            CHK(wc_ccm_decrypt(a, nonce, aad, (uint32_t)al, ct, (uint32_t)cl, mic, o) != 0, "ccm bad mic");
            mic[3] ^= 0x10;
            if (al) { aad[0] ^= 1; CHK(wc_ccm_decrypt(a, nonce, aad, (uint32_t)al, ct, (uint32_t)cl, mic, o) != 0, "ccm bad aad"); }
        }
    }
    if (fails) printf("WCRYPTO FAIL (%d of %d)\n", fails, checked); else printf("WCRYPTO PASS (%d checks)\n", checked);
    return fails != 0;
}
