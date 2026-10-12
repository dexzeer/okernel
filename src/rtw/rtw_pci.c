// PCIe backend of the RTL8821C driver (see rtw_pci.h).
#include "rtw_pci.h"
#include "rtw_priv.h"
#include "../memory.h"
#include "../memlayout.h"
#ifndef RTWPCI_HOST
#include "../paging.h"
#include "../pcix.h"
#endif
#include "../string.h"

#define B(n) (1u << (n))

// ---- the device ---------------------------------------------------------------------------------------------------------
#ifdef RTWPCI_HOST
struct pci_loc { uint8_t bus, dev, fn; };
#endif
static struct {
    int found, mapped;
    struct pci_loc loc;
    uint16_t did;
    volatile uint8_t* mmio;
    uint8_t pcie_cap;
} P;

int rtwpci_present(void) { return P.found && P.mapped; }
uint16_t rtwpci_device_id(void) { return P.did; }

#ifdef RTWPCI_HOST
// host test: the chip model supplies the register file and the "DMA" memory is the process's own (V2P/P2V are a fixed offset on -m32)
uint32_t host_mmio_rd(uint32_t addr, int len);
void host_mmio_wr(uint32_t addr, uint32_t v, int len);
void rtwpci_host_attach(uint16_t did) { P.found = P.mapped = did != 0; P.did = did; }
int rtwpci_init(void) { return 0; }
#else
int rtwpci_init(void) {
    struct pci_loc l;
    for (int n = 0; pcix_find_id(0x10ec, 0xFFFF, n, &l); n++) {
        uint16_t did = (uint16_t)(pcix_read32(l, 0) >> 16);
        if (did != 0xc821 && did != 0xb821) continue;
        uint8_t pm = pcix_find_cap(l, 1, 0);
        if (pm) { uint16_t cs = pcix_read16(l, (uint8_t)(pm + 4)); if (cs & 3) pcix_write16(l, (uint8_t)(pm + 4), cs & ~3u); }
        pcix_enable(l, 1);
        int is_mem, above;
        uint32_t bar = pcix_bar(l, 2, &is_mem, &above);                  // the register window is BAR 2 (BAR 0 is the I/O copy)
        uint32_t size = pcix_bar_size(l, 2);
        if (!bar || !is_mem || above || size < 0x10000) { wlog("pcie: RTL%04x at %x:%x.%x: BAR2 unusable (base %x size %x)", did, l.bus, l.dev, l.fn, bar, size); return 0; }
        P.mmio = (volatile uint8_t*)paging_map_mmio(bar, 0x10000);
        if (!P.mmio) { wlog("pcie: cannot map the registers"); return 0; }
        P.loc = l; P.did = did; P.found = 1; P.mapped = 1;
        P.pcie_cap = pcix_find_cap(l, 0x10, 0);
        wlog("pcie: Realtek %04x at %x:%x.%x, registers at %x", did, l.bus, l.dev, l.fn, bar);
        return 1;
    }
    return 0;
}
#endif

uint32_t rtwpci_rd(uint32_t addr, int len) {
#ifdef RTWPCI_HOST
    return host_mmio_rd(addr, len);
#endif
    if (!P.mmio) return 0xFFFFFFFFu >> (8 * (4 - len));
    volatile uint8_t* a = P.mmio + addr;
    if (len == 1) return *a;
    if (len == 2) return *(volatile uint16_t*)a;
    return *(volatile uint32_t*)a;
}

void rtwpci_wr(uint32_t addr, uint32_t v, int len) {
#ifdef RTWPCI_HOST
    host_mmio_wr(addr, v, len); return;
#endif
    if (!P.mmio) return;
    volatile uint8_t* a = P.mmio + addr;
    if (len == 1) *a = (uint8_t)v;
    else if (len == 2) *(volatile uint16_t*)a = (uint16_t)v;
    else *(volatile uint32_t*)a = v;
}

// ---- registers of the PCIe bridge section --------------------------------------------------------------------------------
#define RTK_PCI_CTRL        0x300
#define BIT_RST_TRXDMA_INTF B(20)
#define BIT_RX_TAG_EN       B(15)
#define REG_MDIO_V1         0x3F4
#define REG_PCIE_MIX_CFG    0x3F8
#define BIT_MDIO_WFLAG_V1   B(5)
#define RTK_PCI_TXBD_BCN_WORK 0x383
#define BIT_PCI_BCNQ_FLAG   B(4)
#define RTK_PCI_TXBD_RWPTR_CLR 0x39C
#define RTK_PCI_TXBD_H2CQ_CSR  0x1330
#define RTK_PCI_RXBD_DESA   0x338
#define RTK_PCI_RXBD_NUM    0x382
#define RTK_PCI_RXBD_IDX    0x3B4
#define RTK_PCI_HIMR0       0x0B0
#define RTK_PCI_HIMR1       0x0B8
#define RTK_PCI_HIMR3       0x10B8

enum { Q_BK, Q_BE, Q_VI, Q_VO, Q_MGMT, Q_HI0, Q_H2C, Q_BCN, Q_N };
static const uint16_t q_desa[Q_N] = { 0x330, 0x328, 0x320, 0x318, 0x310, 0x340, 0x1320, 0x308 };
static const uint16_t q_num[Q_N]  = { 0x38A, 0x388, 0x386, 0x384, 0x380, 0x38C, 0x1328, 0 };
static const uint16_t q_idx[Q_N]  = { 0x3AC, 0x3A8, 0x3A4, 0x3A0, 0x3B0, 0x3B8, 0x132C, 0 };

#define TXBD_OWN 15
#define SLOT_BYTES 2432              // descriptor + the longest frame we send (2300) with room to spare
#define RX_BUF 11478                 // the chip's receive buffer size
#define RX_N 128

struct txbd { uint16_t buf_size, psb_len; uint32_t dma; };      // two of these per transmitted packet (descriptor, payload)

struct ring {
    uint8_t* bd; uint32_t bd_phys;
    int len, wp, rp;
    int slot_bytes;
    uint8_t* slot[32];
    uint32_t slot_phys[32];
};
static struct ring TX[Q_N];

static struct {
    uint8_t* bd; uint32_t bd_phys;
    uint8_t* buf[RX_N];
    uint32_t phys[RX_N];
    int rp;
} RX;

static uint32_t tx_pkts, tx_full, rx_pkts;
static int alloc_done;

static void* dma_alloc(uint32_t size, uint32_t align) {
    uint8_t* p = (uint8_t*)kcalloc(1, size + align);
    if (!p) return 0;
    return (void*)(((uint32_t)p + align - 1) & ~(align - 1));
}

static int ring_alloc(struct ring* r, int len, int slot_bytes) {
    r->len = len; r->wp = r->rp = 0; r->slot_bytes = slot_bytes;
    r->bd = (uint8_t*)dma_alloc((uint32_t)len * 16, 4096);
    if (!r->bd) return -1;
    r->bd_phys = V2P(r->bd);
    for (int i = 0; slot_bytes && i < len; i++) {
        r->slot[i] = (uint8_t*)dma_alloc((uint32_t)slot_bytes, 16);
        if (!r->slot[i]) return -1;
        r->slot_phys[i] = V2P(r->slot[i]);
    }
    return 0;
}

int rtwpci_alloc(void) {
    if (alloc_done) return 0;
    static const int lens[Q_N] = { 8, 16, 8, 8, 16, 8, 16, 1 };
    for (int q = 0; q < Q_N; q++) {
        int bytes = (q == Q_BE || q == Q_MGMT || q == Q_HI0) ? SLOT_BYTES : q == Q_H2C ? 128 : q == Q_BCN ? 4352 : 0;   // the other queues are never used
        if (ring_alloc(&TX[q], lens[q], bytes)) return -1;
    }
    RX.bd = (uint8_t*)dma_alloc(RX_N * 8, 4096);
    if (!RX.bd) return -1;
    RX.bd_phys = V2P(RX.bd);
    for (int i = 0; i < RX_N; i++) {
        RX.buf[i] = (uint8_t*)dma_alloc(RX_BUF, 16);
        if (!RX.buf[i]) return -1;
        RX.phys[i] = V2P(RX.buf[i]);
    }
    alloc_done = 1;
    return 0;
}

static void rx_arm(int i) {
    uint8_t* d = RX.bd + i * 8;
    ((uint16_t*)d)[0] = RX_BUF;
    ((uint16_t*)d)[1] = 0;
    *(uint32_t*)(d + 4) = RX.phys[i];
}

void rtwpci_hci_setup(void) {
    if (!alloc_done) return;
    rtwpci_wr(RTK_PCI_HIMR0, 0, 4);                           // no interrupts: everything is polled
    rtwpci_wr(RTK_PCI_HIMR1, 0, 4);
    rtwpci_wr(RTK_PCI_HIMR3, 0, 4);
    rtwpci_wr(RTK_PCI_CTRL + 3, (uint8_t)(rtwpci_rd(RTK_PCI_CTRL + 3, 1) | 0xf7), 1);
    for (int q = 0; q < Q_N; q++) {
        struct ring* r = &TX[q];
        r->wp = r->rp = 0;
        memset(r->bd, 0, (uint32_t)r->len * 16);
        rtwpci_wr(q_desa[q], r->bd_phys, 4);
        if (q_num[q]) rtwpci_wr(q_num[q], (uint32_t)r->len & 0xfff, 2);
    }
    for (int i = 0; i < RX_N; i++) rx_arm(i);
    RX.rp = 0;
    rtwpci_wr(RTK_PCI_RXBD_NUM, RX_N & 0xfff, 2);
    rtwpci_wr(RTK_PCI_RXBD_DESA, RX.bd_phys, 4);
    rtwpci_wr(RTK_PCI_TXBD_RWPTR_CLR, 0xffffffffu, 4);
    rtwpci_wr(RTK_PCI_TXBD_H2CQ_CSR, rtwpci_rd(RTK_PCI_TXBD_H2CQ_CSR, 4) | B(16) | B(8), 4);   // clear the H2C queue's host and hardware index
    rtwpci_wr(RTK_PCI_CTRL, rtwpci_rd(RTK_PCI_CTRL, 4) | BIT_RST_TRXDMA_INTF | BIT_RX_TAG_EN, 4);   // reset the DMA, receive tags on
}

// ---- MDIO / link settings ---------------------------------------------------------------------------------------------------
static void mdio_write(uint8_t addr, uint16_t data, int gen1) {
    rtwpci_wr(REG_MDIO_V1, data, 2);
    uint8_t page = addr < 32 ? 0 : 1;
    page = (uint8_t)(page + (gen1 ? 0 : 2));
    rtwpci_wr(REG_PCIE_MIX_CFG, addr & 0x1f, 1);
    rtwpci_wr(REG_PCIE_MIX_CFG + 3, page, 1);
    rtwpci_wr(REG_PCIE_MIX_CFG, rtwpci_rd(REG_PCIE_MIX_CFG, 4) | BIT_MDIO_WFLAG_V1, 4);
    for (int i = 0; i < 20; i++) {
        if (!(rtwpci_rd(REG_PCIE_MIX_CFG, 4) & BIT_MDIO_WFLAG_V1)) return;
        rtw_udelay(10);
    }
    wlog("pcie: MDIO write %02x did not complete", addr);
}

void rtwpci_phy_cfg(void) {
    mdio_write(0x09, 0x6380, 1);                              // the chip's gen1 PHY parameter
#ifndef RTWPCI_HOST
    if (P.pcie_cap) {
        uint8_t c = P.pcie_cap;
        uint16_t lc = pcix_read16(P.loc, (uint8_t)(c + 0x10));
        pcix_write16(P.loc, (uint8_t)(c + 0x10), lc & ~3u);   // no ASPM L0s / L1 on our side of the link
        uint16_t dc2 = pcix_read16(P.loc, (uint8_t)(c + 0x28));
        pcix_write16(P.loc, (uint8_t)(c + 0x28), dc2 | 0x10); // completion timeout off (as the vendor driver does for this chip)
        wlog("pcie: link control %04x -> %04x", lc, pcix_read16(P.loc, (uint8_t)(c + 0x10)));
    }
#endif
}

// ---- transmit ---------------------------------------------------------------------------------------------------------------
static int queue_of(int qsel) {
    switch (qsel) {
    case 16: return Q_BCN;
    case 17: return Q_HI0;
    case 18: return Q_MGMT;
    case 19: return Q_H2C;
    case 0: case 3: return Q_BE;
    case 1: case 2: return Q_BK;
    case 4: case 5: return Q_VI;
    default: return Q_VO;
    }
}

static void fill_bd(struct ring* r, int i, uint32_t phys, int total, int own) {
    struct txbd* b = (struct txbd*)(r->bd + i * 16);
    uint16_t psb = (uint16_t)((total - 1) / 128 + 1);
    if (own) psb |= (uint16_t)(1u << TXBD_OWN);
    b[0].psb_len = psb; b[0].buf_size = 48; b[0].dma = phys;
    b[1].psb_len = 0; b[1].buf_size = (uint16_t)(total - 48); b[1].dma = phys + 48;
}

int rtwpci_tx(int qsel, const uint8_t* pk, int total) {
    if (!alloc_done || total < 48) return -1;
    int q = queue_of(qsel);
    struct ring* r = &TX[q];
    if (!r->slot[0] || total > r->slot_bytes) return -1;
    if (q == Q_BCN) {                                         // reserved-page packets: one descriptor, owned by the hardware until sent
        memcpy(r->slot[0], pk, (uint32_t)total);
        fill_bd(r, 0, r->slot_phys[0], total, 1);
        __asm__ volatile("" ::: "memory");
        rtwpci_wr(RTK_PCI_TXBD_BCN_WORK, (uint8_t)(rtwpci_rd(RTK_PCI_TXBD_BCN_WORK, 1) | BIT_PCI_BCNQ_FLAG), 1);
        tx_pkts++;
        return 0;
    }
    for (int tries = 0; ; tries++) {                          // wait for a free descriptor (the chip's read pointer moves on as it sends)
        r->rp = (int)(rtwpci_rd(q_idx[q], 4) >> 16) & 0xfff;
        int avail = r->rp > r->wp ? r->rp - r->wp - 1 : r->len - r->wp + r->rp - 1;
        if (avail > 0) break;
        if (tries > 2000) { tx_full++; return -2; }
        rtw_udelay(10);
    }
    memcpy(r->slot[r->wp], pk, (uint32_t)total);
    fill_bd(r, r->wp, r->slot_phys[r->wp], total, 0);
    r->wp = (r->wp + 1) % r->len;
    __asm__ volatile("" ::: "memory");
    rtwpci_wr(q_idx[q], (uint32_t)r->wp & 0xfff, 2);
    tx_pkts++;
    if (q == Q_H2C) {                                         // commands are few: wait until the chip has taken it
        for (int i = 0; i < 2000; i++) {
            if (((rtwpci_rd(q_idx[q], 4) >> 16) & 0xfff) == (uint32_t)r->wp) break;
            rtw_udelay(10);
        }
    }
    return 0;
}

// ---- receive ------------------------------------------------------------------------------------------------------------------
int rtwpci_rx_get(const uint8_t** buf) {
    if (!alloc_done) return 0;
    uint32_t wp = (rtwpci_rd(RTK_PCI_RXBD_IDX, 4) >> 16) & 0xfff;
    if (wp == (uint32_t)RX.rp) return 0;
    *buf = RX.buf[RX.rp];
    rx_pkts++;
    return RX_BUF;
}

void rtwpci_rx_done(void) {
    rx_arm(RX.rp);
    RX.rp = (RX.rp + 1) % RX_N;
    __asm__ volatile("" ::: "memory");
    rtwpci_wr(RTK_PCI_RXBD_IDX, (uint32_t)RX.rp & 0xfff, 2);
}

void rtwpci_status(char* buf, int cap) {
    int n = 0;
    const char* s = P.found ? "PCIe adapter found" : "no PCIe adapter";
    while (s[n] && n < cap - 1) { buf[n] = s[n]; n++; }
    buf[n] = 0;
}
