#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// USB 3 host controller (xHCI) + a HID boot-protocol mouse. Polled: no interrupts. Written from the xHCI 1.2,
// USB 2.0 and HID 1.11 specifications.
//
// xhci_init() runs at boot BEFORE the first process exists (the register window comes from paging_map_mmio);
// it takes the controller from the firmware, resets it, enumerates what is plugged in and starts the mouse.
// Boot with "nousb" on the multiboot command line to skip all of it. xhci_poll() runs from the main loop:
// it moves the pointer (cursor.c: mouse_inject), notices hot-plug and re-enumerates.
void xhci_init(uint32_t mboot_phys);
void xhci_poll(void);

// One line of status for the `usb` shell command.
void xhci_status(char* buf, int cap);

#endif
