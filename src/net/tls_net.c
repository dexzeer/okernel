// TLS 1.3 network integration for the kernel.
// Provides https_get() which queues an HTTPS GET (DNS resolution, TCP:443
// connect, full TLS 1.3 handshake, HTTP GET, and response buffering) and runs
// it ASYNCHRONOUSLY: https_get_poll() is called once per main loop and advances
// the fetch by one step. This keeps the desktop responsive (mouse/keyboard/
// other windows) while a page loads, instead of blocking the whole kernel for
// the duration of the handshake + download.
//
// The TLS client driver (tls_client.c) is a resumable state machine. Its recv
// callback (kernel_tcp_recv) is NON-blocking: it returns available bytes, -1 on
// peer close, or 0 when no data has arrived yet. The driver then yields to the
// main loop; the next iteration re-attempts. Incoming packets are appended to
// tls_rx_buf by tcp_handle_packet -> tls_append_data() as the main loop polls
// the NIC (e1000_poll / net_poll).
#include "tls_net.h"
#include "../crypto/tls_client.h"
#include "network.h"
#include "e1000.h"
#include "../serial.h"
#include "../io.h"
#include "../idt.h"
#include <string.h>

// TLS receive buffer — TCP data from tcp_handle_packet goes here.
#define TLS_RX_BUF_SIZE 65536
static uint8_t  tls_rx_buf[TLS_RX_BUF_SIZE];
static uint32_t tls_rx_head;     // next write position
static uint32_t tls_rx_tail;     // next read position
static uint32_t tls_rx_len;      // bytes available

// TLS response buffer — decrypted HTTP response goes here.
static char     tls_response[262144];
static uint32_t tls_response_len;

// Async fetch state machine.
enum { HP_IDLE = 0, HP_DNS, HP_TCP, HP_TLS };
static int tls_active;           // 1 while a fetch is in progress
static int tls_phase;            // HP_* current phase
static char tls_host_buf[128];   // copied (caller's host buffer may be transient)
static const char* tls_host;
static char tls_path[128];
static char tls_req_buf[512];
static uint32_t tls_req_len;

static int tls_done = 0;         // 1 once the response is buffered
static int tls_peer_closed = 0;  // set by tcp_handle_packet on FIN
// TCP connection attempts for the current fetch. Bounded so an unreachable host
// gives up instead of re-tcp_connect() forever (which would wedge the okai
// single-owner fetch model).
static int tls_conn_attempts = 0;
#define TLS_MAX_CONN_ATTEMPTS 4

static uint32_t tls_resolved_ip = 0;

// Fetch start tick, for a defensive timeout so a connection that can never
// complete (e.g. unreachable host) stops the async machine instead of spinning
// forever and leaving the okai on a blank page.
extern uint32_t tick_count;
static uint32_t tls_start_tick = 0;
#define TLS_FETCH_TIMEOUT_TICKS 1200 // ~66s at 18 ticks/s; generous on purpose

// Resumable TLS client state, advanced one step per https_get_poll() call.
static int kernel_tcp_send(const uint8_t* buf, uint32_t len, void* user);
static int kernel_tcp_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user);
static struct tls_state tls_s;
static struct tls_client_io tls_io = {
    .send = kernel_tcp_send,
    .recv = kernel_tcp_recv,
    .user = NULL,
};

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

static int kernel_tcp_send(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    tcp_send_data((uint8_t*)buf, (uint16_t)len);
    return 0;
}

// NON-blocking recv: returns buffered bytes, -1 on peer close, or 0 when no
// data has arrived yet (caller yields to the main loop). The main loop polls
// the NIC every frame, so data shows up within a frame or two.
static int kernel_tcp_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)user;
    (void)timeout_ms;
    if (tls_rx_len == 0) {
        if (tls_peer_closed || tcp_is_closed()) return -1;
        return 0; // would block
    }
    uint32_t to_read = tls_rx_len < cap ? tls_rx_len : cap;
    for (uint32_t i = 0; i < to_read; i++) {
        buf[i] = tls_rx_buf[tls_rx_tail];
        tls_rx_tail = (tls_rx_tail + 1) % TLS_RX_BUF_SIZE;
    }
    tls_rx_len -= to_read;
    return (int)to_read;
}

void https_get(const char* host, const char* path) {
    tls_rx_reset();
    tls_response_len = 0;
    tls_response[0] = 0;
    tls_active = 1;
    tls_phase = HP_DNS;
    tls_done = 0;
    tls_peer_closed = 0;
    tls_resolved_ip = 0;
    tls_conn_attempts = 0;
    tls_start_tick = tick_count;

    int i;
    for (i = 0; host[i] && i < 127; i++) tls_host_buf[i] = host[i];
    tls_host_buf[i] = 0;
    tls_host = tls_host_buf;

    for (i = 0; path[i] && i < 127; i++) tls_path[i] = path[i];
    tls_path[i] = 0;

    int rlen = 0;
    const char* req = "GET ";
    while (*req) tls_req_buf[rlen++] = *req++;
    for (i = 0; tls_path[i]; i++) tls_req_buf[rlen++] = tls_path[i];
    tls_req_buf[rlen++] = ' ';
    req = "HTTP/1.1\r\nHost: ";
    while (*req) tls_req_buf[rlen++] = *req++;
    for (i = 0; tls_host[i]; i++) tls_req_buf[rlen++] = tls_host[i];
    req = "\r\nUser-Agent: okernel/0.4\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    while (*req) tls_req_buf[rlen++] = *req++;
    tls_req_len = rlen;

    dns_resolve(host);
    serial_puts("[tls-net] queued HTTPS ");
    serial_puts(host);
    serial_puts(tls_path);
    serial_puts("\n");
}

// Advance the in-flight HTTPS fetch by one step. Called once per main-loop
// iteration. Yields between phases so the desktop stays interactive.
void https_get_poll(void) {
    if (!tls_active) return;

    // Defensive timeout: a fetch that can't complete (unreachable host, dropped
    // SYN) must stop the machine rather than hang the okai on a blank page.
    if (tls_phase != HP_IDLE && (uint32_t)(tick_count - tls_start_tick) > TLS_FETCH_TIMEOUT_TICKS) {
        serial_puts("[tls-net] fetch timed out\n");
        tls_response_len = 0;
        tls_response[0] = 0;
        tls_done = 0;
        tls_active = 0;
        tls_phase = HP_IDLE;
        return;
    }

    if (tls_phase == HP_DNS) {
        if (dns_is_resolved(&tls_resolved_ip, tls_host)) {
            serial_puts("[tls-net] DNS resolved, opening TCP:443\n");
            tls_phase = HP_TCP;
        } else if (!dns_is_pending()) {
            // The query was answered but carried no A record (bad/empty host,
            // NXDOMAIN). Abort now instead of stalling until the 66s timeout —
            // the okai owner model must be released for the next window.
            serial_puts("[tls-net] DNS failed (no A record), aborting fetch\n");
            tls_response_len = 0;
            tls_response[0] = 0;
            tls_done = 0;
            tls_active = 0;
            tls_phase = HP_IDLE;
        }
        return;
    }

    if (tls_phase == HP_TCP) {
        if (tcp_is_established()) {
            // Connection is up: run the TLS handshake exactly once.
            tls_state_init(&tls_s, tls_host, 443,
                           (const uint8_t*)tls_req_buf, tls_req_len,
                           (uint8_t*)tls_response, sizeof(tls_response) - 1);
            tls_phase = HP_TLS;
            serial_puts("[tls-net] TCP established, handshake...\n");
            return;
        }
        // Open a FRESH connection only when the global socket is not already
        // mid-handshake. tcp_connect() re-randomizes the ephemeral source port
        // on every call, so re-invoking it each poll while SYN_SENT sends a SYN
        // from a new port every frame; the server's SYN-ACK (destined for the
        // previous port) is then dropped and the handshake can never complete —
        // the 2nd fetch just retransmits forever and the window stays black.
        // A SYN_SENT connection is owned by tcp_poll()'s retransmit timer: send
        // the SYN once here, then let that timer ride until ESTABLISHED.
        if (tcp_conn_state() != TCP_STATE_SYN_SENT) {
            tls_conn_attempts++;
            if (tls_conn_attempts > TLS_MAX_CONN_ATTEMPTS) {
                // Unreachable after several SYN attempts: abandon so the okai
                // owner model can move on to the next pending window.
                tls_response_len = 0;
                tls_response[0] = 0;
                tls_done = 0;
                tls_active = 0;
                tls_phase = HP_IDLE;
                serial_puts("[tls-net] giving up: host unreachable\n");
                return;
            }
            tcp_connect(tls_resolved_ip, 443);
        }
        return;
    }

    if (tls_phase == HP_TLS) {
        int r = tls_state_step(&tls_s, &tls_io);
        if (r == TLS_STEP_DONE) {
            tls_response_len = tls_s.out_len;
            tls_response[tls_s.out_len] = 0;
            tls_done = 1;
            tls_active = 0;
            tls_phase = HP_IDLE;
            serial_printf("[tls-net] received %u bytes\n", (unsigned)tls_s.out_len);
        } else if (r == TLS_STEP_ERR) {
            tls_response_len = 0;
            tls_response[0] = 0;
            tls_done = 0;
            tls_active = 0;
            tls_phase = HP_IDLE;
            serial_puts("[tls-net] handshake/download FAILED\n");
        }
        return;
    }
}

int tls_poll(void) {
    https_get_poll();
    return tls_done ? 1 : 0;
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

int tls_is_done(void) {
    return tls_done;
}

void tls_connection_closed(void) {
    tls_peer_closed = 1;
}
