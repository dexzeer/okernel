#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>

// Initialize the RTL8139 NIC
void rtl8139_init(void);

// Send an Ethernet frame
int rtl8139_send(uint8_t* data, uint32_t len);

// Poll for received packets (call in main loop)
void rtl8139_poll(void);

// Get MAC address
uint8_t* rtl8139_get_mac(void);

// Register callback for received packets
typedef void (*rtl8139_rx_callback_t)(uint8_t* data, uint32_t len);
void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb);

#endif
