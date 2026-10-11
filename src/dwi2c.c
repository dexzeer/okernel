// DesignWare I2C master (see dwi2c.h).
#include "dwi2c.h"

#define R_CON 0x00
#define R_TAR 0x04
#define R_DATA 0x10
#define R_SS_HCNT 0x14
#define R_SS_LCNT 0x18
#define R_FS_HCNT 0x1C
#define R_FS_LCNT 0x20
#define R_INTR_MASK 0x30
#define R_RAW_INTR 0x34
#define R_RX_TL 0x38
#define R_TX_TL 0x3C
#define R_CLR_INTR 0x40
#define R_CLR_TX_ABRT 0x54
#define R_ENABLE 0x6C
#define R_STATUS 0x70
#define R_ABRT_SRC 0x80
#define R_ENABLE_STATUS 0x9C
#define R_COMP_PARAM1 0xF4
#define R_COMP_TYPE 0xFC

#define INTR_TX_ABRT (1u << 6)
#define ST_ACTIVITY (1u << 0)
#define ST_TFNF (1u << 1)
#define ST_TFE (1u << 2)
#define ST_RFNE (1u << 3)

#define CMD_READ (1u << 8)
#define CMD_STOP (1u << 9)
#define CMD_RESTART (1u << 10)

int dw_present(struct dwi2c* d) { return d->rd(d->ctx, R_COMP_TYPE) == DW_COMP_TYPE; }

static int wait_enable_status(struct dwi2c* d, int want) {
    for (int i = 0; i < 200; i++) {
        if ((int)(d->rd(d->ctx, R_ENABLE_STATUS) & 1) == want) return 1;
        d->delay_us(50);
    }
    return 0;
}

static int disable(struct dwi2c* d) {
    d->wr(d->ctx, R_ENABLE, 0);
    return wait_enable_status(d, 0);
}

int dw_setup(struct dwi2c* d, int fast) {
    if (!disable(d)) return -1;
    uint32_t p = d->rd(d->ctx, R_COMP_PARAM1);
    d->rx_depth = (int)((p >> 8) & 0xFF) + 1;
    d->tx_depth = (int)((p >> 16) & 0xFF) + 1;
    if (p == 0 || p == 0xFFFFFFFFu || d->rx_depth < 2) { d->rx_depth = 8; d->tx_depth = 8; }
    d->fast = fast;
    // master, speed, restart enabled, slave disabled; counts for a 100 MHz bus clock (standard ~100 kHz, fast ~400 kHz)
    d->wr(d->ctx, R_CON, 0x01u | ((fast ? 2u : 1u) << 1) | 0x20u | 0x40u);
    d->wr(d->ctx, R_SS_HCNT, 0x1F4); d->wr(d->ctx, R_SS_LCNT, 0x1F4);
    d->wr(d->ctx, R_FS_HCNT, 0x55);  d->wr(d->ctx, R_FS_LCNT, 0x99);
    d->wr(d->ctx, R_INTR_MASK, 0);
    d->wr(d->ctx, R_RX_TL, 0);
    d->wr(d->ctx, R_TX_TL, 0);
    d->rd(d->ctx, R_CLR_INTR);
    d->addr = -1;
    return 0;
}

static void drain_rx(struct dwi2c* d) {
    for (int i = 0; i < 256 && (d->rd(d->ctx, R_STATUS) & ST_RFNE); i++) d->rd(d->ctx, R_DATA);
}

int dw_xfer(struct dwi2c* d, int addr, const uint8_t* w, int wl, uint8_t* r, int rl) {
    if (wl < 0 || rl < 0 || wl + rl == 0) return -3;
    if (addr != d->addr) {
        if (!disable(d)) return -1;
        d->wr(d->ctx, R_TAR, (uint32_t)addr & 0x7F);
        d->addr = addr;
    }
    d->wr(d->ctx, R_ENABLE, 1);
    if (!wait_enable_status(d, 1)) return -1;
    d->rd(d->ctx, R_CLR_TX_ABRT);
    d->rd(d->ctx, R_CLR_INTR);
    drain_rx(d);

    int total = wl + rl, txi = 0, rxi = 0;
    int limit = d->rx_depth > 2 ? d->rx_depth - 1 : 1;       // reads in flight: never more than the receive FIFO holds
    int idle = 0;
    for (;;) {
        if (d->rd(d->ctx, R_RAW_INTR) & INTR_TX_ABRT) {
            d->last_abort = d->rd(d->ctx, R_ABRT_SRC);
            d->rd(d->ctx, R_CLR_TX_ABRT);
            d->rd(d->ctx, R_CLR_INTR);
            drain_rx(d);
            return -2;
        }
        int progress = 0;
        uint32_t st = d->rd(d->ctx, R_STATUS);
        if (st == 0xFFFFFFFFu) return -1;                    // powered off
        while (txi < total && (st & ST_TFNF)) {
            uint32_t cmd;
            if (txi < wl) {
                cmd = w[txi];
                if (txi == wl - 1 && rl == 0) cmd |= CMD_STOP;
            } else {
                int outstanding = (txi - wl) - rxi;
                if (outstanding >= limit) break;
                cmd = CMD_READ;
                if (txi == wl && wl > 0) cmd |= CMD_RESTART;
                if (txi == total - 1) cmd |= CMD_STOP;
            }
            d->wr(d->ctx, R_DATA, cmd);
            txi++; progress = 1;
            st = d->rd(d->ctx, R_STATUS);
        }
        while (rxi < rl && (st & ST_RFNE)) {
            r[rxi++] = (uint8_t)d->rd(d->ctx, R_DATA);
            progress = 1;
            st = d->rd(d->ctx, R_STATUS);
        }
        if (rxi == rl && txi == total) break;
        if (!progress) {
            if (++idle > 20000) return -1;                   // 200 ms without a byte moving
            d->delay_us(10);
        } else idle = 0;
    }
    // the transfer is queued: wait for the bus to go idle (the stop condition), then look for a late abort (NACK)
    for (int i = 0; i < 5000; i++) {
        uint32_t st = d->rd(d->ctx, R_STATUS);
        if ((st & ST_TFE) && !(st & ST_ACTIVITY)) break;
        if (d->rd(d->ctx, R_RAW_INTR) & INTR_TX_ABRT) break;
        d->delay_us(10);
    }
    if (d->rd(d->ctx, R_RAW_INTR) & INTR_TX_ABRT) {
        d->last_abort = d->rd(d->ctx, R_ABRT_SRC);
        d->rd(d->ctx, R_CLR_TX_ABRT);
        d->rd(d->ctx, R_CLR_INTR);
        drain_rx(d);
        return -2;
    }
    return 0;
}
