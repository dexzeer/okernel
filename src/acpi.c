#include "acpi.h"

// ACPI table discovery. The pure parsers at the top have no hardware access
// (tests/acpi builds them on the host against crafted tables); the part under
// `#ifndef ACPI_HOST` walks real memory.

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int acpi_checksum_ok(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

// MADT: SDT header (36 bytes), local APIC address, flags, then a list of
// {type, length, ...} records. A malformed record (length < 2 or running past
// the table) ends the walk instead of looping or reading out of bounds.
int acpi_parse_madt(const uint8_t* t, uint32_t len, struct acpi_info* out) {
    if (len < 44) return 0;
    if (t[0] != 'A' || t[1] != 'P' || t[2] != 'I' || t[3] != 'C') return 0;
    uint32_t tl = rd32(t + 4);
    if (tl < 44 || tl > len) return 0;
    if (!acpi_checksum_ok(t, tl)) return 0;

    out->lapic_addr = rd32(t + 36);
    out->pcat_compat = (rd32(t + 40) & 1) != 0;
    out->ncpu = out->nioapic = out->noverride = 0;

    uint32_t off = 44;
    while (off + 2 <= tl) {
        uint8_t type = t[off], rl = t[off + 1];
        if (rl < 2 || off + rl > tl) break;
        const uint8_t* r = t + off;
        if (type == 0 && rl >= 8) {                       // processor local APIC
            uint32_t fl = rd32(r + 4);
            uint32_t id = r[3];
            int dup = 0;
            for (int i = 0; i < out->ncpu; i++) if (out->cpu[i].apic_id == id) dup = 1;
            if (!dup && out->ncpu < SMP_MAX_CPUS) {
                out->cpu[out->ncpu].apic_id = id;
                out->cpu[out->ncpu].enabled = (fl & 1) ? 1 : 0;
                out->ncpu++;
            }
        } else if (type == 9 && rl >= 16) {               // processor local x2APIC
            uint32_t id = rd32(r + 4), fl = rd32(r + 8);
            int dup = 0;
            for (int i = 0; i < out->ncpu; i++) if (out->cpu[i].apic_id == id) dup = 1;
            if (!dup && out->ncpu < SMP_MAX_CPUS) {
                out->cpu[out->ncpu].apic_id = id;
                out->cpu[out->ncpu].enabled = (fl & 1) ? 1 : 0;
                out->ncpu++;
            }
        } else if (type == 1 && rl >= 12) {               // I/O APIC
            if (out->nioapic < ACPI_MAX_IOAPIC) {
                out->ioapic[out->nioapic].id = r[2];
                out->ioapic[out->nioapic].addr = rd32(r + 4);
                out->ioapic[out->nioapic].gsi_base = rd32(r + 8);
                out->nioapic++;
            }
        } else if (type == 2 && rl >= 10) {               // interrupt source override
            if (out->noverride < ACPI_MAX_OVERRIDE) {
                out->over[out->noverride].src = r[3];
                out->over[out->noverride].gsi = rd32(r + 4);
                out->over[out->noverride].flags = rd16(r + 8);
                out->noverride++;
            }
        } else if (type == 5 && rl >= 12) {               // 64-bit local APIC address override
            uint32_t lo = rd32(r + 4), hi = rd32(r + 8);
            if (hi == 0 && lo) out->lapic_addr = lo;      // above 4GB is unreachable for us
        }
        off += rl;
    }
    out->madt = 1;
    return 1;
}

#ifndef ACPI_HOST

#include "memlayout.h"
#include "memory.h"
#include "paging.h"
#include "serial.h"

static struct acpi_info info;
static struct acpi_pm pm_info;

const struct acpi_pm* acpi_get_pm(void) { return &pm_info; }

const struct acpi_info* acpi_get(void) { return &info; }

// RAM below 128MB is always in the kernel's high alias; anything above comes
// through the 4MB MMIO windows (one window per call, so reads are chunked).
int acpi_phys_read(uint32_t phys, void* dst, uint32_t len) {
    uint8_t* d = (uint8_t*)dst;
    while (len) {
        if (phys + len < phys) return 0;
        uint32_t chunk = len;
        const uint8_t* src;
        if (phys < 0x08000000u) {
            if (chunk > 0x08000000u - phys) chunk = 0x08000000u - phys;
            src = (const uint8_t*)P2V_U32(phys);
        } else {
            uint32_t room = 0x00400000u - (phys & 0x003FFFFFu);
            if (chunk > room) chunk = room;
            src = (const uint8_t*)paging_map(phys);
            if (!src) return 0;
        }
        for (uint32_t i = 0; i < chunk; i++) d[i] = src[i];
        d += chunk; phys += chunk; len -= chunk;
    }
    return 1;
}

static int rsdp_valid(const uint8_t* r) {
    if (r[0] != 'R' || r[1] != 'S' || r[2] != 'D' || r[3] != ' ' ||
        r[4] != 'P' || r[5] != 'T' || r[6] != 'R' || r[7] != ' ') return 0;
    if (!acpi_checksum_ok(r, 20)) return 0;
    if (r[15] >= 2 && !acpi_checksum_ok(r, 36)) return 0;      // extended checksum (ACPI 2.0+)
    return 1;
}

// Search [base, base+len) on 16-byte boundaries. Returns the RSDP's phys or 0.
static uint32_t scan_for_rsdp(uint32_t base, uint32_t len) {
    uint8_t buf[512];
    for (uint32_t o = 0; o < len; o += sizeof(buf) - 32) {
        uint32_t n = len - o;
        if (n > sizeof(buf)) n = sizeof(buf);
        if (n < 36) break;
        if (!acpi_phys_read(base + o, buf, n)) return 0;
        for (uint32_t i = 0; i + 36 <= n; i += 16) {
            if (rsdp_valid(buf + i)) return base + o + i;
        }
    }
    return 0;
}

static uint32_t find_rsdp(uint32_t mboot_phys) {
    uint32_t r;
    // 1. EBDA (BIOS data area word 0x40E is its segment), first KB.
    uint8_t w[2];
    if (acpi_phys_read(0x40E, w, 2)) {
        uint32_t ebda = (uint32_t)rd16(w) << 4;
        if (ebda >= 0x70000 && ebda < 0xA0000 && (r = scan_for_rsdp(ebda, 1024))) return r;
    }
    // 2. The BIOS read-only area.
    if ((r = scan_for_rsdp(0xE0000, 0x20000))) return r;
    // 3. UEFI firmware keeps the RSDP in ACPI memory: search the multiboot
    //    memory map's ACPI-reclaimable (3) and NVS (4) regions.
    uint8_t mi[56];
    if (!acpi_phys_read(mboot_phys, mi, sizeof(mi))) return 0;
    if (!(rd32(mi) & (1u << 6))) return 0;
    uint32_t mlen = rd32(mi + 44), maddr = rd32(mi + 48);
    uint32_t scanned = 0;
    for (uint32_t o = 0; o + 24 <= mlen && o < 4096; ) {
        uint8_t e[28];
        if (!acpi_phys_read(maddr + o, e, 28)) break;
        uint32_t esz = rd32(e);
        uint32_t base = rd32(e + 4), baseh = rd32(e + 8);
        uint32_t rl = rd32(e + 12), rlh = rd32(e + 16);
        uint32_t type = rd32(e + 20);
        if (esz < 20) break;
        if ((type == 3 || type == 4) && baseh == 0 && rlh == 0 && rl) {
            if (rl > 0x100000) rl = 0x100000;
            if (scanned + rl <= 0x800000) {
                scanned += rl;
                if ((r = scan_for_rsdp(base, rl))) return r;
            }
        }
        o += esz + 4;
    }
    return 0;
}

// Read a whole table (header first for its length). Caller kfrees.
static uint8_t* read_table(uint32_t phys, const char* want, uint32_t* lenp) {
    uint8_t h[36];
    if (!phys || !acpi_phys_read(phys, h, 36)) return 0;
    if (want && (h[0] != want[0] || h[1] != want[1] || h[2] != want[2] || h[3] != want[3])) return 0;
    uint32_t len = rd32(h + 4);
    if (len < 36 || len > 0x100000) return 0;
    uint8_t* t = (uint8_t*)kmalloc(len);
    if (!t) return 0;
    if (!acpi_phys_read(phys, t, len) || !acpi_checksum_ok(t, len)) { kfree(t); return 0; }
    if (lenp) *lenp = len;
    return t;
}

void acpi_init(uint32_t mboot_phys) {
    info.found = 0; info.madt = 0;
    uint32_t rp = find_rsdp(mboot_phys);
    if (!rp) { serial_puts("[acpi] no RSDP found\n"); return; }
    uint8_t rd[36];
    if (!acpi_phys_read(rp, rd, 36)) return;
    info.rsdp_phys = rp;
    info.revision = rd[15];

    uint32_t root = 0, esz = 4;
    if (info.revision >= 2 && rd32(rd + 28) == 0 && rd32(rd + 24)) {   // XSDT below 4GB
        root = rd32(rd + 24); esz = 8;
    }
    uint32_t rlen = 0;
    uint8_t* rt = root ? read_table(root, "XSDT", &rlen) : 0;
    if (!rt) { root = rd32(rd + 16); esz = 4; rt = read_table(root, "RSDT", &rlen); }
    if (!rt) { serial_printf("[acpi] RSDP at %x but no valid RSDT/XSDT\n", rp); return; }
    info.found = 1;

    for (uint32_t o = 36; o + esz <= rlen; o += esz) {
        uint32_t addr = rd32(rt + o);
        if (esz == 8 && rd32(rt + o + 4) != 0) continue;           // above 4GB
        uint32_t tl = 0;
        uint8_t* t = read_table(addr, "APIC", &tl);
        if (!t) {
            t = read_table(addr, "FACP", &tl);
            if (t) {
                if (acpi_parse_fadt(t, tl, &pm_info)) {
                    uint32_t dl = 0;
                    uint8_t* d = pm_info.dsdt ? read_table(pm_info.dsdt, "DSDT", &dl) : 0;
                    if (d) { pm_info.have_s5 = acpi_find_s5(d, dl, &pm_info.slp_typa, &pm_info.slp_typb); kfree(d); }
                    serial_printf("[acpi] power: PM1a %x PM1b %x smi %x/%x%s sleep_ctl %x S5 %s (%x,%x) reset %s\n",
                                  pm_info.pm1a_cnt, pm_info.pm1b_cnt, pm_info.smi_cmd, pm_info.acpi_enable,
                                  pm_info.hw_reduced ? " HW-reduced" : "", pm_info.sleep_ctl_port,
                                  pm_info.have_s5 ? "found" : "MISSING", pm_info.slp_typa, pm_info.slp_typb,
                                  pm_info.reset_ok ? "reg" : "none");
                }
                kfree(t);
            }
            continue;
        }
        if (!info.madt) acpi_parse_madt(t, tl, &info);
        kfree(t);
    }
    kfree(rt);

    int enabled = 0;
    for (int i = 0; i < info.ncpu; i++) if (info.cpu[i].enabled) enabled++;
    serial_printf("[acpi] RSDP %x rev %d: %d cpus (%d enabled), lapic %x, %d ioapic, %d override%s\n",
                  rp, info.revision, info.ncpu, enabled, info.lapic_addr,
                  info.nioapic, info.noverride, info.madt ? "" : " (no MADT)");
}

// Copies of the DSDT and every SSDT (AML), for drivers that look up device descriptions. Caller kfrees each table.
int acpi_aml_tables(uint8_t** out, uint32_t* lens, int max) {
    int n = 0;
    if (!info.found || max <= 0) return 0;
    uint8_t rd[36];
    if (!acpi_phys_read(info.rsdp_phys, rd, 36)) return 0;
    uint32_t root = 0, esz = 4, rlen = 0;
    if (info.revision >= 2 && rd32(rd + 28) == 0 && rd32(rd + 24)) { root = rd32(rd + 24); esz = 8; }
    uint8_t* rt = root ? read_table(root, "XSDT", &rlen) : 0;
    if (!rt) { root = rd32(rd + 16); esz = 4; rt = read_table(root, "RSDT", &rlen); }
    if (!rt) return 0;
    if (pm_info.dsdt) {
        uint32_t l = 0;
        uint8_t* d = read_table(pm_info.dsdt, "DSDT", &l);
        if (d) { out[n] = d; lens[n] = l; n++; }
    }
    for (uint32_t o = 36; o + esz <= rlen && n < max; o += esz) {
        uint32_t addr = rd32(rt + o);
        if (esz == 8 && rd32(rt + o + 4) != 0) continue;
        uint32_t l = 0;
        uint8_t* t = read_table(addr, "SSDT", &l);
        if (t) { out[n] = t; lens[n] = l; n++; }
    }
    kfree(rt);
    return n;
}

#endif
