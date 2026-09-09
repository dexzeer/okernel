// Host test: X.509 parser + RSA PKCS#1 v1.5 + ECDSA P-256/P-384 against REAL
// certificates (the live example.com chain + system root CAs).
// Build: gcc -m32 -O2 -Isrc/crypto -Isrc -o t test_pki.c ../src/crypto/... 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x509.h"
#include "rsa.h"
#include "ec.h"
#include "certverify.h"

static uint8_t buf[16384];

static int load(const char* path, uint8_t* out, uint32_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("SKIP %s (missing)\n", path); return -1; }
    uint32_t n = (uint32_t)fread(out, 1, cap, f);
    fclose(f);
    return (int)n;
}

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("  PASS %s\n", name); } \
    else { printf("  FAIL %s\n", name); failures++; } \
} while (0)

// verify cert[i]'s signature with cert[j]'s public key
static int verify_with(int cert_alg, const x509_cert* cert, const x509_cert* key) {
    if (cert_alg == X509_SIG_RSA_SHA256 || cert_alg == X509_SIG_RSA_SHA384 ||
        cert_alg == X509_SIG_RSA_SHA512) {
        rsa_pub k;
        if (rsa_pub_from_x509(key, &k) != 0) return -2;
        return rsa_verify_pkcs1(&k, cert_alg, cert->tbs.p, cert->tbs.len,
                                cert->signature.p, cert->signature.len);
    }
    if (key->key_type != X509_KEY_EC_P256 && key->key_type != X509_KEY_EC_P384)
        return -2;
    return ec_verify(cert_alg, key->ec_point, key->ec_point_len,
                     cert->tbs.p, cert->tbs.len,
                     cert->signature.p, cert->signature.len);
}

static void dump_cert(const char* tag, const x509_cert* c) {
    printf("  [%s] key=%d sig_alg=%d san=%d ca=%d pathlen=%d siglen=%u\n",
           tag, c->key_type, c->sig_alg, c->san_count, c->is_ca, c->path_len,
           c->signature.len);
}

int main(void) {
    printf("== x509 parse: example.com chain ==\n");
    x509_cert leaf, int1, int2, rootx;
    int nl = load("tests/fixtures/example_leaf.der", buf, sizeof(buf));
    CHECK(nl > 0 && x509_parse(buf, (uint32_t)nl, &leaf) == 0, "parse leaf");
    dump_cert("leaf", &leaf);
    {
        uint8_t b2[16384];
        int n1 = load("tests/fixtures/example_int1.der", b2, sizeof(b2));
        CHECK(n1 > 0 && x509_parse(b2, (uint32_t)n1, &int1) == 0, "parse int1");
        dump_cert("int1", &int1);
        uint8_t b3[16384];
        int n2 = load("tests/fixtures/example_int2.der", b3, sizeof(b3));
        CHECK(n2 > 0 && x509_parse(b3, (uint32_t)n2, &int2) == 0, "parse int2");
        dump_cert("int2", &int2);
        uint8_t b4[16384];
        int n3 = load("tests/fixtures/example_root_cross.der", b4, sizeof(b4));
        CHECK(n3 > 0 && x509_parse(b4, (uint32_t)n3, &rootx) == 0, "parse root_cross");
        dump_cert("rootx", &rootx);

        printf("== chain signature verification ==\n");
        CHECK(verify_with(leaf.sig_alg, &leaf, &int1) == 0, "leaf <- int1 (ECDSA P-256/SHA256)");
        CHECK(verify_with(int1.sig_alg, &int1, &int2) == 0, "int1 <- int2 (ECDSA P-384/SHA384)");
        CHECK(verify_with(int2.sig_alg, &int2, &rootx) == 0, "int2 <- root (ECDSA P-384/SHA384)");

        printf("== tamper detection ==\n");
        {
            uint8_t b2c[16384];
            memcpy(b2c, b2, (size_t)n1);
            // flip a byte INSIDE int1's TBS (byte 500 — well within the
            // signed region; the tail of the DER is the outer alg + signature)
            b2c[500] ^= 0x01;
            x509_cert int1_t;
            if (x509_parse(b2c, (uint32_t)n1, &int1_t) == 0) {
                CHECK(verify_with(int1_t.sig_alg, &int1_t, &int2) != 0,
                      "tampered int1 rejected");
            } else {
                CHECK(1, "tampered int1 rejected (parse refused)");
            }
            // outer/inner signature algorithm mismatch must be refused
            // (outer sig-alg OID content lives at DER offset 629..636 for
            // this fixture — verified via openssl asn1parse)
            memcpy(b2c, b2, (size_t)n1);
            b2c[630] ^= 0xFF;
            if (x509_parse(b2c, (uint32_t)n1, &int1_t) != 0) {
                CHECK(1, "outer/inner alg mismatch refused");
            } else {
                CHECK(0, "outer/inner alg mismatch refused");
            }
        }

        printf("== SAN / hostname ==\n");
        CHECK(x509_hostname_match(&leaf, "example.com") == 0, "match example.com");
        CHECK(x509_hostname_match(&leaf, "WWW.Example.COM") == 0, "match case-insensitive");
        CHECK(x509_hostname_match(&leaf, "a.example.com") == 0, "match *.example.com");
        CHECK(x509_hostname_match(&leaf, "bad.example.com.attacker.io") != 0, "reject suffix spoof");
        CHECK(x509_hostname_match(&leaf, "evIl-example.com") != 0, "reject partial label");
        CHECK(x509_hostname_match(&leaf, "deep.a.example.com") != 0, "reject multi-label wildcard");

        printf("== validity window ==\n");
        x509_time mid = { 2026, 9, 9, 12, 0, 0 };
        x509_set_now(&mid);
        CHECK(x509_time_cmp(&mid, &leaf.not_before) > 0, "now > notBefore");
        CHECK(x509_time_cmp(&mid, &leaf.not_after) < 0, "now < notAfter");
        x509_time past = { 2020, 1, 1, 0, 0, 0 };
        CHECK(x509_time_cmp(&past, &leaf.not_before) < 0, "2020 < notBefore");
        x509_time future = { 2030, 1, 1, 0, 0, 0 };
        CHECK(x509_time_cmp(&future, &leaf.not_after) > 0, "2030 > notAfter");
    }

    printf("== RSA verify vs real roots ==\n");
    {
        struct { const char* file; int self_alg; } roots[] = {
            { "tests/fixtures/ISRG_Root_X1.der", X509_SIG_RSA_SHA256 },
            { "tests/fixtures/DigiCert_Global_Root_G2.der", X509_SIG_RSA_SHA256 },
            { "tests/fixtures/Amazon_Root_CA_1.der", X509_SIG_RSA_SHA256 },
            { "tests/fixtures/USERTrustRSACertificationAuthority.der", X509_SIG_RSA_SHA384 },
            { "tests/fixtures/GlobalSign_Root_CA__R3.der", X509_SIG_RSA_SHA256 },
            { "tests/fixtures/SSL.com_TLS_RSA_Root_CA_2022.der", X509_SIG_RSA_SHA256 },
        };
        for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            uint8_t b[16384];
            int n = load(roots[i].file, b, sizeof(b));
            if (n < 0) continue;
            x509_cert rc;
            if (x509_parse(b, (uint32_t)n, &rc) != 0) {
                char nm[128]; snprintf(nm, sizeof(nm), "parse %s", roots[i].file);
                CHECK(0, nm); continue;
            }
            char nm[128];
            snprintf(nm, sizeof(nm), "self-verify %s (%s)", roots[i].file,
                     rc.key_type == X509_KEY_RSA ? "RSA" : "EC");
            CHECK(verify_with(rc.sig_alg, &rc, &rc) == 0, nm);
        }
    }

    printf("== ECDSA verify vs real ECC roots ==\n");
    {
        struct { const char* file; } roots[] = {
            { "tests/fixtures/SSL.com_TLS_ECC_Root_CA_2022.der" },
            { "tests/fixtures/ISRG_Root_X2.der" },
            { "tests/fixtures/GTS_Root_R1.der" },
            { "tests/fixtures/GTS_Root_R4.der" },
            { "tests/fixtures/Amazon_Root_CA_3.der" },
            { "tests/fixtures/USERTrustECCCertificationAuthority.der" },
        };
        for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            uint8_t b[16384];
            int n = load(roots[i].file, b, sizeof(b));
            if (n < 0) continue;
            x509_cert rc;
            if (x509_parse(b, (uint32_t)n, &rc) != 0) {
                char nm[128]; snprintf(nm, sizeof(nm), "parse %s", roots[i].file);
                CHECK(0, nm); continue;
            }
            char nm[128];
            snprintf(nm, sizeof(nm), "self-verify %s", roots[i].file);
            CHECK(verify_with(rc.sig_alg, &rc, &rc) == 0, nm);
        }
    }

    printf("== full chain verification via cert_verify ==\n");
    {
        // Build a TLS 1.3 Certificate message body from the live chain:
        // ctx_len(1)=0 | list_len(3) | entries { len(3) | DER | ext(2)=0 }
        static uint8_t msg[32768];
        uint32_t mp = 0;
        msg[mp++] = 0;  // empty certificate_request_context
        uint32_t lenpos = mp; mp += 3;
        const char* files[] = {
            "tests/fixtures/example_leaf.der",
            "tests/fixtures/example_int1.der",
            "tests/fixtures/example_int2.der",
            "tests/fixtures/example_root_cross.der",
        };
        for (unsigned i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
            int cn = load(files[i], buf, sizeof(buf));
            if (cn <= 0) { printf("  SKIP missing %s\n", files[i]); continue; }
            msg[mp++] = (uint8_t)((uint32_t)cn >> 16);
            msg[mp++] = (uint8_t)((uint32_t)cn >> 8);
            msg[mp++] = (uint8_t)(cn & 0xff);
            memcpy(msg + mp, buf, (size_t)cn); mp += (uint32_t)cn;
            msg[mp++] = 0; msg[mp++] = 0;   // no entry extensions
        }
        msg[lenpos]   = 0;
        msg[lenpos+1] = (uint8_t)(((mp - lenpos - 3) >> 8) & 0xff);
        msg[lenpos+2] = (uint8_t)((mp - lenpos - 3) & 0xff);

        x509_time now = { 2026, 9, 9, 14, 0, 0 };
        x509_set_now(&now);
        int r = cert_verify(msg, mp, "example.com");
        CHECK(r == CV_OK, "cert_verify(example.com) full chain OK");
        if (r != CV_OK)
            printf("    -> %s\n", cert_verify_strerror(r));
        r = cert_verify(msg, mp, "attacker.example.io");
        CHECK(r == CV_ERR_HOSTNAME, "cert_verify rejects wrong hostname");
        r = cert_verify(msg, mp, "a.b.example.com");
        CHECK(r == CV_ERR_HOSTNAME, "cert_verify rejects deep wildcard");
        x509_time past = { 2020, 1, 1, 0, 0, 0 };
        x509_set_now(&past);
        r = cert_verify(msg, mp, "example.com");
        CHECK(r == CV_ERR_NOT_YET, "cert_verify rejects when clock is 2020");
        x509_time future = { 2031, 1, 1, 0, 0, 0 };
        x509_set_now(&future);
        r = cert_verify(msg, mp, "example.com");
        CHECK(r == CV_ERR_EXPIRED, "cert_verify rejects when clock is 2031");
        x509_time now2 = { 2026, 9, 9, 14, 0, 0 };
        x509_set_now(&now2);
    }

    printf("\n%s: %d failures\n", failures == 0 ? "PKI TESTS PASS" : "PKI TESTS FAIL", failures);
    return failures == 0 ? 0 : 1;
}
