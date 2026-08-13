#include "pci.h"
#include "../io.h"
#include "../serial.h"

uint32_t pci_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t addr = (1 << 31)                    // Enable bit
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)device << 11)
                  | ((uint32_t)function << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t addr = (1 << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)device << 11)
                  | ((uint32_t)function << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

static void pci_read_device(uint8_t bus, uint8_t device, uint8_t function, struct pci_device* dev) {
    dev->bus = bus;
    dev->device = device;
    dev->function = function;

    uint32_t vendor = pci_read(bus, device, function, 0x00);
    dev->vendor_id = vendor & 0xFFFF;
    dev->device_id = (vendor >> 16) & 0xFFFF;

    uint32_t class_reg = pci_read(bus, device, function, 0x08);
    dev->revision = class_reg & 0xFF;
    dev->prog_if = (class_reg >> 8) & 0xFF;
    dev->subclass = (class_reg >> 16) & 0xFF;
    dev->class_code = (class_reg >> 24) & 0xFF;

    dev->bar0 = pci_read(bus, device, function, 0x10);
    dev->bar1 = pci_read(bus, device, function, 0x14);
    dev->bar2 = pci_read(bus, device, function, 0x18);
    dev->bar3 = pci_read(bus, device, function, 0x1C);
    dev->bar4 = pci_read(bus, device, function, 0x20);
    dev->bar5 = pci_read(bus, device, function, 0x24);

    uint32_t int_reg = pci_read(bus, device, function, 0x3C);
    dev->interrupt_line = int_reg & 0xFF;
}

int pci_scan(struct pci_device* devices, int max_devices) {
    int count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint32_t vendor = pci_read(bus, device, 0, 0x00);
            if ((vendor & 0xFFFF) == 0xFFFF) continue; // No device

            // Check all functions
            for (uint8_t function = 0; function < 8; function++) {
                uint32_t v = pci_read(bus, device, function, 0x00);
                if ((v & 0xFFFF) == 0xFFFF) continue;

                if (count < max_devices) {
                    pci_read_device(bus, device, function, &devices[count]);
                    serial_puts("[pci] found: ");
                    // Print vendor:device in hex
                    uint16_t vid = devices[count].vendor_id;
                    uint16_t did = devices[count].device_id;
                    static const char hex[] = "0123456789abcdef";
                    serial_putchar(hex[(vid >> 12) & 0xF]);
                    serial_putchar(hex[(vid >> 8) & 0xF]);
                    serial_putchar(hex[(vid >> 4) & 0xF]);
                    serial_putchar(hex[vid & 0xF]);
                    serial_putchar(':');
                    serial_putchar(hex[(did >> 12) & 0xF]);
                    serial_putchar(hex[(did >> 8) & 0xF]);
                    serial_putchar(hex[(did >> 4) & 0xF]);
                    serial_putchar(hex[did & 0xF]);
                    serial_putchar('\n');
                    count++;
                }

                if (function == 0 && ((vendor >> 16) & 0xFF) == 0) break; // Single function
            }
        }
    }

    serial_puts("[pci] found ");
    serial_putchar('0' + count);
    serial_puts(" devices\n");

    return count;
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    struct pci_device devices[32];
    int count = pci_scan(devices, 32);

    for (int i = 0; i < count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id) {
            return i;
        }
    }
    return -1;
}
