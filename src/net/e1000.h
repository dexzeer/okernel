#ifndef E1000_H
#define E1000_H

#include <stdint.h>

// Initialize the e1000 NIC
void e1000_init(void);

// Send an Ethernet frame
int e1000_send(uint8_t* data, uint32_t len);

// Poll for received packets
void e1000_poll(void);

// Get MAC address
uint8_t* e1000_get_mac(void);

// Register callback for received packets
typedef void (*e1000_rx_callback_t)(uint8_t* data, uint32_t len);
void e1000_set_rx_callback(e1000_rx_callback_t cb);

#endif
