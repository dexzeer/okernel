// AES-128 / AES-128-GCM known-answer tests (FIPS 197, GCM spec test cases
// 2-4) plus a differential run against OpenSSL vectors on stdin:
//   python3 tests/aes_vectors.py | ./build-host/t_aes -
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "aes.h"

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static int unhex(const char* s, uint8_t* out) {
    int n = 0;
    while (s[0] && s[1] && s[0] != ' ' && s[0] != '\n') { out[n++] = (uint8_t)(hexval(s[0]) * 16 + hexval(s[1])); s += 2; }
    return n;
}
static int fails;
static void check(const char* name, const uint8_t* got, const uint8_t* want, int n) {
    int ok = !memcmp(got, want, n);
    if (!ok) fails++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static void gcm_case(const char* name, const char* k, const char* iv, const char* p, const char* a, const char* c, const char* t) {
    uint8_t key[16], nonce[12], pt[256], aad[256], ct[256], tag[16], out[256], tg[16], back[256];
    unhex(k, key); unhex(iv, nonce);
    int pl = unhex(p, pt), al = unhex(a, aad);
    unhex(c, ct); unhex(t, tag);
    aead_aes128gcm_encrypt(key, nonce, aad, al, pt, pl, out, tg);
    char nm[96];
    snprintf(nm, sizeof nm, "%s ciphertext", name); check(nm, out, ct, pl);
    snprintf(nm, sizeof nm, "%s tag", name); check(nm, tg, tag, 16);
    int r = aead_aes128gcm_decrypt(key, nonce, aad, al, ct, pl, tag, back);
    snprintf(nm, sizeof nm, "%s decrypt", name);
    int ok = r == 0 && !memcmp(back, pt, pl);
    if (!ok) fails++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", nm);
    tag[0] ^= 1;
    r = aead_aes128gcm_decrypt(key, nonce, aad, al, ct, pl, tag, back);
    snprintf(nm, sizeof nm, "%s tamper rejected", name);
    if (r != -1) fails++;
    printf("%s %s\n", r == -1 ? "PASS" : "FAIL", nm);
}

int main(int argc, char** argv) {
    uint8_t key[16], pt[16], ct[16], out[32];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    unhex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
    aes128_ctx ctx;
    aes128_init(&ctx, key);
    aes128_encrypt_blocks(&ctx, pt, out, 1);
    check("FIPS-197 C.1", out, ct, 16);
    uint8_t two[32];
    memcpy(two, pt, 16); memcpy(two + 16, pt, 16);
    aes128_encrypt_blocks(&ctx, two, out, 2);
    check("two-block lane 0", out, ct, 16);
    check("two-block lane 1", out + 16, ct, 16);
    gcm_case("GCM TC2", "00000000000000000000000000000000", "000000000000000000000000",
             "00000000000000000000000000000000", "", "0388dace60b6a392f328c2b971b2fe78", "ab6e47d42cec13bdf53a67b21257bddf");
    gcm_case("GCM TC3", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
             "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
             "", "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
             "4d5c2af327cd64a62cf35abd2ba6fab4");
    gcm_case("GCM TC4", "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
             "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
             "feedfacedeadbeeffeedfacedeadbeefabaddad2",
             "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
             "5bc94fbc3221a5db94fae95ae7121a47");
    if (argc > 1 && !strcmp(argv[1], "-")) {   // differential vectors: key iv aad pt ct tag (hex)
        static char line[1 << 17];
        static uint8_t k[16], iv[12], a[4096], p[16384], c[16384], t[16], o[16384], tg[16];
        int n = 0, bad = 0;
        while (fgets(line, sizeof line, stdin)) {
            char* f[6];
            int nf = 0;
            for (char* s = strtok(line, " \n"); s && nf < 6; s = strtok(0, " \n")) f[nf++] = s;
            if (nf != 6) continue;
            unhex(f[0], k); unhex(f[1], iv);
            int al = f[2][0] == '-' ? 0 : unhex(f[2], a);
            int pl = f[3][0] == '-' ? 0 : unhex(f[3], p);
            if (f[4][0] != '-') unhex(f[4], c);
            unhex(f[5], t);
            aead_aes128gcm_encrypt(k, iv, a, al, p, pl, o, tg);
            if (memcmp(o, c, pl) || memcmp(tg, t, 16)) bad++;
            n++;
        }
        printf("%s differential vs OpenSSL: %d/%d\n", bad ? "FAIL" : "PASS", n - bad, n);
        if (bad) fails++;
    }
    printf("%s\n", fails ? "AES TESTS FAIL" : "AES TESTS PASS");
    return fails != 0;
}
