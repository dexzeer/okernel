// Phase 1 host test: TLS record layer round-trip + malformed rejects.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tls_record.h"

static int fails = 0;

static void ok(const char* name, int cond) {
    printf("%-32s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main(void) {
    // ---- build + parse round-trip ----
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) payload[i] = (uint8_t)i;

    uint8_t out[5 + 256];
    uint32_t total = tls_record_build(TLS_CT_HANDSHAKE, payload, 256, out);
    ok("build length", total == 5 + 256);
    ok("build type byte",   out[0] == TLS_CT_HANDSHAKE);
    ok("build version hi",  out[1] == 0x03);
    ok("build version lo",  out[2] == 0x03);
    ok("build len hi",      out[3] == 0x01);
    ok("build len lo",      out[4] == 0x00);
    ok("build payload copy", memcmp(out + 5, payload, 256) == 0);

    tls_record rec;
    uint32_t hdr_len = tls_record_parse_header(out, total, &rec);
    ok("parse header len", hdr_len == 5);
    ok("parse type",       rec.type == TLS_CT_HANDSHAKE);
    ok("parse version",    rec.version == TLS_VERSION_TLS12);
    ok("parse payload len", rec.payload_len == 256);
    ok("parse payload ptr", rec.payload == out + 5);
    ok("parse payload eq",  memcmp(rec.payload, payload, 256) == 0);

    // ---- parse from a TCP-style chunk (no truncation) ----
    // Simulate the record arriving as two consecutive TCP payloads: the
    // first one carries the header + first 100 bytes; the second one
    // carries the rest. After concatenation into a single contiguous
    // buffer, parse must succeed and the payload pointer must equal the
    // start of the payload bytes.
    uint8_t pkt[5 + 256];
    memcpy(pkt,       out,     5 + 100);     // segment 1
    memcpy(pkt + 105, out + 105, 256 - 100); // segment 2
    tls_record r2;
    uint32_t h2 = tls_record_parse_header(pkt, sizeof(pkt), &r2);
    ok("two-segment parse header", h2 == 5 && r2.payload_len == 256 &&
        memcmp(r2.payload, payload, 256) == 0);

    // ---- rejects ----
    // 1. buffer too short for header
    ok("reject too-short", tls_record_parse_header(out, 4, &rec) == 0);
    // 2. unknown type
    uint8_t bad[16] = {0x99, 0x03, 0x03, 0x00, 0x01, 0xaa};
    ok("reject unknown type", tls_record_parse_header(bad, 6, &rec) == 0);
    // 3. bad version
    uint8_t badver[6] = {TLS_CT_HANDSHAKE, 0x04, 0x01, 0x00, 0x01, 0xaa};
    ok("reject bad version", tls_record_parse_header(badver, 6, &rec) == 0);
    // 4. oversize length (claim 20000 bytes, only 10 here)
    uint8_t big[20] = {TLS_CT_HANDSHAKE, 0x03, 0x03, 0xff, 0xff, 0};
    ok("reject oversize length", tls_record_parse_header(big, sizeof(big), &rec) == 0);
    // 5. truncated payload (claim 100, provide 5+50). The header-only parser
    // can't detect this — caller reads payload bytes separately.
    uint8_t trunc[60] = {0};
    trunc[0] = TLS_CT_HANDSHAKE; trunc[1] = 0x03; trunc[2] = 0x03;
    trunc[3] = 0x00; trunc[4] = 100;
    tls_record trunc_rec;
    ok("truncated header parses (detected at payload read)",
       tls_record_parse_header(trunc, 5, &trunc_rec) == 5);
    ok("truncated claims len=100", trunc_rec.payload_len == 100);
    // 6. exactly at the size limit
    uint32_t exact = tls_record_build(TLS_CT_APPDATA, NULL, 0, out);
    ok("build zero-len", exact == 5);
    tls_record r3;
    ok("parse zero-len", tls_record_parse_header(out, 5, &r3) == 5 &&
        r3.payload_len == 0);

    // 7. TLS 1.3 version on plaintext (legacy_version must be 0x0303,
    // but supported_versions extension carries 0x0304 — wire version is
    // always 0x0303 in TLS 1.3).
    uint8_t v13[6] = {TLS_CT_HANDSHAKE, 0x03, 0x04, 0x00, 0x01, 0xaa};
    tls_record r4;
    ok("accept TLS 1.3 version", tls_record_parse_header(v13, 6, &r4) == 5);

    printf("\n%s\n", fails ? "PHASE 1 FAILED" : "PHASE 1 PASS");
    return fails != 0;
}