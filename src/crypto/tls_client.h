#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <stdint.h>
#include "tls_record.h"

// Top-level TLS 1.3 client driver. Connects to `host:port` over TCP using
// the provided send/recv callbacks, performs the 1-RTT handshake, sends
// `request` (must already be a complete HTTP request), receives the
// response, decrypts it, and stores plaintext into `out`.
//
// Returns the number of plaintext bytes written to `out` (up to `out_cap`),
// or -1 on failure.
//
// Callbacks:
//   tcp_send(buf, len) -> 0 on success, nonzero on error.
//   tcp_recv(buf, cap, timeout_ms) -> bytes read (>=0), -1 on timeout/err.
//
// On host: pass plain BSD socket wrappers. On kernel: wrap tcp_send_data
// and the e1000 RX path.

struct tls_client_io {
    int (*send)(const uint8_t* buf, uint32_t len, void* user);
    int (*recv)(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user);
    void* user;
};

// Resumable client state. tls_client_run() is a thin synchronous wrapper that
// loops tls_state_step() to completion (host tests, blocking I/O). The kernel
// drives tls_state_step() one call at a time from the main loop so the desktop
// never freezes during the handshake/download.
//
// CONCURRENCY CONTRACT (review #6/#33): single-threaded, main-loop only.
// No IRQ handler may enter tls_state_step/cert_verify/ec_verify/rsa_verify
// (the timer ISR stirs the RNG only, and rand_* hold cli across transitions).
// The ticket/pin caches are process-global by design (single connection at a
// time; tls_net's fetch-owner model enforces it) — they persist across
// connections with documented lifetimes, never across threads (none exist).
// g_last_fail_reason is host-test-only for the same reason.
//
// recv() contract: returns >0 bytes read, -1 on peer close/error, 0 to signal
// "would block" (no data yet) — the driver then yields to the main loop and
// retries next tick. Blocking callers (host) never return 0.

#define TLS_STEP_AGAIN 0   // need more I/O; driver should yield and retry
#define TLS_STEP_DONE   1   // out/out_len hold the full plaintext response
#define TLS_STEP_ERR   -1   // handshake or transport failure

// Handshake reassembly cap (review #9): complete flights must fit. Real
// flights run 1-4KB (our RSA-2048 chains ~2KB); 16KB covers RSA-4096
// chains with headroom. Past this: reject, never truncate.
#define TLS_HS_REASSEMBLY_MAX 16384

// Why the state machine stopped. Recorded in tls_state.fail_reason so the
// caller can distinguish "clean end" from "peer killed us" from "the
// certificate chain failed to verify" — the okai must NOT fall back to plain
// HTTP on the latter two (a MITM can force that by killing the handshake).
#define TLS_FAIL_NONE        0
#define TLS_FAIL_ALERT_CLOSE 1  // close_notify — clean EOF
#define TLS_FAIL_ALERT       2  // fatal alert (tls_state.alert_desc)
#define TLS_FAIL_MAC         3  // record decrypt/MAC failure (tampering?)
#define TLS_FAIL_CERT        4  // certificate chain verification failed
#define TLS_FAIL_HOSTNAME    5  // hostname does not match certificate
#define TLS_FAIL_PROTO       6  // protocol violation
#define TLS_FAIL_RNG         7  // entropy source unavailable (never synthesize keys)

enum tls_phase {
    TLS_PH_SEND_CH = 0,  // build + send ClientHello
    TLS_PH_RECV_SH,      // read ServerHello (cleartext)
    TLS_PH_RECV_HS,      // read encrypted EE/Cert/CV/Finished
    TLS_PH_RECV_BODY,    // read encrypted application data
    TLS_PH_DONE
};

struct tls_state {
    int phase;
    const char* host;
#ifndef KERNEL
    const char* verify_host;   // HOST-ONLY test hook (review 2026-09-10 #12):
                               // hostname used for certificate verification;
                               // NULL = use `host`. Lets tests keep a truthful
                               // SNI on the wire while checking a wrong name
                               // (the MITM-accommodation scenario). Absent
                               // from production: authentication parameters
                               // must not be mutable convenience fields.
#endif
    uint16_t port;
    const uint8_t* request;
    uint32_t request_len;
    uint8_t* out;
    uint32_t out_cap;
    uint32_t out_len;

    // Ephemeral handshake material
    uint8_t priv[32], pub[32], random[32], session_id[32];
    uint8_t ch[1024]; uint32_t ch_len;
    uint8_t sh_body[4096]; uint32_t sh_bl;
    uint8_t c_hs_key[32], c_hs_iv[12];
    uint8_t s_hs_key[32], s_hs_iv[12];
    uint8_t c_hs_secret[32];
    uint8_t hs_secret[32];
    uint8_t s_fin_key[32];
    uint64_t s_seq, c_seq;
    uint8_t ee_body[4096], cert_body[12288], cv_body[1024], fin_body[64];
    uint32_t ee_bl, cert_bl, cv_bl, fin_bl;
    int got_ee, got_cert, got_cv, got_sfin;
    uint8_t c_ap_key[32], c_ap_iv[12], s_ap_key[32], s_ap_iv[12];
    uint8_t c_fin_key[32];
    uint64_t c_ap_seq, s_ap_seq;

    // Resumption basis (stashed at handshake end): master secret + the
    // resumption master (through BOTH Finished messages). have_res gates
    // ticket storage — no resumption state, no tickets kept.
    uint8_t master[32]; int have_master;
    uint8_t res_master[32]; int have_res;
    // Clock for ticket-age accounting (ms). Set via tls_state_set_now_ms;
    // 0 = unknown (ticket stored with age 0 — plausible, servers tolerate).
    uint64_t now_ms;
    // PSK offer state (this connection): 1 while a ticket was offered in
    // the CH (early secret stashed for the handshake keys); psk_accepted
    // set in RECV_SH iff the server selected our identity.
    int offer_psk; int psk_accepted;
    uint8_t psk_early[32];

    // OCSP staple noted (RFC 8446 §4.4.2.1): a CertificateEntry carried a
    // well-formed status_request. Presence only — content unenforced (see
    // tls_cert_has_staple). Surfaced for the warning-page honesty note.
    int got_staple;
    // Stapled OCSP response bytes (copied for post-auth validation — the
    // cert_body views stay alive, but an explicit bounded copy keeps the
    // validator's lifetime independent). Cap 2048B (real staples run
    // 500-1500B); larger staples fail the flight, not the parser.
    uint8_t staple[2048];
    uint32_t staple_len;

    // Handshake flight order: 0=expect EE, 1=CERT, 2=CV, 3=Finished.
    int hs_next;
    // Certificate-failure detail (CV_ERR_* or 0): set whenever fail_reason
    // becomes TLS_FAIL_CERT/HOSTNAME so the browser can print WHY (expired?
    // hostname? pin change?) instead of a generic warning.
    int cert_detail;
    // Diagnostics / failure classification for the caller.
    int fail_reason;   // TLS_FAIL_* (TLS_FAIL_NONE while running)
    int alert_desc;    // alert description when fail_reason == TLS_FAIL_ALERT

    // Reassembly buffer for the record currently being read across steps.
    uint8_t rec_buf[TLS_RECORD_MAX_PAYLOAD + 5];
    uint32_t rec_have;
    uint32_t rec_pl;

    // Handshake-message reassembly (RFC 8446 §5.1: messages may fragment
    // across records). Decrypted HANDSHAKE payloads accumulate here;
    // complete messages are dispatched in order, leftovers compacted.
    // Zeroed by tls_state_init with the rest of the struct.
    uint8_t hs_buf[TLS_HS_REASSEMBLY_MAX];
    uint32_t hs_have;
    // Set on authenticated close_notify (cryptoholes #1): lets the caller
    // distinguish "complete, peer-closed-cleanly" from "EOF with no
    // close" (which needs HTTP Content-Length framing to prove complete
    // — see tls_response_complete). Preserved by tls_state_wipe (like
    // out/fail codes, unlike key material).
    int saw_close;
};

void tls_state_init(struct tls_state* st, const char* host, uint16_t port,
                    const uint8_t* request, uint32_t request_len,
                    uint8_t* out, uint32_t out_cap);
// Optional wall clock (ms) for ticket-age accounting. The kernel passes
// tick_count at fetch time; host callers pass gettimeofday-ish ms.
// Default 0 = unknown (stored tickets read age 0).
void tls_state_set_now_ms(struct tls_state* st, uint64_t now_ms);
int tls_state_step(struct tls_state* st, const struct tls_client_io* io);

// Wipe all ephemeral key material in a finished/failed state (review #30):
// ECDHE private, handshake/app secrets + keys/ivs, master + resumption
// master, PSK offer state, transcript + record + reassembly buffers.
// Preserves: out/out_len/out_cap (caller response), fail codes + detail,
// phase, host pointers, saw_close. Ticket/pin stores are
// connection-independent and intentionally survive (documented lifetimes,
// not per-connection state).
void tls_state_wipe(struct tls_state* st);

int tls_last_fail_reason(void); // TLS_FAIL_* of the most recent tls_client_run

int tls_client_run(const char* host, uint16_t port,
                   const uint8_t* request, uint32_t request_len,
                   uint8_t* out, uint32_t out_cap,
                   const struct tls_client_io* io);

// ---- Trust-On-First-Use leaf pinning ----
// Memory-only pin store (lost on reboot): hostname → verified leaf SPKI
// hash. First verified visit stores; later visits with a DIFFERENT leaf key
// fail closed (possible MITM with a rogue-but-valid cert, or a legitimate
// rotation — indistinguishable without an override UX, which doesn't exist
// yet; rotations are rare and the lockout ends at reboot).
// Preloaded hosts (compiled table in tls_client.c): first visit MUST match
// (-2 below) — narrows the TOFU first-visit window for high-value hosts.
// Returns 0 (first-seen stored, or match), -1 (changed — old pin kept, so
// every visit warns until reboot/re-pin window), -2 (preload mismatch).
// Test hook: tls_pin_clear() drops all pins.
int tls_pin_check(const char* host, const uint8_t spki_hash[32]);
void tls_pin_clear(void);
// Persistence (desktop serializes to VFS `/.pins` on change, loads at
// boot; network-attacker threat model — see tls_client.c).
int tls_pin_dirty(void);
void tls_pin_clean(void);
uint32_t tls_pin_export(uint8_t* out, uint32_t cap); // bytes written, 0 = cap
int tls_pin_import(const uint8_t* in, uint32_t len); // 0 ok, -1 malformed

// ---- Session-ticket cache (RFC 8446 §4.6.1) ----
// Process-global, freestanding-safe (static slots, no allocation). Keyed by
// hostname; each new connection to a known host may offer the ticket as a
// PSK (see the offer path in tls_client.c). Tickets fuel resumption ONLY —
// the first handshake to a host always fully verifies the chain.
// Test/diagnostic hooks:
int tls_ticket_have(const char* host, uint64_t now_ms); // 1 if an unexpired
    // ticket is cached AS OF now_ms (explicit trusted time, ms — pass the
    // same clock given to tls_state_set_now_ms; 0/unknown is NOT fresh).
void tls_ticket_clear(void);           // drop all (tests, memory hygiene)

// ---- HTTP response completeness over a TLS stream (cryptoholes #1) ----
// A clean EOF (or missing close_notify) ends the STREAM, not provably the
// MESSAGE: an on-path attacker cutting TCP turns a complete response into
// a prefix. Verdicts: COMPLETE (framing proves it), SHORT (provably cut),
// UNKNOWN (close-delimited: no length signal — unknowable, curl-parity
// accept). Pure function of response bytes (host-testable); the caller
// (desktop fetch-complete) fails SHORT as truncation (no HTTP fallback).
#define TLS_RESP_COMPLETE 0
#define TLS_RESP_SHORT    1
#define TLS_RESP_UNKNOWN  2
int tls_response_complete(const uint8_t* resp, uint32_t len);

#endif