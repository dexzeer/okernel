#include "network.h"
#include "e1000.h"
#include "../io.h"
#include "../serial.h"

static uint8_t our_ip[4] = {10, 0, 2, 15};    // QEMU user-mode default
static uint8_t gateway_ip[4] = {10, 0, 2, 2};  // QEMU user-mode gateway
static uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const char hex[] = "0123456789abcdef";

// ARP cache (4 entries)
#define ARP_CACHE_SIZE 4
static uint8_t arp_cache_ip[ARP_CACHE_SIZE][4];
static uint8_t arp_cache_mac[ARP_CACHE_SIZE][6];
static int arp_cache_count = 0;

// UDP callback
typedef void (*udp_callback_t)(uint8_t* data, uint32_t len, uint16_t src_port, uint16_t dst_port);
static udp_callback_t udp_callback = 0;
static uint16_t udp_callback_port = 0;

// DNS resolver state
static uint16_t dns_tx_id = 0x1234;
static uint32_t dns_resolved_ip = 0;
static int dns_resolved = 0;
static int dns_pending = 0;

// Pending operation state (for retry after ARP resolves)
static int ping_pending = 0;
static uint8_t ping_target_ip[4];
static uint16_t ping_id = 0;
static uint16_t ping_seq = 0;

static char dns_pending_host[128] = {0};
static int dns_retry_pending = 0;

static char http_pending_host[128] = {0};
static char http_pending_path[128] = {0};
static int http_retry_pending = 0;

// Network event callback (for terminal output)
static void (*net_event_callback)(const char* msg) = 0;
static char net_event_msg[256] = {0};
static int net_event_pending = 0;

static void net_event_post(const char* msg) {
    int i;
    for (i = 0; msg[i] && i < 254; i++) net_event_msg[i] = msg[i];
    net_event_msg[i++] = '\n';
    net_event_msg[i] = 0;
    net_event_pending = 1;
}

// TCP state
#define TCP_STATE_CLOSED   0
#define TCP_STATE_SYN_SENT 1
#define TCP_STATE_ESTABLISHED 2
#define TCP_STATE_FIN_WAIT 3

struct tcp_conn {
    uint8_t dst_ip[4]; // stored in network byte order
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;      // SND.NXT: next seq to send
    uint32_t ack;      // RCV.NXT: next seq expected from peer
    uint32_t snd_una;  // oldest unacked sequence number (flight start)
    uint16_t dst_mac[3]; // stored as 3 x uint16_t for alignment
    int state;
    // Retransmission state (single connection, single in-flight request)
    uint16_t rtx_len;        // bytes of unacked data in tcp_rtx_buf
    uint8_t  rtx_has_syn;    // SYN occupies one seq number at snd_una
    uint8_t  rtx_has_fin;    // FIN occupies one seq number after data
    uint8_t  rtx_tries;      // retransmit attempts since last progress
    uint16_t rtx_timeout;    // current RTO in ticks (doubles per retry)
    uint32_t rtx_last_tick;  // tick when the flight was (re)sent
};

static struct tcp_conn tcp_conn;

// Unacked outbound data for retransmission. Our sends are a GET request
// (<= ~512 bytes) or bare FIN, so 4KB is ample for this stack.
#define TCP_RTX_BUF_SIZE 4096
static uint8_t tcp_rtx_buf[TCP_RTX_BUF_SIZE];

// RTO in 18Hz ticks (~55ms each): 4 ticks ~= 220ms initial, doubling
#define TCP_RTO_TICKS 4
#define TCP_MAX_RTX 6

extern uint32_t tick_count;

// Sequence comparison with 32-bit wraparound (RFC 793 ordering)
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static void tcp_rtx_arm(void) {
    tcp_conn.rtx_last_tick = tick_count;
}
// HTTP response accumulation happens directly in tcp_handle_packet (IRQ
// context). A shared one-segment buffer dropped every segment that arrived
// before the main loop's http_poll ran — a multi-segment response started
// mid-headers with no clean \r\n\r\n, so html_parse showed raw headers as
// page text. Main loop only READS http_response after http_done (connection
// closed, no further IRQ appends), so there is no concurrent writer.

// Simple HTTP client state
static int http_pending = 0;
static int http_done = 0; // Set when connection closes with data
static char http_response[4096];
static int http_response_len = 0;

static uint16_t net_checksum(void* data, int len) {
    uint16_t* buf = (uint16_t*)data;
    uint32_t sum = 0;
    for (int i = 0; i < len / 2; i++) {
        sum += buf[i];
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum;
}

static void build_eth_header(struct eth_header* eth, uint8_t* dst, uint8_t* src, uint16_t type) {
    for (int i = 0; i < ETH_ALEN; i++) eth->dst[i] = dst[i];
    for (int i = 0; i < ETH_ALEN; i++) eth->src[i] = src[i];
    eth->type = ((type >> 8) & 0xFF) | ((type & 0xFF) << 8); // Network byte order
}

void arp_send_request(uint8_t* target_ip) {
    serial_puts("[arp] send request to ");
    for (int i = 0; i < 4; i++) {
        serial_putchar('0' + target_ip[i]);
        if (i < 3) serial_putchar('.');
    }
    serial_putchar('\n');

    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = e1000_get_mac();

    // Ethernet header
    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, broadcast_mac, mac, 0x0806); // ARP

    // ARP header
    struct arp_header* arp = (struct arp_header*)(frame + sizeof(struct eth_header));
    arp->hw_type = ((1 >> 8) & 0xFF) | ((1 & 0xFF) << 8);   // Ethernet
    arp->proto_type = ((0x0800 >> 8) & 0xFF) | ((0x0800 & 0xFF) << 8); // IPv4
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = ((1 >> 8) & 0xFF) | ((1 & 0xFF) << 8); // Request

    for (int i = 0; i < 6; i++) arp->sender_mac[i] = mac[i];
    for (int i = 0; i < 4; i++) arp->sender_ip[i] = our_ip[i];
    for (int i = 0; i < 6; i++) arp->target_mac[i] = 0;
    for (int i = 0; i < 4; i++) arp->target_ip[i] = target_ip[i];

    // Pad to minimum Ethernet frame size
    int total = sizeof(struct eth_header) + sizeof(struct arp_header);
    while (total < ETH_FRAME_MIN) frame[total++] = 0;

    e1000_send(frame, total);
}

static void handle_arp(uint8_t* data, uint32_t len) {
    serial_puts("[arp] packet received, len=");
    serial_putchar('0' + (len / 100));
    serial_putchar('0' + ((len / 10) % 10));
    serial_putchar('0' + (len % 10));
    serial_putchar('\n');

    struct arp_header* arp = (struct arp_header*)(data + sizeof(struct eth_header));

    uint16_t opcode = ((arp->opcode >> 8) & 0xFF) | ((arp->opcode & 0xFF) << 8);
    serial_puts("[arp] opcode=");
    serial_putchar('0' + opcode);
    serial_putchar('\n');

    if (opcode == 2) { // ARP Reply
        // Check if it's a reply for us
        if (arp->target_ip[0] == our_ip[0] && arp->target_ip[1] == our_ip[1] &&
            arp->target_ip[2] == our_ip[2] && arp->target_ip[3] == our_ip[3]) {
            // Cache the MAC (add new entry or update existing)
            int found = -1;
            for (int i = 0; i < arp_cache_count; i++) {
                if (arp_cache_ip[i][0] == arp->sender_ip[0] &&
                    arp_cache_ip[i][1] == arp->sender_ip[1] &&
                    arp_cache_ip[i][2] == arp->sender_ip[2] &&
                    arp_cache_ip[i][3] == arp->sender_ip[3]) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                if (arp_cache_count < ARP_CACHE_SIZE) found = arp_cache_count++;
                else found = ARP_CACHE_SIZE - 1; // Overwrite oldest
            }
            for (int i = 0; i < 6; i++) arp_cache_mac[found][i] = arp->sender_mac[i];
            for (int i = 0; i < 4; i++) arp_cache_ip[found][i] = arp->sender_ip[i];

            serial_puts("[arp] resolved: ");
            for (int i = 0; i < 6; i++) {
                serial_putchar(hex[arp_cache_mac[found][i] >> 4]);
                serial_putchar(hex[arp_cache_mac[found][i] & 0xF]);
                if (i < 5) serial_putchar(':');
            }
            serial_putchar('\n');
        }
    } else if (opcode == 1) { // ARP Request — reply if it's for us
        if (arp->target_ip[0] == our_ip[0] && arp->target_ip[1] == our_ip[1] &&
            arp->target_ip[2] == our_ip[2] && arp->target_ip[3] == our_ip[3]) {

            uint8_t frame[ETH_FRAME_MAX];
            uint8_t* mac = e1000_get_mac();

            struct eth_header* eth = (struct eth_header*)frame;
            build_eth_header(eth, arp->sender_mac, mac, 0x0806);

            struct arp_header* reply = (struct arp_header*)(frame + sizeof(struct eth_header));
            reply->hw_type = arp->hw_type;
            reply->proto_type = arp->proto_type;
            reply->hw_len = 6;
            reply->proto_len = 4;
            reply->opcode = ((2 >> 8) & 0xFF) | ((2 & 0xFF) << 8); // Reply

            for (int i = 0; i < 6; i++) reply->sender_mac[i] = mac[i];
            for (int i = 0; i < 4; i++) reply->sender_ip[i] = our_ip[i];
            for (int i = 0; i < 6; i++) reply->target_mac[i] = arp->sender_mac[i];
            for (int i = 0; i < 4; i++) reply->target_ip[i] = arp->sender_ip[i];

            int total = sizeof(struct eth_header) + sizeof(struct arp_header);
            while (total < ETH_FRAME_MIN) frame[total++] = 0;

            e1000_send(frame, total);
            serial_puts("[arp] replied to request\n");
        }
    }
}

static uint16_t ip_checksum(struct ip_header* ip) {
    ip->checksum = 0;
    return net_checksum(ip, sizeof(struct ip_header));
}

void icmp_send_ping(uint8_t* target_ip, uint16_t id, uint16_t seq) {
    // Try to resolve target MAC
    uint8_t target_mac[6];
    if (!arp_resolve(target_ip, target_mac)) {
        serial_puts("[icmp] MAC not resolved, sending ARP...\n");
        arp_send_request(target_ip);
        for (int i = 0; i < 4; i++) ping_target_ip[i] = target_ip[i];
        ping_id = id;
        ping_seq = seq;
        ping_pending = 1;
        return;
    }
    ping_pending = 0;

    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = e1000_get_mac();

    // Ethernet header
    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, target_mac, mac, 0x0800); // IPv4

    // IP header
    struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
    ip->version_ihl = 0x45; // IPv4, 5 words header
    ip->tos = 0;
    ip->total_length = ((sizeof(struct ip_header) + sizeof(struct icmp_header)) >> 8) |
                       ((sizeof(struct ip_header) + sizeof(struct icmp_header)) & 0xFF) << 8;
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = 1; // ICMP
    for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = target_ip[i];
    ip->checksum = ip_checksum(ip);

    // ICMP header
    struct icmp_header* icmp = (struct icmp_header*)(frame + sizeof(struct eth_header) + sizeof(struct ip_header));
    icmp->type = 8; // Echo request
    icmp->code = 0;
    icmp->id = ((id >> 8) & 0xFF) | ((id & 0xFF) << 8);
    icmp->seq = ((seq >> 8) & 0xFF) | ((seq & 0xFF) << 8);
    icmp->checksum = 0;
    icmp->checksum = net_checksum(icmp, sizeof(struct icmp_header));

    int total = sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct icmp_header);
    e1000_send(frame, total);
    serial_puts("[icmp] echo request sent to gateway\n");
}

void udp_send(uint8_t* dst_ip, uint16_t src_port, uint16_t dst_port, uint8_t* data, uint16_t len) {
    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = e1000_get_mac();

    uint8_t target_mac[6];
    if (!arp_resolve(dst_ip, target_mac)) {
        arp_send_request(dst_ip);
        return;
    }

    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, target_mac, mac, 0x0800);

    struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    uint16_t ip_total = sizeof(struct ip_header) + sizeof(struct udp_header) + len;
    ip->total_length = ((ip_total >> 8) & 0xFF) | ((ip_total & 0xFF) << 8);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = 17; // UDP
    for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = dst_ip[i];
    ip->checksum = ip_checksum(ip);

    struct udp_header* udp = (struct udp_header*)(frame + sizeof(struct ip_header));
    udp->src_port = ((src_port >> 8) & 0xFF) | ((src_port & 0xFF) << 8);
    udp->dst_port = ((dst_port >> 8) & 0xFF) | ((dst_port & 0xFF) << 8);
    uint16_t udp_len = sizeof(struct udp_header) + len;
    udp->length = ((udp_len >> 8) & 0xFF) | ((udp_len & 0xFF) << 8);
    udp->checksum = 0;

    for (int i = 0; i < len; i++) {
        frame[sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + i] = data[i];
    }

    int total = sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + len;
    e1000_send(frame, total);
}

// Encode a domain name into DNS format (labels)
static int dns_encode_name(const char* name, uint8_t* out) {
    int out_idx = 0;
    int label_start = 0;
    int i = 0;

    while (name[i]) {
        if (name[i] == '.') {
            int label_len = i - label_start;
            out[out_idx++] = label_len;
            for (int j = label_start; j < i; j++) {
                out[out_idx++] = name[j];
            }
            label_start = i + 1;
        }
        i++;
    }
    // Last label
    int label_len = i - label_start;
    if (label_len > 0) {
        out[out_idx++] = label_len;
        for (int j = label_start; j < i; j++) {
            out[out_idx++] = name[j];
        }
    }
    out[out_idx++] = 0; // Root label
    return out_idx;
}

// Parse DNS name from packet (handles compression pointers)
static int dns_decode_name(uint8_t* packet, int offset, char* out, int max_len) {
    int out_idx = 0;
    int jumped = 0;
    int original_offset = offset;
    int jump_count = 0;

    while (jump_count < 128) {
        uint8_t len = packet[offset];
        if (len == 0) {
            if (!jumped) original_offset = offset + 1;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            // Compression pointer
            if (!jumped) original_offset = offset + 2;
            offset = ((len & 0x3F) << 8) | packet[offset + 1];
            jumped = 1;
            jump_count++;
            continue;
        }
        offset++;
        for (int i = 0; i < len && out_idx < max_len - 1; i++) {
            out[out_idx++] = packet[offset++];
        }
        out[out_idx++] = '.';
        jump_count++;
    }
    out[out_idx > 0 ? out_idx - 1 : 0] = 0;
    if (!jumped) original_offset = offset + 1;
    return original_offset;
}

static void handle_dns_response(uint8_t* data, uint16_t len) {
    if (len < 12) return;

    uint16_t tx_id = ((data[0] << 8) | data[1]);
    uint16_t flags = ((data[2] << 8) | data[3]);
    uint16_t an_count = ((data[6] << 8) | data[7]);

    if (tx_id != dns_tx_id) return;
    if (!(flags & 0x8000)) return; // Not a response
    if (flags & 0x000F) { // RCODE != 0 (error)
        serial_puts("[dns] query error, rcode=");
        serial_putchar('0' + (flags & 0xF));
        serial_putchar('\n');
        dns_pending = 0;
        return;
    }

    serial_puts("[dns] response, answers=");
    serial_putchar('0' + an_count);
    serial_putchar('\n');

    // Skip question section
    int offset = 12;
    uint16_t qd_count = ((data[4] << 8) | data[5]);
    for (int i = 0; i < qd_count; i++) {
        char name[256];
        offset = dns_decode_name(data, offset, name, 256);
        offset += 4; // Skip QTYPE + QCLASS
    }

    // Parse answer section
    for (int i = 0; i < an_count && offset < len; i++) {
        char name[256];
        offset = dns_decode_name(data, offset, name, 256);
        uint16_t atype = ((data[offset] << 8) | data[offset + 1]);
        uint16_t aclass = ((data[offset + 2] << 8) | data[offset + 3]);
        offset += 4;
        uint32_t ttl = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                        ((uint32_t)data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        uint16_t rdlength = ((data[offset] << 8) | data[offset + 1]);
        offset += 2;

        if (atype == 1 && aclass == 1 && rdlength == 4) { // A record, IN class
            dns_resolved_ip = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                               ((uint32_t)data[offset + 2] << 8) | data[offset + 3];
            dns_resolved = 1;
            dns_pending = 0;

            serial_puts("[dns] resolved: ");
            serial_puts(net_event_msg + 10); // reuse net_event_msg after building it
            serial_putchar('\n');

            // Queue event for terminal
            for (int i = 0; i < 255; i++) net_event_msg[i] = 0;
            int idx = 0;
            const char* prefix = "Resolved: ";
            for (int i = 0; prefix[i]; i++) net_event_msg[idx++] = prefix[i];
            for (int octet = 0; octet < 4; octet++) {
                uint8_t v = (dns_resolved_ip >> (24 - octet * 8)) & 0xFF;
                char num[4]; int n = 0;
                if (v >= 100) num[n++] = '0' + v / 100;
                if (v >= 10) num[n++] = '0' + (v / 10) % 10;
                num[n++] = '0' + v % 10;
                for (int j = 0; j < n; j++) net_event_msg[idx++] = num[j];
                if (octet < 3) net_event_msg[idx++] = '.';
            }
            net_event_msg[idx++] = '\n';
            net_event_msg[idx] = 0;
            net_event_pending = 1;

            serial_puts("[dns] resolved: ");
            serial_puts(net_event_msg + 10); // skip "Resolved: "
            return;
        }
        offset += rdlength;
    }
    dns_pending = 0;
}

int dns_resolve(const char* hostname) {
    dns_resolved = 0;
    dns_pending = 1;
    dns_tx_id++;

    uint8_t dns_server_ip[4] = {10, 0, 2, 3}; // QEMU SLIRP DNS

    uint8_t target_mac[6];
    if (!arp_resolve(dns_server_ip, target_mac)) {
        arp_send_request(dns_server_ip);
        for (int i = 0; hostname[i] && i < 127; i++) dns_pending_host[i] = hostname[i];
        dns_pending_host[127] = 0;
        dns_retry_pending = 1;
        return -1; // Waiting for ARP
    }
    dns_retry_pending = 0;

    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = e1000_get_mac();

    // Build DNS query
    uint8_t dns_query[256];
    int dns_len = 0;

    // Header
    dns_query[0] = (dns_tx_id >> 8) & 0xFF; // Transaction ID
    dns_query[1] = dns_tx_id & 0xFF;
    dns_query[2] = 0x01; // Flags: standard query, recursion desired
    dns_query[3] = 0x00;
    dns_query[4] = 0x00; // Questions: 1
    dns_query[5] = 0x01;
    dns_query[6] = 0x00; // Answer RRs: 0
    dns_query[7] = 0x00;
    dns_query[8] = 0x00; // Authority RRs: 0
    dns_query[9] = 0x00;
    dns_query[10] = 0x00; // Additional RRs: 0
    dns_query[11] = 0x00;
    dns_len = 12;

    // Question: encoded name
    dns_len += dns_encode_name(hostname, dns_query + dns_len);

    // QTYPE = A (1), QCLASS = IN (1)
    dns_query[dns_len++] = 0x00;
    dns_query[dns_len++] = 0x01; // QTYPE A
    dns_query[dns_len++] = 0x00;
    dns_query[dns_len++] = 0x01; // QCLASS IN

    serial_puts("[dns] querying '");
    serial_puts(hostname);
    serial_puts("'...\n");

    // Wrap in UDP
    uint16_t src_port = 12345;
    uint16_t dst_port = 53;

    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, target_mac, mac, 0x0800);

    struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    uint16_t ip_total = sizeof(struct ip_header) + sizeof(struct udp_header) + dns_len;
    ip->total_length = ((ip_total >> 8) & 0xFF) | ((ip_total & 0xFF) << 8);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = 17; // UDP
    for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = dns_server_ip[i];
    ip->checksum = ip_checksum(ip);

    struct udp_header* udp = (struct udp_header*)(frame + sizeof(struct eth_header) + sizeof(struct ip_header));
    udp->src_port = ((src_port >> 8) & 0xFF) | ((src_port & 0xFF) << 8);
    udp->dst_port = ((dst_port >> 8) & 0xFF) | ((dst_port & 0xFF) << 8);
    uint16_t udp_len = sizeof(struct udp_header) + dns_len;
    udp->length = ((udp_len >> 8) & 0xFF) | ((udp_len & 0xFF) << 8);
    udp->checksum = 0;

    // Copy DNS query into UDP payload
    for (int i = 0; i < dns_len; i++) {
        frame[sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + i] = dns_query[i];
    }

    int total = sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + dns_len;
    e1000_send(frame, total);
    return 0;
}

int dns_is_resolved(uint32_t* ip) {
    if (dns_resolved) {
        *ip = dns_resolved_ip;
        return 1;
    }
    return 0;
}

int dns_is_pending(void) { return dns_pending; }

// TCP checksum (with pseudo-header) — uses big-endian reads for correctness
static uint16_t tcp_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t* tcp_data, int tcp_len) {
    // Pseudo-header
    uint8_t pseudo[12];
    for (int i = 0; i < 4; i++) { pseudo[i] = src_ip[i]; pseudo[i+4] = dst_ip[i]; }
    pseudo[8] = 0; pseudo[9] = 6; // Protocol: TCP
    pseudo[10] = (tcp_len >> 8) & 0xFF; pseudo[11] = tcp_len & 0xFF;

    uint32_t sum = 0;
    uint8_t* p = pseudo;
    for (int i = 0; i < 12; i += 2) sum += (p[i] << 8) | p[i+1];
    for (int i = 0; i < tcp_len; i += 2) {
        uint16_t val = (tcp_data[i] << 8);
        if (i + 1 < tcp_len) val |= tcp_data[i + 1];
        sum += val;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

void tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    // Store IP in network byte order
    tcp_conn.dst_ip[0] = (dst_ip >> 24) & 0xFF;
    tcp_conn.dst_ip[1] = (dst_ip >> 16) & 0xFF;
    tcp_conn.dst_ip[2] = (dst_ip >> 8) & 0xFF;
    tcp_conn.dst_ip[3] = dst_ip & 0xFF;
    tcp_conn.src_port = 43210;
    tcp_conn.dst_port = dst_port;
    tcp_conn.seq = 0x1000;
    tcp_conn.ack = 0;
    tcp_conn.state = TCP_STATE_CLOSED;
    // Retransmit state: SYN (if sent) occupies seq 0x1000
    tcp_conn.snd_una = 0x1000;
    tcp_conn.rtx_len = 0;
    tcp_conn.rtx_has_syn = 0;
    tcp_conn.rtx_has_fin = 0;
    tcp_conn.rtx_tries = 0;
    tcp_conn.rtx_timeout = TCP_RTO_TICKS;

    uint8_t dst_nbo[4]; // network byte order
    dst_nbo[0] = (dst_ip >> 24) & 0xFF;
    dst_nbo[1] = (dst_ip >> 16) & 0xFF;
    dst_nbo[2] = (dst_ip >> 8) & 0xFF;
    dst_nbo[3] = dst_ip & 0xFF;

    // For NAT (QEMU SLIRP), all external traffic goes through gateway
    uint8_t* arp_target = dst_nbo;
    uint8_t gw_ip[4];
    uint8_t* gw = net_get_gateway();
    for (int i = 0; i < 4; i++) gw_ip[i] = gw[i];

    // Check if target is on local network (same /24)
    if ((dst_nbo[0] & 0xF0) != (our_ip[0] & 0xF0) ||
        dst_nbo[1] != our_ip[1] || dst_nbo[2] != our_ip[2]) {
        arp_target = gw_ip; // Use gateway for external IPs
    }

    uint8_t target_mac[6];
    if (!arp_resolve(arp_target, target_mac)) {
        arp_send_request(arp_target);
        return;
    }

    // Build SYN packet
    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = e1000_get_mac();
    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, target_mac, mac, 0x0800);

    struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    uint16_t ip_total = sizeof(struct ip_header) + 20; // TCP header only (no options)
    ip->total_length = ((ip_total >> 8) & 0xFF) | ((ip_total & 0xFF) << 8);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = 6; // TCP
    for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = tcp_conn.dst_ip[i];
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip);

    uint8_t* tcp = frame + sizeof(struct eth_header) + sizeof(struct ip_header);
    tcp[0] = (tcp_conn.src_port >> 8) & 0xFF;
    tcp[1] = tcp_conn.src_port & 0xFF;
    tcp[2] = (dst_port >> 8) & 0xFF;
    tcp[3] = dst_port & 0xFF;
    tcp[4] = (tcp_conn.seq >> 24) & 0xFF;
    tcp[5] = (tcp_conn.seq >> 16) & 0xFF;
    tcp[6] = (tcp_conn.seq >> 8) & 0xFF;
    tcp[7] = tcp_conn.seq & 0xFF;
    tcp[8] = 0; tcp[9] = 0; tcp[10] = 0; tcp[11] = 0; // ACK number
    tcp[12] = 0x50; // Data offset: 5 words
    tcp[13] = 0x02; // Flags: SYN
    tcp[14] = 0x00; tcp[15] = 0x3C; // Window: 60
    tcp[16] = 0; tcp[17] = 0; // Checksum (set below)
    tcp[18] = 0; tcp[19] = 0; // Urgent pointer

    // Set checksum
    tcp[16] = 0; tcp[17] = 0;
    uint16_t cksum = tcp_checksum(our_ip, dst_nbo, tcp, 20);
    tcp[16] = (cksum >> 8) & 0xFF;
    tcp[17] = cksum & 0xFF;

    int total = sizeof(struct eth_header) + sizeof(struct ip_header) + 20;
    e1000_send(frame, total);
    tcp_conn.state = TCP_STATE_SYN_SENT;
    tcp_conn.rtx_has_syn = 1; // SYN is now in flight
    tcp_rtx_arm();
    tcp_conn.seq++; // SYN consumes 1 sequence number

    serial_puts("[tcp] SYN sent\n");
}

static void tcp_send_raw(uint32_t seq_num, uint32_t ack_num, uint8_t flags,
                         uint8_t* data, uint16_t data_len) {
    uint8_t target_mac[6];

    // Route through gateway for external IPs (same logic as tcp_connect)
    uint8_t* arp_target = tcp_conn.dst_ip;
    if ((tcp_conn.dst_ip[0] & 0xF0) != (our_ip[0] & 0xF0) ||
        tcp_conn.dst_ip[1] != our_ip[1] || tcp_conn.dst_ip[2] != our_ip[2]) {
        arp_target = net_get_gateway();
    }
    if (!arp_resolve(arp_target, target_mac)) {
        arp_send_request(arp_target);
        return;
    }

    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = e1000_get_mac();
    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, target_mac, mac, 0x0800);

    struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    uint16_t tcp_hdr_len = 20;
    uint16_t ip_total = sizeof(struct ip_header) + tcp_hdr_len + data_len;
    ip->total_length = ((ip_total >> 8) & 0xFF) | ((ip_total & 0xFF) << 8);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = 6;
    for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = tcp_conn.dst_ip[i];
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip);

    uint8_t* tcp = frame + sizeof(struct eth_header) + sizeof(struct ip_header);
    tcp[0] = (tcp_conn.src_port >> 8) & 0xFF;
    tcp[1] = tcp_conn.src_port & 0xFF;
    tcp[2] = (tcp_conn.dst_port >> 8) & 0xFF;
    tcp[3] = tcp_conn.dst_port & 0xFF;
    tcp[4] = (seq_num >> 24) & 0xFF;
    tcp[5] = (seq_num >> 16) & 0xFF;
    tcp[6] = (seq_num >> 8) & 0xFF;
    tcp[7] = seq_num & 0xFF;
    tcp[8] = (ack_num >> 24) & 0xFF;
    tcp[9] = (ack_num >> 16) & 0xFF;
    tcp[10] = (ack_num >> 8) & 0xFF;
    tcp[11] = ack_num & 0xFF;
    tcp[12] = 0x50;
    tcp[13] = flags;
    tcp[14] = 0x00; tcp[15] = 0x3C;
    tcp[16] = 0; tcp[17] = 0;
    tcp[18] = 0; tcp[19] = 0;

    for (int i = 0; i < data_len; i++) {
        tcp[tcp_hdr_len + i] = data[i];
    }

    tcp[16] = 0; tcp[17] = 0;
    uint16_t cksum = tcp_checksum(our_ip, tcp_conn.dst_ip, tcp, tcp_hdr_len + data_len);
    tcp[16] = (cksum >> 8) & 0xFF;
    tcp[17] = cksum & 0xFF;

    int total = sizeof(struct eth_header) + sizeof(struct ip_header) + tcp_hdr_len + data_len;
    e1000_send(frame, total);
}

static void tcp_send_packet(uint8_t flags, uint8_t* data, uint16_t data_len) {
    tcp_send_raw(tcp_conn.seq, tcp_conn.ack, flags, data, data_len);
}

// Cumulative ACK processing: drop acked bytes from the retransmit buffer,
// advance snd_una, reset backoff on progress.
static void tcp_process_ack(uint32_t ack_num) {
    if (!seq_lt(tcp_conn.snd_una, ack_num)) return; // old / duplicate ack

    uint32_t flight = (tcp_conn.rtx_has_syn ? 1u : 0u) +
                      tcp_conn.rtx_len +
                      (tcp_conn.rtx_has_fin ? 1u : 0u);
    uint32_t acked = ack_num - tcp_conn.snd_una;
    if (acked > flight) acked = flight; // clamp bogus acks

    if (tcp_conn.rtx_has_syn && acked > 0) {
        tcp_conn.rtx_has_syn = 0;
        tcp_conn.snd_una++;
        acked--;
    }
    if (acked > 0 && tcp_conn.rtx_len > 0) {
        uint32_t take = acked < tcp_conn.rtx_len ? acked : (uint32_t)tcp_conn.rtx_len;
        for (uint32_t i = 0; i < tcp_conn.rtx_len - take; i++)
            tcp_rtx_buf[i] = tcp_rtx_buf[i + take];
        tcp_conn.rtx_len -= take;
        acked -= take;
        tcp_conn.snd_una += take;
    }
    if (acked > 0 && tcp_conn.rtx_has_fin) {
        tcp_conn.rtx_has_fin = 0;
        tcp_conn.snd_una++;
    }

    // Progress: reset backoff, restart timer if anything remains in flight
    tcp_conn.rtx_tries = 0;
    tcp_conn.rtx_timeout = TCP_RTO_TICKS;
    if (tcp_conn.rtx_len > 0 || tcp_conn.rtx_has_syn || tcp_conn.rtx_has_fin)
        tcp_rtx_arm();
}

void tcp_send_data(uint8_t* data, uint16_t len) {
    if (tcp_conn.state != TCP_STATE_ESTABLISHED) return;
    // Buffer for retransmission (a second GET would mean a protocol bug —
    // log and drop old unacked data rather than corrupt the stream)
    if (tcp_conn.rtx_len + len > TCP_RTX_BUF_SIZE) {
        serial_puts("[tcp] rtx buffer overflow, resetting\n");
        tcp_conn.rtx_len = 0;
    }
    for (int i = 0; i < len; i++)
        tcp_rtx_buf[tcp_conn.rtx_len + i] = data[i];
    tcp_conn.rtx_len += len;

    tcp_send_packet(0x18, data, len); // PSH+ACK
    tcp_conn.seq += len;
    if (tcp_conn.rtx_tries == 0) tcp_rtx_arm();
}

void tcp_close(void) {
    if (tcp_conn.state == TCP_STATE_ESTABLISHED || tcp_conn.state == TCP_STATE_SYN_SENT) {
        tcp_send_packet(0x11, 0, 0); // FIN+ACK
        tcp_conn.rtx_has_fin = 1;
        tcp_conn.seq++;
        tcp_conn.state = TCP_STATE_FIN_WAIT;
        tcp_rtx_arm();
        serial_puts("[tcp] FIN sent\n");
    }
}

// Retransmission timer — call from the main loop. Fires when a flight
// (SYN, data, or FIN) goes unacked past the RTO; backs off exponentially
// and gives up after TCP_MAX_RTX attempts.
void tcp_poll(void) {
    if (tcp_conn.state == TCP_STATE_CLOSED &&
        !(tcp_conn.rtx_len || tcp_conn.rtx_has_syn || tcp_conn.rtx_has_fin))
        return;

    uint32_t flight = (tcp_conn.rtx_has_syn ? 1u : 0u) +
                      tcp_conn.rtx_len +
                      (tcp_conn.rtx_has_fin ? 1u : 0u);
    if (flight == 0) return;
    if ((uint32_t)(tick_count - tcp_conn.rtx_last_tick) < tcp_conn.rtx_timeout) return;

    if (tcp_conn.rtx_tries >= TCP_MAX_RTX) {
        serial_puts("[tcp] retransmit give-up, closing connection\n");
        tcp_conn.state = TCP_STATE_CLOSED;
        tcp_conn.rtx_len = 0;
        tcp_conn.rtx_has_syn = 0;
        tcp_conn.rtx_has_fin = 0;
        // Cancel the pending retry too — otherwise net_poll reconnects
        // forever (observed: SYN -> 6 rtx -> give-up -> SYN -> ... loop)
        http_retry_pending = 0;
        if (http_pending) {
            http_pending = 0;
            http_done = 0;
            for (int i = 0; i < 255; i++) net_event_msg[i] = 0;
            int idx = 0;
            const char* msg = "Connection timed out.\n";
            for (int i = 0; msg[i]; i++) net_event_msg[idx++] = msg[i];
            net_event_msg[idx] = 0;
            net_event_pending = 1;
        }
        return;
    }

    tcp_conn.rtx_tries++;
    if (tcp_conn.rtx_tries > 1) tcp_conn.rtx_timeout *= 2;
    tcp_rtx_arm();

    serial_puts("[tcp] retransmit #");
    serial_putchar('0' + tcp_conn.rtx_tries);
    serial_putchar('\n');

    if (tcp_conn.rtx_has_syn) {
        // SYN still unacked: resend it alone (seq = snd_una, no ACK flag)
        tcp_send_raw(tcp_conn.snd_una, 0, 0x02, 0, 0);
    } else {
        uint8_t flags = 0x10; // ACK
        if (tcp_conn.rtx_len > 0) flags |= 0x08; // PSH with data
        else if (tcp_conn.rtx_has_fin) flags |= 0x01; // bare FIN
        tcp_send_raw(tcp_conn.snd_una, tcp_conn.ack, flags,
                     tcp_rtx_buf, tcp_conn.rtx_len);
    }
}

void tcp_handle_packet(uint8_t* data, uint32_t len) {
    if (len < 20) return;
    uint16_t src_port = ((data[0] << 8) | data[1]);
    uint16_t dst_port = ((data[2] << 8) | data[3]);
    uint32_t seq_num = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                        ((uint32_t)data[6] << 8) | data[7];
    uint32_t ack_num = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) |
                        ((uint32_t)data[10] << 8) | data[11];
    uint8_t flags = data[13];
    uint8_t data_offset = (data[12] >> 4) * 4;
    uint16_t payload_len = len - data_offset;

    serial_puts("[tcp] pkt src=");
    serial_putchar(hex[(src_port >> 8) & 0xF]);
    serial_putchar(hex[src_port & 0xF]);
    serial_puts(" dst=");
    serial_putchar(hex[(dst_port >> 8) & 0xF]);
    serial_putchar(hex[dst_port & 0xF]);
    serial_puts(" flags=");
    serial_putchar(hex[(flags >> 4) & 0xF]);
    serial_putchar(hex[flags & 0xF]);
    serial_puts(" state=");
    serial_putchar('0' + tcp_conn.state);
    serial_putchar('\n');

    if (dst_port != tcp_conn.src_port) return;

    serial_puts("[tcp] pkt flags=");
    serial_putchar(hex[(flags >> 4) & 0xF]);
    serial_putchar(hex[flags & 0xF]);
    serial_puts(" state=");
    serial_putchar('0' + tcp_conn.state);
    serial_putchar('\n');

    if (tcp_conn.state == TCP_STATE_CLOSED) {
        // Late ACK (e.g. for our FIN after the server closed first) —
        // still clears the retransmit flight
        if (flags & 0x10) tcp_process_ack(ack_num);
        return;
    }

    if (tcp_conn.state == TCP_STATE_SYN_SENT) {
        if (flags & 0x12) { // SYN+ACK
            tcp_conn.ack = seq_num + 1;
            tcp_conn.seq = ack_num; // Sync our seq
            tcp_conn.state = TCP_STATE_ESTABLISHED;
            tcp_process_ack(ack_num); // clears in-flight SYN
            tcp_send_packet(0x10, 0, 0); // ACK
            serial_puts("[tcp] ESTABLISHED\n");
            for (int i = 0; i < 255; i++) net_event_msg[i] = 0;
            int idx = 0;
            const char* msg = "TCP connected, sending HTTP request...\n";
            for (int i = 0; msg[i]; i++) net_event_msg[idx++] = msg[i];
            net_event_msg[idx] = 0;
            net_event_pending = 1;
        }
    } else if (tcp_conn.state == TCP_STATE_ESTABLISHED) {
        // Cumulative ACK for our in-flight data/FIN (usually piggybacked)
        if (flags & 0x10) tcp_process_ack(ack_num);

        // In-order delivery check. Duplicates (seq < RCV.NXT) arrive when
        // OUR ack of their data was lost; gaps (seq > RCV.NXT) mean an
        // earlier segment went missing. Either way: re-ACK what we have
        // and drop the payload (no out-of-order buffering in this stack).
        if (payload_len > 0 && seq_num != tcp_conn.ack) {
            serial_puts("[tcp] dup/out-of-order seq, re-ACKing\n");
            tcp_send_packet(0x10, 0, 0);
            payload_len = 0; // don't double-append below
        }

        // Payload BEFORE FIN: servers may piggyback the last data bytes on
        // the FIN packet — checking FIN first would drop that payload
        if (payload_len > 0 && http_pending) {
            tcp_conn.ack = seq_num + payload_len;
            tcp_send_packet(0x10, 0, 0); // ACK

            int copy = payload_len;
            if (copy > 1500) copy = 1500;
            if (http_response_len + copy > 4095) copy = 4095 - http_response_len;
            for (int i = 0; i < copy; i++) {
                http_response[http_response_len + i] = data[data_offset + i];
            }
            http_response_len += copy;
            http_response[http_response_len] = 0;

            serial_puts("[http] segment, total ");
            serial_putchar('0' + (http_response_len / 1000) % 10);
            serial_putchar('0' + (http_response_len / 100) % 10);
            serial_putchar('0' + (http_response_len / 10) % 10);
            serial_putchar('0' + (http_response_len % 10));
            serial_puts(" bytes\n");

            // Terminal preview from the FIRST segment only (headers visible
            // there by design; the browser strips them via html_parse)
            if (http_response_len == copy) {
                for (int i = 0; i < 255; i++) net_event_msg[i] = 0;
                int idx = 0;
                const char* hdr = "HTTP response (";
                for (int i = 0; hdr[i]; i++) net_event_msg[idx++] = hdr[i];
                net_event_msg[idx++] = '0' + (http_response_len / 100);
                net_event_msg[idx++] = '0' + (http_response_len / 10) % 10;
                net_event_msg[idx++] = '0' + (http_response_len % 10);
                const char* hdr2 = " bytes):\n";
                for (int i = 0; hdr2[i]; i++) net_event_msg[idx++] = hdr2[i];
                int disp_len = http_response_len < 180 ? http_response_len : 180;
                for (int i = 0; i < disp_len && idx < 254; i++) {
                    net_event_msg[idx++] = http_response[i];
                }
                if (http_response_len > 180) {
                    const char* dots = "\n...(truncated)";
                    for (int i = 0; dots[i] && idx < 254; i++) net_event_msg[idx++] = dots[i];
                }
                net_event_msg[idx++] = '\n';
                net_event_msg[idx] = 0;
                net_event_pending = 1;
            }
        }
        if (flags & 0x01) { // FIN
            tcp_conn.ack = seq_num + payload_len + 1;
            tcp_send_packet(0x10, 0, 0); // ACK their FIN
            tcp_send_packet(0x11, 0, 0); // FIN+ACK (our side closes too)
            tcp_conn.rtx_has_fin = 1;    // track it until ACKed
            tcp_conn.rtx_tries = 0;
            tcp_conn.rtx_timeout = TCP_RTO_TICKS;
            tcp_rtx_arm();
            tcp_conn.seq++;
            tcp_conn.state = TCP_STATE_FIN_WAIT;
            serial_puts("[tcp] FIN_WAIT (server FIN)\n");
            return;
        }
    } else if (tcp_conn.state == TCP_STATE_FIN_WAIT) {
        if (flags & 0x10) { // ACK of our FIN
            tcp_process_ack(ack_num); // clears rtx_has_fin
            tcp_conn.state = TCP_STATE_CLOSED;
            serial_puts("[tcp] CLOSED (FIN ACKed)\n");
        }
        // Retransmitted server FIN (our ACK was lost) — re-ACK it
        if (flags & 0x01) {
            tcp_send_packet(0x10, 0, 0);
        }
    }
}

void http_get(const char* host, const char* path) {
    // Every request starts a fresh response lifecycle. Without this, a
    // second fetch (e.g. browser refresh) raced the parse block with the
    // PREVIOUS response's stale http_done=1 + buffer: an immediate bogus
    // parse ran, double-dechunked the buffer to garbage, and the sentinel
    // then blocked the real response from ever rendering.
    http_response_len = 0;
    http_done = 0;

    // Resolve hostname first
    uint32_t ip;
    if (dns_is_resolved(&ip)) {
        // Already resolved, connect directly
    } else if (dns_is_pending()) {
        // Save for retry after DNS completes
        for (int i = 0; host[i] && i < 127; i++) http_pending_host[i] = host[i];
        http_pending_host[127] = 0;
        for (int i = 0; path[i] && i < 127; i++) http_pending_path[i] = path[i];
        http_pending_path[127] = 0;
        http_retry_pending = 1;
        return;
    } else {
        dns_resolve(host);
        for (int i = 0; host[i] && i < 127; i++) http_pending_host[i] = host[i];
        http_pending_host[127] = 0;
        for (int i = 0; path[i] && i < 127; i++) http_pending_path[i] = path[i];
        http_pending_path[127] = 0;
        http_retry_pending = 1;
        return;
    }

    // Start TCP connection if not connected
    if (tcp_conn.state == TCP_STATE_CLOSED) {
        // Save for retry after TCP handshake completes
        for (int i = 0; host[i] && i < 127; i++) http_pending_host[i] = host[i];
        http_pending_host[127] = 0;
        for (int i = 0; path[i] && i < 127; i++) http_pending_path[i] = path[i];
        http_pending_path[127] = 0;
        http_retry_pending = 1;
        tcp_connect(ip, 80);
        return;
    }
    if (tcp_conn.state == TCP_STATE_SYN_SENT) {
        serial_puts("[http] waiting for TCP handshake...\n");
        return;
    }

    // Build HTTP GET request
    uint8_t req_buf[512];
    int req_len = 0;

    const char* method = "GET ";
    for (int i = 0; method[i]; i++) req_buf[req_len++] = method[i];
    for (int i = 0; path[i]; i++) req_buf[req_len++] = path[i];
    req_buf[req_len++] = ' ';
    req_buf[req_len++] = 'H';
    req_buf[req_len++] = 'T';
    req_buf[req_len++] = 'T';
    req_buf[req_len++] = 'P';
    req_buf[req_len++] = '/';
    req_buf[req_len++] = '1';
    req_buf[req_len++] = '.';
    req_buf[req_len++] = '1';
    req_buf[req_len++] = '\r';
    req_buf[req_len++] = '\n';

    const char* hdr_host = "Host: ";
    for (int i = 0; hdr_host[i]; i++) req_buf[req_len++] = hdr_host[i];
    for (int i = 0; host[i]; i++) req_buf[req_len++] = host[i];
    req_buf[req_len++] = '\r';
    req_buf[req_len++] = '\n';

    const char* hdr_end = "Connection: close\r\n\r\n";
    for (int i = 0; hdr_end[i]; i++) req_buf[req_len++] = hdr_end[i];

    serial_puts("[http] sending GET ");
    serial_puts(path);
    serial_puts(" to ");
    serial_puts(host);
    serial_puts("\n");

    // Arm the RX accumulator BEFORE the request hits the wire — SLIRP is
    // in-process and the response IRQ can land before this function returns
    if (!http_pending) {
        http_response_len = 0;
        http_done = 0;
    }
    http_pending = 1;
    tcp_send_data(req_buf, req_len);
}

void http_poll(void) {
    if (!http_pending) return;
    // Response bytes are accumulated in tcp_handle_packet (IRQ); here we
    // only watch for connection close to flag the response as complete
    if (tcp_conn.state == TCP_STATE_CLOSED && http_pending) {
        http_pending = 0;
        if (http_response_len > 0) http_done = 1;
        serial_puts("[http] connection closed\n");
    }
}

char* http_get_response(void) { return http_response; }

// Decode chunked transfer encoding in place. QEMU SLIRP forwards the
// server's framing verbatim, so a "Transfer-Encoding: chunked" response
// arrives as "N\r\n<data>\r\n0\r\n\r\n" — html_parse would render the hex
// size lines as text. No-op (returns len) when the header isn't present.
int http_dechunk(char* buf, int len) {
    // locate end of headers
    int body = -1;
    for (int i = 0; i < len - 3; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            body = i + 4;
            break;
        }
    }
    if (body < 0) return len;

    // only dechunk when the server actually said chunked
    int chunked = 0;
    for (int i = 0; i < body - 7; i++) {
        if (buf[i] == 'c' && buf[i+1] == 'h' && buf[i+2] == 'u' &&
            buf[i+3] == 'n' && buf[i+4] == 'k' && buf[i+5] == 'e' &&
            buf[i+6] == 'd') { chunked = 1; break; }
    }
    if (!chunked) return len;

    int rd = body, wr = body;
    int first_chunk = 1;
    while (rd < len) {
        // parse hex chunk size
        int size = 0, digits = 0;
        while (rd < len) {
            char c = buf[rd];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            size = size * 16 + v;
            digits++;
            rd++;
        }
        // Idempotence: if the FIRST size line isn't hex, this buffer was
        // already dechunked (or isn't chunked despite the header) —
        // returning `body` here would truncate it to headers-only
        if (!digits && first_chunk) return len;
        if (!digits) break;
        first_chunk = 0;
        while (rd < len && buf[rd] != '\n') rd++; // rest of size line
        if (rd < len) rd++;
        if (size == 0) break; // terminal chunk
        for (int i = 0; i < size && rd < len; i++) buf[wr++] = buf[rd++];
        // exactly one CRLF after each chunk (never skip more — chunk data
        // may legitimately end with \r\n bytes)
        if (rd < len && buf[rd] == '\r') rd++;
        if (rd < len && buf[rd] == '\n') rd++;
    }
    buf[wr] = 0;
    return wr;
}
int http_get_response_len(void) { return http_response_len; }
int http_is_pending(void) { return http_pending; }
int http_is_done(void) { return http_done; }

void net_set_event_callback(void (*cb)(const char* msg)) {
    net_event_callback = cb;
}

// Retry pending operations (called from main loop)
void net_poll(void) {
    // Retransmission timer (SYN/data/FIN) — runs every main-loop pass
    tcp_poll();

    // Dispatch pending network events to terminal
    if (net_event_pending && net_event_callback) {
        net_event_callback(net_event_msg);
        net_event_pending = 0;
    }

    // Retry ping after ARP resolves
    if (ping_pending) {
        uint8_t target_mac[6];
        if (arp_resolve(ping_target_ip, target_mac)) {
            serial_puts("[icmp] ARP resolved, sending ping\n");
            ping_pending = 0;
            // Build and send the ICMP echo request
            uint8_t frame[ETH_FRAME_MAX];
            uint8_t* mac = e1000_get_mac();
            struct eth_header* eth = (struct eth_header*)frame;
            build_eth_header(eth, target_mac, mac, 0x0800);
            struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
            ip->version_ihl = 0x45;
            ip->tos = 0;
            uint16_t ip_total = sizeof(struct ip_header) + sizeof(struct icmp_header);
            ip->total_length = ((ip_total >> 8) & 0xFF) | ((ip_total & 0xFF) << 8);
            ip->id = 0;
            ip->flags_frag = 0;
            ip->ttl = 64;
            ip->protocol = 1;
            for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
            for (int i = 0; i < 4; i++) ip->dst_ip[i] = ping_target_ip[i];
            ip->checksum = ip_checksum(ip);
            struct icmp_header* icmp = (struct icmp_header*)(frame + sizeof(struct eth_header) + sizeof(struct ip_header));
            icmp->type = 8;
            icmp->code = 0;
            icmp->id = ((ping_id >> 8) & 0xFF) | ((ping_id & 0xFF) << 8);
            icmp->seq = ((ping_seq >> 8) & 0xFF) | ((ping_seq & 0xFF) << 8);
            icmp->checksum = 0;
            icmp->checksum = net_checksum(icmp, sizeof(struct icmp_header));
            int total = sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct icmp_header);
            e1000_send(frame, total);
        }
    }

    // Retry DNS query after ARP for DNS server resolves
    if (dns_retry_pending) {
        uint8_t dns_server_ip[4] = {10, 0, 2, 3};
        uint8_t target_mac[6];
        if (arp_resolve(dns_server_ip, target_mac)) {
            serial_puts("[dns] ARP resolved, sending query for '");
            serial_puts(dns_pending_host);
            serial_puts("'\n");
            dns_retry_pending = 0;
            dns_resolve(dns_pending_host);
        }
    }

    // Retry HTTP after DNS resolves
    if (http_retry_pending) {
        uint32_t ip;
        if (dns_is_resolved(&ip)) {
            if (tcp_conn.state == TCP_STATE_CLOSED) {
                http_retry_pending = 0;
                http_get(http_pending_host, http_pending_path);
            } else if (tcp_conn.state == TCP_STATE_ESTABLISHED) {
                http_retry_pending = 0;
                http_get(http_pending_host, http_pending_path);
            }
        }
    }
}

static void handle_ip(uint8_t* data, uint32_t len) {
    struct ip_header* ip = (struct ip_header*)(data + sizeof(struct eth_header));
    uint8_t protocol = ip->protocol;

    if (protocol == 1) { // ICMP
        struct icmp_header* icmp = (struct icmp_header*)(data + sizeof(struct eth_header) + sizeof(struct ip_header));
        if (icmp->type == 8) { // Echo request — reply
            // Build echo reply
            icmp->type = 0;
            icmp->checksum = 0;
            icmp->checksum = net_checksum(icmp, sizeof(struct icmp_header));

            // Swap src/dst IP
            uint8_t tmp[4];
            for (int i = 0; i < 4; i++) { tmp[i] = ip->src_ip[i]; ip->src_ip[i] = ip->dst_ip[i]; ip->dst_ip[i] = tmp[i]; }
            ip->checksum = 0;
            ip->checksum = ip_checksum(ip);

            // Get target MAC from ARP cache or just use broadcast
            uint8_t target_mac[6];
            arp_resolve(ip->dst_ip, target_mac);

            uint8_t frame[ETH_FRAME_MAX];
            uint8_t* mac = e1000_get_mac();
            struct eth_header* eth = (struct eth_header*)frame;
            build_eth_header(eth, target_mac, mac, 0x0800);

            int total = sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct icmp_header);
            for (int i = 0; i < total; i++) {
                frame[i] = data[i];
            }
            // Fix the src/dst in the new frame
            for (int i = 0; i < 4; i++) {
                ((struct ip_header*)(frame + sizeof(struct eth_header)))->src_ip[i] = our_ip[i];
                ((struct ip_header*)(frame + sizeof(struct eth_header)))->dst_ip[i] = ip->dst_ip[i];
            }

            e1000_send(frame, total);
            serial_puts("[icmp] replied to ping\n");
        } else if (icmp->type == 0) { // Echo reply
            serial_puts("[icmp] got ping reply!\n");
            for (int i = 0; i < 255; i++) net_event_msg[i] = 0;
            int idx = 0;
            const char* prefix = "Reply from ";
            for (int i = 0; prefix[i]; i++) net_event_msg[idx++] = prefix[i];
            for (int i = 0; i < 4; i++) {
                uint8_t v = ip->src_ip[i];
                if (v >= 100) net_event_msg[idx++] = '0' + v/100;
                if (v >= 10) net_event_msg[idx++] = '0' + (v/10)%10;
                net_event_msg[idx++] = '0' + v%10;
                if (i < 3) net_event_msg[idx++] = '.';
            }
            net_event_msg[idx++] = ':';
            net_event_msg[idx++] = ' ';
            net_event_msg[idx++] = 'O';
            net_event_msg[idx++] = 'K';
            net_event_msg[idx++] = '\n';
            net_event_msg[idx] = 0;
            net_event_pending = 1;
        }
    } else if (protocol == 17) { // UDP
        struct udp_header* udp = (struct udp_header*)(data + sizeof(struct eth_header) + sizeof(struct ip_header));
        uint16_t dst_port = ((udp->dst_port >> 8) & 0xFF) | ((udp->dst_port & 0xFF) << 8);
        uint16_t src_port = ((udp->src_port >> 8) & 0xFF) | ((udp->src_port & 0xFF) << 8);
        uint16_t udp_len = ((udp->length >> 8) & 0xFF) | ((udp->length & 0xFF) << 8);

        if (src_port == 53 && udp_len > 8) { // DNS response (server replies from port 53)
            uint8_t* dns_data = data + sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header);
            uint16_t dns_len = udp_len - 8;
            handle_dns_response(dns_data, dns_len);
        }

        if (udp_callback && dst_port == udp_callback_port) {
            udp_callback(data, len, src_port, dst_port);
        }
    } else if (protocol == 6) { // TCP
        uint8_t* tcp_data = data + sizeof(struct eth_header) + sizeof(struct ip_header);
        uint32_t tcp_len = len - sizeof(struct eth_header) - sizeof(struct ip_header);
        tcp_handle_packet(tcp_data, tcp_len);
    }
}

static void handle_packet(uint8_t* data, uint32_t len) {
    // data IS the full Ethernet frame (RTL8139 strips 4-byte header)
    if (len < sizeof(struct eth_header)) return;

    uint16_t eth_type = ((data[12] << 8) | data[13]);

    if (eth_type == 0x0806) { // ARP
        handle_arp(data, len);
    } else if (eth_type == 0x0800) { // IPv4
        handle_ip(data, len);
    }
}

void net_init(void) {
    serial_puts("[net] net_init start\n");
    e1000_init();
    serial_puts("[net] net_init done\n");
    e1000_set_rx_callback(handle_packet);

    serial_puts("[net] IP: ");
    for (int i = 0; i < 4; i++) {
        serial_putchar('0' + (our_ip[i] / 10));
        serial_putchar('0' + (our_ip[i] % 10));
        if (i < 3) serial_putchar('.');
    }
    serial_putchar('\n');

    // Send ARP request for gateway to learn its MAC
    arp_send_request(gateway_ip);
}

uint8_t* net_get_ip(void) { return our_ip; }
uint8_t* net_get_gateway(void) { return gateway_ip; }

void net_set_ip(uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3) {
    our_ip[0] = ip0; our_ip[1] = ip1;
    our_ip[2] = ip2; our_ip[3] = ip3;
}

int arp_resolve(uint8_t* ip, uint8_t* mac) {
    for (int i = 0; i < arp_cache_count; i++) {
        if (arp_cache_ip[i][0] == ip[0] && arp_cache_ip[i][1] == ip[1] &&
            arp_cache_ip[i][2] == ip[2] && arp_cache_ip[i][3] == ip[3]) {
            for (int j = 0; j < 6; j++) mac[j] = arp_cache_mac[i][j];
            return 1;
        }
    }
    // Broadcast MAC as fallback
    for (int i = 0; i < 6; i++) mac[i] = 0xFF;
    return 0;
}
