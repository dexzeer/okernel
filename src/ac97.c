#include "ac97.h"
#include "io.h"
#include "lapic.h"
#include "memlayout.h"
#include "pcix.h"
#include "serial.h"

// NAM (mixer) registers, NABM (bus master) PCM-out box.
#define NAM_RESET   0x00
#define NAM_MASTER  0x02
#define NAM_PCMOUT  0x18
#define NAM_EXTID   0x28
#define NAM_EXTCTL  0x2A
#define NAM_RATE    0x2C

#define PO_BDBAR    0x10
#define PO_CIV      0x14
#define PO_LVI      0x15
#define PO_SR       0x16
#define PO_PICB     0x18
#define PO_CR       0x1B
#define GLOB_CNT    0x2C
#define GLOB_STA    0x30

#define CR_RPBM     0x01
#define CR_RR       0x02
#define SR_DCH      0x01

#define NBUF        32

static struct {
    int present;
    uint16_t nam, nabm;
    uint32_t buf_bytes;           // bytes per buffer descriptor
    int lvi;                      // the last value written to LVI (-1 = none yet)
} A;

int ac97_present(void) { return A.present; }

int ac97_exists(void) {
    struct pci_loc loc;
    return pcix_find_class(0x04, 0x01, 0, &loc);
}

int ac97_init(void* ring, uint32_t ring_bytes, void* bdl) {
    struct pci_loc loc;
    if (!pcix_find_class(0x04, 0x01, 0, &loc)) return -1;       // multimedia audio controller
    uint32_t id = pcix_read32(loc, 0);
    uint32_t b0 = pcix_read32(loc, 0x10), b1 = pcix_read32(loc, 0x14);
    if (!(b0 & 1) || !(b1 & 1)) { serial_printf("[ac97] %x:%x: BARs are not I/O ports\n", id & 0xFFFF, id >> 16); return -1; }
    A.nam = (uint16_t)(b0 & 0xFFFC);
    A.nabm = (uint16_t)(b1 & 0xFFFC);
    pcix_write16(loc, 4, (uint16_t)(pcix_read16(loc, 4) | 0x5));  // I/O space + bus master
    pcix_enable(loc, 1);
    if (ring_bytes % (NBUF * 4) || ring_bytes / NBUF / 2 > 0xFFFE) return -1;
    A.buf_bytes = ring_bytes / NBUF;
    serial_printf("[ac97] controller %x:%x at %d:%d.%d, mixer port %x, bus master port %x\n", id & 0xFFFF, id >> 16, loc.bus, loc.dev, loc.fn, A.nam, A.nabm);

    outl(A.nabm + GLOB_CNT, 0x02);                                 // leave cold reset
    for (int i = 0; i < 1000; i++) { if (inl(A.nabm + GLOB_STA) & 0x100) break; pit_delay_us(1000); }   // primary codec ready
    outw(A.nam + NAM_RESET, 0);
    pit_delay_us(1000);
    outw(A.nam + NAM_MASTER, 0x0000);                              // 0 dB, not muted
    outw(A.nam + NAM_PCMOUT, 0x0000);
    if (inw(A.nam + NAM_EXTID) & 1) {                              // variable rate supported: say 48 kHz explicitly
        outw(A.nam + NAM_EXTCTL, (uint16_t)(inw(A.nam + NAM_EXTCTL) | 1));
        outw(A.nam + NAM_RATE, 48000);
    }

    outb(A.nabm + PO_CR, CR_RR);                                   // reset the PCM-out engine
    for (int i = 0; i < 2000 && (inb(A.nabm + PO_CR) & CR_RR); i++) pit_delay_us(1);
    uint32_t ring_phys = V2P((uint32_t)ring);
    for (uint32_t i = 0; i < NBUF; i++) {                          // 32 buffers; the length is counted in 16-bit samples; no interrupts
        uint32_t* e = (uint32_t*)((uint8_t*)bdl + i * 8);
        e[0] = ring_phys + i * A.buf_bytes;
        e[1] = A.buf_bytes / 2;
    }
    outl(A.nabm + PO_BDBAR, V2P((uint32_t)bdl));
    A.lvi = NBUF - 1;
    outb(A.nabm + PO_LVI, (uint8_t)A.lvi);
    outb(A.nabm + PO_CR, CR_RPBM);                                 // run
    A.present = 1;
    return 0;
}

uint32_t ac97_pos(void) {
    if (!A.present) return 0;
    uint32_t civ = 0, picb = 0;
    for (int i = 0; i < 4; i++) {                                  // CIV and PICB are separate reads: retry across a buffer boundary
        civ = inb(A.nabm + PO_CIV);
        picb = inw(A.nabm + PO_PICB);
        if (inb(A.nabm + PO_CIV) == civ) break;
    }
    civ %= NBUF;
    uint32_t left = picb * 2;
    if (left > A.buf_bytes) left = A.buf_bytes;
    return civ * A.buf_bytes + (A.buf_bytes - left);
}

void ac97_tick(void) {
    if (!A.present) return;
    uint32_t civ = inb(A.nabm + PO_CIV) % NBUF;
    int want = (int)((civ + NBUF - 1) % NBUF);
    int halted = inw(A.nabm + PO_SR) & SR_DCH;
    if (want != A.lvi || halted) {                                 // writing LVI while RPBM is set and the engine halted restarts it
        A.lvi = want;
        outb(A.nabm + PO_LVI, (uint8_t)want);
    }
}

void ac97_set_volume(int percent) {
    if (!A.present) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint16_t att = (uint16_t)((31 * (100 - percent)) / 100);       // 5 bits, 1.5 dB per step
    uint16_t v = (uint16_t)((att << 8) | att);
    if (percent == 0) v |= 0x8000;
    outw(A.nam + NAM_PCMOUT, v);
}
