// Wi-Fi station glue on top of the RTL8821C hardware layer: firmware information packets, scanning, the connection (wsta.c)
// and the `wifi` shell command; the network stack sees the link through rtw_net_*.
#include "rtw.h"
#include <stdarg.h>
#include "rtw_priv.h"
#include "wsta.h"
#include "../crypto/rand.h"
#include "../devlog.h"
#include "../string.h"

extern uint32_t tick_count;

// ---- messages to the firmware after power-up ---------------------------------------------------------------------------------
static void h2c_header(uint8_t* p, uint8_t sub, uint16_t total) {
    p[0] = 1;                 // category
    p[1] = 0xFF;              // command id
    p[2] = sub; p[3] = 0;
    p[4] = (uint8_t)total; p[5] = (uint8_t)(total >> 8);
}

void rtw_send_fw_info(void) {
    uint8_t p[32];
    memset(p, 0, sizeof p);
    h2c_header(p, 0x0D, 8 + 4);                                    // general information: where the firmware's TX buffer starts
    p[10] = (uint8_t)(R.rsvd_fw_txbuf - R.rsvd_boundary);
    int r = rtw_h2c_pkt(p);
    memset(p, 0, sizeof p);
    h2c_header(p, 0x11, 8 + 8);                                    // PHY information: RFE type, 1T1R, chip cut, antennas
    p[8] = R.rfe; p[9] = (R.chip_ver & (1u << 27)) ? 2 : 4; p[10] = R.cut; p[11] = 0x11;
    r |= rtw_h2c_pkt(p);
    if (r) wlog("wifi: firmware info packets failed");
}

void rtw_c2h(const uint8_t* p, int len) {
    R.c2h_count++;
    if (R.c2h_count <= 12 && len >= 2) wlog("wifi: firmware message id %02x seq %02x len %d", p[0], p[1], len);
}

// ---- hooks for the network stack ------------------------------------------------------------------------------------------------------
static void (*eth_cb)(const uint8_t*, int);
void rtw_net_set_rx(void (*cb)(const uint8_t*, int)) { eth_cb = cb; }
int rtw_net_up(void) { return R.state == 2 && wsta_state() == WSTA_UP; }
const uint8_t* rtw_net_mac(void) { return R.mac; }
int rtw_net_send(const uint8_t* eth, int len) { return R.state == 2 ? wsta_send_eth(eth, len) : -1; }

// ---- services for the station logic ------------------------------------------------------------------------------------------------
static int op_tx(const uint8_t* f, int len, int is_data) { return rtw_tx_frame(f, len, is_data); }
static void op_chan(int ch) { rtw_set_channel(ch); rtw_iqk(); }
static void op_link(int up, const uint8_t* bssid, int aid) { rtw_set_link(up, bssid, aid); }
static void op_eth(const uint8_t* e, int len) { if (eth_cb) eth_cb(e, len); }
static uint32_t op_now(void) { return tick_count * 10; }
static void op_rand(uint8_t* o, int n) {
    if (!rand_bytes(o, (uint32_t)n)) {                              // the kernel generator is not seeded yet: mix what we have
        uint32_t x = tick_count * 2654435761u ^ (uint32_t)(R.mac[5] | (R.mac[4] << 8));
        for (int i = 0; i < n; i++) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); x = x * 1664525u + 1013904223u + lo; o[i] = (uint8_t)(x >> 16); }
    }
}
static void op_log(const char* fmt, ...) {
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    devlog_fmt(b, sizeof b, fmt, ap);
    va_end(ap);
    devlog('w', "%s", b);
}
static const struct wsta_ops OPS = { op_tx, op_chan, op_link, op_eth, op_now, op_rand, op_log };
static int sta_ready;

void rtw_rx_frame(const uint8_t* f, int len, int rate, int rssi, int icv_err) {
    (void)rate; (void)icv_err;
    if (sta_ready) wsta_rx(f, len, rssi, R.channel);
}

// ---- scanning: visit each channel and listen -----------------------------------------------------------------------------------------
static struct { int active, idx; uint32_t until; } S;
static const uint8_t ch24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const uint8_t ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165 };
static int scan_total(void) { return (int)(sizeof ch24 + sizeof ch5); }
static int scan_chan(int i) { return i < (int)sizeof ch24 ? ch24[i] : ch5[i - (int)sizeof ch24]; }

void rtw_poll(void) {
    if (R.state != 2) return;
    if (!sta_ready) { wsta_init(R.mac, &OPS); sta_ready = 1; }
    rtw_rx_poll();
    wsta_poll();
    if (S.active && (int)(tick_count - S.until) >= 0) {
        if (S.idx >= scan_total()) { S.active = 0; wlog("wifi: scan finished, %d networks", wsta_bss_count()); return; }
        rtw_set_channel(scan_chan(S.idx));
        S.idx++;
        S.until = tick_count + 15;                                   // 150 ms per channel: beacons come every ~100 ms
    }
}

// ---- the shell command --------------------------------------------------------------------------------------------------------------------
static int ap(char* buf, int cap, int at, const char* fmt, ...) {
    char line[200];
    va_list va;
    va_start(va, fmt);
    int n = devlog_fmt(line, sizeof line, fmt, va);
    va_end(va);
    for (int i = 0; i < n && at < cap - 2; i++) buf[at++] = line[i];
    if (at < cap - 1) buf[at++] = '\n';
    buf[at] = 0;
    return at;
}

// "NAME PASS" or "NAME WITH SPACES" PASS: splits into ssid and pass phrase
static void split_args(const char* a, char* ssid, int scap, char* pass, int pcap) {
    int i = 0, n = 0;
    ssid[0] = pass[0] = 0;
    if (*a == '"') {
        a++;
        while (*a && *a != '"' && n < scap - 1) ssid[n++] = *a++;
        if (*a == '"') a++;
    } else {
        while (*a && *a != ' ' && n < scap - 1) ssid[n++] = *a++;
    }
    ssid[n] = 0;
    while (*a == ' ') a++;
    while (*a && i < pcap - 1) pass[i++] = *a++;
    pass[i] = 0;
}

void rtw_command(const char* args, char* buf, int cap) {
    buf[0] = 0;
    int at = 0;
    while (*args == ' ') args++;
    if (!*args || !strcmp(args, "status")) {
        at = ap(buf, cap, at, "wifi: %s%s", R.state == 2 ? "adapter running" : "not started (`wifi start`)", S.active ? ", scanning" : "");
        if (R.state == 2) {
            uint32_t tx, rx, bad;
            wsta_stats(&tx, &rx, &bad);
            const uint8_t* b = wsta_bssid();
            at = ap(buf, cap, at, "adapter %x:%x, MAC %02x:%02x:%02x:%02x:%02x:%02x, channel %u, tx index %d", R.vid, R.pid, R.mac[0], R.mac[1], R.mac[2], R.mac[3], R.mac[4], R.mac[5], R.channel, R.tx_idx);
            at = ap(buf, cap, at, "link: %s%s%s", wsta_state_name(), wsta_state() >= 1 ? " to " : "", wsta_state() >= 1 ? wsta_ssid() : "");
            if (wsta_state() >= 1) at = ap(buf, cap, at, "bssid %02x:%02x:%02x:%02x:%02x:%02x; frames sent %u, received %u, rejected %u", b[0], b[1], b[2], b[3], b[4], b[5], tx, rx, bad);
            at = ap(buf, cap, at, "radio rx: %u buffers, %u frames, %u bad, %u crc errors, %u firmware messages; register errors %u", R.rx_bufs, R.rx_frames, R.rx_bad, R.rx_crc, R.c2h_count, R.io_err);
        }
        return;
    }
    if (!strcmp(args, "start")) {
        int r = rtw_start();
        if (!r && !sta_ready) { wsta_init(R.mac, &OPS); sta_ready = 1; }
        ap(buf, cap, 0, r ? "wifi: start failed (%d); the log (`wifi log`) says where" : "wifi: started", r);
        return;
    }
    if (R.state != 2) { ap(buf, cap, 0, "wifi: not started (`wifi start`)"); return; }
    if (!strcmp(args, "scan")) {
        if (wsta_state() == WSTA_UP || wsta_state() == WSTA_HANDSHAKE) { ap(buf, cap, 0, "wifi: disconnect first (`wifi disconnect`)"); return; }
        wsta_reset_scan();
        S.active = 1; S.idx = 0; S.until = tick_count;
        ap(buf, cap, 0, "wifi: scanning %d channels (about %d s); `wifi list` shows what was heard", scan_total(), scan_total() * 15 / 100 + 1);
        return;
    }
    if (!strcmp(args, "list")) {
        int n = wsta_bss_count();
        for (int i = 0; i < n; i++) {
            const struct wsta_bss* b = wsta_bss_at(i);
            at = ap(buf, cap, at, "%02x:%02x:%02x:%02x:%02x:%02x ch %3u %4d dBm %s  %s", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
                    b->ch, b->rssi, b->sec == 3 ? "WPA2" : b->sec == 4 ? "n/a " : b->sec == 2 ? "WPA " : b->sec == 1 ? "WEP?" : "open", b->ssid_len ? b->ssid : "(hidden)");
        }
        if (!n) ap(buf, cap, 0, "wifi: nothing heard yet%s", S.active ? " (still scanning)" : " (`wifi scan`)");
        return;
    }
    if (!strncmp(args, "connect ", 8)) {
        char ssid[40], pass[70];
        split_args(args + 8, ssid, sizeof ssid, pass, sizeof pass);
        if (S.active) { ap(buf, cap, 0, "wifi: wait for the scan to finish"); return; }
        int r = wsta_connect(ssid, pass);
        ap(buf, cap, 0, r ? "wifi: cannot join %s (%d); `wifi list`, `wifi log`" : "wifi: joining %s ... (`wifi status` shows progress)", ssid, r);
        return;
    }
    if (!strcmp(args, "disconnect")) { wsta_disconnect(); ap(buf, cap, 0, "wifi: disconnected"); return; }
    if (!strncmp(args, "ch ", 3)) {
        int ch = 0;
        for (const char* p = args + 3; *p >= '0' && *p <= '9'; p++) ch = ch * 10 + (*p - '0');
        int r = rtw_set_channel(ch);
        ap(buf, cap, 0, r ? "wifi: bad channel" : "wifi: channel %d", ch);
        return;
    }
    ap(buf, cap, 0, "wifi: start | scan | list | connect NAME [PASSWORD] | disconnect | status | ch N | log");
}
