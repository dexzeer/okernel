#ifndef CRASH_H
#define CRASH_H

#include <stdint.h>

// Crash records in the log partition of an installed disk (part.h), written by the panic screen (panic.c) and read by
// the recovery system. Sectors relative to the partition start:
//   0..4095      the klog region (klog.h: boot logs, status snapshot, serial ring of the last two boots)
//   4096         struct crash_hdr
//   4097 + 128*i slot i (CRASH_SLOTS of them, a ring): sector 0 struct crash_rec, then the report text
//                (CRASH_TEXT_SECTORS), then the serial tail before the panic (CRASH_SER_SECTORS)
#define CRASH_FIRST 4096
#define CRASH_SLOTS 8
#define CRASH_SLOT_SECTORS 128
#define CRASH_TEXT_SECTORS 31
#define CRASH_SER_SECTORS 96
#define CRASH_PART_SECTORS (CRASH_FIRST + 1 + CRASH_SLOTS * CRASH_SLOT_SECTORS)
#define CRASH_MAGIC "KANARCHY-CRASH1"

struct crash_hdr {
    char magic[16];
    uint32_t count;                     // records ever written; the newest is in slot (count - 1) % CRASH_SLOTS
    uint32_t pad[123];
};

struct crash_rec {
    char magic[16];
    uint32_t seq;                       // 1-based record number (crash_hdr.count when it was written)
    uint32_t boot;                      // klog boot counter of the boot that crashed (0 = unknown)
    uint32_t vec, err, eip, cr2, cs, eflags;
    uint32_t uptime_ms;
    uint32_t text_len, ser_len;
    char title[64];
    uint32_t pad[(512 - 16 - 4 * 11 - 64) / 4];
};

#endif
