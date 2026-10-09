#ifndef PCIX_H
#define PCIX_H

#include <stdint.h>

// PCI helpers for drivers (config space access by bus/device/function, device
// lookup, capability list, BAR decoding). The legacy scan in net/pci.c stays
// as it is; this is what the GPU drivers use.

struct pci_loc { uint8_t bus, dev, fn; };

uint8_t  pcix_read8(struct pci_loc l, uint8_t off);
uint16_t pcix_read16(struct pci_loc l, uint8_t off);
uint32_t pcix_read32(struct pci_loc l, uint8_t off);
void     pcix_write16(struct pci_loc l, uint8_t off, uint16_t v);
void     pcix_write32(struct pci_loc l, uint8_t off, uint32_t v);

// First device matching vendor/device id (device_id 0xFFFF = any), or class
// (class 0xFF = any class; prog_if ignored). Returns 1 and fills *out.
int pcix_find_id(uint16_t vendor, uint16_t device, int nth, struct pci_loc* out);
int pcix_find_class(uint8_t cls, uint8_t subclass, int nth, struct pci_loc* out);

// Command register: memory space + bus master on, legacy INTx off.
void pcix_enable(struct pci_loc l, int intx_off);

// Walk the capability list: first capability with id `id` at or after the
// config offset `after` (0 = from the start). Returns its config offset or 0.
uint8_t pcix_find_cap(struct pci_loc l, uint8_t id, uint8_t after);

// Physical base of BAR `idx` (64-bit BARs are combined; *is64 and *mem say what
// it was). Returns 0 when the BAR is unassigned or an I/O BAR; sets *hi_nonzero
// when the address does not fit in 32 bits.
uint32_t pcix_bar(struct pci_loc l, int idx, int* is_mem, int* above4g);

// Size of BAR `idx` in bytes (probes by writing all ones, restores the value).
uint32_t pcix_bar_size(struct pci_loc l, int idx);

#endif
