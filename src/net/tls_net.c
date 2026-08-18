// TLS 1.3 network integration for the kernel.
// Provides https_get() that wraps the TLS client with the kernel's TCP stack.
#include "tls_net.h"
#include "../crypto/tls_client.h"
#include "network.h"
#include "../serial.h"
#include "../io.h"
#include <string.h>

// TLS receive buffer — TCP data from tcp_handle_packet goes here.
#define TLS_RX_BUF_SIZE 16384
static uint8_t  tls_rx_buf[TLS_RX_BUF_SIZE];
static uint32_t tls_rx_head;     // next write position
static uint32_t tls_rx_tail;     // next read position
static uint32_t tls_rx_len;      // bytes available

// TLS response buffer — decrypted HTTP response goes here.
static char     tls_response[16384];
static uint32_t tls_response_len;

// TLS state
static int tls_active;           // 1 when a TLS session is in progress
static int tls_phase;            // handshake phase for the state machine
static const char* tls_host;
static char tls_path[128];
static char tls_req_buf[512];

// ---- Buffer management ----

static void tls_rx_reset(void) {
    tls_rx_head = 0;
    tls_rx_tail = 0;
    tls_rx_len  = 0;
}

void tls_append_data(const uint8_t* data, uint32_t len) {
    for (uint32_t i = 0; i < len && tls_rx_len < TLS_RX_BUF_SIZE; i++) {
        tls_rx_buf[tls_rx_head] = data[i];
        tls_rx_head = (tls_rx_head + 1) % TLS_RX_BUF_SIZE;
        tls_rx_len++;
    }
}

// ---- TCP send callback for tls_client_io ----

static int kernel_tcp_send(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    // tcp_send_data handles segmentation internally.
    tcp_send_data((uint8_t*)buf, (uint16_t)len);
    return 0;
}

// ---- TCP recv callback for tls_client_io ----
// Polls the rx buffer with a simple busy-wait. In the kernel, net_poll()
// processes incoming packets which fill tls_rx_buf via tls_append_data().

static int kernel_tcp_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)user;
    uint32_t waited = 0;
    uint32_t step = 50;

    while (tls_rx_len == 0 && waited < timeout_ms) {
        // Run the network stack to process any pending packets.
        // This is the kernel equivalent of select()/poll().
        asm volatile("sti; nop; cli" ::: "memory"); // enable IRQ briefly
        waited += step;
        // Simple delay loop
        for (volatile uint32_t d = 0; d < 100000; d++) {}
    }

    if (tls_rx_len == 0) return -1;

    uint32_t to_read = tls_rx_len < cap ? tls_rx_len : cap;
    for (uint32_t i = 0; i < to_read; i++) {
        buf[i] = tls_rx_buf[tls_rx_tail];
        tls_rx_tail = (tls_rx_tail + 1) % TLS_RX_BUF_SIZE;
    }
    tls_rx_len -= to_read;
    return (int)to_read;
}

// ---- Public API ----

void https_get(const char* host, const char* path) {
    // Reset state
    tls_rx_reset();
    tls_response_len = 0;
    tls_active = 1;
    tls_phase = 0;
    tls_host = host;

    // Copy path
    int i;
    for (i = 0; path[i] && i < 127; i++) tls_path[i] = path[i];
    tls_path[i] = 0;

    // Build HTTP request
    int rlen = 0;
    const char* req = "GET ";
    while (*req) tls_req_buf[rlen++] = *req++;
    for (i = 0; tls_path[i]; i++) tls_req_buf[rlen++] = tls_path[i];
    tls_req_buf[rlen++] = ' ';
    req = "HTTP/1.1\r\nHost: ";
    while (*req) tls_req_buf[rlen++] = *req++;
    for (i = 0; tls_host[i]; i++) tls_req_buf[rlen++] = tls_host[i];
    req = "\r\nConnection: close\r\n\r\n";
    while (*req) tls_req_buf[rlen++] = *req++;

    serial_puts("[tls-net] starting HTTPS to ");
    serial_puts(host);
    serial_puts("\n");

    // Start TLS handshake — this runs the full handshake in a single call.
    // The recv callback polls tls_rx_buf, which is filled by tcp_handle_packet
    // via tls_append_data() when tls_is_active() returns true.
    struct tls_client_io io = {
        .send = kernel_tcp_send,
        .recv = kernel_tcp_recv,
        .user = NULL,
    };

    int n = tls_client_run(host, 443,
                           (const uint8_t*)tls_req_buf, rlen,
                           (uint8_t*)tls_response, sizeof(tls_response) - 1,
                           &io);

    if (n < 0) {
        serial_puts("[tls-net] handshake FAILED\n");
        tls_response_len = 0;
        tls_response[0] = 0;
    } else {
        tls_response_len = (uint32_t)n;
        tls_response[n] = 0;
        serial_puts("[tls-net] received ");
        // Print length in decimal
        serial_putchar('0' + (n / 10000) % 10);
        serial_putchar('0' + (n / 1000) % 10);
        serial_putchar('0' + (n / 100) % 10);
        serial_putchar('0' + (n / 10) % 10);
        serial_putchar('0' + n % 10);
        serial_puts(" bytes\n");
    }

    tls_active = 0;
}

int tls_poll(void) {
    return -1;  // Not used in current single-call implementation
}

int tls_is_active(void) {
    return tls_active;
}

char* tls_get_response(void) {
    return tls_response;
}

int tls_get_response_len(void) {
    return (int)tls_response_len;
}

void tls_connection_closed(void) {
    // Connection closed — nothing to do, tls_client_run handles EOF
}
