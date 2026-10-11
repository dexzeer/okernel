#ifndef I2CAML_H
#define I2CAML_H

#include <stdint.h>

// Finds I2C-HID devices (touchpads, touchscreens) in AML by pattern scanning, without an interpreter: a Device() scope
// with a known _HID/_CID, an I2cSerialBusV2 resource descriptor (slave address, speed, controller path) and the
// HID-over-I2C _DSM (whose Return constants are candidates for the HID descriptor register). Pure code, host-tested.
struct i2c_hid_dev {
    char hid[12];                  // _HID string
    int addr;                      // 7-bit slave address
    uint32_t speed;                // Hz
    char ctrl[40];                 // controller path, e.g. "\_SB.PCI0.I2C5"
    uint16_t reg[8];               // HID descriptor register candidates from the _DSM
    int nreg;
};

int i2caml_find(const uint8_t* t, uint32_t len, struct i2c_hid_dev* out, int max);

#endif
