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

// DNS resolution (returns 0 on success, -1 if waiting for ARP)
int dns_resolve(const char* hostname);
int dns_is_resolved(uint32_t* ip);
int dns_is_pending(void);

// TCP connection
void tcp_connect(uint32_t dst_ip, uint16_t dst_port);
void tcp_send_data(uint8_t* data, uint16_t len);
void tcp_close(void);

// Poll for pending operations (call from main loop)
void net_poll(void);

// Set callback for network events (ping replies, etc.)
void net_set_event_callback(void (*cb)(const char* msg));

// HTTP client
void http_get(const char* host, const char* path);
void http_poll(void);
char* http_get_response(void);
int http_get_response_len(void);
int http_is_pending(void);
int http_is_done(void);
int http_dechunk(char* buf, int len);

// Browse file save
void net_set_browse_save(const char* filename);

#endif
