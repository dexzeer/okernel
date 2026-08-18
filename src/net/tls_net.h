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

// Start an HTTPS GET request. Resolves DNS, connects TCP:443,
// performs TLS 1.3 handshake, sends GET, and buffers the response.
void https_get(const char* host, const char* path);

// Drive the TLS handshake forward. Call from main loop.
// Returns 0 if still in progress, 1 if response is ready, -1 on error.
int tls_poll(void);

// Check if a TLS session is active.
int tls_is_active(void);

// Get the TLS response (same interface as http_get_response).
char* tls_get_response(void);
int tls_get_response_len(void);

// Called by tcp_handle_packet when tls_session_active is set.
// Appends raw TCP payload into the TLS receive buffer.
void tls_append_data(const uint8_t* data, uint32_t len);

// Called when TCP connection closes (FIN received).
void tls_connection_closed(void);

#endif
