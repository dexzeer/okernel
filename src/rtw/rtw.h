#ifndef RTW_H
#define RTW_H

#include <stdint.h>

// Realtek RTL8821C Wi-Fi (the 8811CU/8821CU USB sticks; the 8821CE on PCIe shares the chip core).
// Written from the chip's behaviour as documented by the rtw88 driver's public sources (register facts, sequences,
// descriptor layouts) - the code is ours. The firmware blob (firmware/rtw8821c_fw.bin) is Realtek's: it runs on the
// chip's own processor, not on ours.
//
//   rtw_usb_id_match: the xHCI driver asks whether a vendor-class device is one of ours.
//   rtw_start:        power the chip up, download the firmware, read the efuse, initialise MAC and radio. 0 = ok.
//   rtw_poll:         main loop; receives frames, drives scanning / connecting.
//   rtw_command:      the `wifi` shell command; output goes into buf.
int rtw_usb_id_match(uint16_t vid, uint16_t pid);
int rtw_start(void);
void rtw_poll(void);
void rtw_command(const char* args, char* buf, int cap);

// For the network stack: the adapter as a network interface.
int rtw_net_up(void);                                   // associated and keyed
const uint8_t* rtw_net_mac(void);
int rtw_net_send(const uint8_t* eth, int len);          // one Ethernet frame; 0 = sent
void rtw_net_set_rx(void (*cb)(const uint8_t* eth, int len));

// ---- for the Wi-Fi window (src/wifigui.c) ----------------------------------------------------------------------------------------------
// One entry per network name (the strongest access point of each); security: 0 open, 1 WEP, 2 WPA, 3 WPA2-Personal, 4 not supported here.
struct rtw_ui_bss { char ssid[33]; uint8_t ch, sec; int8_t rssi; };
enum { RTW_LINK_IDLE, RTW_LINK_BUSY, RTW_LINK_UP, RTW_LINK_FAILED };
struct rtw_ui_link {
    int state;
    char ssid[33];
    char msg[64];                 // progress or the reason for a failure
    char ip[20], gateway[20];
};
void rtw_ui_demo(int on);                              // a simulated adapter and networks (password "demo1234"), for testing the window
int rtw_ui_adapter(void);                              // 0 none, 1 attached but not started, 2 running
void rtw_ui_adapter_name(char* buf, int cap);
int rtw_ui_start(void);                                // power up the adapter (takes several seconds); 0 = ok
void rtw_ui_scan(void);
int rtw_ui_scanning(void);
int rtw_ui_list(struct rtw_ui_bss* out, int max);      // strongest first
void rtw_ui_link_info(struct rtw_ui_link* l);
void rtw_ui_connect(const char* ssid, const char* pass);
void rtw_ui_disconnect(void);

#endif
