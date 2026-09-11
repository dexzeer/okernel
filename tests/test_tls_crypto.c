#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "hmac.h"
#include "hkdf.h"
#include "sha1.h"
#include "aead.h"
#include "x25519.h"

static int fails = 0;

static void check(const char* name, const uint8_t* got, const uint8_t* want, int n) {
    int ok = memcmp(got, want, n) == 0;
    printf("%-28s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        fails++;
        printf("  got:  "); for (int i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n  want: "); for (int i = 0; i < n; i++) printf("%02x", want[i]);
        printf("\n");
    }
}

static void unhex(uint8_t* out, const char* hex, int n) {
    for (int i = 0; i < n; i++) {
        char c = hex[2*i];   int hi = c >= 'a' ? c - 'a' + 10 : c - '0';
        c = hex[2*i+1];      int lo = c >= 'a' ? c - 'a' + 10 : c - '0';
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

int main(void) {
    uint8_t out[64];

    // ---- SHA-1 FIPS 180-4 / RFC 3174 vectors ----
    {
        uint8_t h[20], want[20];
        sha1(NULL, 0, h);
        unhex(want, "da39a3ee5e6b4b0d3255bfef95601890afd80709", 20);
        check("SHA1 empty", h, want, 20);
        sha1((const uint8_t*)"abc", 3, h);
        unhex(want, "a9993e364706816aba3e25717850c26c9cd0d89d", 20);
        check("SHA1 abc", h, want, 20);
        sha1((const uint8_t*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
        unhex(want, "84983e441c3bd26ebaae4aa1f95129e5e54670f1", 20);
        check("SHA1 448-bit", h, want, 20);
    }

    // ---- HMAC-SHA256 RFC 4231 TC1 ----
    uint8_t k20[20]; memset(k20, 0x0b, 20);
    hmac_sha256(k20, 20, (const uint8_t*)"Hi There", 8, out);
    uint8_t hmac1[32]; unhex(hmac1,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);
    check("HMAC TC1", out, hmac1, 32);

    // ---- HMAC-SHA256 RFC 4231 TC2 ----
    hmac_sha256((const uint8_t*)"Jefe", 4,
                (const uint8_t*)"what do ya want for nothing?", 28, out);
    uint8_t hmac2[32]; unhex(hmac2,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);
    check("HMAC TC2", out, hmac2, 32);

    // ---- HKDF RFC 5869 TC1 ----
    uint8_t ikm[22]; memset(ikm, 0x0b, 22);
    uint8_t salt[13]; unhex(salt, "000102030405060708090a0b0c", 13);
    uint8_t info[10]; unhex(info, "f0f1f2f3f4f5f6f7f8f9", 10);
    hkdf(salt, 13, ikm, 22, info, 10, out, 42);
    uint8_t okm1[42]; unhex(okm1,
        "3cb25f25faacd57a90434f64d0362f2a"
        "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865", 42);
    check("HKDF TC1", out, okm1, 42);

    // Oversize refusal (review #40): >8160B must FAIL, never clamp.
    {
        static uint8_t big_okm[9000];
        int r1 = hkdf(salt, 13, ikm, 22, info, 10, big_okm, 9000);
        printf("%-28s %s\n", "HKDF oversize refuses", r1 == -1 ? "PASS" : "FAIL");
        if (r1 != -1) fails++;
        uint8_t prk[32];
        memset(prk, 0x11, 32);
        int r2 = hkdf_expand(prk, info, 10, big_okm, 9000);
        printf("%-28s %s\n", "HKDF-expand oversize refuses", r2 == -1 ? "PASS" : "FAIL");
        if (r2 != -1) fails++;
    }

    // ---- AEAD RFC 8439 §2.8.2 ----
    uint8_t key[32]; unhex(key,
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", 32);
    uint8_t nonce[12]; unhex(nonce, "070000004041424344454647", 12);
    uint8_t aad[12];   unhex(aad, "50515253c0c1c2c3c4c5c6c7", 12);
    const char* pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                     "only one tip for the future, sunscreen would be it.";
    uint8_t ct[114], tag[16];
    aead_chacha20_poly1305_encrypt(key, nonce, aad, 12,
                                   (const uint8_t*)pt, 114, ct, tag);
    uint8_t ct_want[114]; unhex(ct_want,
        "d31a8d34648e60db7b86afbc53ef7ec2"
        "a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b"
        "1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58"
        "fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b"
        "6116", 114);
    check("AEAD ciphertext", ct, ct_want, 114);
    // Expected tag verified against OpenSSL (via python-cryptography) over
    // the same RFC inputs:
    uint8_t tag_want[16]; unhex(tag_want,
        "1ae10b594f09e26a7e902ecbd0600691", 16);
    check("AEAD tag", tag, tag_want, 16);

    // decrypt round-trip + tamper detection
    uint8_t rt[114];
    int rc = aead_chacha20_poly1305_decrypt(key, nonce, aad, 12, ct, 114, tag, rt);
    printf("%-28s %s\n", "AEAD decrypt", rc == 0 && memcmp(rt, pt, 114) == 0 ? "PASS" : "FAIL");
    if (rc != 0 || memcmp(rt, pt, 114)) fails++;
    ct[5] ^= 0x40;
    rc = aead_chacha20_poly1305_decrypt(key, nonce, aad, 12, ct, 114, tag, rt);
    printf("%-28s %s\n", "AEAD tamper detect", rc == -1 ? "PASS" : "FAIL");
    if (rc != -1) fails++;
    ct[5] ^= 0x40;

    // Oversize refusal (review 2026-09-10 #1): >20KB MAC input must FAIL
    // loudly, never emit/compare an unwritten tag.
    {
        static uint8_t big_in[25000], big_out[25000], big_tag[16];
        for (int i = 0; i < 25000; i++) big_in[i] = (uint8_t)i;
        rc = aead_chacha20_poly1305_encrypt(key, nonce, aad, 12,
                                            big_in, 25000, big_out, big_tag);
        printf("%-28s %s\n", "AEAD oversize encrypt refuses", rc == -1 ? "PASS" : "FAIL");
        if (rc != -1) fails++;
        rc = aead_chacha20_poly1305_decrypt(key, nonce, aad, 12,
                                            big_in, 25000, big_tag, big_out);
        printf("%-28s %s\n", "AEAD oversize decrypt refuses", rc == -1 ? "PASS" : "FAIL");
        if (rc != -1) fails++;
    }

    // ---- X25519 RFC 7748 §5.2 vectors ----
    uint8_t sc[32], u[32], pub[32];
    unhex(sc, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", 32);
    unhex(u,  "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", 32);
    x25519(out, sc, u);
    uint8_t x1_want[32]; unhex(x1_want,
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);
    check("X25519 vec1", out, x1_want, 32);

    unhex(sc, "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", 32);
    unhex(u,  "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", 32);
    x25519(out, sc, u);
    uint8_t x2_want[32]; unhex(x2_want,
        "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32);
    check("X25519 vec2", out, x2_want, 32);

    // ---- X25519 RFC 7748 §6.1 Diffie-Hellman ----
    uint8_t alice_priv[32], alice_pub_want[32], bob_priv[32], bob_pub_want[32], shared_want[32];
    unhex(alice_priv, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", 32);
    unhex(alice_pub_want, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
    unhex(bob_priv, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", 32);
    unhex(bob_pub_want, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
    unhex(shared_want, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);

    x25519_public_key(pub, alice_priv);
    check("X25519 alice pub", pub, alice_pub_want, 32);
    x25519_public_key(pub, bob_priv);
    check("X25519 bob pub", pub, bob_pub_want, 32);

    uint8_t shared1[32], shared2[32];
    x25519_shared_secret(shared1, alice_priv, bob_pub_want);
    x25519_shared_secret(shared2, bob_priv, alice_pub_want);
    check("X25519 shared (A)", shared1, shared_want, 32);
    check("X25519 shared (B)", shared2, shared_want, 32);

    printf("\n%s\n", fails ? "SOME TESTS FAILED" : "ALL CRYPTO TESTS PASSED");
    return fails != 0;
}
