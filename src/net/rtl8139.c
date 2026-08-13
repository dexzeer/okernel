#include "rtl8139.h"
#include "pci.h"
#include "../io.h"
#include "../memory.h"
#include "../serial.h"

// Register offsets
#define IDR0        0x00
#define TxStatus0   0x10
#define TxAddr0     0x20
#define RxBuf       0x30
#define Command     0x37
#define Capr        0x38
#define IntrMask    0x3C
#define IntrStatus  0x3E
#define RxConfig    0x44
#define TxConfig    0x40

#define CMD_RESET       0x10
#define CMD_RX_ENABLE    0x08
#define CMD_TX_ENABLE    0x04
#define CMD_RX_EMPTY     0x01

#define RX_CONFIG_WRAP   0x80
#define RX_CONFIG_APM    0x10
#define RX_CONFIG_AB     0x08

#define ISR_RX_OK    0x01
#define ISR_TX_OK    0x04
#define ISR_RX_ERR   0x02
#define ISR_TX_ERR   0x08

#define TX_BUFFER_SIZE 2048
#define RX_BUFFER_SIZE (8192 + 16 + 15872)

static uint16_t io_base = 0;
static uint8_t mac_addr[6];
static uint8_t* rx_buffer = 0;
static uint32_t rx_offset = 0;
static uint8_t* tx_buffers[4];
static int tx_current = 0;
static rtl8139_rx_callback_t rx_callback = 0;

static uint32_t pci_r32(uint8_t b, uint8_t d, uint8_t f, uint8_t o) {
    outl(0xCF8, (1<<31)|((uint32_t)b<<16)|((uint32_t)d<<11)|((uint32_t)f<<8)|(o&0xFC));
    return inl(0xCFC);
}
static void pci_w32(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v) {
    outl(0xCF8, (1<<31)|((uint32_t)b<<16)|((uint32_t)d<<11)|((uint32_t)f<<8)|(o&0xFC));
    outl(0xCFC, v);
}

static void rtl8139_reset(void) {
    outb(io_base + Command, CMD_RESET);
    for (volatile int i = 0; i < 1000000; i++) {
        if (!(inb(io_base + Command) & CMD_RESET)) break;
    }
}

void rtl8139_init(void) {
    // Direct serial write for debug
    outb(0x3F8, 'A'); while (!(inb(0x3F8+5) & 0x20));
    outb(0x3F8, '\n'); while (!(inb(0x3F8+5) & 0x20));

    struct pci_device devs[32];
    int count = pci_scan(devs, 32);

    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id == 0x10EC && devs[i].device_id == 0x8139) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        serial_puts("[rtl8139] NOT FOUND\n");
        return;
    }

    serial_puts("[rtl8139] found\n");

    // Enable bus mastering
    uint32_t cmd = pci_r32(devs[idx].bus, devs[idx].device, devs[idx].function, 0x04);
    pci_w32(devs[idx].bus, devs[idx].device, devs[idx].function, 0x04, cmd | 0x04);

    // I/O base
    io_base = devs[idx].bar0 & 0xFFFFFFFC;
    serial_puts("[rtl8139] io_base OK\n");

    // Reset
    serial_puts("[rtl8139] resetting...\n");
    rtl8139_reset();
    serial_puts("[rtl8139] reset OK\n");

    // MAC
    for (int i = 0; i < 6; i++) mac_addr[i] = inb(io_base + IDR0 + i);
    serial_puts("[rtl8139] MAC OK\n");

    // Allocate buffers
    rx_buffer = (uint8_t*)kmalloc(RX_BUFFER_SIZE);
    for (int i = 0; i < 4; i++) tx_buffers[i] = (uint8_t*)kmalloc(TX_BUFFER_SIZE);
    serial_puts("[rtl8139] alloc OK\n");

    // Configure
    outw(io_base + RxConfig, RX_CONFIG_WRAP | RX_CONFIG_APM | RX_CONFIG_AB);
    outl(io_base + RxBuf, (uint32_t)rx_buffer);
    outl(io_base + TxConfig, 0x03000000);
    outb(io_base + Command, CMD_RX_ENABLE | CMD_TX_ENABLE);
    // DON'T enable NIC interrupts — use polling instead (no IRQ handler registered)
    serial_puts("[rtl8139] configured OK\n");

    serial_puts("[rtl8139] init DONE\n");
}

int rtl8139_send(uint8_t* data, uint32_t len) {
    if (len > TX_BUFFER_SIZE) return -1;
    int slot = tx_current;
    tx_current = (tx_current + 1) % 4;
    for (uint32_t i = 0; i < len; i++) tx_buffers[slot][i] = data[i];
    outl(io_base + TxAddr0 + slot * 4, (uint32_t)tx_buffers[slot]);
    outl(io_base + TxStatus0 + slot * 4, len);
    return 0;
}

void rtl8139_poll(void) {
    if (!io_base) return;
    uint16_t isr = inw(io_base + IntrStatus);
    outw(io_base + IntrStatus, isr);
    if (isr & ISR_RX_OK) {
        while (!(inb(io_base + Command) & CMD_RX_EMPTY)) {
            uint16_t pkt_len = *(uint16_t*)(rx_buffer + rx_offset + 2);
            pkt_len &= 0x3FFF;
            if (pkt_len > 0 && rx_callback) {
                rx_callback(rx_buffer + rx_offset + 4, pkt_len - 4);
            }
            rx_offset = (rx_offset + pkt_len + 4 + 3) & ~3;
            rx_offset %= (RX_BUFFER_SIZE - 16);
            outw(io_base + Capr, rx_offset - 16);
        }
    }
}

uint8_t* rtl8139_get_mac(void) { return mac_addr; }
void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb) { rx_callback = cb; }
