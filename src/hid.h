#ifndef HID_H
#define HID_H

#include <stdint.h>

// HID report descriptor parser for pointing devices (mice, trackballs, tablets in mouse mode). Pure code: no kernel
// dependencies, host-tested with fuzzing (tests/test_hid.c). Written from the USB HID 1.11 specification (section 6.2.2).
//
// hid_parse_pointer() walks the descriptor and returns, for every input report that belongs to a top-level Generic
// Desktop Mouse or Pointer collection and carries X and Y, where the fields sit in the report.

#define HID_MAXRPT 4

struct hid_fld {
    int16_t off;          // bit offset from the first data byte (after the report id, when there is one)
    uint8_t bits;         // 0 = the field is absent
    uint8_t sign;         // logical minimum < 0
    int32_t lmin, lmax;
};

struct hid_ptr {
    uint8_t id;           // report id (0 = the device has no report ids)
    uint8_t absolute;     // X/Y are absolute positions (tablet), else relative motion
    uint16_t bytes;       // length of the whole report including the id byte
    struct hid_fld x, y, wheel, pan;
    struct hid_fld btn[8];     // buttons 1..8 (usage page 9): bits != 0 when present
};

// Returns the number of pointer reports found (0 = not a pointer), at most `max`. *uses_ids = the descriptor uses report
// ids (every report then starts with its id byte).
int hid_parse_pointer(const uint8_t* d, int n, struct hid_ptr* out, int max, int* uses_ids);

// bits [off, off+bits) of the report `r` (len bytes), little-endian bit order; sign-extended when `sign`. Out of range = 0.
int32_t hid_get(const uint8_t* r, int len, int off, int bits, int sign);

// Decodes one report with the layout `p`. Returns 1 when the report belongs to this layout. Buttons come out as bits
// 0..2 = left, right, middle (4th/5th button ignored by the kernel pointer). `rx`/`ry` are the raw X/Y values
// (relative motion, or the absolute position scaled 0..65535 over the field's logical range).
int hid_decode(const struct hid_ptr* p, int uses_ids, const uint8_t* r, int len,
               int* rx, int* ry, int* wheel, int* buttons);

#endif
