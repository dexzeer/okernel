// Host test: the ALPN answer in EncryptedExtensions (h2 only when we offered it) and the ClientHello that offers it.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tls_handshake.h"

static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s\n", m); fails++; } } while (0)

// EncryptedExtensions body carrying one ALPN extension with the protocol string
static uint32_t ee_with(uint8_t* b, const char* proto) {
    uint32_t pl = (uint32_t)strlen(proto), o = 2;
    b[o++] = 0x00; b[o++] = 0x10;                        // ALPN
    uint32_t elen = 2 + 1 + pl;
    b[o++] = (uint8_t)(elen >> 8); b[o++] = (uint8_t)elen;
    b[o++] = (uint8_t)((1 + pl) >> 8); b[o++] = (uint8_t)(1 + pl);
    b[o++] = (uint8_t)pl;
    memcpy(b + o, proto, pl); o += pl;
    b[0] = (uint8_t)((o - 2) >> 8); b[1] = (uint8_t)(o - 2);
    return o;
}

int main(void) {
    uint8_t ee[64];
    int h2 = -1;
    uint32_t n = ee_with(ee, "http/1.1");
    CHECK(tls_parse_ee_validate(ee, n) == 0, "http/1.1 is accepted (old entry point)");
    CHECK(tls_parse_ee_validate_alpn(ee, n, 1, &h2) == 0 && h2 == 0, "http/1.1 is accepted, not h2");
    n = ee_with(ee, "h2");
    CHECK(tls_parse_ee_validate(ee, n) != 0, "h2 is refused when we did not offer it");
    CHECK(tls_parse_ee_validate_alpn(ee, n, 0, &h2) != 0, "...also through the new entry point");
    CHECK(tls_parse_ee_validate_alpn(ee, n, 1, &h2) == 0 && h2 == 1, "h2 is accepted when we offered it");
    n = ee_with(ee, "spdy/3");
    CHECK(tls_parse_ee_validate_alpn(ee, n, 1, &h2) != 0, "an unknown protocol is refused");
    n = ee_with(ee, "h3");
    CHECK(tls_parse_ee_validate_alpn(ee, n, 1, &h2) != 0, "h3 is refused");
    uint8_t none[2] = { 0, 0 };
    CHECK(tls_parse_ee_validate_alpn(none, 2, 1, &h2) == 0 && h2 == 0, "no ALPN at all is fine");

    uint8_t rnd[32] = { 1 }, sid[32] = { 2 }, pub[32] = { 3 }, ch[1024];
    tls_set_offer_h2(0);
    uint32_t a = tls_build_client_hello(ch, sizeof ch, rnd, sid, pub, "example.com");
    int has_h2_a = 0;
    for (uint32_t i = 0; i + 3 < a; i++) if (ch[i] == 2 && ch[i + 1] == 'h' && ch[i + 2] == '2' && ch[i + 3] == 8) has_h2_a = 1;
    tls_set_offer_h2(1);
    uint32_t b = tls_build_client_hello(ch, sizeof ch, rnd, sid, pub, "example.com");
    int has_h2_b = 0;
    for (uint32_t i = 0; i + 3 < b; i++) if (ch[i] == 2 && ch[i + 1] == 'h' && ch[i + 2] == '2' && ch[i + 3] == 8) has_h2_b = 1;
    tls_set_offer_h2(0);
    CHECK(a > 0 && !has_h2_a, "default ClientHello offers only http/1.1");
    CHECK(b == a + 3 && has_h2_b, "with the flag it offers h2 first and http/1.1 second (3 bytes longer)");
    printf(fails ? "ALPN TESTS FAIL\n" : "ALPN TESTS PASS\n");
    return fails != 0;
}
