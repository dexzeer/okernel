// Host test for the touchpad stack: PTP descriptor parsing, gestures, the DesignWare I2C driver against a software model
// of the controller, HID over I2C against a model device, and the AML device finder. ASan + UBSan in the Makefile.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hid.h"
#include "../src/ptp.h"
#include "../src/dwi2c.h"
#include "../src/hidi2c.h"
#include "../src/i2caml.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// ---- a Windows Precision Touchpad descriptor (2 fingers per report, report id 4; Input Mode feature id 3) ----
#define FINGER \
    0x05,0x0D, 0x09,0x22, 0xA1,0x02, \
      0x15,0x00, 0x25,0x01, 0x09,0x42, 0x75,0x01, 0x95,0x01, 0x81,0x02, \
      0x75,0x01, 0x95,0x01, 0x81,0x03, \
      0x75,0x04, 0x95,0x01, 0x25,0x0F, 0x09,0x51, 0x81,0x02, \
      0x75,0x02, 0x95,0x01, 0x81,0x03, \
      0x05,0x01, 0x15,0x00, 0x26,0xE8,0x03, 0x75,0x10, 0x95,0x01, 0x09,0x30, 0x81,0x02, \
      0x26,0x58,0x02, 0x09,0x31, 0x81,0x02, \
    0xC0
static const uint8_t ptp_desc[] = {
    0x05,0x0D, 0x09,0x05, 0xA1,0x01, 0x85,0x04,
    FINGER, FINGER,
    0x05,0x0D, 0x27,0xFF,0xFF,0x00,0x00, 0x75,0x10, 0x95,0x01, 0x09,0x56, 0x81,0x02,
    0x09,0x54, 0x25,0x7F, 0x95,0x01, 0x75,0x08, 0x81,0x02,
    0x05,0x09, 0x09,0x01, 0x25,0x01, 0x75,0x01, 0x95,0x01, 0x81,0x02,
    0x95,0x07, 0x81,0x03,
    0x85,0x03, 0x05,0x0D, 0x09,0x0E, 0xA1,0x02, 0x09,0x52, 0x15,0x00, 0x25,0x0A, 0x75,0x08, 0x95,0x01, 0xB1,0x02, 0xC0,
    0xC0,
    // a mouse collection (what such a device sends before it is switched to touchpad mode)
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x01, 0x09,0x01, 0xA1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x02, 0x15,0x00, 0x25,0x01,
    0x95,0x02, 0x75,0x01, 0x81,0x02, 0x95,0x06, 0x81,0x03, 0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08,
    0x95,0x02, 0x81,0x06, 0xC0, 0xC0 };

static void set_bits(uint8_t* r, int off, int bits, uint32_t v) {
    for (int i = 0; i < bits; i++) if ((v >> i) & 1) r[(off + i) >> 3] |= (uint8_t)(1u << ((off + i) & 7));
}

static void test_ptp_parse(void) {
    struct hid_touch t; int ids = 0;
    CHECK(hid_parse_touch(ptp_desc, sizeof ptp_desc, &t, &ids) == 1);
    CHECK(ids == 1); CHECK(t.id == 4); CHECK(t.nf == 2); CHECK(t.bytes == 15);
    CHECK(t.f[0].tip.off == 0 && t.f[0].tip.bits == 1);
    CHECK(t.f[0].id.off == 2 && t.f[0].id.bits == 4);
    CHECK(t.f[0].x.off == 8 && t.f[0].x.bits == 16 && t.f[0].x.lmax == 1000);
    CHECK(t.f[0].y.off == 24 && t.f[0].y.lmax == 600);
    CHECK(t.f[1].tip.off == 40 && t.f[1].x.off == 48 && t.f[1].y.off == 64);
    CHECK(t.count.off == 96 && t.count.bits == 8);
    CHECK(t.button.off == 104 && t.button.bits == 1);
    CHECK(t.mode.bits == 8 && t.mode_id == 3 && t.mode_bytes == 1);
    uint8_t m[8];
    CHECK(hid_touch_mode_report(&t, ids, m, sizeof m) == 2 && m[0] == 3 && m[1] == 3);

    // and the mouse collection is still found as a mouse
    struct hid_ptr p[HID_MAXRPT]; int pids;
    CHECK(hid_parse_pointer(ptp_desc, sizeof ptp_desc, p, HID_MAXRPT, &pids) == 1 && p[0].id == 1 && p[0].bytes == 4);

    uint8_t r[15];
    memset(r, 0, sizeof r);
    r[0] = 4;
    uint8_t* d = r + 1;
    set_bits(d, 0, 1, 1); set_bits(d, 2, 4, 3); set_bits(d, 8, 16, 500); set_bits(d, 24, 16, 300);   // finger 0: tip, id 3, (500,300)
    set_bits(d, 40, 1, 0); set_bits(d, 42, 4, 5); set_bits(d, 48, 16, 100); set_bits(d, 64, 16, 50);  // finger 1 up
    set_bits(d, 96, 8, 1); set_bits(d, 104, 1, 1);                                                     // count 1, clicked
    struct hid_contact c[HID_MAXFINGER]; int nc, count, button;
    CHECK(hid_touch_decode(&t, ids, r, 15, c, &nc, &count, &button));
    CHECK(nc == 2 && c[0].tip == 1 && c[0].id == 3 && c[0].x == 500 && c[0].y == 300 && c[1].tip == 0 && c[1].id == 5);
    CHECK(count == 1 && button == 1);
    r[0] = 1;
    CHECK(!hid_touch_decode(&t, ids, r, 15, c, &nc, &count, &button));      // another report id
}

static void feed(struct ptp* p, uint32_t t, int tip, int id, int x, int y, struct ptp_out* o) {
    struct hid_contact c = { tip, id, x, y };
    ptp_report(p, t, &c, 1, tip ? 1 : 0, 0, o);
}

static void test_gestures(void) {
    struct ptp p; struct ptp_out o;
    ptp_init(&p, 1000, 600, 1000);
    // one finger moving right 10 units per frame: the first frame is only a touch-down
    int sx = 0, sy = 0;
    for (int i = 0; i < 20; i++) { feed(&p, 1000 + i * 10, 1, 1, 100 + i * 10, 200, &o); sx += o.dx; sy += o.dy; if (i == 0) CHECK(o.dx == 0 && o.dy == 0); }
    CHECK(sx >= 190 && sx <= 300); CHECK(sy == 0);
    feed(&p, 1300, 0, 1, 290, 200, &o);                                   // lift after 300 ms of movement: no tap
    CHECK(!(o.buttons & 1));
    ptp_idle(&p, 1310, 0, &o); CHECK(!(o.buttons & 1));

    // tap: down 100 ms, no movement
    ptp_init(&p, 1000, 600, 1000);
    feed(&p, 5000, 1, 7, 400, 300, &o);
    feed(&p, 5050, 1, 7, 401, 300, &o);
    feed(&p, 5100, 0, 7, 401, 300, &o);
    CHECK(o.buttons == 1 && o.changed);                                   // press
    ptp_idle(&p, 5110, 0, &o);
    CHECK(o.buttons == 0 && o.changed);                                   // release
    ptp_idle(&p, 5120, 0, &o);
    CHECK(!o.changed);

    // slow touch: not a tap
    ptp_init(&p, 1000, 600, 1000);
    feed(&p, 6000, 1, 1, 400, 300, &o);
    feed(&p, 6600, 0, 1, 400, 300, &o);
    CHECK(o.buttons == 0);

    // two-finger scroll: both fingers move up 40 units per frame (yrange/30 = 20 per wheel step)
    ptp_init(&p, 1000, 600, 1000);
    int wheel = 0;
    for (int i = 0; i < 6; i++) {
        struct hid_contact c[2] = { { 1, 1, 300, 400 - i * 40 }, { 1, 2, 500, 400 - i * 40 } };
        ptp_report(&p, 7000 + i * 10, c, 2, 2, 0, &o);
        wheel += o.wheel;
        CHECK(o.dx == 0 && o.dy == 0);
    }
    CHECK(wheel == 5 * 2);                                                // 5 deltas of 40 = 2 steps each, up = positive (down)

    // two-finger tap = right click
    ptp_init(&p, 1000, 600, 1000);
    { struct hid_contact c[2] = { { 1, 1, 300, 300 }, { 1, 2, 500, 300 } }; ptp_report(&p, 8000, c, 2, 2, 0, &o); ptp_report(&p, 8040, c, 2, 2, 0, &o); }
    feed(&p, 8100, 0, 1, 300, 300, &o);
    CHECK(o.buttons == 2);

    // physical click passes through and drags
    ptp_init(&p, 1000, 600, 1000);
    { struct hid_contact c = { 1, 1, 100, 100 }; ptp_report(&p, 9000, &c, 1, 1, 1, &o); CHECK(o.buttons == 1 && o.changed); }
    { struct hid_contact c = { 1, 1, 130, 100 }; ptp_report(&p, 9010, &c, 1, 1, 1, &o); CHECK(o.buttons == 1 && o.dx > 0); }
    { struct hid_contact c = { 0, 1, 130, 100 }; ptp_report(&p, 9020, &c, 1, 0, 0, &o); CHECK(o.buttons == 0 && o.changed); }

    // hybrid frame: 2 contacts announced in the first report, the second one follows in a report with count 0
    ptp_init(&p, 1000, 600, 1000);
    { struct hid_contact a = { 1, 1, 300, 400 }, b = { 1, 2, 500, 400 };
      ptp_report(&p, 9500, &a, 1, 2, 0, &o); CHECK(!o.changed);
      ptp_report(&p, 9505, &b, 1, 0, 0, &o);
      a.y = 360; b.y = 360;
      ptp_report(&p, 9510, &a, 1, 2, 0, &o);
      ptp_report(&p, 9515, &b, 1, 0, 0, &o); CHECK(o.wheel == 2); }
}

// ---- a software model of the DesignWare controller + a HID-over-I2C touchpad -------------------------
struct model {
    int enabled, tar, abort, in_txn, did_write_phase;
    uint8_t wbuf[2048]; int wn;
    uint8_t rbuf[2048]; int rn, rh;          // device -> host bytes of the current transaction
    uint8_t fifo[256]; int fn, fh;           // controller RX FIFO
    uint32_t abrt_src;
    // the device
    int dev_addr;
    uint8_t desc[30];
    uint8_t rdesc[512]; int rdl;
    int reset_pending, power, ptr;
    uint8_t input[64]; int inlen;            // a report waiting (with its 2-byte length prefix already in place)
    uint8_t last_cmd[2048]; int last_cmd_n;
    int fifo_max_seen;
};

static void dev_write_phase(struct model* m) {
    if (m->did_write_phase) return;
    m->did_write_phase = 1;
    m->rn = m->rh = 0;
    unsigned reg = m->wn >= 2 ? (unsigned)(m->wbuf[0] | (m->wbuf[1] << 8)) : 0;
    // like the real SYNA3602: a plain read returns what the LAST ADDRESSED register holds (the descriptor after a descriptor read)
    int plain = m->wn == 0;
    if (plain && m->ptr == 0x20) { memcpy(m->rbuf, m->desc, 30); m->rn = 30; return; }
    if (plain && m->ptr == 0x21) { memcpy(m->rbuf, m->rdesc, (size_t)m->rdl); m->rn = m->rdl; return; }
    if (plain || (m->wn == 2 && reg == 0x0023)) {   // the input register
        m->ptr = 0;
        if (m->inlen) { memcpy(m->rbuf, m->input, (size_t)m->inlen); m->rn = m->inlen; m->inlen = 0; }
        else if (m->reset_pending) { m->rbuf[0] = m->rbuf[1] = 0; m->rn = 2; m->reset_pending = 0; }
        else { m->rbuf[0] = m->rbuf[1] = 0; m->rn = 2; }
        return;
    }
    if (m->wn == 2 && reg == 0x0020) { memcpy(m->rbuf, m->desc, 30); m->rn = 30; m->ptr = 0x20; }
    else if (m->wn == 2 && reg == 0x0021) { memcpy(m->rbuf, m->rdesc, (size_t)m->rdl); m->rn = m->rdl; m->ptr = 0x21; }
    else if (reg == 0x0022 && m->wn >= 4) {
        m->ptr = 0;
        memcpy(m->last_cmd, m->wbuf, (size_t)m->wn); m->last_cmd_n = m->wn;
        if (m->wbuf[3] == 0x01) m->reset_pending = 1;
        if (m->wbuf[3] == 0x08) m->power = m->wbuf[2] == 0;
    }
    // unknown registers: nothing (the reads return zeros)
}

static uint32_t m_rd(void* c, uint32_t off) {
    struct model* m = (struct model*)c;
    switch (off) {
    case 0x9C: return (uint32_t)m->enabled;
    case 0x70: return 2u | 4u | (m->fn > m->fh ? 8u : 0u);
    case 0x34: return m->abort ? (1u << 6) : 0;
    case 0x10: return m->fn > m->fh ? m->fifo[m->fh++] : 0;
    case 0x54: m->abort = 0; return 0;
    case 0x40: m->abort = 0; return 0;
    case 0x80: return m->abrt_src;
    case 0xFC: return DW_COMP_TYPE;
    case 0xF4: return (7u << 8) | (31u << 16);             // receive FIFO 8 deep, transmit 32
    default: return 0;
    }
}

static void m_wr(void* c, uint32_t off, uint32_t v) {
    struct model* m = (struct model*)c;
    if (off == 0x6C) { m->enabled = (int)(v & 1); if (!m->enabled) { m->fn = m->fh = 0; } return; }
    if (off == 0x04) { if (m->enabled) { printf("FAIL: TAR written while enabled\n"); fails++; } m->tar = (int)v; return; }
    if (off != 0x10) return;
    if (!m->enabled) { printf("FAIL: data written while disabled\n"); fails++; return; }
    if (m->abort) return;
    if (m->tar != m->dev_addr) { m->abort = 1; m->abrt_src = 1; return; }
    if (!m->in_txn) { m->in_txn = 1; m->wn = 0; m->did_write_phase = 0; }
    if (v & 0x100) {                                       // read
        dev_write_phase(m);
        if (m->fn - m->fh > m->fifo_max_seen) m->fifo_max_seen = m->fn - m->fh;
        if (m->fn - m->fh >= 8) { printf("FAIL: receive FIFO overrun\n"); fails++; }
        if (m->fh == m->fn) m->fh = m->fn = 0;
        m->fifo[m->fn++] = m->rh < m->rn ? m->rbuf[m->rh++] : 0xEE;
    } else {
        if (m->wn < (int)sizeof m->wbuf) m->wbuf[m->wn++] = (uint8_t)v;
    }
    if (v & 0x200) { dev_write_phase(m); m->in_txn = 0; }
}

static void m_delay(uint32_t us) { (void)us; }
static void m_delay_ms(uint32_t ms) { (void)ms; }

static struct model M;
static struct dwi2c DW;
static int bus_xfer(void* ctx, int addr, const uint8_t* w, int wl, uint8_t* r, int rl) { (void)ctx; return dw_xfer(&DW, addr, w, wl, r, rl); }

static void test_bus(void) {
    memset(&M, 0, sizeof M);
    M.dev_addr = 0x2C;
    uint8_t d[30] = { 30, 0, 0x00, 0x01, (uint8_t)sizeof ptp_desc, 0, 0x21, 0, 0x23, 0, 0x4F, 0, 0x24, 0, 0x4F, 0, 0x22, 0, 0x25, 0, 0xCB, 0x06, 0x02, 0x36, 0x01, 0 };
    d[4] = (uint8_t)(sizeof ptp_desc & 0xFF); d[5] = (uint8_t)(sizeof ptp_desc >> 8);
    memcpy(M.desc, d, 30);
    memcpy(M.rdesc, ptp_desc, sizeof ptp_desc); M.rdl = (int)sizeof ptp_desc;
    DW.rd = m_rd; DW.wr = m_wr; DW.delay_us = m_delay; DW.ctx = &M;
    CHECK(dw_present(&DW));
    CHECK(dw_setup(&DW, 0) == 0);
    CHECK(DW.rx_depth == 8 && DW.tx_depth == 32);

    struct hidi2c h;
    memset(&h, 0, sizeof h);
    h.bus.xfer = bus_xfer; h.bus.delay_ms = m_delay_ms; h.addr = 0x2C;
    CHECK(hidi2c_get_desc(&h, 0x0001) == 1);                      // wrong register: zeros come back
    CHECK(hidi2c_get_desc(&h, 0x0020) == 0);
    CHECK(h.vid == 0x06CB && h.pid == 0x3602 && h.rdesc_reg == 0x21 && h.in_reg == 0x23 && h.max_in == 0x4F && h.cmd_reg == 0x22 && h.data_reg == 0x25);
    h.addr = 0x2D;
    CHECK(hidi2c_get_desc(&h, 0x0020) == -2 && DW.last_abort == 1);   // nobody there: NACK
    h.addr = 0x2C;
    CHECK(hidi2c_get_desc(&h, 0x0020) == 0);                      // the controller recovered from the abort

    CHECK(hidi2c_power(&h, 1) == 0);
    CHECK(M.last_cmd_n == 4 && M.last_cmd[2] == 0x00 && M.last_cmd[3] == 0x08 && M.power);
    CHECK(hidi2c_reset(&h) == 0);
    CHECK(M.last_cmd[3] == 0x01);

    // the report descriptor is longer than the receive FIFO
    h.rdesc_len = (uint16_t)sizeof ptp_desc;
    uint8_t rd[256];
    CHECK(hidi2c_report_desc(&h, rd, sizeof rd) == h.rdesc_len);
    CHECK(!memcmp(rd, ptp_desc, h.rdesc_len));
    CHECK(M.fifo_max_seen <= 7);

    // input polling: nothing, then a report (the last thing addressed was the report descriptor: must still read input)
    uint8_t buf[64];
    h.max_in = 0x10;
    CHECK(hidi2c_input(&h, buf, sizeof buf) == -5 && h.stale == 1);   // the device still answers from the descriptor register: not a report, and it is pointed back
    CHECK(hidi2c_input(&h, buf, sizeof buf) == 0 && h.zeros == 1);
    CHECK(hidi2c_get_desc(&h, 0x0020) == 0);                          // the bring-up ends with a descriptor read: the next input read sees the descriptor
    CHECK(hidi2c_input(&h, buf, sizeof buf) == -5 && h.stale == 2);
    CHECK(hidi2c_input(&h, buf, sizeof buf) == 0);
    M.input[0] = 5; M.input[1] = 0; M.input[2] = 4; M.input[3] = 0xAA; M.input[4] = 0xBB; M.inlen = 5;
    h.max_in = 0x10;
    int n = hidi2c_input(&h, buf, sizeof buf);
    CHECK(n == 3 && buf[0] == 4 && buf[1] == 0xAA && buf[2] == 0xBB);
    CHECK(hidi2c_input(&h, buf, sizeof buf) == 0);

    // SET_REPORT
    uint8_t mode = 3;
    CHECK(hidi2c_set_feature(&h, 3, &mode, 1) == 0);
    CHECK(M.last_cmd_n == 10);
    CHECK(M.last_cmd[2] == 0x33 && M.last_cmd[3] == 0x03 && M.last_cmd[4] == 0x25 && M.last_cmd[5] == 0x00);
    CHECK(M.last_cmd[6] == 4 && M.last_cmd[7] == 0 && M.last_cmd[8] == 3 && M.last_cmd[9] == 3);
}

// ---- AML ---------------------------------------------------------------------------------------
static int put_pkg(uint8_t* o, int body) {       // 2-byte PkgLength for a package whose content (after the length) is `body` bytes
    int total = body + 2;
    o[0] = (uint8_t)(0x40 | (total & 0x0F)); o[1] = (uint8_t)(total >> 4);
    return 2;
}

static int build_dsdt(uint8_t* t, int with_dsm) {
    memset(t, 0, 4096);
    memcpy(t, "DSDT", 4);
    int n = 36;
    uint8_t body[1024]; int b = 0;
    memcpy(body + b, "TPD0", 4); b += 4;
    body[b++] = 0x08; memcpy(body + b, "_HID", 4); b += 4; body[b++] = 0x0D; memcpy(body + b, "SYNA3602", 9); b += 9;
    body[b++] = 0x08; memcpy(body + b, "_CID", 4); b += 4; body[b++] = 0x0D; memcpy(body + b, "PNP0C50", 8); b += 8;
    body[b++] = 0x08; memcpy(body + b, "SBFB", 4); b += 4; body[b++] = 0x11; body[b++] = 0x40; body[b++] = 0x00; body[b++] = 0x0A; body[b++] = 0x20;
    static const uint8_t desc[] = { 0x8E, 0x1D, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x06, 0x00, 0x80, 0x1A, 0x06, 0x00, 0x2C, 0x00 };
    memcpy(body + b, desc, sizeof desc); b += sizeof desc;
    memcpy(body + b, "\\_SB.PCI0.I2C5", 15); b += 15;
    body[b++] = 0x79; body[b++] = 0x00;
    if (with_dsm) {
        body[b++] = 0x14; body[b++] = 0x30; memcpy(body + b, "_DSM", 4); b += 4; body[b++] = 0x04;
        static const uint8_t guid[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45, 0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
        body[b++] = 0x11; body[b++] = 0x13; body[b++] = 0x0A; body[b++] = 0x10; memcpy(body + b, guid, 16); b += 16;
        body[b++] = 0xA0; body[b++] = 0x10; body[b++] = 0x93; body[b++] = 0x5F; body[b++] = 0x54; body[b++] = 0x5F; body[b++] = 0x31;
        body[b++] = 0xA4; body[b++] = 0x0A; body[b++] = 0x20;                           // Return (0x20)
        body[b++] = 0xA4; body[b++] = 0x0B; body[b++] = 0x01; body[b++] = 0x01;         // Return (0x0101)
    }
    t[n++] = 0x5B; t[n++] = 0x82;
    n += put_pkg(t + n, b);
    memcpy(t + n, body, (size_t)b); n += b;
    *(uint32_t*)(t + 4) = (uint32_t)n;
    return n;
}

static void test_aml(void) {
    static uint8_t t[4096];
    int n = build_dsdt(t, 1);
    struct i2c_hid_dev d[4];
    CHECK(i2caml_find(t, (uint32_t)n, d, 4) == 1);
    CHECK(!strcmp(d[0].hid, "SYNA3602") && d[0].addr == 0x2C && d[0].speed == 400000 && !strcmp(d[0].ctrl, "\\_SB.PCI0.I2C5"));
    CHECK(d[0].nreg == 2 && d[0].reg[0] == 0x20 && d[0].reg[1] == 0x0101);
    n = build_dsdt(t, 0);
    CHECK(i2caml_find(t, (uint32_t)n, d, 4) == 1 && d[0].nreg == 0);
    // not a touchpad id
    memcpy(t + 36 + 2 + 2 + 4 + 1 + 4 + 1, "XXXX3602", 8);
    memcpy(t + 36 + 2 + 2 + 4 + 1 + 4 + 1 + 9 + 1 + 4 + 1, "XXXXXXX", 7);
    CHECK(i2caml_find(t, (uint32_t)n, d, 4) == 0);
}

static void fuzz(void) {
    unsigned seed = 777;
    uint8_t base[4096], buf[4096];
    int bn = build_dsdt(base, 1);
    for (int it = 0; it < 200000; it++) {
        int n;
        if (it & 1) {
            n = 36 + (int)(seed = seed * 1103515245u + 12345u, (seed >> 8) % 600);
            for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; buf[i] = (uint8_t)(seed >> 16); }
        } else {
            n = bn; memcpy(buf, base, (size_t)bn);
            for (int m = 0; m < 5; m++) { seed = seed * 1103515245u + 12345u; int p = (int)((seed >> 8) % (unsigned)n); seed = seed * 1103515245u + 12345u; buf[p] = (uint8_t)(seed >> 16); }
        }
        struct i2c_hid_dev d[4];
        i2caml_find(buf, (uint32_t)n, d, 4);
        struct hid_touch t; int ids;
        int dn = (it & 1) ? n : (int)sizeof ptp_desc;
        if (!(it & 1)) { memcpy(buf, ptp_desc, sizeof ptp_desc); for (int m = 0; m < 4; m++) { seed = seed * 1103515245u + 12345u; int p = (int)((seed >> 8) % (unsigned)dn); seed = seed * 1103515245u + 12345u; buf[p] = (uint8_t)(seed >> 16); } }
        if (hid_parse_touch(buf, dn, &t, &ids)) {
            uint8_t rep[64];
            for (int i = 0; i < 64; i++) { seed = seed * 1103515245u + 12345u; rep[i] = (uint8_t)(seed >> 16); }
            struct hid_contact c[HID_MAXFINGER]; int nc, cnt, bt;
            hid_touch_decode(&t, ids, rep, 64, c, &nc, &cnt, &bt);
            uint8_t mr[16];
            hid_touch_mode_report(&t, ids, mr, sizeof mr);
        }
    }
}

static void fuzz_ptp(void) {
    unsigned seed = 99;
    struct ptp p; struct ptp_out o;
    ptp_init(&p, 1000, 600, 1366);
    uint32_t t = 0;
    for (int it = 0; it < 500000; it++) {
        struct hid_contact c[HID_MAXFINGER];
        seed = seed * 1103515245u + 12345u;
        int nc = (int)((seed >> 8) % 7);
        for (int i = 0; i < HID_MAXFINGER; i++) { seed = seed * 1103515245u + 12345u; c[i].tip = (seed >> 8) & 1; c[i].id = (int)((seed >> 12) & 7); c[i].x = (int)((seed >> 4) & 0x3FF); c[i].y = (int)((seed >> 14) & 0x3FF); }
        seed = seed * 1103515245u + 12345u;
        t += (seed >> 8) & 31;
        if (it % 5 == 0) ptp_idle(&p, t, (int)(seed >> 20) & 1, &o);
        else ptp_report(&p, t, c, nc, (int)((seed >> 16) % 8), (int)(seed >> 24) & 1, &o);
        if (o.dx > 3000 || o.dx < -3000 || o.dy > 3000 || o.dy < -3000) { printf("FAIL: runaway motion %d %d\n", o.dx, o.dy); fails++; break; }
    }
}

// Input Mode feature in a separate "Device Configuration" collection (what the real SYNA3602 descriptor has: mode-feature was 0)
static void test_mode_separate(void) {
    static const uint8_t d[] = {
        0x05,0x0D,0x09,0x05,0xA1,0x01,0x85,0x04,                       // Touch Pad, report id 4
        0x09,0x22,0xA1,0x02,0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02,0x75,0x07,0x95,0x01,0x81,0x03,
        0x05,0x01,0x09,0x30,0x75,0x10,0x95,0x01,0x15,0x00,0x26,0x80,0x0C,0x81,0x02,0x09,0x31,0x26,0x5C,0x06,0x81,0x02,0xC0,
        0x05,0x0D,0x09,0x54,0x75,0x08,0x95,0x01,0x81,0x02,0xC0,
        0x05,0x0D,0x09,0x0E,0xA1,0x01,0x85,0x03,                       // Device Configuration, report id 3
        0x09,0x22,0xA1,0x02,0x09,0x52,0x15,0x00,0x25,0x0A,0x75,0x08,0x95,0x01,0xB1,0x02,0xC0,0xC0 };
    struct hid_touch t;
    int ids = 0;
    CHECK(hid_parse_touch(d, (int)sizeof d, &t, &ids));
    CHECK(ids == 1 && t.id == 4 && t.nf == 1);
    CHECK(t.mode.bits == 8 && t.mode_id == 3 && t.mode_bytes == 1);
    uint8_t mr[8];
    CHECK(hid_touch_mode_report(&t, ids, mr, sizeof mr) == 2 && mr[0] == 3 && mr[1] == 3);
}

// the controller base from the firmware's named fields (bytes of the real laptop's I2C5 device, GNVS region/field synthetic)
static uint32_t fake_mem[1024];                     // pretend GNVS at 0x7b7dba98
static int fake_rd(uint32_t phys, uint32_t* v) {
    if (phys < 0x7b7dba98u || phys >= 0x7b7dba98u + 4096) return 0;
    *v = fake_mem[(phys - 0x7b7dba98u) / 4];
    return 1;
}
static void test_bars(void) {
    static const uint8_t dev[] = {
        0x5b,0x82,0x41,0x37,'I','2','C','5',0x08,'_','H','I','D',0x0d,'8','0','8','6','2','2','C','1',0x00,
        0x08,'_','U','I','D',0x0a,0x05,0x08,'R','B','U','F',0x11,0x26,0x0a,0x23,0x86,0x09,0x00,0x01,0x00,0x00,0x00,
        0x00,0x00,0x10,0x00,0x00,0x89,0x06,0x00,0x05,0x01,0x24,0x00,0x00,0x00,0x79,0x00,0x14,0x33,'_','C','R','S',0x00,
        0x8a,'R','B','U','F',0x0a,0x04,'B','0','B','A',0x8a,'R','B','U','F',0x0a,0x08,'B','0','L','N',
        0x70,'I','5','0','A','B','0','B','A',0x70,'I','5','0','L','B','0','L','N',0xa4,'R','B','U','F',
        0x14,0x26,'_','P','S','0',0x00,0xa0,0x1f,0x93,'I','5','D','3',0x01,0x7b,'P','S','A','T',0x0c,0xfc,0xff,
        0xff,0xff,'P','S','A','T',0x7d,'P','S','A','T',0x00,'P','S','A','T',
        0x5b,0x80,'K','E','Y','S',0x00,'I','5','1','A',0x0b,0x00,0x01,
        0x5b,0x81,0x0e,'K','E','Y','S',0x43,0x00,0x40,0x42,'P','S','A','T',0x20 };
    uint8_t t[2048];
    uint32_t n = 36;
    memset(t, 0, sizeof t);
    memcpy(t, "DSDT", 4);
    static const uint8_t reg[] = { 0x5b,0x80,'G','N','V','S',0x00,0x0c,0x98,0xba,0x7d,0x7b,0x0b,0x4a,0x03 };   // OperationRegion(GNVS, SystemMemory, 0x7b7dba98, 842)
    memcpy(t + n, reg, sizeof reg); n += sizeof reg;
    // Field(GNVS) { Offset(456), I50A,32, I50L,32, I51A,32 }: 3648 reserved bits = PkgLength 0x40 0xE4
    static const uint8_t fld[] = { 0x5b,0x81,0x1a,'G','N','V','S',0x10, 0x00,0x40,0xe4, 'I','5','0','A',0x20, 'I','5','0','L',0x20, 'I','5','1','A',0x20 };
    memcpy(t + n, fld, sizeof fld); n += sizeof fld;
    memcpy(t + n, dev, sizeof dev);
    uint32_t body = (uint32_t)sizeof dev - 2;                                          // PkgLength covers itself and everything after it
    t[n + 2] = (uint8_t)(0x40 | (body & 0xF)); t[n + 3] = (uint8_t)(body >> 4);
    n += sizeof dev;
    t[36 + sizeof reg + 2] = 0x18;                                                     // the Field's PkgLength
    fake_mem[456 / 4] = 0x81350000u; fake_mem[460 / 4] = 0x1000; fake_mem[464 / 4] = 0x81351000u;
    const uint8_t* tabs[1] = { t };
    uint32_t lens[1] = { n };
    struct i2c_ctrl_bars b;
    CHECK(i2caml_ctrl_bars(tabs, lens, 1, "\\_SB.PCI0.I2C5", fake_rd, &b));
    CHECK(b.bar0 == 0x81350000u && b.len0 == 0x1000 && b.psat_known && b.bar1 == 0x81351000u);
    CHECK(!i2caml_ctrl_bars(tabs, lens, 1, "\\_SB.PCI0.I2C6", fake_rd, &b));         // no such device
    for (uint32_t cut = 0; cut < n; cut++) { uint32_t l2 = cut; (void)i2caml_ctrl_bars(tabs, &l2, 1, "\\_SB.PCI0.I2C5", fake_rd, &b); }   // truncations stay in bounds
}

int main(void) {
    test_ptp_parse(); test_gestures(); test_bus(); test_aml(); test_bars(); test_mode_separate(); fuzz(); fuzz_ptp();
    printf(fails ? "TOUCH FAIL (%d)\n" : "TOUCH PASS\n", fails);
    return fails != 0;
}
