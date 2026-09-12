#include "network.h"
#include "../crypto/tls_client.h" // http_parse_framing (shared framing)
#include "e1000.h"
#include "../io.h"
#include "../serial.h"
#include "tls_net.h"

static uint8_t our_ip[4] = {10, 0, 2, 15};    // QEMU user-mode default
static uint8_t gateway_ip[4] = {10, 0, 2, 2};  // QEMU user-mode gateway
static uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const char hex[] = "0123456789abcdef";

static void print_hex32(uint32_t val) {
    for (int s = 28; s >= 0; s -= 4)
        serial_putchar(hex[(val >> s) & 0xF]);
}

static int tls_dumped = 0; // one-shot dump of first TLS segment bytes

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
// The host the cached dns_resolved_ip belongs to. DNS has no per-host cache,
// so we must only reuse the cached IP when the requested host matches — otherwise
// a second navigation would connect to the first host's IP and hang.
static char dns_resolved_host[128] = {0};

// Pending operation state (for retry after ARP resolves)
static int ping_pending = 0;
static uint8_t ping_target_ip[4];
static uint16_t ping_id = 0;
static uint16_t ping_seq = 0;

static char dns_pending_host[128] = {0};
static int dns_retry_pending = 0;
// Host of the last DNS query actually TRANSMITTED (set where the query goes
// on the wire, after ARP). On a definitive DNS failure (RCODE error or a
// response with no A record), this tells us whose pending request to abort.
static char dns_query_host[128] = {0};

static char http_pending_host[128] = {0};
static char http_pending_path[128] = {0};
static int http_retry_pending = 0;
static uint16_t http_pending_port = 80; // port for the parked request
// Connection attempts for the current (in-flight) request. Bounded so an
// unreachable host gives up instead of letting net_poll re-fire http_get forever
// (which would wedge the single-owner fetch model in okai.c).
static int http_conn_attempts = 0;
#define HTTP_MAX_CONN_ATTEMPTS 4

// Copy a host/path string, always NUL-terminating at the ACTUAL length.
// The old pattern (copy loop + dst[127]=0) left the previous, longer value's
// tail in place when the new value was shorter: fetching example.com then
// iana.org left "iana.orgcom" in dns_resolved_host, so dns_host_matches()
// failed forever and the okai fetch stalled in HP_DNS until timeout
// (the "clicked link opens a black window" bug).
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

// TCP state constants live in network.h (shared with tls_net.c).

// --- Reorder buffer: hold out-of-order gap segments until the hole fills.
// 8 slots x 1500B (12KB — covers a typical burst of reordered segments).
// All IRQ-context (tcp_handle_packet runs in IRQ): single-threaded vs the
// main loop's readers (buffers are only drained here, never concurrently).
// Declared up here (not at tcp_handle_packet) because tcp_connect resets it.
#define TCP_REORDER_SLOTS 8
#define TCP_REORDER_SEG 1500
static uint8_t tcp_reorder_data[TCP_REORDER_SLOTS][TCP_REORDER_SEG];
static uint32_t tcp_reorder_seq[TCP_REORDER_SLOTS];
static uint16_t tcp_reorder_len[TCP_REORDER_SLOTS];
static uint8_t tcp_reorder_used[TCP_REORDER_SLOTS];

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
    // Receive window (what WE advertise): fixed 32KB. Our buffers are huge
    // (1MB http_response, 64KB TLS ring), so we never need to shrink it —
    // but advertising a real window (not a stub) lets fast servers stream
    // without silly-window stalls. Sent on every segment (tcp_send_raw).
    // Peer window (what THEY advertise): cached from incoming segments,
    // caps our sends (never send past SND.UNA + peer_win). 0 = persist
    // (send 1-byte probes on rtx timer instead of stalling forever).
    uint32_t peer_win;       // last advertised peer window (bytes)
    // Fast retransmit: count DUP acks (same ack_num, no progress). At 3,
    // resend the head of the flight immediately (no RTO wait) — classic
    // Tahoe fast-retransmit (no fast-recovery/cwnd inflation: congestion
    // window stays 1 MSS-equivalent since we only ever have one small
    // flight; the gain is latency, not throughput).
    uint32_t dup_acks;       // consecutive duplicate ACKs
    uint32_t last_ack_num;   // ack number of the last processed ACK
    // RTT estimation (Jacobson/Karels, tick granularity): SRTT/RTTVAR drive
    // the RTO instead of the fixed 220ms. Ticks are ~10ms (100Hz PIT), so
    // alpha=1/8 beta=1/4 via shifts. RTO clamped [2 ticks, 60 ticks].
    uint32_t srtt;           // smoothed RTT (ticks << 3)
    uint32_t rttvar;         // variance (ticks << 2)
    uint32_t rtt_seq;        // seq being timed (0 = none in flight)
    uint32_t rtt_tick;       // tick when rtt_seq was first sent
};

static struct tcp_conn tcp_conn;

// Ephemeral source port for outbound TCP connections. Incremented on every
// connect so consecutive fetches don't share a 4-tuple — QEMU's SLIRP NAT
// keeps the previous connection's mapping (TIME_WAIT) around and would
// otherwise route/drop the second connection's SYN and its response. Observed
// symptom: the first HTTPS fetch worked, the second hung forever at the TCP
// handshake (SYN retransmits, no SYNACK). A fresh source port gives SLIRP a
// clean mapping so the second fetch completes on the first attempt.
static uint16_t tcp_ephemeral_port = 43210;

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
// Real pages run large (google.com serves ~85KB of HTML); a 64KB buffer
// truncated mid-script, and the parser then produced only the header links.
// Plain-HTTP response accumulator. Raised from 128 KiB to 1 MiB so larger
// pages (multi-hundred-KiB bodies, e.g. real homepages) are captured whole
// instead of being truncated mid-stream. Raising this array is the only change
// needed: the clamp in tcp_handle_packet keys off sizeof(http_response), and
// html_parse / dechunk read http_response_len, not a fixed size.
static char http_response[1048576];
static int  http_response_len = 0;
static int  http_response_overflow = 0; // set once when a response exceeds the buffer

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

// Case-insensitive hostname comparison (hostnames are case-insensitive).
static int dns_host_matches(const char* a, const char* b);

// A DNS query for `dns_query_host` has definitively failed (RCODE error or a
// response carrying no A record). If an HTTP fetch was parked waiting on this
// host (http_retry_pending), abort it now — otherwise the okai HTTP give-up
// condition (!http_is_retry_pending()) can never fire and the fetch owner
// wedges forever, leaving every later okai window black.
static void dns_fail_pending_http(void) {
    if (http_retry_pending && dns_host_matches(dns_query_host, http_pending_host)) {
        http_retry_pending = 0;
        serial_puts("[dns] failed for pending HTTP host, aborting request\n");
    }
}

// Parse a strict dotted-quad ("10.0.2.2") into the DNS-cache layout
// (first octet in the high byte). Returns 1 on success.
int net_parse_ip(const char* s, uint32_t* out) {  // shared: TLS path skips DNS for numeric hosts
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

// Seed the DNS cache with a literal address — numeric-IP URLs skip DNS.
void dns_seed(const char* host, uint32_t ip) {
    net_copy_str(dns_resolved_host, host);
    dns_resolved_ip = ip;
    dns_resolved = 1;
    dns_pending = 0;
    serial_puts("[dns] numeric host, seeded\n");
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
        dns_fail_pending_http();
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
    // Response parsed but carried no usable A record — definitive failure.
    dns_pending = 0;
    dns_fail_pending_http();
}

int dns_resolve(const char* hostname) {
    dns_resolved = 0;
    dns_pending = 1;
    dns_tx_id++;
    net_copy_str(dns_resolved_host, hostname);

    uint8_t dns_server_ip[4] = {10, 0, 2, 3}; // QEMU SLIRP DNS

    uint8_t target_mac[6];
    if (!arp_resolve(dns_server_ip, target_mac)) {
        arp_send_request(dns_server_ip);
        net_copy_str(dns_pending_host, hostname);
        dns_retry_pending = 1;
        return -1; // Waiting for ARP
    }
    dns_retry_pending = 0;
    net_copy_str(dns_query_host, hostname);

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

int dns_is_resolved(uint32_t* ip, const char* host) {
    if (dns_resolved && dns_host_matches(dns_resolved_host, host)) {
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
    tcp_conn.src_port = tcp_ephemeral_port;
    if (tcp_ephemeral_port >= 0xFFFE) tcp_ephemeral_port = 44000;
    else tcp_ephemeral_port++;
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
    // Estimator + window state reset per connection (stale SRTT from a LAN
    // peer would mis-time a WAN peer and vice versa).
    tcp_conn.peer_win = 65535; // assume open until the first segment says so
    tcp_conn.dup_acks = 0;
    tcp_conn.last_ack_num = 0;
    tcp_conn.srtt = 0;
    tcp_conn.rttvar = 0;
    tcp_conn.rtt_seq = 0;
    tcp_conn.rtt_tick = 0;
    // Reorder buffer is per-connection: a stale gap from the previous
    // connection would deliver garbage into the new stream on seq overlap.
    for (int ri = 0; ri < TCP_REORDER_SLOTS; ri++) tcp_reorder_used[ri] = 0;

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
    tcp[14] = 0x80; tcp[15] = 0x00; // Window: 32768 (see tcp_send_raw)
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
    // Advertise a real receive window (32KB — our buffers dwarf it) instead
    // of the old 60-byte stub (0x3C). The stub throttled fast servers to
    // ~60 bytes per round trip (silly-window behavior on THEIR side).
    tcp[14] = 0x80; tcp[15] = 0x00;
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
// advance snd_una, reset backoff on progress. Also feeds the RTT estimator,
// the duplicate-ACK counter (fast retransmit), and the peer-window cache.
static void tcp_process_ack(uint32_t ack_num, uint16_t seg_plen) {
    // DUP-ACK DISCIPLINE (RFC 5681 §2: a duplicate ACK acknowledges NO new
    // data AND carries no payload). Fast retransmit counts ONLY pure-ACK
    // repeats (plen == 0, ack_num <= SND.UNA). A segment carrying THEIR data
    // (plen > 0) advances the receive stream even when the ack field repeats
    // (their data ACKs are usually piggybacked, not per-segment) — counting
    // those fired bogus fast retransmits mid-download (bisected 2026-09-08:
    // 3 "fast rtx" during a healthy example.com fetch, each resending the
    // GET and stalling the server into SYN-timeout on the NEXT connection).
    if (seg_plen == 0 && !seq_lt(tcp_conn.snd_una, ack_num)) {
        // Duplicate ACK (no progress): count toward fast retransmit. Only
        // count pure ACKs (the retransmit path resends on #3 — see below).
        // Cap the counter so a long stall doesn't overflow it.
        if (ack_num == tcp_conn.last_ack_num && tcp_conn.dup_acks < 100)
            tcp_conn.dup_acks++;
        else if (ack_num != tcp_conn.last_ack_num) {
            tcp_conn.last_ack_num = ack_num;
            tcp_conn.dup_acks = 1;
        }
        if (tcp_conn.dup_acks == 3) {
            // Fast retransmit: resend the head of the flight NOW (Tahoe —
            // no cwnd inflation, just skip the RTO wait). Serial-gated so
            // headless logs show exactly when it fires.
            serial_puts("[tcp] fast rtx (3 dup ACKs)\n");
            tcp_conn.rtx_tries++; // counts toward give-up like an RTO retry
            tcp_rtx_arm();
            if (tcp_conn.rtx_has_syn) {
                tcp_send_raw(tcp_conn.snd_una, 0, 0x02, 0, 0);
            } else {
                uint8_t flags = 0x10;
                if (tcp_conn.rtx_len > 0) flags |= 0x08;
                else if (tcp_conn.rtx_has_fin) flags |= 0x01;
                tcp_send_raw(tcp_conn.snd_una, tcp_conn.ack, flags,
                             tcp_rtx_buf, tcp_conn.rtx_len);
            }
        }
        return; // old / duplicate ack
    }

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

    // Progress: fresh ACK number resets the dup counter; RTT sample if the
    // ack covers the timed seq (Karn: skip samples from retransmitted flights
    // — rtx_tries > 0 means this ACK may ack a resend, so don't sample).
    tcp_conn.dup_acks = 0;
    tcp_conn.last_ack_num = ack_num;
    if (tcp_conn.rtx_tries == 0 && tcp_conn.rtt_seq != 0 &&
        !seq_lt(ack_num, tcp_conn.rtt_seq)) {
        uint32_t rtt = tick_count - tcp_conn.rtt_tick; // ~10ms ticks
        if (tcp_conn.srtt == 0) {
            // First sample: SRTT = R, RTTVAR = R/2.
            tcp_conn.srtt = rtt << 3;
            tcp_conn.rttvar = rtt << 1;
        } else {
            // Jacobson/Karels: SRTT += (R-SRTT)/8; RTTVAR += (|R-SRTT|-RTTVAR)/4.
            uint32_t sr = tcp_conn.srtt >> 3;
            uint32_t diff = rtt > sr ? rtt - sr : sr - rtt;
            tcp_conn.srtt += (rtt << 3 > tcp_conn.srtt) ?
                             ((rtt << 3) - tcp_conn.srtt) >> 3 :
                             0 - (tcp_conn.srtt - (rtt << 3) >> 3);
            uint32_t rv = tcp_conn.rttvar >> 2;
            if (diff > rv) tcp_conn.rttvar += (diff - rv);
            else tcp_conn.rttvar -= (rv - diff);
        }
        // RTO = SRTT + 4*RTTVAR, clamped [2, 60] ticks (20ms..600ms).
        uint32_t rto = (tcp_conn.srtt >> 3) + tcp_conn.rttvar;
        if (rto < 2) rto = 2;
        if (rto > 60) rto = 60;
        tcp_conn.rtx_timeout = (uint16_t)rto;
        tcp_conn.rtt_seq = 0;
    }
    // Fresh progress always clears the retry count (backoff restarts).
    tcp_conn.rtx_tries = 0;
    if (tcp_conn.rtx_len > 0 || tcp_conn.rtx_has_syn || tcp_conn.rtx_has_fin)
        tcp_rtx_arm();
}

void tcp_send_data(uint8_t* data, uint16_t len) {
    if (tcp_conn.state != TCP_STATE_ESTABLISHED) return;
    // Peer-window gate: never send past SND.UNA + peer_win. Our flights are
    // one small GET (<=512B) and the peer window is KBs, so this never fires
    // in practice — but a zero-window peer must NOT get a full send (it
    // would drop it); tcp_poll covers persistence (see below).
    if (tcp_conn.peer_win == 0) {
        // Zero window: buffer (if room) and let the persist timer probe.
        if (tcp_conn.rtx_len + len > TCP_RTX_BUF_SIZE) {
            serial_puts("[tcp] rtx buffer overflow, resetting\n");
            tcp_conn.rtx_len = 0;
        }
        for (int i = 0; i < len; i++)
            tcp_rtx_buf[tcp_conn.rtx_len + i] = data[i];
        tcp_conn.rtx_len += len;
        tcp_rtx_arm();
        return;
    }
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
    // Start an RTT sample on this flight (Karn: only when nothing unacked —
    // otherwise the ACK is ambiguous).
    if (tcp_conn.rtx_tries == 0 && tcp_conn.rtt_seq == 0) {
        tcp_conn.rtt_seq = tcp_conn.seq;
        tcp_conn.rtt_tick = tick_count;
    }
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

// --- TCP state probes (used by the TLS integration to drive a blocking
//     handshake from a single call context, e.g. the keyboard ISR) ---

int tcp_is_established(void) {
    return tcp_conn.state == TCP_STATE_ESTABLISHED;
}

int tcp_conn_state(void) {
    return tcp_conn.state;
}

// True once the peer has closed (FIN seen, possibly our FIN already ACKed).
// Used by the TLS recv callback to return EOF promptly instead of spinning
// on the 5s record timeout.
int tcp_is_closed(void) {
    return tcp_conn.state == TCP_STATE_CLOSED ||
           tcp_conn.state == TCP_STATE_FIN_WAIT;
}

// Retransmission + persist timer — call from the main loop. Fires when a
// flight (SYN, data, or FIN) goes unacked past the RTO; backs off
// exponentially and gives up after TCP_MAX_RTX attempts. Doubles as the
// zero-window persist timer: with peer_win == 0 and data buffered, sends a
// 1-byte probe (RFC 793 §3.7) instead of the full flight so a window update
// (not a drop) comes back.
void tcp_poll(void) {
    if (tcp_conn.state == TCP_STATE_CLOSED &&
        !(tcp_conn.rtx_len || tcp_conn.rtx_has_syn || tcp_conn.rtx_has_fin))
        return;

    uint32_t flight = (tcp_conn.rtx_has_syn ? 1u : 0u) +
                      tcp_conn.rtx_len +
                      (tcp_conn.rtx_has_fin ? 1u : 0u);
    if (flight == 0) return;
    if ((uint32_t)(tick_count - tcp_conn.rtx_last_tick) < tcp_conn.rtx_timeout) return;

    // Zero-window persist: probe with 1 byte (never the full flight — the
    // peer advertised zero and would drop it). Does NOT count toward give-up
    // (a shut window is not packet loss) and does NOT double the RTO.
    if (tcp_conn.peer_win == 0 && tcp_conn.rtx_len > 0 &&
        !tcp_conn.rtx_has_syn && tcp_conn.state == TCP_STATE_ESTABLISHED) {
        serial_puts("[tcp] persist probe (peer win 0)\n");
        tcp_send_raw(tcp_conn.snd_una, tcp_conn.ack, 0x10,
                     tcp_rtx_buf, 1);
        tcp_rtx_arm();
        return;
    }

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

// --- Reorder helpers (definitions live just above tcp_handle_packet).
// Store a gap segment (seq > RCV.NXT). Returns 1 stored, 0 dropped.
static int tcp_reorder_store(uint32_t seq, const uint8_t *payload, uint16_t plen);
// Deliver one in-order payload + drain abutting buffered segments.
static void tcp_deliver_in_order(uint32_t seq, const uint8_t *payload,
                                 uint16_t plen, int is_tls);

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
    // Cache the peer's advertised window on EVERY segment (window updates
    // ride on pure ACKs too, not just data). Zero is meaningful (persist).
    tcp_conn.peer_win = ((uint32_t)data[14] << 8) | data[15];

    serial_puts("[tcp] pkt src=");
    serial_putchar(hex[(src_port >> 8) & 0xF]);
    serial_putchar(hex[src_port & 0xF]);
    serial_puts(" dst=");
    serial_putchar(hex[(dst_port >> 8) & 0xF]);
    serial_putchar(hex[dst_port & 0xF]);
    serial_puts(" flags=");
    serial_putchar(hex[(flags >> 4) & 0xF]);
    serial_putchar(hex[flags & 0xF]);
    serial_puts(" seq=");
    print_hex32(seq_num);
    serial_puts(" ack=");
    print_hex32(ack_num);
    serial_puts(" our_ack=");
    print_hex32(tcp_conn.ack);
    serial_puts(" plen=");
    serial_putchar(hex[(payload_len >> 8) & 0xF]);
    serial_putchar(hex[payload_len & 0xF]);
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
        if (flags & 0x10) tcp_process_ack(ack_num, payload_len);
        return;
    }

    if (tcp_conn.state == TCP_STATE_SYN_SENT) {
        if (flags & 0x12) { // SYN+ACK
            tcp_conn.ack = seq_num + 1;
            tcp_conn.seq = ack_num; // Sync our seq
            tcp_conn.state = TCP_STATE_ESTABLISHED;
            tcp_process_ack(ack_num, payload_len); // clears in-flight SYN
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
        // Cumulative ACK for our in-flight data/FIN (usually piggybacked).
        // Thread the segment length so pure-ACK repeats (fast-retransmit
        // candidates) are distinguished from data-carrying segments.
        if (flags & 0x10) tcp_process_ack(ack_num, payload_len);

        // In-order fast path; duplicates + gaps go to the reorder buffer.
        // Duplicates (seq < RCV.NXT) arrive when OUR ack of their data was
        // lost — re-ACK only. Gaps (seq > RCV.NXT) mean an earlier segment
        // went missing — STASH the payload in the reorder buffer (up to 8
        // segments / 12KB) instead of dropping it; tcp_reorder_drain()
        // delivers it in order once the hole fills. Beyond capacity: drop +
        // re-ACK (classic behavior, sender retransmits).
        if (payload_len > 0 && seq_num != tcp_conn.ack) {
            if (seq_lt(seq_num, tcp_conn.ack)) {
                serial_puts("[tcp] dup seq, re-ACKing\n");
                tcp_send_packet(0x10, 0, 0);
                payload_len = 0; // don't double-append below
            } else if (!tcp_reorder_store(seq_num, data + data_offset,
                                           payload_len)) {
                serial_puts("[tcp] reorder full, dropping gap segment\n");
                tcp_send_packet(0x10, 0, 0);
                payload_len = 0;
            } else {
                // Stored for later: ACK the hole edge (fast-retransmit hint
                // for the sender) but don't advance RCV.NXT yet.
                serial_puts("[tcp] gap buffered, re-ACKing\n");
                tcp_send_packet(0x10, 0, 0);
                payload_len = 0;
            }
        }

        // Payload BEFORE FIN: servers may piggyback the last data bytes on
        // the FIN packet — checking FIN first would drop that payload.
        // Both branches go through tcp_deliver_in_order() so a filled hole
        // immediately drains any buffered gap segments sitting behind it.
        if (payload_len > 0) {
            if (tls_is_active()) {
                // TLS session in progress: feed raw TCP payload into the TLS
                // receive buffer. tls_client_run polls this buffer from its
                // recv callback; the main loop never reads it concurrently
                // with the IRQ, so there is no writer race.
                tcp_deliver_in_order(seq_num, data + data_offset, payload_len, 1);
            } else if (http_pending) {
                tcp_deliver_in_order(seq_num, data + data_offset, payload_len, 0);
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
            if (tls_is_active()) tls_connection_closed();
            serial_puts("[tcp] FIN_WAIT (server FIN)\n");
            return;
        }
    } else if (tcp_conn.state == TCP_STATE_FIN_WAIT) {
        if (flags & 0x10) { // ACK of our FIN
            tcp_process_ack(ack_num, payload_len); // clears rtx_has_fin
            tcp_conn.state = TCP_STATE_CLOSED;
            serial_puts("[tcp] CLOSED (FIN ACKed)\n");
        }
        // Retransmitted server FIN (our ACK was lost) — re-ACK it
        if (flags & 0x01) {
            tcp_send_packet(0x10, 0, 0);
        }
    }
}

// --- Reorder helper definitions (prototypes above tcp_handle_packet). ---
static int tcp_reorder_store(uint32_t seq, const uint8_t *payload, uint16_t plen) {
    if (!payload || plen == 0 || plen > TCP_REORDER_SEG) return 0;
    for (int i = 0; i < TCP_REORDER_SLOTS; i++) {
        if (tcp_reorder_used[i] && tcp_reorder_seq[i] == seq) return 0;
    }
    for (int i = 0; i < TCP_REORDER_SLOTS; i++) {
        if (!tcp_reorder_used[i]) {
            for (int b = 0; b < plen; b++)
                tcp_reorder_data[i][b] = payload[b];
            tcp_reorder_seq[i] = seq;
            tcp_reorder_len[i] = plen;
            tcp_reorder_used[i] = 1;
            return 1;
        }
    }
    return 0;
}

static void tcp_sink_payload(const uint8_t *payload, uint16_t plen, int is_tls) {
    if (is_tls) {
        int copy = plen < 1500 ? plen : 1500;
        tls_append_data((uint8_t*)payload, copy);
        if (!tls_dumped) {
            tls_dumped = 1;
            serial_puts("[tls] RX bytes: ");
            for (int d = 0; d < copy && d < 32; d++) {
                serial_putchar(hex[(payload[d] >> 4) & 0xF]);
                serial_putchar(hex[payload[d] & 0xF]);
                serial_putchar(' ');
            }
            serial_putchar('\n');
        }
        serial_puts("[tls] segment, fed ");
        serial_putchar('0' + (copy / 1000) % 10);
        serial_putchar('0' + (copy / 100) % 10);
        serial_putchar('0' + (copy / 10) % 10);
        serial_putchar('0' + (copy % 10));
        serial_puts(" bytes to TLS rx\n");
        return;
    }
    int copy = plen;
    if (copy > 1500) copy = 1500;
    if (http_response_len + copy > (int)sizeof(http_response) - 1) {
        copy = (int)sizeof(http_response) - 1 - http_response_len;
        if (!http_response_overflow) {
            http_response_overflow = 1;
            serial_puts("[http] WARNING: response exceeded buffer, truncating\n");
        }
    }
    for (int i = 0; i < copy; i++) {
        http_response[http_response_len + i] = (char)payload[i];
    }
    http_response_len += copy;
    http_response[http_response_len] = 0;

    serial_printf("[http] segment, total %u bytes\n",
                  (unsigned)http_response_len);

    // Terminal preview from the FIRST segment only. Show just the
    // status line — the old 180-byte raw-header dump flooded the
    // terminal on every fetch (full headers stay in the serial
    // log, where debugging belongs).
    if (http_response_len == copy) {
        for (int i = 0; i < 255; i++) net_event_msg[i] = 0;
        int idx = 0;
        for (int i = 0; i < copy && idx < 60; i++) {
            char ch = http_response[i];
            if (ch == '\r' || ch == '\n') break;
            net_event_msg[idx++] = ch;
        }
        const char* tail = " — receiving...";
        for (int i = 0; tail[i] && idx < 254; i++) net_event_msg[idx++] = tail[i];
        net_event_msg[idx++] = '\n';
        net_event_msg[idx] = 0;
        net_event_pending = 1;
    }
}

static void tcp_deliver_in_order(uint32_t seq, const uint8_t *payload,
                                 uint16_t plen, int is_tls) {
    // Caller guarantees seq == RCV.NXT for the head segment.
    tcp_conn.ack = seq + plen;
    tcp_send_packet(0x10, 0, 0); // ACK
    tcp_sink_payload(payload, plen, is_tls);
    // Drain loop: find the buffered segment starting exactly at RCV.NXT.
    for (;;) {
        int found = -1;
        for (int i = 0; i < TCP_REORDER_SLOTS; i++) {
            if (tcp_reorder_used[i] && tcp_reorder_seq[i] == tcp_conn.ack) {
                found = i;
                break;
            }
        }
        if (found < 0) break;
        uint16_t flen = tcp_reorder_len[found];
        tcp_conn.ack += flen;
        tcp_send_packet(0x10, 0, 0); // ACK (cumulative — covers the gap)
        tcp_sink_payload(tcp_reorder_data[found], flen, is_tls);
        tcp_reorder_used[found] = 0;
        serial_puts("[tcp] reorder drain: delivered buffered segment\n");
    }
}

void http_get(const char* host, const char* path) {
    http_get_port(host, path, 80);
}

// Port-aware GET (numeric-IP URLs and non-80 ports). port 0 means 80.
void http_get_port(const char* host, const char* path, uint16_t port) {
    uint16_t use_port = port ? port : 80;
    http_pending_port = use_port;

    // Every request starts a fresh response lifecycle. Without this, a
    // second fetch (e.g. okai refresh) raced the parse block with the
    // PREVIOUS response's stale http_done=1 + buffer: an immediate bogus
    // parse ran, double-dechunked the buffer to garbage, and the sentinel
    // then blocked the real response from ever rendering.
    http_response_len = 0;
    http_response_overflow = 0;
    http_done = 0;

    // Numeric host ("10.0.2.2")? Seed the cache and skip DNS entirely.
    uint32_t nip;
    if (net_parse_ip(host, &nip)) dns_seed(host, nip);

    // Resolve hostname first
    uint32_t ip;
    if (dns_is_resolved(&ip, host)) {
        // Already resolved for this host, connect directly
    } else if (dns_is_pending()) {
        // Save for retry after DNS completes
        net_copy_str(http_pending_host, host);
        net_copy_str(http_pending_path, path);
        http_retry_pending = 1;
        return;
    } else {
        dns_resolve(host);
        net_copy_str(http_pending_host, host);
        net_copy_str(http_pending_path, path);
        http_retry_pending = 1;
        return;
    }

    // The stack services one global connection, so a new request must open a
    // fresh one unless the current session is a live, reusable ESTABLISHED
    // (keep-alive) connection. Every other state (CLOSED, FIN_WAIT, CLOSE_WAIT,
    // TIME_WAIT, LAST_ACK) means the previous fetch ended — reconnect.
    if (tcp_conn.state == TCP_STATE_ESTABLISHED) {
        // Keep-alive: reuse the live connection, fall through to send the GET.
    } else if (tcp_conn.state == TCP_STATE_SYN_SENT) {
        // Handshake already in progress for this request; the retry path
        // re-fires http_get() once it reaches ESTABLISHED.
        net_copy_str(http_pending_host, host);
        net_copy_str(http_pending_path, path);
        http_retry_pending = 1;
        serial_puts("[http] waiting for TCP handshake...\n");
        return;
    } else {
        // Previous connection ended (or is mid-close) — open a fresh one.
        http_conn_attempts++;
        if (http_conn_attempts > HTTP_MAX_CONN_ATTEMPTS) {
            // Unreachable after several SYN attempts: abandon this request so
            // the okai owner model can move on to the next pending window.
            http_retry_pending = 0;
            http_done = 0;
            serial_puts("[http] giving up: host unreachable\n");
            return;
        }
        net_copy_str(http_pending_host, host);
        net_copy_str(http_pending_path, path);
        http_retry_pending = 1;
        tcp_connect(ip, use_port);
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

    const char* hdr_ua = "User-Agent: okernel/0.4\r\nAccept: */*\r\n";
    for (int i = 0; hdr_ua[i]; i++) req_buf[req_len++] = hdr_ua[i];

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
        http_response_overflow = 0;
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
int http_get_response_len(void) { return http_response_len; }
int http_is_pending(void) { return http_pending; }
int http_is_done(void) { return http_done; }
int http_is_retry_pending(void) { return http_retry_pending; }
// Reset the connection-attempt counter for a brand-new request (called by the
// okai fetch driver before http_get; NOT by net_poll's retry re-fire).
void http_reset_conn_attempts(void) { http_conn_attempts = 0; }

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
        if (dns_is_resolved(&ip, http_pending_host)) {
            if (tcp_conn.state == TCP_STATE_CLOSED) {
                http_retry_pending = 0;
                http_get_port(http_pending_host, http_pending_path, http_pending_port);
            } else if (tcp_conn.state == TCP_STATE_ESTABLISHED) {
                http_retry_pending = 0;
                http_get_port(http_pending_host, http_pending_path, http_pending_port);
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
