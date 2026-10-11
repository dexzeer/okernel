#include "pcix.h"
#include "io.h"

static uint32_t cfg_addr(struct pci_loc l, uint8_t off) {
    return (1u << 31) | ((uint32_t)l.bus << 16) | ((uint32_t)l.dev << 11) | ((uint32_t)l.fn << 8) | (off & 0xFC);
}

uint32_t pcix_read32(struct pci_loc l, uint8_t off) {
    outl(0xCF8, cfg_addr(l, off));
    return inl(0xCFC);
}
uint16_t pcix_read16(struct pci_loc l, uint8_t off) {
    return (uint16_t)(pcix_read32(l, off) >> ((off & 2) * 8));
}
uint8_t pcix_read8(struct pci_loc l, uint8_t off) {
    return (uint8_t)(pcix_read32(l, off) >> ((off & 3) * 8));
}
void pcix_write32(struct pci_loc l, uint8_t off, uint32_t v) {
    outl(0xCF8, cfg_addr(l, off));
    outl(0xCFC, v);
}
void pcix_write16(struct pci_loc l, uint8_t off, uint16_t v) {
    uint32_t cur = pcix_read32(l, off & 0xFC);
    int sh = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pcix_write32(l, off & 0xFC, cur);
}

// Enumerate every function; `match` returns 1 for a hit. Skips empty slots
// and only probes functions 1-7 of multifunction devices (header type bit 7).
static int enumerate(int (*match)(struct pci_loc, const void*), const void* arg, int nth, struct pci_loc* out) {
    int seen = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            struct pci_loc l0 = { (uint8_t)bus, (uint8_t)dev, 0 };
            if ((pcix_read32(l0, 0) & 0xFFFF) == 0xFFFF) continue;
            int multi = (pcix_read8(l0, 0x0E) & 0x80) != 0;
            for (int fn = 0; fn < (multi ? 8 : 1); fn++) {
                struct pci_loc l = { (uint8_t)bus, (uint8_t)dev, (uint8_t)fn };
                if ((pcix_read32(l, 0) & 0xFFFF) == 0xFFFF) continue;
                if (match(l, arg) && seen++ == nth) { *out = l; return 1; }
            }
        }
    }
    return 0;
}

struct id_arg { uint16_t v, d; };
static int match_id(struct pci_loc l, const void* a) {
    const struct id_arg* x = (const struct id_arg*)a;
    uint32_t id = pcix_read32(l, 0);
    return (id & 0xFFFF) == x->v && (x->d == 0xFFFF || (id >> 16) == x->d);
}
int pcix_find_id(uint16_t vendor, uint16_t device, int nth, struct pci_loc* out) {
    struct id_arg a = { vendor, device };
    return enumerate(match_id, &a, nth, out);
}

static int match_any(struct pci_loc l, const void* a) { (void)l; (void)a; return 1; }
int pcix_find_any(int nth, struct pci_loc* out) { return enumerate(match_any, 0, nth, out); }

struct cls_arg { uint8_t c, s; };
static int match_class(struct pci_loc l, const void* a) {
    const struct cls_arg* x = (const struct cls_arg*)a;
    uint32_t r = pcix_read32(l, 8);
    return (uint8_t)(r >> 24) == x->c && (x->s == 0xFF || (uint8_t)(r >> 16) == x->s);
}
int pcix_find_class(uint8_t cls, uint8_t subclass, int nth, struct pci_loc* out) {
    struct cls_arg a = { cls, subclass };
    return enumerate(match_class, &a, nth, out);
}

void pcix_enable(struct pci_loc l, int intx_off) {
    uint16_t cmd = pcix_read16(l, 0x04);
    cmd |= 0x0002 | 0x0004;                 // memory space, bus master
    if (intx_off) cmd |= 0x0400;            // we poll: keep the level-triggered INTx line quiet
    pcix_write16(l, 0x04, cmd);
}

uint8_t pcix_find_cap(struct pci_loc l, uint8_t id, uint8_t after) {
    if (!(pcix_read16(l, 0x06) & 0x10)) return 0;          // status: no capability list
    uint8_t p = pcix_read8(l, 0x34) & 0xFC;
    if (after) {                                            // continue past `after`
        while (p && p != after) p = pcix_read8(l, p + 1) & 0xFC;
        if (!p) return 0;
        p = pcix_read8(l, p + 1) & 0xFC;
    }
    for (int guard = 0; p && guard < 48; guard++) {
        if (pcix_read8(l, p) == id) return p;
        p = pcix_read8(l, p + 1) & 0xFC;
    }
    return 0;
}

uint32_t pcix_bar(struct pci_loc l, int idx, int* is_mem, int* above4g) {
    uint8_t off = (uint8_t)(0x10 + 4 * idx);
    uint32_t lo = pcix_read32(l, off);
    if (is_mem) *is_mem = !(lo & 1);
    if (above4g) *above4g = 0;
    if (lo & 1) return 0;                                   // I/O BAR
    uint32_t base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 3) == 2 && idx < 5) {                  // 64-bit memory BAR
        uint32_t hi = pcix_read32(l, off + 4);
        if (hi) { if (above4g) *above4g = 1; return 0; }
    }
    return base;
}

uint32_t pcix_bar_size(struct pci_loc l, int idx) {
    uint8_t off = (uint8_t)(0x10 + 4 * idx);
    // Memory decode is off and the BAR holds all-ones for a moment: no interrupt
    // handler may touch the device (or the framebuffer behind it) in between.
    uint32_t fl;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(fl) :: "memory");
    uint16_t cmd = pcix_read16(l, 0x04);
    pcix_write16(l, 0x04, cmd & ~0x0003);
    uint32_t orig = pcix_read32(l, off);
    pcix_write32(l, off, 0xFFFFFFFF);
    uint32_t v = pcix_read32(l, off);
    pcix_write32(l, off, orig);
    pcix_write16(l, 0x04, cmd);
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
    if (orig & 1) v &= 0xFFFFFFFCu; else v &= 0xFFFFFFF0u;
    return v ? (~v + 1) : 0;
}
