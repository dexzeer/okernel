#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>

#define ETH_ALEN 6
#define ETH_FRAME_MIN 60
#define ETH_FRAME_MAX 1514

// Ethernet header
struct eth_header {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} __attribute__((packed));

// ARP header
struct arp_header {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[ETH_ALEN];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[ETH_ALEN];
    uint8_t  target_ip[4];
} __attribute__((packed));

// IP header
struct ip_header {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed));

// ICMP header
struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

// UDP header
struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

// Initialize networking
void net_init(void);

// Send an ARP request
void arp_send_request(uint8_t* target_ip);

// Send a ping (ICMP echo request)
void icmp_send_ping(uint8_t* target_ip, uint16_t id, uint16_t seq);

// Send a UDP packet
void udp_send(uint8_t* dst_ip, uint16_t src_port, uint16_t dst_port, uint8_t* data, uint16_t len);

// Get our IP address
uint8_t* net_get_ip(void);

// Get gateway IP
uint8_t* net_get_gateway(void);

// Set IP address
void net_set_ip(uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3);

// Resolve IP to MAC (returns 1 if resolved, 0 if pending)
int arp_resolve(uint8_t* ip, uint8_t* mac);

// ---- DNS: multi-entry cache, concurrent queries (QEMU SLIRP 10.0.2.3) ----
// 1 = resolved (*ip set, host byte order), 0 = query in flight (call again),
// -1 = definitive failure (NXDOMAIN / no A record / no answer after
// retries; negative-cached briefly). Numeric hosts resolve immediately.
int dns_lookup(const char* host, uint32_t* ip);
// Shell `resolve`: look up and post "Resolved: a.b.c.d" as a net event.
int dns_resolve(const char* hostname);
int net_parse_ip(const char* s, uint32_t* out); // "a.b.c.d" -> host-order IP (0 = fail)

// ---- TCP sockets ----
// A small socket table: several connections at once (the browser fetches
// in parallel). Every socket owns its retransmit flight, reorder buffer
// and a receive ring; the advertised window is the ring's free space, so
// bytes we ACK are always bytes we kept.
#define TCP_MAX_SOCKS 16
#define TCP_STATE_CLOSED      0
#define TCP_STATE_SYN_SENT    1
#define TCP_STATE_ESTABLISHED 2
#define TCP_STATE_FIN_WAIT    3
int  tcp_open(uint32_t dst_ip, uint16_t dst_port);   // socket id (SYN sent) or -1
int  tcp_state(int s);                               // TCP_STATE_*
// Queue + send (retransmitted until ACKed). 0 ok, -1 not established /
// flight buffer full.
int  tcp_send(int s, const uint8_t* data, uint32_t len);
// >0 bytes copied, 0 nothing buffered yet, -1 end of stream (peer FIN,
// reset or give-up) once every buffered byte has been read.
int  tcp_recv(int s, uint8_t* buf, uint32_t cap);
int  tcp_rx_avail(int s);
int  tcp_failed(int s);       // 1 after RST / retransmit give-up
// Graceful close (FIN; the slot frees itself once closed) / abort (RST,
// slot free immediately). The id is invalid after either call.
void tcp_close(int s);
void tcp_abort(int s);

// Poll timers, DNS retries and delayed ACKs (call from the main loop,
// right after e1000_poll).
void net_poll(void);

// Set callback for network events (ping replies, etc.)
void net_set_event_callback(void (*cb)(const char* msg));

// Top-level navigation: send a browser's HTML-first Accept instead of */*.
// Some sites route on it — crates.io answers "Accept: */*" with an empty
// 404 (its API), text/html with the app.
#define NET_ACCEPT_HTML "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
// Extra request header lines ("Cookie: ...\r\n"), <= 4KB.
#define NET_EXTRA_MAX 4096
// Decode a raw HTTP response in place to its body (headers dropped,
// chunked transfer encoding undone). Returns the body length.
int http_dechunk(char* buf, int len);

#endif
