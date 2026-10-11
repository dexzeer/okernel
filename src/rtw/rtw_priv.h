#ifndef RTW_PRIV_H
#define RTW_PRIV_H

#include <stdint.h>
#include "../devlog.h"

#define wlog(...) devlog('w', __VA_ARGS__)

// the adapter's state
struct rtw {
    int state;                              // 0 off, 2 running
    uint16_t vid, pid;
    int nout, speed;
    uint32_t chip_ver;
    uint8_t cut, rfe, pkg, xtal;
    uint8_t bb_swing_2g, bb_swing_5g;
    uint8_t pwr_cck[6], pwr_2g40[5], pwr_5g[14];
    int8_t ofdm_diff_2g, ofdm_diff_5g;
    uint8_t mac[6];
    uint16_t rsvd_boundary, rsvd_fw_txbuf, rsvd_h2cq;
    uint32_t rcr;
    uint32_t ch_param[3];
    uint32_t h2c_seq, h2c_box;
    uint8_t channel;
    int tx_idx;
    uint32_t io_err;
    uint32_t rx_bufs, rx_frames, rx_bad, rx_crc, c2h_count;
};
extern struct rtw R;

// register access
uint8_t rtw_r8(uint32_t a);
uint16_t rtw_r16(uint32_t a);
uint32_t rtw_r32(uint32_t a);
void rtw_w8(uint32_t a, uint8_t v);
void rtw_w16(uint32_t a, uint16_t v);
void rtw_w32(uint32_t a, uint32_t v);
void rtw_w8_set(uint32_t a, uint8_t m);
void rtw_w8_clr(uint32_t a, uint8_t m);
void rtw_w16_set(uint32_t a, uint16_t m);
void rtw_w32_set(uint32_t a, uint32_t m);
void rtw_w32_clr(uint32_t a, uint32_t m);
void rtw_w32_mask(uint32_t a, uint32_t mask, uint32_t v);
uint32_t rtw_r32_mask(uint32_t a, uint32_t mask);
uint32_t rtw_rf_read(uint32_t reg, uint32_t mask);
void rtw_rf_write(uint32_t reg, uint32_t mask, uint32_t data);
void rtw_udelay(uint32_t us);
void rtw_mdelay(uint32_t ms);

int rtw_tx_raw(int qsel, const uint32_t w[10], const uint8_t* payload, int len);
int rtw_h2c_pkt(uint8_t* pkt32);
void rtw_h2c_mbox(const uint8_t* h2c8);
int rtw_set_channel(int ch);
void rtw_rx_poll(void);

int rtw_tx_frame(const uint8_t* f, int len, int is_data);
void rtw_set_link(int up, const uint8_t* bssid, int aid);
void rtw_iqk(void);

// implemented in rtw_wifi.c
void rtw_rx_frame(const uint8_t* f, int len, int rate, int rssi, int icv_err);
void rtw_c2h(const uint8_t* p, int len);
void rtw_send_fw_info(void);

#endif
