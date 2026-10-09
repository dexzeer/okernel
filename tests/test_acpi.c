// Host test for the ACPI MADT parser (src/acpi.c, the part with no hardware
// access). Crafted tables cover every record type, the 64-bit address
// override, duplicates, caps, and malformed input (zero-length records,
// records past the end, bad checksums, truncated tables) plus a fuzz pass
// under ASan/UBSan: the parser must never read outside the buffer it is given.
#define ACPI_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/acpi.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t buf[4096];
static uint32_t len;

static void put32(uint32_t off, uint32_t v) { for (int i = 0; i < 4; i++) buf[off + i] = (uint8_t)(v >> (8 * i)); }

static void begin(uint32_t lapic, uint32_t flags) {
    memset(buf, 0, sizeof buf);
    memcpy(buf, "APIC", 4);
    buf[8] = 4;                              // revision
    memcpy(buf + 10, "OKTEST", 6);
    put32(36, lapic);
    put32(40, flags);
    len = 44;
}
static void rec_cpu(uint8_t id, uint32_t flags) {
    buf[len] = 0; buf[len + 1] = 8; buf[len + 2] = id; buf[len + 3] = id; put32(len + 4, flags); len += 8;
}
static void rec_x2(uint32_t id, uint32_t flags) {
    buf[len] = 9; buf[len + 1] = 16; put32(len + 4, id); put32(len + 8, flags); put32(len + 12, id); len += 16;
}
static void rec_ioapic(uint8_t id, uint32_t addr, uint32_t gsi) {
    buf[len] = 1; buf[len + 1] = 12; buf[len + 2] = id; put32(len + 4, addr); put32(len + 8, gsi); len += 12;
}
static void rec_override(uint8_t src, uint32_t gsi, uint16_t fl) {
    buf[len] = 2; buf[len + 1] = 10; buf[len + 2] = 0; buf[len + 3] = src; put32(len + 4, gsi);
    buf[len + 8] = (uint8_t)fl; buf[len + 9] = (uint8_t)(fl >> 8); len += 10;
}
static void rec_lapic64(uint32_t lo, uint32_t hi) {
    buf[len] = 5; buf[len + 1] = 12; put32(len + 4, lo); put32(len + 8, hi); len += 12;
}
static void seal(void) {                     // length + checksum
    put32(4, len);
    buf[9] = 0;
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + buf[i]);
    buf[9] = (uint8_t)(0 - s);
}

int main(void) {
    struct acpi_info a;

    // 1. a normal 4-CPU machine
    memset(&a, 0, sizeof a);
    begin(0xFEE00000, 1);
    rec_cpu(0, 1); rec_cpu(1, 1); rec_cpu(2, 1); rec_cpu(3, 0);       // cpu 3 disabled
    rec_ioapic(4, 0xFEC00000, 0);
    rec_override(0, 2, 0); rec_override(9, 9, 0x000F);
    seal();
    CHECK(acpi_parse_madt(buf, len, &a), "parse");
    CHECK(a.ncpu == 4, "ncpu %d", a.ncpu);
    CHECK(a.cpu[2].apic_id == 2 && a.cpu[2].enabled == 1 && a.cpu[3].enabled == 0, "cpu flags");
    CHECK(a.nioapic == 1 && a.ioapic[0].addr == 0xFEC00000 && a.ioapic[0].id == 4 && a.ioapic[0].gsi_base == 0, "ioapic");
    CHECK(a.noverride == 2 && a.over[0].src == 0 && a.over[0].gsi == 2 && a.over[1].src == 9 && a.over[1].flags == 0x0F, "overrides");
    CHECK(a.lapic_addr == 0xFEE00000 && a.pcat_compat == 1 && a.madt == 1, "lapic/pcat");

    // 2. x2APIC records, and a CPU listed twice
    memset(&a, 0, sizeof a);
    begin(0xFEE00000, 0);
    rec_cpu(0, 1); rec_x2(0x100, 1); rec_x2(0x101, 3); rec_cpu(0, 1);
    seal();
    CHECK(acpi_parse_madt(buf, len, &a) && a.ncpu == 3, "x2apic ncpu %d", a.ncpu);
    CHECK(a.cpu[1].apic_id == 0x100 && a.cpu[2].apic_id == 0x101 && a.pcat_compat == 0, "x2apic ids");

    // 3. 64-bit local APIC address override (low 32 bits used when the high word is 0)
    memset(&a, 0, sizeof a);
    begin(0xFEE00000, 1); rec_lapic64(0xFEE01000, 0); seal();
    CHECK(acpi_parse_madt(buf, len, &a) && a.lapic_addr == 0xFEE01000, "override %x", a.lapic_addr);
    memset(&a, 0, sizeof a);
    begin(0xFEE00000, 1); rec_lapic64(0xFEE01000, 1); seal();         // above 4GB: unreachable, keep the 32-bit one
    CHECK(acpi_parse_madt(buf, len, &a) && a.lapic_addr == 0xFEE00000, "override above 4G ignored");

    // 4. caps: more CPUs than we track
    memset(&a, 0, sizeof a);
    begin(0xFEE00000, 1);
    for (int i = 0; i < 100; i++) rec_cpu((uint8_t)i, 1);
    seal();
    CHECK(acpi_parse_madt(buf, len, &a) && a.ncpu == SMP_MAX_CPUS, "cpu cap %d", a.ncpu);
    memset(&a, 0, sizeof a);
    begin(0xFEE00000, 1);
    for (int i = 0; i < 40; i++) rec_ioapic((uint8_t)i, 0xFEC00000 + i * 0x1000, i * 24);
    for (int i = 0; i < 40; i++) rec_override((uint8_t)i, i, 0);
    seal();
    CHECK(acpi_parse_madt(buf, len, &a) && a.nioapic == ACPI_MAX_IOAPIC && a.noverride == ACPI_MAX_OVERRIDE, "ioapic/override caps");

    // 5. malformed: bad checksum, bad signature, truncated, zero-length record, record past the end
    begin(0xFEE00000, 1); rec_cpu(0, 1); seal();
    buf[9] ^= 1;
    memset(&a, 0, sizeof a);
    CHECK(!acpi_parse_madt(buf, len, &a), "bad checksum accepted");
    begin(0xFEE00000, 1); rec_cpu(0, 1); seal(); buf[0] = 'X';
    CHECK(!acpi_parse_madt(buf, len, &a), "bad signature accepted");
    begin(0xFEE00000, 1); rec_cpu(0, 1); seal();
    CHECK(!acpi_parse_madt(buf, len - 1, &a), "truncated buffer accepted");
    CHECK(!acpi_parse_madt(buf, 20, &a), "tiny buffer accepted");
    begin(0xFEE00000, 1); rec_cpu(0, 1); buf[len] = 0; buf[len + 1] = 0; len += 2; rec_cpu(1, 1); seal();   // zero-length record
    memset(&a, 0, sizeof a);
    CHECK(acpi_parse_madt(buf, len, &a) && a.ncpu == 1, "zero-length record: ncpu %d", a.ncpu);
    begin(0xFEE00000, 1); rec_cpu(0, 1); buf[len] = 0; buf[len + 1] = 200; len += 8; seal();               // record claims to run past the table
    memset(&a, 0, sizeof a);
    CHECK(acpi_parse_madt(buf, len, &a) && a.ncpu == 1, "overlong record: ncpu %d", a.ncpu);
    begin(0xFEE00000, 1); rec_cpu(0, 1); buf[len] = 0; buf[len + 1] = 8; len += 4; seal();                 // CPU record cut short by the table end
    memset(&a, 0, sizeof a);
    CHECK(acpi_parse_madt(buf, len, &a) && a.ncpu == 1, "cut-short record: ncpu %d", a.ncpu);

    // 6. checksum helper
    uint8_t z[4] = { 1, 2, 3, 250 };
    CHECK(acpi_checksum_ok(z, 4), "checksum helper");
    z[3] = 251;
    CHECK(!acpi_checksum_ok(z, 4), "checksum helper (bad)");

    // 7. fuzz: random bodies behind a valid header; exactly-sized heap buffers so
    //    ASan catches any read past the end.
    srand(12345);
    for (int it = 0; it < 200000; it++) {
        uint32_t n = 44 + (uint32_t)(rand() % 200);
        uint8_t* t = (uint8_t*)malloc(n);
        for (uint32_t i = 0; i < n; i++) t[i] = (uint8_t)rand();
        if (rand() & 3) memcpy(t, "APIC", 4);
        t[4] = (uint8_t)n; t[5] = (uint8_t)(n >> 8); t[6] = t[7] = 0;
        t[9] = 0;
        uint8_t s = 0;
        for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + t[i]);
        t[9] = (uint8_t)(0 - s);
        memset(&a, 0, sizeof a);
        acpi_parse_madt(t, n, &a);
        if (a.ncpu < 0 || a.ncpu > SMP_MAX_CPUS || a.nioapic > ACPI_MAX_IOAPIC || a.noverride > ACPI_MAX_OVERRIDE) {
            fails++; printf("FAIL fuzz counts out of range\n");
        }
        free(t);
    }

    printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
