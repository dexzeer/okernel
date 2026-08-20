#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

struct pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint32_t bar0;
    uint32_t bar1;
    uint32_t bar2;
    uint32_t bar3;
    uint32_t bar4;
    uint32_t bar5;
    uint16_t interrupt_line;
};

// Read a 32-bit value from PCI config space
uint32_t pci_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

// Write a 32-bit value to PCI config space
void pci_write(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

// Scan PCI bus for devices, returns count found
int pci_scan(struct pci_device* devices, int max_devices);

// Find device by vendor/device ID, returns index or -1
int pci_find_device(uint16_t vendor_id, uint16_t device_id);

#endif
