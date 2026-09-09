#include "tls_client.h"
#include "tls_record.h"
#include "tls_handshake.h"
#include "tls_keysched.h"
#include "x25519.h"
#include "aead.h"
#include "hmac.h"
#include "certverify.h"
#include "x509.h"
#include "rsa.h"
#include "ec.h"
#include "sha256.h"
#include "sha512.h"
#include <string.h>
#include "tls_dbg.h"

#ifdef KERNEL
#include "rand.h"
#endif

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

// Fills `out` with n random bytes. Returns 0 on failure — callers MUST abort
// the handshake in that case. Never synthesize deterministic "random": these
// bytes become the ECDHE private key and the ClientHello nonce.
static int tls_fill_random(uint8_t* out, uint32_t n) {
#ifdef KERNEL
    return rand_bytes(out, n) == 1;
#else
    host_rng(out, n);
    return 1;
#endif
}

int shared_is_zero(const uint8_t s[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= s[i];
    return acc == 0;
}

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
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

static int send_record(uint8_t type, const uint8_t* payload, uint32_t plen,
                       const struct tls_client_io* io) {
    uint8_t buf[5 + 18432];
    uint32_t total = tls_record_build(type, payload, plen, buf);
    if (total == 0) return -1;
    return io->send(buf, total, io->user);
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

static uint32_t parse_hs(const uint8_t* buf, uint32_t buf_len,
                         uint8_t* t, uint32_t* bl) {
    if (buf_len < 4) return 0;
    *t = buf[0];
    *bl = ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    if (buf_len < 4 + *bl) return 0;
    return 4 + *bl;
}

static void transcript_of(const uint8_t* ch, uint32_t ch_len,
                          const uint8_t* sh_body, uint32_t sh_bl,
                          const uint8_t* ee_body, uint32_t ee_bl,
                          const uint8_t* cert_body, uint32_t cert_bl,
                          const uint8_t* cv_body, uint32_t cv_bl,
                          const uint8_t* fin_body, uint32_t fin_bl,
                          uint8_t out[32]) {
    tls_transcript t;
    tls_transcript_init(&t);
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

// Accumulates one TLS record's bytes (header + payload) into st->rec_buf across
// calls, so a main-loop tick that only receives part of a record returns
// TLS_STEP_AGAIN and resumes next tick. Returns 1 when a full record is buffered
// (st->rec_pl set), 0 if recv() would block (no progress), -1 on close/error.
static int tls_recv_record_st(struct tls_state* st, const struct tls_client_io* io) {
    for (;;) {
        if (st->rec_have >= 5) {
            uint32_t pl = ((uint32_t)st->rec_buf[3] << 8) | st->rec_buf[4];
            if (pl > TLS_RECORD_MAX_PAYLOAD) return -1;
            uint32_t need = 5 + pl;
            if (st->rec_have >= need) { st->rec_pl = pl; return 1; }
            uint32_t want = need - st->rec_have;
            int n = io->recv(st->rec_buf + st->rec_have, want, 5000, io->user);
            if (n < 0) return -1;
            if (n == 0) return 0;
            st->rec_have += (uint32_t)n;
        } else {
            uint32_t want = 5 - st->rec_have;
            int n = io->recv(st->rec_buf + st->rec_have, want, 5000, io->user);
            if (n < 0) return -1;
            if (n == 0) return 0;
            st->rec_have += (uint32_t)n;
        }
    }
}

// Decrypt the single fully-buffered record (st->rec_buf) under the given keys.
// Returns inner plaintext length (minus trailing content-type byte) on success,
// 0 for a ChangeCipherSpec (caller skips), -2 for an alert record (description
// recorded in st->alert_desc), -1 on any other error (including MAC failure).
static int tls_decrypt_one(struct tls_state* st, uint8_t key[32], uint8_t iv[12],
                           uint64_t* seq, uint8_t* pt, uint32_t ptcap,
                           uint8_t* ctype) {
    uint32_t rec_pl = st->rec_pl;
    tls_record v;
    if (tls_record_parse_header(st->rec_buf, 5, &v) != 5) return -1;
    if (v.type == TLS_CT_CHANGE_CIPHER_SPEC) { *ctype = TLS_CT_CHANGE_CIPHER_SPEC; return 0; }
    if (v.type == TLS_CT_ALERT) {
        // Plaintext alert (pre-handshake). Body = level(1) + description(1).
        if (rec_pl == 2) st->alert_desc = st->rec_buf[6];
        *ctype = TLS_CT_ALERT;
        return -2;
    }
    if (v.type != TLS_CT_APPDATA) return -1;
    if (rec_pl < 16) return -1;
    uint32_t ct_len = rec_pl - 16;
    if (ct_len > ptcap) return -1;
    uint8_t nonce[12]; make_nonce(nonce, iv, *seq); (*seq)++;
    uint8_t aad[5]; memcpy(aad, st->rec_buf, 5);
    uint8_t* enc = st->rec_buf + 5;
    if (aead_chacha20_poly1305_decrypt(key, nonce, aad, 5,
                                       enc, ct_len, enc + ct_len, pt) != 0)
        return -1;
    int plen = ct_len;
    while (plen > 0 && pt[plen - 1] == 0) plen--;
    if (plen == 0) return -1;
    *ctype = pt[plen - 1];
    return plen - 1;
}

void tls_state_init(struct tls_state* st, const char* host, uint16_t port,
                    const uint8_t* request, uint32_t request_len,
                    uint8_t* out, uint32_t out_cap) {
    for (uint32_t i = 0; i < sizeof(*st); i++) ((uint8_t*)st)[i] = 0;
    st->host = host;
    st->port = port;
    st->request = request;
    st->request_len = request_len;
    st->out = out;
    st->out_cap = out_cap;
    st->out_len = 0;
    st->phase = TLS_PH_SEND_CH;
    st->rec_have = 0;
    st->rec_pl = 0;
    st->hs_next = 0;
    st->fail_reason = TLS_FAIL_NONE;
    st->alert_desc = -1;
}

// Advance the client by (at most) one blocking I/O op. Returns TLS_STEP_AGAIN
// when more I/O is required, TLS_STEP_DONE when the full response is in
// st->out/st->out_len, or TLS_STEP_ERR on failure.
int tls_state_step(struct tls_state* st, const struct tls_client_io* io) {
    switch (st->phase) {
    case TLS_PH_SEND_CH: {
        // Entropy is mandatory: the private key and hello nonce MUST come
        // from the CPRNG. If it is unavailable we abort — a predictable
        // private key breaks confidentiality outright.
        if (!tls_fill_random(st->priv, 32)) {
            st->fail_reason = TLS_FAIL_RNG;
            return TLS_STEP_ERR;
        }
        st->priv[0] &= 248; st->priv[31] &= 127; st->priv[31] |= 64;
        x25519_public_key(st->pub, st->priv);
        if (!tls_fill_random(st->random, 32)) {
            st->fail_reason = TLS_FAIL_RNG;
            return TLS_STEP_ERR;
        }
        if (!tls_fill_random(st->session_id, 32)) {
            st->fail_reason = TLS_FAIL_RNG;
            return TLS_STEP_ERR;
        }
        st->ch_len = tls_build_client_hello(st->ch, sizeof(st->ch),
                                            st->random, st->session_id,
                                            st->pub, st->host);
        if (st->ch_len == 0) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
        if (send_record(TLS_CT_HANDSHAKE, st->ch, st->ch_len, io) != 0) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        st->phase = TLS_PH_RECV_SH;
        st->rec_have = 0;
        return TLS_STEP_AGAIN;
    }

    case TLS_PH_RECV_SH: {
        int r = tls_recv_record_st(st, io);
        if (r == 0) return TLS_STEP_AGAIN;
        if (r < 0) return TLS_STEP_ERR;

        tls_record rec_v;
        if (tls_record_parse_header(st->rec_buf, 5 + st->rec_pl, &rec_v) != 5) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        if (rec_v.type != TLS_CT_HANDSHAKE) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }

        uint8_t hs_t; uint32_t hs_bl;
        uint32_t consumed = parse_hs(st->rec_buf + 5, st->rec_pl, &hs_t, &hs_bl);
        if (consumed == 0 || hs_t != TLS_HS_SERVER_HELLO) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }

        tls_server_hello sh;
        if (tls_parse_server_hello(st->rec_buf + 5 + 4, hs_bl,
                                   st->session_id, &sh) != 0) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        st->sh_bl = hs_bl;
        for (uint32_t i = 0; i < hs_bl && i < sizeof(st->sh_body); i++)
            st->sh_body[i] = st->rec_buf[5 + 4 + i];

        // ch_len includes the 4-byte handshake header. MUST be uint32_t: a
        // uint8_t truncated mod 256 for long SNI hostnames (CH body > 255B),
        // corrupting the transcript hash and killing the handshake.
        uint32_t ch_body_len = st->ch_len - 4;
        uint8_t transcript_after_sh[32];        {
            tls_transcript snap;
            tls_transcript_init(&snap);
            tls_transcript_update_msg(&snap, TLS_HS_CLIENT_HELLO,
                                      st->ch + 4, ch_body_len);
            tls_transcript_update_msg(&snap, TLS_HS_SERVER_HELLO,
                                      st->sh_body, st->sh_bl);
            tls_transcript_final(&snap, transcript_after_sh);
        }

        uint8_t shared[32];
        x25519_shared_secret(shared, st->priv, sh.key_share);
        if (shared_is_zero(shared)) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
        uint8_t early_secret[32]; tls_early_secret(NULL, 0, early_secret);
        uint8_t derived[32];
        tls_derive_secret(early_secret, derived);
        tls_handshake_secret(derived, shared, st->hs_secret);
        uint8_t s_hs[32];
        tls_traffic_secret(st->hs_secret, "c hs traffic", transcript_after_sh, st->c_hs_secret);
        tls_traffic_secret(st->hs_secret, "s hs traffic", transcript_after_sh, s_hs);
        tls_record_key(st->c_hs_secret, st->c_hs_key);
        tls_record_iv(st->c_hs_secret, st->c_hs_iv);
        tls_record_key(s_hs, st->s_hs_key);
        tls_record_iv(s_hs, st->s_hs_iv);
        tls_finished_key(s_hs, st->s_fin_key);

        st->phase = TLS_PH_RECV_HS;
        st->rec_have = 0;
        return TLS_STEP_AGAIN;
    }

    case TLS_PH_RECV_HS: {
        int r = tls_recv_record_st(st, io);
        if (r == 0) return TLS_STEP_AGAIN;
        if (r < 0) return TLS_STEP_ERR;

        uint8_t pt[TLS_RECORD_MAX_PAYLOAD];
        uint8_t ctype;
        int pl = tls_decrypt_one(st, st->s_hs_key, st->s_hs_iv,
                                 &st->s_seq, pt, sizeof(pt), &ctype);
        if (pl < 0) {
            // -2 = alert record; anything else is a MAC/decrypt failure
            // (active tampering or corruption).
            if (pl == -2 && ctype == TLS_CT_ALERT) {
                st->fail_reason = TLS_FAIL_ALERT;
            } else {
                st->fail_reason = TLS_FAIL_MAC;
            }
            return TLS_STEP_ERR;
        }
        if (ctype == TLS_CT_CHANGE_CIPHER_SPEC) { st->rec_have = 0; return TLS_STEP_AGAIN; }
        if (ctype == TLS_CT_ALERT) {
            // Encrypted alert mid-handshake: body = level(1) + description(1).
            if (pl == 2) { st->alert_desc = pt[1]; st->fail_reason = TLS_FAIL_ALERT; }
            else st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        if (ctype != TLS_CT_HANDSHAKE) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }

        uint32_t p = 0;
        while (p < (uint32_t)pl) {
            uint8_t t; uint32_t bl;
            uint32_t c = parse_hs(pt + p, pl - p, &t, &bl);
            if (c == 0) {
                tls_dbg("[tls] flight parse failed at p=%u pl=%d\n", p, pl);
                st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
            }
            const uint8_t* body = pt + p + 4;
            // RFC 8446 §4.4: the encrypted flight is EXACTLY EncryptedExtensions,
            // Certificate, CertificateVerify, Finished — in that order, each
            // once. Enforce it: an unexpected or repeated message is a protocol
            // violation, not something to tolerate.
            {
                static const uint8_t expect_types[4] = {
                    TLS_HS_ENCRYPTED_EXTENSIONS, TLS_HS_CERTIFICATE,
                    TLS_HS_CERTIFICATE_VERIFY, TLS_HS_FINISHED
                };
                if (st->hs_next < 0 || st->hs_next > 3 ||
                    t != expect_types[st->hs_next]) {
                    tls_dbg("[tls] flight order violation: got type %u, expected %u\n",
                            t, st->hs_next >= 0 && st->hs_next <= 3
                               ? expect_types[st->hs_next] : 0);
                    st->fail_reason = TLS_FAIL_PROTO;
                    return TLS_STEP_ERR;
                }
            }
            if (t == TLS_HS_ENCRYPTED_EXTENSIONS) {
                if (bl > sizeof(st->ee_body)) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
                memcpy(st->ee_body, body, bl); st->ee_bl = bl; st->got_ee = 1;
                st->hs_next = 1;
            } else if (t == TLS_HS_CERTIFICATE) {
                if (bl > sizeof(st->cert_body)) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
                memcpy(st->cert_body, body, bl); st->cert_bl = bl;
                if (tls_parse_certificate(st->cert_body, st->cert_bl) != 0) {
                    tls_dbg("[tls] Certificate structural parse failed (len=%u)\n",
                            st->cert_bl);
                    st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
                }
                st->got_cert = 1;
                st->hs_next = 2;
            } else if (t == TLS_HS_CERTIFICATE_VERIFY) {
                if (bl > sizeof(st->cv_body)) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
                memcpy(st->cv_body, body, bl); st->cv_bl = bl;
                if (tls_parse_certificate_verify(st->cv_body, st->cv_bl) != 0) {
                    st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
                }
                st->got_cv = 1;
                st->hs_next = 3;
            } else { // TLS_HS_FINISHED
                if (bl != 32) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
                memcpy(st->fin_body, body, 32); st->fin_bl = 32; st->got_sfin = 1;
                st->hs_next = 4;
            }
            p += c;
        }
        st->rec_have = 0;
        if (!st->got_sfin) return TLS_STEP_AGAIN;

        if (!st->got_ee || !st->got_cert || !st->got_cv || st->hs_next != 4) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }

        // ---- SERVER AUTHENTICATION ----
        // RFC 8446 §4.4.2-4.4.4. Without this the "s" in https is decorative:
        // any MITM can present its own certificate and complete the handshake.
        // Order: chain -> anchor -> hostname, then the CertificateVerify
        // signature (proof-of-possession of the leaf key), then Finished.
        {
            const char* vhost = st->verify_host ? st->verify_host : st->host;
            int cvr = cert_verify(st->cert_body, st->cert_bl, vhost);
            if (cvr != CV_OK) {
                tls_dbg("[tls] cert verification failed: %s\n",
                        cert_verify_strerror(cvr));
                st->fail_reason = TLS_FAIL_CERT;
                return TLS_STEP_ERR;
            }

            // CertificateVerify: signs 64 sp || "TLS 1.3, server
            // CertificateVerify" || 0x00, hashed per the signature algorithm,
            // with the LEAF certificate's key. Transcript covers CH..Certificate.
            x509_cert leaf;
            if (cert_leaf(st->cert_body, st->cert_bl, &leaf) != 0) {
                tls_dbg("[tls] leaf certificate parse failed (len=%u)\n", st->cert_bl);
                st->fail_reason = TLS_FAIL_CERT;
                return TLS_STEP_ERR;
            }
            uint16_t cv_alg;
            const uint8_t* cv_sig;
            uint32_t cv_sig_len;
            if (tls_parse_cv_sig(st->cv_body, st->cv_bl, &cv_alg, &cv_sig, &cv_sig_len) != 0) {
                tls_dbg("[tls] CertificateVerify parse failed (len=%u)\n", st->cv_bl);
                st->fail_reason = TLS_FAIL_CERT;
                return TLS_STEP_ERR;
            }
            tls_dbg("[tls] leaf key_type=%d sig_alg=%d cv_alg=%04x sig_len=%u\n",
                    leaf.key_type, leaf.sig_alg, cv_alg, cv_sig_len);
            uint8_t cv_content[64 + 33 + 1];
            memset(cv_content, 0x20, 64);
            memcpy(cv_content + 64, "TLS 1.3, server CertificateVerify", 33);
            cv_content[64 + 33] = 0x00;
            uint32_t cv_content_len = 64 + 33 + 1;

            uint8_t tx_through_cert[32];
            transcript_of(st->ch, st->ch_len, st->sh_body, st->sh_bl,
                          st->ee_body, st->ee_bl, st->cert_body, st->cert_bl,
                          NULL, 0, NULL, 0, tx_through_cert);

            // The signed data is content || transcript-hash-through-Certificate
            // (RFC 8446 §4.4.3) — hashed per the signature algorithm below.
            uint8_t signed_data[sizeof(cv_content) + 32];
            memcpy(signed_data, cv_content, cv_content_len);
            memcpy(signed_data + cv_content_len, tx_through_cert, 32);
            uint32_t signed_len = cv_content_len + 32;

            int vr = -2;
            if (cv_alg == 0x0403) {          // ecdsa_secp256r1_sha256
                if (leaf.key_type != X509_KEY_EC_P256) vr = -2;
                else vr = ec_verify(X509_SIG_ECDSA_SHA256,
                                    leaf.ec_point, leaf.ec_point_len,
                                    signed_data, signed_len,
                                    cv_sig, cv_sig_len);
            } else if (cv_alg == 0x0503) {   // ecdsa_secp384r1_sha384
                if (leaf.key_type != X509_KEY_EC_P384) vr = -2;
                else vr = ec_verify(X509_SIG_ECDSA_SHA384,
                                    leaf.ec_point, leaf.ec_point_len,
                                    signed_data, signed_len,
                                    cv_sig, cv_sig_len);
            } else if (cv_alg == 0x0804) {   // rsa_pss_rsae_sha256 (TLS 1.3 mandate)
                if (leaf.key_type != X509_KEY_RSA) vr = -2;
                else {
                    rsa_pub rk;
                    if (rsa_pub_from_x509(&leaf, &rk) != 0) vr = -2;
                    else vr = rsa_verify_pss(&rk, X509_SIG_RSA_SHA256,
                                             signed_data, signed_len,
                                             cv_sig, cv_sig_len);
                }
            } else if (cv_alg == 0x0805) {   // rsa_pss_rsae_sha384
                if (leaf.key_type != X509_KEY_RSA) vr = -2;
                else {
                    rsa_pub rk;
                    if (rsa_pub_from_x509(&leaf, &rk) != 0) vr = -2;
                    else vr = rsa_verify_pss(&rk, X509_SIG_RSA_SHA384,
                                             signed_data, signed_len,
                                             cv_sig, cv_sig_len);
                }
            } else if (cv_alg == 0x0401) {   // rsa_pkcs1_sha256
                if (leaf.key_type != X509_KEY_RSA) vr = -2;
                else {
                    rsa_pub rk;
                    if (rsa_pub_from_x509(&leaf, &rk) != 0) vr = -2;
                    else vr = rsa_verify_pkcs1(&rk, X509_SIG_RSA_SHA256,
                                               signed_data, signed_len,
                                               cv_sig, cv_sig_len);
                }
            }
            if (vr != 0) {
                tls_dbg("[tls] CertificateVerify signature INVALID (alg=%04x)\n",
                        cv_alg);
                st->fail_reason = TLS_FAIL_CERT;
                return TLS_STEP_ERR;
            }
            tls_dbg("[tls] certificate chain verified, host matched\n");
        }

        uint8_t tx_pre_sfin[32];
        transcript_of(st->ch, st->ch_len, st->sh_body, st->sh_bl,
                      st->ee_body, st->ee_bl, st->cert_body, st->cert_bl,
                      st->cv_body, st->cv_bl, NULL, 0, tx_pre_sfin);
        if (tls_verify_finished(st->s_fin_key, tx_pre_sfin, st->fin_body) != 0) {
            // A bad server Finished is an authentication failure, not a
            // transport error — flag it so the caller can distinguish it.
            tls_dbg("[tls] server Finished INVALID\n");
            st->fail_reason = TLS_FAIL_MAC;
            return TLS_STEP_ERR;
        }

        uint8_t tx_through_sfin[32];
        transcript_of(st->ch, st->ch_len, st->sh_body, st->sh_bl,
                      st->ee_body, st->ee_bl, st->cert_body, st->cert_bl,
                      st->cv_body, st->cv_bl, st->fin_body, st->fin_bl,
                      tx_through_sfin);

        uint8_t derived2[32], master[32];
        tls_derive_secret(st->hs_secret, derived2);
        tls_master_secret(derived2, master);
        uint8_t c_ap[32], s_ap[32];
        tls_traffic_secret(master, "c ap traffic", tx_through_sfin, c_ap);
        tls_traffic_secret(master, "s ap traffic", tx_through_sfin, s_ap);
        tls_record_key(c_ap, st->c_ap_key); tls_record_iv(c_ap, st->c_ap_iv);
        tls_record_key(s_ap, st->s_ap_key); tls_record_iv(s_ap, st->s_ap_iv);
        tls_finished_key(st->c_hs_secret, st->c_fin_key);

        uint8_t our_fin[32];
        tls_build_finished(st->c_fin_key, tx_through_sfin, our_fin);

        uint8_t ccs[6] = { TLS_CT_CHANGE_CIPHER_SPEC, 0x03, 0x03, 0x00, 0x01, 0x01 };
        if (io->send(ccs, 6, io->user) != 0) return TLS_STEP_ERR;
        uint8_t fin_msg[4 + 32];
        fin_msg[0] = TLS_HS_FINISHED; fin_msg[1] = 0; fin_msg[2] = 0; fin_msg[3] = 32;
        memcpy(fin_msg + 4, our_fin, 32);
        if (send_aead(st->c_hs_key, st->c_hs_iv, &st->c_seq,
                      TLS_CT_HANDSHAKE, fin_msg, 36, io) != 0) return TLS_STEP_ERR;

        st->c_ap_seq = 0; st->s_ap_seq = 0;
        if (send_aead(st->c_ap_key, st->c_ap_iv, &st->c_ap_seq,
                      TLS_CT_APPDATA, st->request, st->request_len, io) != 0)
            return TLS_STEP_ERR;

        st->phase = TLS_PH_RECV_BODY;
        return TLS_STEP_AGAIN;
    }

    case TLS_PH_RECV_BODY: {
        int r = tls_recv_record_st(st, io);
        if (r == 0) return TLS_STEP_AGAIN;
        if (r < 0) {
            // Transport EOF: we always send Connection: close, so the peer
            // closing after the response IS the normal end of the body.
            st->phase = TLS_PH_DONE;
            return TLS_STEP_DONE;
        }

        uint8_t pt[TLS_RECORD_MAX_PAYLOAD];
        uint8_t ctype;
        int pl = tls_decrypt_one(st, st->s_ap_key, st->s_ap_iv,
                                 &st->s_ap_seq, pt, sizeof(pt), &ctype);
        if (pl < 0) {
            // A MAC failure here is an ACTIVE attack (or corruption) on the
            // stream — never deliver partial plaintext as if it were a clean
            // response. Only close_notify ends the fetch "successfully".
            if (pl == -2 && ctype == TLS_CT_ALERT) {
                if (st->alert_desc == 0) {  // close_notify
                    st->fail_reason = TLS_FAIL_ALERT_CLOSE;
                    st->phase = TLS_PH_DONE;
                    return TLS_STEP_DONE;
                }
                st->fail_reason = TLS_FAIL_ALERT;
                return TLS_STEP_ERR;
            }
            st->fail_reason = TLS_FAIL_MAC;
            return TLS_STEP_ERR;
        }
        if (ctype == TLS_CT_CHANGE_CIPHER_SPEC) { st->rec_have = 0; return TLS_STEP_AGAIN; }
        if (ctype == TLS_CT_ALERT) {
            if (pl == 2) {
                if (pt[1] == 0) {  // close_notify
                    st->fail_reason = TLS_FAIL_ALERT_CLOSE;
                    st->phase = TLS_PH_DONE;
                    return TLS_STEP_DONE;
                }
                st->alert_desc = pt[1];
                st->fail_reason = TLS_FAIL_ALERT;
            } else {
                st->fail_reason = TLS_FAIL_PROTO;
            }
            return TLS_STEP_ERR;
        }
        if (ctype == TLS_CT_APPDATA) {
            if (st->out_len + pl > st->out_cap) { st->fail_reason = TLS_FAIL_PROTO; st->phase = TLS_PH_DONE; return TLS_STEP_ERR; }
            memcpy(st->out + st->out_len, pt, pl);
            st->out_len += pl;
        } else {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        st->rec_have = 0;
        return TLS_STEP_AGAIN;
    }

    case TLS_PH_DONE:
        return TLS_STEP_DONE;
    }
    return TLS_STEP_ERR;
}

// Synchronous convenience wrapper (host tests / blocking I/O). Drives the state
// machine to completion. Blocking recv() callers never return 0, so the loop
// terminates at DONE/ERR.
static int g_last_fail_reason = TLS_FAIL_NONE;

int tls_last_fail_reason(void) {
    return g_last_fail_reason;
}

int tls_client_run(const char* host, uint16_t port,
                   const uint8_t* request, uint32_t request_len,
                   uint8_t* out, uint32_t out_cap,
                   const struct tls_client_io* io) {
    struct tls_state st;
    tls_state_init(&st, host, port, request, request_len, out, out_cap);
    int r;
    do {
        r = tls_state_step(&st, io);
    } while (r == TLS_STEP_AGAIN);
    g_last_fail_reason = st.fail_reason;
    if (r == TLS_STEP_DONE) return (int)st.out_len;
    return -1;
}
