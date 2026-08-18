// Phase 2 host test: handshake codec + transcript hash.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tls_handshake.h"

static int fails = 0;

static void ok(const char* name, int cond) {
    printf("%-32s %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main(void) {
    // ---- 1. ClientHello builds with the expected fields ----
    uint8_t random[32];
    for (int i = 0; i < 32; i++) random[i] = (uint8_t)(0x40 + i);
    uint8_t pub[32];
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0x80 + i);
    uint8_t hello[512];
    uint32_t hello_len = tls_build_client_hello(hello, sizeof(hello),
                                                random, pub, "example.com");
    ok("ClientHello builds", hello_len > 0);
    ok("ClientHello type byte", hello[0] == TLS_HS_CLIENT_HELLO);
    uint32_t body_len = ((uint32_t)hello[1] << 16) | ((uint32_t)hello[2] << 8) | hello[3];
    ok("ClientHello body length", body_len == hello_len - 4);
    // Spot-check: legacy_version 0x0303 at byte 4,5
    ok("ClientHello legacy_version", hello[4] == 0x03 && hello[5] == 0x03);
    // Spot-check: random at 6..37
    ok("ClientHello random copy", memcmp(hello + 6, random, 32) == 0);
    // Spot-check: session_id_len = 0 at byte 38
    ok("ClientHello session_id empty", hello[38] == 0);
    // Spot-check: cipher suite at 39..42 (len=2, value=0x1303)
    ok("ClientHello cs len",   hello[39] == 0 && hello[40] == 2);
    ok("ClientHello cs value", hello[41] == 0x13 && hello[42] == 0x03);
    // legacy_compression: len=1, value=0x00 at bytes 43,44
    ok("ClientHello comp",     hello[43] == 1 && hello[44] == 0);
    // Extensions start at 45: 2-byte length
    uint32_t ext_off = 45;
    uint32_t ext_len = ((uint32_t)hello[ext_off] << 8) | hello[ext_off + 1];
    ok("ClientHello ext len sane", ext_len > 0 && ext_len + ext_off + 2 == hello_len);

    // Scan extensions to find supported_versions, key_share, SNI, ALPN
    int have_sv = 0, have_ks = 0, have_sni = 0, have_alpn = 0;
    uint32_t p = ext_off + 2, eend = ext_off + 2 + ext_len;
    while (p + 4 <= eend) {
        uint16_t etype = ((uint16_t)hello[p] << 8) | hello[p+1];
        uint16_t elen  = ((uint16_t)hello[p+2] << 8) | hello[p+3];
        if (etype == TLS_EXT_SUPPORTED_VERSIONS && elen == 3) {
            // body: list_len(1) | 0x0304
            have_sv = (hello[p+4] == 2 && hello[p+5] == 0x03 && hello[p+6] == 0x04);
        } else if (etype == TLS_EXT_KEY_SHARE) {
            // body: list_len(2) | entry(group(2) | key_len(2) | key(32))
            if (elen >= 2 + 4 + 32) {
                uint16_t list_len = ((uint16_t)hello[p+4] << 8) | hello[p+5];
                if (list_len >= 4 + 32) {
                    uint16_t g = ((uint16_t)hello[p+6] << 8) | hello[p+7];
                    uint16_t kl = ((uint16_t)hello[p+8] << 8) | hello[p+9];
                    if (g == TLS_GROUP_X25519 && kl == 32 &&
                        memcmp(hello + p + 10, pub, 32) == 0) {
                        have_ks = 1;
                    }
                }
            }
        } else if (etype == TLS_EXT_SERVER_NAME) {
            // body: list_len(2) | entry(name_type(1) | name_len(2) | name)
            // find "example.com" bytes
            for (int i = 0; i + 10 <= elen; i++) {
                if (memcmp(hello + p + 4 + i, "example.com", 10) == 0) {
                    have_sni = 1; break;
                }
            }
        } else if (etype == TLS_EXT_APPLICATION_LAYER_PROTOCOL) {
            // body: list_len(2) | entry(proto_len(1) | proto)
            for (int i = 0; i + 8 <= elen; i++) {
                if (memcmp(hello + p + 4 + i, "http/1.1", 8) == 0) {
                    have_alpn = 1; break;
                }
            }
        }
        p += 4 + elen;
    }
    ok("ClientHello ext supported_versions", have_sv);
    ok("ClientHello ext key_share",          have_ks);
    ok("ClientHello ext SNI",                have_sni);
    ok("ClientHello ext ALPN",               have_alpn);

    // ---- 2. ClientHello without hostname omits SNI ----
    uint8_t hello2[512];
    uint32_t hello2_len = tls_build_client_hello(hello2, sizeof(hello2),
                                                 random, pub, NULL);
    ok("ClientHello nohost builds", hello2_len > 0 && hello2_len < hello_len);

    // ---- 3. Transcript hash: deterministic over known input ----
    // Compute SHA-256 of a known 32-byte string and compare against
    // Python's hashlib.
    tls_transcript tx;
    tls_transcript_init(&tx);
    uint8_t fixed[32];
    for (int i = 0; i < 32; i++) fixed[i] = (uint8_t)i;
    tls_transcript_update(&tx, fixed, 32);
    uint8_t h[32];
    tls_transcript_final(&tx, h);
    // python3 -c "import hashlib;print(hashlib.sha256(bytes(range(32))).hexdigest())"
    uint8_t want[32];
    static const char want_hex[] =
        "630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd";
    for (int i = 0; i < 32; i++) {
        char c = want_hex[2*i];   int hi = c >= 'a' ? c - 'a' + 10 : c - '0';
        c = want_hex[2*i+1];      int lo = c >= 'a' ? c - 'a' + 10 : c - '0';
        want[i] = (uint8_t)((hi << 4) | lo);
    }
    ok("transcript sha256 matches hashlib", memcmp(h, want, 32) == 0);

    // ---- 4. Transcript with handshake header + body ----
    // tls_transcript_update_msg hashes type(1) || len(3) || body
    tls_transcript tx2;
    tls_transcript_init(&tx2);
    uint8_t body[5] = {0x41, 0x42, 0x43, 0x44, 0x45};
    tls_transcript_update_msg(&tx2, TLS_HS_CLIENT_HELLO, body, 5);
    uint8_t h2[32];
    tls_transcript_final(&tx2, h2);
    // Compare against a raw update of the same header||body bytes.
    tls_transcript tx3;
    tls_transcript_init(&tx3);
    tls_transcript_update(&tx3, (const uint8_t*)"\x01\x00\x00\x05" "ABCDE", 9);
    uint8_t h3[32];
    tls_transcript_final(&tx3, h3);
    ok("update_msg == update of header||body", memcmp(h2, h3, 32) == 0);

    // ---- 5. ServerHello parse ----
    // Build a synthetic ServerHello body:
    // legacy_version(2) 0x0303 | random(32) | sid_len(1)=0 | cipher(2)=0x1303
    // | compression(1)=0 | ext_len(2)=N | exts
    //   - supported_versions (43) body=0x0304
    //   - key_share (51) group=0x001D, key_len=32, key=pub[]
    uint8_t sh[256];
    uint32_t sp = 0;
    sh[sp++] = 0x03; sh[sp++] = 0x03;
    for (int i = 0; i < 32; i++) sh[sp++] = (uint8_t)(0xa0 + i);
    sh[sp++] = 0;             // sid_len
    sh[sp++] = 0x13; sh[sp++] = 0x03;
    sh[sp++] = 0;             // compression
    uint32_t exts_start = sp;
    sp += 2;
    // supported_versions ext
    sh[sp++] = 0x00; sh[sp++] = 0x2b; sh[sp++] = 0x00; sh[sp++] = 0x02;
    sh[sp++] = 0x03; sh[sp++] = 0x04;
    // key_share ext
    sh[sp++] = 0x00; sh[sp++] = 0x33; sh[sp++] = 0x00; sh[sp++] = 36;
    sh[sp++] = 0x00; sh[sp++] = 0x1d; sh[sp++] = 0x00; sh[sp++] = 32;
    for (int i = 0; i < 32; i++) sh[sp++] = (uint8_t)(0xc0 + i);
    // patch ext_len
    ((uint8_t*)(sh + exts_start))[0] = (uint8_t)((sp - exts_start - 2) >> 8);
    ((uint8_t*)(sh + exts_start))[1] = (uint8_t)((sp - exts_start - 2) & 0xff);

    tls_server_hello parsed;
    int rc = tls_parse_server_hello(sh, sp, &parsed);
    ok("ServerHello parses", rc == 0);
    ok("ServerHello cipher", parsed.cipher_suite == TLS_CIPHER_CHACHA20_POLY1305_SHA256);
    ok("ServerHello group",  parsed.named_group == TLS_GROUP_X25519);
    uint8_t expected_pub[32];
    for (int i = 0; i < 32; i++) expected_pub[i] = (uint8_t)(0xc0 + i);
    ok("ServerHello pubkey", memcmp(parsed.key_share, expected_pub, 32) == 0);

    // ---- 6. ServerHello rejects malformed ----
    uint8_t bad_sh[256];
    memcpy(bad_sh, sh, sp);
    bad_sh[0] = 0x03; bad_sh[1] = 0x04;   // wrong legacy_version
    ok("ServerHello rejects bad ver", tls_parse_server_hello(bad_sh, sp, &parsed) == -1);
    memcpy(bad_sh, sh, sp);
    bad_sh[2 + 32] = 33;                  // sid_len > 32
    ok("ServerHello rejects sid_len", tls_parse_server_hello(bad_sh, sp, &parsed) == -1);

    // ---- 7. EncryptedExtensions + ALPN ----
    uint8_t ee[64];
    uint32_t ep = 0;
    // ext_len first, fill later
    uint32_t ee_ext_start = ep;
    ep += 2;
    // ALPN ext
    ee[ep++] = 0x00; ee[ep++] = 0x10;     // type = 16
    uint32_t alpn_ext_body_start = ep;
    ep += 2;                              // body len
    uint32_t alpn_list_start = ep;
    ep += 2;                              // list len
    ee[ep++] = 2;                         // proto_len
    ee[ep++] = 'h'; ee[ep++] = '2';
    // Patch list len
    ((uint8_t*)(ee + alpn_list_start))[0] = (uint8_t)((ep - alpn_list_start - 2) >> 8);
    ((uint8_t*)(ee + alpn_list_start))[1] = (uint8_t)((ep - alpn_list_start - 2) & 0xff);
    // Patch body len
    ((uint8_t*)(ee + alpn_ext_body_start))[0] = (uint8_t)((ep - alpn_ext_body_start - 2) >> 8);
    ((uint8_t*)(ee + alpn_ext_body_start))[1] = (uint8_t)((ep - alpn_ext_body_start - 2) & 0xff);
    // Patch ee ext len
    ((uint8_t*)(ee + ee_ext_start))[0] = (uint8_t)((ep - ee_ext_start - 2) >> 8);
    ((uint8_t*)(ee + ee_ext_start))[1] = (uint8_t)((ep - ee_ext_start - 2) & 0xff);

    const uint8_t* alpn; uint32_t alpn_len;
    rc = tls_parse_ee_alpn(ee, ep, &alpn, &alpn_len);
    ok("EE parses", rc == 0);
    ok("EE alpn len", alpn_len == 2);
    ok("EE alpn content", alpn[0] == 'h' && alpn[1] == '2');

    // ---- 8. Certificate structural parse ----
    // body: list_len(3) | { entry_len(3) | entry_data | entry_ext_len(2) }
    // list_len = total bytes after list_len = entry_len_field + data + ext_len
    //          = 3 + 3 + 2 = 8
    uint8_t cert_msg[64];
    cert_msg[0] = 0; cert_msg[1] = 0; cert_msg[2] = 8; // list_len = 8
    cert_msg[3] = 0; cert_msg[4] = 0; cert_msg[5] = 3; // entry_len = 3
    cert_msg[6] = 1; cert_msg[7] = 2; cert_msg[8] = 3; // entry data
    cert_msg[9] = 0; cert_msg[10] = 0;                 // entry ext_len = 0
    ok("Certificate parses", tls_parse_certificate(cert_msg, 11) == 0);
    ok("Certificate rejects empty", tls_parse_certificate(cert_msg, 2) == -1);

    // ---- 9. CertificateVerify parse ----
    uint8_t cv[16];
    cv[0] = 0x04; cv[1] = 0x03;  // sig_alg ECDSA_SECP256R1_SHA256
    cv[2] = 0x00; cv[3] = 0x08;  // sig_len = 8
    for (int i = 0; i < 8; i++) cv[4 + i] = (uint8_t)i;
    ok("CertificateVerify parses", tls_parse_certificate_verify(cv, 12) == 0);
    ok("CertificateVerify rejects empty", tls_parse_certificate_verify(cv, 3) == -1);

    // ---- 10. Finished MAC round-trip ----
    uint8_t fin_key[32];
    for (int i = 0; i < 32; i++) fin_key[i] = (uint8_t)(0xe0 + i);
    uint8_t tx_hash[32];
    for (int i = 0; i < 32; i++) tx_hash[i] = (uint8_t)(0xf0 + i);
    uint8_t computed[32];
    tls_build_finished(fin_key, tx_hash, computed);
    ok("Finished verify true", tls_verify_finished(fin_key, tx_hash, computed) == 0);
    // Tamper one bit in computed MAC
    computed[5] ^= 0x01;
    ok("Finished verify false on tamper", tls_verify_finished(fin_key, tx_hash, computed) == -1);

    printf("\n%s\n", fails ? "PHASE 2 FAILED" : "PHASE 2 PASS");
    return fails != 0;
}