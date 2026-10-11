#ifndef HIDI2C_H
#define HIDI2C_H

#include <stdint.h>

// HID over I2C (Microsoft's protocol, v1.0): descriptor registers, power and reset commands, polled input reports.
// Pure code over an abstract I2C bus, host-tested against a model device.
struct hidi2c_bus {
    int (*xfer)(void* ctx, int addr, const uint8_t* w, int wl, uint8_t* r, int rl);   // 0 ok, < 0 failure (NACK = -2)
    void (*delay_ms)(uint32_t ms);
    void* ctx;
};

struct hidi2c {
    struct hidi2c_bus bus;
    int addr;
    uint16_t desc_reg;
    uint16_t bcd, rdesc_len, rdesc_reg, in_reg, max_in, out_reg, max_out, cmd_reg, data_reg, vid, pid, ver;
    uint8_t bad[16];                // the first bytes of the last input read that was not a report (diagnostics)
    uint32_t nack, zeros, ones, stale;   // input reads: NACKed, length 0, length 0xFFFF, answered with something that was not a report
};

#define HIDI2C_MAXREPORT 1024

// Reads the 30-byte HID descriptor from register `reg` and checks it (length 30, version 1.00, sane sizes). 0 = valid.
int hidi2c_get_desc(struct hidi2c* h, uint16_t reg);
// SET_POWER on / sleep.
int hidi2c_power(struct hidi2c* h, int on);
// RESET and wait for the device's zero-length answer. 0 = acknowledged, 1 = no answer (carry on), < 0 = bus error.
int hidi2c_reset(struct hidi2c* h);
// The report descriptor (returns its length, or < 0).
int hidi2c_report_desc(struct hidi2c* h, uint8_t* buf, int cap);
// SET_REPORT (feature). `id` = report id (0 = none), `data` = the report without the id byte. Returns 0 or a bus error.
int hidi2c_set_feature(struct hidi2c* h, int id, const uint8_t* data, int len);
// Poll the input register. Returns the report length (report id first), 0 = nothing pending, < 0 = error.
int hidi2c_input(struct hidi2c* h, uint8_t* buf, int cap);

#endif
