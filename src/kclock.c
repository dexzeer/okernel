// kclock.c — wall clock for the kernel: the CMOS RTC is read once (UTC, as
// QEMU and most hypervisors keep it), then advanced by the 100 Hz tick.

#include <stdint.h>
#include "rtc.h"

extern uint32_t tick_count;     // 100 Hz since boot (desktop.c)

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t boot_epoch = -1;  // UTC seconds at base_tick
static uint32_t base_tick;

static void kclock_init(void) {
    x509_time t;
    base_tick = tick_count;
    if (rtc_read(&t) == 0)
        boot_epoch = days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    else
        boot_epoch = 1791331200;   // 2026-10-07: RTC unreadable
}

// milliseconds since 1970-01-01T00:00:00Z
int64_t kclock_epoch_ms(void) {
    if (boot_epoch < 0) kclock_init();
    uint32_t dt = tick_count - base_tick;
    return boot_epoch * 1000 + (int64_t)dt * 10;
}

// seconds since the epoch (the C library shape okai uses)
long long time(long long* t) {
    long long s = kclock_epoch_ms() / 1000;
    if (t) *t = s;
    return s;
}
