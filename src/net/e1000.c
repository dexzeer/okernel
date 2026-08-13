#include "e1000.h"
#include "pci.h"
#include "../io.h"
#include "../memory.h"
#include "../paging.h"
#include "../serial.h"

// e1000 register offsets (MMIO)
#define E1000_CTRL      0x0000  // Device Control
#define E1000_STATUS    0x0008  // Device Status
#define E1000_EECD      0x0010  // EEPROM Control/Data
#define E1000_CTRL_EXT  0x0018  // Extended Device Control

// Interrupt registers
#define E1000_ICR       0x00C0  // Interrupt Cause Read
#define E1000_IMS       0x00D0  // Interrupt Mask Set
#define E1000_IMC       0x00D8  // Interrupt Mask Clear

// Receive registers
#define E1000_RCTL      0x0100  // Receive Control
#define E1000_RDBAL     0x0280  // Receive Descriptor Base Address Low
#define E1000_RDBAH     0x0284  // Receive Descriptor Base Address High
#define E1000_RDLEN     0x0288  // Receive Descriptor Length
#define E1000_RDH       0x0281  // Receive Descriptor Head (16-bit, byte offset 0x280+1)
#define E1000_RDT       0x0282  // Receive Descriptor Tail (16-bit, byte offset 0x280+2)

// Transmit registers
#define E1000_TCTL      0x0400  // Transmit Control
#define E1000_TDBAL     0x0380  // Transmit Descriptor Base Address Low
#define E1000_TDBAH     0x0384  // Transmit Descriptor Base Address High
#define E1000_TDLEN     0x0388  // Transmit Descriptor Length
#define E1000_TDH       0x0381  // Transmit Descriptor Head
#define E1000_TDT       0x0382  // Transmit Descriptor Tail

// RCTL bits
#define RCTL_EN         0x00000002  // Receiver Enable
#define RCTL_BAM        0x00000004  // Broadcast Accept Mode
#define RCTL_SECRC      0x00000008  // Strip Ethernet CRC

// TCTL bits
#define TCTL_EN         0x00000002  // Transmitter Enable
#define TCTL_PSP        0x00000008  // Pad Short Packets

// RX descriptor status
#define RXD_STAT_DD     0x01        // Descriptor Done
#define RXD_STAT_EOP    0x02        // End of Packet

// TX descriptor status
#define TXD_STAT_DD     0x01        // Descriptor Done

// MAC registers
#define E1000_RAL       0x0054      // Receive Address Low
#define E1000_RAH       0x0058      // Receive Address High

#define NUM_RX_DESCRIPTORS 32
#define NUM_TX_DESCRIPTORS 8
#define RX_BUFFER_SIZE 2048

// RX descriptor (16 bytes each)
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

// TX descriptor (16 bytes each)
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint16_t status;
    uint16_t css;
} __attribute__((packed));

static volatile uint32_t* mmio = 0;
static uint8_t mac_addr[6];
// RX buffers at fixed low addresses (below 1MB for DMA)
// RX buffers at fixed low memory (below 1MB for DMA)
static uint8_t rx_buffers_mem[NUM_RX_DESCRIPTORS * RX_BUFFER_SIZE];
static uint8_t* rx_buffers_ptrs[NUM_RX_DESCRIPTORS];
// RX/TX descriptors at fixed low memory
static struct e1000_rx_desc rx_descs_mem[NUM_RX_DESCRIPTORS] __attribute__((aligned(16)));
static struct e1000_tx_desc tx_descs_mem[NUM_TX_DESCRIPTORS] __attribute__((aligned(16)));
static uint8_t tx_buffers_mem[NUM_TX_DESCRIPTORS * 2048];
static uint8_t rx_cur = 0;
static uint8_t tx_cur = 0;
static e1000_rx_callback_t rx_callback = 0;

static const char hex[] = "0123456789abcdef";

static void mmio_write(uint32_t offset, uint32_t value) {
    mmio[offset / 4] = value;
}

static uint32_t mmio_read(uint32_t offset) {
    return mmio[offset / 4];
}

// Read EEPROM (e1000 has onboard EEPROM for MAC)
static uint16_t eeprom_read(uint8_t addr) {
    uint32_t val = 0;
    mmio_write(E1000_EECD, (1 | (uint32_t)addr << 2)); // Start + address
    // Wait for done
    for (volatile int i = 0; i < 10000; i++) {
        val = mmio_read(E1000_EECD);
        if (val & 0x10) break; // Done bit
    }
    return (val >> 16) & 0xFFFF;
}

static void e1000_irq_handler(void) {
    if (!mmio) return;
    uint32_t icr = mmio_read(E1000_ICR);
    if (icr & 0x01) {
        serial_puts("[e1000_irq] RX!\n");
    }
    if (icr & 0x04) {
        serial_puts("[e1000_irq] TX!\n");
    }
}

static void e1000_reset(void) {
    mmio_write(E1000_CTRL, 0x04000000); // Reset
    for (volatile int i = 0; i < 100000; i++); // Wait
}

void e1000_init(void) {
    // Find e1000 on PCI bus
    struct pci_device devs[32];
    int count = pci_scan(devs, 32);
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id == 0x8086 && devs[i].device_id == 0x100E) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        serial_puts("[e1000] not found!\n");
        return;
    }
    serial_puts("[e1000] found\n");

    // Enable bus mastering + memory space
    uint32_t cmd = pci_read(devs[idx].bus, devs[idx].device, devs[idx].function, 0x04);
    cmd |= 0x04 | 0x02;
    pci_write(devs[idx].bus, devs[idx].device, devs[idx].function, 0x04, cmd);

    // Get MMIO base from BAR0
    uint32_t bar0 = devs[idx].bar0;
    serial_puts("[e1000] BAR0=");
    serial_putchar(hex[(bar0 >> 28) & 0xF]);
    serial_putchar(hex[(bar0 >> 24) & 0xF]);
    serial_putchar(hex[(bar0 >> 20) & 0xF]);
    serial_putchar(hex[(bar0 >> 16) & 0xF]);
    serial_putchar(hex[(bar0 >> 12) & 0xF]);
    serial_putchar(hex[(bar0 >> 8) & 0xF]);
    serial_putchar(hex[(bar0 >> 4) & 0xF]);
    serial_putchar(hex[bar0 & 0xF]);
    serial_putchar('\n');

    if (bar0 & 0x01) {
        serial_puts("[e1000] BAR0 is I/O, not MMIO!\n");
        return;
    }
    mmio = (volatile uint32_t*)(bar0 & 0xFFFFFFF0);
    serial_puts("[e1000] MMIO base OK\n");

    // Map MMIO region into virtual address space
    serial_puts("[e1000] mapping MMIO...\n");
    void* mapped = paging_map(bar0 & 0xFFFFFFF0);
    mmio = (volatile uint32_t*)mapped;
    serial_puts("[e1000] MMIO mapped OK\n");

    // Test MMIO access
    uint32_t ctrl = mmio_read(E1000_CTRL);
    serial_puts("[e1000] CTRL=");
    serial_putchar(hex[(ctrl >> 28) & 0xF]);
    serial_putchar(hex[(ctrl >> 24) & 0xF]);
    serial_putchar(hex[(ctrl >> 20) & 0xF]);
    serial_putchar(hex[(ctrl >> 16) & 0xF]);
    serial_putchar(hex[(ctrl >> 12) & 0xF]);
    serial_putchar(hex[(ctrl >> 8) & 0xF]);
    serial_putchar(hex[(ctrl >> 4) & 0xF]);
    serial_putchar(hex[ctrl & 0xF]);
    serial_putchar('\n');

    // Disable interrupts during init
    mmio_write(E1000_IMC, 0xFFFFFFFF);

    // Reset
    serial_puts("[e1000] resetting...\n");
    e1000_reset();
    serial_puts("[e1000] reset OK\n");

    // Read MAC from RAL/RAH registers (after reset, they contain default MAC)
    uint32_t ral = mmio_read(E1000_RAL);
    uint32_t rah = mmio_read(E1000_RAH);
    mac_addr[0] = ral & 0xFF;
    mac_addr[1] = (ral >> 8) & 0xFF;
    mac_addr[2] = (ral >> 16) & 0xFF;
    mac_addr[3] = (ral >> 24) & 0xFF;
    mac_addr[4] = rah & 0xFF;
    mac_addr[5] = (rah >> 8) & 0xFF;
    serial_puts("[e1000] MAC OK\n");

    // Set MAC address in RAL/RAH
    mmio_write(E1000_RAL, mac_addr[0] | (mac_addr[1] << 8) |
                           (mac_addr[2] << 16) | (mac_addr[3] << 24));
    mmio_write(E1000_RAH, mac_addr[4] | (mac_addr[5] << 8) | 0x80000000); // Address Valid bit

    // Set up RX descriptors
    uint32_t rx_phys = (uint32_t)rx_descs_mem;
    serial_puts("[e1000] rx_descs addr=");
    serial_putchar(hex[(rx_phys >> 24) & 0xF]);
    serial_putchar(hex[(rx_phys >> 20) & 0xF]);
    serial_putchar(hex[(rx_phys >> 16) & 0xF]);
    serial_putchar(hex[(rx_phys >> 12) & 0xF]);
    serial_putchar(hex[(rx_phys >> 8) & 0xF]);
    serial_putchar(hex[(rx_phys >> 4) & 0xF]);
    serial_putchar(hex[rx_phys & 0xF]);
    serial_putchar('\n');

    mmio_write(E1000_RDBAL, rx_phys);
    mmio_write(E1000_RDBAH, 0);
    mmio_write(E1000_RDLEN, NUM_RX_DESCRIPTORS * 16);
    mmio_write(E1000_RDH, 0);
    mmio_write(E1000_RDT, NUM_RX_DESCRIPTORS - 1);

    // Set up RX descriptor addresses using contiguous buffer
    for (int i = 0; i < NUM_RX_DESCRIPTORS; i++) {
        rx_buffers_ptrs[i] = rx_buffers_mem + (i * RX_BUFFER_SIZE);
        rx_descs_mem[i].addr = (uint32_t)rx_buffers_ptrs[i];
        rx_descs_mem[i].length = 0;
        rx_descs_mem[i].status = 0;
    }

    // Set up TX descriptors
    uint32_t tx_phys = (uint32_t)tx_descs_mem;
    mmio_write(E1000_TDBAL, tx_phys);
    mmio_write(E1000_TDBAH, 0);
    mmio_write(E1000_TDLEN, NUM_TX_DESCRIPTORS * 16);
    mmio_write(E1000_TDH, 0);
    mmio_write(E1000_TDT, 0);

    // Set up TX descriptor addresses
    for (int i = 0; i < NUM_TX_DESCRIPTORS; i++) {
        tx_descs_mem[i].addr = (uint32_t)tx_buffers_mem[i];
        tx_descs_mem[i].status = TXD_STAT_DD; // Mark as ready
    }

    // Enable RX (accept broadcast + MAC filter)
    mmio_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    // Enable TX
    mmio_write(E1000_TCTL, TCTL_EN | TCTL_PSP);

    // Enable interrupts (RX)
    mmio_write(E1000_ICR, 0xFFFFFFFF);
    mmio_write(E1000_IMS, 0x01);

    // Check device status
    uint32_t status = mmio_read(E1000_STATUS);
    serial_puts("[e1000] Status=");
    serial_putchar(hex[(status >> 28) & 0xF]);
    serial_putchar(hex[(status >> 24) & 0xF]);
    serial_putchar(hex[(status >> 20) & 0xF]);
    serial_putchar(hex[(status >> 16) & 0xF]);
    serial_putchar(hex[(status >> 12) & 0xF]);
    serial_putchar(hex[(status >> 8) & 0xF]);
    serial_putchar(hex[(status >> 4) & 0xF]);
    serial_putchar(hex[status & 0xF]);
    serial_putchar('\n');
    if (status & 0x02) {
        serial_puts("[e1000] Link UP!\n");
    } else {
        serial_puts("[e1000] Link DOWN\n");
    }

    // Register IRQ handler
    if (devs[idx].interrupt_line > 0) {
        irq_register_handler(devs[idx].interrupt_line, e1000_irq_handler);
        serial_puts("[e1000] IRQ registered on line ");
        serial_putchar('0' + devs[idx].interrupt_line);
        serial_putchar('\n');
    }

    serial_puts("[e1000] initialized OK\n");
}

int e1000_send(uint8_t* data, uint32_t len) {
    if (len > 2048) return -1;

    for (uint32_t i = 0; i < len; i++) {
        tx_buffers_mem[tx_cur * 2048 + i] = data[i];
    }

    tx_descs_mem[tx_cur].addr = (uint32_t)tx_buffers_mem[tx_cur];
    tx_descs_mem[tx_cur].length = len;
    tx_descs_mem[tx_cur].cmd = 0x01 | 0x08;
    tx_descs_mem[tx_cur].status = 0;

    tx_cur = (tx_cur + 1) % NUM_TX_DESCRIPTORS;
    mmio_write(E1000_TDT, tx_cur);

    serial_puts("[e1000_tx] sent ");
    serial_putchar(hex[(len >> 8) & 0xF]);
    serial_putchar(hex[(len >> 4) & 0xF]);
    serial_putchar(hex[len & 0xF]);
    serial_puts(" bytes\n");
    return 0;
}

void e1000_poll(void) {
    // Check interrupt cause
    uint32_t icr = mmio_read(E1000_ICR);
    if (icr & 0x01) { // RX
        serial_puts("[e1000] RX interrupt!\n");
    }

    // Check for received packets
    while (rx_descs_mem[rx_cur].status & RXD_STAT_DD) {
        uint16_t len = rx_descs_mem[rx_cur].length;

        serial_puts("[e1000_rx] pkt len=");
        serial_putchar(hex[(len >> 8) & 0xF]);
        serial_putchar(hex[(len >> 4) & 0xF]);
        serial_putchar(hex[len & 0xF]);
        serial_putchar('\n');

        if (len > 4 && len < RX_BUFFER_SIZE && rx_callback) {
            rx_callback(rx_buffers_ptrs[rx_cur], len - 4);
        }

        rx_descs_mem[rx_cur].status = 0;
        rx_descs_mem[rx_cur].length = 0;
        rx_cur = (rx_cur + 1) % NUM_RX_DESCRIPTORS;
        mmio_write(E1000_RDH, rx_cur);
    }
}

uint8_t* e1000_get_mac(void) { return mac_addr; }

void e1000_set_rx_callback(e1000_rx_callback_t cb) { rx_callback = cb; }
