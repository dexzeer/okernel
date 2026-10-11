// Host test for the Realtek Wi-Fi driver (src/rtw): the whole bring-up (power sequence, firmware download, efuse, MAC / PHY
// tables, channel switch) runs against a software model of the chip behind the USB calls, then received beacons go through
// the receive path into the scan list. The model encodes what we know of the hardware, so it checks the driver's own logic
// (loops that end, buffer sizes, register arithmetic, table conditions), not the hardware.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "../src/xhci.h"
char* strstr(const char*, const char*);

__asm__(".section .rodata\n.global rtw_fw_start\nrtw_fw_start:\n.incbin \"firmware/rtw8821c_fw.bin\"\n.global rtw_fw_end\nrtw_fw_end:\n.byte 0\n.text\n");

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- kernel services the driver uses --------------------------------------------------------------------------------
uint32_t tick_count;
void pit_delay_us(uint32_t us) { tick_count += us / 10000; (void)us; }
void* kmalloc(uint32_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
int devlog_fmt(char* out, int cap, const char* fmt, va_list ap);
static int vlog_n;
int devlog_fmt(char* out, int cap, const char* fmt, va_list ap) { return vsnprintf(out, (size_t)cap, fmt, ap); }
void devlog(char tag, const char* fmt, ...) {
    char b[200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    vlog_n++;
    if (getenv("RTW_LOG")) printf("[%c] %s\n", tag, b);
}
int devlog_tail(char tag, char* buf, int cap, int at, int maxlines) { (void)tag; (void)buf; (void)cap; (void)maxlines; return at; }

// ---- the chip model ---------------------------------------------------------------------------------------------------
static uint8_t regs[0x10000];
static uint8_t phy_efuse[512];
static int ef_pending;
static uint32_t writes_808, lte_ctrl;
static uint8_t dmem_img[0x10000], imem_img[0x20000], emem_img[0x20000];
static uint32_t dl_sa, dl_da;
static uint8_t last_page[8192]; static int last_page_len;
static int bulk_calls, wl_present = 1, nout = 3;
static const uint8_t* rx_q[8]; static int rx_len[8], rx_n, rx_i;

static uint32_t rd32m(uint32_t a) { return (uint32_t)regs[a] | ((uint32_t)regs[a + 1] << 8) | ((uint32_t)regs[a + 2] << 16) | ((uint32_t)regs[a + 3] << 24); }
static void wr32m(uint32_t a, uint32_t v) { regs[a] = (uint8_t)v; regs[a + 1] = (uint8_t)(v >> 8); regs[a + 2] = (uint8_t)(v >> 16); regs[a + 3] = (uint8_t)(v >> 24); }

static void model_read_hook(uint32_t a, int len) {
    (void)len;
    if (a == 0x0005) regs[0x0005] &= (uint8_t)~3;                                   // self-clearing handshake bits
    if (a == 0x0006) regs[0x0006] |= 2;
    if (a == 0x0080 || a == 0x0081) {                                               // firmware status: ready once the CPU runs the downloaded image
        uint32_t v = rd32m(0x80);
        if ((v & (1u << 14)) && (regs[0x03] & 4)) wr32m(0x80, v | (1u << 15) | (1u << 3) | (1u << 5) | (1u << 4) | (1u << 6));
    }
    if (a == 0x0030 && ef_pending) {                                                // efuse byte arrives
        uint32_t v = rd32m(0x30);
        uint32_t ad = (v >> 8) & 0x3ff;
        wr32m(0x30, (v & ~0xffu) | phy_efuse[ad % 512] | (1u << 31));
        ef_pending = 0;
    }
    if (a == 0x1700) wr32m(0x1700, rd32m(0x1700) | (1u << 29));
    if (a == 0x1708) wr32m(0x1708, rd32m(0x1704));
}

static void model_write_hook(uint32_t a, int len) {
    (void)len;
    if (a == 0x0006 && (regs[0x0006] & 1)) regs[0x100] = 0;                         // powered on
    if (a == 0x0005 && (regs[0x0005] & 2)) regs[0x100] = 0xEA;                      // powered off
    if (a == 0x0030) { if (!(rd32m(0x30) & (1u << 31))) ef_pending = 1; }
    if (a == 0x1208) {                                                              // DDMA channel 0
        uint32_t c = rd32m(0x1208);
        if (c & (1u << 31)) {
            dl_sa = rd32m(0x1200); dl_da = rd32m(0x1204);
            uint32_t n = c & 0x3ffff;
            if (dl_sa != 0x18780000u + 48) printf("FAIL: DDMA source %08x\n", dl_sa), fails++;
            if (n > (uint32_t)last_page_len - 48 + 1) printf("FAIL: DDMA length %u > page payload\n", n), fails++;
            uint8_t* dst = dl_da >= 0x200000 ? dmem_img + (dl_da - 0x200000) : dl_da >= 0x100000 ? emem_img + (dl_da - 0x100000) : imem_img + (dl_da - 0x30000);
            memcpy(dst, last_page + 48, n);
            wr32m(0x1208, c & ~(1u << 31));
        }
    }
    if (a == 0x0208 && (regs[0x208] & 1)) regs[0x208] &= (uint8_t)~1;               // automatic LLT init done
    if (a == 0x0003 || a == 0x0002) {}
}

int xhci_wl_present(struct xhci_wl_info* i) {
    if (!wl_present) return 0;
    i->vid = 0x0bda; i->pid = 0xc811; i->speed = 4; i->nout = nout;
    return 1;
}
int xhci_wl_ctrl(int in, uint8_t req, uint16_t value, uint16_t index, void* buf, uint16_t len) {
    (void)index;
    if (req != 5) return -1;
    if (in) { model_read_hook(value, len); memcpy(buf, regs + value, len); }
    else { memcpy(regs + value, buf, len); model_write_hook(value, len); }
    return len;
}
int xhci_wl_bulk_out(int ep, const void* data, uint32_t len) {
    (void)ep;
    bulk_calls++;
    if (getenv("RTW_LOG") && bulk_calls < 6) printf("bulk ep %d len %u\n", ep, len);
    const uint8_t* d = data;
    if (len < 48) return -1;
    uint32_t w0 = (uint32_t)d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24), w1 = (uint32_t)d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    uint32_t qsel = (w1 >> 8) & 0x1f;
    CHECK((w0 & 0xffff) == len - 48 || qsel == 19);
    uint16_t x = 0;
    for (int i = 0; i < 16; i++) x ^= (uint16_t)(d[2 * i] | (d[2 * i + 1] << 8));
    CHECK(x == 0);                                                                   // the descriptor checksum folds to zero
    if (qsel == 16) { memcpy(last_page, d, len < sizeof last_page ? len : sizeof last_page); last_page_len = (int)len; wr32m(0x204, rd32m(0x204) | (1u << 15)); }
    return 0;
}
void xhci_wl_rx_start(void) {}
int xhci_wl_rx_get(const uint8_t** data) { if (rx_i >= rx_n) return 0; *data = rx_q[rx_i]; return rx_len[rx_i]; }
void xhci_wl_rx_done(void) { rx_i++; }
void xhci_wl_pump(void) {}

#include "../src/rtw/rtw_hw.c"
#include "../src/rtw/rtw_wifi.c"

// ---- helpers ------------------------------------------------------------------------------------------------------------
static void build_efuse(const uint8_t* logical) {
    memset(phy_efuse, 0xff, sizeof phy_efuse);
    int pi = 0;
    for (int blk = 0; blk < 64; blk++) {
        int any = 0;
        for (int i = 0; i < 8; i++) if (logical[blk * 8 + i] != 0xff) any = 1;
        if (!any) continue;
        phy_efuse[pi++] = (uint8_t)(((blk & 7) << 5) | 0xF);
        phy_efuse[pi++] = (uint8_t)(((blk >> 3) << 4) | 0);                          // all four words present
        for (int i = 0; i < 8; i++) phy_efuse[pi++] = logical[blk * 8 + i];
    }
}

static int put_beacon(uint8_t* out, const char* ssid, int ch, const uint8_t* bssid, int rsn) {
    uint8_t* d = out;
    memset(d, 0, 24);
    // RX descriptor + PHY status (32 bytes) + frame + FCS
    uint8_t f[200]; int n = 0;
    f[n++] = 0x80; f[n++] = 0; f[n++] = 0; f[n++] = 0;
    memset(f + n, 0xff, 6); n += 6;
    memcpy(f + n, bssid, 6); n += 6; memcpy(f + n, bssid, 6); n += 6;
    f[n++] = 0; f[n++] = 0;
    memset(f + n, 0, 8); n += 8;
    f[n++] = 0x64; f[n++] = 0; f[n++] = 0x11 | (rsn ? 0x10 : 0); f[n++] = 0;
    f[n++] = 0; f[n++] = (uint8_t)strlen(ssid); memcpy(f + n, ssid, strlen(ssid)); n += (int)strlen(ssid);
    f[n++] = 3; f[n++] = 1; f[n++] = (uint8_t)ch;
    if (rsn) { f[n++] = 48; f[n++] = 2; f[n++] = 1; f[n++] = 0; }
    f[n++] = 0xde; f[n++] = 0xad; f[n++] = 0xbe; f[n++] = 0xef;                         // FCS
    uint32_t w0 = (uint32_t)n | (4u << 16) | (1u << 26);                                 // length, 32 bytes of PHY status, PHY status present
    d[0] = (uint8_t)w0; d[1] = (uint8_t)(w0 >> 8); d[2] = (uint8_t)(w0 >> 16); d[3] = (uint8_t)(w0 >> 24);
    d[12] = 4;                                                                           // rate 6 Mbit/s
    uint8_t* ps = d + 24; memset(ps, 0, 32); ps[0] = 1; ps[1] = 60;                      // page 1, 60 -> -50 dBm
    memcpy(ps + 32, f, (size_t)n);
    return 24 + 32 + n;
}

int main(void) {
    // efuse: rfe 0, MAC 02:11:22:33:44:55 at 0x107, a plausible TX power table
    uint8_t lg[512];
    memset(lg, 0xff, sizeof lg);
    lg[0xca] = 0; lg[0xb9] = 0x2a; lg[0xc6] = 0; lg[0xc7] = 0;
    for (int i = 0; i < 6; i++) lg[0x10 + i] = (uint8_t)(0x2d + i);
    for (int i = 0; i < 5; i++) lg[0x16 + i] = (uint8_t)(0x28 + i);
    lg[0x1b] = 0x0f;
    for (int i = 0; i < 14; i++) lg[0x22 + i] = (uint8_t)(0x26 + i);
    const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(lg + 0x107, mac, 6);
    build_efuse(lg);
    regs[0x100] = 0xEA;                                                              // powered off
    wr32m(0xF0, (0x2u << 12) | (1u << 23));                                          // chip version register: cut 2, 1T1R

    int r = rtw_start();
    CHECK(r == 0);
    CHECK(R.state == 2);
    CHECK(!memcmp(R.mac, mac, 6));
    CHECK(R.rfe == 0 && R.xtal == 0x2a);
    CHECK(R.pwr_cck[0] == 0x2d && R.pwr_5g[13] == 0x26 + 13);
    CHECK(R.ofdm_diff_2g == -1);                                                     // 0xf = -1 as a signed nibble
    CHECK(R.rsvd_boundary == 460 && R.rsvd_fw_txbuf == 508);
    CHECK(R.io_err == 0);
    // the firmware image arrived in chip memory exactly as in the file
    const uint8_t* fw = rtw_fw_start;
    uint32_t dsz = ((uint32_t)fw[0x24] | (fw[0x25] << 8) | (fw[0x26] << 16)) + 8, isz = ((uint32_t)fw[0x30] | (fw[0x31] << 8) | (fw[0x32] << 16)) + 8;
    uint32_t esz = ((uint32_t)fw[0x34] | (fw[0x35] << 8) | (fw[0x36] << 16)) + 8;
    CHECK(!memcmp(dmem_img, fw + 64, dsz));
    CHECK(!memcmp(imem_img, fw + 64 + dsz, isz));
    CHECK(!memcmp(emem_img, fw + 64 + dsz + isz, esz));
    // MAC / PHY: spot checks of what the tables and the channel code wrote
    CHECK(regs[0x420] == 0x80 && regs[0x421] == 0x1F);                               // from the MAC table
    CHECK(rd32m(0x608) == (0xE400220Eu | (1u << 28)));                               // RCR
    CHECK(rd32m(0xC90) != 0);                                                        // an RF register was written last through the serial interface
    CHECK(R.channel == 1);
    CHECK(regs[0x1d00] == 0x2d && regs[0x1d04] == 0x28 + 0 - 1);                     // CCK base and OFDM base + signed OFDM offset

    // channel switching across both bands
    CHECK(rtw_set_channel(11) == 0 && R.channel == 11);
    CHECK(rtw_set_channel(36) == 0 && R.channel == 36 && (regs[0x454] & 0x80));
    CHECK(rtw_set_channel(6) == 0 && !(regs[0x454] & 0x80));
    CHECK(rtw_set_channel(0) < 0 && rtw_set_channel(300) < 0);

    // receive path: two beacons in one USB buffer, then a scan listing
    static uint8_t buf[600];
    const uint8_t b1[6] = { 0xaa, 0xbb, 0xcc, 0, 0, 1 }, b2[6] = { 0xaa, 0xbb, 0xcc, 0, 0, 2 };
    int n1 = put_beacon(buf, "home-net", 6, b1, 1);
    int pad = (n1 + 7) & ~7;
    int n2 = put_beacon(buf + pad, "open cafe", 11, b2, 0);
    rx_q[0] = buf; rx_len[0] = pad + n2; rx_n = 1; rx_i = 0;
    char out[2048];
    rtw_command("scan", out, sizeof out);
    CHECK(strstr(out, "scanning"));
    // the scan itself drives the channel; our fake RX returns the same buffer once
    for (int i = 0; i < 400; i++) { tick_count += 20; rtw_poll(); }
    rtw_command("list", out, sizeof out);
    CHECK(strstr(out, "home-net") && strstr(out, "WPA2"));
    CHECK(strstr(out, "open cafe") && strstr(out, "open"));
    CHECK(R.rx_frames == 2);
    rtw_command("status", out, sizeof out);
    CHECK(strstr(out, "02:11:22:33:44:55"));

    // no adapter: a clean failure
    wl_present = 0;
    CHECK(rtw_start() < 0);

    printf(fails ? "RTW FAIL (%d)\n" : "RTW PASS\n", fails);
    return fails != 0;
}
