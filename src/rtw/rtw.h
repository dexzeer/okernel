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

#endif
