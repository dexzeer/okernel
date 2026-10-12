// Wi-Fi station glue on top of the RTL8821C hardware layer: firmware information packets, scanning, the connection (wsta.c)
// and the `wifi` shell command; the network stack sees the link through rtw_net_*.
#include "rtw.h"
#include <stdarg.h>
#include "rtw_priv.h"
#include "wsta.h"
#include "../crypto/rand.h"
#include "../devlog.h"
#include "../string.h"
#include "../net/network.h"
#include "../xhci.h"
#include "rtw_pci.h"

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

// ---- the Wi-Fi window's view of things --------------------------------------------------------------------------------------------------
static void start_scan(void) {
    wsta_reset_scan();
    S.active = 1; S.idx = 0; S.until = tick_count;
}

// A simulated adapter for testing the window under QEMU: a fixed set of networks, one scan takes ~1.5 s, a connection ~2 s and
// succeeds for open networks and for the password "demo1234".
static struct { int on, scan_done_at, conn_at, conn_state, pass_ok; char ssid[33]; } demo;
static const struct rtw_ui_bss demo_nets[] = {
    { "Home-Fiber", 6, 3, -48 }, { "Home-Fiber-5G", 36, 3, -52 }, { "CoffeeShop Guest", 1, 0, -63 }, { "AndroidAP-7F2C", 3, 3, -66 },
    { "HP-Printer-1A2B", 1, 3, -70 }, { "Neighbour 2.4", 11, 3, -77 }, { "xfinitywifi", 6, 0, -82 }, { "Old-Router-WEP", 9, 1, -85 },
    { "WPA3-Only-Net", 44, 4, -60 },
};
void rtw_ui_demo(int on) { memset(&demo, 0, sizeof demo); demo.on = on; }

int rtw_ui_adapter(void) {
    if (demo.on || R.state == 2) return 2;
    return (xhci_wl_present(0) || rtwpci_present()) ? 1 : 0;
}

static void copy_str(char* d, int cap, const char* s) { int i = 0; for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }

void rtw_ui_adapter_name(char* buf, int cap) {
    if (demo.on) { copy_str(buf, cap, "Simulated adapter (demo)"); return; }
    if (R.state == 2) {
        char t[64];
        static const char hx[] = "0123456789ABCDEF";
        const char* pre = R.hci ? "Realtek PCIe adapter " : "Realtek USB adapter ";
        int n = 0;
        for (; pre[n]; n++) t[n] = pre[n];
        for (int i = 0; i < 6; i++) { t[n++] = hx[R.mac[i] >> 4]; t[n++] = hx[R.mac[i] & 15]; if (i < 5) t[n++] = ':'; }
        t[n] = 0;
        copy_str(buf, cap, t);
        return;
    }
    copy_str(buf, cap, xhci_wl_present(0) ? "Realtek USB adapter (not started)" : rtwpci_present() ? "Realtek PCIe adapter (not started)" : "No Wi-Fi adapter found");
}

int rtw_ui_start(void) {
    if (demo.on || R.state == 2) return 0;
    int r = rtw_start();
    if (!r && !sta_ready) { wsta_init(R.mac, &OPS); sta_ready = 1; }
    return r;
}

void rtw_ui_scan(void) {
    if (demo.on) { demo.scan_done_at = (int)(tick_count + 150); return; }
    if (R.state != 2 || S.active) return;
    if (wsta_state() == WSTA_UP || wsta_state() == WSTA_HANDSHAKE) return;
    start_scan();
}

int rtw_ui_scanning(void) {
    if (demo.on) return demo.scan_done_at && (int)(tick_count - (uint32_t)demo.scan_done_at) < 0;
    return S.active;
}

int rtw_ui_list(struct rtw_ui_bss* out, int max) {
    int n = 0;
    if (demo.on) {
        if (!demo.scan_done_at || rtw_ui_scanning()) return 0;
        for (int i = 0; i < (int)(sizeof demo_nets / sizeof demo_nets[0]) && n < max; i++) out[n++] = demo_nets[i];
        return n;
    }
    for (int i = 0; i < wsta_bss_count() && n < max; i++) {
        const struct wsta_bss* b = wsta_bss_at(i);
        if (!b->ssid_len || b->ssid[0] == 0) continue;
        int dup = -1;
        for (int k = 0; k < n; k++) if (!strcmp(out[k].ssid, b->ssid)) dup = k;
        if (dup >= 0) { if (b->rssi > out[dup].rssi) { out[dup].rssi = b->rssi; out[dup].ch = b->ch; out[dup].sec = b->sec; } continue; }
        memcpy(out[n].ssid, b->ssid, 33); out[n].ch = b->ch; out[n].sec = b->sec; out[n].rssi = b->rssi;
        n++;
    }
    for (int i = 1; i < n; i++) {                                          // strongest first
        struct rtw_ui_bss x = out[i];
        int k = i - 1;
        while (k >= 0 && out[k].rssi < x.rssi) { out[k + 1] = out[k]; k--; }
        out[k + 1] = x;
    }
    return n;
}

static void put_ip(char* o, const uint8_t* ip) {
    int n = 0;
    for (int i = 0; i < 4; i++) {
        int v = ip[i];
        if (v >= 100) o[n++] = (char)('0' + v / 100);
        if (v >= 10) o[n++] = (char)('0' + (v / 10) % 10);
        o[n++] = (char)('0' + v % 10);
        if (i < 3) o[n++] = '.';
    }
    o[n] = 0;
}

void rtw_ui_link_info(struct rtw_ui_link* l) {
    memset(l, 0, sizeof *l);
    if (demo.on) {
        copy_str(l->ssid, sizeof l->ssid, demo.ssid);
        if (demo.conn_state == 1) {
            if ((int)(tick_count - (uint32_t)demo.conn_at) < 200) { l->state = RTW_LINK_BUSY; copy_str(l->msg, sizeof l->msg, "Connecting..."); }
            else demo.conn_state = demo.pass_ok ? 2 : 3;
        }
        if (demo.conn_state == 2) { l->state = RTW_LINK_UP; copy_str(l->ip, sizeof l->ip, "192.168.50.23"); copy_str(l->gateway, sizeof l->gateway, "192.168.50.1"); }
        else if (demo.conn_state == 3) { l->state = RTW_LINK_FAILED; copy_str(l->msg, sizeof l->msg, "Wrong password, or the key exchange failed"); }
        return;
    }
    if (R.state != 2) return;
    int st = wsta_state();
    copy_str(l->ssid, sizeof l->ssid, wsta_ssid());
    if (st == WSTA_AUTH) { l->state = RTW_LINK_BUSY; copy_str(l->msg, sizeof l->msg, "Authenticating..."); }
    else if (st == WSTA_ASSOC) { l->state = RTW_LINK_BUSY; copy_str(l->msg, sizeof l->msg, "Joining the network..."); }
    else if (st == WSTA_HANDSHAKE) { l->state = RTW_LINK_BUSY; copy_str(l->msg, sizeof l->msg, "Checking the password..."); }
    else if (st == WSTA_UP) {
        if (net_wifi_bound()) { l->state = RTW_LINK_UP; put_ip(l->ip, net_get_ip()); put_ip(l->gateway, net_get_gateway()); }
        else { l->state = RTW_LINK_BUSY; copy_str(l->msg, sizeof l->msg, "Getting an address..."); }
    } else if (st == WSTA_FAILED) { l->state = RTW_LINK_FAILED; copy_str(l->msg, sizeof l->msg, wsta_last_error() ? wsta_last_error() : "Could not connect"); }
}

void rtw_ui_connect(const char* ssid, const char* pass) {
    if (demo.on) {
        copy_str(demo.ssid, sizeof demo.ssid, ssid);
        int open = 0;
        for (int i = 0; i < (int)(sizeof demo_nets / sizeof demo_nets[0]); i++) if (!strcmp(demo_nets[i].ssid, ssid)) open = demo_nets[i].sec == 0;
        demo.pass_ok = open || !strcmp(pass, "demo1234");
        demo.conn_state = 1; demo.conn_at = (int)tick_count;
        return;
    }
    if (R.state != 2 || S.active) return;
    wsta_connect(ssid, pass);
}

void rtw_ui_disconnect(void) {
    if (demo.on) { demo.conn_state = 0; return; }
    if (R.state == 2) wsta_disconnect();
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
        start_scan();
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
