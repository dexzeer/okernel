// Intel integrated graphics: detection and a read-only report (see i915.h).
#include "i915.h"

// ---- pure helpers (host-tested, tests/test_i915.c) ------------------------------

struct i915_gen i915_classify(uint16_t id) {
    struct i915_gen g = { 0, "unknown Intel graphics" };
    uint16_t hi = id & 0xFF00;
    switch (hi) {
    case 0x2500: case 0x2700: g.gen = 3; g.name = "i915/i945 (Gen3)"; break;
    case 0x2900: g.gen = 4; g.name = "i965/G33/Q35 (Gen3-4)"; break;
    case 0x2A00: g.gen = 4; g.name = "GM965/GM45 (Gen4)"; break;
    case 0x2E00: g.gen = 4; g.name = "G45/G41 (Gen4)"; break;
    case 0xA000: g.gen = 3; g.name = "Pineview (Gen3)"; break;
    case 0x0000: if (id == 0x0042 || id == 0x0046) { g.gen = 5; g.name = "Ironlake (Gen5)"; } break;
    case 0x0100:                                        // 0x01xx: Sandy Bridge below 0x50, Ivy Bridge above
        if ((id & 0xFF) >= 0x50) { g.gen = 7; g.name = "Ivy Bridge (Gen7)"; }
        else { g.gen = 6; g.name = "Sandy Bridge (Gen6)"; }
        break;
    case 0x0F00: g.gen = 7; g.name = "Bay Trail (Gen7)"; break;
    case 0x0400: case 0x0A00: case 0x0C00: case 0x0D00: g.gen = 7; g.name = "Haswell (Gen7.5)"; break;
    case 0x1600: g.gen = 8; g.name = "Broadwell (Gen8)"; break;
    case 0x2200: g.gen = 8; g.name = "Cherry Trail (Gen8)"; break;
    case 0x1900: g.gen = 9; g.name = "Skylake (Gen9)"; break;
    case 0x5900: g.gen = 9; g.name = "Kaby Lake (Gen9.5)"; break;
    case 0x3E00: g.gen = 9; g.name = "Coffee Lake (Gen9.5)"; break;
    case 0x9B00: g.gen = 9; g.name = "Comet Lake (Gen9.5)"; break;
    case 0x3100: g.gen = 9; g.name = "Gemini Lake (Gen9)"; break;
    case 0x5A00: case 0x1A00: g.gen = 9; g.name = "Broxton (Gen9)"; break;
    case 0x8A00: g.gen = 11; g.name = "Ice Lake (Gen11)"; break;
    case 0x4500: g.gen = 11; g.name = "Elkhart Lake (Gen11)"; break;
    case 0x9A00: g.gen = 12; g.name = "Tiger Lake (Gen12)"; break;
    case 0x4C00: g.gen = 12; g.name = "Rocket Lake (Gen12)"; break;
    case 0x4600: case 0x4680: g.gen = 12; g.name = "Alder Lake (Gen12)"; break;
    case 0x4900: g.gen = 12; g.name = "DG1 (Gen12)"; break;
    case 0xA700: g.gen = 12; g.name = "Raptor Lake (Gen12)"; break;
    case 0x7D00: g.gen = 12; g.name = "Meteor Lake (Xe-LPG)"; break;
    default: break;
    }
    if (g.gen == 0 && (id & 0xFF00) == 0x3E00) { g.gen = 9; g.name = "Coffee Lake (Gen9.5)"; }
    return g;
}

// Plane control register: display plane A..C control (DSPCNTR on gen<=8,
// PLANE_CTL on gen>=9). Bit 31 enables the plane in both.
int i915_plane_enabled(uint8_t gen, uint32_t ctl) { (void)gen; return (ctl >> 31) & 1; }

const char* i915_plane_format(uint8_t gen, uint32_t ctl) {
    if (gen >= 9) {                                     // PLANE_CTL bits 27:24
        switch ((ctl >> 24) & 0xF) {
        case 0x2: return "YUV 4:2:2";
        case 0x4: return "XRGB 8888";
        case 0x5: return "XBGR 8888";
        case 0x6: return "XRGB 2:10:10:10";
        case 0x7: return "XBGR 2:10:10:10";
        case 0x8: return "RGB 565";
        case 0xC: return "XRGB 16:16:16:16 float";
        default: return "other";
        }
    }
    switch ((ctl >> 26) & 0xF) {                        // DSPCNTR bits 29:26
    case 0x2: return "8bpp indexed";
    case 0x5: return "RGB 565";
    case 0x6: return "XRGB 8888";
    case 0x7: return "ARGB 8888";
    case 0x8: return "XRGB 2:10:10:10";
    case 0xA: return "XBGR 2:10:10:10";
    default: return "other";
    }
}

int i915_plane_tiling(uint8_t gen, uint32_t ctl) {
    if (gen >= 9) {                                     // PLANE_CTL bits 12:10
        uint32_t t = (ctl >> 10) & 7;
        return t == 0 ? 0 : t == 1 ? 1 : t == 4 ? 2 : 3;
    }
    return (ctl >> 10) & 1 ? 1 : 0;                     // DSPCNTR bit 10: X tiled
}

int i915_pte_valid(uint8_t gen, uint32_t lo) { (void)gen; return lo & 1; }

uint64_t i915_pte_phys(uint8_t gen, uint32_t lo, uint32_t hi) {
    if (gen >= 8)                                       // 64-bit PTE: address bits 38:12
        return ((uint64_t)(hi & 0x7F) << 32) | (lo & 0xFFFFF000u);
    if (gen >= 6)                                       // 32-bit PTE: bits 11:4 hold address bits 39:32
        return ((uint64_t)((lo >> 4) & 0xFF) << 32) | (lo & 0xFFFFF000u);
    return lo & 0xFFFFF000u;
}

#ifdef KERNEL
#include "display.h"
#include "paging.h"
#include "pcix.h"
#include "serial.h"

static struct {
    int found;
    struct pci_loc loc;
    uint16_t did;
    uint8_t rev;
    struct i915_gen g;
    uint32_t bar0, bar0_size, bar2, bar2_size;
    volatile uint8_t* mmio;       // BAR0, uncached (0 if it could not be mapped)
    uint32_t mmio_len;
} I;

// Boot-time probe: identity and PCI configuration only. Maps BAR0 so the report
// can read it later (windows must be created before the first process).
const struct display_ops* i915_probe(uint32_t* backbuffer, int w, int h) {
    (void)backbuffer; (void)w; (void)h;
    struct pci_loc loc;
    for (int n = 0; pcix_find_class(0x03, 0xFF, n, &loc); n++) {
#ifdef I915_TEST_VENDOR        /* test build: pose as the Intel device (QEMU has none) */
        if ((pcix_read32(loc, 0) & 0xFFFF) != I915_TEST_VENDOR) continue;
#else
        if ((pcix_read32(loc, 0) & 0xFFFF) != 0x8086) continue;
#endif
        I.found = 1;
        I.loc = loc;
        I.did = (uint16_t)(pcix_read32(loc, 0) >> 16);
        I.rev = pcix_read8(loc, 0x08);
        I.g = i915_classify(I.did);
#ifdef I915_TEST_GEN
        I.g.gen = I915_TEST_GEN;
#endif
        int is_mem, above;
        I.bar0 = pcix_bar(loc, 0, &is_mem, &above);
        I.bar0_size = pcix_bar_size(loc, 0);
        I.bar2 = pcix_bar(loc, 2, &is_mem, &above);
        I.bar2_size = pcix_bar_size(loc, 2);
        serial_printf("[i915] Intel graphics %x:%x rev %x at %d:%d.%d: %s, BAR0 %x (%u KB), aperture %x (%u MB)\n",
                      0x8086, I.did, I.rev, loc.bus, loc.dev, loc.fn, I.g.name,
                      I.bar0, I.bar0_size >> 10, I.bar2, I.bar2_size >> 20);
        if (I.bar0 && I.bar0_size && I.bar0_size <= 16u * 1024 * 1024) {
            I.mmio = (volatile uint8_t*)paging_map_mmio(I.bar0, I.bar0_size);
            I.mmio_len = I.bar0_size;
        }
        break;
    }
    return 0;                      // the display keeps using the GRUB framebuffer
}

static uint32_t rd(uint32_t off) {
    if (!I.mmio || off + 4 > I.mmio_len) return 0xFFFFFFFFu;
    return *(volatile uint32_t*)(I.mmio + off);
}

#include <stdarg.h>
static void put_dec(char** p, uint32_t v) {
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) *(*p)++ = t[--k];
}
static void put_hex(char** p, uint32_t v) {
    char t[9]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { uint32_t d = v & 15; t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    while (k) *(*p)++ = t[--k];
}
static void line(void (*out)(const char*, void*), void* ud, const char* fmt, ...) {
    char b[160], *p = b;
    va_list ap;
    va_start(ap, fmt);
    for (const char* f = fmt; *f && p < b + 140; f++) {
        if (*f != '%') { *p++ = *f; continue; }
        f++;
        if (*f == 'u') put_dec(&p, va_arg(ap, uint32_t));
        else if (*f == 'x') put_hex(&p, va_arg(ap, uint32_t));
        else if (*f == 's') { const char* s = va_arg(ap, const char*); while (s && *s && p < b + 150) *p++ = *s++; }
        else *p++ = *f;
    }
    va_end(ap);
    *p = 0;
    out(b, ud);
}

int i915_report(void (*out)(const char*, void*), void* ud) {
    if (!I.found) { out("no Intel graphics device found", ud); return 0; }
    uint8_t gen = I.g.gen;
    line(out, ud, "Intel %x:%x rev %x - %s", 0x8086, I.did, I.rev, I.g.name);
    line(out, ud, "BAR0 (regs+GTT) %x size %u KB, BAR2 (aperture) %x size %u MB, mapped %s",
         I.bar0, I.bar0_size >> 10, I.bar2, I.bar2_size >> 20, I.mmio ? "yes" : "no");
    if (gen >= 6) {
        uint32_t gmch = pcix_read16(I.loc, 0x50);
        uint32_t bdsm = pcix_read32(I.loc, gen >= 9 ? 0xB0 : 0x5C);
        line(out, ud, "GMCH_CTL %x  BDSM (stolen memory base) %x", gmch, bdsm);
    }
    {
        extern uint8_t* graphics_fb_base(void);
        uint32_t fb = (uint32_t)graphics_fb_base();
        const char* where = (I.bar2 && fb >= I.bar2 && fb < I.bar2 + I.bar2_size) ? "inside the aperture" : "outside the aperture";
        line(out, ud, "GRUB framebuffer at %x (%s), aperture offset %x", fb, where, fb - I.bar2);
    }
    if (!I.mmio) { out("registers not mapped: no further data", ud); return 1; }
    for (int p = 0; p < 3; p++) {
        uint32_t base = (uint32_t)p * 0x1000;
        uint32_t conf = rd(0x70008 + base), src = rd(0x6001C + base);
        uint32_t htot = rd(0x60000 + base), vtot = rd(0x6000C + base);
        uint32_t pctl = rd(0x70180 + base), pstr = rd(0x70188 + base), psurf = rd(0x7019C + base);
        uint32_t ccn = rd(0x70080 + base), cba = rd(0x70084 + base), cpos = rd(0x70088 + base);
        if (conf == 0xFFFFFFFFu || (!(conf & 0x80000000u) && !i915_plane_enabled(gen, pctl))) {
            line(out, ud, "pipe %s: off (PIPECONF %x)", p == 0 ? "A" : p == 1 ? "B" : "C", conf);
            continue;
        }
        line(out, ud, "pipe %s: PIPECONF %x  size %ux%u  htotal %u vtotal %u", p == 0 ? "A" : p == 1 ? "B" : "C",
             conf, (src >> 16) + 1, (src & 0xFFFF) + 1, (htot & 0xFFFF) + 1, (vtot & 0xFFFF) + 1);
        line(out, ud, "  plane1: ctl %x %s %s tiling %u stride %u surf %x", pctl,
             i915_plane_enabled(gen, pctl) ? "enabled" : "disabled", i915_plane_format(gen, pctl),
             (uint32_t)i915_plane_tiling(gen, pctl), pstr, psurf);
        line(out, ud, "  cursor: ctl %x base %x pos %x", ccn, cba, cpos);
        // where the scanout buffer lives: GTT entry for the plane's surface address
        if (I.mmio && gen >= 6 && i915_plane_enabled(gen, pctl)) {
            uint32_t gtt = I.bar0_size / 2;                        // GTT = upper half of BAR0
            uint32_t idx = psurf >> 12;
            uint32_t esz = gen >= 8 ? 8 : 4;
            if (gtt + (idx + 1) * esz <= I.mmio_len) {
                uint32_t lo = rd(gtt + idx * esz), hi = gen >= 8 ? rd(gtt + idx * esz + 4) : 0;
                uint64_t phys = i915_pte_phys(gen, lo, hi);
                line(out, ud, "  surf GTT entry %x:%x valid %u -> phys %x", hi, lo,
                     (uint32_t)i915_pte_valid(gen, lo), (uint32_t)phys);
            }
        }
    }
    return 1;
}
#endif
