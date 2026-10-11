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
    char name[5];                  // the device's own 4 character AML name (e.g. "TPD0")
    uint16_t reg[8];               // HID descriptor register candidates from the _DSM
    int nreg;
};

int i2caml_find(const uint8_t* t, uint32_t len, struct i2c_hid_dev* out, int max);

// Where the firmware put the controller's registers (see i2caml.c): `ctrl` is the controller path from i2caml_find
// ("\_SB.PCI0.I2C5"). The AML fills its Memory32Fixed base from a named field of a SystemMemory region; `rd` reads a
// dword of physical memory. bar0 = register block (0 = unassigned), len0 its length, bar1 = the PCI-config mirror whose
// PSAT register (+0x84, the PMCSR power state, bits 1:0 = 0 for D0) the device's _PS0 clears (psat_known = bar1 valid).
// The device's interrupt line (its GpioInt resource): pin number and the last 4 characters of the GPIO controller's path.
int i2caml_gpioint(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* devname, uint32_t* pin, char src[5]);
// A device's first Memory32Fixed base (e.g. a GPIO community's registers); *is_chv = it is an INT33FF (Cherry Trail pin controller).
int i2caml_mem32(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* devname, uint32_t* base, int* is_chv);

typedef int (*i2caml_rd)(uint32_t phys, uint32_t* v);
struct i2c_ctrl_bars { int ok; uint32_t bar0, bar1, len0; int psat_known; };
int i2caml_ctrl_bars(const uint8_t* const* tabs, const uint32_t* lens, int nt, const char* ctrl, i2caml_rd rd, struct i2c_ctrl_bars* out);

#endif
