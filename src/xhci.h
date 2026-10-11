#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// USB 3 host controller (xHCI) + USB pointing devices (any HID mouse, trackball or tablet, directly or behind USB 2 hubs). Polled: no interrupts. Written from the xHCI 1.2,
// USB 2.0 and HID 1.11 specifications.
//
// xhci_init() runs at boot BEFORE the first process exists (the register window comes from paging_map_mmio);
// it takes the controller from the firmware, resets it, enumerates what is plugged in and starts the mouse.
// Boot with "nousb" on the multiboot command line to skip all of it. xhci_poll() runs from the main loop:
// it moves the pointer (cursor.c: mouse_inject), notices hot-plug and re-enumerates.
void xhci_init(uint32_t mboot_phys);
void xhci_poll(void);

// Recovery self test for the `usb selftest` command (stalls endpoint 0, restarts the pointer endpoints).
void xhci_selftest(char* buf, int cap);
void xhci_selftest_stop(char* buf, int cap);

// Forget the ports that failed or hold a non-pointer device and try them again (`usb rescan`).
void xhci_rescan(void);

// The first USB mass storage device (bulk-only, 512 byte blocks), for the on-stick log (klog.c).
// xhci_storage_ready: 1 when there is one (and it has not failed), *sectors = its size.
// xhci_storage_rw: n <= 128 sectors; 1 on success, 0 on failure (which also switches the storage off).
int xhci_storage_ready(uint32_t* sectors);
int xhci_storage_rw(int write, uint32_t lba, void* data, uint32_t n);

// A USB Wi-Fi adapter (vendor class, matched by rtw_usb_id_match in src/rtw): the xHCI driver configures its bulk
// endpoints and the Wi-Fi driver talks through these calls. All synchronous except the receive side, which keeps
// several 32 KB buffers in flight and hands completed ones out in order.
struct xhci_wl_info { uint16_t vid, pid; int speed, nout; };
int xhci_wl_present(struct xhci_wl_info* info);
// Vendor control transfer on endpoint 0 (len <= 512). `in`: device to host. Returns the bytes transferred or < 0.
int xhci_wl_ctrl(int in, uint8_t req, uint16_t value, uint16_t index, void* buf, uint16_t len);
// Bulk OUT on the nth OUT endpoint (a zero-length packet follows when the length is a multiple of the packet size).
// len <= 65536. Returns 0 on success.
int xhci_wl_bulk_out(int ep, const void* data, uint32_t len);
// Next completed bulk IN buffer: returns its length (0 = none yet) and its address in *data. Call xhci_wl_rx_done()
// when finished with it. xhci_wl_rx_start() queues the buffers (once, after the chip is ready to send).
void xhci_wl_rx_start(void);
int xhci_wl_rx_get(const uint8_t** data);
void xhci_wl_rx_done(void);
// Drive the controller's event ring (rx completions) without waiting for xhci_poll.
void xhci_wl_pump(void);

// One line of status for the `usb` shell command.
void xhci_status(char* buf, int cap);

#endif
