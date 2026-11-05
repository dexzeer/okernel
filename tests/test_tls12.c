// TLS 1.2 building blocks against published and OpenSSL-generated vectors, and
// the rules a ServerHello must pass (the full handshake: tests/tls12_interop.sh).
#include <stdio.h>
#include <stdint.h>
#include "string.h"
#include "tls12.h"
#include "aes.h"
#include "hmac.h"
#include "ec.h"

static int fails;
static void ok(const char* name, int c) {
    if (!c) { printf("FAIL %s\n", name); fails++; }
}
static uint32_t unhex(const char* h, uint8_t* out) {
    uint32_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        uint32_t v = 0;
        for (int i = 0; i < 2; i++) {
            char c = h[i];
            v = v * 16 + (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[n++] = (uint8_t)v;
    }
    return n;
}
static int same(const uint8_t* a, const char* hex) {
    uint8_t b[512];
    uint32_t n = unhex(hex, b);
    return memcmp(a, b, n) == 0;
}

// A ServerHello body; each test flips one thing.
struct sh_spec {
    uint16_t version, suite;
    const char* tail;          // the last 8 bytes of the random
    int echo_sid, compression, ri, ri_bad, ems, ks;
};
static const uint8_t our_sid[32] = { 9, 9, 9 };
static uint32_t build_sh(const struct sh_spec* b, uint8_t* m) {
    uint32_t p = 0;
    m[p++] = (uint8_t)(b->version >> 8); m[p++] = (uint8_t)b->version;
    for (int i = 0; i < 24; i++) m[p++] = (uint8_t)(i + 1);
    for (int i = 0; i < 8; i++) m[p++] = (uint8_t)(b->tail ? b->tail[i] : 0x55);
    m[p++] = 32;
    for (int i = 0; i < 32; i++) m[p++] = b->echo_sid ? our_sid[i] : (uint8_t)0xA5;
    m[p++] = (uint8_t)(b->suite >> 8); m[p++] = (uint8_t)b->suite;
    m[p++] = (uint8_t)b->compression;
    uint32_t el = p; p += 2;
    if (b->ri) {
        m[p++] = 0xFF; m[p++] = 0x01; m[p++] = 0; m[p++] = 1; m[p++] = (uint8_t)b->ri_bad;
    }
    if (b->ems) { m[p++] = 0; m[p++] = 23; m[p++] = 0; m[p++] = 0; }
    if (b->ks) { m[p++] = 0; m[p++] = 51; m[p++] = 0; m[p++] = 0; }
    m[el] = (uint8_t)((p - el - 2) >> 8); m[el + 1] = (uint8_t)(p - el - 2);
    return p;
}
static int sh_verdict(const struct sh_spec* b) {
    uint8_t m[256];
    struct tls12_sh out;
    uint32_t n = build_sh(b, m);
    return tls12_parse_server_hello(m, n, our_sid, 1, &out);
}

int main(void) {
    uint8_t k[64], in[256], out[512], tag[16];

    // ---- AES-256 (FIPS 197 C.3, NIST GCM test cases 13-14) ----
    for (int i = 0; i < 32; i++) k[i] = (uint8_t)i;
    unhex("00112233445566778899aabbccddeeff", in);
    aes128_ctx c;
    aes256_init(&c, k);
    aes128_encrypt_blocks(&c, in, out, 1);
    ok("AES-256 block", same(out, "8ea2b7ca516745bfeafc49904b496089"));
    memset(k, 0, 32);
    uint8_t iv[12] = { 0 };
    ok("GCM-256 empty", aead_aes256gcm_encrypt(k, iv, 0, 0, in, 0, out, tag) == 0 && same(tag, "530f8afbc74536b9a963b4f1c4cb738b"));
    memset(in, 0, 16);
    aead_aes256gcm_encrypt(k, iv, 0, 0, in, 16, out, tag);
    ok("GCM-256 ciphertext", same(out, "cea7403d4d606b6e074ec5d3baf39d18"));
    ok("GCM-256 tag", same(tag, "d0d1c8a799996bf0265b98b5d48ab919"));
    uint8_t back[16];
    ok("GCM-256 open", aead_aes256gcm_decrypt(k, iv, 0, 0, out, 16, tag, back) == 0 && memcmp(back, in, 16) == 0);
    tag[3] ^= 1;
    ok("GCM-256 forged tag", aead_aes256gcm_decrypt(k, iv, 0, 0, out, 16, tag, back) != 0);

    // ---- HMAC-SHA384 (RFC 4231 case 2) ----
    hmac_sha384((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, out);
    ok("HMAC-SHA384", same(out, "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649"));

    // ---- PRF (values from `openssl kdf TLS1-PRF`) ----
    uint8_t sec[32], seed[32];
    uint32_t sl = unhex("9bbe436ba940f017b17652849a71db35", sec);
    uint32_t dl = unhex("a0ba9f936cda311827a6f796ffd5198c", seed);
    tls12_prf(0, sec, sl, "test label", seed, dl, out, 100);
    ok("PRF SHA-256", same(out, "e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a6b301791e90d35c9c9a46b4e14baf9af0fa022f7077def17abfd3797c0564bab4fbc91666e9def9b97fce34f796789baa48082d122ee42c5a72e5a5110fff70187347b66"));
    sl = unhex("b80b733d6ceefcdc71566ea48e5567df", sec);
    dl = unhex("cd665cf6a8447dd6ff8b27555edb7465", seed);
    tls12_prf(1, sec, sl, "test label", seed, dl, out, 148);
    ok("PRF SHA-384", same(out, "7b0c18e9ced410ed1804f2cfa34a336a1c14dffb4900bb5fd7942107e81c83cde9ca0faa60be9fe34f82b1233c9146a0e534cb400fed2700884f9dc236f80edd8bfa961144c9e8d792eca722a7b32fc3d416d473ebc2c5fd4abfdad05d9184259b5bf8cd4d90fa0d31e2dec479e4f1a26066f2eea9a69236a3e52655c9e9aee691c8f3a26854308d5eaa3be85e0990703d73e56f"));

    // ---- records survive a round trip and bind their context, for every suite ----
    static const uint16_t suites[] = { 0xC02B, 0xC02F, 0xC02C, 0xC030, 0xCCA9, 0xCCA8 };
    for (unsigned i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        uint16_t s = suites[i];
        uint8_t key[32], civ[12], pt[64], rec[128], dec[64];
        uint32_t wl = 0, n = 0;
        for (int j = 0; j < 32; j++) key[j] = (uint8_t)(j * 7);
        for (int j = 0; j < 12; j++) civ[j] = (uint8_t)(j * 3 + 1);
        for (int j = 0; j < 64; j++) pt[j] = (uint8_t)j;
        ok("seal", tls12_seal(s, key, civ, 5, 23, pt, 64, rec, &wl) == 0);
        ok("record size", wl == 64 + 16 + (tls12_suite_iv_len(s) == 4 ? 8u : 0u));
        ok("open", tls12_open(s, key, civ, 5, 23, rec, wl, dec, &n) == 0 && n == 64 && memcmp(dec, pt, 64) == 0);
        ok("wrong sequence", tls12_open(s, key, civ, 6, 23, rec, wl, dec, &n) != 0);
        ok("wrong type", tls12_open(s, key, civ, 5, 22, rec, wl, dec, &n) != 0);
        rec[wl - 1] ^= 1;
        ok("bad tag", tls12_open(s, key, civ, 5, 23, rec, wl, dec, &n) != 0);
        ok("short record", tls12_open(s, key, civ, 5, 23, rec, 7, dec, &n) != 0);
    }

    // ---- ServerHello rules ----
    struct sh_spec good = { 0x0303, 0xC02F, 0, 0, 0, 1, 0, 1, 0 };
    ok("good hello", sh_verdict(&good) == 0);
    uint8_t m[256];
    ok("version is 1.2", tls12_sh_version(m, build_sh(&good, m)) == 12);
    struct sh_spec b;
    b = good; b.tail = "DOWNGRD\x01"; ok("1.2 downgrade mark", sh_verdict(&b) != 0);
    b = good; b.tail = "DOWNGRD\x00"; ok("1.1 downgrade mark", sh_verdict(&b) != 0);
    b = good; b.tail = "DOWNGRD\x02"; ok("look-alike random is fine", sh_verdict(&b) == 0);
    b = good; b.ri = 0; ok("no renegotiation_info", sh_verdict(&b) != 0);
    b = good; b.ri_bad = 1; ok("non-empty renegotiation_info", sh_verdict(&b) != 0);
    b = good; b.suite = 0xC013; ok("CBC suite", sh_verdict(&b) != 0);
    b = good; b.suite = 0x009C; ok("RSA key exchange", sh_verdict(&b) != 0);
    b = good; b.suite = 0x1303; ok("1.3 suite", sh_verdict(&b) != 0);
    b = good; b.compression = 1; ok("compression", sh_verdict(&b) != 0);
    b = good; b.echo_sid = 1; ok("resumes nothing", sh_verdict(&b) != 0);
    b = good; b.version = 0x0302; ok("TLS 1.1", sh_verdict(&b) != 0);
    b = good; b.ks = 1; ok("key_share in a 1.2 hello", sh_verdict(&b) != 0);

    // ---- Certificate layout ----
    {
        static const uint8_t two[] = { 0, 0, 12,  0, 0, 3, 'a', 'b', 'c',  0, 0, 3, 'x', 'y', 'z' };
        static const uint8_t want[] = { 0, 0, 0, 16,  0, 0, 3, 'a', 'b', 'c', 0, 0,  0, 0, 3, 'x', 'y', 'z', 0, 0 };
        uint32_t ol = 0;
        ok("cert convert", tls12_cert_to_13(two, sizeof two, out, sizeof out, &ol) == 0 && ol == sizeof want && memcmp(out, want, ol) == 0);
        ok("cert over cap", tls12_cert_to_13(two, sizeof two, out, 10, &ol) != 0);
        static const uint8_t none[] = { 0, 0, 0 };
        ok("empty chain", tls12_cert_to_13(none, 3, out, sizeof out, &ol) != 0);
        static const uint8_t lie[] = { 0, 0, 6,  0, 0, 9, 'a', 'b', 'c' };
        ok("entry longer than list", tls12_cert_to_13(lie, sizeof lie, out, sizeof out, &ol) != 0);
    }

    // ---- ServerKeyExchange ----
    {
        uint8_t ske[36 + 8] = { 3, 0, 0x1D, 32 };
        ske[36] = 4; ske[37] = 1;
        ske[39] = 4;
        const uint8_t* sg; uint16_t alg, grp; uint32_t l, pl;
        ok("SKE good", tls12_parse_ske(ske, sizeof ske, &grp, &pl, &alg, &sg, &l) == 0 && alg == 0x0401 && l == 4 && pl == 36 && grp == 0x1D);
        ok("SKE trailing byte", tls12_parse_ske(ske, sizeof ske - 1, &grp, &pl, &alg, &sg, &l) != 0);
        ske[2] = 0x17; ok("SKE key the wrong size for its curve", tls12_parse_ske(ske, sizeof ske, &grp, &pl, &alg, &sg, &l) != 0);
        ske[2] = 0x19; ok("SKE curve we did not offer", tls12_parse_ske(ske, sizeof ske, &grp, &pl, &alg, &sg, &l) != 0);
        ske[2] = 0x1D; ske[0] = 1; ok("SKE explicit curve", tls12_parse_ske(ske, sizeof ske, &grp, &pl, &alg, &sg, &l) != 0);
    }

    // ---- ECDH: both sides agree, and a point off the curve is refused ----
    static const uint16_t groups[] = { EC_GROUP_P256, EC_GROUP_P384 };
    for (unsigned i = 0; i < 2; i++) {
        uint16_t g = groups[i];
        uint32_t pl = ec_ecdh_point_len(g), fl = (pl - 1) / 2;
        uint8_t a[48], b[48], pa[97], pb[97], sa[48], sb[48];
        for (uint32_t j = 0; j < fl; j++) { a[j] = (uint8_t)(0x11 + j); b[j] = (uint8_t)(0x71 + 3 * j); }
        ok("ECDH keygen", ec_ecdh_keygen(g, a, pa) == 0 && ec_ecdh_keygen(g, b, pb) == 0);
        ok("ECDH shared", ec_ecdh_shared(g, a, pb, pl, sa) == 0 && ec_ecdh_shared(g, b, pa, pl, sb) == 0);
        ok("ECDH agree", memcmp(sa, sb, fl) == 0);
        ok("ECDH differs from the public x", memcmp(sa, pa + 1, fl) != 0);
        uint8_t zero[48] = { 0 }, ff[48];
        memset(ff, 0xFF, sizeof ff);
        ok("ECDH zero key", ec_ecdh_keygen(g, zero, pa) != 0);
        ok("ECDH key >= order", ec_ecdh_keygen(g, ff, pa) != 0);
        ec_ecdh_keygen(g, a, pa);
        pb[pl - 1] ^= 1;
        ok("ECDH off the curve", ec_ecdh_shared(g, a, pb, pl, sa) != 0);
        pb[pl - 1] ^= 1;
        pb[0] = 2;
        ok("ECDH compressed", ec_ecdh_shared(g, a, pb, pl, sa) != 0);
        pb[0] = 4;
        ok("ECDH short point", ec_ecdh_shared(g, a, pb, pl - 1, sa) != 0);
        memset(pb + 1, 0xFF, fl);
        ok("ECDH x >= p", ec_ecdh_shared(g, a, pb, pl, sa) != 0);
    }

    printf(fails ? "tls12: %d FAILED\n" : "tls12: all passed\n", fails);
    return fails != 0;
}
