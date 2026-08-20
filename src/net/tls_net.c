// TLS 1.3 network integration for the kernel.
// Provides https_get() which resolves DNS, opens a TCP:443 connection, runs the
// full TLS 1.3 handshake, sends the HTTP request, and buffers the decrypted
// response — all from a single blocking call.
//
// The kernel has no select()/poll() syscall and the TLS client (tls_client.c)
// is a single blocking driver. So this module makes the recv callback *itself*
// pump the NIC: each time the handshake waits for bytes it calls e1000_poll()
// + net_poll(), which process incoming packets into the TLS receive buffer via
// tcp_handle_packet -> tls_append_data(). This is the kernel equivalent of a
// blocking select() loop, and it runs fine from the keyboard ISR context
// (interrupts are off there, but RX is DMA so polling the descriptors works).
#include "tls_net.h"
#include "../crypto/tls_client.h"
#include "network.h"
#include "e1000.h"
#include "../serial.h"
#include "../io.h"
#include "../idt.h"
#include <string.h>

// TLS receive buffer — TCP data from tcp_handle_packet goes here.
// Must be large enough to hold an entire in-flight burst: e1000_poll() drains
// every available RX descriptor in one call, so a ~28KB page can land in the
// ring before the handshake driver reads it. Too small and tls_append_data
// silently drops bytes, truncating the TLS stream mid-record (seen with
// notdexy.ru: 16KB dropped ~14KB and corrupted a 7133-byte record).
#define TLS_RX_BUF_SIZE 65536
static uint8_t  tls_rx_buf[TLS_RX_BUF_SIZE];
static uint32_t tls_rx_head;     // next write position
static uint32_t tls_rx_tail;     // next read position
static uint32_t tls_rx_len;      // bytes available

// TLS response buffer — decrypted HTTP response goes here.
// Must exceed the largest page we want to render (notdexy.ru is ~29KB). The
// handshake driver caps out_len at this size and fails past it, so a too-small
// buffer yields a blank page rather than a truncated one.
static char     tls_response[65536];
static uint32_t tls_response_len;

// TLS state
static int tls_active;           // 1 while a TLS session is in progress
static int tls_phase;            // handshake phase for the state machine
static const char* tls_host;
static char tls_path[128];
static char tls_req_buf[512];

static int tls_done = 0;         // 1 once https_get has a finished response
static int tls_peer_closed = 0;  // set by tcp_handle_packet when FIN arrives

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

// Yield the vCPU so QEMU's SLIRP (which shares the QEMU process) can run and
// DMA inbound packets into the RX ring we're polling. A tight CPU-spin here
// would starve SLIRP under TCG (no KVM): the guest hogs the process and the
// SYN/ACK + TLS records never get delivered, so the fetch hangs. Halting with
// interrupts enabled lets the timer/NIC IRQs wake us (~55ms windows at 18Hz),
// which is enough to keep the network pump fed. The keyboard IRQ is masked for
// the duration so a stray keystroke can't re-enter on_keypress mid-fetch, and
// the original interrupt flag is preserved either way.
static void net_yield(void) {
    uint32_t flags;
    __asm__ volatile("pushf; popl %0" : "=r"(flags));
    uint8_t kmask = inb(0x21);
    outb(0x21, kmask | 0x02); // block IRQ1 (keyboard)
    if (flags & 0x200) {
        __asm__ volatile("hlt"); // IF already on; stay enabled
    } else {
        __asm__ volatile("sti; hlt; cli"); // re-establish IF=0 afterwards
    }
    outb(0x21, kmask); // restore keyboard mask
}

// Brief yield between NIC polls in the blocking loop (see net_yield).
static void tls_spin(void) {
    net_yield();
}

// ---- TCP send callback for tls_client_io ----

static int kernel_tcp_send(const uint8_t* buf, uint32_t len, void* user) {
    (void)user;
    // tcp_send_data handles segmentation internally (requires ESTABLISHED).
    tcp_send_data((uint8_t*)buf, (uint16_t)len);
    return 0;
}

// ---- TCP recv callback for tls_client_io ----
// Polls the NIC (which fills tls_rx_buf via tcp_handle_packet -> tls_append_data)
// and returns buffered bytes, or -1 on peer close / timeout.

static int kernel_tcp_recv(uint8_t* buf, uint32_t cap, uint32_t timeout_ms, void* user) {
    (void)user;
    uint32_t waited = 0;
    uint32_t step = 40; // approximate ms per spin

    while (tls_rx_len == 0) {
        // Peer closed the connection and nothing is buffered -> EOF.
        if (tls_peer_closed || tcp_is_closed()) return -1;

        // Process any arrived packets. In the kernel there is no background
        // packet pump (the main loop is not running while we block), so we
        // must drive the NIC ourselves.
        e1000_poll();
        net_poll();
        tls_spin();

        waited += step;
        if (timeout_ms && waited >= timeout_ms) return -1;
    }

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
    tls_rx_reset();
    tls_response_len = 0;
    tls_active = 1;
    tls_phase = 0;
    tls_peer_closed = 0;
    tls_done = 0;
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

    // 1) DNS resolution (busy-wait, pumping the NIC)
    uint32_t ip = 0;
    if (!dns_is_resolved(&ip)) {
        dns_resolve(host);
        uint32_t t = 0;
        while (!dns_is_resolved(&ip) && t < 30000) {
            e1000_poll(); net_poll(); tls_spin(); t += 50;
        }
    }
    if (!dns_is_resolved(&ip)) {
        serial_puts("[tls-net] DNS FAILED\n");
        tls_active = 0;
        return;
    }
    serial_puts("[tls-net] resolved, opening TCP:443\n");

    // 2) TCP connect — call tcp_connect() only while CLOSED (it resets the
    //    connection state, so re-calling every iteration would re-send SYN).
    {
        uint32_t t = 0;
        while (!tcp_is_established() && t < 30000) {
            if (tcp_conn_state() == TCP_STATE_CLOSED) tcp_connect(ip, 443);
            e1000_poll(); net_poll(); tls_spin(); t += 50;
        }
    }
    if (!tcp_is_established()) {
        serial_puts("[tls-net] TCP connect FAILED\n");
        tls_active = 0;
        return;
    }
    serial_puts("[tls-net] TCP established, handshake...\n");

    // 3) TLS handshake + GET + read. The recv callback pumps the NIC.
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
        serial_printf("[tls-net] received %u bytes\n", (unsigned)n);
    }

    tls_done = (n > 0);
    tls_active = 0;
}

int tls_poll(void) {
    // The handshake is driven synchronously inside https_get(); there is no
    // asynchronous state machine to advance from the main loop.
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
    // Peer sent FIN. The recv callback watches this to return EOF promptly.
    tls_peer_closed = 1;
}
