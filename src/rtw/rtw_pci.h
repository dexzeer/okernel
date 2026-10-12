#ifndef RTW_PCI_H
#define RTW_PCI_H

#include <stdint.h>

// PCIe side of the Realtek RTL8821C driver (the laptop's built-in RTL8821CE): registers are memory mapped (BAR 2), and frames
// move through DMA rings of buffer descriptors instead of USB endpoints. Polled; the chip's interrupts stay masked.
//
//   rtwpci_init:      boot time (before the first process, the register window comes from paging_map_mmio): find the device,
//                     power state D0, memory space + bus master, map the registers. Fail-safe; 0 = a device was found.
//   rtwpci_alloc:     the rings and receive buffers (first start).
//   rtwpci_hci_setup: hand the ring addresses to the chip and reset all indexes (before every power-on and after the firmware
//                     download, as the vendor driver does).
//   rtwpci_tx:        one packet (48-byte TX descriptor + payload) on the queue the descriptor's queue select names.
//   rtwpci_rx_get/done: next received buffer (valid until done), then give it back to the chip.
int rtwpci_init(void);
int rtwpci_present(void);
uint16_t rtwpci_device_id(void);
uint32_t rtwpci_rd(uint32_t addr, int len);
void rtwpci_wr(uint32_t addr, uint32_t v, int len);
int rtwpci_alloc(void);
void rtwpci_hci_setup(void);
void rtwpci_phy_cfg(void);
int rtwpci_tx(int qsel, const uint8_t* pk, int total);
int rtwpci_rx_get(const uint8_t** buf);
void rtwpci_rx_done(void);
void rtwpci_status(char* buf, int cap);

#endif
