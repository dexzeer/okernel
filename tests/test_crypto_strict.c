// Host tests for verifier strictness (review #13/#14/#15/#16):
// ECDSA canonical forms, EC coordinate range, RSA exponent discipline.
// Strategy: take REAL signatures/keys from the adversarial PKI fixtures
// and malleate them — every mutant must be REJECTED while the control
// verifies.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "x509.h"
#include "ec.h"
#include "rsa.h"

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  PASS %s\n", name); \
    else { printf("  FAIL %s\n", name); fails++; } \
} while (0)

static int load(const char* path, uint8_t* out, uint32_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("SKIP %s (missing)\n", path); return -1; }
    uint32_t n = (uint32_t)fread(out, 1, cap, f);
    fclose(f);
    return (int)n;
}

// P-256 order n and field prime p (big-endian).
static const uint8_t P256_N[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51 };
static const uint8_t P256_P[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

// Parse a DER ECDSA-Sig-Value into r/s views. Returns 0 on success.
static int sig_rs(const uint8_t* der, uint32_t len,
                  const uint8_t** r, uint32_t* rl,
                  const uint8_t** s, uint32_t* sl) {
    if (len < 8 || der[0] != 0x30) return -1;
    uint32_t seqlen = der[1];
    uint32_t p = (seqlen & 0x80) ? 2 + (seqlen & 0x7F) : 2;
    if (p >= len || der[p] != 0x02) return -1;
    uint32_t rlen = der[p+1];
    if (p + 2 + rlen > len || der[p+2+rlen] != 0x02) return -1;
    *r = der + p + 2; *rl = rlen;
    uint32_t q = p + 2 + rlen + 2;
    if (q > len) return -1;
    uint32_t slen = der[q-1];
    if (q + slen > len) return -1;
    *s = der + q; *sl = slen;
    return 0;
}

// Encode SEQ{INT(r), INT(s)} with EXACT given contents (caller crafts
// pads/values, short-form lengths only — our mutants are small).
static uint32_t sig_enc(uint8_t* out, const uint8_t* r, uint32_t rl,
                        const uint8_t* s, uint32_t sl) {
    uint32_t inner = 2 + rl + 2 + sl;
    if (inner >= 128) return 0;
    out[0] = 0x30; out[1] = (uint8_t)inner;
    out[2] = 0x02; out[3] = (uint8_t)rl;
    memcpy(out + 4, r, rl);
    out[4+rl] = 0x02; out[4+rl+1] = (uint8_t)sl;
    memcpy(out + 4 + rl + 2, s, sl);
    return 4 + inner;
}

static void bignum_add32(const uint8_t* a, const uint8_t* b, uint8_t* out,
                         int* carry_out) {
    int carry = 0;
    for (int i = 31; i >= 0; i--) {
        int v = a[i] + b[i] + carry;
        out[i] = (uint8_t)(v & 0xFF);
        carry = v >> 8;
    }
    if (carry_out) *carry_out = carry;
}

int main(void) {
    printf("== EC/RSA verifier strictness ==\n");
    static uint8_t int_der[2048], leaf_der[2048];
    int inl = load("tests/adversarial/at_int.der", int_der, sizeof(int_der));
    int lfl = load("tests/adversarial/at_leaf.der", leaf_der, sizeof(leaf_der));
    CHECK(inl > 0 && lfl > 0, "fixtures load");
    if (inl <= 0 || lfl <= 0) return 1;

    x509_cert intc, leafc;
    CHECK(x509_parse(int_der, (uint32_t)inl, &intc) == 0, "int parses");
    CHECK(x509_parse(leaf_der, (uint32_t)lfl, &leafc) == 0, "leaf parses");

    // Control: issuer key verifies leaf TBS/signature (mirrors certverify).
    int ctrl = ec_verify(X509_SIG_ECDSA_SHA256,
                         intc.ec_point, intc.ec_point_len,
                         leafc.tbs.p, leafc.tbs.len,
                         leafc.signature.p, leafc.signature.len);
    CHECK(ctrl == 0, "control verifies");

    const uint8_t *r, *s;
    uint32_t rl, sl;
    CHECK(sig_rs(leafc.signature.p, leafc.signature.len, &r, &rl, &s, &sl) == 0,
          "sig parses");
    static uint8_t mut[160];
    uint32_t ml;

    // (a) non-canonical pad: prepend an extra 0x00 to r.
    {
        static uint8_t rp[40];
        rp[0] = 0x00; memcpy(rp + 1, r, rl);
        ml = sig_enc(mut, rp, rl + 1, s, sl);
        CHECK(ml > 0 && ec_verify(X509_SIG_ECDSA_SHA256, intc.ec_point,
                                  intc.ec_point_len, leafc.tbs.p, leafc.tbs.len,
                                  mut, ml) != 0, "padded r rejected");
    }
    // (b) r + n (valid DER, out of range) must not fold into r.
    {
        static uint8_t rp[40];
        uint8_t sum[32];
        int carry = 0;
        // normalize r to 32 bytes (strip the single legal pad if present)
        uint8_t r32[32]; memset(r32, 0, 32);
        const uint8_t* rv = r; uint32_t rvl = rl;
        if (rvl == 33 && rv[0] == 0x00) { rv++; rvl--; }
        if (rvl > 32) { CHECK(0, "test setup: r fits 32B"); return 1; }
        memcpy(r32 + 32 - rvl, rv, rvl);
        bignum_add32(r32, P256_N, sum, &carry);
        uint32_t off = 0, suml = 32;
        if (carry) { rp[0] = 0x01; memcpy(rp + 1, sum, 32); suml = 33; off = 0; ml = sig_enc(mut, rp, suml, s, sl); }
        else {
            // pad if high bit set (DER positivity), else raw
            if (sum[0] & 0x80) { rp[0] = 0; memcpy(rp + 1, sum, 32); ml = sig_enc(mut, rp, 33, s, sl); }
            else ml = sig_enc(mut, sum, 32, s, sl);
            off = 0;
        }
        (void)off;
        CHECK(ml > 0 && ec_verify(X509_SIG_ECDSA_SHA256, intc.ec_point,
                                  intc.ec_point_len, leafc.tbs.p, leafc.tbs.len,
                                  mut, ml) != 0, "r+n rejected");
    }
    // (c) zero r.
    {
        static uint8_t z[1] = { 0x00 };
        ml = sig_enc(mut, z, 1, s, sl);
        CHECK(ml > 0 && ec_verify(X509_SIG_ECDSA_SHA256, intc.ec_point,
                                  intc.ec_point_len, leafc.tbs.p, leafc.tbs.len,
                                  mut, ml) != 0, "zero r rejected");
    }
    // (d) negative r (strip the positivity pad when present; else force
    // high bit by setting it — both malformed).
    {
        const uint8_t* rv = r; uint32_t rvl = rl;
        static uint8_t rn[40];
        if (rvl > 1 && rv[0] == 0x00 && (rv[1] & 0x80)) { rv++; rvl--; }
        else { memcpy(rn, r, rl); rn[0] |= 0x80; rv = rn; }
        ml = sig_enc(mut, rv, rvl, s, sl);
        CHECK(ml > 0 && ec_verify(X509_SIG_ECDSA_SHA256, intc.ec_point,
                                  intc.ec_point_len, leafc.tbs.p, leafc.tbs.len,
                                  mut, ml) != 0, "negative r rejected");
    }
    // (e) coordinate >= p: point = 04 || p || valid-Y.
    {
        static uint8_t pt[65];
        pt[0] = 0x04;
        memcpy(pt + 1, P256_P, 32);
        memcpy(pt + 33, intc.ec_point + 33, 32);
        CHECK(ec_verify(X509_SIG_ECDSA_SHA256, pt, 65,
                        leafc.tbs.p, leafc.tbs.len,
                        leafc.signature.p, leafc.signature.len) != 0,
              "x == p rejected");
    }
    // (f) compressed prefix misread: 0x02 || X || Y must not verify as raw.
    {
        static uint8_t pt[65];
        memcpy(pt, intc.ec_point, 65);
        pt[0] = 0x02;
        CHECK(ec_verify(X509_SIG_ECDSA_SHA256, pt, 65,
                        leafc.tbs.p, leafc.tbs.len,
                        leafc.signature.p, leafc.signature.len) != 0,
              "compressed prefix rejected");
    }

    // ---- RSA exponent discipline (review #15) ----
    static uint8_t rsader[2048];
    int rsl = load("tests/adversarial/at_rsa_leaf.der", rsader, sizeof(rsader));
    CHECK(rsl > 0, "rsa leaf loads");
    x509_cert rsac;
    CHECK(x509_parse(rsader, (uint32_t)rsl, &rsac) == 0, "rsa leaf parses");
    {
        rsa_pub k;
        CHECK(rsa_pub_from_x509(&rsac, &k) == 0, "65537 control loads");
    }
    {
        // e variants against the REAL 2048-bit modulus.
        struct { const char* name; uint8_t e[40]; uint32_t el; } cases[] = {
            { "e=1", { 0x01 }, 1 },
            { "e=2", { 0x02 }, 1 },
            { "e=even", { 0x01, 0x00, 0x00 }, 3 },
            { "e=0", { 0x00 }, 1 },
            { "e>32b", { [0 ... 39] = 0x01 }, 40 },
        };
        for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            x509_cert m = rsac;
            m.rsa_e = cases[i].e; m.rsa_e_len = cases[i].el;
            rsa_pub k;
            char nm[64];
            snprintf(nm, sizeof(nm), "rsa %s rejected", cases[i].name);
            CHECK(rsa_pub_from_x509(&m, &k) != 0, nm);
        }
    }

    if (fails == 0) printf("STRICT TESTS PASS: 0 failures\n");
    else printf("STRICT TESTS FAIL: %d failures\n", fails);
    return fails == 0 ? 0 : 1;
}
