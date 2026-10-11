// Realtek RTL8821C hardware layer over USB: register access, power sequences, firmware download, efuse, MAC / baseband /
// radio initialisation, channel switching and the frame receive path. See rtw.h.
#include "rtw.h"
#include <stdarg.h>
#include "rtw_regs.h"
#include "rtw_priv.h"
#include "../devlog.h"
#include "../lapic.h"
#include "../memory.h"
#include "../string.h"
#include "../xhci.h"

extern uint32_t tick_count;
extern const uint8_t rtw_fw_start[], rtw_fw_end[];
extern const uint32_t rtw_mac_tbl[], rtw_mac_tbl_n, rtw_agc_tbl[], rtw_agc_tbl_n, rtw_agc_btg2_tbl[], rtw_agc_btg2_tbl_n;
extern const uint32_t rtw_bb_tbl[], rtw_bb_tbl_n, rtw_rf_a_tbl[], rtw_rf_a_tbl_n;

struct rtw R;

// ---- the adapters we drive -------------------------------------------------------------------------------------
int rtw_usb_id_match(uint16_t vid, uint16_t pid) {
    static const struct { uint16_t v, p; } ids[] = {
        { 0x0bda, 0x2006 }, { 0x0bda, 0x8731 }, { 0x0bda, 0x8811 }, { 0x0bda, 0xb820 }, { 0x0bda, 0xb82b }, { 0x0bda, 0xc80c },
        { 0x0bda, 0xc811 }, { 0x0bda, 0xc820 }, { 0x0bda, 0xc821 }, { 0x0bda, 0xc82a }, { 0x0bda, 0xc82b }, { 0x2001, 0x331d },
        { 0x7392, 0xc811 }, { 0x7392, 0xd811 }, { 0x2c4e, 0x0105 },
    };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) if (ids[i].v == vid && ids[i].p == pid) return 1;
    return 0;
}

// ---- time ---------------------------------------------------------------------------------------------------
void rtw_udelay(uint32_t us) { pit_delay_us(us); }
void rtw_mdelay(uint32_t ms) { while (ms--) pit_delay_us(1000); }

// ---- register access (vendor control transfers: request 5, wValue = register, 1/2/4 bytes) -------------------------
// After an access to the always-on register section the vendor driver writes the value's low byte to 0x4E0.
static void reg_sec(uint32_t addr, uint8_t d) {
    if (addr <= 0xff || (addr >= 0x1000 && addr <= 0x10ff)) xhci_wl_ctrl(0, 5, R_REG_SEC, 0, &d, 1);
}

static uint32_t rd(uint32_t addr, int len) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    int r = xhci_wl_ctrl(1, 5, (uint16_t)addr, 0, b, (uint16_t)len);
    if (r < 0) { R.io_err++; return 0xFFFFFFFFu >> (8 * (4 - len)); }
    uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    reg_sec(addr, b[0]);
    return v;
}

static void wr(uint32_t addr, uint32_t v, int len) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    if (xhci_wl_ctrl(0, 5, (uint16_t)addr, 0, b, (uint16_t)len) < 0) { R.io_err++; return; }
    reg_sec(addr, b[0]);
}

uint8_t rtw_r8(uint32_t a) { return (uint8_t)rd(a, 1); }
uint16_t rtw_r16(uint32_t a) { return (uint16_t)rd(a, 2); }
uint32_t rtw_r32(uint32_t a) { return rd(a, 4); }
void rtw_w8(uint32_t a, uint8_t v) { wr(a, v, 1); }
void rtw_w16(uint32_t a, uint16_t v) { wr(a, v, 2); }
void rtw_w32(uint32_t a, uint32_t v) { wr(a, v, 4); }
void rtw_w8_set(uint32_t a, uint8_t m) { rtw_w8(a, (uint8_t)(rtw_r8(a) | m)); }
void rtw_w8_clr(uint32_t a, uint8_t m) { rtw_w8(a, (uint8_t)(rtw_r8(a) & ~m)); }
void rtw_w16_set(uint32_t a, uint16_t m) { rtw_w16(a, (uint16_t)(rtw_r16(a) | m)); }
void rtw_w32_set(uint32_t a, uint32_t m) { rtw_w32(a, rtw_r32(a) | m); }
void rtw_w32_clr(uint32_t a, uint32_t m) { rtw_w32(a, rtw_r32(a) & ~m); }
void rtw_w32_mask(uint32_t a, uint32_t mask, uint32_t v) {
    uint32_t o = rtw_r32(a);
    int sh = __builtin_ctz(mask);
    rtw_w32(a, (o & ~mask) | ((v << sh) & mask));
}
uint32_t rtw_r32_mask(uint32_t a, uint32_t mask) { return (rtw_r32(a) & mask) >> __builtin_ctz(mask); }

static int hw_ready(uint32_t addr, uint32_t mask, uint32_t target, int tries) {
    for (int i = 0; i < tries; i++) {
        if (rtw_r32_mask(addr, mask) == target) return 1;
        rtw_udelay(10);
    }
    return 0;
}

// ---- RF registers (path A): read directly at 0x2800 + 4*reg, write through the serial interface at 0xC90 ----------------
uint32_t rtw_rf_read(uint32_t reg, uint32_t mask) {
    reg &= 0xff;
    return rtw_r32_mask(0x2800 + (reg << 2), mask & 0xfffff);
}

void rtw_rf_write(uint32_t reg, uint32_t mask, uint32_t data) {
    reg &= 0xff; mask &= 0xfffff;
    if (mask != 0xfffff) {
        uint32_t old = rtw_rf_read(reg, 0xfffff);
        int sh = __builtin_ctz(mask);
        data = (old & ~mask) | (data << sh);
    }
    rtw_w32(0xC90, ((reg << 20) | (data & 0x000fffff)) & 0x0fffffffu);
    rtw_udelay(13);
}

// ---- power sequences (the USB-relevant steps of the chip's power-on / power-off flows) ------------------------------
enum { P_W, P_POLL, P_DELAY_MS };
struct pcmd { uint16_t reg; uint8_t op, mask, val; };

static const struct pcmd pwr_on_seq[] = {
    // card disable -> card emulation
    { 0x004A, P_W, B(0), 0 },
    { 0x0005, P_W, B(3) | B(4) | B(7), 0 },
    // card emulation -> active
    { 0x0020, P_W, B(0), B(0) },
    { 0x0001, P_DELAY_MS, 1, 0 },
    { 0x0000, P_W, B(5), 0 },
    { 0x0005, P_W, B(4) | B(3) | B(2), 0 },
    { 0x0006, P_POLL, B(1), B(1) },
    { 0x0006, P_W, B(0), B(0) },
    { 0x0005, P_W, B(7), 0 },
    { 0x0005, P_W, B(4) | B(3), 0 },
    { 0x10C3, P_W, B(0), B(0) },
    { 0x0005, P_W, B(0), B(0) },
    { 0x0005, P_POLL, B(0), 0 },
    { 0x0020, P_W, B(3), B(3) },
    { 0x007C, P_W, B(1), 0 },
};

static const struct pcmd pwr_off_seq[] = {
    // active -> card emulation
    { 0x0093, P_W, B(3), 0 },
    { 0x001F, P_W, 0xFF, 0 },
    { 0x0049, P_W, B(1), 0 },
    { 0x0006, P_W, B(0), B(0) },
    { 0x0002, P_W, B(1), 0 },
    { 0x10C3, P_W, B(0), 0 },
    { 0x0005, P_W, B(1), B(1) },
    { 0x0005, P_POLL, B(1), 0 },
    { 0x0020, P_W, B(3), 0 },
    { 0x0000, P_W, B(5), B(5) },
    // card emulation -> card disable
    { 0x0007, P_W, 0xFF, 0x20 },
    { 0x0067, P_W, B(5), 0 },
    { 0x004A, P_W, B(0), 0 },
    { 0x0081, P_W, B(7) | B(6), 0 },
    { 0x0005, P_W, B(3) | B(4), B(3) },
    { 0x0090, P_W, B(1), 0 },
};

static int run_seq(const struct pcmd* s, int n) {
    for (int i = 0; i < n; i++) {
        const struct pcmd* c = &s[i];
        if (c->op == P_W) {
            uint8_t v = rtw_r8(c->reg);
            rtw_w8(c->reg, (uint8_t)((v & ~c->mask) | (c->val & c->mask)));
        } else if (c->op == P_POLL) {
            int ok = 0;
            for (int t = 0; t < 20000 && !ok; t++) {
                if ((rtw_r8(c->reg) & c->mask) == (c->val & c->mask)) ok = 1;
                else rtw_udelay(50);
            }
            if (!ok) { wlog("power sequence: reg %04x never became %02x/%02x", c->reg, c->val, c->mask); return -1; }
        } else rtw_mdelay(c->reg);
    }
    return 0;
}

static void pre_system_cfg(void) {
    rtw_w8(R_RSV_CTRL, 0);
    uint32_t v = rtw_r32(R_PAD_CTRL1); v |= B(29) | B(28); rtw_w32(R_PAD_CTRL1, v);                 // pin mux
    v = rtw_r32(R_LED_CFG); v &= ~(B(25) | B(26)); rtw_w32(R_LED_CFG, v);
    v = rtw_r32(R_GPIO_MUXCFG); v |= B(2); rtw_w32(R_GPIO_MUXCFG, v);
    uint8_t b = rtw_r8(R_SYS_FUNC_EN); b &= (uint8_t)~(B(0) | B(1)); rtw_w8(R_SYS_FUNC_EN, b);       // disable BB / RF
    b = rtw_r8(R_RF_CTRL); b &= (uint8_t)~(B(2) | B(1) | B(0)); rtw_w8(R_RF_CTRL, b);
    v = rtw_r32(R_WLRF1); v &= ~(B(24) | B(25) | B(26)); rtw_w32(R_WLRF1, v);
}

static int power_switch(int on) {
    uint8_t rpwm = rtw_r8(0xfe58);
    if (rtw_r16(R_MCUFW_CTRL) == 0xC078) { rpwm = (uint8_t)((rpwm ^ B(7)) & B(7)); rtw_w8(0xfe58, rpwm); }   // firmware still there: toggle RPWM
    int cur;
    if (rtw_r8(R_CR) == 0xea) cur = 0;
    else if (rtw_r8(R_SYS_STATUS1 + 1) & 1) cur = 0;
    else cur = 1;
    if (on == cur) return 1;                                                                       // already in the wanted state
    int r = on ? run_seq(pwr_on_seq, sizeof pwr_on_seq / sizeof pwr_on_seq[0]) : run_seq(pwr_off_seq, sizeof pwr_off_seq / sizeof pwr_off_seq[0]);
    if (on) rtw_w8_clr(R_SYS_STATUS1 + 1, B(0));
    return r;
}

static int mac_power_on(void) {
    pre_system_cfg();
    int r = power_switch(1);
    if (r == 1) {                                                                                  // was already on: switch off and on again
        power_switch(0);
        pre_system_cfg();
        r = power_switch(1);
    }
    if (r) return -1;
    uint32_t v = rtw_r32(R_CPU_DMEM_CON); v |= WL_PLATFORM_RST | DDMA_EN; rtw_w32(R_CPU_DMEM_CON, v);
    rtw_w8_set(R_SYS_FUNC_EN + 1, 0xD8);
    rtw_w8(R_CR_EXT + 3, (uint8_t)((rtw_r8(R_CR_EXT + 3) & 0xF0) | 0x0C));
    uint32_t t = rtw_r32(R_MCUFW_CTRL);
    if (t & B(20)) { rtw_w32(R_MCUFW_CTRL, t & ~B(20)); rtw_w32(R_GPIO_MUXCFG, rtw_r32(R_GPIO_MUXCFG) & ~B(19)); }   // no boot from flash
    return 0;
}

static void mac_power_off(void) { power_switch(0); }

// ---- sending packets to the chip (TX descriptor + payload on a bulk OUT endpoint) --------------------------------------
// Queue select values and the endpoint each queue's frames take (which depends on how many OUT endpoints there are).
#define QSEL_BEACON 16
#define QSEL_HIGH   17
#define QSEL_MGMT   18
#define QSEL_H2C    19

// DMA queue each traffic class maps to (0 extra, 1 low, 2 normal, 3 high), per number of bulk OUT endpoints (2, 3, 4)
static const uint8_t rqpn[5][6] = {            // vo vi be bk mg hi
    { 2, 2, 2, 3, 3, 3 }, { 2, 2, 2, 3, 3, 3 }, { 2, 2, 2, 3, 3, 3 }, { 2, 2, 1, 1, 3, 3 }, { 2, 2, 1, 1, 0, 3 },
};
static const struct { uint16_t hq, nq, lq, exq, gapq; } pgtbl[5] = { { 16, 16, 16, 14, 1 }, { 16, 16, 16, 14, 1 }, { 16, 16, 0, 0, 1 }, { 16, 16, 16, 0, 1 }, { 16, 16, 16, 14, 1 } };

static int dma_to_ep(int dma) { return dma == 3 ? 0 : dma == 2 ? 1 : dma == 1 ? 2 : 3; }

static int qsel_ep(int qsel) {
    const uint8_t* q = rqpn[R.nout];
    int map;
    switch (qsel) {
    case 0: case 3: map = q[2]; break;               // best effort TIDs
    case 1: case 2: map = q[3]; break;
    case 4: case 5: map = q[1]; break;
    case 6: case 7: map = q[0]; break;
    case QSEL_MGMT: map = q[4]; break;
    default: map = q[5]; break;                      // beacon, high, H2C
    }
    return dma_to_ep(map);
}

static void txdesc_checksum(uint8_t* d) {
    d[28] = d[29] = 0;
    uint16_t x = 0;
    for (int i = 0; i < 16; i++) x ^= (uint16_t)(d[2 * i] | (d[2 * i + 1] << 8));
    d[28] = (uint8_t)x; d[29] = (uint8_t)(x >> 8);
}

// payload (len <= 4200) with the given descriptor words; returns 0 on success
int rtw_tx_raw(int qsel, const uint32_t w[10], const uint8_t* payload, int len) {
    static uint8_t buf[48 + 4200 + 8];
    if (len < 0 || len > 4200) return -1;
    memset(buf, 0, 48);
    for (int i = 0; i < 10; i++) { buf[4 * i] = (uint8_t)w[i]; buf[4 * i + 1] = (uint8_t)(w[i] >> 8); buf[4 * i + 2] = (uint8_t)(w[i] >> 16); buf[4 * i + 3] = (uint8_t)(w[i] >> 24); }
    memcpy(buf + 48, payload, (uint32_t)len);
    txdesc_checksum(buf);
    int total = 48 + len;
    return xhci_wl_bulk_out(qsel_ep(qsel), buf, (uint32_t)total);
}

// ---- reserved page write (firmware download goes through the beacon queue) ----------------------------------------------
static int write_rsvd_page(uint16_t pg_addr, const uint8_t* data, uint32_t size) {
    static uint8_t pk[4096 + 8];
    if (!size || size > 4096 + 1) return -1;
    uint8_t bcn = rtw_r8(R_BCN_CTRL);
    rtw_w16(R_FIFOPAGE_CTRL_2, (uint16_t)((pg_addr & 0xfff) | B(15)));
    uint8_t cr1 = rtw_r8(R_CR + 1);
    rtw_w8(R_CR + 1, (uint8_t)(cr1 | (CR_ENSWBCN >> 8)));
    rtw_w8(R_BCN_CTRL, (uint8_t)((bcn & ~EN_BCN_FUNCTION) | DIS_TSF_UDT));
    uint32_t w[10] = { 0 };
    w[0] = size | (48u << 16) | B(26);               // packet size, payload offset, last segment
    w[1] = (uint32_t)QSEL_BEACON << 8;
    memcpy(pk, data, size);
    int r = rtw_tx_raw(QSEL_BEACON, w, pk, (int)size);
    int ok = r == 0 && hw_ready(R_FIFOPAGE_CTRL_2, B(15), 1, 1000);
    if (r == 0 && !ok) wlog("fw: reserved page was not marked valid");
    rtw_w16(R_FIFOPAGE_CTRL_2, (uint16_t)(R.rsvd_boundary | B(15)));
    rtw_w8(R_BCN_CTRL, bcn);
    rtw_w8(R_CR + 1, cr1);
    return ok ? 0 : -1;
}

// ---- firmware download ---------------------------------------------------------------------------------------------
static void wlan_cpu_enable(int on) {
    if (on) { rtw_w8_set(R_RSV_CTRL + 1, B(0)); rtw_w8_set(R_SYS_FUNC_EN + 1, B(2)); }
    else { rtw_w8_clr(R_SYS_FUNC_EN + 1, B(2)); rtw_w8_clr(R_RSV_CTRL + 1, B(0)); }
}

static int iddma(uint32_t src, uint32_t dst, uint32_t len, int first) {
    if (!hw_ready(R_DDMA_CH0CTRL, DDMA_OWN, 0, 1000)) return -1;
    uint32_t ctrl = DDMA_CHKSUM_EN | DDMA_OWN | (len & DDMA_DLEN_MASK);
    if (!first) ctrl |= DDMA_CHKSUM_CONT;
    rtw_w32(R_DDMA_CH0SA, src);
    rtw_w32(R_DDMA_CH0DA, dst);
    rtw_w32(R_DDMA_CH0CTRL, ctrl);
    return hw_ready(R_DDMA_CH0CTRL, DDMA_OWN, 0, 1000) ? 0 : -1;
}

static int fw_to_mem(const uint8_t* data, uint32_t dst, uint32_t size) {
    uint32_t off = 0, left = size;
    int first = 1;
    static uint8_t chunk[4096 + 8];
    rtw_w32(R_DDMA_CH0CTRL, rtw_r32(R_DDMA_CH0CTRL) | DDMA_RESET_CHKSUM_STS);
    while (left) {
        uint32_t n = left > 0x1000 ? 0x1000 : left;
        uint32_t send = n;
        if (((send + 48) & 511) == 0) send += 1;                         // a packet that fills the last USB packet exactly would need a zero-length one
        memset(chunk, 0, send);
        memcpy(chunk, data + off, n);
        if (write_rsvd_page(0, chunk, send)) { wlog("fw: writing the reserved page failed at offset %u", off); return -1; }
        if (iddma(OCPBASE_TXBUF + 0 + 48, dst + off, n, first)) { wlog("fw: DMA to chip memory failed at offset %u", off); return -1; }
        first = 0; off += n; left -= n;
    }
    uint8_t fc = rtw_r8(R_MCUFW_CTRL);
    int imem = dst < OCPBASE_DMEM;
    if (rtw_r32(R_DDMA_CH0CTRL) & DDMA_CHKSUM_STS) {
        fc |= imem ? B(3) : B(5); fc &= (uint8_t)~(imem ? B(4) : B(6));
        rtw_w8(R_MCUFW_CTRL, fc);
        wlog("fw: checksum error in %s", imem ? "IMEM" : "DMEM");
        return -1;
    }
    fc |= imem ? (B(3) | B(4)) : (B(5) | B(6));
    rtw_w8(R_MCUFW_CTRL, fc);
    return 0;
}

static uint32_t fw_addr(uint32_t a) { return a & ~B(31); }
static uint32_t le32p(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int download_firmware(void) {
    const uint8_t* fw = rtw_fw_start;
    uint32_t size = (uint32_t)(rtw_fw_end - rtw_fw_start);
    if (size < 64) return -1;
    uint32_t dmem = le32p(fw + 0x24), imem = le32p(fw + 0x30), emem = (fw[0x18] & B(4)) ? le32p(fw + 0x34) : 0;
    uint32_t dsz = dmem + 8, isz = imem + 8, esz = emem ? emem + 8 : 0;
    if (64 + dsz + isz + esz != size) { wlog("fw: size mismatch (%u != %u)", 64 + dsz + isz + esz, size); return -1; }
    wlog("fw: version %u.%u, dmem %u imem %u emem %u", fw[4] | (fw[5] << 8), fw[6], dmem, imem, emem);

    uint32_t ltecoex = 0;
    int have_lte = 0;
    if (hw_ready(0x1700, B(29), 1, 1000)) { rtw_w32(0x1700, 0x800F0000u | 0x38); ltecoex = rtw_r32(0x1708); have_lte = 1; }
    wlan_cpu_enable(0);

    // download only uses the high-priority queue: remember what we change
    uint8_t b_map1 = rtw_r8(R_TXDMA_PQ_MAP + 1), b_cr = rtw_r8(R_CR), b_bcn = rtw_r8(R_BCN_CTRL);
    uint16_t b_info1 = rtw_r16(R_FIFOPAGE_INFO_1);
    uint32_t b_rqpn = rtw_r32(R_RQPN_CTRL_2) | B(31);
    rtw_w8(R_TXDMA_PQ_MAP + 1, 3 << 6);
    rtw_w8(R_CR, CR_HCI_TXDMA_EN | CR_TXDMA_EN);
    rtw_w32(R_H2CQ_CSR, B(31));
    rtw_w16(R_FIFOPAGE_INFO_1, 0x200);
    rtw_w32(R_RQPN_CTRL_2, b_rqpn);
    rtw_w8(R_BCN_CTRL, (uint8_t)((b_bcn & ~EN_BCN_FUNCTION) | DIS_TSF_UDT));
    // reset the platform
    rtw_w8_clr(R_CPU_DMEM_CON + 2, B(0));
    rtw_w8_clr(R_SYS_CLK_CTRL + 1, B(6));
    rtw_w8_set(R_CPU_DMEM_CON + 2, B(0));
    rtw_w8_set(R_SYS_CLK_CTRL + 1, B(6));

    uint16_t v = (uint16_t)((rtw_r16(R_MCUFW_CTRL) & 0x3800) | MCUFW_EN);
    rtw_w16(R_MCUFW_CTRL, v);
    int r = fw_to_mem(fw + 64, fw_addr(le32p(fw + 0x20)), dsz);
    if (!r) r = fw_to_mem(fw + 64 + dsz, fw_addr(le32p(fw + 0x3c)), isz);
    if (!r && esz) r = fw_to_mem(fw + 64 + dsz + isz, fw_addr(le32p(fw + 0x38)), esz);
    if (r) {
        rtw_w8_clr(R_MCUFW_CTRL, MCUFW_EN);
        rtw_w8_set(R_SYS_FUNC_EN + 1, B(2));
        return -1;
    }
    rtw_w8(R_TXDMA_PQ_MAP + 1, b_map1); rtw_w8(R_CR, b_cr); rtw_w32(R_H2CQ_CSR, B(31)); rtw_w16(R_FIFOPAGE_INFO_1, b_info1);
    rtw_w32(R_RQPN_CTRL_2, b_rqpn); rtw_w8(R_BCN_CTRL, b_bcn);
    rtw_w32(R_TXDMA_STATUS, B(2));
    uint16_t fc = rtw_r16(R_MCUFW_CTRL);
    if ((fc & CHECK_SUM_OK) == CHECK_SUM_OK) rtw_w16(R_MCUFW_CTRL, (uint16_t)((fc | FW_DW_RDY) & ~MCUFW_EN));
    else wlog("fw: checksum flags not set (%04x)", fc);
    wlan_cpu_enable(1);
    if (have_lte) { if (hw_ready(0x1700, B(29), 1, 1000)) { rtw_w32(0x1704, ltecoex); rtw_w32(0x1700, 0xC00F0000u | 0x38); } }
    int ready = 0;
    for (int i = 0; i < 100 && !ready; i++) {
        if ((rtw_r16(R_MCUFW_CTRL) & FW_READY_MASK) == FW_READY) ready = 1; else rtw_mdelay(5);
    }
    if (!ready) { wlog("fw: not ready (MCUFW_CTRL %08x, DBG7 %08x)", rtw_r32(R_MCUFW_CTRL), rtw_r32(R_FW_DBG7)); return -1; }
    R.h2c_seq = 0; R.h2c_box = 0;
    wlog("fw: running");
    return 0;
}

// ---- H2C (host to firmware commands) ------------------------------------------------------------------------------------
// Packet form (32 bytes through the H2C queue) and the mailbox form (8 bytes through registers).
int rtw_h2c_pkt(uint8_t* pkt32) {
    pkt32[6] = (uint8_t)R.h2c_seq; pkt32[7] = (uint8_t)(R.h2c_seq >> 8);          // sequence number, bits 31:16 of word 1
    R.h2c_seq++;
    uint32_t w[10] = { 0 };
    w[0] = 32u;                                                                 // no payload offset, not a last segment (as the vendor driver sends them)
    w[1] = (uint32_t)QSEL_H2C << 8;
    return rtw_tx_raw(QSEL_H2C, w, pkt32, 32);
}

void rtw_h2c_mbox(const uint8_t* h2c8) {
    static const uint16_t box[4] = { 0x01D0, 0x01D4, 0x01D8, 0x01DC }, boxex[4] = { 0x01F0, 0x01F4, 0x01F8, 0x01FC };
    int b = R.h2c_box;
    int ok = 0;
    for (int i = 0; i < 30 && !ok; i++) { if (!((rtw_r8(R_HMETFR) >> b) & 1)) ok = 1; else rtw_udelay(100); }
    if (!ok) { wlog("h2c: mailbox %d busy", b); return; }
    rtw_w32(boxex[b], le32p(h2c8 + 4));
    rtw_w32(box[b], le32p(h2c8));
    R.h2c_box = (b + 1) & 3;
}

// ---- efuse ------------------------------------------------------------------------------------------------------------
static int efuse_read(uint8_t* log, int log_size) {
    uint8_t phy[512];
    rtw_w8(R_EFUSE_ACCESS, 0x69);
    rtw_w32_mask(R_LDO_EFUSE_CTRL, B(8) | B(9), 0);
    rtw_w8_clr(R_LDO_EFUSE_CTRL + 3, B(7));
    uint32_t ctl = rtw_r32(R_EFUSE_CTRL);
    for (uint32_t a = 0; a < 512; a++) {
        ctl &= ~(0xffu | (0x3ffu << 8));
        ctl |= (a & 0x3ff) << 8;
        rtw_w32(R_EFUSE_CTRL, ctl & ~B(31));
        int ok = 0;
        for (int i = 0; i < 1000 && !ok; i++) { ctl = rtw_r32(R_EFUSE_CTRL); if (ctl & B(31)) ok = 1; else rtw_udelay(10); }
        if (!ok) { rtw_w8(R_EFUSE_ACCESS, 0); return -1; }
        phy[a] = (uint8_t)ctl;
    }
    rtw_w8(R_EFUSE_ACCESS, 0);
    memset(log, 0xff, (uint32_t)log_size);
    uint32_t pi = 0, limit = 512 - 96;
    while (pi < limit) {
        uint8_t h1 = phy[pi], h2 = phy[pi + 1];
        if (h1 == 0xff || ((h1 & 0x1f) == 0xf && h2 == 0xff)) break;
        uint32_t blk, wen;
        if ((h1 & 0x1f) == 0xf) { blk = (uint32_t)(((h2 & 0xf0) >> 1) | ((h1 >> 5) & 7)); wen = h2 & 0xf; pi += 2; }
        else { blk = (uint32_t)((h1 & 0xf0) >> 4); wen = h1 & 0xf; pi += 1; }
        for (int i = 0; i < 4; i++) {
            if (wen & (1u << i)) continue;
            uint32_t li = (blk << 3) + ((uint32_t)i << 1);
            if (pi + 1 > limit || li + 1 >= (uint32_t)log_size) return -2;
            log[li] = phy[pi]; log[li + 1] = phy[pi + 1];
            pi += 2;
        }
    }
    return 0;
}

// ---- table loading ------------------------------------------------------------------------------------------------------
enum { T_MAC, T_BB, T_AGC, T_RF };
static void cfg_entry(int kind, uint32_t addr, uint32_t data) {
    switch (kind) {
    case T_MAC: rtw_w8(addr, (uint8_t)data); break;
    case T_AGC: rtw_w32(addr, data); break;
    case T_BB:
        if (addr == 0xfe) rtw_mdelay(50); else if (addr == 0xfd) rtw_mdelay(5); else if (addr == 0xfc) rtw_mdelay(1);
        else if (addr == 0xfb) rtw_udelay(50); else if (addr == 0xfa) rtw_udelay(5); else if (addr == 0xf9) rtw_udelay(1);
        else rtw_w32(addr, data);
        break;
    case T_RF:
        if (addr == 0xffe) rtw_mdelay(50); else if (addr == 0xfe) rtw_udelay(100);
        else { rtw_rf_write(addr, 0xfffff, data); rtw_udelay(1); }
        break;
    }
}

// does a condition tile (IF / ELIF header word) match this chip?
static int cond_match(uint32_t c) {
    uint32_t rfe = c & 0xff, intf = (c >> 8) & 0xf, pkg = (c >> 12) & 0xf, cut = (c >> 24) & 0xf;
    if (cut && cut != (R.cut ? R.cut : 15u)) return 0;
    if (pkg && pkg != (R.pkg ? R.pkg : 15u)) return 0;
    if (intf && intf != 2) return 0;                          // USB
    return rfe == R.rfe;
}

static void load_table(const uint32_t* t, uint32_t nwords, int kind) {
    int matched = 1, skipped = 0;
    uint32_t pos = 0;
    for (uint32_t i = 0; i + 1 < nwords; i += 2) {
        uint32_t a = t[i], b = t[i + 1];
        if (a & B(31)) {
            switch ((a >> 28) & 3) {
            case 3: matched = 1; skipped = 0; break;                       // ENDIF
            case 2: matched = skipped ? 0 : 1; break;                      // ELSE
            default: pos = a; break;                                       // IF / ELIF
            }
        } else if (a & B(30)) {                                            // the "then" marker of the header seen before
            if (!skipped) { if (cond_match(pos)) { matched = 1; skipped = 1; } else { matched = 0; skipped = 0; } }
            else matched = 0;
        } else if (matched) cfg_entry(kind, a, b);
    }
}

// ---- MAC / PHY initialisation ----------------------------------------------------------------------------------------------
static int priority_queue_cfg(void) {
    int n = R.nout;
    if (n < 2 || n > 4) return -1;
    uint32_t txff_pg = 65536 / 128, rsvd = 8 + 24 + 8 + 8 + 0 + 4 + 0;
    uint32_t acq = txff_pg - rsvd;
    R.rsvd_boundary = (uint16_t)acq;
    uint32_t cur = txff_pg;                                              // reserved pages from the top: fw txbuf, h2cq, static info, h2c info, driver
    R.rsvd_fw_txbuf = (uint16_t)(cur -= 4);
    R.rsvd_h2cq = (uint16_t)(cur -= 8);
    cur -= 8; cur -= 24; cur -= 8;
    if (cur != acq) return -2;
    uint32_t pub = acq - pgtbl[n].hq - pgtbl[n].lq - pgtbl[n].nq - pgtbl[n].exq - pgtbl[n].gapq;
    rtw_w16(R_FIFOPAGE_INFO_1, pgtbl[n].hq);
    rtw_w16(R_FIFOPAGE_INFO_2, pgtbl[n].lq);
    rtw_w16(R_FIFOPAGE_INFO_3, pgtbl[n].nq);
    rtw_w16(R_FIFOPAGE_INFO_4, pgtbl[n].exq);
    rtw_w16(R_FIFOPAGE_INFO_5, (uint16_t)pub);
    rtw_w32_set(R_RQPN_CTRL_2, B(31));
    rtw_w16(R_FIFOPAGE_CTRL_2, R.rsvd_boundary);
    rtw_w8_set(R_FWHW_TXQ_CTRL + 2, B(4));
    rtw_w16(R_BCNQ_BDNY_V1, R.rsvd_boundary);
    rtw_w16(R_FIFOPAGE_CTRL_2 + 2, R.rsvd_boundary);
    rtw_w16(R_BCNQ1_BDNY_V1, R.rsvd_boundary);
    rtw_w32(R_RXFF_BNDY, 16384 - 256 - 1);
    uint8_t v = rtw_r8(R_AUTO_LLT_V1); v = (uint8_t)((v & 0x0f) | (3 << 4)); rtw_w8(R_AUTO_LLT_V1, v);     // USB TX aggregation descriptors
    rtw_w8(R_AUTO_LLT_V1 + 3, 3);
    rtw_w8_set(R_TXDMA_OFFSET_CHK + 1, B(1));
    rtw_w8_set(R_AUTO_LLT_V1, B(0));
    if (!hw_ready(R_AUTO_LLT_V1, B(0), 0, 1000)) return -3;
    rtw_w8(R_CR + 3, 0);
    return 0;
}

static int init_trx_cfg(void) {
    const uint8_t* q = rqpn[R.nout];
    uint16_t map = (uint16_t)((q[5] << 14) | (q[4] << 12) | (q[3] << 10) | (q[2] << 8) | (q[1] << 6) | (q[0] << 4));
    rtw_w16(R_TXDMA_PQ_MAP, map);
    rtw_w8(R_CR, 0);
    rtw_w8(R_CR, (uint8_t)MAC_TRX_ENABLE);
    rtw_w32(R_H2CQ_CSR, B(31));
    rtw_w8_set(R_TXDMA_PQ_MAP, B(0));
    int r = priority_queue_cfg();
    if (r) { wlog("mac: queue setup failed (%d)", r); return r; }
    // H2C queue geometry
    uint32_t addr = (uint32_t)R.rsvd_h2cq << 7, size = 8u << 7;
    uint32_t v = rtw_r32(0x0244); v = (v & 0xFFFC0000u) | addr; rtw_w32(0x0244, v);
    v = rtw_r32(0x024C); v = (v & 0xFFFC0000u) | addr; rtw_w32(0x024C, v);
    v = rtw_r32(0x0248); v = (v & 0xFFFC0000u) | (addr + size); rtw_w32(0x0248, v);
    uint8_t b = rtw_r8(0x0254); b = (uint8_t)((b & 0xFC) | 1); rtw_w8(0x0254, b);
    b = rtw_r8(0x0254); b = (uint8_t)((b & 0xFB) | 4); rtw_w8(0x0254, b);
    b = rtw_r8(R_TXDMA_OFFSET_CHK + 1); b = (uint8_t)((b & 0x7f) | 0x80); rtw_w8(R_TXDMA_OFFSET_CHK + 1, b);
    uint32_t wp = rtw_r32(R_H2C_PKT_WRITEADDR) & 0x3FFFF, rp = rtw_r32(R_H2C_PKT_READADDR) & 0x3FFFF;
    uint32_t free_sz = wp >= rp ? size - (wp - rp) : rp - wp;
    if (free_sz != size) { wlog("mac: H2C queue mismatch (wp %x rp %x)", wp, rp); return -1; }
    return 0;
}

static void chip_mac_init(void) {
    rtw_w8(R_AMPDU_MAX_TIME_V1, 0x70);
    rtw_w8_set(R_TX_HANG_CTRL, B(2));
    uint16_t pre = 0x1E4 | B(11);
    rtw_w8(R_PRECNT_CTRL, (uint8_t)pre); rtw_w8(R_PRECNT_CTRL + 1, (uint8_t)(pre >> 8));
    rtw_w32(R_PROT_MODE_CTRL, 0xFFu | (0x08u << 8) | (0x20u << 16) | (0x20u << 24));
    rtw_w16(R_BAR_MODE_CTRL + 2, (uint16_t)(1 | (8 << 8)));
    rtw_w8(R_FAST_EDCA_VOVI, 6); rtw_w8(R_FAST_EDCA_VOVI + 2, 6); rtw_w8(R_FAST_EDCA_BEBK, 6); rtw_w8(R_FAST_EDCA_BEBK + 2, 6);
    rtw_w8_set(R_INIRTS_RATE_SEL, B(5));
    rtw_w8_clr(R_TIMER0_SRC_SEL, B(4) | B(5) | B(6));
    rtw_w16(R_TXPAUSE, 0);
    rtw_w8(R_SLOT, 0x09);
    rtw_w8(R_PIFS, 0x19);
    rtw_w32(R_SIFS, 0xA | (0xE << 8) | (0x10 << 16) | (0x10u << 24));
    rtw_w16(R_EDCA_VO_PARAM + 2, 0x186);
    rtw_w16(R_EDCA_VI_PARAM + 2, 0x3BC);
    rtw_w32(R_RD_NAV_NXT, 0x05 | (0x1B << 16));
    rtw_w16(R_RXTSF_OFFSET_CCK, 0x30 | (0x30 << 8));
    rtw_w8_set(R_BCN_CTRL, EN_BCN_FUNCTION);
    rtw_w32(R_TBTT_PROHIBIT, 0x04 | (0x064 << 8));
    rtw_w8(R_DRVERLYINT, 0x04);
    rtw_w8(R_BCNDMATIM, 0x02);
    rtw_w8_clr(R_TX_PTCL_CTRL + 1, B(12) >> 8);
    rtw_w16(R_RXFLTMAP0, 0xFFFF);
    rtw_w16(R_RXFLTMAP1, 0x0FFF);
    rtw_w16(R_RXFLTMAP2, 0xFFFF);
    R.rcr = 0xE400220E;
    rtw_w32(R_RCR, R.rcr);
    rtw_w8(R_RX_PKT_LIMIT, 12288 >> 9);
    rtw_w8(R_TCR + 2, 0x30);
    rtw_w8(R_TCR + 1, 0x30);
    rtw_w8(R_ACKTO_CCK, 0x40);
    rtw_w8_set(R_WMAC_TRXPTCL_CTL_H, B(1));
    rtw_w8_set(R_SND_PTCL_CTRL, B(3));
    rtw_w32(R_WMAC_OPTION_FUNCTION + 8, 0xb0810041u);
    rtw_w8(R_WMAC_OPTION_FUNCTION + 4, 0x98);
}

static void drv_info_cfg(void) {
    rtw_w8(R_RX_DRVINFO_SZ, 4);
    rtw_w8(0x0115, (uint8_t)((rtw_r8(0x0115) & 0xF0) | 0xF));
    rtw_w32_set(R_RCR, RCR_APP_PHYSTS);
    R.rcr |= RCR_APP_PHYSTS;
    rtw_w32_clr(R_WMAC_OPTION_FUNCTION + 4, B(8) | B(9));
}

static void usb_interface_cfg(void) {
    int burst = R.speed == 4 ? 0 : R.speed == 3 ? 1 : 2;               // 1024 / 512 / 64 byte bursts
    rtw_w8(R_RXDMA_MODE, (uint8_t)(0xE | (burst << 4)));
    rtw_w16_set(R_TXDMA_OFFSET_CHK, B(9));
    // receive aggregation off (size 0, timeout 1)
    rtw_w8_set(R_TXDMA_PQ_MAP, B(2));
    rtw_w8_clr(R_RXDMA_AGG_PG_TH + 3, B(7));
    rtw_w16(R_RXDMA_AGG_PG_TH, (uint16_t)(0 | (1 << 8)));
}

static void phy_set_param(void) {
    uint8_t val = rtw_r8(R_SYS_FUNC_EN);
    val |= B(6);
    rtw_w8(R_SYS_FUNC_EN, val);
    val |= B(0) | B(1); rtw_w8(R_SYS_FUNC_EN, val);
    val &= (uint8_t)~(B(0) | B(1)); rtw_w8(R_SYS_FUNC_EN, val);
    val |= B(0) | B(1); rtw_w8(R_SYS_FUNC_EN, val);
    rtw_w8(R_RF_CTRL, B(0) | B(1) | B(2));
    rtw_udelay(10);
    rtw_w8(R_WLRF1 + 3, B(0) | B(1) | B(2));
    rtw_udelay(10);
    rtw_w32_clr(R_RXPSEL, B(28) | B(29));
    load_table(rtw_mac_tbl, rtw_mac_tbl_n, T_MAC);
    load_table(rtw_bb_tbl, rtw_bb_tbl_n, T_BB);
    load_table(rtw_agc_tbl, rtw_agc_tbl_n, T_AGC);
    if (R.rfe == 2 || R.rfe == 4) load_table(rtw_agc_btg2_tbl, rtw_agc_btg2_tbl_n, T_AGC);
    load_table(rtw_rf_a_tbl, rtw_rf_a_tbl_n, T_RF);
    uint32_t cap = R.xtal & 0x3F;
    rtw_w32_mask(R_AFE_XTAL_CTRL, 0x7e000000, cap);
    rtw_w32_mask(R_AFE_PLL_CTRL, 0x7e, cap);
    rtw_w32_mask(0xa2c, B(18) | B(22), 0);
    rtw_w32_set(R_RXPSEL, B(28) | B(29));
    R.ch_param[0] = rtw_r32(0xa24); R.ch_param[1] = rtw_r32(0xa28); R.ch_param[2] = rtw_r32(0xaac);
    rtw_w32(0x1C94, 0xAFFFAFFF);
}

// ---- channel switching (20 MHz) ---------------------------------------------------------------------------------------------
static void switch_rf_set(int set) {                                  // 0 BTG, 1 WLG (2.4 GHz), 2 WLA (5 GHz)
    rtw_w32_set(0x1080, B(16));
    rtw_w32_set(0x0000, B(26));
    uint32_t reg = rtw_r32(0x0CB8);
    if (set == 0) { reg |= B(16); reg &= ~(B(18) | B(20) | B(22) | B(21) | B(23)); rtw_w32_mask(0xa84, 0xff0000, 0x0e); rtw_w32_mask(0xa80, 0xffff, 0xfc84); }
    else if (set == 1) { reg |= B(20) | B(22) | B(21); reg &= ~(B(16) | B(18) | B(23)); rtw_w32_mask(0xa84, 0xff0000, 0x12); rtw_w32_mask(0xa80, 0xffff, 0x7532); }
    else { reg |= B(20) | B(22) | B(23); reg &= ~(B(16) | B(18) | B(21)); }
    rtw_w32(0x0CB8, reg);
}

static int rfe_btg(void) { switch (R.rfe) { case 2: case 4: case 7: case 0xa: case 0xc: case 0xf: return 1; } return 0; }

static void set_channel_bb(int ch) {
    if (ch <= 14) {
        rtw_w32_mask(0x808, B(28), 1);
        rtw_w32_mask(0x454, B(7), 0);
        rtw_w32_mask(0xa80, B(18), 0);
        rtw_w32_mask(0x814, 0x0000FC00, 15);
        rtw_w32_mask(0xc1c, 0xf00, 0);
        rtw_w32_mask(0x860, 0x1ffe0000, 0x96a);
        if (ch == 14) {
            rtw_w32(0xa24, 0x0000b81c); rtw_w32_mask(0xa28, 0xffff, 0); rtw_w32(0xaac, 0x00003667);
        } else {
            rtw_w32(0xa24, R.ch_param[0]); rtw_w32_mask(0xa28, 0xffff, R.ch_param[1] & 0xffff); rtw_w32(0xaac, R.ch_param[2]);
        }
    } else if (ch > 35) {
        rtw_w32_mask(0xa80, B(18), 1);
        rtw_w32_mask(0x454, B(7), 1);
        rtw_w32_mask(0x808, B(28), 0);
        rtw_w32_mask(0x814, 0x0000FC00, 15);
        if (ch >= 36 && ch <= 64) rtw_w32_mask(0xc1c, 0xf00, 1);
        else if (ch >= 100 && ch <= 144) rtw_w32_mask(0xc1c, 0xf00, 2);
        else if (ch >= 149) rtw_w32_mask(0xc1c, 0xf00, 3);
        if (ch >= 36 && ch <= 48) rtw_w32_mask(0x860, 0x1ffe0000, 0x494);
        else if (ch >= 52 && ch <= 64) rtw_w32_mask(0x860, 0x1ffe0000, 0x453);
        else if (ch >= 100 && ch <= 116) rtw_w32_mask(0x860, 0x1ffe0000, 0x452);
        else if (ch >= 118 && ch <= 177) rtw_w32_mask(0x860, 0x1ffe0000, 0x412);
    }
    // 20 MHz
    uint32_t v = rtw_r32(0x8ac); v &= 0xffcffc00; v |= 0x10010000; rtw_w32(0x8ac, v);
    rtw_w32_mask(0x8c4, B(30), 1);
}

static uint32_t bb_swing(int ch) {
    static const uint32_t s2s[4] = { 0x200, 0x16a, 0x101, 0x0b6 };
    uint8_t sw = ch <= 14 ? R.bb_swing_2g : R.bb_swing_5g;
    if (sw > 9) sw = 0;
    return s2s[sw / 3];
}

static void set_channel_rf(int ch) {
    uint32_t rf18 = rtw_rf_read(0x18, 0xfffff);
    rf18 &= ~(B(16) | B(9) | B(8) | 0xff | B(18) | B(17) | B(11) | B(10));
    rf18 |= ch <= 14 ? 0 : (B(16) | B(8));
    rf18 |= (uint32_t)ch & 0xff;
    if (ch >= 100 && ch <= 140) rf18 |= B(17); else if (ch > 140) rf18 |= B(18);
    rf18 |= B(11) | B(10);                                               // 20 MHz
    if (ch <= 14) {
        switch_rf_set(rfe_btg() ? 0 : 1);
        rtw_rf_write(0xdf, B(6), 1);
        rtw_rf_write(0x64, 0xf, 0xf);
    } else {
        switch_rf_set(2);
        rtw_rf_write(0xdf, B(6), 0);
    }
    rtw_rf_write(0x18, 0xfffff, rf18);
    rtw_rf_write(0xb8, B(19), 0);
    rtw_rf_write(0xb8, B(19), 1);
}

static void set_channel_mac(int ch) {
    rtw_w8(R_DATA_SC, 0);
    uint32_t v = rtw_r32(R_WMAC_TRXPTCL_CTL); v &= ~(B(7) | B(8)); rtw_w32(R_WMAC_TRXPTCL_CTL, v);      // 20 MHz
    v = rtw_r32(R_AFE_CTRL1) & ~(B(20) | B(21)); rtw_w32(R_AFE_CTRL1, v);
    rtw_w8(R_USTIME_TSF, 80);
    rtw_w8(R_USTIME_EDCA, 80);
    uint8_t b = rtw_r8(R_CCK_CHECK) & (uint8_t)~B(7);
    if (ch > 14) b |= B(7);
    rtw_w8(R_CCK_CHECK, b);
}

// transmit power: the efuse's base index for the channel group on every rate (the vendor driver also adds per-rate
// offsets from its power-by-rate table; this is plain and safe for management frames and the low rates)
static int group_5g(int ch) {
    static const struct { uint8_t lo, hi, g; } t[] = { { 36, 42, 0 }, { 44, 50, 1 }, { 52, 58, 2 }, { 60, 64, 3 }, { 100, 106, 4 }, { 108, 114, 5 }, { 116, 122, 6 },
        { 124, 130, 7 }, { 132, 138, 8 }, { 140, 144, 9 }, { 149, 155, 10 }, { 157, 161, 11 }, { 165, 171, 12 }, { 173, 177, 13 } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (ch >= t[i].lo && ch <= t[i].hi) return t[i].g;
    return 0;
}

// CCK rates have their own base index; OFDM / HT use the 40 MHz base plus the OFDM offset.
static void set_tx_power(int ch) {
    int cck, ofdm;
    if (ch <= 14) {
        int g = ch <= 2 ? 0 : ch <= 5 ? 1 : ch <= 8 ? 2 : ch <= 11 ? 3 : 4;
        cck = R.pwr_cck[ch == 14 ? 5 : g];
        ofdm = R.pwr_2g40[g] + R.ofdm_diff_2g;
    } else {
        cck = 0;
        ofdm = R.pwr_5g[group_5g(ch)] + R.ofdm_diff_5g;
    }
    if (cck <= 0 || cck > 0x3f) cck = 0x20;
    if (ofdm <= 0 || ofdm > 0x3f) ofdm = 0x20;
    rtw_w32(0x1d00, (uint32_t)cck * 0x01010101u);
    uint32_t w = (uint32_t)ofdm * 0x01010101u;
    for (uint32_t a = 0x1d04; a <= 0x1d3c; a += 4) rtw_w32(a, w);
    R.tx_idx = ofdm;
}

int rtw_set_channel(int ch) {
    if (ch < 1 || ch > 177) return -1;
    set_channel_bb(ch);
    rtw_w32_mask(0xc1c, 0xffe00000, bb_swing(ch));
    set_channel_mac(ch);
    set_channel_rf(ch);
    // RX DFIR for 20 MHz
    rtw_w32_mask(0x948, B(29) | B(28), 2);
    rtw_w32_mask(0x94c, B(29) | B(28), 2);
    rtw_w32_mask(0xc20, B(31), 1);
    rtw_w32_mask(0x8f0, B(31), 0);
    set_tx_power(ch);
    R.channel = (uint8_t)ch;
    return 0;
}

// ---- receive path ----------------------------------------------------------------------------------------------------------------
static const int8_t lna_tbl0[8] = { 22, 8, -6, -22, -31, -40, -46, -52 };
static const int8_t lna_tbl1[16] = { 10, 6, 2, -2, -6, -10, -14, -17, -20, -24, -28, -31, -34, -37, -40, -44 };

static int phy_rssi(const uint8_t* ps) {
    int page = ps[0] & 0xf;
    if (page == 1) return (int)ps[1] - 110;                           // OFDM / HT / VHT: power in dB above -110 dBm
    if (page == 0) {                                                   // CCK: from the front end gains
        int vga = (ps[13] & 0x1f);
        int lna = ((ps[13] >> 5) & 7) | (((ps[14] >> 7) & 1) << 3);
        int n = R.rfe == 0 ? 8 : 16;
        if (lna >= n) return -120;
        return (R.rfe == 0 ? lna_tbl0[lna] : lna_tbl1[lna]) - 2 * vga;
    }
    return -120;
}

void rtw_rx_poll(void) {
    const uint8_t* buf;
    int got = 0;
    for (int n; (n = xhci_wl_rx_get(&buf)) > 0 && got < 8; got++) {
        R.rx_bufs++;
        int off = 0;
        while (off + 24 <= n) {
            const uint8_t* d = buf + off;
            uint32_t w0 = le32p(d), w2 = le32p(d + 8), w3 = le32p(d + 12);
            int plen = (int)(w0 & 0x3fff), crc = (int)((w0 >> 14) & 1), icv = (int)((w0 >> 15) & 1);
            int dinfo = (int)((w0 >> 16) & 0xf) * 8, shift = (int)((w0 >> 24) & 3), physt = (int)((w0 >> 26) & 1);
            int is_c2h = (int)((w2 >> 28) & 1);
            int rate = (int)(w3 & 0x7f);
            int total = 24 + dinfo + shift + plen;
            if (plen == 0 || total > n - off || (dinfo && dinfo != 32)) { R.rx_bad++; break; }
            const uint8_t* payload = d + 24 + dinfo + shift;
            if (is_c2h) {
                rtw_c2h(payload, plen);
            } else if (!crc && plen > 4) {
                int rssi = physt && dinfo ? phy_rssi(d + 24 + shift) : -120;
                R.rx_frames++;
                rtw_rx_frame(payload, plen - 4, rate, rssi, icv);          // the 4-byte FCS is appended (RCR.APP_FCS)
            } else R.rx_crc++;
            off += (total + 7) & ~7;
        }
        xhci_wl_rx_done();
    }
}

// ---- transmitting 802.11 frames ---------------------------------------------------------------------------------------------------
// Management frames go out on the management queue at 6 Mbit/s with the hardware numbering them; data frames at a fixed OFDM
// rate (the firmware's rate adaptation is not driven), numbered by the caller.
#define RATE_6M  0x04
#define RATE_24M 0x08

int rtw_tx_frame(const uint8_t* f, int len, int is_data) {
    if (R.state != 2 || len < 10 || len > 2300) return -1;
    int bmc = f[4] & 1;
    int mgmt = !is_data;
    int qsel = mgmt ? QSEL_MGMT : (bmc ? QSEL_HIGH : 0);
    uint32_t w[10] = { 0 };
    w[0] = (uint32_t)len | (48u << 16) | ((uint32_t)bmc << 24) | B(26) | (mgmt ? B(31) : 0);
    w[1] = (uint32_t)0 | ((uint32_t)qsel << 8) | ((uint32_t)7 << 16) | (qsel == QSEL_HIGH ? B(29) : 0);   // MACID 0, queue, rate id (G), more data for the high queue
    w[3] = B(8) | B(10);                                                      // use the rate in this descriptor, no fallback
    w[4] = is_data ? RATE_24M : RATE_6M;
    if (mgmt) w[8] = B(15);                                                   // sequence numbers from the hardware
    else w[9] = (uint32_t)((((uint32_t)f[22] | ((uint32_t)f[23] << 8)) >> 4) & 0xfff) << 12;
    return rtw_tx_raw(qsel, w, f, len);
}

// ---- station link state ---------------------------------------------------------------------------------------------------------------
void rtw_set_link(int up, const uint8_t* bssid, int aid) {
    uint8_t h[8];
    for (int i = 0; i < 6; i++) rtw_w8(R_BSSID + (uint32_t)i, up ? bssid[i] : 0);
    rtw_w32_mask(R_CR, 0x30000, up ? 2 : 0);                                  // network type: infrastructure station / no link
    rtw_w32_mask(0x06a8, 0x7ff, up ? (uint32_t)aid : 0);
    memset(h, 0, 8);
    h[0] = 0x01; h[1] = (uint8_t)(up ? 1 : 0); h[2] = 0;                      // media status report for MACID 0
    rtw_h2c_mbox(h);
    if (up) {
        memset(h, 0, 8);
        h[0] = 0x2c; h[1] = 0; h[2] = 0;                                      // default port 0 <- MACID 0
        rtw_h2c_mbox(h);
        memset(h, 0, 8);                                                      // rate adaptation info: legacy rates only
        uint32_t mask = R.channel <= 14 ? 0xFFF : 0xFF0;
        h[0] = 0x40; h[1] = 0; h[2] = (uint8_t)(R.channel <= 14 ? 6 : 7); h[3] = (uint8_t)(1 << 6);       // rate id BG / G, disable power tracking
        h[4] = (uint8_t)mask; h[5] = (uint8_t)(mask >> 8);
        rtw_h2c_mbox(h);
    }
    wlog("wifi: link %s (bssid %02x:%02x:%02x:%02x:%02x:%02x, aid %d)", up ? "up" : "down", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], aid);
}

// RF calibration done by the firmware (IQK), needed before transmitting on a new channel.
void rtw_iqk(void) {
    uint8_t p[32];
    memset(p, 0, sizeof p);
    p[0] = 1; p[1] = 0xFF; p[2] = 0x0E; p[4] = 8 + 1;
    if (rtw_h2c_pkt(p)) return;
    int i;
    for (i = 0; i < 300; i++) {
        if (rtw_rf_read(0x08, 0xfffff) == 0xabcde) break;
        rtw_mdelay(20);
    }
    rtw_rf_write(0x08, 0xfffff, 0);
    wlog("wifi: calibration %s after %d ms (result %08x)", i < 300 ? "done" : "TIMED OUT", i * 20, rtw_r32(0x1bf0));
}

// ---- Bluetooth coexistence in "Wi-Fi only" mode ------------------------------------------------------------------------------------
// The chip shares its antenna switch and radio front end with Bluetooth. After power-up the switch points at BT; without
// these steps the radio hears nothing. We never run Bluetooth, so: GNT_BT low, GNT_WL high, the path owned by Wi-Fi, the
// antenna switch on the Wi-Fi side.
static void lte_wait(void) { hw_ready(0x1700, B(29), 1, 1000); }
static void lte_mask(uint16_t off, uint32_t mask, uint32_t val) {
    lte_wait();
    rtw_w32(0x1700, 0x800F0000u | off);
    uint32_t t = rtw_r32(0x1708);
    t = (t & ~mask) | ((val << __builtin_ctz(mask)) & mask);
    lte_wait();
    rtw_w32(0x1704, t);
    rtw_w32(0x1700, 0xC00F0000u | off);
}

enum { SW_BT, SW_WLG, SW_WLA, SW_WLG_BT };

static void ant_switch(int pos, int by_bt) {
    int wlg_at_btg = 0, pol = 0, exist = 1;
    switch (R.rfe) {
    case 2: case 10: case 7: case 15: wlg_at_btg = 1; break;
    case 3: case 11: pol = 1; break;
    case 4: case 12: wlg_at_btg = 1; pol = 1; break;
    case 5: case 13: case 6: case 14: exist = 0; break;
    }
    if (!exist) return;
    if (wlg_at_btg && !by_bt) pos = pol ? SW_WLA : SW_WLG_BT;
    if (by_bt) {
        rtw_w32_clr(R_LED_CFG, B(23));
        rtw_w32_clr(R_LED_CFG, B(24));
    } else {
        rtw_w32_clr(R_LED_CFG, B(23));
        rtw_w32_set(R_LED_CFG, B(24));
        rtw_w32_mask(0xcb4, 0xff, 0x77);
        uint32_t reg;
        if (pos == SW_WLG_BT) reg = (R.rfe != 4 && R.rfe != 2) ? 0x3 : (!pol ? 0x2 : 0x1);
        else if (pos == SW_WLG) reg = !pol ? 0x2 : 0x1;
        else reg = !pol ? 0x1 : 0x2;
        rtw_w32_mask(0xcb4, 0xf0000000u, reg);
    }
    if (by_bt) rtw_w8_clr(0x67, B(5) | B(4)); else rtw_w8_set(0x67, B(5) | B(4));
}

static void coex_wifi_only(void) {
    // power-on setting
    rtw_w8_set(R_SYS_FUNC_EN, B(0) | B(1));
    int bt_disabled = !(rtw_r16(0xAA) & 2);
    if (bt_disabled) rtw_w8_set(0x73, B(2)); else rtw_w8_clr(0x73, B(2));              // path owner (at power-on: Wi-Fi when BT is absent)
    ant_switch(SW_BT, 0);
    rtw_w8(0xff1a, 0);
    rtw_w32_clr(R_PAD_CTRL1, B(20) | B(24) | B(15)); rtw_w32_clr(R_GPIO_MUXCFG, B(19));
    rtw_w32_clr(0x70, B(18) | B(27));
    // init hardware config
    rtw_w8_set(R_BCN_CTRL, EN_BCN_FUNCTION);
    rtw_w32_mask(0x790, 0x3f, 5);
    rtw_w8(0x778, 1);
    rtw_w32_set(R_GPIO_MUXCFG, B(5)); rtw_w32_set(R_GPIO_MUXCFG, B(9));
    rtw_w8_set(R_QUEUE_CTRL, B(4)); rtw_w8_clr(R_QUEUE_CTRL, B(5));
    rtw_w16_set(0x762, B(12));
    rtw_w32_mask(0x6cc, 0x08000000, 1);
    // Wi-Fi only antenna path
    lte_mask(0x38, 0xc000, 1); lte_mask(0x38, 0x0c00, 1);                                // GNT_BT = software low
    lte_mask(0x38, 0x3000, 3); lte_mask(0x38, 0x0300, 3);                                // GNT_WL = software high
    rtw_w8_set(0x73, B(2));                                                              // Wi-Fi owns the path
    ant_switch(SW_WLG, 0);
    uint16_t sb = (uint16_t)(rtw_r16(0xAA) & ~B(15));
    sb |= 2; sb |= 0x1 | 0x2;                                                            // score board: active + on
    rtw_w16(0xAA, (uint16_t)(sb | B(15)));
}

// ---- bring-up ----------------------------------------------------------------------------------------------------------------------
static int parse_efuse(const uint8_t* m) {
    R.rfe = m[0xca] & 0x1f;
    R.pkg = (m[0xca] & B(5)) ? 1 : 0;
    R.xtal = m[0xb9];
    R.bb_swing_2g = m[0xc6]; R.bb_swing_5g = m[0xc7];
    if (R.xtal == 0xff) R.xtal = 0;
    if (R.bb_swing_2g == 0xff) R.bb_swing_2g = 0;
    if (R.bb_swing_5g == 0xff) R.bb_swing_5g = 0;
    for (int g = 0; g < 6; g++) R.pwr_cck[g] = m[0x10 + g];              // path A: 2.4 GHz block at 0x10 (cck_base[6], bw40_base[5], diffs), 5 GHz block at 0x22
    for (int g = 0; g < 5; g++) R.pwr_2g40[g] = m[0x16 + g];
    for (int g = 0; g < 14; g++) R.pwr_5g[g] = m[0x22 + g];
    R.ofdm_diff_2g = (int8_t)(m[0x1b] << 4) >> 4;                         // signed low nibble
    R.ofdm_diff_5g = (int8_t)(m[0x30] << 4) >> 4;
    memcpy(R.mac, m + 0x107, 6);                                           // USB adapters keep the MAC at 0x107
    int bad = 1;
    for (int i = 0; i < 6; i++) if (R.mac[i] != 0xff && R.mac[i] != 0) bad = 0;
    if (bad || (R.mac[0] & 1)) return -1;
    return 0;
}

int rtw_start(void) {
    struct xhci_wl_info wi;
    memset(&R, 0, sizeof R);
    if (!xhci_wl_present(&wi)) { wlog("wifi: no supported USB adapter attached"); return -1; }
    R.nout = wi.nout; R.speed = wi.speed; R.vid = wi.vid; R.pid = wi.pid;
    wlog("wifi: adapter %x:%x, %d bulk OUT endpoints, USB speed %d", wi.vid, wi.pid, wi.nout, wi.speed);
    R.chip_ver = rtw_r32(R_SYS_CFG1);
    R.cut = (uint8_t)((R.chip_ver >> 12) & 0xf);
    wlog("wifi: SYS_CFG1 %08x (cut %u, %s), io errors so far %u", R.chip_ver, R.cut, (R.chip_ver & B(27)) ? "2T2R" : "1T1R", R.io_err);
    if (R.io_err) return -1;

    // first power-up: only to read the efuse (the firmware reports its hardware features at the same time)
    if (mac_power_on()) { wlog("wifi: power-on sequence failed"); return -2; }
    rtw_w8(R_C2HEVT, 0xfd);
    R.rsvd_boundary = 460;                                                // for the reserved-page writes before the queues are set up
    if (download_firmware()) { mac_power_off(); return -3; }
    uint8_t* map = (uint8_t*)kmalloc(512);
    if (!map) return -4;
    int er = efuse_read(map, 512);
    wlog("wifi: efuse read %s; feature report id %02x", er ? "FAILED" : "ok", rtw_r8(R_C2HEVT));
    if (er) { kfree(map); mac_power_off(); return -5; }
    rtw_w8(R_C2HEVT, 0);
    int pr = parse_efuse(map);
    wlog("wifi: efuse: rfe %u pkg %u xtal %02x swing %u/%u cck idx %02x %02x %02x", R.rfe, R.pkg, R.xtal, R.bb_swing_2g, R.bb_swing_5g, R.pwr_cck[0], R.pwr_cck[1], R.pwr_cck[2]);
    wlog("wifi: MAC address %02x:%02x:%02x:%02x:%02x:%02x%s", R.mac[0], R.mac[1], R.mac[2], R.mac[3], R.mac[4], R.mac[5], pr ? " (invalid)" : "");
    kfree(map);
    if (pr) { mac_power_off(); return -6; }
    mac_power_off();

    // real start: power on, firmware, MAC, radio
    if (mac_power_on()) { wlog("wifi: second power-on failed"); return -7; }
    if (download_firmware()) { mac_power_off(); return -8; }
    if (init_trx_cfg()) { mac_power_off(); return -9; }
    chip_mac_init();
    drv_info_cfg();
    usb_interface_cfg();
    phy_set_param();
    wlog("wifi: MAC and PHY initialised (io errors %u)", R.io_err);
    rtw_send_fw_info();
    coex_wifi_only();
    rtw_w8(R_C2HEVT, 0);
    xhci_wl_rx_start();
    rtw_w32(R_RCR, R.rcr);
    R.state = 2;
    for (int i = 0; i < 6; i++) rtw_w8(R_MACID + (uint32_t)i, R.mac[i]);       // our address (acknowledgements, unicast reception)
    rtw_w32_mask(R_CR, 0x30000, 0);
    rtw_w8(R_BCN_CTRL, EN_BCN_FUNCTION);
    rtw_set_channel(1);
    wlog("wifi: on channel 1, tx power index %u", R.tx_idx);
    return 0;
}
