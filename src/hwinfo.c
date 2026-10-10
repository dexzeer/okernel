// Hardware report for the text-mode build: what a machine we have never seen is made of. It runs on
// plain VGA text with the PS/2 keyboard, so it works on any old PC even when no graphics mode does.
// Commands (shell.c): cpu, memmap, acpi, pci [page], pcidump B:D.F, gpu, all.

#include "hwinfo.h"
#include "terminal.h"
#include "vga.h"
#include "serial.h"
#include "io.h"
#include "pcix.h"
#include "i915.h"
#include "string.h"
#include "display.h"
const struct display_ops* i915_probe(uint32_t* backbuffer, int w, int h);
#include <stdarg.h>

static uint32_t g_mboot;

void hwinfo_init(uint32_t mboot_phys) { g_mboot = mboot_phys; }

// ---- output: screen + serial -----------------------------------------------------------------
static void out(const char* s) {
    terminal_puts(s);
    serial_puts(s);
}

static void put_num(char** p, uint32_t v, unsigned base, int width, int upper) {
    char t[12]; int k = 0;
    if (!v) t[k++] = '0';
    while (v) { uint32_t d = v % base; t[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; }
    while (k < width) t[k++] = '0';
    while (k) *(*p)++ = t[--k];
}

// %s %u %x (%02x, %04x, %08x) %c
static void pf(const char* fmt, ...) {
    char b[200], *p = b;
    va_list ap;
    va_start(ap, fmt);
    for (const char* f = fmt; *f && p < b + 180; f++) {
        if (*f != '%') { *p++ = *f; continue; }
        f++;
        int width = 0;
        if (*f == '0') f++;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'u') put_num(&p, va_arg(ap, uint32_t), 10, width, 0);
        else if (*f == 'x') put_num(&p, va_arg(ap, uint32_t), 16, width, 0);
        else if (*f == 'c') *p++ = (char)va_arg(ap, int);
        else if (*f == 's') { const char* s = va_arg(ap, const char*); while (s && *s && p < b + 190) *p++ = *s++; }
        else *p++ = *f;
    }
    va_end(ap);
    *p = 0;
    out(b);
}

static void head(const char* t) {
    terminal_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    pf("== %s ==\n", t);
    terminal_set_color(VGA_WHITE, VGA_BLACK);
}

static void gpu_line(const char* s, void* ud) { (void)ud; pf("%s\n", s); }

// ---- cpu -------------------------------------------------------------------------------------
static int has_cpuid(void) {
    uint32_t a, b;
    __asm__ volatile("pushfl; pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; popfl"
                     : "=r"(a), "=r"(b) :: "cc");
    return ((a ^ b) & 0x200000) != 0;
}

static void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

void hwinfo_cpu(void) {
    head("CPU");
    if (!has_cpuid()) { pf("no CPUID instruction (a 486 or older)\n"); return; }
    uint32_t a, b, c, d, maxleaf;
    char vendor[13];
    cpuid(0, &maxleaf, &b, &c, &d);
    *(uint32_t*)(vendor + 0) = b; *(uint32_t*)(vendor + 4) = d; *(uint32_t*)(vendor + 8) = c; vendor[12] = 0;
    pf("vendor %s, highest basic leaf %u\n", vendor, maxleaf);
    uint32_t emax;
    cpuid(0x80000000u, &emax, &b, &c, &d);
    if (emax >= 0x80000004u) {
        char brand[49];
        for (int i = 0; i < 3; i++) cpuid(0x80000002u + (uint32_t)i, (uint32_t*)(brand + i * 16), (uint32_t*)(brand + i * 16 + 4), (uint32_t*)(brand + i * 16 + 8), (uint32_t*)(brand + i * 16 + 12));
        brand[48] = 0;
        const char* s = brand;
        while (*s == ' ') s++;
        pf("brand  %s\n", s);
    }
    if (maxleaf >= 1) {
        cpuid(1, &a, &b, &c, &d);
        uint32_t fam = (a >> 8) & 15, mod = (a >> 4) & 15, step = a & 15;
        if (fam == 15) fam += (a >> 20) & 255;
        if (fam == 6 || fam >= 15) mod |= ((a >> 16) & 15) << 4;
        pf("family %u model %u (0x%02x) stepping %u   signature %08x   logical CPUs/package %u\n", fam, mod, mod, step, a, (b >> 16) & 255);
        static const struct { int reg; int bit; const char* n; } F[] = {
            { 0, 0, "fpu" }, { 0, 4, "tsc" }, { 0, 5, "msr" }, { 0, 6, "pae" }, { 0, 8, "cx8" }, { 0, 9, "apic" }, { 0, 11, "sep" },
            { 0, 12, "mtrr" }, { 0, 13, "pge" }, { 0, 15, "cmov" }, { 0, 16, "pat" }, { 0, 19, "clflush" }, { 0, 23, "mmx" }, { 0, 24, "fxsr" },
            { 0, 25, "sse" }, { 0, 26, "sse2" }, { 0, 28, "htt" }, { 1, 0, "sse3" }, { 1, 9, "ssse3" }, { 1, 19, "sse4.1" }, { 1, 20, "sse4.2" },
            { 1, 5, "vmx" }, { 1, 21, "x2apic" }, { 1, 28, "avx" }, { 1, 30, "rdrand" },
        };
        out("features:");
        for (unsigned i = 0; i < sizeof F / sizeof *F; i++)
            if ((F[i].reg ? c : d) & (1u << F[i].bit)) pf(" %s", F[i].n);
        out("\n");
    }
    if (emax >= 0x80000001u) {
        cpuid(0x80000001u, &a, &b, &c, &d);
        pf("extended: %s%s%s\n", (d & (1u << 29)) ? "64-bit(lm) " : "", (d & (1u << 20)) ? "nx " : "", (d & (1u << 11)) ? "syscall" : "");
    }
}

// ---- memory map ------------------------------------------------------------------------------
void hwinfo_memmap(void) {
    head("Memory map (from the bootloader)");
    uint32_t flags = *(volatile uint32_t*)(g_mboot + 0);
    pf("lower %u KB, upper %u KB\n", *(volatile uint32_t*)(g_mboot + 4), *(volatile uint32_t*)(g_mboot + 8));
    if (!(flags & (1u << 6))) { pf("no memory map\n"); return; }
    uint32_t len = *(volatile uint32_t*)(g_mboot + 44), addr = *(volatile uint32_t*)(g_mboot + 48), off = 0;
    static const char* const T[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad" };
    while (off + 24 <= len && off < 4096) {
        uint32_t sz = *(volatile uint32_t*)(addr + off);
        uint32_t bl = *(volatile uint32_t*)(addr + off + 4), bh = *(volatile uint32_t*)(addr + off + 8);
        uint32_t ll = *(volatile uint32_t*)(addr + off + 12), lh = *(volatile uint32_t*)(addr + off + 16);
        uint32_t ty = *(volatile uint32_t*)(addr + off + 20);
        pf("  %08x%08x  %08x%08x  %s\n", bh, bl, lh, ll, ty < 6 ? T[ty] : "?");
        off += sz + 4;
    }
    if (flags & (1u << 12)) {
        pf("bootloader framebuffer: %x pitch %u %ux%u bpp %u type %u\n", *(volatile uint32_t*)(g_mboot + 88), *(volatile uint32_t*)(g_mboot + 96),
           *(volatile uint32_t*)(g_mboot + 100), *(volatile uint32_t*)(g_mboot + 104), (uint32_t)*(volatile uint8_t*)(g_mboot + 108), (uint32_t)*(volatile uint8_t*)(g_mboot + 109));
    }
}

// ---- ACPI ------------------------------------------------------------------------------------
static int sum_ok(const uint8_t* p, uint32_t n) { uint8_t s = 0; while (n--) s = (uint8_t)(s + *p++); return s == 0; }

void hwinfo_acpi(void) {
    head("ACPI");
    uint32_t ebda = (uint32_t)(*(volatile uint16_t*)0x40E) << 4;
    uint32_t rsdp = 0;
    for (int pass = 0; pass < 2 && !rsdp; pass++) {
        uint32_t lo = pass == 0 ? ebda : 0xE0000, hi = pass == 0 ? ebda + 1024 : 0x100000;
        if (pass == 0 && (ebda < 0x80000 || ebda > 0xA0000)) continue;
        for (uint32_t p = lo; p + 20 <= hi; p += 16)
            if (!memcmp((const void*)p, "RSD PTR ", 8) && sum_ok((const uint8_t*)p, 20)) { rsdp = p; break; }
    }
    if (!rsdp) { pf("no RSDP found\n"); return; }
    const uint8_t* r = (const uint8_t*)rsdp;
    char oem[7];
    for (int i = 0; i < 6; i++) oem[i] = (char)r[9 + i];
    oem[6] = 0;
    uint32_t rsdt = *(const uint32_t*)(r + 16);
    pf("RSDP at %x, revision %u, OEM %s, RSDT %x\n", rsdp, (uint32_t)r[15], oem, rsdt);
    if (rsdt < 0x1000 || rsdt > 0xFFFF0000u) return;
    const uint8_t* t = (const uint8_t*)rsdt;
    uint32_t tlen = *(const uint32_t*)(t + 4);
    if (tlen < 36 || tlen > 4096) { pf("RSDT length %u looks wrong\n", tlen); return; }
    out("tables:");
    for (uint32_t i = 36; i + 4 <= tlen; i += 4) {
        uint32_t tp = *(const uint32_t*)(t + i);
        if (tp < 0x1000 || tp > 0xFFFF0000u) continue;
        const char* sg = (const char*)tp;
        pf(" %c%c%c%c", sg[0], sg[1], sg[2], sg[3]);
    }
    out("\n");
}

// ---- PCI -------------------------------------------------------------------------------------
static const char* vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel"; case 0x10EC: return "Realtek"; case 0x14E4: return "Broadcom"; case 0x168C: return "Atheros";
    case 0x1969: return "Qualcomm Atheros"; case 0x10DE: return "NVIDIA"; case 0x1002: return "AMD/ATI"; case 0x1022: return "AMD";
    case 0x1106: return "VIA"; case 0x1039: return "SiS"; case 0x1180: return "Ricoh"; case 0x104C: return "Texas Instruments";
    case 0x1217: return "O2 Micro"; case 0x197B: return "JMicron"; case 0x11AB: return "Marvell"; case 0x10B7: return "3Com";
    case 0x1814: return "Ralink"; case 0x1B21: return "ASMedia"; case 0x1AF4: return "virtio"; case 0x1234: return "QEMU/Bochs";
    case 0x10EE: return "Xilinx";
    }
    return "";
}

static const char* class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x00: return "unclassified";
    case 0x01: return s == 1 ? "IDE controller" : s == 6 ? "SATA (AHCI)" : s == 8 ? "NVMe" : "storage";
    case 0x02: return "network";
    case 0x03: return s == 0 ? "VGA display" : "display";
    case 0x04: return s == 3 ? "HD audio" : "multimedia";
    case 0x05: return "memory ctrl";
    case 0x06: return s == 0 ? "host bridge" : s == 1 ? "ISA bridge" : s == 4 ? "PCI bridge" : s == 7 ? "CardBus bridge" : "bridge";
    case 0x07: return "comm ctrl";
    case 0x08: return "system peripheral";
    case 0x09: return "input device";
    case 0x0C: return s == 3 ? "USB controller" : s == 5 ? "SMBus" : s == 0 ? "FireWire" : "serial bus";
    case 0x0D: return "wireless";
    }
    return "other";
}

#define PCI_PAGE 20
void hwinfo_pci(int page) {
    head("PCI devices");
    int idx = 0, shown = 0, total = 0;
    int first = (page < 1 ? 0 : page - 1) * PCI_PAGE;
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                struct pci_loc l = { (uint8_t)bus, (uint8_t)dev, (uint8_t)fn };
                uint32_t id = pcix_read32(l, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                total++;
                if (idx++ < first || shown >= PCI_PAGE) continue;
                uint32_t cls = pcix_read32(l, 8);
                uint16_t ven = (uint16_t)id;
                pf("%02x:%02x.%u  %04x:%04x  %02x%02x%02x  %s %s\n", bus, dev, fn, (uint32_t)ven, id >> 16, cls >> 24, (cls >> 16) & 255, (cls >> 8) & 255,
                   vendor_name(ven), class_name((uint8_t)(cls >> 24), (uint8_t)(cls >> 16)));
                shown++;
                if (fn == 0 && !((pcix_read32(l, 0x0C) >> 16) & 0x80)) break;      // single-function device
            }
    pf("%u devices; page %u of %u%s\n", (uint32_t)total, (uint32_t)(first / PCI_PAGE + 1), (uint32_t)((total + PCI_PAGE - 1) / PCI_PAGE),
       first + PCI_PAGE < total ? "  (type 'pci 2' for the next page)" : "");
}

void hwinfo_pcidump(const char* arg) {
    // B:D.F in hex
    unsigned v[3] = { 0, 0, 0 };
    int k = 0;
    for (const char* s = arg; s && *s && k < 3; s++) {
        if (*s == ':' || *s == '.') { k++; continue; }
        int h = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
        if (h >= 0) v[k] = v[k] * 16 + (unsigned)h;
    }
    struct pci_loc l = { (uint8_t)v[0], (uint8_t)(v[1] & 31), (uint8_t)(v[2] & 7) };
    head("PCI config space");
    pf("%02x:%02x.%u\n", v[0], v[1] & 31, v[2] & 7);
    for (int off = 0; off < 256; off += 16) {
        pf("%02x:", off);
        for (int i = 0; i < 16; i += 4) pf(" %08x", pcix_read32(l, (uint8_t)(off + i)));
        out("\n");
    }
}

// ---- GPU -------------------------------------------------------------------------------------
void hwinfo_gpu(void) {
    head("Graphics");
    i915_probe(0, 0, 0);
    // the first display controller, whoever made it
    struct pci_loc loc;
    for (int n = 0; pcix_find_class(0x03, 0xFF, n, &loc); n++) {
        uint32_t id = pcix_read32(loc, 0);
        pf("display controller %02x:%02x.%u  %04x:%04x  %s   subsystem %08x\n", loc.bus, loc.dev, loc.fn, id & 0xFFFF, id >> 16,
           vendor_name((uint16_t)id), pcix_read32(loc, 0x2C));
        for (int b = 0; b < 6; b++) {
            int is_mem, above;
            uint32_t bar = pcix_bar(loc, b, &is_mem, &above);
            if (bar) pf("  BAR%u %08x  %s  %u KB%s\n", (uint32_t)b, bar, is_mem ? "mem" : "io ", pcix_bar_size(loc, b) >> 10, above ? "  (64-bit)" : "");
        }
    }
    i915_report(gpu_line, 0);
}

void hwinfo_all(void) {
    hwinfo_cpu();
    hwinfo_memmap();
    hwinfo_acpi();
    hwinfo_pci(1);
    hwinfo_pci(2);
    hwinfo_gpu();
}
