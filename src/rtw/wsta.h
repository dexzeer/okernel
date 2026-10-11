#ifndef WSTA_H
#define WSTA_H

#include <stdint.h>

// 802.11 station: scan list, open-system authentication, association, the WPA2-Personal four-way handshake (CCMP) and the
// encrypted data path. Hardware independent: the driver gives it the frames it hears and the services below, the
// network stack gets Ethernet frames. Written from IEEE 802.11-2020 clauses 9, 11 and 12.
struct wsta_ops {
    int  (*tx)(const uint8_t* frame, int len, int is_data);     // a complete 802.11 frame (no FCS); the driver adds descriptor, rate and sequence
    void (*set_channel)(int ch);
    void (*link)(int up, const uint8_t bssid[6], int aid);       // program the MAC for the BSS (or clear it)
    void (*eth_rx)(const uint8_t* eth, int len);                 // a received Ethernet frame for the IP stack
    uint32_t (*now_ms)(void);
    void (*random)(uint8_t* out, int n);
    void (*log)(const char* fmt, ...);
};

enum { WSTA_IDLE, WSTA_AUTH, WSTA_ASSOC, WSTA_HANDSHAKE, WSTA_UP, WSTA_FAILED };

struct wsta_bss {
    uint8_t used, bssid[6], ch, sec;       // sec: 0 open, 1 WEP / unknown privacy, 2 WPA1, 3 WPA2-PSK (CCMP), 4 unusable (WPA3-only, PMF required, TKIP group...)
    int8_t rssi;
    uint8_t ssid_len;
    char ssid[33];
    uint32_t seen;
    uint8_t rates_5g;                        // channel > 14
};

void wsta_init(const uint8_t mac[6], const struct wsta_ops* ops);
void wsta_reset_scan(void);
int wsta_bss_count(void);
const struct wsta_bss* wsta_bss_at(int i);
// Choose the strongest BSS with this SSID and join it (pass = NULL or "" for an open network). Returns 0 when the attempt starts.
int wsta_connect(const char* ssid, const char* pass);
void wsta_disconnect(void);
void wsta_rx(const uint8_t* frame, int len, int rssi, int channel);
void wsta_poll(void);
// An Ethernet frame from the IP stack. Returns 0 when it went out.
int wsta_send_eth(const uint8_t* eth, int len);
int wsta_state(void);
const char* wsta_state_name(void);
const uint8_t* wsta_bssid(void);
const char* wsta_ssid(void);
void wsta_stats(uint32_t* tx, uint32_t* rx, uint32_t* bad);
const char* wsta_last_error(void);            // why the last attempt failed (NULL = no failure)

#endif
