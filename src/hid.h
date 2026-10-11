#ifndef HID_H
#define HID_H

#include <stdint.h>

// HID report descriptor parser for pointing devices (mice, trackballs, tablets in mouse mode) and touchpads
// (Windows Precision Touchpad reports). Pure code: no kernel dependencies, host-tested with fuzzing
// (tests/test_hid.c, tests/test_touch.c). Written from the USB HID 1.11 specification (section 6.2.2) and the
// Windows Precision Touchpad report layout (usage page 0x0D: Touch Pad 5, Finger 0x22, Tip Switch 0x42, Contact
// Identifier 0x51, Contact Count 0x54).
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

// ---- touchpads ----------------------------------------------------------------------------------
#define HID_MAXFINGER 5

struct hid_finger {
    struct hid_fld tip, id, x, y;
};

struct hid_touch {
    uint8_t id;                       // report id (0 = none)
    uint16_t bytes;                   // whole report including the id byte
    int nf;                           // finger blocks in one report
    struct hid_finger f[HID_MAXFINGER];
    struct hid_fld count;             // contact count (report level)
    struct hid_fld button;            // the click button of a clickpad (usage page 9, button 1)
    struct hid_fld mode;              // the Input Mode feature (usage 0x52) that switches mouse emulation to touchpad reports
    uint8_t mode_id;                  // its report id
    uint16_t mode_bytes;              // size of that feature report without the id byte
};

// The input report of a top-level Touch Pad collection (the last one wins when there are several). Returns 1 when found.
int hid_parse_touch(const uint8_t* d, int n, struct hid_touch* out, int* uses_ids);

// Builds the feature report that puts the touchpad into Precision Touchpad mode (Input Mode = 3). Returns its length
// (the id byte first when uses_ids), 0 when the device has no such feature.
int hid_touch_mode_report(const struct hid_touch* t, int uses_ids, uint8_t* out, int cap);

struct hid_contact { int tip, id, x, y; };

// Decodes a report into at most HID_MAXFINGER contacts; *count = the contact count field (0 when absent), *button = click.
// Returns 1 when the report belongs to the layout.
int hid_touch_decode(const struct hid_touch* t, int uses_ids, const uint8_t* r, int len,
                     struct hid_contact* c, int* nc, int* count, int* button);

#endif
