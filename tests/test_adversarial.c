// Adversarial security tests for the TLS 1.3 + X.509 stack.
//
// 1. MUTATION FUZZ of the real example.com certificate flight through
//    cert_verify (run under ASan): no crash, and NO mutation may verify.
// 2. MOCK ADVERSARIAL TLS SERVER driving tls_state_step through the io
//    callbacks — a working TLS 1.3 server built from the stack's own
//    primitives + a test PKI (openssl signs the CertificateVerify bytes).
//    Positive control first (a fully valid flight must complete), then the
//    attacks: wrong-key/forged/garbled CV, hostname mismatch, expired certs,
//    key-share swap (forwarding MITM), cipher/session-id/HRR-shaped
//    ServerHellos, flight-order violations, Finished tampering, alert
//    injection, app-data ciphertext bit-flips, record garbage/overflow.
//
// Build: gcc -m32 -O1 -g -fsanitize=address,undefined -Isrc/crypto -Isrc ...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "tls_client.h"
#include "tls_record.h"
#include "tls_handshake.h"
#include "tls_keysched.h"
#include "x25519.h"
#include "aead.h"
#include "hmac.h"
#include "sha256.h"
#include "sha512.h"
#include "certverify.h"
#include "x509.h"

static int failures = 0;
static int passes = 0;
#define CHECK(cond, name) do { \
    if (cond) { passes++; printf("  PASS %s\n", name); } \
    else { failures++; printf("  FAIL %s\n", name); } \
} while (0)

static int load(const char* path, uint8_t* out, uint32_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static uint32_t build_cert_msg(uint8_t* msg, uint32_t cap,
                               const char* const* files, int nfiles) {
    uint32_t mp = 0;
    if (cap < 8) return 0;
    msg[mp++] = 0;                       // empty certificate_request_context
    uint32_t lenpos = mp; mp += 3;
    static uint8_t der[8192];
    for (int i = 0; i < nfiles; i++) {
        int cn = load(files[i], der, sizeof(der));
        if (cn <= 0) return 0;
        if (mp + 5 + (uint32_t)cn > cap) return 0;
        msg[mp++] = (uint8_t)(((uint32_t)cn >> 16) & 0xff);
        msg[mp++] = (uint8_t)(((uint32_t)cn >> 8) & 0xff);
        msg[mp++] = (uint8_t)(cn & 0xff);
        memcpy(msg + mp, der, (size_t)cn); mp += (uint32_t)cn;
        msg[mp++] = 0; msg[mp++] = 0;
    }
    uint32_t list_len = mp - lenpos - 3;
    msg[lenpos] = (uint8_t)((list_len >> 16) & 0xff);
    msg[lenpos + 1] = (uint8_t)((list_len >> 8) & 0xff);
    msg[lenpos + 2] = (uint8_t)(list_len & 0xff);
    return mp;
}

// ---------------------------------------------------------------- 1. fuzz

static void fuzz_section(void) {
    printf("== 1. mutation fuzz: example.com cert flight ==\n");
    static const char* files[] = {
        "tests/fixtures/live_c200.der",
        "tests/fixtures/live_c201.der",
        "tests/fixtures/live_c202.der",
        "tests/fixtures/live_c203.der",
    };
    static uint8_t msg[16384];
    uint32_t msg_len = build_cert_msg(msg, sizeof(msg), files, 4);
    CHECK(msg_len > 0, "real flight fixture builds");

    // Locate the LAST cert (the trust anchor) in the flight: anchors are
    // trusted BY KEY (RFC 5280) — mutations of the root's non-SPKI TBS
    // fields (serial, unused extensions...) legitimately still verify, the
    // same way a pinned SSH host key works. Everything up to the anchor and
    // every security-relevant anchor region MUST reject mutations.
    x509_cert anchor;
    uint32_t rootx_off = 0;
    static uint8_t anchor_der[16384];
    int anchor_der_len = 0;
    {
        // walk entries to find the last one's offset
        uint32_t p = 1;                    // ctx_len = 0
        uint32_t list_len = ((uint32_t)msg[p] << 16) | (msg[p+1] << 8) | msg[p+2];
        p += 3;
        uint32_t e = 0;
        while (e + 3 <= list_len) {
            uint32_t entry_len = ((uint32_t)msg[p+e] << 16) |
                                 ((uint32_t)msg[p+e+1] << 8) | msg[p+e+2];
            rootx_off = p + e + 3;
            e += 3 + entry_len + 2;
        }
        anchor_der_len = load(files[3], anchor_der, sizeof(anchor_der));
        if (anchor_der_len <= 0 ||
            x509_parse(msg + rootx_off, (uint32_t)anchor_der_len, &anchor) != 0) {
            CHECK(0, "anchor parses in place");
            return;
        }
        CHECK(1, "anchor parses in place");
    }

    x509_time now = { 2026, 9, 9, 14, 0, 0 };
    x509_set_now(&now);
    int r0 = cert_verify(msg, msg_len, "example.com");
    CHECK(r0 == CV_OK, "unmutated flight verifies (control)");

    uint64_t rng = 0x9E3779B97F4A7C15ull;
#define NEXT32() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, \
                  (uint32_t)(rng & 0xFFFFFFFFu))

    static uint8_t mut[16384];
    int accepted = 0, bad_code = 0, accepted_outside_ignored = 0;
    int by_parse = 0, by_chain = 0, by_root = 0, by_other = 0;
    const int ROUNDS = 4000;
    for (int round = 0; round < ROUNDS; round++) {
        memcpy(mut, msg, msg_len);
        uint32_t mlen = msg_len;
        uint32_t pos;
        int mode = (int)(NEXT32() % 3);
        if (mode == 0) {
            pos = NEXT32() % mlen;
            mut[pos] ^= (uint8_t)(1u << (NEXT32() % 8));
        } else if (mode == 1) {
            pos = NEXT32() % mlen;
            mut[pos] = (uint8_t)(NEXT32() & 0xff);
            if (mut[pos] == msg[pos]) continue;   // no-op overwrite: skip
        } else {
            pos = mlen;                    // truncation
            mlen = 1 + NEXT32() % (mlen - 1);
        }
        int r = cert_verify(mut, mlen, "example.com");
        if (r == CV_OK) {
            accepted++;
            // tolerated ONLY if the mutation is inside the anchor's TBS but
            // outside its SPKI (the trusted key) and signature areas.
            uint32_t spki_off = (uint32_t)(anchor.spki.p - (msg + rootx_off));
            int in_anchor_ignored =
                pos >= rootx_off && pos < rootx_off + (uint32_t)anchor_der_len &&
                !(pos >= rootx_off + spki_off &&
                  pos <  rootx_off + spki_off + anchor.spki.len);
            if (!in_anchor_ignored) {
                accepted_outside_ignored++;
                if (accepted_outside_ignored <= 5)
                    printf("    [FLAGGED] pos=%u rootx_off=%u anchor_len=%d spki_off=%u spki_len=%u\n",
                           pos, rootx_off, anchor_der_len,
                           (uint32_t)(anchor.spki.p - (msg + rootx_off)),
                           anchor.spki.len);
            }
        }
        else if (r == CV_ERR_PARSE) by_parse++;
        else if (r == CV_ERR_CHAIN) by_chain++;
        else if (r == CV_ERR_ROOT) by_root++;
        else if (r > CV_OK && r <= CV_ERR_NO_CERT) by_other++;
        else bad_code++;
    }
    printf("    %d mutations: rejected=%d accepted=%d (parse=%d chain=%d root=%d other=%d)\n",
           ROUNDS, ROUNDS - accepted, accepted, by_parse, by_chain, by_root, by_other);
    CHECK(accepted_outside_ignored == 0,
          "no security-relevant mutation ever verifies");
    CHECK(bad_code == 0, "no mutation produces an out-of-range code");

    int g_bad = 0;
    for (int round = 0; round < 2000; round++) {
        uint32_t gl = NEXT32() % 2000;
        for (uint32_t i = 0; i < gl; i++) mut[i] = (uint8_t)(NEXT32() & 0xff);
        int r = cert_verify(mut, gl, "example.com");
        if (r == CV_OK) g_bad++;
        if (r < CV_OK || r > CV_ERR_NO_CERT) bad_code++;
    }
    CHECK(g_bad == 0, "no random garbage verifies");
    CHECK(bad_code == 0, "no garbage produces an out-of-range code");
}

// ---------------------------------------------------------------- 2. mock

#define SCRATCH_DIR "/tmp/opencode-adv"

// scripted server -> client queue
static uint8_t q_buf[65536];
static uint32_t q_len, q_pos;
static void q_reset(void) { q_len = q_pos = 0; }
static void q_put(const uint8_t* p, uint32_t n) {
    memcpy(q_buf + q_len, p, n);
    q_len += n;
}

// client -> server capture
static uint8_t c_buf[65536];
static uint32_t c_len;

// ---- attack modes ----
enum mock_mode {
    MOCK_VALID = 0,
    MOCK_RSA_PSS_VALID,
    MOCK_P384_VALID,
    MOCK_CV_WRONG_KEY,
    MOCK_CV_FLIPPED,
    MOCK_CV_GARBAGE_TRANSCRIPT,
    MOCK_HOSTNAME_MISMATCH,
    MOCK_EXPIRED,
    MOCK_KEYSHARE_SWAP,
    MOCK_CIPHER_1302,
    MOCK_SID_BAD,
    MOCK_NO_KEYSHARE,
    MOCK_NO_CV,
    MOCK_DUP_EE,
    MOCK_FIN_FLIPPED,
    MOCK_ALERT,
    MOCK_APPDATA_BITFLIP,
    MOCK_CLOSE_NOTIFY,
    MOCK_GARBAGE_FIRST,
    MOCK_RECORD_OVERFLOW,
    MOCK_SPLIT,        // valid flight, server dribbles 7B/recv (rec_have path)
    MOCK_TRUNCATED,    // flight cut short mid-record (must ERR, never hang)
    MOCK_COUNT
};

struct mock_result {
    int r;
    int fail_reason;
    int alert_desc;
    uint32_t out_len;
    uint8_t out[4096];
};

// mock state needed by the lazy body path
struct mtrans { tls_transcript t; };
static struct mtrans g_mt;
static uint8_t g_hs_secret[32];
static uint8_t g_s_ap_traffic[32];
static uint8_t g_c_hs_traffic[32];
static uint8_t g_s_ap_key[32], g_s_ap_iv[12];
static uint64_t g_s_ap_seq;
static uint8_t g_resp[512];
static uint32_t g_resp_len;
static uint8_t g_sh_body[256];
static uint32_t g_sh_len;
static uint8_t g_sfin_msg[36];
static uint32_t g_sfin_len;
static int g_body_queued;
static int g_expect_body;
static int g_mode;

static int mock_recv(uint8_t* out, uint32_t cap, uint32_t timeout_ms, void* user);
static int mock_send(const uint8_t* buf, uint32_t len, void* user);
static struct tls_client_io mock_io = { .send = mock_send, .recv = mock_recv, .user = 0 };

static void mtrans_init(struct mtrans* m) { tls_transcript_init(&m->t); }
static void mtrans_msg(struct mtrans* m, uint8_t type, const uint8_t* body, uint32_t bl) {
    tls_transcript_update_msg(&m->t, type, body, bl);
}
static void mtrans_hash(struct mtrans* m, uint8_t out[32]) {
    tls_transcript snap = m->t;         // struct copy: final() stays local
    tls_transcript_final(&snap, out);
}

static uint32_t enc_record(uint8_t* out, uint32_t cap, uint8_t key[32], uint8_t iv[12], uint64_t* seq, uint8_t inner_type, const uint8_t* pt, uint32_t pt_len);
static int try_queue_body(const uint8_t* resp, uint32_t resp_len) {
    if (g_body_queued) return 1;
    // client sent: CCS(6) | enc Finished | enc request. The Finished is
    // needed to complete the transcript before s_ap can be derived.
    static uint8_t fin_pt[128];
    uint32_t p = 0;
    // walk the client's plaintext records: [CH][CCS][enc Finished]...
    while (p + 5 <= c_len) {
        uint8_t rt = c_buf[p];
        uint32_t rplen = ((uint32_t)c_buf[p+3] << 8) | c_buf[p+4];
        if (rt == TLS_CT_HANDSHAKE) { p += 5 + rplen; continue; }  // the CH
        if (rt == TLS_CT_CHANGE_CIPHER_SPEC) break;                // found it
        { printf("    [tqb] walk: unexpected rec type %02x at p=%u\n", rt, p); return 0; }
    }
    if (p + 6 > c_len || c_buf[p] != TLS_CT_CHANGE_CIPHER_SPEC) { printf("    [tqb] no CCS at p=%u c_len=%u type=%02x\n", p, c_len, p < c_len ? c_buf[p] : 0); return 0; }
    p += 6;
    static uint8_t c_hs_key[32], c_hs_iv[12];
    tls_record_key(g_c_hs_traffic, c_hs_key);
    tls_record_iv(g_c_hs_traffic, c_hs_iv);
    if (p + 5 > c_len || c_buf[p] != TLS_CT_APPDATA) { printf("    [tqb] no appdata at p=%u type=%02x\n", p, p < c_len ? c_buf[p] : 0); return 0; }
    uint32_t pl = ((uint32_t)c_buf[p+3] << 8) | c_buf[p+4];
    if (p + 5 + pl > c_len || pl < 16) { printf("    [tqb] bad pl=%u p=%u c_len=%u\n", pl, p, c_len); return 0; }
    uint8_t nonce[12];
    memcpy(nonce, c_hs_iv, 12);
    uint64_t seq0 = 0;   // client's first encrypted record: seq 0
    for (int i = 0; i < 8; i++)
        nonce[4+i] ^= (uint8_t)((seq0 >> (56 - 8 * i)) & 0xff);
    uint8_t aad[5] = { TLS_CT_APPDATA, 0x03, 0x03,
                       (uint8_t)(pl >> 8), (uint8_t)(pl & 0xff) };
    if (aead_chacha20_poly1305_decrypt(c_hs_key, nonce, aad, 5,
                                       c_buf + p + 5, pl - 16,
                                       c_buf + p + 5 + pl - 16, fin_pt) != 0) {
        printf("    mock key:"); for (int i = 0; i < 8; i++) printf(" %02x", c_hs_key[i]);
        printf("  nonce:"); for (int i = 0; i < 6; i++) printf(" %02x", nonce[i]);
        printf("\n    hs_secret:"); for (int i = 0; i < 8; i++) printf(" %02x", g_hs_secret[i]);
        printf("  ct:"); for (int i = 0; i < 8; i++) printf(" %02x", c_buf[p+5+i]);
        printf("\n");
        // recompute the client-side c hs traffic secret independently
        {
            uint8_t sh2[32], hs2[32], tr[32], chs2[32], k2[32], iv2[12], n2[12];
            tls_transcript t2;
            tls_transcript_init(&t2);
            uint32_t ch_rplen = ((uint32_t)c_buf[3] << 8) | c_buf[4];
            tls_transcript_update_msg(&t2, TLS_HS_CLIENT_HELLO, c_buf + 9,
                                      ch_rplen - 4);   // CH body: record hdr(5)+hs hdr(4)
            tls_transcript_update_msg(&t2, TLS_HS_SERVER_HELLO, g_sh_body, g_sh_len);
            tls_transcript_final(&t2, tr);
            tls_traffic_secret(g_hs_secret, "c hs traffic", tr, chs2);
            tls_record_key(chs2, k2);
            tls_record_iv(chs2, iv2);
            memcpy(n2, iv2, 12);
            (void)sh2; (void)hs2;
            printf("    recomp key:"); for (int i = 0; i < 8; i++) printf(" %02x", k2[i]);
            printf("  tr:"); for (int i = 0; i < 8; i++) printf(" %02x", tr[i]);
            printf("\n");
            uint8_t aad2[5] = { TLS_CT_APPDATA, 0x03, 0x03,
                                (uint8_t)(pl >> 8), (uint8_t)(pl & 0xff) };
            if (aead_chacha20_poly1305_decrypt(k2, n2, aad2, 5,
                                               c_buf + p + 5, pl - 16,
                                               c_buf + p + 5 + pl - 16, fin_pt) == 0)
                printf("    recomp decrypt OK -> g_c_hs_traffic was stale!\n");
            else
                printf("    recomp decrypt ALSO fails -> ct/nonce/aad mismatch\n");
            printf("    iv:"); for (int i = 0; i < 12; i++) printf(" %02x", c_hs_iv[i]);
            printf("  hdr:"); for (int i = 0; i < 5; i++) printf(" %02x", c_buf[p+i]);
            printf("  tag:"); for (int i = 0; i < 8; i++) printf(" %02x", c_buf[p+5+pl-16+i]);
            printf("\n");
        }
        return 0;
    }
    // inner: Finished hs message (type 20 | len 3 | 32B data) | ct-type(1)=22
    if (pl < 4 + 32 + 1 || fin_pt[0] != TLS_HS_FINISHED) { printf("    [tqb] not fin msg: type=%02x pl=%u\n", fin_pt[0], pl); return 0; }
    if (fin_pt[4 + 32] != TLS_CT_HANDSHAKE) { printf("    [tqb] wrong tail ct=%02x\n", fin_pt[4+32]); return 0; }
    // RFC 8446: "s ap traffic" spans CH..server Finished — add the server's
    // own Finished, hash, then add the client's (resumption master only).
    mtrans_msg(&g_mt, TLS_HS_FINISHED, g_sfin_msg + 4, g_sfin_len - 4);
    uint8_t tx_pre_cfin[32];
    mtrans_hash(&g_mt, tx_pre_cfin);
    mtrans_msg(&g_mt, TLS_HS_FINISHED, fin_pt + 5, 32);

    uint8_t derived2[32], master[32];
    tls_derive_secret(g_hs_secret, derived2);
    tls_master_secret(derived2, master);
    tls_traffic_secret(master, "s ap traffic", tx_pre_cfin, g_s_ap_traffic);
    tls_record_key(g_s_ap_traffic, g_s_ap_key);
    tls_record_iv(g_s_ap_traffic, g_s_ap_iv);
    g_s_ap_seq = 0;

    static uint8_t rec[16500];
    uint32_t rl = enc_record(rec, sizeof(rec), g_s_ap_key, g_s_ap_iv,
                             &g_s_ap_seq, TLS_CT_APPDATA, resp, resp_len);
    if (rl == 0) { printf("    [tqb] enc_record returned 0\n"); return 0; }
    q_put(rec, rl);

    if (g_mode == MOCK_APPDATA_BITFLIP) {
        // flip a bit inside the last record's encrypted body (before the tag)
        if (q_len > 6 + 16) q_buf[q_len - 16 - 1] ^= 0x08;
    }
    if (g_mode == MOCK_CLOSE_NOTIFY) {
        uint8_t cn[2] = { 0x01, 0x00 };   // warning + close_notify
        rl = enc_record(rec, sizeof(rec), g_s_ap_key, g_s_ap_iv,
                        &g_s_ap_seq, TLS_CT_ALERT, cn, 2);
        if (rl) q_put(rec, rl);
    }
    g_body_queued = 1;
    return 1;
}

static int mock_recv(uint8_t* out, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)timeout_ms; (void)user;
    if (q_pos >= q_len) {
        if (!g_body_queued && g_expect_body) try_queue_body(g_resp, g_resp_len);
        if (q_pos >= q_len) return -1;     // server closed
    }
    uint32_t take = q_len - q_pos < cap ? q_len - q_pos : cap;
    // SPLIT: dribble at most 7 bytes per recv — the client's rec_have
    // reassembly path (partial record headers AND bodies) must still
    // complete the handshake byte-identically.
    if (g_mode == MOCK_SPLIT && take > 7) take = 7;
    memcpy(out, q_buf + q_pos, take);
    q_pos += take;
    return (int)take;
}

static int mock_send(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    if (c_len + len > sizeof(c_buf)) return -1;
    memcpy(c_buf + c_len, buf, len);
    c_len += len;
    return 0;
}

static int sign_digest(const char* key, const uint8_t* data, uint32_t len,
                       int pss, int sha384, uint8_t* sig, uint32_t* sig_len) {
    mkdir(SCRATCH_DIR, 0777);
    static char path[256], cmd[640];
    snprintf(path, sizeof(path), SCRATCH_DIR "/data.bin");
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(data, 1, len, f);
    fclose(f);
    snprintf(path, sizeof(path), SCRATCH_DIR "/sig.bin");
    if (pss)
        snprintf(cmd, sizeof(cmd),
                 "openssl dgst -sha256 -sigopt rsa_padding_mode:pss "
                 "-sigopt rsa_pss_saltlen:32 -sign %s -out %s " SCRATCH_DIR "/data.bin 2>/dev/null",
                 key, path);
    else if (sha384)
        snprintf(cmd, sizeof(cmd),
                 "openssl dgst -sha384 -sign %s -out %s " SCRATCH_DIR "/data.bin 2>/dev/null",
                 key, path);
    else
        snprintf(cmd, sizeof(cmd),
                 "openssl dgst -sha256 -sign %s -out %s " SCRATCH_DIR "/data.bin 2>/dev/null",
                 key, path);
    if (system(cmd) != 0) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(sig, 1, 1024, f);
    fclose(f);
    *sig_len = (uint32_t)n;
    return n > 0 ? 0 : -1;
}

static int parse_client_ch(const uint8_t* c0, uint32_t clen,
                           uint8_t sid[32], uint8_t pub[32]) {
    if (clen < 5 + 4 + 2 + 32 + 33 + 6 + 2 + 2) return -1;
    const uint8_t* b = c0 + 5 + 4;      // CH body
    memcpy(sid, b + 2 + 32 + 1, 32);
    uint32_t p = 2 + 32 + 1 + 32;       // past version+random+sid
    p += 2 + 2 * 2;                     // ciphers (len + 0x1303 + 0x00ff)
    p += 1 + 1;                         // compression (len + null)
    uint32_t elen = ((uint32_t)b[p] << 8) | b[p+1];
    p += 2;
    uint32_t eend = p + elen;
    if (eend > clen - 5 - 4) return -1;
    while (p + 4 <= eend) {
        uint16_t et = (uint16_t)((b[p] << 8) | b[p+1]);
        uint16_t l = (uint16_t)((b[p+2] << 8) | b[p+3]);
        p += 4;
        if (p + l > eend) return -1;
        if (et == 0x0033 && l >= 38) {  // key_share: list(2)|group(2)|klen(2)|key(32)
            uint16_t klen = (uint16_t)((b[p+4] << 8) | b[p+5]);
            if (klen != 32) return -1;
            memcpy(pub, b + p + 6, 32);
            return 0;
        }
        p += l;
    }
    return -1;
}

// encrypted TLS 1.3 record: outer type APPDATA, inner content_type appended
static uint32_t enc_record(uint8_t* out, uint32_t cap,
                           uint8_t key[32], uint8_t iv[12], uint64_t* seq,
                           uint8_t inner_type, const uint8_t* pt, uint32_t pt_len) {
    uint32_t ct_len = pt_len + 1 + 16;
    if (cap < 5 + ct_len || pt_len > 16000) return 0;
    static uint8_t inner[16500];
    memcpy(inner, pt, pt_len);
    inner[pt_len] = inner_type;
    uint8_t nonce[12];
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] ^= (uint8_t)((*seq >> (56 - 8 * i)) & 0xff);
    (*seq)++;
    uint8_t tag[16];
    uint8_t aad[5] = { TLS_CT_APPDATA, 0x03, 0x03,
                       (uint8_t)(ct_len >> 8), (uint8_t)(ct_len & 0xff) };
    aead_chacha20_poly1305_encrypt(key, nonce, aad, 5, inner, pt_len + 1,
                                   out + 5, tag);
    memcpy(out + 5 + pt_len + 1, tag, 16);
    out[0] = TLS_CT_APPDATA;
    out[1] = 0x03; out[2] = 0x03;
    out[3] = (uint8_t)(ct_len >> 8);
    out[4] = (uint8_t)(ct_len & 0xff);
    return 5 + ct_len;
}

static uint32_t plain_record(uint8_t* out, uint8_t type, const uint8_t* pt, uint32_t len) {
    out[0] = type; out[1] = 0x03; out[2] = 0x03;
    out[3] = (uint8_t)(len >> 8); out[4] = (uint8_t)(len & 0xff);
    memcpy(out + 5, pt, len);
    return 5 + len;
}

static const char* mode_name(int m) {
    static const char* names[] = {
        "VALID", "RSA_PSS_VALID", "P384_VALID", "CV_WRONG_KEY", "CV_FLIPPED",
        "CV_GARBAGE_TRANSCRIPT", "HOSTNAME_MISMATCH", "EXPIRED", "KEYSHARE_SWAP",
        "CIPHER_1302", "SID_BAD", "NO_KEYSHARE", "NO_CV", "DUP_EE",
        "FIN_FLIPPED", "ALERT", "APPDATA_BITFLIP", "CLOSE_NOTIFY",
        "GARBAGE_FIRST", "RECORD_OVERFLOW", "SPLIT", "TRUNCATED"
    };
    return names[m];
}

static void mock_run(int mode, struct mock_result* res) {
    memset(res, 0, sizeof(*res));
    g_mode = mode;
    g_body_queued = 0;
    g_expect_body = 0;

    static int loaded = 0;
    static uint8_t leaf_der[4096], root_der[4096];
    static int leaf_len, root_len;
    static uint8_t p384_root_der[4096];
    static int p384_root_len;
    if (!loaded) {
        leaf_len = load("tests/adversarial/at_leaf.der", leaf_der, sizeof(leaf_der));
        root_len = load("tests/adversarial/at_root.der", root_der, sizeof(root_der));
        p384_root_len = load("tests/adversarial/at_p384_root.der",
                             p384_root_der, sizeof(p384_root_der));
        loaded = 1;
    }
    // Single extra-trust slot: point it at whichever root anchors this
    // mode's chain (P-384 modes use the P-384 root, everything else the
    // P-256 root). Cheap per-run parse; keeps modes independent.
    {
        x509_cert rc;
        const uint8_t* rd = (mode == MOCK_P384_VALID) ? p384_root_der : root_der;
        int rl = (mode == MOCK_P384_VALID) ? p384_root_len : root_len;
        if (rl > 0 && x509_parse(rd, (uint32_t)rl, &rc) == 0)
            cert_verify_trust_extra(rc.spki.p, rc.spki.len);
    }
    (void)leaf_len;

    const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nevil-ok\n";
    memcpy(g_resp, resp, strlen(resp));
    g_resp_len = (uint32_t)strlen(resp);

    q_reset();
    c_len = 0;

    const char* verify_host = "evil.example.com";
    static struct tls_state st;
    static const char req[] = "GET / HTTP/1.1\r\nHost: evil.example.com\r\n\r\n";
    tls_state_init(&st, "evil.example.com", 443,
                   (const uint8_t*)req, (uint32_t)(sizeof(req) - 1),
                   res->out, sizeof(res->out));
    if (mode == MOCK_HOSTNAME_MISMATCH) verify_host = "example.com";
    st.verify_host = verify_host;

    {
        x509_time n;
        if (mode == MOCK_EXPIRED) {
            n.year = 2026; n.month = 9; n.day = 12; n.hour = 0; n.minute = 0; n.second = 0;
        } else {
            n.year = 2026; n.month = 9; n.day = 9; n.hour = 23; n.minute = 0; n.second = 0;
        }
        x509_set_now(&n);
    }

    // step 1: SEND_CH (captures the ClientHello)
    int r = tls_state_step(&st, &mock_io);
    if (r != TLS_STEP_AGAIN || c_len < 100) {
        res->r = (r == TLS_STEP_DONE) ? -2 : r;
        res->fail_reason = st.fail_reason;
        return;
    }
    uint8_t client_sid[32], client_pub[32];
    if (parse_client_ch(c_buf, c_len, client_sid, client_pub) != 0) {
        res->r = -2;
        return;
    }

    uint8_t srv_priv[32], srv_priv_B[32], srv_pub_A[32], srv_pub_B[32];
    for (int i = 0; i < 32; i++) {
        srv_priv[i] = (uint8_t)(0xA0 + i);
        srv_priv_B[i] = (uint8_t)(0x5C + i * 3);
    }
    srv_priv[0] &= 248; srv_priv[31] &= 127; srv_priv[31] |= 64;
    srv_priv_B[0] &= 248; srv_priv_B[31] &= 127; srv_priv_B[31] |= 64;
    x25519_public_key(srv_pub_A, srv_priv);

    uint8_t shared[32];
    x25519_shared_secret(shared, srv_priv, client_pub);
    uint8_t early[32], derived[32];
    tls_early_secret(NULL, 0, early);
    tls_derive_secret(early, derived);
    tls_handshake_secret(derived, shared, g_hs_secret);

    // --- ServerHello ---
    uint8_t sh_body[160];
    uint32_t sp = 0;
    sh_body[sp++] = 0x03; sh_body[sp++] = 0x03;
    memset(sh_body + sp, 0xAB, 32); sp += 32;
    sh_body[sp++] = 32;
    memcpy(sh_body + sp, client_sid, 32); sp += 32;
    if (mode == MOCK_SID_BAD) sh_body[sp - 1] ^= 0xFF;
    sh_body[sp++] = 0x13; sh_body[sp++] = 0x03;
    if (mode == MOCK_CIPHER_1302) sh_body[sp - 1] = 0x02;
    sh_body[sp++] = 0x00;
    uint32_t ext_len_pos = sp; sp += 2;
    sh_body[sp++] = 0x00; sh_body[sp++] = 0x2b;      // supported_versions
    sh_body[sp++] = 0x00; sh_body[sp++] = 0x02;      // SH ext body = 2 bytes
    sh_body[sp++] = 0x03; sh_body[sp++] = 0x04;      // (no list len in SH)
    sh_body[sp++] = 0x00; sh_body[sp++] = 0x33;      // key_share
    sh_body[sp++] = 0x00; sh_body[sp++] = 0x24;
    sh_body[sp++] = 0x00; sh_body[sp++] = 0x1d;
    sh_body[sp++] = 0x00; sh_body[sp++] = 0x20;
    // KEYSHARE_SWAP: advertise share B while the flight stays encrypted
    // under share A — the forwarding-MITM shape (client must fail to read it)
    memcpy(sh_body + sp, srv_pub_A, 32); sp += 32;
    if (mode == MOCK_KEYSHARE_SWAP) {
        x25519_public_key(srv_pub_B, srv_priv_B);
        memcpy(sh_body + sp - 32, srv_pub_B, 32);
    }
    if (mode == MOCK_NO_KEYSHARE) sp = ext_len_pos + 2 + 6; // keep only the 6-byte supported_versions ext
    uint32_t ext_total = sp - ext_len_pos - 2;
    sh_body[ext_len_pos] = (uint8_t)(ext_total >> 8);
    sh_body[ext_len_pos + 1] = (uint8_t)(ext_total & 0xff);

    uint8_t sh_msg[200];
    sh_msg[0] = TLS_HS_SERVER_HELLO;
    sh_msg[1] = 0; sh_msg[2] = (uint8_t)(sp >> 8); sh_msg[3] = (uint8_t)(sp & 0xff);
    memcpy(sh_msg + 4, sh_body, sp);
    uint32_t sh_msg_len = 4 + sp;

    mtrans_init(&g_mt);
    mtrans_msg(&g_mt, TLS_HS_CLIENT_HELLO, c_buf + 9, c_len - 9);
    mtrans_msg(&g_mt, TLS_HS_SERVER_HELLO, sh_msg + 4, sh_msg_len - 4);
    memcpy(g_sh_body, sh_msg + 4, sh_msg_len - 4);
    g_sh_len = sh_msg_len - 4;

    uint8_t tx_after_sh[32];
    mtrans_hash(&g_mt, tx_after_sh);
    uint8_t s_hs_traffic[32];
    tls_traffic_secret(g_hs_secret, "s hs traffic", tx_after_sh, s_hs_traffic);
    tls_traffic_secret(g_hs_secret, "c hs traffic", tx_after_sh, g_c_hs_traffic);
    uint8_t s_hs_key[32], s_hs_iv[12];
    tls_record_key(s_hs_traffic, s_hs_key);
    tls_record_iv(s_hs_traffic, s_hs_iv);

    uint8_t rec[16500];
    uint64_t s_seq = 0;
    uint32_t rl = plain_record(rec, TLS_CT_HANDSHAKE, sh_msg, sh_msg_len);
    q_put(rec, rl);

    if (mode == MOCK_GARBAGE_FIRST) {
        q_reset();
        uint8_t junk[100];
        for (int i = 0; i < 100; i++) junk[i] = (uint8_t)(0x30 + (i * 7) & 0xff);
        q_put(junk, 100);
    } else if (mode == MOCK_RECORD_OVERFLOW) {
        q_reset();
        uint8_t bad[5] = { TLS_CT_HANDSHAKE, 0x03, 0x03, 0xFF, 0xFF };
        q_put(bad, 5);
    } else if (mode == MOCK_ALERT) {
        q_reset();
        uint8_t al[2] = { 0x02, 0x28 };   // fatal handshake_failure
        rl = plain_record(rec, TLS_CT_ALERT, al, 2);
        q_put(rec, rl);
    } else if (mode != MOCK_CIPHER_1302 && mode != MOCK_SID_BAD &&
               mode != MOCK_NO_KEYSHARE) {
        // ---- encrypted flight ----
        uint8_t ee_msg[6] = { TLS_HS_ENCRYPTED_EXTENSIONS, 0, 0, 2, 0, 0 };
        mtrans_msg(&g_mt, TLS_HS_ENCRYPTED_EXTENSIONS, ee_msg + 4, 2);

        const char* chain[3] = { 0, 0, 0 };
        static uint8_t cert_msg[16384];
        const char* leaf_file = (mode == MOCK_EXPIRED)
            ? "tests/adversarial/at_leaf_expired.der"
            : (mode == MOCK_RSA_PSS_VALID)
            ? "tests/adversarial/at_rsa_leaf.der"
            : (mode == MOCK_P384_VALID)
            ? "tests/adversarial/at_p384_leaf.der"
            : "tests/adversarial/at_leaf.der";
        chain[0] = leaf_file;
        chain[1] = (mode == MOCK_P384_VALID)
            ? "tests/adversarial/at_p384_int.der"
            : "tests/adversarial/at_int.der";
        chain[2] = (mode == MOCK_P384_VALID)
            ? "tests/adversarial/at_p384_root.der"
            : "tests/adversarial/at_root.der";   // anchor terminates the path
        uint32_t cert_len = build_cert_msg(cert_msg, sizeof(cert_msg), chain, 3);
        if (cert_len == 0) { res->r = -3; return; }
        mtrans_msg(&g_mt, TLS_HS_CERTIFICATE, cert_msg, cert_len);

        // CertificateVerify over spaces||label||0||TH(CH..CERT)
        uint8_t th_cert[32];
        mtrans_hash(&g_mt, th_cert);
        uint8_t signed_data[64 + 33 + 1 + 32];
        memset(signed_data, 0x20, 64);
        memcpy(signed_data + 64, "TLS 1.3, server CertificateVerify", 33);
        signed_data[64 + 33] = 0x00;
        memcpy(signed_data + 64 + 33 + 1, th_cert, 32);
        if (mode == MOCK_CV_GARBAGE_TRANSCRIPT)
            memset(signed_data + 64 + 33 + 1, 0x5A, 32);

        int is_rsa = (mode == MOCK_RSA_PSS_VALID);
        int is_p384 = (mode == MOCK_P384_VALID);
        const char* leaf_key = is_rsa ? "tests/adversarial/at_rsa_leaf.key"
                             : is_p384 ? "tests/adversarial/at_p384_leaf.key"
                                       : "tests/adversarial/at_leaf.key";
        const char* sign_key = (mode == MOCK_CV_WRONG_KEY)
            ? "tests/adversarial/at_root.key" : leaf_key;
        uint16_t cv_alg = is_rsa ? 0x0804 : is_p384 ? 0x0503 : 0x0403;

        uint8_t cv_sig[1024];
        uint32_t cv_sig_len = 0;
        if (sign_digest(sign_key, signed_data, sizeof(signed_data), is_rsa,
                        is_p384, cv_sig, &cv_sig_len) != 0) {
            printf("    [mock] openssl signing failed (%s)\n", mode_name(mode));
            res->r = -3;
            return;
        }
        if (mode == MOCK_CV_FLIPPED && cv_sig_len > 0)
            cv_sig[cv_sig_len - 1] ^= 0x01;

        uint8_t cv_msg[1200];
        cv_msg[0] = TLS_HS_CERTIFICATE_VERIFY;
        cv_msg[1] = 0;
        cv_msg[2] = (uint8_t)((4 + cv_sig_len) >> 8);
        cv_msg[3] = (uint8_t)((4 + cv_sig_len) & 0xff);
        cv_msg[4] = (uint8_t)(cv_alg >> 8);
        cv_msg[5] = (uint8_t)(cv_alg & 0xff);
        cv_msg[6] = (uint8_t)(cv_sig_len >> 8);
        cv_msg[7] = (uint8_t)(cv_sig_len & 0xff);
        memcpy(cv_msg + 8, cv_sig, cv_sig_len);
        uint32_t cv_msg_len = 8 + cv_sig_len;
        mtrans_msg(&g_mt, TLS_HS_CERTIFICATE_VERIFY, cv_msg + 4, cv_msg_len - 4);

        // server Finished over the transcript through CV
        uint8_t tx_cv[32], s_fin_key[32], fin[32];
        mtrans_hash(&g_mt, tx_cv);
        tls_finished_key(s_hs_traffic, s_fin_key);
        hmac_sha256(s_fin_key, 32, tx_cv, 32, fin);
        if (mode == MOCK_FIN_FLIPPED) fin[5] ^= 0x10;

        // assemble the flight
        static uint8_t flight[16384];
        uint32_t fl = 0;
        memcpy(flight + fl, ee_msg, 6); fl += 6;
        // Certificate: full handshake message = header(4) + body
        flight[fl] = TLS_HS_CERTIFICATE;
        flight[fl+1] = (uint8_t)((cert_len >> 16) & 0xff);
        flight[fl+2] = (uint8_t)((cert_len >> 8) & 0xff);
        flight[fl+3] = (uint8_t)(cert_len & 0xff);
        fl += 4;
        memcpy(flight + fl, cert_msg, cert_len); fl += cert_len;
        if (mode != MOCK_NO_CV) {
            memcpy(flight + fl, cv_msg, cv_msg_len); fl += cv_msg_len;
        }
        uint8_t fin_msg[36] = { TLS_HS_FINISHED, 0, 0, 32, 0 };
        memcpy(fin_msg + 4, fin, 32);
        memcpy(g_sfin_msg, fin_msg, 36);
        g_sfin_len = 36;
        memcpy(flight + fl, fin_msg, 36); fl += 36;

        if (mode == MOCK_DUP_EE) {
            memmove(flight + 6, flight, fl);
            memcpy(flight, ee_msg, 6);
            fl += 6;
        }

        rl = enc_record(rec, sizeof(rec), s_hs_key, s_hs_iv, &s_seq,
                        TLS_CT_HANDSHAKE, flight, fl);
        if (rl == 0) { res->r = -3; return; }
        q_put(rec, rl);

        g_expect_body = (mode == MOCK_VALID || mode == MOCK_RSA_PSS_VALID ||
                          mode == MOCK_P384_VALID || mode == MOCK_SPLIT ||
                          mode == MOCK_CLOSE_NOTIFY || mode == MOCK_APPDATA_BITFLIP);
        // TRUNCATED: cut the queue mid-flight-record (SH complete + 30B of
        // the encrypted flight). The client must ERR on the short close —
        // never DONE (partial bytes are not a page) and never spin: the
        // step loop below caps at 500 iters and the CHECK requires ERR.
        if (mode == MOCK_TRUNCATED && q_len > 140) q_len = 140;
    }

    int iters = 0;
    do {
        r = tls_state_step(&st, &mock_io);
        if (++iters > 500) break;
    } while (r == TLS_STEP_AGAIN);

    res->r = r;
    res->fail_reason = st.fail_reason;
    res->alert_desc = st.alert_desc;
    res->out_len = st.out_len;
}

static void mock_section(void) {
    printf("== 2. adversarial mock TLS server ==\n");

    struct mock_result res;
    x509_time now = { 2026, 9, 9, 14, 0, 0 };
    x509_set_now(&now);

    mock_run(MOCK_VALID, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "positive control: valid ECDSA flight completes");
    mock_run(MOCK_RSA_PSS_VALID, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0,
          "positive control: RSA leaf + PSS CertificateVerify completes");
    mock_run(MOCK_P384_VALID, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0,
          "positive control: P-384 leaf + ECDSA-SHA384 CertificateVerify completes");

    mock_run(MOCK_CV_WRONG_KEY, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_CERT,
          "CV signed by the wrong key rejected");
    mock_run(MOCK_CV_FLIPPED, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_CERT,
          "CV signature byte flip rejected");
    mock_run(MOCK_CV_GARBAGE_TRANSCRIPT, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_CERT,
          "CV over the wrong transcript rejected");
    mock_run(MOCK_HOSTNAME_MISMATCH, &res);
    CHECK(res.r == TLS_STEP_ERR &&
          (res.fail_reason == TLS_FAIL_HOSTNAME || res.fail_reason == TLS_FAIL_CERT),
          "hostname mismatch rejected");
    mock_run(MOCK_EXPIRED, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_CERT,
          "expired certificate rejected");
    mock_run(MOCK_KEYSHARE_SWAP, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_MAC,
          "key-share swap (forwarding MITM): flight unreadable, rejected");
    mock_run(MOCK_CIPHER_1302, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_PROTO,
          "unoffered cipher suite rejected");
    mock_run(MOCK_SID_BAD, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_PROTO,
          "corrupted session-id echo rejected");
    mock_run(MOCK_NO_KEYSHARE, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_PROTO,
          "key-share-less ServerHello (HRR shape) rejected");
    mock_run(MOCK_NO_CV, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_PROTO,
          "missing CertificateVerify rejected (flight order)");
    mock_run(MOCK_DUP_EE, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_PROTO,
          "duplicate EncryptedExtensions rejected (flight order)");
    mock_run(MOCK_FIN_FLIPPED, &res);
    CHECK(res.r == TLS_STEP_ERR &&
          (res.fail_reason == TLS_FAIL_CERT || res.fail_reason == TLS_FAIL_PROTO ||
           res.fail_reason == TLS_FAIL_MAC),
          "server Finished bit-flip rejected");
    mock_run(MOCK_ALERT, &res);
    // pre-encryption alert (before the SH) is a plaintext record: the client
    // classifies it as a protocol violation in RECV_SH.
    CHECK(res.r == TLS_STEP_ERR &&
          (res.fail_reason == TLS_FAIL_ALERT || res.fail_reason == TLS_FAIL_PROTO),
          "fatal alert rejected");
    mock_run(MOCK_APPDATA_BITFLIP, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_MAC,
          "app-data ciphertext bit-flip rejected (AEAD)");
    mock_run(MOCK_CLOSE_NOTIFY, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          res.fail_reason == TLS_FAIL_ALERT_CLOSE,
          "close_notify ends the fetch cleanly");
    mock_run(MOCK_GARBAGE_FIRST, &res);
    CHECK(res.r == TLS_STEP_ERR, "record-layer garbage rejected");
    mock_run(MOCK_RECORD_OVERFLOW, &res);
    CHECK(res.r == TLS_STEP_ERR, "oversized record length rejected");
    mock_run(MOCK_SPLIT, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "split delivery (7B/recv) still completes");
    mock_run(MOCK_TRUNCATED, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "truncated flight rejected (no hang, no partial DONE)");
}

int main(void) {
    fuzz_section();
    mock_section();
    printf("\n%s: %d passed, %d failed\n",
           failures == 0 ? "ADVERSARIAL TESTS PASS" : "ADVERSARIAL TESTS FAIL",
           passes, failures);
    return failures == 0 ? 0 : 1;
}
