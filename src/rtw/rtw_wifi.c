// Wi-Fi station logic on top of the RTL8821C hardware layer: firmware information packets, scanning (passive: beacons and
// probe responses seen on each channel), the scan list and the `wifi` shell command.
#include "rtw.h"
#include <stdarg.h>
#include "rtw_priv.h"
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

// ---- scan list ---------------------------------------------------------------------------------------------------------------------
#define MAXBSS 48
struct bss { uint8_t used, bssid[6], ch, sec; int8_t rssi; char ssid[33]; uint32_t tick; };
static struct bss L[MAXBSS];
static uint32_t beacons;

static struct bss* bss_find(const uint8_t* bssid, int alloc) {
    int free_i = -1, oldest = 0;
    for (int i = 0; i < MAXBSS; i++) {
        if (L[i].used && !memcmp(L[i].bssid, bssid, 6)) return &L[i];
        if (!L[i].used && free_i < 0) free_i = i;
        if (L[i].used && L[i].tick < L[oldest].tick) oldest = i;
    }
    if (!alloc) return 0;
    int i = free_i >= 0 ? free_i : oldest;
    memset(&L[i], 0, sizeof L[i]);
    L[i].used = 1; memcpy(L[i].bssid, bssid, 6);
    return &L[i];
}

void rtw_rx_frame(const uint8_t* f, int len, int rate, int rssi, int icv_err) {
    (void)rate; (void)icv_err;
    if (len < 36) return;
    int type = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (type != 0 || (sub != 8 && sub != 5)) return;                 // beacon or probe response
    const uint8_t* bssid = f + 16;
    struct bss* b = bss_find(bssid, 1);
    beacons++;
    b->tick = tick_count;
    b->rssi = (int8_t)rssi;
    b->ch = R.channel;
    uint16_t cap = (uint16_t)(f[34] | (f[35] << 8));
    int sec = (cap & 0x10) ? 1 : 0;                                   // 1 = WEP / privacy, 2 = WPA, 3 = WPA2
    for (int p = 36; p + 2 <= len;) {
        int id = f[p], l = f[p + 1];
        if (p + 2 + l > len) break;
        const uint8_t* d = f + p + 2;
        if (id == 0) { int n = l > 32 ? 32 : l; memcpy(b->ssid, d, (uint32_t)n); b->ssid[n] = 0; for (int i = 0; i < n; i++) if ((uint8_t)b->ssid[i] < 32 || (uint8_t)b->ssid[i] > 126) b->ssid[i] = '?'; }
        else if (id == 3 && l >= 1) b->ch = d[0];
        else if (id == 48) sec = 3;
        else if (id == 221 && l >= 4 && d[0] == 0 && d[1] == 0x50 && d[2] == 0xf2 && d[3] == 1 && sec < 2) sec = 2;
        p += 2 + l;
    }
    b->sec = (uint8_t)sec;
}

// ---- scanning: visit each channel and listen ----------------------------------------------------------------------------------------
static struct { int active, idx, band_5g; uint32_t until; } S;
static const uint8_t ch24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const uint8_t ch5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165 };

static int scan_total(void) { return (int)(sizeof ch24 + sizeof ch5); }
static int scan_chan(int i) { return i < (int)sizeof ch24 ? ch24[i] : ch5[i - (int)sizeof ch24]; }

void rtw_poll(void) {
    if (R.state != 2) return;
    rtw_rx_poll();
    if (S.active && (int)(tick_count - S.until) >= 0) {
        if (S.idx >= scan_total()) { S.active = 0; wlog("wifi: scan finished, %u beacons seen", beacons); return; }
        rtw_set_channel(scan_chan(S.idx));
        S.idx++;
        S.until = tick_count + 15;                                   // 150 ms per channel: beacons come every ~100 ms
    }
}

// ---- the shell command --------------------------------------------------------------------------------------------------------------------
static int ap(char* buf, int cap, int at, const char* fmt, ...) {
    char line[160];
    va_list va;
    va_start(va, fmt);
    int n = devlog_fmt(line, sizeof line, fmt, va);
    va_end(va);
    for (int i = 0; i < n && at < cap - 2; i++) buf[at++] = line[i];
    if (at < cap - 1) buf[at++] = '\n';
    buf[at] = 0;
    return at;
}

void rtw_command(const char* args, char* buf, int cap) {
    buf[0] = 0;
    int at = 0;
    while (*args == ' ') args++;
    if (!*args || !strcmp(args, "status")) {
        at = ap(buf, cap, at, "wifi: %s%s", R.state == 2 ? "running" : "not started (`wifi start`)", S.active ? ", scanning" : "");
        if (R.state == 2) {
            at = ap(buf, cap, at, "adapter %x:%x, MAC %02x:%02x:%02x:%02x:%02x:%02x, channel %u, tx index %d", R.vid, R.pid, R.mac[0], R.mac[1], R.mac[2], R.mac[3], R.mac[4], R.mac[5], R.channel, R.tx_idx);
            at = ap(buf, cap, at, "rx: %u buffers, %u frames, %u bad, %u crc errors, %u firmware messages; register errors %u", R.rx_bufs, R.rx_frames, R.rx_bad, R.rx_crc, R.c2h_count, R.io_err);
        }
        return;
    }
    if (!strcmp(args, "start")) {
        int r = rtw_start();
        ap(buf, cap, 0, r ? "wifi: start failed (%d); the log (`wifi log`) says where" : "wifi: started", r);
        return;
    }
    if (R.state != 2) { ap(buf, cap, 0, "wifi: not started (`wifi start`)"); return; }
    if (!strcmp(args, "scan")) {
        memset(L, 0, sizeof L); beacons = 0;
        S.active = 1; S.idx = 0; S.until = tick_count;
        ap(buf, cap, 0, "wifi: scanning %d channels (about %d s); `wifi list` shows what was heard", scan_total(), scan_total() * 15 / 100 + 1);
        return;
    }
    if (!strcmp(args, "list")) {
        int n = 0;
        for (int i = 0; i < MAXBSS; i++) {
            if (!L[i].used) continue;
            n++;
            at = ap(buf, cap, at, "%02x:%02x:%02x:%02x:%02x:%02x ch %3u %4d dBm %s  %s", L[i].bssid[0], L[i].bssid[1], L[i].bssid[2], L[i].bssid[3], L[i].bssid[4], L[i].bssid[5],
                    L[i].ch, L[i].rssi, L[i].sec == 3 ? "WPA2" : L[i].sec == 2 ? "WPA " : L[i].sec == 1 ? "WEP?" : "open", L[i].ssid[0] ? L[i].ssid : "(hidden)");
        }
        if (!n) ap(buf, cap, 0, "wifi: nothing heard yet (%u beacons%s)", beacons, S.active ? ", still scanning" : "");
        return;
    }
    if (!strncmp(args, "ch ", 3)) {
        int ch = 0;
        for (const char* p = args + 3; *p >= '0' && *p <= '9'; p++) ch = ch * 10 + (*p - '0');
        int r = rtw_set_channel(ch);
        ap(buf, cap, 0, r ? "wifi: bad channel" : "wifi: channel %d", ch);
        return;
    }
    ap(buf, cap, 0, "wifi: start | status | scan | list | ch N | log");
}
