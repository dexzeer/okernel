// HID over I2C protocol (see hidi2c.h).
#include "hidi2c.h"

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int hidi2c_get_desc(struct hidi2c* h, uint16_t reg) {
    uint8_t w[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) }, d[30];
    int r = h->bus.xfer(h->bus.ctx, h->addr, w, 2, d, 30);
    if (r < 0) return r;
    if (le16(d) != 30 || le16(d + 2) != 0x0100) return 1;
    uint16_t rdl = le16(d + 4), rdr = le16(d + 6), inr = le16(d + 8), maxin = le16(d + 10);
    if (rdl < 8 || rdl > 4096 || !rdr || !inr || maxin < 3 || maxin > HIDI2C_MAXREPORT + 2) return 1;
    h->desc_reg = reg;
    h->bcd = le16(d + 2); h->rdesc_len = rdl; h->rdesc_reg = rdr; h->in_reg = inr; h->max_in = maxin;
    h->out_reg = le16(d + 12); h->max_out = le16(d + 14); h->cmd_reg = le16(d + 16); h->data_reg = le16(d + 18);
    h->vid = le16(d + 20); h->pid = le16(d + 22); h->ver = le16(d + 24);
    return 0;
}

int hidi2c_power(struct hidi2c* h, int on) {
    uint8_t w[4] = { (uint8_t)h->cmd_reg, (uint8_t)(h->cmd_reg >> 8), (uint8_t)(on ? 0 : 1), 0x08 };
    return h->bus.xfer(h->bus.ctx, h->addr, w, 4, 0, 0);
}

int hidi2c_reset(struct hidi2c* h) {
    uint8_t w[4] = { (uint8_t)h->cmd_reg, (uint8_t)(h->cmd_reg >> 8), 0x00, 0x01 };
    int r = h->bus.xfer(h->bus.ctx, h->addr, w, 4, 0, 0);
    if (r < 0) return r;
    for (int i = 0; i < 60; i++) {                            // up to about 600 ms
        h->bus.delay_ms(10);
        uint8_t in[2];
        if (h->bus.xfer(h->bus.ctx, h->addr, 0, 0, in, 2) == 0 && in[0] == 0 && in[1] == 0) return 0;
    }
    return 1;
}

int hidi2c_report_desc(struct hidi2c* h, uint8_t* buf, int cap) {
    if (h->rdesc_len > cap) return -4;
    uint8_t w[2] = { (uint8_t)h->rdesc_reg, (uint8_t)(h->rdesc_reg >> 8) };
    int r = h->bus.xfer(h->bus.ctx, h->addr, w, 2, buf, h->rdesc_len);
    return r < 0 ? r : (int)h->rdesc_len;
}

int hidi2c_input(struct hidi2c* h, uint8_t* buf, int cap) {
    uint8_t in[HIDI2C_MAXREPORT + 2];
    int want = h->max_in;
    if (want > (int)sizeof in) want = (int)sizeof in;
    // A plain read returns what the device last addressed (the real SYNA3602 kept answering with its HID descriptor after a
    // descriptor read) until a command is written, which points it back at the input register: so anything that is not
    // a report is answered with SET_POWER(on) and counted. (An explicit write of the input register address was tried and
    // the device never produced a report that way.)
    int r = h->bus.xfer(h->bus.ctx, h->addr, 0, 0, in, want);
    if (r == -2) { h->nack++; return 0; }                     // a device with nothing to say may not answer at all
    if (r < 0) return r;
    int len = le16(in);
    if (len == 0) { h->zeros++; return 0; }
    if (len == 0xFFFF) { h->ones++; return 0; }
    if (len < 3 || len > want || (len == 30 && le16(in + 2) == 0x0100)) {          // garbage, or the descriptor again
        for (int i = 0; i < 16 && i < want; i++) h->bad[i] = in[i];
        h->stale++;
        hidi2c_power(h, 1);
        return -5;
    }
    len -= 2;
    if (len > cap) len = cap;
    for (int i = 0; i < len; i++) buf[i] = in[2 + i];
    return len;
}

int hidi2c_set_feature(struct hidi2c* h, int id, const uint8_t* data, int len) {
    uint8_t w[HIDI2C_MAXREPORT + 16];
    if (len < 0 || len > HIDI2C_MAXREPORT) return -4;
    int n = 0;
    w[n++] = (uint8_t)h->cmd_reg; w[n++] = (uint8_t)(h->cmd_reg >> 8);
    w[n++] = (uint8_t)(0x30 | (id >= 15 ? 15 : id));              // report type 3 (feature) + id
    w[n++] = 0x03;                                                // SET_REPORT
    if (id >= 15) w[n++] = (uint8_t)id;
    w[n++] = (uint8_t)h->data_reg; w[n++] = (uint8_t)(h->data_reg >> 8);
    int size = 2 + (id ? 1 : 0) + len;                            // the length counts its own two bytes
    w[n++] = (uint8_t)size; w[n++] = (uint8_t)(size >> 8);
    if (id) w[n++] = (uint8_t)id;
    for (int i = 0; i < len; i++) w[n++] = data[i];
    return h->bus.xfer(h->bus.ctx, h->addr, w, n, 0, 0);
}
