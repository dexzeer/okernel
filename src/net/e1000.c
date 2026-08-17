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
#define E1000_RDBAL     0x02800 // Receive Descriptor Base Address Low
#define E1000_RDBAH     0x02804 // Receive Descriptor Base Address High
#define E1000_RDLEN     0x02808 // Receive Descriptor Length
#define E1000_RDH       0x02810 // Receive Descriptor Head (16-bit)
#define E1000_RDT       0x02818 // Receive Descriptor Tail (16-bit)

// Transmit registers
#define E1000_TCTL      0x0400  // Transmit Control
#define E1000_TDBAL     0x03800 // Transmit Descriptor Base Address Low
#define E1000_TDBAH     0x03804 // Transmit Descriptor Base Address High
#define E1000_TDLEN     0x03808 // Transmit Descriptor Length
#define E1000_TDH       0x03810 // Transmit Descriptor Head (16-bit)
#define E1000_TDT       0x03818 // Transmit Descriptor Tail (16-bit)

// RCTL bits
#define RCTL_EN         0x00000002  // Receiver Enable
#define RCTL_BAM        0x00000004  // Broadcast Accept Mode
#define RCTL_SECRC      0x00000008  // Strip Ethernet CRC
#define RCTL_BSEX       0x02000000  // Buffer Size Extension

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

// Fixed low memory addresses for DMA (below 1MB, identity-mapped)
// These MUST be below 1MB for NIC DMA to work
#define E1000_RX_DESCS_ADDR  0x80000  // 512KB - RX descriptors
#define E1000_RX_BUFS_ADDR   0x80200  // 512KB + 512B - RX buffers (32 * 2048 = 64KB)
#define E1000_TX_DESCS_ADDR  0x90000  // 576KB - TX descriptors
#define E1000_TX_BUFS_ADDR   0x90100  // 576KB + 128B - TX buffers (8 * 2048 = 16KB)

// Pointers to low memory buffers
static struct e1000_rx_desc* rx_descs = (struct e1000_rx_desc*)E1000_RX_DESCS_ADDR;
static uint8_t* rx_buffers = (uint8_t*)E1000_RX_BUFS_ADDR;
static struct e1000_tx_desc* tx_descs = (struct e1000_tx_desc*)E1000_TX_DESCS_ADDR;
static uint8_t* tx_buffers = (uint8_t*)E1000_TX_BUFS_ADDR;

static uint8_t rx_cur = 0;
static uint8_t tx_cur = 0;
static e1000_rx_callback_t rx_callback = 0;

static const char hex[] = "0123456789abcdef";

static void print_hex32(uint32_t val) {
    serial_putchar(hex[(val >> 28) & 0xF]);
    serial_putchar(hex[(val >> 24) & 0xF]);
    serial_putchar(hex[(val >> 20) & 0xF]);
    serial_putchar(hex[(val >> 16) & 0xF]);
    serial_putchar(hex[(val >> 12) & 0xF]);
    serial_putchar(hex[(val >> 8) & 0xF]);
    serial_putchar(hex[(val >> 4) & 0xF]);
    serial_putchar(hex[val & 0xF]);
}

static void mmio_write(uint32_t offset, uint32_t value) {
    mmio[offset / 4] = value;
}

static void mmio_write16(uint32_t offset, uint16_t value) {
    uint32_t addr = (uint32_t)&mmio[offset / 4];
    *(volatile uint16_t*)addr = value;
}

static uint32_t mmio_read(uint32_t offset) {
    return mmio[offset / 4];
}

static uint16_t mmio_read16(uint32_t offset) {
    uint32_t addr = (uint32_t)&mmio[offset / 4];
    return *(volatile uint16_t*)addr;
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
        serial_puts("[e1000_irq] RX ICR=");
        print_hex32(icr);
        serial_puts(" RDH=");
        serial_putchar(hex[mmio_read16(E1000_RDH) & 0xF]);
        serial_puts(" desc0_st=");
        serial_putchar(hex[rx_descs[0].status]);
        serial_putchar('\n');
    }
    if (icr & 0x04) {
        serial_puts("[e1000_irq] TX\n");
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
    print_hex32(bar0);
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
    print_hex32(ctrl);
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

    // Clear RX descriptors in low memory
    for (int i = 0; i < NUM_RX_DESCRIPTORS * sizeof(struct e1000_rx_desc); i++) {
        ((uint8_t*)rx_descs)[i] = 0;
    }

    // Set up RX descriptors at low memory address
    uint32_t rx_phys = E1000_RX_DESCS_ADDR;
    serial_puts("[e1000] rx_descs addr=");
    print_hex32(rx_phys);
    serial_putchar('\n');

    mmio_write(E1000_RDBAL, rx_phys);
    mmio_write(E1000_RDBAH, 0);
    mmio_write(E1000_RDLEN, NUM_RX_DESCRIPTORS * 16);
    mmio_write16(E1000_RDH, 0);
    mmio_write16(E1000_RDT, NUM_RX_DESCRIPTORS - 1);

    // Set up RX buffer addresses in low memory
    for (int i = 0; i < NUM_RX_DESCRIPTORS; i++) {
        uint32_t buf_addr = E1000_RX_BUFS_ADDR + (i * RX_BUFFER_SIZE);
        rx_descs[i].addr = buf_addr;
        rx_descs[i].length = 0;
        rx_descs[i].status = 0;

        serial_puts("[e1000] rx_buf[");
        serial_putchar('0' + i);
        serial_puts("] addr=");
        print_hex32(buf_addr);
        serial_putchar('\n');
    }

    // Clear TX descriptors in low memory
    for (int i = 0; i < NUM_TX_DESCRIPTORS * sizeof(struct e1000_tx_desc); i++) {
        ((uint8_t*)tx_descs)[i] = 0;
    }

    // Set up TX descriptors at low memory address
    uint32_t tx_phys = E1000_TX_DESCS_ADDR;
    serial_puts("[e1000] tx_descs addr=");
    print_hex32(tx_phys);
    serial_putchar('\n');

    mmio_write(E1000_TDBAL, tx_phys);
    mmio_write(E1000_TDBAH, 0);
    mmio_write(E1000_TDLEN, NUM_TX_DESCRIPTORS * 16);
    mmio_write16(E1000_TDH, 0);
    mmio_write16(E1000_TDT, 0);

    // Set up TX buffer addresses in low memory
    for (int i = 0; i < NUM_TX_DESCRIPTORS; i++) {
        uint32_t buf_addr = E1000_TX_BUFS_ADDR + (i * 2048);
        tx_descs[i].addr = buf_addr;
        tx_descs[i].status = TXD_STAT_DD; // Mark as ready
    }

    // Enable RX (accept broadcast + MAC filter)
    mmio_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    // Enable TX
    mmio_write(E1000_TCTL, TCTL_EN | TCTL_PSP);

    // Enable interrupts (RX + RX Timer + RX Desc Minimum)
    mmio_write(E1000_ICR, 0xFFFFFFFF);
    mmio_write(E1000_IMS, 0x01 | 0x40 | 0x10); // RX + RXDTYP0 + RXDMT0

    // Check device status
    uint32_t status = mmio_read(E1000_STATUS);
    serial_puts("[e1000] Status=");
    print_hex32(status);
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

    // Copy data to TX buffer in low memory
    for (uint32_t i = 0; i < len; i++) {
        tx_buffers[tx_cur * 2048 + i] = data[i];
    }

    // Set up TX descriptor
    tx_descs[tx_cur].addr = E1000_TX_BUFS_ADDR + (tx_cur * 2048);
    tx_descs[tx_cur].length = len;
    tx_descs[tx_cur].cmd = 0x01 | 0x08; // EOP + IFCS
    tx_descs[tx_cur].status = 0;

    tx_cur = (tx_cur + 1) % NUM_TX_DESCRIPTORS;
    mmio_write16(E1000_TDT, tx_cur);

    serial_puts("[e1000_tx] sent ");
    serial_putchar(hex[(len >> 8) & 0xF]);
    serial_putchar(hex[(len >> 4) & 0xF]);
    serial_putchar(hex[len & 0xF]);
    serial_puts(" bytes\n");
    return 0;
}

static int poll_count = 0;
static int last_debug = 0;

void e1000_poll(void) {
    if (!mmio) return;

    poll_count++;

    // Debug: print status every 500 polls
    if (poll_count - last_debug >= 500) {
        last_debug = poll_count;
        uint32_t status = mmio_read(E1000_STATUS);
        uint32_t rctl = mmio_read(E1000_RCTL);
        uint16_t rdh = mmio_read16(E1000_RDH);
        uint16_t rdt = mmio_read16(E1000_RDT);
        serial_puts("[poll] St=");
        print_hex32(status);
        serial_puts(" RCTL=");
        print_hex32(rctl);
        serial_puts(" RDH=");
        serial_putchar(hex[rdh & 0xF]);
        serial_puts(" RDT=");
        serial_putchar(hex[rdt & 0xF]);
        serial_puts(" desc_st=");
        serial_putchar(hex[rx_descs[rx_cur].status]);
        serial_putchar('\n');
    }

    // Check for received packets
    while (rx_descs[rx_cur].status & RXD_STAT_DD) {
        uint16_t len = rx_descs[rx_cur].length;

        serial_puts("[e1000_rx] pkt len=");
        serial_putchar(hex[(len >> 8) & 0xF]);
        serial_putchar(hex[(len >> 4) & 0xF]);
        serial_putchar(hex[len & 0xF]);
        serial_puts(" cur=");
        serial_putchar(hex[rx_cur]);
        serial_putchar('\n');

        if (len > 4 && len < RX_BUFFER_SIZE && rx_callback) {
            // Point to the buffer in low memory
            uint8_t* pkt = (uint8_t*)(E1000_RX_BUFS_ADDR + (rx_cur * RX_BUFFER_SIZE));
            rx_callback(pkt, len - 4);
        }

        rx_descs[rx_cur].status = 0;
        rx_descs[rx_cur].length = 0;
        // Release THIS descriptor back to hardware via the TAIL. RDT must
        // point AT the last posted descriptor (the one just processed).
        // Writing RDT = rx_cur+1 instead makes RDT == RDH, which reads as
        // "no free descriptors" — hardware then drops every packet (the
        // old code wrote RDH, leaving RDT frozen so the ring exhausted
        // after ~15 packets; both variants stalled fetches mid-response).
        mmio_write16(E1000_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NUM_RX_DESCRIPTORS;
    }
}

uint8_t* e1000_get_mac(void) { return mac_addr; }

void e1000_set_rx_callback(e1000_rx_callback_t cb) { rx_callback = cb; }
