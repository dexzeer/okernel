#ifndef E1000_H
#define E1000_H

#include <stdint.h>

// Initialize the e1000 NIC
void e1000_init(void);

// Send an Ethernet frame
int e1000_send(uint8_t* data, uint32_t len);

// Poll for received packets
void e1000_poll(void);

// Statistics for the once-a-second [net] line (network.c): packets/bytes received and sent, the longest time between two polls (a poll
// that comes late lets the 256-entry receive ring fill up and the card drop packets), the fullest the ring was, and the card's own
// counters of packets it had to throw away (read-and-clear registers: missed = MPC, nobuf = RNBC).
struct e1000_stats { uint32_t rx_pkts, rx_bytes, tx_pkts, tx_bytes, max_batch; uint64_t poll_gap_max; };
void e1000_take_stats(struct e1000_stats* out, uint32_t* hw_missed, uint32_t* hw_nobuf);

// Get MAC address
uint8_t* e1000_get_mac(void);

// 1 when an e1000 was found
int e1000_present(void);

// Register callback for received packets
typedef void (*e1000_rx_callback_t)(uint8_t* data, uint32_t len);
void e1000_set_rx_callback(e1000_rx_callback_t cb);

// Test hook: drop every Nth received packet (0 = disable). Exercises the
// TCP retransmission path under controlled loss.
void net_set_drop_rate(int every_nth);

// Per-packet serial tracing (frames, descriptors, TCP segments). Off by
// default: every serial byte is two emulated port I/Os, and the per-packet
// lines made a page load write hundreds of KB to COM1. `nettrace` toggles.
extern int net_trace;

#endif
