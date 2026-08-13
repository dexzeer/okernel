#include "network.h"
#include "rtl8139.h"
#include "../io.h"
#include "../serial.h"

static uint8_t our_ip[4] = {10, 0, 2, 15};    // QEMU user-mode default
static uint8_t gateway_ip[4] = {10, 0, 2, 2};  // QEMU user-mode gateway
static uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ARP cache (simple, single entry for now)
static uint8_t arp_cache_ip[4];
static uint8_t arp_cache_mac[6];
static int arp_cache_valid = 0;

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
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        serial_putchar('0' + target_ip[i]);
        if (i < 3) serial_putchar('.');
    }
    serial_putchar('\n');

    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = rtl8139_get_mac();

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

    rtl8139_send(frame, total);
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
            // Cache the MAC
            for (int i = 0; i < 6; i++) arp_cache_mac[i] = arp->sender_mac[i];
            for (int i = 0; i < 4; i++) arp_cache_ip[i] = arp->sender_ip[i];
            arp_cache_valid = 1;

            serial_puts("[arp] resolved: ");
            static const char hex[] = "0123456789abcdef";
            for (int i = 0; i < 6; i++) {
                serial_putchar(hex[arp_cache_mac[i] >> 4]);
                serial_putchar(hex[arp_cache_mac[i] & 0xF]);
                if (i < 5) serial_putchar(':');
            }
            serial_putchar('\n');
        }
    } else if (opcode == 1) { // ARP Request — reply if it's for us
        if (arp->target_ip[0] == our_ip[0] && arp->target_ip[1] == our_ip[1] &&
            arp->target_ip[2] == our_ip[2] && arp->target_ip[3] == our_ip[3]) {

            uint8_t frame[ETH_FRAME_MAX];
            uint8_t* mac = rtl8139_get_mac();

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

            rtl8139_send(frame, total);
            serial_puts("[arp] replied to request\n");
        }
    }
}

static uint16_t ip_checksum(struct ip_header* ip) {
    ip->checksum = 0;
    return net_checksum(ip, sizeof(struct ip_header));
}

void icmp_send_ping(uint8_t* target_ip, uint16_t id, uint16_t seq) {
    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = rtl8139_get_mac();

    // Try to resolve target MAC
    uint8_t target_mac[6];
    if (!arp_resolve(target_ip, target_mac)) {
        serial_puts("[icmp] MAC not resolved yet\n");
        arp_send_request(target_ip);
        return;
    }

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
    rtl8139_send(frame, total);
}

void udp_send(uint8_t* dst_ip, uint16_t src_port, uint16_t dst_port, uint8_t* data, uint16_t len) {
    uint8_t frame[ETH_FRAME_MAX];
    uint8_t* mac = rtl8139_get_mac();

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
    rtl8139_send(frame, total);
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
            uint8_t* mac = rtl8139_get_mac();
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

            rtl8139_send(frame, total);
            serial_puts("[icmp] replied to ping\n");
        } else if (icmp->type == 0) { // Echo reply
            serial_puts("[icmp] got ping reply!\n");
        }
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
    rtl8139_init();
    serial_puts("[net] net_init done\n");
    rtl8139_set_rx_callback(handle_packet);

    serial_puts("[net] IP: ");
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        serial_putchar('0' + (our_ip[i] / 10));
        serial_putchar('0' + (our_ip[i] % 10));
        if (i < 3) serial_putchar('.');
    }
    serial_putchar('\n');

    // Send gratuitous ARP to announce ourselves
    arp_send_request(our_ip);
}

uint8_t* net_get_ip(void) { return our_ip; }
uint8_t* net_get_gateway(void) { return gateway_ip; }

void net_set_ip(uint8_t ip0, uint8_t ip1, uint8_t ip2, uint8_t ip3) {
    our_ip[0] = ip0; our_ip[1] = ip1;
    our_ip[2] = ip2; our_ip[3] = ip3;
}

int arp_resolve(uint8_t* ip, uint8_t* mac) {
    if (arp_cache_valid &&
        arp_cache_ip[0] == ip[0] && arp_cache_ip[1] == ip[1] &&
        arp_cache_ip[2] == ip[2] && arp_cache_ip[3] == ip[3]) {
        for (int i = 0; i < 6; i++) mac[i] = arp_cache_mac[i];
        return 1;
    }
    // Broadcast MAC as fallback
    for (int i = 0; i < 6; i++) mac[i] = 0xFF;
    return 0;
}
