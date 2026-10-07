#include "network.h"
#include "../string.h"
#include "../memory.h"
#include "../crypto/tls_client.h" // http_parse_framing (shared framing)
#include "e1000.h"
#include "../io.h"
#include "../serial.h"

static uint8_t our_ip[4] = {10, 0, 2, 15};    // QEMU user-mode default
static uint8_t gateway_ip[4] = {10, 0, 2, 2};  // QEMU user-mode gateway
static uint8_t dns_server_ip[4] = {10, 0, 2, 3}; // QEMU SLIRP DNS
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

// Pending operation state (for retry after ARP resolves)
static int ping_pending = 0;
static uint8_t ping_target_ip[4];
static uint16_t ping_id = 0;
static uint16_t ping_seq = 0;

// Copy a host string, always NUL-terminating at the ACTUAL length.
// (A copy loop + dst[127]=0 once left the previous, longer value's tail in
// place: example.com then iana.org gave "iana.orgcom" and the lookup never
// matched — the "clicked link opens a black window" bug.)
static void net_copy_str(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < 127) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

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

extern uint32_t tick_count;   // 100 Hz PIT

// Sequence comparison with 32-bit wraparound (RFC 793 ordering)
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

// ---- TCP socket table ---------------------------------------------------------------
// Every socket: retransmit flight (our unacked bytes), reorder buffer
// (out-of-order segments held until the hole fills), receive ring (in-order
// bytes the application has not read yet). All buffers are heap blocks
// allocated the first time a slot is used and kept for reuse.
#define TCP_RTX_CAP     16384          // unacked outbound bytes (requests, TLS flights)
#define TCP_RX_CAP      (256 * 1024)   // receive ring
#define TCP_REORDER_SLOTS 8
#define TCP_REORDER_SEG 1500
#define TCP_WIN_MAX     65535          // no window scaling: 16-bit window

struct tcp_sock {
    uint8_t used;
    uint8_t dst_ip[4];       // network byte order
    uint16_t src_port, dst_port;
    int state;
    uint32_t seq;            // SND.NXT
    uint32_t ack;            // RCV.NXT
    uint32_t snd_una;        // oldest unacked sequence number
    // Retransmit flight
    uint8_t* rtx;
    uint32_t rtx_len;
    uint8_t rtx_has_syn, rtx_has_fin, rtx_tries;
    uint16_t rtx_timeout;    // RTO in ticks
    uint32_t rtx_last_tick;
    // Peer window (caps our sends; 0 = persist probing)
    uint32_t peer_win, persist_since;
    // Fast retransmit (pure duplicate ACKs only — see tcp_process_ack)
    uint32_t dup_acks, last_ack_num;
    // RTT estimation (Jacobson/Karels, ticks)
    uint32_t srtt, rttvar, rtt_seq, rtt_tick;
    // Early FIN: arrived while a receive hole was open (see tcp_handle_packet)
    uint8_t fin_pending;
    uint32_t fin_seq;
    // Receive ring
    uint8_t* rx;
    uint32_t rx_head, rx_tail, rx_len;
    uint32_t adv_win;        // window in the last segment we sent
    uint8_t ack_pending;     // in-order data arrived: ACK at the next flush
    uint8_t peer_closed;     // FIN received: EOF once the ring drains
    uint8_t failed;          // RST or retransmit give-up
    uint8_t app_closed;      // owner let go: free the slot once CLOSED
    uint32_t close_tick;     // when app_closed was set (force-free bound)
    // Reorder buffer
    uint8_t* ro_data;        // TCP_REORDER_SLOTS * TCP_REORDER_SEG
    uint32_t ro_seq[TCP_REORDER_SLOTS];
    uint16_t ro_len[TCP_REORDER_SLOTS];
    uint8_t ro_used[TCP_REORDER_SLOTS];
};

static struct tcp_sock tcp_socks[TCP_MAX_SOCKS];

// Ephemeral source port, advanced on every connect so no two connections
// share a 4-tuple — QEMU's SLIRP NAT keeps a closed connection's mapping
// around and would route a reused tuple's SYN-ACK to the dead one.
static uint16_t tcp_ephemeral_port = 43210;

// Initial RTO 250ms (SLIRP answers the SYN only once the host-side connect
// completes — a real Internet round trip); data RTOs come from the RTT
// estimator, floored at 200ms (Linux's minimum).
#define TCP_RTO_SYN   25
#define TCP_RTO_MIN   20
#define TCP_RTO_MAX   300
#define TCP_MAX_RTX   6
// Zero-window persist ceiling: a shut window must fail the fetch eventually.
#define TCP_PERSIST_TIMEOUT 3000
// A closed-by-owner socket still in FIN_WAIT after this is freed anyway.
#define TCP_CLOSE_LINGER 500

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

    struct udp_header* udp = (struct udp_header*)(frame + sizeof(struct eth_header) + sizeof(struct ip_header));
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
static int dns_decode_name(uint8_t* packet, int packet_len, int offset,
                           char* out, int max_len) {
    // Fully bounds-checked (every byte verified against packet_len):
    // a malicious/truncated response can neither over-read the packet
    // nor overflow `out`. Returns the offset past the name (or past the
    // 2-byte pointer) or -1 on any malformed input. Compression chains
    // and label counts are capped; output is always NUL-terminated.
    int out_idx = 0;
    int jumped = 0;
    int original_offset = offset;
    int jump_count = 0;
    if (max_len <= 0) return -1;
    out[0] = 0;
    while (jump_count < 128) {
        if (offset < 0 || offset >= packet_len) return -1;
        uint8_t len = packet[offset];
        if (len == 0) {
            if (!jumped) original_offset = offset + 1;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            // Compression pointer
            if (offset + 1 >= packet_len) return -1;
            int target = ((len & 0x3F) << 8) | packet[offset + 1];
            if (target < 0 || target >= packet_len) return -1;
            if (!jumped) original_offset = offset + 2;
            offset = target;
            jumped = 1;
            jump_count++;
            continue;
        }
        if (len & 0xC0) return -1; // reserved label bits (RFC 1035)
        offset++;
        if ((uint32_t)len > (uint32_t)(packet_len - offset)) return -1;
        for (int i = 0; i < len && out_idx < max_len - 1; i++) {
            out[out_idx++] = (char)packet[offset++];
        }
        // Separator — or fail closed when the name doesn't fit the output
        // (the old code kept writing past `out` here: stack smash).
        if (out_idx >= max_len - 1) {
            if (max_len > 0) out[max_len - 1] = 0;
            return -1;
        }
        out[out_idx++] = '.';
        jump_count++;
    }
    if (jump_count >= 128) return -1; // pointer/label loop
    out[out_idx > 0 ? out_idx - 1 : 0] = 0;
    if (!jumped) original_offset = offset + 1;
    return original_offset;
}

// Case-insensitive hostname comparison (hostnames are case-insensitive).
static int dns_host_matches(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == b[i];
}

// Parse a strict dotted-quad ("10.0.2.2") into host order (first octet in
// the high byte). Returns 1 on success.
int net_parse_ip(const char* s, uint32_t* out) {
    uint32_t ip = 0;
    for (int oct = 0; oct < 4; oct++) {
        int val = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) { val = val * 10 + (*s - '0'); s++; digits++; }
        if (!digits || val > 255) return 0;
        ip = (ip << 8) | (uint32_t)val;
        if (oct < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s) return 0; // trailing junk — it's a hostname, not an IP
    *out = ip;
    return 1;
}

// ---- DNS cache + concurrent resolver ----
// One entry per host: PENDING (query on the wire or waiting for ARP),
// OK (address until expiry) or FAILED (negative-cached briefly). Queries
// are matched by transaction id, so several lookups run at once — the
// old single-slot resolver made parallel fetches to different hosts
// supersede each other's queries.
#define DNS_SLOTS 32
#define DNS_ST_FREE    0
#define DNS_ST_PENDING 1
#define DNS_ST_OK      2
#define DNS_ST_FAILED  3
#define DNS_RETRY_TICKS 200      // resend an unanswered query after 2s
#define DNS_MAX_TRIES   4        // ~8s before a lookup is given up
#define DNS_NEG_TICKS   1000     // remember a failure for 10s
struct dns_ent {
    char host[128];
    uint32_t ip;
    uint8_t st, tries, notify;
    uint16_t txid;
    uint32_t sent_tick;          // 0 = not on the wire yet (waiting for ARP)
    uint32_t expire_tick;
    uint32_t used_tick;
};
static struct dns_ent dns_tab[DNS_SLOTS];
static uint16_t dns_txid = 0x1234;

static int dns_expired(const struct dns_ent* e) {
    return (int32_t)(tick_count - e->expire_tick) >= 0;
}

static void dns_notify(struct dns_ent* e) {
    if (!e->notify) return;
    e->notify = 0;
    char msg[64];
    int n = 0;
    const char* pre = e->st == DNS_ST_OK ? "Resolved: " : "DNS lookup failed";
    while (*pre) msg[n++] = *pre++;
    if (e->st == DNS_ST_OK) {
        for (int octet = 0; octet < 4; octet++) {
            uint8_t v = (uint8_t)(e->ip >> (24 - octet * 8));
            if (v >= 100) msg[n++] = (char)('0' + v / 100);
            if (v >= 10) msg[n++] = (char)('0' + (v / 10) % 10);
            msg[n++] = (char)('0' + v % 10);
            if (octet < 3) msg[n++] = '.';
        }
    }
    msg[n] = 0;
    net_event_post(msg);
}

static void dns_send_query(struct dns_ent* e) {
    uint8_t target_mac[6];
    if (!arp_resolve(dns_server_ip, target_mac)) {
        arp_send_request(dns_server_ip);
        e->sent_tick = 0;              // net_poll sends once ARP resolves
        return;
    }
    // Same id for every resend: a slow answer to an earlier copy (SLIRP asks
    // the host resolver, which can take seconds for NXDOMAIN) still counts.
    if (e->tries == 0) e->txid = ++dns_txid;
    e->tries++;
    e->sent_tick = tick_count ? tick_count : 1;

    uint8_t q[300];
    int n = 0;
    q[n++] = (uint8_t)(e->txid >> 8); q[n++] = (uint8_t)e->txid;
    q[n++] = 0x01; q[n++] = 0x00;      // standard query, recursion desired
    q[n++] = 0x00; q[n++] = 0x01;      // 1 question
    for (int i = 0; i < 6; i++) q[n++] = 0;
    n += dns_encode_name(e->host, q + n);
    q[n++] = 0x00; q[n++] = 0x01;      // QTYPE A
    q[n++] = 0x00; q[n++] = 0x01;      // QCLASS IN
    serial_printf("[dns] querying '%s'...\n", e->host);
    udp_send(dns_server_ip, 12345, 53, q, (uint16_t)n);
}

int dns_lookup(const char* host, uint32_t* ip) {
    if (!host || !host[0]) return -1;
    if (net_parse_ip(host, ip)) return 1;
    int free_i = -1, lru_i = -1;
    for (int i = 0; i < DNS_SLOTS; i++) {
        struct dns_ent* e = &dns_tab[i];
        if (e->st == DNS_ST_FREE) { if (free_i < 0) free_i = i; continue; }
        if (!dns_host_matches(e->host, host)) {
            if (e->st != DNS_ST_PENDING &&
                (lru_i < 0 || (int32_t)(e->used_tick - dns_tab[lru_i].used_tick) < 0)) lru_i = i;
            continue;
        }
        e->used_tick = tick_count;
        if (e->st == DNS_ST_PENDING) return 0;
        if (!dns_expired(e)) {
            if (e->st == DNS_ST_OK) { *ip = e->ip; return 1; }
            return -1;
        }
        // expired: query again in place
        e->st = DNS_ST_PENDING;
        e->tries = 0;
        dns_send_query(e);
        return 0;
    }
    int slot = free_i >= 0 ? free_i : lru_i;
    if (slot < 0) return 0;            // every slot is mid-query: try again shortly
    struct dns_ent* e = &dns_tab[slot];
    memset(e, 0, sizeof *e);
    net_copy_str(e->host, host);
    e->st = DNS_ST_PENDING;
    e->used_tick = tick_count;
    dns_send_query(e);
    return 0;
}

int dns_resolve(const char* hostname) {
    uint32_t ip;
    int r = dns_lookup(hostname, &ip);
    for (int i = 0; i < DNS_SLOTS; i++)
        if (dns_tab[i].st != DNS_ST_FREE && dns_host_matches(dns_tab[i].host, hostname)) {
            dns_tab[i].notify = 1;
            if (r != 0) dns_notify(&dns_tab[i]);
            break;
        }
    return r;
}

static void dns_fail(struct dns_ent* e) {
    e->st = DNS_ST_FAILED;
    e->expire_tick = tick_count + DNS_NEG_TICKS;
    serial_printf("[dns] lookup failed for '%s'\n", e->host);
    dns_notify(e);
}

// Retries: queries waiting for ARP go out once it resolves; unanswered
// ones are resent, then fail.
static void dns_poll(void) {
    for (int i = 0; i < DNS_SLOTS; i++) {
        struct dns_ent* e = &dns_tab[i];
        if (e->st != DNS_ST_PENDING) continue;
        if (e->sent_tick == 0) { dns_send_query(e); continue; }
        if ((uint32_t)(tick_count - e->sent_tick) < DNS_RETRY_TICKS) continue;
        if (e->tries >= DNS_MAX_TRIES) dns_fail(e);
        else dns_send_query(e);
    }
}

static void handle_dns_response(uint8_t* data, uint16_t len) {
    if (len < 12) return;
    uint16_t tx_id = (uint16_t)((data[0] << 8) | data[1]);
    uint16_t flags = (uint16_t)((data[2] << 8) | data[3]);
    uint16_t qd_count = (uint16_t)((data[4] << 8) | data[5]);
    uint16_t an_count = (uint16_t)((data[6] << 8) | data[7]);
    if (!(flags & 0x8000)) return;     // not a response
    struct dns_ent* e = 0;
    for (int i = 0; i < DNS_SLOTS; i++)
        // A late answer to a query already given up on still updates the
        // cache (the next lookup of that host then hits).
        if ((dns_tab[i].st == DNS_ST_PENDING || dns_tab[i].st == DNS_ST_FAILED) &&
            dns_tab[i].sent_tick && dns_tab[i].txid == tx_id) {
            e = &dns_tab[i];
            break;
        }
    if (!e) return;                    // stale / unknown transaction
    if (flags & 0x000F) {              // RCODE error (NXDOMAIN...)
        serial_printf("[dns] query error, rcode=%u\n", flags & 0xF);
        dns_fail(e);
        return;
    }

    int offset = 12;
    int pkt_len = (int)len;
    for (int i = 0; i < qd_count; i++) {
        char name[256];
        offset = dns_decode_name(data, pkt_len, offset, name, 256);
        if (offset < 0 || offset + 4 > pkt_len) return; // malformed question
        offset += 4;
    }
    for (int i = 0; i < an_count && offset < pkt_len; i++) {
        char name[256];
        offset = dns_decode_name(data, pkt_len, offset, name, 256);
        if (offset < 0 || offset + 10 > pkt_len) return;
        uint16_t atype = (uint16_t)((data[offset] << 8) | data[offset + 1]);
        uint16_t aclass = (uint16_t)((data[offset + 2] << 8) | data[offset + 3]);
        uint32_t ttl = ((uint32_t)data[offset + 4] << 24) | ((uint32_t)data[offset + 5] << 16) |
                       ((uint32_t)data[offset + 6] << 8) | data[offset + 7];
        uint16_t rdlength = (uint16_t)((data[offset + 8] << 8) | data[offset + 9]);
        offset += 10;
        if (atype == 1 && aclass == 1 && rdlength == 4) {   // A record, IN class
            if (offset + 4 > pkt_len) return;
            e->ip = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                    ((uint32_t)data[offset + 2] << 8) | data[offset + 3];
            e->st = DNS_ST_OK;
            if (ttl < 30) ttl = 30;
            if (ttl > 600) ttl = 600;
            e->expire_tick = tick_count + ttl * 100;
            serial_printf("[dns] resolved: %s -> %u.%u.%u.%u\n", e->host,
                          e->ip >> 24, (e->ip >> 16) & 0xFF, (e->ip >> 8) & 0xFF, e->ip & 0xFF);
            dns_notify(e);
            return;
        }
        offset += rdlength;
    }
    dns_fail(e);                       // answered, but no usable A record
}

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


// ---- TCP sockets ------------------------------------------------------------------

static struct tcp_sock* sock_get(int s) {
    if (s < 0 || s >= TCP_MAX_SOCKS || !tcp_socks[s].used) return 0;
    return &tcp_socks[s];
}

// Free receive-ring space = the window we may advertise.
static uint32_t tcp_rx_free(const struct tcp_sock* c) {
    uint32_t f = TCP_RX_CAP - c->rx_len;
    return f > TCP_WIN_MAX ? TCP_WIN_MAX : f;
}

static int tcp_is_local(const uint8_t* ip) {
    return (ip[0] & 0xF0) == (our_ip[0] & 0xF0) && ip[1] == our_ip[1] && ip[2] == our_ip[2];
}

// TCP checksum over pseudo-header + segment (big-endian 16-bit words).
static uint16_t tcp_checksum(uint8_t* src_ip, uint8_t* dst_ip, uint8_t* tcp_data, int tcp_len);

// Largest TCP payload in one Ethernet frame (MTU 1500 - IP 20 - TCP 20).
#define TCP_SEG_MAX (ETH_FRAME_MAX - 14 - 20 - 20)

// One segment. data_len MUST be <= TCP_SEG_MAX: the frame is a 1514-byte
// stack buffer (a 1.6KB request once overflowed it and smashed the stack).
// SYN segments carry an MSS option.
static void tcp_send_one(struct tcp_sock* c, uint32_t seq_num, uint32_t ack_num, uint8_t flags,
                         const uint8_t* data, uint16_t data_len) {
    uint8_t target_mac[6];
    if (data_len > TCP_SEG_MAX) data_len = TCP_SEG_MAX;   // never overflow the frame
    // Route through the gateway for off-subnet hosts (QEMU SLIRP NAT)
    uint8_t* arp_target = tcp_is_local(c->dst_ip) ? c->dst_ip : gateway_ip;
    if (!arp_resolve(arp_target, target_mac)) {
        arp_send_request(arp_target);
        return;                          // the retransmit timer resends
    }

    uint8_t frame[ETH_FRAME_MAX];
    struct eth_header* eth = (struct eth_header*)frame;
    build_eth_header(eth, target_mac, e1000_get_mac(), 0x0800);

    uint16_t tcp_hdr_len = (flags & 0x02) ? 24 : 20;
    struct ip_header* ip = (struct ip_header*)(frame + sizeof(struct eth_header));
    ip->version_ihl = 0x45;
    ip->tos = 0;
    uint16_t ip_total = (uint16_t)(sizeof(struct ip_header) + tcp_hdr_len + data_len);
    ip->total_length = (uint16_t)(((ip_total >> 8) & 0xFF) | ((ip_total & 0xFF) << 8));
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = 6;
    for (int i = 0; i < 4; i++) ip->src_ip[i] = our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = c->dst_ip[i];
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip);

    uint8_t* tcp = frame + sizeof(struct eth_header) + sizeof(struct ip_header);
    tcp[0] = (uint8_t)(c->src_port >> 8); tcp[1] = (uint8_t)c->src_port;
    tcp[2] = (uint8_t)(c->dst_port >> 8); tcp[3] = (uint8_t)c->dst_port;
    tcp[4] = (uint8_t)(seq_num >> 24); tcp[5] = (uint8_t)(seq_num >> 16);
    tcp[6] = (uint8_t)(seq_num >> 8);  tcp[7] = (uint8_t)seq_num;
    tcp[8] = (uint8_t)(ack_num >> 24); tcp[9] = (uint8_t)(ack_num >> 16);
    tcp[10] = (uint8_t)(ack_num >> 8); tcp[11] = (uint8_t)ack_num;
    tcp[12] = (uint8_t)((tcp_hdr_len / 4) << 4);
    tcp[13] = flags;
    // Window = free receive-ring space: the peer can never send more than
    // we can keep (the old fixed 32KB window over a ring that could fill
    // while the main loop rendered meant ACKed-then-dropped bytes).
    uint32_t win = tcp_rx_free(c);
    c->adv_win = win;
    tcp[14] = (uint8_t)(win >> 8); tcp[15] = (uint8_t)win;
    tcp[16] = 0; tcp[17] = 0;
    tcp[18] = 0; tcp[19] = 0;
    if (tcp_hdr_len == 24) {             // MSS option (SYN only)
        tcp[20] = 2; tcp[21] = 4;
        tcp[22] = (uint8_t)(TCP_SEG_MAX >> 8); tcp[23] = (uint8_t)TCP_SEG_MAX;
    }
    if (data_len) memcpy(tcp + tcp_hdr_len, data, data_len);
    uint16_t cksum = tcp_checksum(our_ip, c->dst_ip, tcp, tcp_hdr_len + data_len);
    tcp[16] = (uint8_t)(cksum >> 8);
    tcp[17] = (uint8_t)cksum;

    e1000_send(frame, (uint32_t)(sizeof(struct eth_header) + sizeof(struct ip_header) + tcp_hdr_len + data_len));
    if (flags & 0x10) c->ack_pending = 0;   // this segment carried the ACK
}

// Send data as MSS-sized segments (PSH/FIN only on the last one). Every
// sender (first transmission, RTO and fast retransmit of the whole
// unacked flight) goes through here.
static void tcp_send_raw(struct tcp_sock* c, uint32_t seq_num, uint32_t ack_num, uint8_t flags,
                         const uint8_t* data, uint32_t data_len) {
    if (data_len <= TCP_SEG_MAX) { tcp_send_one(c, seq_num, ack_num, flags, data, (uint16_t)data_len); return; }
    uint32_t off = 0;
    while (off < data_len) {
        uint32_t n = data_len - off > TCP_SEG_MAX ? TCP_SEG_MAX : data_len - off;
        uint8_t f = flags;
        if (off + n < data_len) f &= (uint8_t)~(0x08 | 0x01);   // PSH/FIN: last segment only
        tcp_send_one(c, seq_num + off, ack_num, f, data + off, (uint16_t)n);
        off += n;
    }
}

static void tcp_send_ctl(struct tcp_sock* c, uint8_t flags) {
    tcp_send_one(c, c->seq, c->ack, flags, 0, 0);
}

static void tcp_rtx_arm(struct tcp_sock* c) { c->rtx_last_tick = tick_count; }

// Resend the head of the unacked flight (RTO / fast retransmit).
static void tcp_resend(struct tcp_sock* c) {
    if (c->rtx_has_syn) {
        tcp_send_one(c, c->snd_una, 0, 0x02, 0, 0);
        return;
    }
    uint8_t flags = 0x10;
    if (c->rtx_len > 0) flags |= 0x08;
    if (c->rtx_has_fin) flags |= 0x01;
    tcp_send_raw(c, c->snd_una, c->ack, flags, c->rtx, c->rtx_len);
}

int tcp_open(uint32_t dst_ip, uint16_t dst_port) {
    int s = -1;
    for (int i = 0; i < TCP_MAX_SOCKS; i++) if (!tcp_socks[i].used) { s = i; break; }
    if (s < 0) return -1;
    struct tcp_sock* c = &tcp_socks[s];
    // Buffers survive slot reuse (allocated once per slot).
    uint8_t* rtx = c->rtx; uint8_t* rx = c->rx; uint8_t* ro = c->ro_data;
    if (!rtx) rtx = (uint8_t*)kmalloc(TCP_RTX_CAP);
    if (!rx) rx = (uint8_t*)kmalloc(TCP_RX_CAP);
    if (!ro) ro = (uint8_t*)kmalloc(TCP_REORDER_SLOTS * TCP_REORDER_SEG);
    memset(c, 0, sizeof *c);
    c->rtx = rtx; c->rx = rx; c->ro_data = ro;
    if (!rtx || !rx || !ro) return -1;   // (keeps whatever did allocate for next time)
    c->used = 1;
    c->dst_ip[0] = (uint8_t)(dst_ip >> 24); c->dst_ip[1] = (uint8_t)(dst_ip >> 16);
    c->dst_ip[2] = (uint8_t)(dst_ip >> 8);  c->dst_ip[3] = (uint8_t)dst_ip;
    // Unique ephemeral port among live sockets
    for (int tries = 0; tries < 64; tries++) {
        uint16_t p = tcp_ephemeral_port;
        tcp_ephemeral_port = tcp_ephemeral_port >= 0xFFFE ? 44000 : (uint16_t)(tcp_ephemeral_port + 1);
        int clash = 0;
        for (int i = 0; i < TCP_MAX_SOCKS; i++)
            if (i != s && tcp_socks[i].used && tcp_socks[i].src_port == p) clash = 1;
        if (!clash) { c->src_port = p; break; }
    }
    c->dst_port = dst_port;
    // ISN from the timestamp counter (distinct per connection; SLIRP keeps
    // stale mappings around, so never reuse a predictable sequence space)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    c->seq = (lo ^ (hi << 13) ^ ((uint32_t)c->src_port << 16)) & 0x7FFFFFFFu;
    c->snd_una = c->seq;
    c->peer_win = 65535;
    c->rtx_timeout = TCP_RTO_SYN;
    c->state = TCP_STATE_SYN_SENT;
    c->rtx_has_syn = 1;
    tcp_send_one(c, c->seq, 0, 0x02, 0, 0);
    c->seq++;                            // SYN consumes one sequence number
    tcp_rtx_arm(c);
    if (net_trace) serial_printf("[tcp] s%d SYN -> port %u\n", s, dst_port);
    return s;
}

int tcp_state(int s) {
    struct tcp_sock* c = sock_get(s);
    return c ? c->state : TCP_STATE_CLOSED;
}

int tcp_failed(int s) {
    struct tcp_sock* c = sock_get(s);
    return c ? c->failed : 1;
}

int tcp_rx_avail(int s) {
    struct tcp_sock* c = sock_get(s);
    return c ? (int)c->rx_len : 0;
}

int tcp_send(int s, const uint8_t* data, uint32_t len) {
    struct tcp_sock* c = sock_get(s);
    if (!c || c->state != TCP_STATE_ESTABLISHED || c->failed) return -1;
    if (len > TCP_RTX_CAP - c->rtx_len) {
        serial_printf("[tcp] s%d flight buffer full (%u + %u)\n", s, c->rtx_len, len);
        return -1;
    }
    memcpy(c->rtx + c->rtx_len, data, len);
    uint32_t first = c->rtx_len;
    c->rtx_len += len;
    // Zero peer window: keep it buffered, the persist timer probes.
    if (c->peer_win != 0)
        tcp_send_raw(c, c->seq, c->ack, 0x18, c->rtx + first, len);   // PSH+ACK
    c->seq += len;
    // RTT sample (Karn: only on a fresh flight, never across a retransmit)
    if (c->rtx_tries == 0 && c->rtt_seq == 0) {
        c->rtt_seq = c->seq;
        c->rtt_tick = tick_count;
    }
    if (first == 0) tcp_rtx_arm(c);
    return 0;
}

int tcp_recv(int s, uint8_t* buf, uint32_t cap) {
    struct tcp_sock* c = sock_get(s);
    if (!c) return -1;
    if (c->rx_len == 0) {
        if (c->peer_closed || c->failed || c->state == TCP_STATE_CLOSED) return -1;
        return 0;
    }
    uint32_t n = c->rx_len < cap ? c->rx_len : cap;
    uint32_t first = TCP_RX_CAP - c->rx_tail;
    if (first > n) first = n;
    memcpy(buf, c->rx + c->rx_tail, first);
    if (n > first) memcpy(buf + first, c->rx, n - first);
    c->rx_tail = (c->rx_tail + n) % TCP_RX_CAP;
    c->rx_len -= n;
    // Window update: the window we last advertised was small and reading
    // opened it up a lot — tell the peer (otherwise a sender stalled on a
    // nearly shut window waits for its persist timer).
    if (c->state == TCP_STATE_ESTABLISHED && c->adv_win < 4 * TCP_SEG_MAX &&
        tcp_rx_free(c) >= TCP_WIN_MAX / 2)
        c->ack_pending = 1;
    return (int)n;
}

static void tcp_release(struct tcp_sock* c) {
    c->used = 0;
    c->state = TCP_STATE_CLOSED;
    c->rtx_len = 0;
    c->rx_len = 0;
}

void tcp_close(int s) {
    struct tcp_sock* c = sock_get(s);
    if (!c) return;
    c->app_closed = 1;
    c->close_tick = tick_count;
    c->rx_len = c->rx_head = c->rx_tail = 0;   // nobody reads it any more
    if (c->state == TCP_STATE_ESTABLISHED) {
        c->rtx_has_fin = 1;
        tcp_send_raw(c, c->seq, c->ack, 0x11, c->rtx, 0);   // FIN+ACK
        c->seq++;
        c->state = TCP_STATE_FIN_WAIT;
        tcp_rtx_arm(c);
    } else if (c->state != TCP_STATE_FIN_WAIT) {
        tcp_release(c);
    }
}

void tcp_abort(int s) {
    struct tcp_sock* c = sock_get(s);
    if (!c) return;
    if (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_FIN_WAIT)
        tcp_send_ctl(c, 0x14);           // RST+ACK
    tcp_release(c);
}

// Cumulative ACK processing: drop acked bytes from the retransmit flight,
// advance snd_una, reset backoff on progress, feed the RTT estimator and
// the duplicate-ACK counter.
static void tcp_process_ack(struct tcp_sock* c, uint32_t ack_num, uint16_t seg_plen) {
    // DUP-ACK DISCIPLINE (RFC 5681 §2): a duplicate ACK acknowledges no new
    // data AND carries no payload. Counting data-carrying segments with a
    // repeated ack field fired bogus fast retransmits mid-download.
    if (!seq_lt(c->snd_una, ack_num)) {
        if (seg_plen != 0) return;
        uint32_t flight = (c->rtx_has_syn ? 1u : 0u) + c->rtx_len + (c->rtx_has_fin ? 1u : 0u);
        if (flight == 0) return;
        if (ack_num == c->last_ack_num && c->dup_acks < 100) c->dup_acks++;
        else if (ack_num != c->last_ack_num) { c->last_ack_num = ack_num; c->dup_acks = 1; }
        if (c->dup_acks == 3) {
            serial_puts("[tcp] fast rtx (3 dup ACKs)\n");
            c->rtx_tries++;
            tcp_rtx_arm(c);
            tcp_resend(c);
        }
        return;
    }
    uint32_t flight = (c->rtx_has_syn ? 1u : 0u) + c->rtx_len + (c->rtx_has_fin ? 1u : 0u);
    uint32_t acked = ack_num - c->snd_una;
    if (acked > flight) acked = flight; // clamp bogus acks
    if (c->rtx_has_syn && acked > 0) { c->rtx_has_syn = 0; c->snd_una++; acked--; }
    if (acked > 0 && c->rtx_len > 0) {
        uint32_t take = acked < c->rtx_len ? acked : c->rtx_len;
        memmove(c->rtx, c->rtx + take, c->rtx_len - take);
        c->rtx_len -= take;
        acked -= take;
        c->snd_una += take;
    }
    if (acked > 0 && c->rtx_has_fin) { c->rtx_has_fin = 0; c->snd_una++; }

    c->dup_acks = 0;
    c->last_ack_num = ack_num;
    if (c->rtx_tries == 0 && c->rtt_seq != 0 && !seq_lt(ack_num, c->rtt_seq)) {
        uint32_t rtt = tick_count - c->rtt_tick;
        if (c->srtt == 0) {
            c->srtt = rtt << 3;
            c->rttvar = rtt << 1;
        } else {
            uint32_t sr = c->srtt >> 3;
            uint32_t diff = rtt > sr ? rtt - sr : sr - rtt;
            if ((rtt << 3) > c->srtt) c->srtt += ((rtt << 3) - c->srtt) >> 3;
            else c->srtt -= (c->srtt - (rtt << 3)) >> 3;
            uint32_t rv = c->rttvar >> 2;
            if (diff > rv) c->rttvar += diff - rv;
            else c->rttvar -= rv - diff;
        }
        uint32_t rto = (c->srtt >> 3) + c->rttvar;
        if (rto < TCP_RTO_MIN) rto = TCP_RTO_MIN;
        if (rto > TCP_RTO_MAX) rto = TCP_RTO_MAX;
        c->rtx_timeout = (uint16_t)rto;
        c->rtt_seq = 0;
    }
    if (c->rtx_timeout < TCP_RTO_MIN) c->rtx_timeout = TCP_RTO_MIN;
    c->rtx_tries = 0;
    if (c->rtx_len > 0 || c->rtx_has_syn || c->rtx_has_fin) tcp_rtx_arm(c);
}

// Append in-order payload to the receive ring. The window never exceeds
// the free space, so this only truncates for a peer that ignores it.
static uint32_t tcp_rx_put(struct tcp_sock* c, const uint8_t* p, uint32_t n) {
    uint32_t room = TCP_RX_CAP - c->rx_len;
    if (n > room) n = room;
    uint32_t first = TCP_RX_CAP - c->rx_head;
    if (first > n) first = n;
    memcpy(c->rx + c->rx_head, p, first);
    if (n > first) memcpy(c->rx, p + first, n - first);
    c->rx_head = (c->rx_head + n) % TCP_RX_CAP;
    c->rx_len += n;
    return n;
}

// Store a gap segment (seq > RCV.NXT). Returns 1 stored, 0 dropped.
static int tcp_reorder_store(struct tcp_sock* c, uint32_t seq, const uint8_t* payload, uint16_t plen) {
    if (!payload || plen == 0 || plen > TCP_REORDER_SEG) return 0;
    for (int i = 0; i < TCP_REORDER_SLOTS; i++)
        if (c->ro_used[i] && c->ro_seq[i] == seq) return 1;   // already held
    for (int i = 0; i < TCP_REORDER_SLOTS; i++) {
        if (!c->ro_used[i]) {
            memcpy(c->ro_data + i * TCP_REORDER_SEG, payload, plen);
            c->ro_seq[i] = seq;
            c->ro_len[i] = plen;
            c->ro_used[i] = 1;
            return 1;
        }
    }
    return 0;
}

static void tcp_complete_fin(struct tcp_sock* c);

// Deliver one in-order payload (seq == RCV.NXT) + drain abutting buffered
// segments. The ACK goes out at the next flush (one per receive batch,
// not one per segment — each frame is an MMIO exit).
static void tcp_deliver_in_order(struct tcp_sock* c, const uint8_t* payload, uint16_t plen) {
    uint32_t took = tcp_rx_put(c, payload, plen);
    c->ack += took;
    c->ack_pending = 1;
    if (took < plen) return;             // ring full: the rest is retransmitted
    for (;;) {
        int found = -1;
        for (int i = 0; i < TCP_REORDER_SLOTS; i++)
            if (c->ro_used[i] && c->ro_seq[i] == c->ack) { found = i; break; }
        if (found < 0) break;
        uint16_t flen = c->ro_len[found];
        if (TCP_RX_CAP - c->rx_len < flen) break;   // keep it until there is room
        tcp_rx_put(c, c->ro_data + found * TCP_REORDER_SEG, flen);
        c->ack += flen;
        c->ro_used[found] = 0;
    }
    // Drop held segments the stream has already passed.
    for (int i = 0; i < TCP_REORDER_SLOTS; i++)
        if (c->ro_used[i] && seq_lt(c->ro_seq[i], c->ack)) c->ro_used[i] = 0;
    if (c->fin_pending && c->fin_seq == c->ack) tcp_complete_fin(c);
}

// Complete a peer FIN once every byte before it has been received: ACK it,
// close our side too (HTTP never sends after the server closes), mark EOF.
static void tcp_complete_fin(struct tcp_sock* c) {
    c->fin_pending = 0;
    c->peer_closed = 1;
    c->ack++;                            // FIN consumes one sequence number
    if (c->state == TCP_STATE_ESTABLISHED) {
        c->rtx_has_fin = 1;
        c->rtx_tries = 0;
        tcp_send_raw(c, c->seq, c->ack, 0x11, c->rtx, 0);   // ACK + our FIN
        c->seq++;
        c->state = TCP_STATE_FIN_WAIT;
        tcp_rtx_arm(c);
    } else {
        tcp_send_ctl(c, 0x10);
    }
    if (net_trace) serial_printf("[tcp] s%d peer FIN\n", (int)(c - tcp_socks));
}

void tcp_handle_packet(uint8_t* src_ip, uint8_t* data, uint32_t len) {
    if (len < 20) return;
    uint16_t src_port = (uint16_t)((data[0] << 8) | data[1]);
    uint16_t dst_port = (uint16_t)((data[2] << 8) | data[3]);
    struct tcp_sock* c = 0;
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        struct tcp_sock* t = &tcp_socks[i];
        if (t->used && t->dst_port == src_port && t->src_port == dst_port &&
            t->dst_ip[0] == src_ip[0] && t->dst_ip[1] == src_ip[1] &&
            t->dst_ip[2] == src_ip[2] && t->dst_ip[3] == src_ip[3]) { c = t; break; }
    }
    if (!c) return;                      // no such connection (stale / stray)
    uint32_t seq_num = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                       ((uint32_t)data[6] << 8) | data[7];
    uint32_t ack_num = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) |
                       ((uint32_t)data[10] << 8) | data[11];
    uint8_t flags = data[13];
    uint32_t data_offset = (uint32_t)(data[12] >> 4) * 4;
    if (data_offset < 20 || data_offset > len) return;
    uint16_t payload_len = (uint16_t)(len - data_offset);
    const uint8_t* payload = data + data_offset;
    c->peer_win = ((uint32_t)data[14] << 8) | data[15];
    if (c->peer_win != 0) c->persist_since = 0;
    if (net_trace)
        serial_printf("[tcp] s%d flags=%x seq=%x ack=%x plen=%u state=%d\n", (int)(c - tcp_socks),
                      flags, seq_num, ack_num, payload_len, c->state);

    if (flags & 0x04) {                  // RST: peer aborted
        if (c->state == TCP_STATE_SYN_SENT && !(flags & 0x10)) return;
        c->failed = 1;
        c->state = TCP_STATE_CLOSED;
        c->rtx_len = 0; c->rtx_has_syn = 0; c->rtx_has_fin = 0;
        c->fin_pending = 0;
        if (c->app_closed) tcp_release(c);
        return;
    }

    if (c->state == TCP_STATE_SYN_SENT) {
        if ((flags & 0x12) == 0x12 && ack_num == c->snd_una + 1) {   // SYN+ACK for our SYN
            c->ack = seq_num + 1;
            c->state = TCP_STATE_ESTABLISHED;
            tcp_process_ack(c, ack_num, 0);
            tcp_send_ctl(c, 0x10);
            if (net_trace) serial_printf("[tcp] s%d ESTABLISHED\n", (int)(c - tcp_socks));
        }
        return;
    }
    if (c->state == TCP_STATE_CLOSED) {
        if (flags & 0x10) tcp_process_ack(c, ack_num, payload_len);
        return;
    }

    if (flags & 0x10) tcp_process_ack(c, ack_num, payload_len);

    if (c->state == TCP_STATE_FIN_WAIT && c->app_closed) {
        // Owner is gone: just finish the close.
        if (!c->rtx_has_fin && !c->rtx_len) tcp_release(c);
        else if (flags & 0x01) tcp_send_ctl(c, 0x10);
        return;
    }

    // In-order data is delivered (+ buffered successors drained); a gap is
    // stashed and the hole edge re-ACKed (fast-retransmit hint); duplicates
    // are re-ACKed only. Only ESTABLISHED accepts new data.
    uint16_t seg_dlen = payload_len;
    if (payload_len > 0 && c->state == TCP_STATE_ESTABLISHED) {
        if (seq_num == c->ack) {
            tcp_deliver_in_order(c, payload, payload_len);
        } else if (seq_lt(seq_num, c->ack)) {
            // Overlap: deliver only the new tail.
            uint32_t skip = c->ack - seq_num;
            if (skip < payload_len) tcp_deliver_in_order(c, payload + skip, (uint16_t)(payload_len - skip));
            else tcp_send_ctl(c, 0x10);
        } else {
            if (!tcp_reorder_store(c, seq_num, payload, payload_len) && net_trace)
                serial_puts("[tcp] reorder full, dropping gap segment\n");
            tcp_send_ctl(c, 0x10);
        }
    }
    if (flags & 0x01) {                  // FIN
        uint32_t fin_seq = seq_num + seg_dlen;
        if (c->peer_closed) { tcp_send_ctl(c, 0x10); }       // retransmitted FIN
        else if (c->state != TCP_STATE_ESTABLISHED && c->state != TCP_STATE_FIN_WAIT) {}
        else if (fin_seq != c->ack && seq_lt(c->ack, fin_seq)) {
            // FIN ahead of missing data: completing it now would ACK bytes
            // never received. Hold it; the drain completes the close.
            c->fin_pending = 1;
            c->fin_seq = fin_seq;
            tcp_send_ctl(c, 0x10);
        } else {
            tcp_complete_fin(c);
        }
    }
    if (c->state == TCP_STATE_FIN_WAIT && c->peer_closed && !c->rtx_has_fin && !c->rtx_len)
        c->state = TCP_STATE_CLOSED;     // both FINs exchanged and acknowledged
}

// Retransmission + persist timers, delayed ACKs, closed-socket reaping.
static void tcp_poll(void) {
    for (int i = 0; i < TCP_MAX_SOCKS; i++) {
        struct tcp_sock* c = &tcp_socks[i];
        if (!c->used) continue;
        if (c->ack_pending && c->state != TCP_STATE_CLOSED) tcp_send_ctl(c, 0x10);
        if (c->app_closed) {
            if (c->state == TCP_STATE_CLOSED ||
                (uint32_t)(tick_count - c->close_tick) > TCP_CLOSE_LINGER) {
                tcp_release(c);
                continue;
            }
        }
        uint32_t flight = (c->rtx_has_syn ? 1u : 0u) + c->rtx_len + (c->rtx_has_fin ? 1u : 0u);
        if (flight == 0) continue;
        if ((uint32_t)(tick_count - c->rtx_last_tick) < c->rtx_timeout) continue;

        // Zero-window persist: 1-byte probes (not counted as loss), bounded.
        if (c->peer_win == 0 && c->rtx_len > 0 && c->state == TCP_STATE_ESTABLISHED) {
            if (!c->persist_since) c->persist_since = tick_count;
            if ((uint32_t)(tick_count - c->persist_since) > TCP_PERSIST_TIMEOUT) {
                serial_printf("[tcp] s%d persist give-up (window shut)\n", i);
                c->failed = 1;
                c->state = TCP_STATE_CLOSED;
                c->rtx_len = 0; c->rtx_has_syn = 0; c->rtx_has_fin = 0;
                continue;
            }
            tcp_send_raw(c, c->snd_una, c->ack, 0x10, c->rtx, 1);
            tcp_rtx_arm(c);
            continue;
        }
        if (c->rtx_tries >= TCP_MAX_RTX) {
            serial_printf("[tcp] s%d retransmit give-up, closing\n", i);
            c->failed = 1;
            c->state = TCP_STATE_CLOSED;
            c->rtx_len = 0; c->rtx_has_syn = 0; c->rtx_has_fin = 0;
            continue;
        }
        c->rtx_tries++;
        if (c->rtx_tries > 1 && c->rtx_timeout < TCP_RTO_MAX) c->rtx_timeout = (uint16_t)(c->rtx_timeout * 2);
        tcp_rtx_arm(c);
        if (net_trace || c->rtx_tries > 2) serial_printf("[tcp] s%d retransmit #%d\n", i, c->rtx_tries);
        tcp_resend(c);
    }
}

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

    // Framing decision from the SHARED line-oriented parser (crypto/
    // tls_client.c): dechunk if and only if the parser says
    // Transfer-Encoding ends in chunked. The old substring sniff for
    // "chunked" anywhere in the headers disagreed with the completeness
    // gate (cryptoholes round 4, P1): a "chunked" token inside another
    // header's VALUE (or a case-variant name) made this dechunk a plain
    // Content-Length body — corrupting it whenever the body began with
    // hex digits — while the gate had blessed it COMPLETE via CL. One
    // parser, zero differentials by construction. Malformed blocks also
    // take the strip-only path (the gate already refused to bless them).
    int chunked = 0;
    {
        struct http_framing fr;
        http_parse_framing((const uint8_t*)buf, (uint32_t)body - 2, &fr);
        chunked = !fr.malformed && fr.chunked;
    }

    // Shift the body to the start of the buffer so callers (html_parse etc.)
    // receive a body-only buffer at buf[0]. Headers are discarded.
    int out = len - body;
    memmove(buf, buf + body, out);
    len = out;
    if (!chunked) return len;

    int rd = 0, wr = 0;
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
        // returning `len` here leaves the un-shifted body intact.
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

void net_set_event_callback(void (*cb)(const char* msg)) {
    net_event_callback = cb;
}

// Timers and retries (called from the main loop right after e1000_poll, so
// the ACKs owed for the batch just received go out together).
void net_poll(void) {
    tcp_poll();
    dns_poll();

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
            icmp_send_ping(ping_target_ip, ping_id, ping_seq);
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
        // Derive the TCP segment length from the IP total length, NOT the
        // (padded) Ethernet frame length. Ethernet pads short frames up to the
        // 64-byte minimum, so a frame-length calculation would silently include
        // zero padding as payload — feeding garbage (or, worse, advancing
        // RCV.NXT by the padding count) to the application.
        struct ip_header* iph = (struct ip_header*)(data + sizeof(struct eth_header));
        uint16_t ip_total = ((iph->total_length >> 8) & 0xFF) |
                            ((iph->total_length & 0xFF) << 8);
        uint8_t ip_hlen = (iph->version_ihl & 0x0F) * 4;
        if (ip_total < (uint16_t)ip_hlen + 20) return; // malformed
        uint8_t* tcp_data = (uint8_t*)iph + ip_hlen;
        uint32_t tcp_len = ip_total - ip_hlen;
        tcp_handle_packet(iph->src_ip, tcp_data, tcp_len);
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
