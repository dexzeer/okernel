#ifndef I2CHID_H
#define I2CHID_H

#include <stdint.h>

// I2C-HID touchpad: finds the touchpad in the ACPI tables (slave address, speed, controller path), hunts for the
// DesignWare I2C controller registers in memory (they are ACPI-mode devices with no PCI function and a base address
// the AML fills in at run time), brings the bus up, speaks HID over I2C and feeds the pointer (cursor.c mouse_inject).
// Everything is on-screen in `touchpad`. Boot with "noi2c" on the multiboot command line to skip it.
//
// i2chid_init() runs at boot, only reads the ACPI tables, the memory map and the PCI BARs (what the hunt must not touch)
// and does anything only when the AML describes an I2C-HID device. The hunt for the controller itself runs from
// i2chid_poll() (main loop, 100 Hz): 3 s after boot, ~0.5 ms per tick, with the block being read named at the top left
// of the screen (so a bus that stalls tells where).
void i2chid_init(uint32_t mboot_phys);
void i2chid_poll(void);
void i2chid_status(char* buf, int cap);
// `touchpad aml`: the controller's AML (_CRS/_PS0/_STA bytes, memory/PCI-config OperationRegions), logged when the hunt failed.
void i2chid_aml(char* buf, int cap);
// `touchpad retry`: run the bring-up again on the controller blocks found at boot (with the LPSS reset/clock kick).
void i2chid_retry(void);
// `touchpad rescan`: start the controller hunt again from the beginning.
void i2chid_rescan(void);
// `touchpad off` / `touchpad on`: stop or resume feeding the pointer (a touchpad that misbehaves can be silenced).
void i2chid_enable(int on);

#endif
