#include "tls_client.h"
#include "tls_record.h"
#include "tls_handshake.h"
#include "tls_keysched.h"
#include "x25519.h"
#include "aead.h"
#include "hmac.h"
#include "certverify.h"
#include "ocsp.h"
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
// Host-test RNG (review 2026-09-10 #5/#8): /dev/urandom ONLY — the old
// time/pid-xorshift fallback was predictable, and silently downgrading
// cryptographic randomness is never acceptable. If urandom is unavailable
// the handshake aborts (tls_fill_random returns 0 → TLS_FAIL_RNG), exactly
// like the kernel's not-ready CPRNG. Host binaries stay test-only
// (production = kernel CPRNG); this just refuses to run them broken.
#include <stdio.h>
#include <time.h>
static int host_rng(uint8_t* out, uint32_t n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return 0;
    size_t got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
}
#endif

// Fills `out` with n random bytes. Returns 0 on failure — callers MUST abort
// the handshake in that case. Never synthesize deterministic "random": these
// bytes become the ECDHE private key and the ClientHello nonce.
static int tls_fill_random(uint8_t* out, uint32_t n) {
#ifdef KERNEL
    return rand_bytes(out, n) == 1;
#else
    return host_rng(out, n);
#endif
}

int shared_is_zero(const uint8_t s[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= s[i];
    return acc == 0;
}

// ---- Session-ticket cache (RFC 8446 §4.6.1) ----
// Process-global static slots (freestanding-safe, no allocation). One slot
// per host; a new ticket for a known host replaces the old (latest wins —
// servers commonly send 2; either resumes). Tickets authenticate NOTHING:
// the first handshake always fully verifies the chain, and the ticket only
// re-proves that same server later (bound to the resumption master secret
// derived under the verified handshake).
#define TLS_TICKET_SLOTS 4
#define TLS_TICKET_MAX 1024
#define TLS_TICKET_NONCE_MAX 64  // RFC allows 255; real nonces are ~8-32B
struct tls_ticket_slot {
    int used;
    char host[64];
    uint8_t ticket[TLS_TICKET_MAX]; uint32_t ticket_len;
    uint8_t nonce[TLS_TICKET_NONCE_MAX]; uint32_t nonce_len;
    uint8_t res_master[32];
    uint32_t lifetime;   // seconds (0 = treat as expired)
    uint32_t age_add;
    uint64_t received_ms;
};
static struct tls_ticket_slot ticket_slots[TLS_TICKET_SLOTS];

static void ticket_host_copy(char dst[64], const char* src) {
    uint32_t i = 0;
    while (src[i] && i < 63) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int ticket_host_eq(const char a[64], const char* b) {
    uint32_t i = 0;
    for (;;) {
        char ca = i < 64 ? a[i] : 0, cb = b[i];
        if (ca != cb) return 0;
        if (!ca) return 1;
        i++;
        if (i > 70) return 0;
    }
}

// Expiry check against `now_ms` (0 = clock unknown → never expire on age;
// a zero lifetime still rejects — the server said "don't resume").
// NOTE: freestanding i386 has no 64-bit divide (no libgcc) — compare in
// milliseconds with a multiply, never divide.
static int ticket_fresh(const struct tls_ticket_slot* s, uint64_t now_ms) {
    if (!s->used || s->ticket_len == 0 || s->lifetime == 0) return 0;
    if (now_ms != 0 && now_ms >= s->received_ms) {
        if (now_ms - s->received_ms > (uint64_t)s->lifetime * 1000u) return 0;
    }
    return 1;
}

int tls_ticket_have(const char* host) {
    if (!host) return 0;
    for (int i = 0; i < TLS_TICKET_SLOTS; i++)
        if (ticket_host_eq(ticket_slots[i].host, host) &&
            ticket_fresh(&ticket_slots[i], 0))
            return 1;
    return 0;
}

void tls_ticket_clear(void) {
    for (uint32_t i = 0; i < sizeof(ticket_slots); i++)
        ((uint8_t*)ticket_slots)[i] = 0;
}

// ---- Trust-On-First-Use leaf pinning (see tls_client.h) ----
#define TLS_PIN_SLOTS 8
struct tls_pin_slot {
    int used;
    char host[64];
    uint8_t spki_hash[32];
    uint32_t seq; // insertion order: true oldest-first eviction (review #35)
};
static struct tls_pin_slot pin_slots[TLS_PIN_SLOTS];
static uint32_t pin_seq;

void tls_pin_clear(void) {
    for (uint32_t i = 0; i < sizeof(pin_slots); i++)
        ((uint8_t*)pin_slots)[i] = 0;
}

// ---- Pin persistence (P2: TOFU pins survive reboot via VFS) ----
// Wire format: "OKPIN1"(6) || version u8 (1) || count u8 (<=8) ||
// entries of host[64] + spki_hash[32]. The desktop serializes on change
// and loads at boot (see desktop.c); the threat model is network-only
// attackers (disk-write attackers already own VFS + kernel — persistence
// buys nothing against them, and claims nothing more).
static int pin_dirty_flag = 0;

int tls_pin_dirty(void) { return pin_dirty_flag; }
void tls_pin_clean(void) { pin_dirty_flag = 0; }

uint32_t tls_pin_export(uint8_t* out, uint32_t cap) {
    uint32_t n = 0;
    for (int i = 0; i < TLS_PIN_SLOTS; i++)
        if (pin_slots[i].used) n++;
    if (cap < 8 + n * (64 + 32)) return 0;
    out[0] = 'O'; out[1] = 'K'; out[2] = 'P'; out[3] = 'I'; out[4] = 'N';
    out[5] = '1'; out[6] = 1; out[7] = (uint8_t)n;
    uint32_t p = 8;
    for (int i = 0; i < TLS_PIN_SLOTS; i++) {
        if (!pin_slots[i].used) continue;
        for (int j = 0; j < 64; j++) out[p++] = (uint8_t)pin_slots[i].host[j];
        for (int j = 0; j < 32; j++) out[p++] = pin_slots[i].spki_hash[j];
    }
    return p;
}

int tls_pin_import(const uint8_t* in, uint32_t len) {
    if (!in || len < 8) return -1;
    if (in[0] != 'O' || in[1] != 'K' || in[2] != 'P' || in[3] != 'I' ||
        in[4] != 'N' || in[5] != '1' || in[6] != 1)
        return -1;
    uint32_t n = in[7];
    if (n > TLS_PIN_SLOTS || 8 + n * (64 + 32) != len) return -1;
    tls_pin_clear();
    uint32_t p = 8;
    for (uint32_t i = 0; i < n; i++) {
        int valid = 0;
        for (int j = 0; j < 64; j++) {
            pin_slots[i].host[j] = (char)in[p++];
            if (pin_slots[i].host[j]) valid = 1;
        }
        if (!valid) { tls_pin_clear(); return -1; } // empty hostname
        for (int j = 0; j < 32; j++)
            pin_slots[i].spki_hash[j] = in[p++];
        pin_slots[i].seq = ++pin_seq;
        pin_slots[i].used = 1;
    }
    pin_dirty_flag = 0;
    return 0;
}

int tls_pin_check(const char* host, const uint8_t spki_hash[32]) {
    if (!host || !host[0] || !spki_hash) return -1;
    for (int i = 0; i < TLS_PIN_SLOTS; i++) {
        if (!pin_slots[i].used) continue;
        if (!ticket_host_eq(pin_slots[i].host, host)) continue;
        uint8_t diff = 0;
        for (int j = 0; j < 32; j++) diff |= pin_slots[i].spki_hash[j] ^ spki_hash[j];
        if (diff == 0) return 0; // match: same key as first visit
        tls_dbg("[tls] PIN CHANGED for %s (possible MITM or rotation)\n", host);
        return -1; // changed: old pin kept (warns every visit till reboot)
    }
    // First verified visit: store. Past capacity, evict the OLDEST pin
    // (lowest insertion seq — review #35: the old code always overwrote
    // slot 0 despite claiming oldest-first). 8 hosts cover a session.
    int slot = -1;
    for (int i = 0; i < TLS_PIN_SLOTS; i++)
        if (!pin_slots[i].used) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < TLS_PIN_SLOTS; i++)
            if (pin_slots[i].seq < pin_slots[slot].seq) slot = i;
    }
    ticket_host_copy(pin_slots[slot].host, host);
    memcpy(pin_slots[slot].spki_hash, spki_hash, 32);
    pin_slots[slot].seq = ++pin_seq;
    pin_slots[slot].used = 1;
    pin_dirty_flag = 1; // desktop persists on change
    return 0;
}

// Store (or replace) `host`'s ticket. Caps lengths; oversized tickets are
// dropped (a server can always issue a smaller one next time — and a
// multi-KB "ticket" smells like a fingerprinting probe, not resumption).
static void ticket_store(const char* host,
                         const uint8_t* ticket, uint32_t ticket_len,
                         const uint8_t* nonce, uint32_t nonce_len,
                         const uint8_t res_master[32],
                         uint32_t lifetime, uint32_t age_add,
                         uint64_t now_ms) {
    if (!host || !host[0] || !ticket || ticket_len == 0 ||
        ticket_len > TLS_TICKET_MAX || !res_master || lifetime == 0)
        return;
    // Oversized nonces are REJECTED, never truncated (review #32): the
    // nonce feeds PSK derivation, and silently hashing a prefix would
    // derive a different PSK than the server — worse, it normalizes
    // shaking the client into degenerate inputs. Drop the ticket; the
    // connection still completes (tickets are optional).
    if (nonce_len > TLS_TICKET_NONCE_MAX) {
        tls_dbg("[tls] ticket nonce too long (%uB), dropped\n", nonce_len);
        return;
    }
    int slot = -1;
    for (int i = 0; i < TLS_TICKET_SLOTS; i++)
        if (ticket_slots[i].used &&
            ticket_host_eq(ticket_slots[i].host, host)) { slot = i; break; }
    if (slot < 0) {
        // Evict the oldest (received_ms 0 sorts first — empty slots win).
        slot = 0;
        for (int i = 1; i < TLS_TICKET_SLOTS; i++)
            if (ticket_slots[i].received_ms < ticket_slots[slot].received_ms)
                slot = i;
    }
    struct tls_ticket_slot* s = &ticket_slots[slot];
    ticket_host_copy(s->host, host);
    memcpy(s->ticket, ticket, ticket_len); s->ticket_len = ticket_len;
    s->nonce_len = nonce_len; // bounded above (<= NONCE_MAX) by the reject
    if (nonce && s->nonce_len) memcpy(s->nonce, nonce, s->nonce_len);
    memcpy(s->res_master, res_master, 32);
    s->lifetime = lifetime;
    s->age_add = age_add;
    s->received_ms = now_ms;
    s->used = 1;
    tls_dbg("[tls] ticket stored for %s (%uB, lifetime %us)\n",
            host, ticket_len, lifetime);
}

static void make_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq) {
    memcpy(nonce, iv, 12);
    // (The64-bit counter is BIG-ENDIAN, left-padded — see HANDOFF.)
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
    // Overflow discipline (review 2026-09-10 #2): NEVER add before
    // checking. `pt_len + 1 > sizeof` wraps at UINT32_MAX (→0, check
    // passes, 4GB memcpy). Bound pt_len itself; every sum below is then
    // safe by construction (pt_len ≤ 18432 → ct_len ≤ 18449).
    if (pt_len >= sizeof(inner)) return -1;
    memcpy(inner, pt, pt_len);
    inner[pt_len] = ct_type;

    uint8_t nonce[12];
    // Sequence exhaustion (review #8/#29): TLS 1.3 forbids nonce reuse —
    // terminate before 2^64 wraps (practically unreachable; encoded anyway).
    if (*seq == (uint64_t)0xFFFFFFFFFFFFFFFFull) return -1;
    make_nonce(nonce, iv, *seq); (*seq)++;

    uint8_t ct[18432 + 16 + 1];
    uint8_t tag[16];
    uint8_t aad[5];
    uint32_t ct_len = pt_len + 1 + 16;
    aad[0] = TLS_CT_APPDATA; aad[1] = 0x03; aad[2] = 0x03;
    aad[3] = (uint8_t)(ct_len >> 8); aad[4] = (uint8_t)(ct_len & 0xff);
    // AEAD refusal (oversize) aborts the connection — never emit untagged
    // records (review 2026-09-10 #1).
    if (aead_chacha20_poly1305_encrypt(key, nonce, aad, 5,
                                       inner, pt_len + 1, ct, tag) != 0)
        return -1;
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

// State-aware transcript: Certificate/CertificateVerify legs are OMITTED
// entirely (not hashed even as empty) when the server accepted PSK — they
// were never sent. with_sfin adds the server Finished; cfin (or NULL) adds
// the client Finished (resumption-master computation).
static void transcript_st(const struct tls_state* st, int with_sfin,
                          const uint8_t* cfin, uint8_t out[32]) {
    tls_transcript t;
    tls_transcript_init(&t);
    tls_transcript_update_msg(&t, TLS_HS_CLIENT_HELLO,
                              st->ch + 4, st->ch_len - 4);
    tls_transcript_update_msg(&t, TLS_HS_SERVER_HELLO,
                              st->sh_body, st->sh_bl);
    tls_transcript_update_msg(&t, TLS_HS_ENCRYPTED_EXTENSIONS,
                              st->ee_body, st->ee_bl);
    if (!st->psk_accepted) {
        tls_transcript_update_msg(&t, TLS_HS_CERTIFICATE,
                                  st->cert_body, st->cert_bl);
        tls_transcript_update_msg(&t, TLS_HS_CERTIFICATE_VERIFY,
                                  st->cv_body, st->cv_bl);
    }
    if (with_sfin)
        tls_transcript_update_msg(&t, TLS_HS_FINISHED,
                                  st->fin_body, st->fin_bl);
    if (cfin)
        tls_transcript_update_msg(&t, TLS_HS_FINISHED, cfin, 32);
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
    if (v.type == TLS_CT_CHANGE_CIPHER_SPEC) {
        // Middlebox-compat CCS is exactly one 0x01 byte (review #25): any
        // other shape is a protocol violation, not something to skip over.
        if (rec_pl != 1 || st->rec_buf[5] != 0x01) return -1;
        *ctype = TLS_CT_CHANGE_CIPHER_SPEC; return 0;
    }
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
    // Sequence consumes ONLY on successful decrypt (review 2026-09-10 #11):
    // bumping before verification desyncs the stream the day any retry
    // logic exists. (Today every failure aborts, so this is hygiene — but
    // cheap hygiene.)
    uint8_t nonce[12];
    // Same exhaustion guard as the send path (review #8/#29).
    if (*seq == (uint64_t)0xFFFFFFFFFFFFFFFFull) return -1;
    make_nonce(nonce, iv, *seq);
    uint8_t aad[5]; memcpy(aad, st->rec_buf, 5);
    uint8_t* enc = st->rec_buf + 5;
    if (aead_chacha20_poly1305_decrypt(key, nonce, aad, 5,
                                       enc, ct_len, enc + ct_len, pt) != 0)
        return -1;
    (*seq)++;
    int plen = ct_len;
    while (plen > 0 && pt[plen - 1] == 0) plen--;
    if (plen == 0) return -1;
    *ctype = pt[plen - 1];
    return plen - 1;
}

void tls_state_set_now_ms(struct tls_state* st, uint64_t now_ms) {
    if (st) st->now_ms = now_ms;
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
//
// STATE MACHINE (review #28 — explicit rejection table for a minimal
// client; anything not listed below is PROTO by construction):
//   SEND_CH -> RECV_SH: exactly one SH record, exact-consumed, no HRR
//     (magic random rejected), session-id echo + cipher enforced.
//   RECV_SH -> RECV_HS: flight EE, [CERT, CV,] Finished in order, each
//     once (PSK-accepted: EE, Finished). CertificateRequest (client-auth),
//     extra Certificates, or any other type → order violation.
//     Fragmented messages reassembled (16KB cap); trailing bytes rejected.
//   RECV_HS -> RECV_BODY: request sent; app keys live.
//   RECV_BODY: APPDATA appended (overflow-checked); NST consumed+stored;
//     KeyUpdate/unknown post-HS types rejected; plaintext alerts rejected;
//     encrypted close_notify ends cleanly; EOF ends (Connection: close).
//   No 0-RTT/early-data is ever sent. No client certificate exists.
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
        // PSK offer (RFC 8446 §4.2.11): a fresh cached ticket for this host
        // upgrades the CH to carry a pre_shared_key extension (built by the
        // PSK variant, byte-identical base + trailing ext). The binder is
        // computed over the truncated CH (through the binders-length field)
        // and patched in. Offering never weakens the handshake: the server
        // may ignore it (clean fallback to the full flight below) and the
        // ECDHE share is always present (forward secrecy either way).
        // Guards: ticket must be fresh AND small (CH cap is 1024B).
        st->offer_psk = 0; st->psk_accepted = 0;
        // Clock gate (review #31): no resumption offer without a
        // trustworthy clock. With unknown time we can neither enforce the
        // ticket lifetime nor report an honest age (age 0 for a months-old
        // ticket is a lie the server shouldn't have to catch). The kernel
        // always passes tick_count; host tls_client_run stamps wall time;
        // the mock stamps a fixed ms. now_ms == 0 (unwired caller) → no
        // offer, full handshake as if no ticket existed.
        if (st->now_ms != 0) {
            const struct tls_ticket_slot* tslot = NULL;
            for (int i = 0; i < TLS_TICKET_SLOTS; i++)
                if (ticket_host_eq(ticket_slots[i].host, st->host) &&
                    ticket_fresh(&ticket_slots[i], st->now_ms) &&
                    ticket_slots[i].ticket_len <= 512) { tslot = &ticket_slots[i]; break; }
            if (tslot) {
                uint8_t psk[32], early[32], bkey[32];
                tls_resumption_psk(tslot->res_master, tslot->nonce,
                                   tslot->nonce_len, psk);
                tls_early_secret(psk, 32, early);
                tls_psk_binder_key(early, bkey);
                uint32_t age_obf;
                if (st->now_ms != 0 && st->now_ms >= tslot->received_ms)
                    age_obf = (uint32_t)(((st->now_ms - tslot->received_ms) +
                                          tslot->age_add) & 0xFFFFFFFFu);
                else
                    age_obf = tslot->age_add;
                uint8_t zero_binder[32] = {0};
                uint32_t binder_off = 0;
                uint32_t psk_len = tls_build_client_hello_psk(
                    st->ch, sizeof(st->ch), st->random, st->session_id,
                    st->pub, st->host, tslot->ticket, tslot->ticket_len,
                    age_obf, zero_binder, &binder_off);
                if (psk_len != 0 && binder_off + 32 == psk_len) {
                    // Truncated transcript: type(1) || len(3, truncated) ||
                    // body[0..truncated), truncated right AFTER the u16
                    // binders-length field (binder entry prefix + values
                    // excluded). This is ClientHello1 (RFC §4.2.11.2) — the
                    // server reconstructs the identical prefix to check the
                    // binder. binder_off points at the binder VALUES (msg
                    // units, incl 4B HS header); the u16 field starts 3B
                    // earlier (u16 len + u8 entry len), and body units drop
                    // the 4B header: trunc_body = binder_off - 3 + 2 - 4.
                    // (An earlier revision used -4, then -7 — both wrong by
                    // up to 2B; caught because real servers *and* the mock
                    // disagreed with us. The mock's parse-driven trunc
                    // body_len-33 was right all along.)
                    uint32_t trunc_body = binder_off - 5;
                    tls_transcript bt;
                    tls_transcript_init(&bt);
                    {
                        uint8_t hdr[4];
                        hdr[0] = TLS_HS_CLIENT_HELLO;
                        hdr[1] = (uint8_t)((trunc_body >> 16) & 0xff);
                        hdr[2] = (uint8_t)((trunc_body >> 8) & 0xff);
                        hdr[3] = (uint8_t)(trunc_body & 0xff);
                        tls_transcript_update(&bt, hdr, 4);
                        tls_transcript_update(&bt, st->ch + 4, trunc_body);
                    }
                    uint8_t th[32];
                    tls_transcript_final(&bt, th);
                    uint8_t binder[32];
                    hmac_sha256(bkey, 32, th, 32, binder);
                    memcpy(st->ch + binder_off, binder, 32);
                    st->ch_len = psk_len;
                    memcpy(st->psk_early, early, 32);
                    st->offer_psk = 1;
                    tls_dbg("[tls] offering PSK for %s (ticket %uB)\n",
                            st->host, tslot->ticket_len);
                }
                // else: builder overflow (absurd) — fall through to the
                // plain CH already built above (offer_psk stays 0).
            }
        }
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
        // ServerHello fills its record exactly (review 2026-09-10 #11):
        // trailing bytes would be a coalesced second message we would
        // silently drop. Real servers always send SH alone (plaintext,
        // middlebox-compat shape).
        if (consumed != st->rec_pl) {
            tls_dbg("[tls] SH record has trailing bytes\n");
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }

        tls_server_hello sh;
        // Length cap BEFORE copy (review 2026-09-10 #1: a >4096B SH body
        // used to truncate the copy but keep the attacker's length — every
        // later user (transcript, parsers) then read OOB past sh_body).
        // Real ServerHellos are ~120B; 4KB is generous headroom, not a cap
        // any legitimate server will notice.
        if (hs_bl > sizeof(st->sh_body)) {
            tls_dbg("[tls] oversize ServerHello (%uB) rejected\n", hs_bl);
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        if (tls_parse_server_hello(st->rec_buf + 5 + 4, hs_bl,
                                   st->session_id, &sh) != 0) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }
        st->sh_bl = hs_bl;
        for (uint32_t i = 0; i < hs_bl; i++)
            st->sh_body[i] = st->rec_buf[5 + 4 + i];

        // HelloRetryRequest is explicitly unsupported (review #28): our CH
        // is fixed-shape (X25519-only, ChaCha-only), so a server asking us
        // to retry can never succeed — and silently misparsing the HRR
        // random as a real server random would poison every key below.
        // HRR magic (RFC 8446 §4.1.3): CF 21 AD 74 ...
        {
            static const uint8_t hrr_magic[16] = {
                0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,
                0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91 };
            uint8_t diff = 0;
            for (int i = 0; i < 16; i++) diff |= sh.random[i] ^ hrr_magic[i];
            if (diff == 0) {
                tls_dbg("[tls] HelloRetryRequest unsupported\n");
                st->fail_reason = TLS_FAIL_PROTO;
                return TLS_STEP_ERR;
            }
        }

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
        // PSK accept (RFC 8446 §4.2.11): the server echoes pre_shared_key
        // with selected_identity 0 iff it took our offer. Accepted → the
        // handshake runs on the PSK-mixed early secret and the flight skips
        // Certificate/CertificateVerify (auth rides the resumed ticket).
        // Ignored → ZERO early secret exactly as without an offer (the
        // server derived its keys that way; offering changes nothing else —
        // same CH transcript, same ECDHE share).
        st->psk_accepted = 0;
        if (st->offer_psk) {
            int ps = tls_parse_sh_psk(st->sh_body, st->sh_bl);
            if (ps == -2) {
                tls_dbg("[tls] server selected a foreign PSK identity\n");
                st->fail_reason = TLS_FAIL_PROTO;
                return TLS_STEP_ERR;
            }
            if (ps == 0) {
                st->psk_accepted = 1;
                tls_dbg("[tls] resumption accepted by server\n");
            }
        }
        uint8_t early_secret[32];
        if (st->psk_accepted)
            memcpy(early_secret, st->psk_early, 32);
        else
            tls_early_secret(NULL, 0, early_secret);
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
            // -2 = PLAINTEXT alert record (outer type ALERT, unencrypted).
            // Post-ServerHello every legitimate record is encrypted: a
            // plaintext alert here is unauthenticated bytes — either a
            // broken peer or an injection — so it is ALWAYS a protocol
            // error, never information (review 2026-09-10 #5).
            // -1 = AEAD decrypt/MAC failure (active tampering or wrong
            // keys, e.g. key-share-swapped flight).
            if (pl == -2) {
                st->fail_reason = TLS_FAIL_PROTO;
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

        // Handshake-message reassembly (review #9 — RFC 8446 §5.1 permits
        // fragmenting handshake messages across records; servers do it for
        // large flights, e.g. RSA-4096 chains). Decrypted HANDSHAKE payloads
        // accumulate in hs_buf; complete messages are parsed out below.
        // Anything past TLS_HS_REASSEMBLY_MAX is rejected, not truncated.
        if (st->hs_have + (uint32_t)pl > sizeof(st->hs_buf)) {
            tls_dbg("[tls] flight exceeds reassembly cap\n");
            st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
        }
        for (int i = 0; i < pl; i++) st->hs_buf[st->hs_have + i] = pt[i];
        st->hs_have += (uint32_t)pl;

        uint32_t p = 0;
        while (p + 4 <= st->hs_have) {
            uint8_t t; uint32_t bl;
            uint32_t c = parse_hs(st->hs_buf + p, st->hs_have - p, &t, &bl);
            if (c == 0) break; // partial message at the end: await more
            const uint8_t* body = st->hs_buf + p + 4;
            // RFC 8446 §4.4: the encrypted flight is EXACTLY EncryptedExtensions,
            // Certificate, CertificateVerify, Finished — in that order, each
            // once. Enforce it: an unexpected or repeated message is a protocol
            // violation, not something to tolerate.
            // PSK-accepted abbreviates to EE, Finished (RFC §4.2.11: the
            // server omits Certificate/CertificateVerify — authentication
            // rides the resumed ticket, verified by the Finished MACs).
            {
                static const uint8_t expect_types[4] = {
                    TLS_HS_ENCRYPTED_EXTENSIONS, TLS_HS_CERTIFICATE,
                    TLS_HS_CERTIFICATE_VERIFY, TLS_HS_FINISHED
                };
                uint8_t want;
                if (st->psk_accepted && st->hs_next == 1)
                    want = TLS_HS_FINISHED;
                else if (st->hs_next < 0 || st->hs_next > 3)
                    want = 255; // impossible: force the violation below
                else
                    want = expect_types[st->hs_next];
                if (t != want) {
                    tls_dbg("[tls] flight order violation: got type %u, expected %u\n",
                            t, want);
                    st->fail_reason = TLS_FAIL_PROTO;
                    return TLS_STEP_ERR;
                }
            }
            if (t == TLS_HS_ENCRYPTED_EXTENSIONS) {
                if (bl > sizeof(st->ee_body)) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
                memcpy(st->ee_body, body, bl); st->ee_bl = bl; st->got_ee = 1;
                // Validate what the server selected (review #26/#27): exact
                // framing, no duplicates, ALPN-if-present == "http/1.1".
                // An h2-selecting server would otherwise break the HTTP
                // layer above silently.
                if (tls_parse_ee_validate(st->ee_body, st->ee_bl) != 0) {
                    tls_dbg("[tls] EncryptedExtensions rejected\n");
                    st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
                }
                st->hs_next = 1;
            } else if (t == TLS_HS_CERTIFICATE) {
                if (bl > sizeof(st->cert_body)) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
                memcpy(st->cert_body, body, bl); st->cert_bl = bl;
                if (tls_parse_certificate(st->cert_body, st->cert_bl) != 0) {
                    tls_dbg("[tls] Certificate structural parse failed (len=%u)\n",
                            st->cert_bl);
                    st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
                }
                {
                    int staple = 0;
                    const uint8_t* sbytes = 0;
                    uint32_t sblen = 0;
                    if (tls_cert_has_staple(st->cert_body, st->cert_bl,
                                            &staple, &sbytes, &sblen) != 0) {
                        tls_dbg("[tls] Certificate entry framing failed\n");
                        st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
                    }
                    st->got_staple = staple;
                    st->staple_len = 0;
                    if (staple) {
                        // Oversized staples fail closed (same discipline as
                        // every bounded copy here — never truncate crypto
                        // inputs silently).
                        if (sblen == 0 || sblen > sizeof(st->staple)) {
                            tls_dbg("[tls] staple size %u rejected\n", sblen);
                            st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR;
                        }
                        for (uint32_t i = 0; i < sblen; i++)
                            st->staple[i] = sbytes[i];
                        st->staple_len = sblen;
                        tls_dbg("[tls] OCSP staple present (%uB, validating post-auth)\n",
                                sblen);
                    }
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
        // Compact: drop consumed messages, keep the partial tail (if any)
        // for the next record. Bytes are only ever consumed as complete,
        // order-valid messages — the tail is either empty or an incomplete
        // message prefix.
        if (p > 0) {
            for (uint32_t i = p; i < st->hs_have; i++)
                st->hs_buf[i - p] = st->hs_buf[i];
            st->hs_have -= p;
        }
        st->rec_have = 0;
        if (!st->got_sfin) return TLS_STEP_AGAIN;

        // A complete Finished with bytes still buffered means trailing
        // garbage rode in after it (the old code rejected this at parse;
        // the reassembly buffer must not swallow it silently).
        if (st->hs_have != 0) {
            tls_dbg("[tls] trailing bytes after Finished\n");
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }

        if (!st->got_ee || st->hs_next != 4 ||
            (!st->psk_accepted && (!st->got_cert || !st->got_cv))) {
            st->fail_reason = TLS_FAIL_PROTO;
            return TLS_STEP_ERR;
        }

        // ---- SERVER AUTHENTICATION ----
        // RFC 8446 §4.4.2-4.4.4. Without this the "s" in https is decorative:
        // any MITM can present its own certificate and complete the handshake.
        // Order: chain -> anchor -> hostname, then the CertificateVerify
        // signature (proof-of-possession of the leaf key), then Finished.
        // PSK-accepted skips this whole block: no Certificate/CV arrived,
        // and the Finished MACs (verified below under PSK-mixed keys) are
        // the authentication — only the original ticket holder, verified
        // during the full handshake that issued it, can compute them.
        if (!st->psk_accepted) {
#ifdef KERNEL
            const char* vhost = st->host;
#else
            const char* vhost = st->verify_host ? st->verify_host : st->host;
#endif
            int cvr = cert_verify(st->cert_body, st->cert_bl, vhost);
            if (cvr != CV_OK) {
                tls_dbg("[tls] cert verification failed: %s\n",
                        cert_verify_strerror(cvr));
                st->fail_reason = TLS_FAIL_CERT;
                st->cert_detail = cvr;
                return TLS_STEP_ERR;
            }

            // CertificateVerify: signs 64 sp || "TLS 1.3, server
            // CertificateVerify" || 0x00, hashed per the signature algorithm,
            // with the LEAF certificate's key. Transcript covers CH..Certificate.
            x509_cert leaf;
            if (cert_leaf(st->cert_body, st->cert_bl, &leaf) != 0) {
                tls_dbg("[tls] leaf certificate parse failed (len=%u)\n", st->cert_bl);
                st->fail_reason = TLS_FAIL_CERT;
                st->cert_detail = CV_ERR_PARSE;
                return TLS_STEP_ERR;
            }
            uint16_t cv_alg;
            const uint8_t* cv_sig;
            uint32_t cv_sig_len;
            if (tls_parse_cv_sig(st->cv_body, st->cv_bl, &cv_alg, &cv_sig, &cv_sig_len) != 0) {
                tls_dbg("[tls] CertificateVerify parse failed (len=%u)\n", st->cv_bl);
                st->fail_reason = TLS_FAIL_CERT;
                st->cert_detail = CV_ERR_PARSE;
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
            }
            // NOTE (review 2026-09-10 #4): NO 0x0401 (RSA PKCS#1) case — RFC
            // 8446 §4.4.3 mandates PSS for CertificateVerify. The parse gate
            // above already rejects it; reaching here with 0x0401 keeps
            // vr=-2 (invalid). PKCS#1 stays valid for CHAIN signatures (offer
            // + certverify path), just never for CV.
            if (vr != 0) {
                tls_dbg("[tls] CertificateVerify signature INVALID (alg=%04x)\n",
                        cv_alg);
                st->fail_reason = TLS_FAIL_CERT;
                st->cert_detail = CV_ERR_CHAIN;
                return TLS_STEP_ERR;
            }
            tls_dbg("[tls] certificate chain verified, host matched\n");
            // TOFU leaf pin (see tls_client.h): first verified visit stores
            // the leaf SPKI hash; a later DIFFERENT key fails closed here
            // (no fallback — same gate as every CERT failure). Pin against
            // the verified identity (host-only verify_host override in tests).
            {
#ifdef KERNEL
                const char* phost = st->host;
#else
                const char* phost = st->verify_host ? st->verify_host : st->host;
#endif
                uint8_t ph[32];
                sha256(leaf.spki.p, leaf.spki.len, ph);
                if (tls_pin_check(phost, ph) != 0) {
                    st->fail_reason = TLS_FAIL_CERT;
                    st->cert_detail = CV_ERR_PINCHANGED;
                    return TLS_STEP_ERR;
                }
            }
            // OCSP staple validation (P2): the chain verified above, so
            // `leaf` is authentic and the flight's second cert is the
            // direct issuer the responder must be. A present-but-invalid
            // staple fails closed (revoked/stale/forged — indistinguishable
            // from attack, and the warning page discloses enforcement).
            // Absent staple: soft-fail proceed (documented model).
            if (st->got_staple) {
                x509_cert issuer;
                int ocsp_rc;
                if (cert_issuer(st->cert_body, st->cert_bl, &issuer) != 0) {
                    tls_dbg("[tls] staple with unparsable issuer\n");
                    st->fail_reason = TLS_FAIL_CERT;
                    st->cert_detail = CV_ERR_OCSP;
                    return TLS_STEP_ERR;
                }
                ocsp_rc = ocsp_check_staple(st->staple, st->staple_len,
                                           &leaf, &issuer, x509_get_now());
                tls_dbg("[tls] OCSP staple verdict: %s\n",
                        ocsp_strerror(ocsp_rc));
                if (ocsp_rc != OCSP_OK) {
                    st->fail_reason = TLS_FAIL_CERT;
                    st->cert_detail = CV_ERR_OCSP;
                    return TLS_STEP_ERR;
                }
            }
        }

        uint8_t tx_pre_sfin[32];
        transcript_st(st, 0, NULL, tx_pre_sfin);
        if (tls_verify_finished(st->s_fin_key, tx_pre_sfin, st->fin_body) != 0) {
            // A bad server Finished is an authentication failure, not a
            // transport error — flag it so the caller can distinguish it.
            tls_dbg("[tls] server Finished INVALID\n");
            st->fail_reason = TLS_FAIL_MAC;
            return TLS_STEP_ERR;
        }

        uint8_t tx_through_sfin[32];
        transcript_st(st, 1, NULL, tx_through_sfin);

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

        // Resumption basis (RFC 8446 §7.1): resumption_master =
        // Expand-Label(master, "res master", Transcript-Hash(CH..client
        // Finished), 32). Stashed with the master secret so post-handshake
        // NewSessionTickets can be bound (ticket_store below). The client
        // Finished body is our_fin (fin_msg+4).
        {
            uint8_t tx_both[32];
            transcript_st(st, 1, our_fin, tx_both);
            memcpy(st->master, master, 32); st->have_master = 1;
            tls_traffic_secret(master, "res master", tx_both, st->res_master);
            st->have_res = 1;
        }

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
            // -2 = PLAINTEXT alert record (outer type ALERT, unencrypted).
            // Post-handshake everything legitimate arrives AEAD-sealed: a
            // plaintext alert — EVEN close_notify — is unauthenticated, so
            // an on-path attacker could inject 7 bytes and truncate any
            // response undetectably. It is ALWAYS a protocol error, never a
            // clean DONE (review 2026-09-10 #5 — this contradicted the
            // never-deliver-partial-plaintext rule below).
            // -1 = AEAD decrypt/MAC failure: ACTIVE attack or corruption on
            // the stream — never deliver partial plaintext as if clean.
            if (pl == -2) {
                st->fail_reason = TLS_FAIL_PROTO;
                return TLS_STEP_ERR;
            }
            st->fail_reason = TLS_FAIL_MAC;
            return TLS_STEP_ERR;
        }
        if (ctype == TLS_CT_CHANGE_CIPHER_SPEC) { st->rec_have = 0; return TLS_STEP_AGAIN; }
        if (ctype == TLS_CT_HANDSHAKE) {
            // Post-handshake handshake message: the only legal one here is
            // NewSessionTicket (RFC 8446 §4.6.1). Servers (Cloudflare et al.)
            // routinely send 2 per connection, sometimes BEFORE app data —
            // without this branch the flight dies as PROTO. Each ticket is
            // bound to this connection's resumption master and cached for a
            // future PSK offer. Anything else (KeyUpdate, rogue flight) is a
            // protocol violation. Malformed tickets are fatal too (RFC §6):
            // silently keeping a corrupt ticket would resume into garbage.
            uint32_t hp = 0;
            int saw_nst = 0;
            while (hp < (uint32_t)pl) {
                uint8_t t; uint32_t bl;
                uint32_t c = parse_hs(pt + hp, (uint32_t)pl - hp, &t, &bl);
                if (c == 0 || t != TLS_HS_NEW_SESSION_TICKET) {
                    tls_dbg("[tls] post-handshake msg type %u rejected\n", t);
                    st->fail_reason = TLS_FAIL_PROTO;
                    return TLS_STEP_ERR;
                }
                struct tls_nst nst;
                if (tls_parse_nst(pt + hp + 4, bl, &nst) != 0) {
                    tls_dbg("[tls] malformed ticket rejected\n");
                    st->fail_reason = TLS_FAIL_PROTO;
                    return TLS_STEP_ERR;
                }
                if (st->have_res)
                    ticket_store(st->host, nst.ticket, nst.ticket_len,
                                 nst.nonce, nst.nonce_len, st->res_master,
                                 nst.lifetime, nst.age_add, st->now_ms);
                else
                    tls_dbg("[tls] ticket dropped (no resumption state)\n");
                saw_nst = 1;
                hp += c;
            }
            if (!saw_nst) { st->fail_reason = TLS_FAIL_PROTO; return TLS_STEP_ERR; }
            st->rec_have = 0;
            return TLS_STEP_AGAIN;
        }
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
            // Overflow discipline (review #3): NEVER `out_len + pl > cap`
            // (wraps past 4GB after enough app data → heap... stack-buffer
            // overwrite). Subtract instead; out_len <= out_cap is the
            // maintained invariant (every append path checks first).
            if (st->out_len > st->out_cap ||
                (uint32_t)pl > st->out_cap - st->out_len) { st->fail_reason = TLS_FAIL_PROTO; st->phase = TLS_PH_DONE; return TLS_STEP_ERR; }
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

void tls_state_wipe(struct tls_state* st) {
    if (!st) return;
    secure_zero(st->priv, sizeof(st->priv));
    secure_zero(st->ch, sizeof(st->ch));
    secure_zero(st->sh_body, sizeof(st->sh_body));
    secure_zero(st->c_hs_key, sizeof(st->c_hs_key));
    secure_zero(st->c_hs_iv, sizeof(st->c_hs_iv));
    secure_zero(st->s_hs_key, sizeof(st->s_hs_key));
    secure_zero(st->s_hs_iv, sizeof(st->s_hs_iv));
    secure_zero(st->c_hs_secret, sizeof(st->c_hs_secret));
    secure_zero(st->hs_secret, sizeof(st->hs_secret));
    secure_zero(st->s_fin_key, sizeof(st->s_fin_key));
    secure_zero(st->ee_body, sizeof(st->ee_body));
    secure_zero(st->cert_body, sizeof(st->cert_body));
    secure_zero(st->cv_body, sizeof(st->cv_body));
    secure_zero(st->fin_body, sizeof(st->fin_body));
    secure_zero(st->c_ap_key, sizeof(st->c_ap_key));
    secure_zero(st->c_ap_iv, sizeof(st->c_ap_iv));
    secure_zero(st->s_ap_key, sizeof(st->s_ap_key));
    secure_zero(st->s_ap_iv, sizeof(st->s_ap_iv));
    secure_zero(st->c_fin_key, sizeof(st->c_fin_key));
    secure_zero(st->master, sizeof(st->master));
    secure_zero(st->res_master, sizeof(st->res_master));
    secure_zero(st->psk_early, sizeof(st->psk_early));
    secure_zero(st->rec_buf, sizeof(st->rec_buf));
    secure_zero(st->hs_buf, sizeof(st->hs_buf));
    st->have_master = st->have_res = 0;
    st->offer_psk = st->psk_accepted = 0;
    st->c_ap_seq = st->s_ap_seq = st->c_seq = st->s_seq = 0;
}

int tls_client_run(const char* host, uint16_t port,
                   const uint8_t* request, uint32_t request_len,
                   uint8_t* out, uint32_t out_cap,
                   const struct tls_client_io* io) {
    struct tls_state st;
    tls_state_init(&st, host, port, request, request_len, out, out_cap);
#ifndef KERNEL
    // Host wall clock for the ticket-age gate (review #31). The kernel
    // passes tick_count at fetch time instead (see tls_net.c).
    {
        time_t tt = time(0);
        if (tt > 0) tls_state_set_now_ms(&st, (uint64_t)tt * 1000u);
    }
#endif
    int r;
    do {
        r = tls_state_step(&st, io);
    } while (r == TLS_STEP_AGAIN);
    g_last_fail_reason = st.fail_reason;
    int out_len = (int)st.out_len;
    tls_state_wipe(&st); // stack state must not outlive the call
    if (r == TLS_STEP_DONE) return out_len;
    return -1;
}
