#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include "acpi_pm.h"

// ACPI table discovery (RSDP -> RSDT/XSDT -> MADT). Desktop kernel only.
// We only read what SMP and interrupt routing need: the CPU list (local APIC
// ids), the I/O APICs and the ISA interrupt source overrides.

#define SMP_MAX_CPUS 32
#define ACPI_MAX_IOAPIC 8
#define ACPI_MAX_OVERRIDE 16

struct acpi_cpu {
    uint32_t apic_id;
    uint8_t  enabled;      // MADT flags bit 0 (or bit 1: online-capable)
};

struct acpi_ioapic {
    uint8_t  id;
    uint32_t addr;         // phys MMIO base
    uint32_t gsi_base;
};

struct acpi_override {     // ISA irq `src` is wired to global system interrupt `gsi`
    uint8_t  src;
    uint32_t gsi;
    uint16_t flags;        // MPS INTI flags: polarity (bits 0-1), trigger (bits 2-3)
};

struct acpi_info {
    int      found;        // RSDP + a valid root table were found
    int      madt;         // a valid MADT was parsed
    uint8_t  revision;     // RSDP revision (0 = ACPI 1.0, RSDT only)
    uint32_t lapic_addr;   // phys base of the local APIC (MADT, or the 64-bit override)
    int      pcat_compat;  // dual 8259 present
    int      ncpu;
    struct acpi_cpu cpu[SMP_MAX_CPUS];
    int      nioapic;
    struct acpi_ioapic ioapic[ACPI_MAX_IOAPIC];
    int      noverride;
    struct acpi_override over[ACPI_MAX_OVERRIDE];
    uint32_t rsdp_phys;
};

// Find and parse the tables. mboot_phys lets us search the firmware memory map
// (UEFI keeps the RSDP outside the legacy BIOS area). Call after paging_init.
void acpi_init(uint32_t mboot_phys);
// Power-off / reset facts gathered from the FADT and the DSDT's _S5_ object (valid == 0 when missing).
const struct acpi_pm* acpi_get_pm(void);
const struct acpi_info* acpi_get(void);

// Copy `len` bytes of physical memory (any address below 4GB) into dst.
// Returns 0 when the range cannot be mapped.
int acpi_phys_read(uint32_t phys, void* dst, uint32_t len);

// Copies of the DSDT and the SSDTs (AML) read from physical memory; returns the count. Caller kfrees each table.
int acpi_aml_tables(uint8_t** out, uint32_t* lens, int max);

// Pure parsers (no hardware access), exposed so the host tests can feed them
// crafted tables: validates checksums and fills `out` from a MADT image.
int acpi_checksum_ok(const void* p, uint32_t len);
int acpi_parse_madt(const uint8_t* madt, uint32_t len, struct acpi_info* out);

#endif
