#ifndef TLS_NET_H
#define TLS_NET_H

#include <stdint.h>

// HTTPS client for the kernel. Wraps the TLS 1.3 handshake driver
// (tls_client.c) and the kernel TCP stack (network.c).
//
// Usage from shell: `https_get("example.com", "/")`
//
// Flow:
//   1. https_get() resolves DNS, connects TCP:443, starts TLS handshake
//   2. tcp_handle_packet() appends into tls_rx_buf when tls_session_active
//   3. tls_poll() drives the TLS handshake from the main loop
//   4. When the response is ready, http_get_response() returns it

// Start an HTTPS GET request. Queues the fetch (DNS + TCP:443 + TLS 1.3
// handshake + download); it runs asynchronously, driven by https_get_poll()
// from the main loop, so the desktop never freezes during the load.
void https_get(const char* host, const char* path);
void https_get_port(const char* host, const char* path, uint16_t port); // https://host:port/

// Advance the in-flight HTTPS fetch by one step (DNS / TCP / TLS). Call once
// per main-loop iteration. When the response is ready, tls_is_done() returns 1.
void https_get_poll(void);

// Drive the TLS handshake forward. Call from main loop.
// Returns 0 if still in progress, 1 if response is ready, -1 on error.
int tls_poll(void);

// Check if a TLS session is active.
int tls_is_active(void);

// Check if the last https_get() completed with a buffered response.
int tls_is_done(void);
int tls_get_fail_reason(void); // TLS_FAIL_* of the last failed fetch (0 = none/success)
int tls_get_fail_detail(void); // CV_ERR_* detail when reason is CERT class (0 = none)

// Get the TLS response (same interface as http_get_response).
char* tls_get_response(void);
int tls_get_response_len(void);

// Called by tcp_handle_packet when tls_session_active is set.
// Appends raw TCP payload into the TLS receive buffer.
void tls_append_data(const uint8_t* data, uint32_t len);

// Called when TCP connection closes (FIN received).
void tls_connection_closed(void);

#endif
