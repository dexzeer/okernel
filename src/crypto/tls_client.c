#include "tls_client.h"
#include "tls_record.h"
#include "tls_handshake.h"
#include "tls_keysched.h"
#include "x25519.h"
#include "aead.h"
#include "hmac.h"
#include <string.h>
#include "tls_dbg.h"

#ifdef KERNEL
#include "rand.h"
#endif

// ----- entropy -----
// Host builds use a tiny xorshift; the kernel uses the seeded ChaCha20 CPRNG
// (rand.c), which is fed RDTSC + IRQ jitter + NIC MAC at boot.
#ifndef KERNEL
static uint64_t host_rng_state = 0x123456789abcdef0ull;
static void host_rng(uint8_t* out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint64_t x = host_rng_state;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        host_rng_state = x;
        out[i] = (uint8_t)(x & 0xff);
    }
}
#endif

static void tls_fill_random(uint8_t* out, uint32_t n) {
#ifdef KERNEL
    if (rand_bytes(out, n) != 1) {
        // CPRNG not yet ready (shouldn't happen post-boot) — degenerate fill
        for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)(i * 0x6D + 0x13);
    }
#else
    host_rng(out, n);
#endif
}

int shared_is_zero(const uint8_t s[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= s[i];
    return acc == 0;
}

static uint32_t recv_record(uint8_t* out, uint32_t cap,
                            const struct tls_client_io* io) {
    if (cap < 5) return 0;
    int n = io->recv(out, 5, 5000, io->user);
    if (n != 5) { tls_dbg( "[recv_record] hdr read=%d/5\n", n); return 0; }

    tls_record rec;
    if (tls_record_parse_header(out, 5, &rec) != 5) {
        tls_dbg( "[recv_record] header parse failed type=%u ver=%u\n", rec.type, rec.version);
        return 0;
    }
    if (cap < 5 + rec.payload_len) return 0;
    if (rec.payload_len > 0) {
        int r = io->recv(out + 5, rec.payload_len, 5000, io->user);
        if (r != (int)rec.payload_len) { tls_dbg( "[recv_record] payload read=%d/%u\n", r, rec.payload_len); return 0; }
    }
    return 5 + rec.payload_len;
}

static int send_record(uint8_t type, const uint8_t* payload, uint32_t plen,
                       const struct tls_client_io* io) {
    uint8_t buf[5 + 18432];
    uint32_t total = tls_record_build(type, payload, plen, buf);
    if (total == 0) return -1;
    return io->send(buf, total, io->user);
}

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
    // RFC 8446 §5.3: nonce = static_iv XOR seq (seq as big-endian uint64,
    // left-padded to 12 bytes — high byte of seq lands at nonce[4]).
    memcpy(nonce, iv, 12);
    nonce[4] ^= (uint8_t)((seq >> 56) & 0xff);
    nonce[5] ^= (uint8_t)((seq >> 48) & 0xff);
    nonce[6] ^= (uint8_t)((seq >> 40) & 0xff);
    nonce[7] ^= (uint8_t)((seq >> 32) & 0xff);
    nonce[8] ^= (uint8_t)((seq >> 24) & 0xff);
    nonce[9] ^= (uint8_t)((seq >> 16) & 0xff);
    nonce[10] ^= (uint8_t)((seq >> 8) & 0xff);
    nonce[11] ^= (uint8_t)(seq & 0xff);
}

static int send_aead(uint8_t key[32], const uint8_t iv[12], uint64_t* seq,
                     uint8_t ct_type, const uint8_t* pt, uint32_t pt_len,
                     const struct tls_client_io* io) {
    uint8_t inner[18432 + 1];
    if (pt_len + 1 > sizeof(inner)) return -1;
    memcpy(inner, pt, pt_len);
    inner[pt_len] = ct_type;

    uint8_t nonce[12]; make_nonce(nonce, iv, *seq); (*seq)++;

    uint8_t ct[18432 + 16 + 1];
    uint8_t tag[16];
    uint8_t aad[5];
    uint32_t ct_len = pt_len + 1 + 16;
    aad[0] = TLS_CT_APPDATA; aad[1] = 0x03; aad[2] = 0x03;
    aad[3] = (uint8_t)(ct_len >> 8); aad[4] = (uint8_t)(ct_len & 0xff);
    aead_chacha20_poly1305_encrypt(key, nonce, aad, 5,
                                   inner, pt_len + 1, ct, tag);
    memcpy(ct + pt_len + 1, tag, 16);

    uint8_t hdr[5];
    hdr[0] = TLS_CT_APPDATA; hdr[1] = 0x03; hdr[2] = 0x03;
    hdr[3] = (uint8_t)(ct_len >> 8); hdr[4] = (uint8_t)(ct_len & 0xff);
    if (io->send(hdr, 5, io->user) != 0) return -1;
    if (io->send(ct, ct_len, io->user) != 0) return -1;
    return 0;
}

static int recv_aead(uint8_t key[32], const uint8_t iv[12], uint64_t* seq,
                     uint8_t* out, uint32_t out_cap, uint8_t* ct_type,
                     const struct tls_client_io* io) {
    for (;;) {
        uint8_t rec[18432 + 32];
        uint32_t total = recv_record(rec, sizeof(rec), io);
        if (total == 0) return -1;
        tls_record v;
        if (tls_record_parse_header(rec, total, &v) != 5) return -1;
        if (v.type == TLS_CT_CHANGE_CIPHER_SPEC) continue;
        if (v.type == TLS_CT_ALERT) return -1;
        if (v.type != TLS_CT_APPDATA) return -1;
        if (v.payload_len < 16) return -1;
        uint32_t ct_len = v.payload_len - 16;
        uint8_t nonce[12]; make_nonce(nonce, iv, *seq); (*seq)++;
        uint8_t aad[5]; memcpy(aad, rec, 5);
        if (ct_len > out_cap) return -1;
        int rc = aead_chacha20_poly1305_decrypt(key, nonce, aad, 5,
                                                v.payload, ct_len,
                                                v.payload + ct_len, out);
        if (rc != 0) { tls_dbg( "[tls] AEAD decrypt fail\n"); return -1; }
        int plen = ct_len;
        while (plen > 0 && out[plen - 1] == 0) plen--;
        if (plen == 0) return -1;
        *ct_type = out[plen - 1];
        return plen - 1;
    }
}

static uint32_t parse_hs(const uint8_t* buf, uint32_t buf_len,
                         uint8_t* t, uint32_t* bl) {
    if (buf_len < 4) return 0;
    *t = buf[0];
    *bl = ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    if (buf_len < 4 + *bl) return 0;
    return 4 + *bl;
}

// Run a transcript over a series of (hs_type, body, body_len) triples,
// including the initial ClientHello.
static void transcript_of(const uint8_t* ch, uint32_t ch_len,
                          const uint8_t* sh_body, uint32_t sh_bl,
                          const uint8_t* ee_body, uint32_t ee_bl,
                          const uint8_t* cert_body, uint32_t cert_bl,
                          const uint8_t* cv_body, uint32_t cv_bl,
                          const uint8_t* fin_body, uint32_t fin_bl,
                          uint8_t out[32]) {
    tls_transcript t;
    tls_transcript_init(&t);
    // ch is the full ClientHello message (header + body). Pass body only.
    tls_transcript_update_msg(&t, TLS_HS_CLIENT_HELLO, ch + 4, ch_len - 4);
    tls_transcript_update_msg(&t, TLS_HS_SERVER_HELLO, sh_body, sh_bl);
    if (ee_body)  tls_transcript_update_msg(&t, TLS_HS_ENCRYPTED_EXTENSIONS,
                                            ee_body, ee_bl);
    if (cert_body) tls_transcript_update_msg(&t, TLS_HS_CERTIFICATE,
                                             cert_body, cert_bl);
    if (cv_body)   tls_transcript_update_msg(&t, TLS_HS_CERTIFICATE_VERIFY,
                                             cv_body, cv_bl);
    if (fin_body)  tls_transcript_update_msg(&t, TLS_HS_FINISHED,
                                             fin_body, fin_bl);
    tls_transcript_final(&t, out);
}

int tls_client_run(const char* host, uint16_t port,
                   const uint8_t* request, uint32_t request_len,
                   uint8_t* out, uint32_t out_cap,
                   const struct tls_client_io* io) {
    (void)port;
    tls_dbg( "[tls] start, host=%s\n", host);

    uint8_t priv[32]; tls_fill_random(priv, 32);
    priv[0] &= 248; priv[31] &= 127; priv[31] |= 64;
    uint8_t pub[32]; x25519_public_key(pub, priv);
    uint8_t random[32]; tls_fill_random(random, 32);

    uint8_t ch[1024];
    uint32_t ch_len = tls_build_client_hello(ch, sizeof(ch), random, pub, host);
    if (ch_len == 0) { tls_dbg( "[tls] CH build failed\n"); return -1; }
    tls_dbg( "[tls] CH built len=%u\n", ch_len);

    if (send_record(TLS_CT_HANDSHAKE, ch, ch_len, io) != 0) return -1;
    tls_dbg( "[tls] sent ClientHello\n");

    // Receive ServerHello
    uint8_t rec_buf[18432 + 32];
    uint32_t total = recv_record(rec_buf, sizeof(rec_buf), io);
    if (total == 0) { tls_dbg( "[tls] recv SH failed\n"); return -1; }
    tls_record rec_v;
    if (tls_record_parse_header(rec_buf, total, &rec_v) != 5) return -1;
    if (rec_v.type != TLS_CT_HANDSHAKE) { tls_dbg( "[tls] non-HS type=%u\n", rec_v.type); return -1; }
    uint8_t hs_t; uint32_t hs_bl;
    uint32_t consumed = parse_hs(rec_v.payload, rec_v.payload_len, &hs_t, &hs_bl);
    if (consumed == 0 || hs_t != TLS_HS_SERVER_HELLO) return -1;
    tls_server_hello sh;
    if (tls_parse_server_hello(rec_v.payload + 4, hs_bl, &sh) != 0) {
        tls_dbg( "[tls] parse SH failed\n"); return -1; }
    const uint8_t* sh_body = rec_v.payload + 4;
    tls_dbg( "[tls] got SH, cipher=0x%04x group=0x%04x\n",
            sh.cipher_suite, sh.named_group);

    // Compute transcript after SH (for handshake traffic key derivation)
    // ch is the full ClientHello message (header + body). tls_transcript_update_msg
    // expects body-only and adds the handshake header internally, so skip the
    // 4-byte header (type(1) + len(3)) when feeding the transcript.
    uint8_t ch_body_len = ch_len - 4;
    uint8_t transcript_after_sh[32];
    {
        tls_transcript snap;
        tls_transcript_init(&snap);
        tls_transcript_update_msg(&snap, TLS_HS_CLIENT_HELLO, ch + 4, ch_body_len);
        tls_transcript_update_msg(&snap, TLS_HS_SERVER_HELLO, sh_body, hs_bl);
        tls_transcript_final(&snap, transcript_after_sh);
    }

    // Derive handshake keys
    uint8_t shared[32];
    x25519_shared_secret(shared, priv, sh.key_share);
    if (shared_is_zero(shared)) { tls_dbg( "[tls] zero shared\n"); return -1; }
    uint8_t early_secret[32];
    tls_early_secret(NULL, 0, early_secret);
    uint8_t derived[32], hs_secret[32];
    tls_derive_secret(early_secret, derived);
    tls_handshake_secret(derived, shared, hs_secret);
    uint8_t c_hs[32], s_hs[32];
    tls_traffic_secret(hs_secret, "c hs traffic", transcript_after_sh, c_hs);
    tls_dbg( "[tls] handshake keys derived\n");
    tls_traffic_secret(hs_secret, "s hs traffic", transcript_after_sh, s_hs);
    uint8_t c_hs_key[32], c_hs_iv[12], s_hs_key[32], s_hs_iv[12];
    tls_record_key(c_hs, c_hs_key); tls_record_iv(c_hs, c_hs_iv);
    tls_record_key(s_hs, s_hs_key); tls_record_iv(s_hs, s_hs_iv);
    uint8_t s_fin_key[32]; tls_finished_key(s_hs, s_fin_key);

    uint64_t s_seq = 0, c_seq = 0;

    // Receive AEAD records; buffer EE/Cert/CV/Finished bodies.
    int got_ee = 0, got_cert = 0, got_cv = 0, got_sfin = 0;
    uint8_t ee_body[4096], cert_body[8192], cv_body[1024];
    uint8_t fin_body[64];
    uint32_t ee_bl = 0, cert_bl = 0, cv_bl = 0, fin_bl = 0;
    uint8_t pt[18432];
    uint8_t ct_type;
    int pt_len;
    while (!got_sfin) {
        pt_len = recv_aead(s_hs_key, s_hs_iv, &s_seq,
                           pt, sizeof(pt), &ct_type, io);
        if (pt_len < 0) { tls_dbg( "[tls] recv_aead failed pt_len=%d\n", pt_len); return -1; }
        if (ct_type != TLS_CT_HANDSHAKE) return -1;
        uint32_t p = 0;
        while (p < (uint32_t)pt_len) {
            uint8_t t; uint32_t bl;
            uint32_t c = parse_hs(pt + p, pt_len - p, &t, &bl);
            if (c == 0) { tls_dbg( "[tls] parse_hs fail at offset %u remaining %d\n", p, pt_len - (int)p); return -1; }
            const uint8_t* body = pt + p + 4;
            if (t == TLS_HS_ENCRYPTED_EXTENSIONS && !got_ee) {
                if (bl > sizeof(ee_body)) { tls_dbg( "[tls] EE too big %u\n", bl); return -1; }
                memcpy(ee_body, body, bl); ee_bl = bl;
                got_ee = 1;
            } else if (t == TLS_HS_CERTIFICATE && !got_cert) {
                if (bl > sizeof(cert_body)) { tls_dbg( "[tls] cert too big %u\n", bl); return -1; }
                memcpy(cert_body, body, bl); cert_bl = bl;
                if (tls_parse_certificate(cert_body, cert_bl) != 0) { tls_dbg( "[tls] cert parse failed\n"); return -1; }
                got_cert = 1;
            } else if (t == TLS_HS_CERTIFICATE_VERIFY && !got_cv) {
                if (bl > sizeof(cv_body)) { tls_dbg( "[tls] CV too big %u\n", bl); return -1; }
                memcpy(cv_body, body, bl); cv_bl = bl;
                if (tls_parse_certificate_verify(cv_body, cv_bl) != 0) { tls_dbg( "[tls] CV parse failed\n"); return -1; }
                got_cv = 1;
            } else if (t == TLS_HS_FINISHED && !got_sfin) {
                if (bl != 32) { tls_dbg( "[tls] Finished bad len %u\n", bl); return -1; }
                memcpy(fin_body, body, 32); fin_bl = 32;
                got_sfin = 1;
            } else {
                tls_dbg( "[tls] unknown/unexpected HS type=%u\n", t);
            }
            p += c;
        }
    }
    if (!got_ee || !got_cert || !got_cv) return -1;

    // Verify server Finished: verify_data = HMAC(s_fin_key, Hash(CH..CV))
    uint8_t tx_pre_sfin[32];
    transcript_of(ch, ch_len, sh_body, hs_bl,
                  ee_body, ee_bl, cert_body, cert_bl, cv_body, cv_bl,
                  NULL, 0, tx_pre_sfin);
    if (tls_verify_finished(s_fin_key, tx_pre_sfin, fin_body) != 0)
        return -1;

    // Transcript hash for our Finished (CH..server Finished) and for
    // c_ap_traffic derivation.
    uint8_t tx_through_sfin[32];
    transcript_of(ch, ch_len, sh_body, hs_bl,
                  ee_body, ee_bl, cert_body, cert_bl, cv_body, cv_bl,
                  fin_body, fin_bl, tx_through_sfin);

    // Compute application traffic secrets
    uint8_t derived2[32], master[32];
    tls_derive_secret(hs_secret, derived2);
    tls_master_secret(derived2, master);
    uint8_t c_ap[32], s_ap[32];
    tls_traffic_secret(master, "c ap traffic", tx_through_sfin, c_ap);
    tls_traffic_secret(master, "s ap traffic", tx_through_sfin, s_ap);
    uint8_t c_ap_key[32], c_ap_iv[12], s_ap_key[32], s_ap_iv[12];
    tls_record_key(c_ap, c_ap_key); tls_record_iv(c_ap, c_ap_iv);
    tls_record_key(s_ap, s_ap_key); tls_record_iv(s_ap, s_ap_iv);
    // RFC 8446 s4.4.4: client Finished key derived from handshake traffic secret,
    // NOT application traffic secret.
    uint8_t c_fin_key[32]; tls_finished_key(c_hs, c_fin_key);

    // Build our Finished
    uint8_t our_fin[32];
    tls_build_finished(c_fin_key, tx_through_sfin, our_fin);

    // Send our Finished (encrypted under c_hs keys)
    // CCS for middlebox compat (same record format: type=20, ver=0x0303, len=1, value=1)
    {
        uint8_t ccs[6] = { TLS_CT_CHANGE_CIPHER_SPEC, 0x03, 0x03, 0x00, 0x01, 0x01 };
        if (io->send(ccs, 6, io->user) != 0) return -1;
    }
    uint8_t fin_msg[4 + 32];
    fin_msg[0] = TLS_HS_FINISHED;
    fin_msg[1] = 0; fin_msg[2] = 0; fin_msg[3] = 32;
    memcpy(fin_msg + 4, our_fin, 32);
    if (send_aead(c_hs_key, c_hs_iv, &c_seq,
                  TLS_CT_HANDSHAKE, fin_msg, 36, io) != 0) return -1;

    // Send HTTP request (encrypted under c_ap keys)
    // NOTE: client and server application traffic use independent sequence
    // counters even though they share the same IV base.
    uint64_t c_ap_seq = 0, s_ap_seq = 0;
    if (send_aead(c_ap_key, c_ap_iv, &c_ap_seq,
                  TLS_CT_APPDATA, request, request_len, io) != 0) return -1;

    // Read until server closes
    uint32_t out_len = 0;
    int r;
    while ((r = recv_aead(s_ap_key, s_ap_iv, &s_ap_seq,
                          pt, sizeof(pt), &ct_type, io)) >= 0) {
        if (ct_type == TLS_CT_APPDATA) {
            if (out_len + r > out_cap) return -1;
            memcpy(out + out_len, pt, r);
            out_len += r;
        }
    }
    return (int)out_len;
}