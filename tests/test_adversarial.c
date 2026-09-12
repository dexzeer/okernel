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
#include <time.h>

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

// ---------------------------------------------------------------- fuzz-0
// Parser robustness fuzz (review 2026-09-10 P0): every externally reachable
// parser gets random garbage + truncated valid inputs. Requirements: no
// crash (ASan/UBSan build), return codes in {-1, 0} (or [CV_OK,
// CV_ERR_MAX] for cert_verify). Deterministic LCG — reproducible.
static void parser_fuzz_section(void) {
    printf("== 0. parser robustness fuzz ==\n");
    uint64_t frng = 0x243F6A8885A308D3ull;
#define FNEXT32() (frng ^= frng << 13, frng ^= frng >> 7, frng ^= frng << 17, \
                   (uint32_t)(frng & 0xFFFFFFFFu))
    static uint8_t fbuf[2048];
    static uint8_t sid[32];
    {
        // Clock for the cert_verify garbage rounds (garbage dies at PARSE
        // before dates matter, but keep the state sane for the suite).
        x509_time fn = { 2026, 9, 10, 12, 0, 0 };
        x509_set_now(&fn);
    }
    int bad = 0;
    // Random garbage, lengths 0..600 (covers empty, truncated, oversized).
    for (int round = 0; round < 4000; round++) {
        uint32_t len = FNEXT32() % 601;
        for (uint32_t i = 0; i < len; i++) fbuf[i] = (uint8_t)(FNEXT32() & 0xff);
        {
            tls_record vr;
            int r = tls_record_parse_header(fbuf, len < 5 ? len : 5, &vr);
            if (r != 0 && r != 5) bad++;
        }
        {
            tls_server_hello sh;
            int r = tls_parse_server_hello(fbuf, len, sid, &sh);
            if (r != 0 && r != -1) bad++;
        }
        if (tls_parse_certificate(fbuf, len) != 0 &&
            tls_parse_certificate(fbuf, len) != -1) bad++;
        if (tls_parse_certificate_verify(fbuf, len) != 0 &&
            tls_parse_certificate_verify(fbuf, len) != -1) bad++;
        {
            uint16_t a; const uint8_t* s; uint32_t sl;
            int q = tls_parse_cv_sig(fbuf, len, &a, &s, &sl);
            if (q != 0 && q != -1) bad++;
        }
        {
            struct tls_nst n;
            int q = tls_parse_nst(fbuf, len, &n);
            if (q != 0 && q != -1) bad++;
        }
        if (tls_parse_sh_psk(fbuf, len) < -2 ||
            tls_parse_sh_psk(fbuf, len) > 0) bad++;
        {
            int present = 0;
            int q = tls_cert_has_staple(fbuf, len, &present, NULL, NULL);
            if (q != 0 && q != -1) bad++;
        }
        {
            x509_cert c;
            int q = x509_parse(fbuf, len > 1500 ? 1500 : len, &c);
            if (q != 0 && q != -1) bad++;
        }
        {
            int q = cert_verify(fbuf, len > 400 ? 400 : len, "example.com");
            if (q < CV_OK || q > CV_ERR_MAX) bad++;
        }
    }
    CHECK(bad == 0, "4000 garbage rounds: no crash, codes in range");
#undef FNEXT32
}

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
        else if (r > CV_OK && r <= CV_ERR_MAX) by_other++;
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
        if (r < CV_OK || r > CV_ERR_MAX) bad_code++;
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
    MOCK_SH_FLIPPED,   // SH random byte flipped (transcript fork: must ERR)
    MOCK_EE_FLIPPED,   // EE body byte flipped (transcript fork: must ERR)
    MOCK_NST,          // valid flight + NewSessionTicket + body (ticket stored)
    MOCK_NST_BAD,      // corrupt ticket (must ERR, nothing stored)
    MOCK_NST_BIGNONCE, // 65B nonce (dropped, flight completes)
    MOCK_PSK_ACCEPT,   // offer verified (binder) + abbreviated flight (DONE)
    MOCK_PSK_FALLBACK, // offer ignored, full flight (DONE via fallback)
    MOCK_PSK_FOREIGN,  // server selects unoffered identity (must ERR)
    MOCK_CV_PKCS1,     // RSA PKCS#1 v1.5 CV (RFC-forbidden, must ERR)
    MOCK_STAPLE,       // stapled status_request noted, flight completes
    MOCK_STAPLE_BAD,   // malformed staple (must ERR)
    MOCK_STAPLE_REVOKED, // valid sig, revoked status (must ERR)
    MOCK_STAPLE_STALE,   // valid sig, aged past nextUpdate (must ERR)
    MOCK_SH_BIG,       // >4KB ServerHello (must ERR, OOB regression)
    MOCK_SH_TRAIL,     // SH with trailing bytes past exts (must ERR)
    MOCK_SH_DUP,       // SH with duplicate extension (must ERR)
    MOCK_EE_DUP,       // EE with duplicate extension (must ERR)
    MOCK_EE_ALPN_H2,   // EE selecting h2 (must ERR — we speak http/1.1)
    MOCK_FRAGMENT,     // flight split mid-Certificate (must DONE)
    MOCK_SH_SPLIT,     // ServerHello split across 2 records (must DONE)
    MOCK_APP_TRUNCATED, // valid flight+body, last app record cut (must ERR)
    MOCK_COUNT
};

struct mock_result {
    int r;
    int fail_reason;
    int alert_desc;
    uint32_t out_len;
    uint8_t out[4096];
};

// Clocks derived from the TEST PKI mtimes (gen_pki.sh stamps notBefore at
// generation time): normal = leaf mtime + 2h (inside every 3650-day
// window), expired-case = expired-leaf mtime + 2 days (past its 1-day
// notAfter). Fixed calendar dates rot on every regen and red the whole
// suite (bisected 2026-09-10: notBefore=Sep-10 vs clock Sep-09); mtimes
// never rot. Falls back to 2026-09-10 when stat fails.
static void adv_clocks(x509_time* normal, x509_time* expired) {
    time_t base = 0, expb = 0;
    struct stat st;
    if (stat("tests/adversarial/at_leaf.der", &st) == 0) base = st.st_mtime;
    if (stat("tests/adversarial/at_leaf_expired.der", &st) == 0) expb = st.st_mtime;
    time_t tn = base ? base + 7200 : 1780272000;   // ~2026-09-10
    time_t te = expb ? expb + 172800 : 1780444800; // ~gen+2d
    struct tm* g = gmtime(&tn);
    normal->year = 1900 + g->tm_year; normal->month = g->tm_mon + 1;
    normal->day = g->tm_mday; normal->hour = g->tm_hour;
    normal->minute = g->tm_min; normal->second = g->tm_sec;
    g = gmtime(&te);
    expired->year = 1900 + g->tm_year; expired->month = g->tm_mon + 1;
    expired->day = g->tm_mday; expired->hour = g->tm_hour;
    expired->minute = g->tm_min; expired->second = g->tm_sec;
}

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
static uint8_t g_sh_body[4352]; // SH bodies to the 4KB+ SH_BIG size
static uint32_t g_sh_len;
static uint8_t g_sfin_msg[36];
static uint32_t g_sfin_len;
static int g_body_queued;
static int g_expect_body;
static int g_mode;
// Cross-run resumption truth (MOCK_NST → MOCK_PSK_*): the ticket bytes the
// mock issued plus the resumption master it independently derived (same
// inputs as the client: full key schedule + both-fin transcript). Lets the
// ACCEPT mock verify the offered binder with no shared secrets beyond the
// protocol itself.
static uint8_t psv_ticket[64]; static uint32_t psv_ticket_len;
static uint8_t psv_nonce[16]; static uint32_t psv_nonce_len;
static uint8_t psv_res_master[32]; static int psv_have;
// Set when the mock sends the abbreviated (EE+Finished, no cert) flight.
static int g_abbrev;
// Last run's staple flag (client state), for the STAPLE CHECKs.
static int g_staple_seen;

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
    mtrans_msg(&g_mt, TLS_HS_FINISHED, fin_pt + 4, 32);
    // Resumption truth for the later PSK modes: resumption_master over the
    // both-Finished transcript (g_mt now holds exactly CH..cFin), saved iff
    // this run issued the ticket the client will offer back.
    if (g_mode == MOCK_NST) {
        uint8_t tx_both[32], derived2[32], master[32];
        mtrans_hash(&g_mt, tx_both);
        tls_derive_secret(g_hs_secret, derived2);
        tls_master_secret(derived2, master);
        tls_traffic_secret(master, "res master", tx_both, psv_res_master);
        for (int i = 0; i < 32; i++) psv_ticket[i] = (uint8_t)(0xA0 + i);
        psv_ticket_len = 32;
        for (int i = 0; i < 8; i++) psv_nonce[i] = (uint8_t)(0xC0 + i);
        psv_nonce_len = 8;
        psv_have = 1;
    }

    uint8_t derived2[32], master[32];
    tls_derive_secret(g_hs_secret, derived2);
    tls_master_secret(derived2, master);
    tls_traffic_secret(master, "s ap traffic", tx_pre_cfin, g_s_ap_traffic);
    tls_record_key(g_s_ap_traffic, g_s_ap_key);
    tls_record_iv(g_s_ap_traffic, g_s_ap_iv);
    g_s_ap_seq = 0;

    static uint8_t rec[16500];
    // NST modes: encode + queue the ticket record FIRST (seq 0) so record
    // sequence numbers match queue order (body follows at seq 1).
    if (g_mode == MOCK_NST || g_mode == MOCK_NST_BAD ||
        g_mode == MOCK_NST_BIGNONCE) {
        static uint8_t nst[256];
        uint32_t np = 0;
        uint32_t nonce_len = (g_mode == MOCK_NST_BIGNONCE) ? 65 : 8;
        nst[np++] = 0; nst[np++] = 0; nst[np++] = 0; nst[np++] = 40; // hs header placeholder
        nst[np++] = 0; nst[np++] = 0; nst[np++] = 0x0e; nst[np++] = 0x10; // lifetime 3600
        nst[np++] = 0x12; nst[np++] = 0x34; nst[np++] = 0x56; nst[np++] = 0x78; // age_add
        nst[np++] = (uint8_t)nonce_len;
        for (uint32_t i = 0; i < nonce_len; i++) nst[np++] = (uint8_t)(0xC0 + i); // nonce
        nst[np++] = 0; nst[np++] = 32; for (int i = 0; i < 32; i++) nst[np++] = (uint8_t)(0xA0 + i); // ticket
        nst[np++] = 0; nst[np++] = 0; // no extensions
        // Fix the handshake header: type NST(4), body len = np - 4.
        nst[0] = TLS_HS_NEW_SESSION_TICKET;
        uint32_t nbl = np - 4;
        nst[1] = (uint8_t)((nbl >> 16) & 0xff);
        nst[2] = (uint8_t)((nbl >> 8) & 0xff);
        nst[3] = (uint8_t)(nbl & 0xff);
        // NST_BAD: zero the ticket_len (valid framing, empty ticket — the
        // parser must reject; ticket_len sits at 4(hs)+4+4+1+8 = offset 21).
        if (g_mode == MOCK_NST_BAD) { nst[4 + 17] = 0; nst[4 + 18] = 0; }
        // NOTE: separate record buffer — rec is used for the body below.
        static uint8_t nrec[16500];
        uint32_t nrl = enc_record(nrec, sizeof(nrec), g_s_ap_key, g_s_ap_iv,
                                  &g_s_ap_seq, TLS_CT_HANDSHAKE, nst, np);
        if (nrl == 0) { printf("    [tqb] NST enc_record returned 0\n"); return 0; }
        q_put(nrec, nrl);
    }
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
    // APP_TRUNCATED (cryptoholes #1): the body was just queued above; cut
    // the TAIL mid-app-record (10B off the end lands inside the last
    // record: 5B header + ~50B body + 16B tag). EOF with a partial record
    // buffered must ERR (PROTO truncation), never DONE as a full page.
    if (g_mode == MOCK_APP_TRUNCATED && q_len > 30) q_len -= 10;
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
        "GARBAGE_FIRST", "RECORD_OVERFLOW", "SPLIT", "TRUNCATED",
        "SH_FLIPPED", "EE_FLIPPED", "NST", "NST_BAD", "NST_BIGNONCE",
        "PSK_ACCEPT", "PSK_FALLBACK", "PSK_FOREIGN", "CV_PKCS1",
        "STAPLE", "STAPLE_BAD", "STAPLE_REVOKED", "STAPLE_STALE", "SH_BIG",
        "SH_TRAIL", "SH_DUP", "EE_DUP", "EE_ALPN_H2", "FRAGMENT",
        "SH_SPLIT", "APP_TRUNCATED"
    };
    return names[m];
}

// Parse the client's offered pre_shared_key (last CH extension): extracts
// the identity bytes + binder, and INDEPENDENTLY recomputes the expected
// binder (separate truncation code from the client's: parse-driven, not
// offset arithmetic). Returns 1 iff identity == psv_ticket AND the binder
// verifies under psv_res_master. Anything else (absent/malformed/mismatch)
// returns 0 — the mock must then ignore (fallback) or abort per mode.
static int mock_check_psk_offer(void) {
    if (!psv_have) return 0;
    if (c_len < 5 + 4) return 0;
    uint32_t ch_rplen = ((uint32_t)c_buf[3] << 8) | c_buf[4];
    if (c_buf[0] != TLS_CT_HANDSHAKE || 5 + ch_rplen > c_len) return 0;
    if (c_buf[5] != TLS_HS_CLIENT_HELLO) return 0;
    uint32_t body_len = ((uint32_t)c_buf[6] << 16) |
                        ((uint32_t)c_buf[7] << 8) | c_buf[8];
    if (5 + 4 + body_len > c_len || body_len < 34) return 0;
    const uint8_t* body = c_buf + 9;
    // Walk to extensions: ver(2) random(32) sid(u8+n) ciphers(u16+n)
    // comp(u8+n) ext_total(u16).
    uint32_t p = 2 + 32;
    if (p + 1 > body_len) return 0;
    p += 1 + body[p];
    if (p + 2 > body_len) return 0;
    p += 2 + ((body[p] << 8) | body[p+1]);
    if (p + 1 > body_len) return 0;
    p += 1 + body[p];
    if (p + 2 > body_len) return 0;
    uint32_t ext_total = (body[p] << 8) | body[p+1]; p += 2;
    uint32_t eend = p + ext_total;
    if (eend > body_len) return 0;
    const uint8_t* id = NULL; uint32_t id_len = 0;
    const uint8_t* binder = NULL;
    int modes_ok = 0; // psk_key_exchange_modes must offer psk_dhe_ke (1);
    // real servers silently decline the PSK otherwise (interop 2026-09-11:
    // we once sent mode 2 and every live server fell back — mock it).
    while (p + 4 <= eend) {
        uint16_t et = (uint16_t)((body[p] << 8) | body[p+1]);
        uint16_t l = (uint16_t)((body[p+2] << 8) | body[p+3]);
        p += 4;
        if (p + l > eend) return 0;
        if (et == 45) { // psk_key_exchange_modes
            for (uint16_t mi = 1; mi < l; mi++)
                if (body[p + mi] == 1) modes_ok = 1;
        }
        if (et == 41) { // pre_shared_key (last ext by construction)
            if (l < 2 + 2 + 4 + 2 + 1 + 32) return 0;
            uint32_t q = p;
            uint32_t list_len = (body[q] << 8) | body[q+1]; q += 2;
            uint32_t ilen = (body[q] << 8) | body[q+1]; q += 2;
            if (2 + ilen + 4 > list_len) return 0;
            id = body + q; id_len = ilen; q += ilen + 4; // skip age
            if (q + 2 > p + l) return 0;
            uint32_t blen = (body[q] << 8) | body[q+1]; q += 2;
            // RFC 8446 §4.2.11.2: binders<33..> with a u8-prefixed
            // PskBinderEntry — 33B total, entry len byte must be 32.
            if (blen != 33 || q + 33 > p + l) return 0;
            if (body[q] != 32) return 0;
            binder = body + q + 1;
        }
        p += l;
    }
    if (!id || !binder || id_len != psv_ticket_len) return 0;
    if (!modes_ok) return 0; // no common kex mode: server must ignore PSK
    for (uint32_t i = 0; i < id_len; i++)
        if (id[i] != psv_ticket[i]) return 0;
    // Independent binder recompute: ClientHello1 = type || len(trunc) ||
    // body[:trunc], trunc right after the u16 binders-length field (the
    // binder entry prefix + 32B value are the last 33B of the body).
    uint32_t trunc = body_len - 33;
    uint8_t psk[32], early[32], bkey[32], th[32], want[32];
    tls_resumption_psk(psv_res_master, psv_nonce, psv_nonce_len, psk);
    tls_early_secret(psk, 32, early);
    tls_psk_binder_key(early, bkey);
    {
        tls_transcript t;
        tls_transcript_init(&t);
        uint8_t hdr[4];
        hdr[0] = TLS_HS_CLIENT_HELLO;
        hdr[1] = (uint8_t)((trunc >> 16) & 0xff);
        hdr[2] = (uint8_t)((trunc >> 8) & 0xff);
        hdr[3] = (uint8_t)(trunc & 0xff);
        tls_transcript_update(&t, hdr, 4);
        tls_transcript_update(&t, body, trunc);
        tls_transcript_final(&t, th);
    }
    hmac_sha256(bkey, 32, th, 32, want);
    for (int i = 0; i < 32; i++)
        if (want[i] != binder[i]) return 0;
    return 1;
}

// Mint a real OCSP response with openssl (responder = at_int itself) for
// the CURRENT at_leaf (serial read at runtime — regen-safe). revoked != 0
// marks the leaf revoked in a scratch index. Returns response bytes in out
// (cap-checked), length via out_len. Mirrors sign_digest's system() style.
static int mint_ocsp(int revoked, uint8_t* out, uint32_t cap,
                     uint32_t* out_len) {
    static char cmd[1024], serial[128];
    // Leaf serial (hex, no 0x).
    snprintf(cmd, sizeof(cmd),
             "openssl x509 -in tests/adversarial/at_leaf.der -inform DER "
             "-noout -serial 2>/dev/null");
    FILE* pf = popen(cmd, "r");
    if (!pf) return -1;
    if (!fgets(serial, sizeof(serial), pf)) { pclose(pf); return -1; }
    pclose(pf);
    char* nl = strchr(serial, '\n');
    if (nl) *nl = 0;
    if (strncmp(serial, "serial=", 7) != 0) return -1;
    const char* hex = serial + 7;
    // Scratch CA database (V = valid, R = revoked).
    mkdir(SCRATCH_DIR, 0777);
    snprintf(cmd, sizeof(cmd), SCRATCH_DIR "/ocsp_index");
    FILE* f = fopen(cmd, "w");
    if (!f) return -1;
    // expiry far future; revocation date = now for R entries.
    fprintf(f, "%c\t30000101000000Z\t%s\t%s\t%s\t/CN=evil.example.com\n",
            revoked ? 'R' : 'V', revoked ? "260101000000Z" : "",
            hex, revoked ? "unknown" : "unknown");
    fclose(f);
    // Responder bundle (cert + key) for -rsigner.
    snprintf(cmd, sizeof(cmd),
             "cat tests/adversarial/at_int.key tests/adversarial/at_int.pem "
             "> " SCRATCH_DIR "/ocsp_rsigner.pem 2>/dev/null");
    if (system(cmd) != 0) return -1;
    // Request for the leaf, then the response.
    snprintf(cmd, sizeof(cmd),
             "openssl ocsp -issuer tests/adversarial/at_int.pem "
             "-cert tests/adversarial/at_leaf.pem -reqout " SCRATCH_DIR "/ocsp_req.der "
             "2>/dev/null");
    if (system(cmd) != 0) return -1;
    snprintf(cmd, sizeof(cmd),
             "openssl ocsp -index " SCRATCH_DIR "/ocsp_index "
             "-CA tests/adversarial/at_int.pem "
             "-rsigner " SCRATCH_DIR "/ocsp_rsigner.pem "
             "-reqin " SCRATCH_DIR "/ocsp_req.der "
             "-respout " SCRATCH_DIR "/ocsp_resp.der -ndays 7 2>/dev/null");
    if (system(cmd) != 0) return -1;
    snprintf(cmd, sizeof(cmd), SCRATCH_DIR "/ocsp_resp.der");
    f = fopen(cmd, "rb");
    if (!f) return -1;
    int n = (int)fread(out, 1, cap, f);
    fclose(f);
    if (n <= 0) return -1;
    *out_len = (uint32_t)n;
    return 0;
}

// Patch the first CertificateEntry of a built Certificate message body to
// carry a status_request extension (RFC 8446 §4.4.2.1): type 5, body
// type=ocsp(1) + response bytes (or the legacy 1-byte dummy when resp is
// NULL). bad != 0 makes the inner length lie (claims 8, holds 5).
// Returns the new message length (list_len fixed up), 0 on error.
static uint32_t staple_patch(uint8_t* msg, uint32_t cap, uint32_t len,
                             const uint8_t* resp, uint32_t resplen, int bad) {
    static const uint8_t ext_bad[] =
        { 0x00,0x09, 0x00,0x05, 0x00,0x14, 0x01, 0x00,0x00, 0xAA, 0xAA };
    if (bad) {
        if (len < 1 + 3 + 3 + 2 || len + 9 > cap) return 0;
        uint32_t list_len = ((uint32_t)msg[1] << 16) |
                            ((uint32_t)msg[2] << 8) | msg[3];
        uint32_t cert_len = ((uint32_t)msg[4] << 16) |
                            ((uint32_t)msg[5] << 8) | msg[6];
        uint32_t eoff = 4 + 3 + cert_len;
        if (eoff + 2 > len || msg[eoff] != 0 || msg[eoff+1] != 0) return 0;
        memmove(msg + eoff + 11, msg + eoff + 2, len - (eoff + 2));
        memcpy(msg + eoff, ext_bad, 11);
        list_len += 9;
        msg[1] = (uint8_t)((list_len >> 16) & 0xff);
        msg[2] = (uint8_t)((list_len >> 8) & 0xff);
        msg[3] = (uint8_t)(list_len & 0xff);
        return len + 9;
    }
    // Real staple: ext = type(2) + extlen(2) + body(type(1) + rlen(3) + resp).
    uint32_t body_len = 1 + 3 + resplen;
    uint32_t ext_total = 4 + body_len;
    uint32_t block = 2 + ext_total; // entry ext_len field + extensions
    if (!resp || resplen == 0 || resplen > 1800) return 0;
    if (len < 1 + 3 + 3 + 2 || len + block - 2 > cap) return 0;
    uint32_t list_len = ((uint32_t)msg[1] << 16) |
                        ((uint32_t)msg[2] << 8) | msg[3];
    uint32_t cert_len = ((uint32_t)msg[4] << 16) |
                        ((uint32_t)msg[5] << 8) | msg[6];
    uint32_t eoff = 4 + 3 + cert_len;
    if (eoff + 2 > len || msg[eoff] != 0 || msg[eoff+1] != 0) return 0;
    memmove(msg + eoff + block, msg + eoff + 2, len - (eoff + 2));
    uint8_t* e = msg + eoff;
    e[0] = (uint8_t)((ext_total >> 8) & 0xff);
    e[1] = (uint8_t)(ext_total & 0xff);
    e[2] = 0x00; e[3] = 0x05;
    e[4] = (uint8_t)((body_len >> 8) & 0xff);
    e[5] = (uint8_t)(body_len & 0xff);
    e[6] = 0x01;
    e[7] = (uint8_t)((resplen >> 16) & 0xff);
    e[8] = (uint8_t)((resplen >> 8) & 0xff);
    e[9] = (uint8_t)(resplen & 0xff);
    memcpy(e + 10, resp, resplen);
    list_len += block - 2;
    msg[1] = (uint8_t)((list_len >> 16) & 0xff);
    msg[2] = (uint8_t)((list_len >> 8) & 0xff);
    msg[3] = (uint8_t)(list_len & 0xff);
    return len + block - 2;
}

static void mock_run(int mode, struct mock_result* res) {
    memset(res, 0, sizeof(*res));
    g_mode = mode;
    g_body_queued = 0;
    g_expect_body = 0;
    g_abbrev = 0;
    g_staple_seen = 0;

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
    tls_state_set_now_ms(&st, 1000000); // exercise the ticket-age clock path
    if (mode == MOCK_HOSTNAME_MISMATCH) verify_host = "example.com";
    st.verify_host = verify_host;

    {
        x509_time n, nx;
        adv_clocks(&n, &nx);
        if (mode == MOCK_EXPIRED) n = nx;
        // STAPLE modes validate openssl-minted responses (thisUpdate =
        // wall-clock mint time, nextUpdate +7d). The mtime-frozen suite
        // clock drifts behind wall time (fixtures age; skew tolerance is
        // 1d), so these modes run on wall clock like STALE does — the
        // chain windows are 10y wide, so wall stays inside for a decade.
        // (Bisected 2026-09-11: mtime-clock +26h reds the good-staple
        // check as future-dated. Environmental, not a stack bug.)
        if (mode == MOCK_STAPLE || mode == MOCK_STAPLE_BAD ||
            mode == MOCK_STAPLE_REVOKED) {
            time_t tt = time(0);
            struct tm* g = gmtime(&tt);
            n.year = 1900 + g->tm_year; n.month = g->tm_mon + 1;
            n.day = g->tm_mday; n.hour = g->tm_hour;
            n.minute = g->tm_min; n.second = g->tm_sec;
        }
        // STAPLE_STALE: the minted response is fresh as of real-now; run
        // the validation clock 30 days ahead so it reads as aged past its
        // 7-day nextUpdate (the stale condition, no openssl date tricks).
        if (mode == MOCK_STAPLE_STALE) {
            time_t tt = time(0) + 30 * 86400;
            struct tm* g = gmtime(&tt);
            n.year = 1900 + g->tm_year; n.month = g->tm_mon + 1;
            n.day = g->tm_mday; n.hour = g->tm_hour;
            n.minute = g->tm_min; n.second = g->tm_sec;
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
    // PSK_ACCEPT runs on the resumption-derived early secret (same inputs
    // as the client: saved resumption master + ticket nonce). Everything
    // else uses the zero early secret (plain 1-RTT).
    if (mode == MOCK_PSK_ACCEPT && psv_have) {
        uint8_t mpsk[32];
        tls_resumption_psk(psv_res_master, psv_nonce, psv_nonce_len, mpsk);
        tls_early_secret(mpsk, 32, early);
    } else {
        tls_early_secret(NULL, 0, early);
    }
    tls_derive_secret(early, derived);
    tls_handshake_secret(derived, shared, g_hs_secret);

    // --- ServerHello ---
    uint8_t sh_body[4352];
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
    // PSK modes: ServerHello carries pre_shared_key (identity 0 = accept,
    // 1 = foreign selection the client must refuse). ACCEPT requires a
    // verified offer (independent binder recompute); a bad binder aborts
    // into the alert path below (client ERRs — the math proof by
    // contradiction).
    int psk_ok = 0;
    if (mode == MOCK_PSK_ACCEPT) psk_ok = mock_check_psk_offer();
    if (mode == MOCK_PSK_ACCEPT && psk_ok) {
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x29;   // pre_shared_key
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x02;
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x00;   // selected_identity 0
    } else if (mode == MOCK_PSK_FOREIGN) {
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x29;
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x02;
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x01;   // foreign identity
    }
    // SH_BIG: pad ServerHello past the client's 4KB SH cap with an unknown
    // extension (regression test for the SH OOB read, review #1: the old
    // code truncated the copy but kept the attacker's length).
    if (mode == MOCK_SH_BIG) {
        sh_body[sp++] = 0x12; sh_body[sp++] = 0x34;
        sh_body[sp++] = 0x0F; sh_body[sp++] = 0xA0;   // len 4000
        for (int i = 0; i < 4000; i++) sh_body[sp++] = 0;
    }
    // SH_DUP: duplicate supported_versions extension (inside the ext block:
    // ext_total covers it; the duplicate itself must ERR).
    if (mode == MOCK_SH_DUP) {
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x2b;
        sh_body[sp++] = 0x00; sh_body[sp++] = 0x02;
        sh_body[sp++] = 0x03; sh_body[sp++] = 0x04;
    }
    if (mode == MOCK_NO_KEYSHARE) sp = ext_len_pos + 2 + 6; // keep only the 6-byte supported_versions ext
    uint32_t ext_total = sp - ext_len_pos - 2;
    sh_body[ext_len_pos] = (uint8_t)(ext_total >> 8);
    sh_body[ext_len_pos + 1] = (uint8_t)(ext_total & 0xff);
    // SH_TRAIL: two bytes past the extension block (ext_total does NOT cover
    // them; the message length does) — exact-consumption violation, must ERR.
    if (mode == MOCK_SH_TRAIL) {
        sh_body[sp++] = 0xAA; sh_body[sp++] = 0xBB;
    }

    uint8_t sh_msg[4608];
    sh_msg[0] = TLS_HS_SERVER_HELLO;
    sh_msg[1] = 0; sh_msg[2] = (uint8_t)(sp >> 8); sh_msg[3] = (uint8_t)(sp & 0xff);
    memcpy(sh_msg + 4, sh_body, sp);
    uint32_t sh_msg_len = 4 + sp;
    // SH_FLIPPED (wire-only MITM tamper): the mock's transcript/keys use the
    // ORIGINAL SH, but the client RECEIVES a flipped server-random. Both
    // sides parse fine; the transcripts fork → flight keys diverge → the
    // encrypted flight fails MAC. Must ERR, never DONE.
    uint8_t sh_wire[4608]; // matches sh_msg (SH_BIG needs 4KB+)
    memcpy(sh_wire, sh_msg, sh_msg_len);
    if (mode == MOCK_SH_FLIPPED) sh_wire[4 + 2 + 3] ^= 0x01;

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
    uint32_t rl = 0;
    if (mode == MOCK_SH_SPLIT) {
        // cryptoholes #7: ServerHello fragmented across two records
        // (RFC 8446 §5.1 permits it) — must reassemble and complete.
        uint32_t cut = 30; // mid-body split
        rl = plain_record(rec, TLS_CT_HANDSHAKE, sh_wire, cut);
        q_put(rec, rl);
        rl = plain_record(rec, TLS_CT_HANDSHAKE, sh_wire + cut,
                          sh_msg_len - cut);
        q_put(rec, rl);
    } else {
        rl = plain_record(rec, TLS_CT_HANDSHAKE, sh_wire, sh_msg_len);
        q_put(rec, rl);
    }

    // PSK_ACCEPT with an UNVERIFIED offer: binder (or identity) doesn't
    // check out — the server aborts with a fatal alert instead of any
    // flight. The committed test expects the client to produce a binder
    // this mock accepts, so reaching here fails the run LOUDLY (the math
    // proof by contradiction: wrong binder math can never yield DONE).
    if (mode == MOCK_PSK_ACCEPT && !psk_ok) {
        q_reset();
        uint8_t al[2] = { 0x02, 0x28 };   // fatal handshake_failure
        rl = plain_record(rec, TLS_CT_ALERT, al, 2);
        q_put(rec, rl);
    } else if (mode == MOCK_GARBAGE_FIRST) {
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
               mode != MOCK_NO_KEYSHARE &&
               !(mode == MOCK_PSK_ACCEPT && !psk_ok)) {
        // ---- encrypted flight ----
        // PSK_ACCEPT abbreviates to EE + Finished (no Certificate/CV —
        // authentication rides the resumed ticket, checked by the Finished
        // MACs). g_abbrev records the path for the CHECK.
        int abbrev = (mode == MOCK_PSK_ACCEPT);
        // EE body per mode (default: empty extension list). EE_DUP repeats
        // an extension type; EE_ALPN_H2 selects h2 (unspeakable for us).
        uint8_t ee_body_raw[16]; uint32_t ee_body_len = 2;
        ee_body_raw[0] = 0; ee_body_raw[1] = 0;
        if (mode == MOCK_EE_DUP) {
            ee_body_raw[0] = 0; ee_body_raw[1] = 8;
            ee_body_raw[2] = 0xFF; ee_body_raw[3] = 0x00;
            ee_body_raw[4] = 0; ee_body_raw[5] = 0;
            ee_body_raw[6] = 0xFF; ee_body_raw[7] = 0x00;
            ee_body_raw[8] = 0; ee_body_raw[9] = 0;
            ee_body_len = 10;
        } else if (mode == MOCK_EE_ALPN_H2) {
            ee_body_raw[0] = 0; ee_body_raw[1] = 9;
            ee_body_raw[2] = 0x00; ee_body_raw[3] = 0x10;
            ee_body_raw[4] = 0x00; ee_body_raw[5] = 0x05;
            ee_body_raw[6] = 0x00; ee_body_raw[7] = 0x03;
            ee_body_raw[8] = 0x02; ee_body_raw[9] = 'h'; ee_body_raw[10] = '2';
            ee_body_len = 11;
        }
        uint8_t ee_msg[4 + 16];
        ee_msg[0] = TLS_HS_ENCRYPTED_EXTENSIONS;
        ee_msg[1] = 0; ee_msg[2] = (uint8_t)(ee_body_len >> 8);
        ee_msg[3] = (uint8_t)(ee_body_len & 0xff);
        memcpy(ee_msg + 4, ee_body_raw, ee_body_len);
        uint32_t ee_msg_len = 4 + ee_body_len;
        // EE_FLIPPED (wire-only MITM tamper, e.g. extension stripping): the
        // mock signs/computes over the ORIGINAL EE, but the client receives
        // flipped bytes. Transcripts fork at EE → CertificateVerify (signed
        // over mock-TH) fails client verification. Must ERR, never DONE.
        static uint8_t ee_wire[4 + 16];
        memcpy(ee_wire, ee_msg, ee_msg_len);
        if (mode == MOCK_EE_FLIPPED) ee_wire[5] ^= 0x01;
        mtrans_msg(&g_mt, TLS_HS_ENCRYPTED_EXTENSIONS, ee_msg + 4, ee_body_len);

        const char* chain[3] = { 0, 0, 0 };
        static uint8_t cert_msg[16384];
        uint32_t cert_len = 0;
        uint32_t cv_msg_len = 0;
        static uint8_t cv_msg[1200];
        if (!abbrev) {
        const char* leaf_file = (mode == MOCK_EXPIRED)
            ? "tests/adversarial/at_leaf_expired.der"
            : (mode == MOCK_RSA_PSS_VALID || mode == MOCK_CV_PKCS1)
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
        cert_len = build_cert_msg(cert_msg, sizeof(cert_msg), chain, 3);
        if (cert_len == 0) { res->r = -3; return; }
        // STAPLE modes: graft a status_request extension onto entry 0.
        // STAPLE/STAPLE_REVOKED/STAPLE_STALE carry REAL openssl-minted
        // responses (good/revoked); STAPLE_BAD carries framing garbage.
        // The mock transcript follows the wire bytes either way.
        if (mode == MOCK_STAPLE || mode == MOCK_STAPLE_BAD ||
            mode == MOCK_STAPLE_REVOKED || mode == MOCK_STAPLE_STALE) {
            if (mode == MOCK_STAPLE_BAD) {
                cert_len = staple_patch(cert_msg, sizeof(cert_msg), cert_len,
                                        NULL, 0, 1);
            } else {
                static uint8_t ocsp_resp[2048];
                uint32_t ocsp_len = 0;
                if (mint_ocsp(mode == MOCK_STAPLE_REVOKED,
                              ocsp_resp, sizeof(ocsp_resp),
                              &ocsp_len) != 0) {
                    printf("    [mock] ocsp mint failed (%s)\n",
                           mode_name(mode));
                    res->r = -3;
                    return;
                }
                cert_len = staple_patch(cert_msg, sizeof(cert_msg), cert_len,
                                        ocsp_resp, ocsp_len, 0);
            }
            if (cert_len == 0) { res->r = -3; return; }
        }
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

        int is_rsa = (mode == MOCK_RSA_PSS_VALID || mode == MOCK_CV_PKCS1);
        int is_p384 = (mode == MOCK_P384_VALID);
        const char* leaf_key = is_rsa ? "tests/adversarial/at_rsa_leaf.key"
                             : is_p384 ? "tests/adversarial/at_p384_leaf.key"
                                       : "tests/adversarial/at_leaf.key";
        const char* sign_key = (mode == MOCK_CV_WRONG_KEY)
            ? "tests/adversarial/at_root.key" : leaf_key;
        // CV_PKCS1: RSA PKCS#1 v1.5 signature (non-PSS openssl path) under
        // cv_alg 0x0401 — RFC 8446 §4.4.3 forbids it; the client must reject
        // even though the signature itself is cryptographically valid.
        int use_pkcs1 = (mode == MOCK_CV_PKCS1);
        uint16_t cv_alg = is_rsa ? (use_pkcs1 ? 0x0401 : 0x0804)
                          : is_p384 ? 0x0503 : 0x0403;

        uint8_t cv_sig[1024];
        uint32_t cv_sig_len = 0;
        if (sign_digest(sign_key, signed_data, sizeof(signed_data),
                        is_rsa && !use_pkcs1, is_p384,
                        cv_sig, &cv_sig_len) != 0) {
            printf("    [mock] openssl signing failed (%s)\n", mode_name(mode));
            res->r = -3;
            return;
        }
        if (mode == MOCK_CV_FLIPPED && cv_sig_len > 0)
            cv_sig[cv_sig_len - 1] ^= 0x01;

        cv_msg[0] = TLS_HS_CERTIFICATE_VERIFY;
        cv_msg[1] = 0;
        cv_msg[2] = (uint8_t)((4 + cv_sig_len) >> 8);
        cv_msg[3] = (uint8_t)((4 + cv_sig_len) & 0xff);
        cv_msg[4] = (uint8_t)(cv_alg >> 8);
        cv_msg[5] = (uint8_t)(cv_alg & 0xff);
        cv_msg[6] = (uint8_t)(cv_sig_len >> 8);
        cv_msg[7] = (uint8_t)(cv_sig_len & 0xff);
        memcpy(cv_msg + 8, cv_sig, cv_sig_len);
        cv_msg_len = 8 + cv_sig_len;
        mtrans_msg(&g_mt, TLS_HS_CERTIFICATE_VERIFY, cv_msg + 4, cv_msg_len - 4);
        } // !abbrev (abbreviated flights carry no Certificate/CV at all)

        // server Finished over the transcript through CV — or through EE
        // alone when abbreviated (no cert/CV legs at all).
        uint8_t tx_cv[32], s_fin_key[32], fin[32];
        mtrans_hash(&g_mt, tx_cv);
        tls_finished_key(s_hs_traffic, s_fin_key);
        hmac_sha256(s_fin_key, 32, tx_cv, 32, fin);
        if (mode == MOCK_FIN_FLIPPED) fin[5] ^= 0x10;

        // assemble the flight
        static uint8_t flight[16384];
        uint32_t fl = 0;
        memcpy(flight + fl, ee_wire, ee_msg_len); fl += ee_msg_len;
        if (!abbrev) {
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
        } else {
            g_abbrev = 1; // abbreviated: EE + Finished only, no cert/CV
        }
        uint8_t fin_msg[36] = { TLS_HS_FINISHED, 0, 0, 32, 0 };
        memcpy(fin_msg + 4, fin, 32);
        memcpy(g_sfin_msg, fin_msg, 36);
        g_sfin_len = 36;
        memcpy(flight + fl, fin_msg, 36); fl += 36;

        if (mode == MOCK_DUP_EE) {
            memmove(flight + ee_msg_len, flight, fl);
            memcpy(flight, ee_wire, ee_msg_len);
            fl += ee_msg_len;
        }

        // (NOTE: encode exactly what is queued — a throwaway encode here
        // would burn a sequence number and desync the client's nonces.)
        if (mode == MOCK_FRAGMENT && fl > 100) {
            uint32_t cut = fl / 2;
            uint32_t r1 = enc_record(rec, sizeof(rec), s_hs_key, s_hs_iv,
                                     &s_seq, TLS_CT_HANDSHAKE, flight, cut);
            uint32_t r2 = enc_record(rec + r1, sizeof(rec) - r1,
                                     s_hs_key, s_hs_iv,
                                     &s_seq, TLS_CT_HANDSHAKE,
                                     flight + cut, fl - cut);
            if (r1 == 0 || r2 == 0) { res->r = -3; return; }
            q_put(rec, r1 + r2); // [rec1][rec2] already contiguous
        } else {
            rl = enc_record(rec, sizeof(rec), s_hs_key, s_hs_iv, &s_seq,
                            TLS_CT_HANDSHAKE, flight, fl);
            if (rl == 0) { res->r = -3; return; }
            q_put(rec, rl);
        }

        g_expect_body = (mode == MOCK_VALID || mode == MOCK_RSA_PSS_VALID ||
                          mode == MOCK_P384_VALID || mode == MOCK_SPLIT ||
                          mode == MOCK_NST || mode == MOCK_NST_BAD ||
                          mode == MOCK_NST_BIGNONCE ||
                          mode == MOCK_PSK_ACCEPT || mode == MOCK_PSK_FALLBACK ||
                          mode == MOCK_STAPLE || mode == MOCK_FRAGMENT ||
                          mode == MOCK_SH_SPLIT || mode == MOCK_APP_TRUNCATED ||
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
    // Staple visibility for the CHECK (got_staple lives in the run's state).
    g_staple_seen = st.got_staple;
}

static void mock_section(void) {
    printf("== 2. adversarial mock TLS server ==\n");

    struct mock_result res;
    {
        x509_time n, nx;
        adv_clocks(&n, &nx);
        x509_set_now(&n);
    }

    mock_run(MOCK_VALID, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "positive control: valid ECDSA flight completes");
    // Pin isolation: every positive control below presents a DIFFERENT leaf
    // key for the same mock hostname — without a clear the TOFU pin (set
    // by the previous mode) would correctly refuse them. Real servers have
    // distinct names; the mock reuses one.
    tls_pin_clear();
    mock_run(MOCK_RSA_PSS_VALID, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0,
          "positive control: RSA leaf + PSS CertificateVerify completes");
    tls_pin_clear();
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
    tls_pin_clear(); // P-384 pin above would refuse these ECDSA leaves
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
    tls_pin_clear(); // P-384 pin above would refuse this ECDSA leaf
    mock_run(MOCK_SPLIT, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "split delivery (7B/recv) still completes");
    mock_run(MOCK_TRUNCATED, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "truncated flight rejected (no hang, no partial DONE)");
    mock_run(MOCK_APP_TRUNCATED, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_PROTO,
          "app-phase mid-record EOF rejected as truncation (cryptoholes #1)");
    mock_run(MOCK_SH_FLIPPED, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "tampered ServerHello rejected (transcript fork)");
    mock_run(MOCK_EE_FLIPPED, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "tampered EncryptedExtensions rejected (transcript fork)");
    tls_ticket_clear();
    mock_run(MOCK_NST, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "NewSessionTicket consumed, body still delivered");
    CHECK(tls_ticket_have("evil.example.com", 1000000),
          "ticket cached for the host after NST");
    CHECK(!tls_ticket_have("evil.example.com", 0),
          "unknown clock (0) is not fresh (cryptoholes #7)");
    CHECK(!tls_ticket_have("other.example.com", 1000000),
          "no ticket cached for other hosts");
    mock_run(MOCK_NST_BAD, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "corrupt ticket rejected (no silent keep)");
    mock_run(MOCK_CV_PKCS1, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "RSA PKCS#1 CertificateVerify rejected (PSS-only per RFC)");
    // Resumption: NST run above cached evil.example.com's ticket in the
    // client store — the PSK modes below offer it back.
    mock_run(MOCK_PSK_ACCEPT, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0 && g_abbrev,
          "PSK accept: binder verifies, abbreviated flight completes");
    mock_run(MOCK_PSK_FALLBACK, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0 && !g_abbrev,
          "PSK fallback: ignored offer still completes full handshake");
    mock_run(MOCK_PSK_FOREIGN, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "foreign PSK identity selection refused");
    // Big-nonce isolation AFTER the PSK trio (which needs NST's ticket):
    // clear, then prove an oversized nonce stores nothing yet still
    // completes the flight (tickets are optional).
    tls_ticket_clear();
    mock_run(MOCK_NST_BIGNONCE, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "oversize-nonce flight still completes");
    CHECK(!tls_ticket_have("evil.example.com", 1000000),
          "oversize nonce dropped, nothing stored");
    mock_run(MOCK_STAPLE, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0 && g_staple_seen,
          "stapled status_request noted, flight completes");
    mock_run(MOCK_STAPLE_BAD, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "malformed staple rejected");
    mock_run(MOCK_STAPLE_REVOKED, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_CERT,
          "revoked staple fails closed (no fallback class)");
    mock_run(MOCK_STAPLE_STALE, &res);
    CHECK(res.r == TLS_STEP_ERR && res.fail_reason == TLS_FAIL_CERT,
          "stale staple fails closed");
    mock_run(MOCK_SH_BIG, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "oversize ServerHello rejected (OOB regression)");
    mock_run(MOCK_SH_TRAIL, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "ServerHello trailing bytes rejected");
    mock_run(MOCK_SH_DUP, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "ServerHello duplicate extension rejected");
    mock_run(MOCK_EE_DUP, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "EncryptedExtensions duplicate rejected");
    mock_run(MOCK_EE_ALPN_H2, &res);
    CHECK(res.r == TLS_STEP_ERR,
          "EncryptedExtensions selecting h2 rejected");
    mock_run(MOCK_FRAGMENT, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "fragmented flight reassembled");
    mock_run(MOCK_SH_SPLIT, &res);
    CHECK(res.r == TLS_STEP_DONE && res.out_len > 0 &&
          memcmp(res.out, "HTTP/1.1 200 OK", 15) == 0,
          "fragmented ServerHello reassembled (cryptoholes #7)");
}

static void truncation_section(void) {
    printf("== 2b. truncation completeness (cryptoholes #1) ==\n");
    // Complete Content-Length body.
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_COMPLETE, "complete C-L body accepted");
    }
    // Short body vs declared length.
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nhello";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_SHORT, "short C-L body is truncation");
    }
    // Headers cut mid-flight.
    {
        static const char r[] = "HTTP/1.1 200 OK\r\nContent-Len";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_SHORT, "cut headers are truncation");
    }
    // Empty input.
    {
        CHECK(tls_response_complete((const uint8_t*)"", 0) ==
              TLS_RESP_SHORT, "empty response is truncation");
    }
    // No length signal: close-delimited, unknowable either way.
    {
        static const char r[] = "HTTP/1.1 200 OK\r\n\r\nhello";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_UNKNOWN, "lengthless body is UNKNOWN");
    }
    // Chunked with terminator.
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5\r\nhello\r\n0\r\n\r\n";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_COMPLETE, "terminated chunked body accepted");
    }
    // Chunked cut before the terminal chunk.
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5\r\nhello\r\n";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_SHORT, "unterminated chunked body is truncation");
    }
    // Case-insensitive header names.
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello";
        CHECK(tls_response_complete((const uint8_t*)r, sizeof(r) - 1) ==
              TLS_RESP_COMPLETE, "header names case-insensitive");
    }
}

static void keyuse_section(void) {
    printf("== 3. key usage / EKU / criticality negatives ==\n");
    // Anchor the adversarial test root (host-only hook) so the ONLY
    // possible failure below is the targeted usage check — never trust.
    static uint8_t rder[4096];
    int rn = load("tests/adversarial/at_root.der", rder, sizeof(rder));
    CHECK(rn > 0, "ku/eku anchor loads");
    if (rn <= 0) return;
    {
        x509_cert rc;
        if (x509_parse(rder, (uint32_t)rn, &rc) != 0) {
            CHECK(0, "ku/eku anchor parses");
            return;
        }
        cert_verify_trust_extra(rc.spki.p, rc.spki.len);
    }
    {
        x509_time n, nx;
        adv_clocks(&n, &nx);
        x509_set_now(&n);
    }
    static uint8_t flight[16384];
    // keyUsage without digitalSignature (keyEncipherment only)
    {
        const char* chain[3] = { "tests/adversarial/at_ku_leaf.der",
                                 "tests/adversarial/at_int.der",
                                 "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), chain, 3);
        CHECK(fl > 0, "ku flight builds");
        int r = cert_verify(flight, fl, "evil.example.com");
        CHECK(r == CV_ERR_KEYUSE, "leaf without digitalSignature rejected");
    }
    // EKU without serverAuth (clientAuth only)
    {
        const char* chain[3] = { "tests/adversarial/at_eku_leaf.der",
                                 "tests/adversarial/at_int.der",
                                 "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), chain, 3);
        CHECK(fl > 0, "eku flight builds");
        int r = cert_verify(flight, fl, "evil.example.com");
        CHECK(r == CV_ERR_KEYUSE, "leaf EKU without serverAuth rejected");
    }
    // Unknown CRITICAL extension: parse itself must fail.
    {
        static uint8_t cder[4096];
        int cn = load("tests/adversarial/at_crit_leaf.der", cder, sizeof(cder));
        CHECK(cn > 0, "critical-ext leaf loads");
        if (cn > 0) {
            x509_cert cc;
            CHECK(x509_parse(cder, (uint32_t)cn, &cc) != 0,
                  "unknown critical extension rejected at parse");
        }
    }
    // Control: the good leaf still verifies (usage checks don't false-fire
    // on real chains — the at_* PKI carries KU digitalSignature throughout).
    {
        const char* chain[3] = { "tests/adversarial/at_leaf.der",
                                 "tests/adversarial/at_int.der",
                                 "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), chain, 3);
        CHECK(fl > 0 && cert_verify(flight, fl, "evil.example.com") == CV_OK,
              "usage-enforcing control still verifies");
    }
    cert_verify_trust_extra(NULL, 0);
}

static void pin_section(void) {
    printf("== 4. TOFU pin store ==\n");
    static uint8_t hA[32], hB[32];
    for (int i = 0; i < 32; i++) { hA[i] = (uint8_t)(0x10 + i); hB[i] = (uint8_t)(0x80 + i); }
    tls_pin_clear();
    CHECK(tls_pin_check("a.example.com", hA) == 0, "first visit pins");
    CHECK(tls_pin_check("a.example.com", hA) == 0, "same key matches");
    CHECK(tls_pin_check("a.example.com", hB) != 0, "changed key refuses");
    CHECK(tls_pin_check("a.example.com", hA) == 0, "old pin kept (matches again)");
    CHECK(tls_pin_check("b.example.com", hB) == 0, "pins are per-host");
    CHECK(tls_pin_check(NULL, hA) != 0, "null host refuses");
    tls_pin_clear();
    CHECK(tls_pin_check("a.example.com", hB) == 0, "clear re-pins");
    // Persistence round-trip (P2): export, wipe, import, match. Malformed
    // blobs rejected without touching the live store.
    {
        static uint8_t blob[1024];
        uint32_t bl = tls_pin_export(blob, sizeof(blob));
        CHECK(bl > 8, "export writes a header + entry");
        tls_pin_clear();
        CHECK(tls_pin_check("a.example.com", hB) == 0, "wiped store re-pins (control)");
        tls_pin_clear();
        CHECK(tls_pin_import(blob, bl) == 0, "import accepts");
        CHECK(tls_pin_check("a.example.com", hB) == 0, "imported pin matches");
        CHECK(tls_pin_check("a.example.com", hA) != 0, "imported pin refuses change");
        CHECK(tls_pin_import(blob, 3) != 0, "truncated blob rejected");
        blob[0] = 'X';
        CHECK(tls_pin_import(blob, bl) != 0, "bad magic rejected");
        CHECK(tls_pin_check("a.example.com", hB) == 0, "failed imports keep store");
    }
    tls_pin_clear();
}

static void name_section(void) {
    printf("== 5. AKI binding / IP SAN / hostname discipline ==\n");
    static uint8_t rder[4096];
    int rn = load("tests/adversarial/at_root.der", rder, sizeof(rder));
    CHECK(rn > 0, "name anchor loads");
    if (rn <= 0) return;
    {
        x509_cert rc;
        if (x509_parse(rder, (uint32_t)rn, &rc) != 0) {
            CHECK(0, "name anchor parses");
            return;
        }
        cert_verify_trust_extra(rc.spki.p, rc.spki.len);
    }
    {
        x509_time n, nx;
        adv_clocks(&n, &nx);
        x509_set_now(&n);
    }
    static uint8_t flight[16384];
    // AKI binding: same issuer NAME, right vs wrong issuer KEY.
    {
        const char* okc[3] = { "tests/adversarial/at_aki_ok.der",
                               "tests/adversarial/at_int.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), okc, 3);
        CHECK(fl > 0 && cert_verify(flight, fl, "evil.example.com") == CV_OK,
              "matching AKI/SKI verifies");
    }
    {
        const char* badc[3] = { "tests/adversarial/at_aki_bad.der",
                                "tests/adversarial/at_int.der",
                                "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), badc, 3);
        CHECK(fl > 0 && cert_verify(flight, fl, "evil.example.com") == CV_ERR_CHAIN,
              "mismatched AKI rejected (key, not name)");
    }
    // IP SANs: exact v4 match only; DNS never matches IP hosts and vice
    // versa; leading-zero octet rejected (no octal ambiguity).
    {
        const char* ipc[3] = { "tests/adversarial/at_ip_leaf.der",
                               "tests/adversarial/at_int.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), ipc, 3);
        CHECK(fl > 0, "ip flight builds");
        if (fl > 0) {
            CHECK(cert_verify(flight, fl, "1.2.3.4") == CV_OK,
                  "IP SAN exact match verifies");
            CHECK(cert_verify(flight, fl, "1.2.3.5") == CV_ERR_HOSTNAME,
                  "IP mismatch rejected");
            CHECK(cert_verify(flight, fl, "evil.example.com") == CV_ERR_HOSTNAME,
                  "DNS name vs IP-only leaf rejected");
            CHECK(cert_verify(flight, fl, "01.2.3.4") == CV_ERR_HOSTNAME,
                  "leading-zero IP rejected");
        }
    }
    // dNSName holding an IP string must NOT match an IP-literal host.
    {
        const char* dnc[3] = { "tests/adversarial/at_dnsip_leaf.der",
                               "tests/adversarial/at_int.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), dnc, 3);
        CHECK(fl > 0, "dns-ip flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "1.2.3.4") == CV_ERR_HOSTNAME,
                  "dNSName-IP never matches IP host");
    }
    // NameConstraints: permitted DNS subtree (mint_nc.py chains hang off
    // at_root — at_int's pathlen:0 forbids sub-CAs, which would fail the
    // wrong check).
    {
        const char* okc[4] = { "tests/adversarial/at_nc_ok.der",
                               "tests/adversarial/at_nc_int.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), okc, 3);
        CHECK(fl > 0, "nc-ok flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "www.example.com") == CV_OK,
                  "in-namespace leaf under constrained CA verifies");
    }
    {
        const char* badc[4] = { "tests/adversarial/at_nc_bad.der",
                                "tests/adversarial/at_nc_int.der",
                                "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), badc, 3);
        CHECK(fl > 0, "nc-bad flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "evil.example.io") == CV_ERR_CAFLAGS,
                  "out-of-namespace leaf rejected by permitted subtree");
    }
    {
        const char* xlc[4] = { "tests/adversarial/at_nc_xlf.der",
                               "tests/adversarial/at_nc_xint.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), xlc, 3);
        CHECK(fl > 0, "nc-excluded flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "www.example.com") == CV_ERR_CAFLAGS,
                  "excluded-namespace leaf rejected");
    }
    // cryptoholes #2/#3/#4/#5 — fail-closed parser limits. Each chain is
    // otherwise fully valid (hostname matches, signatures good); the ONLY
    // defect is unenforceable/truncated policy, which must fail the parse
    // (CV_ERR_PARSE), never silently vanish into an accept.
    {
        const char* ovc[4] = { "tests/adversarial/at_nc_over_leaf.der",
                               "tests/adversarial/at_nc_over.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), ovc, 3);
        CHECK(fl > 0, "nc-overflow flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "www.a.example.com") == CV_ERR_PARSE,
                  "5-constraint NC (cap 4) fails closed, not truncated");
    }
    {
        const char* dnc[4] = { "tests/adversarial/at_nc_dir_leaf.der",
                               "tests/adversarial/at_nc_dir.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), dnc, 3);
        CHECK(fl > 0, "nc-dirname flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "www.example.com") == CV_ERR_PARSE,
                  "directoryName NC fails closed (unenforceable here)");
    }
    {
        const char* v6c[4] = { "tests/adversarial/at_nc_ip6_leaf.der",
                               "tests/adversarial/at_nc_ip6.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), v6c, 3);
        CHECK(fl > 0, "nc-ipv6 flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "www.example.com") == CV_ERR_PARSE,
                  "IPv6 NC fails closed (unenforceable here)");
    }
    {
        const char* soc[4] = { "tests/adversarial/at_san_over.der",
                               "tests/adversarial/at_int.der",
                               "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), soc, 3);
        CHECK(fl > 0, "san-overflow flight builds");
        if (fl > 0)
            CHECK(cert_verify(flight, fl, "h0.example.com") == CV_ERR_PARSE,
                  "17-SAN leaf (cap 16) fails closed, not truncated");
    }
    // Hostname discipline: overlong + non-ASCII rejected, never truncated.
    {
        const char* chain[3] = { "tests/adversarial/at_leaf.der",
                                 "tests/adversarial/at_int.der",
                                 "tests/adversarial/at_root.der" };
        uint32_t fl = build_cert_msg(flight, sizeof(flight), chain, 3);
        CHECK(fl > 0, "discipline flight builds");
        if (fl > 0) {
            static char longhost[300];
            for (int i = 0; i < 254; i++) longhost[i] = 'a';
            longhost[254] = 0;
            CHECK(cert_verify(flight, fl, longhost) == CV_ERR_HOSTNAME,
                  "254-char hostname rejected");
            CHECK(cert_verify(flight, fl, "evil\xff.example.com") == CV_ERR_HOSTNAME,
                  "non-ASCII hostname rejected");
        }
    }
    cert_verify_trust_extra(NULL, 0);
}

int main(void) {
    parser_fuzz_section();
    fuzz_section();
    mock_section();
    truncation_section();
    keyuse_section();
    pin_section();
    name_section();
    printf("\n%s: %d passed, %d failed\n",
           failures == 0 ? "ADVERSARIAL TESTS PASS" : "ADVERSARIAL TESTS FAIL",
           passes, failures);
    return failures == 0 ? 0 : 1;
}
