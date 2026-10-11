#ifndef PTP_H
#define PTP_H

#include <stdint.h>
#include "hid.h"

// Touchpad gestures: turns frames of finger contacts (hid_touch_decode) into pointer motion. Pure code, host-tested.
//   one finger        -> pointer motion (with a little acceleration)
//   two fingers       -> scrolling
//   quick tap         -> left click, quick two-finger tap -> right click
//   the clickpad button -> left button held
struct ptp {
    int xrange, yrange;            // logical size of the pad (units)
    int screen_w;                  // pixels the full pad width maps to
    // frame assembly (hybrid reports deliver the contacts of one frame over several reports)
    struct hid_contact buf[HID_MAXFINGER * 2];
    int nbuf, expect;
    // tracking
    int touching, prim_id, have_prev, px, py;
    int pair_prev, pair_y;         // previous mean Y of two fingers
    int max_f, travel, lastb;
    uint32_t t_start, t_frame;
    int remx, remy, scroll_acc;
    int pend, pend_stage;          // tap click pending: bit0 left, bit1 right; stage 1 = press, 2 = release
};

struct ptp_out {
    int dx, dy, wheel, buttons;    // buttons: bit0 left, bit1 right
    int changed;                   // anything to inject
};

void ptp_init(struct ptp* p, int xrange, int yrange, int screen_w);

// One decoded report. now_ms = a millisecond clock. Fills `o`.
void ptp_report(struct ptp* p, uint32_t now_ms, const struct hid_contact* c, int nc, int count, int button, struct ptp_out* o);

// Call every poll without a report: delivers the release of a tap click.
void ptp_idle(struct ptp* p, uint32_t now_ms, int button, struct ptp_out* o);

#endif
