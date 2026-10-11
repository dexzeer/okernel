#ifndef DWI2C_H
#define DWI2C_H

#include <stdint.h>

// Synopsys DesignWare I2C master (the controller inside Intel's LPSS block on Bay/Cherry Trail and many other SoCs),
// polled, 7-bit addressing. Written from the DW_apb_i2c register map. The register access is abstracted so the host
// tests can run the driver against a software model of the controller.
struct dwi2c {
    uint32_t (*rd)(void* ctx, uint32_t off);
    void (*wr)(void* ctx, uint32_t off, uint32_t v);
    void (*delay_us)(uint32_t us);
    void* ctx;
    int rx_depth, tx_depth;
    int addr;                      // current IC_TAR (-1 = unset)
    int fast;
    uint32_t last_abort;           // IC_TX_ABRT_SOURCE of the last failed transfer
};

#define DW_COMP_TYPE 0x44570140u

// 1 when the registers look like a DesignWare I2C block (component type register).
int dw_present(struct dwi2c* d);

// Program the master: standard mode (about 100 kHz at a 100 MHz bus clock) or fast mode (about 400 kHz). Returns 0 or
// -1 when the controller does not switch off (stuck).
int dw_setup(struct dwi2c* d, int fast);

// Write `wl` bytes, then (repeated start) read `rl` bytes. Either may be 0 but not both. Returns 0, -1 timeout,
// -2 no acknowledge / arbitration lost (d->last_abort has the source bits).
int dw_xfer(struct dwi2c* d, int addr, const uint8_t* w, int wl, uint8_t* r, int rl);

#endif
