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
// recv() contract: returns >0 bytes read, -1 on peer close/error, 0 to signal
// "would block" (no data yet) — the driver then yields to the main loop and
// retries next tick. Blocking callers (host) never return 0.

#define TLS_STEP_AGAIN 0   // need more I/O; driver should yield and retry
#define TLS_STEP_DONE   1   // out/out_len hold the full plaintext response
#define TLS_STEP_ERR   -1   // handshake or transport failure

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
    const char* verify_host;   // hostname used for certificate verification;
                               // NULL = use `host`. Lets tests keep a truthful
                               // SNI on the wire while checking a wrong name
                               // (the MITM-accommodation scenario).
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

    // Handshake flight order: 0=expect EE, 1=CERT, 2=CV, 3=Finished.
    int hs_next;
    // Diagnostics / failure classification for the caller.
    int fail_reason;   // TLS_FAIL_* (TLS_FAIL_NONE while running)
    int alert_desc;    // alert description when fail_reason == TLS_FAIL_ALERT

    // Reassembly buffer for the record currently being read across steps.
    uint8_t rec_buf[TLS_RECORD_MAX_PAYLOAD + 5];
    uint32_t rec_have;
    uint32_t rec_pl;
};

void tls_state_init(struct tls_state* st, const char* host, uint16_t port,
                    const uint8_t* request, uint32_t request_len,
                    uint8_t* out, uint32_t out_cap);
int tls_state_step(struct tls_state* st, const struct tls_client_io* io);

int tls_last_fail_reason(void); // TLS_FAIL_* of the most recent tls_client_run

int tls_client_run(const char* host, uint16_t port,
                   const uint8_t* request, uint32_t request_len,
                   uint8_t* out, uint32_t out_cap,
                   const struct tls_client_io* io);

#endif