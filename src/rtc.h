#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include "crypto/x509.h"

// CMOS RTC read (ports 0x70/0x71 — plain port I/O, no BIOS needed).
// Used for certificate validity checking. Best effort: on garbage/timeout
// the caller's default (build date) stays in effect.

// Reads the wall clock into *out. Returns 0 on success, -1 if the values
// looked insane (so the caller can keep its default).
int rtc_read(x509_time* out);

#endif
