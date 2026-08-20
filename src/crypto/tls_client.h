#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include <stdint.h>

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

int tls_client_run(const char* host, uint16_t port,
                   const uint8_t* request, uint32_t request_len,
                   uint8_t* out, uint32_t out_cap,
                   const struct tls_client_io* io);

#endif