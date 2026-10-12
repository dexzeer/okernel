#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

// A log that survives the machine: when the boot USB stick carries the marker the flash tool puts in its last 2 MB
// (see tools/stick/), every devlog line, a status snapshot every 5 s (`usb`, `touchpad`) and the raw ACPI tables are
// written there, and tools/stick/readlog.ps1 reads them back on the PC. No photos, no serial port needed.
// A stick without the marker is never written to.
//
// Layout of the last 4096 sectors (512 B each) of the stick, start S = capacity - 4096:
//   S+0         header (see struct klog_hdr)
//   S+1..S+256  log of an even boot, S+257..S+512 log of an odd boot (text)
//   S+513..S+576 status snapshot (text, rewritten every 5 s)
//   S+577..S+3583 raw ACPI tables (directory in the header)
//   S+3584..S+3839 / S+3840..S+4095  the serial output (serial.c ring: the last 128 KB of an even / odd boot, a plain ring;
//                    the header's ser_total says how many bytes were written, the oldest kept byte is at ser_total % 128 KB)
#define KLOG_REGION_SECTORS 4096
#define KLOG_LOG_SECTORS 256
#define KLOG_SNAP_SECTORS 64
#define KLOG_TAB_FIRST 577
#define KLOG_SER_FIRST 3584
#define KLOG_SER_SECTORS 256
#define KLOG_MAGIC "KANARCHY-LOG-V1"

struct klog_tab { char sig[4]; uint32_t sect; uint32_t len; };       // len in bytes; sect relative to S
struct klog_hdr {
    char magic[16];
    uint32_t boot;                                                   // incremented at every boot that finds the marker
    uint32_t ntab;
    struct klog_tab tab[16];
    uint32_t scan_window;                                            // the 4 MB window the touchpad hunt was reading (0 = none)
    uint32_t ser_total[2];                                           // serial bytes written by the even / odd boot
    uint32_t ser_boot[2];                                            // the boot counter that wrote them
    uint32_t pad[69];
};

void klog_write(char tag, const char* line);   // called by devlog() for every line
void klog_poll(void);                          // main loop: finds the stick, flushes, snapshots
void klog_flush(void);                         // write what is pending now (the touchpad hunt does this before each memory window)
int klog_active(void);
void klog_set_scan_window(uint32_t w);         // persistent: which window is being read (cleared with 0)

#endif
