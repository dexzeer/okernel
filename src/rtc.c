#include "rtc.h"
#include "io.h"

// CMOS RTC: address/data through ports 0x70/0x71. Registers:
//   0x00 seconds, 0x02 minutes, 0x04 hours, 0x07 day-of-month,
//   0x08 month, 0x09 year, 0x32 century, 0x0A status A (bit7 = UIP),
//   0x0B status B (bit2 = 0 => BCD, bit1 = 24h mode).

#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_CENTURY     0x32
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B

static uint8_t rtc_reg(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

// Wait for the update-in-progress flag to clear (bounded; never hangs).
static void rtc_wait_uip(void) {
    for (int i = 0; i < 1000000; i++) {
        if ((rtc_reg(RTC_STATUS_A) & 0x80) == 0) return;
    }
}

static int bcd_or_bin(uint8_t v, int is_bcd) {
    if (is_bcd) return (v & 0x0F) + (v >> 4) * 10;
    return v;
}

int rtc_read(x509_time* out) {
    // Read twice; the values must agree (RTC tick won't straddle both).
    int is_bcd = (rtc_reg(RTC_STATUS_B) & 0x04) == 0;

    rtc_wait_uip();
    int s1 = bcd_or_bin(rtc_reg(RTC_SECONDS), is_bcd);
    int m1 = bcd_or_bin(rtc_reg(RTC_MINUTES), is_bcd);
    int h1 = bcd_or_bin(rtc_reg(RTC_HOURS) & 0x7F, is_bcd);  // strip 12h/PM bit
    int d1 = bcd_or_bin(rtc_reg(RTC_DAY), is_bcd);
    int mo1 = bcd_or_bin(rtc_reg(RTC_MONTH), is_bcd);
    int y1 = bcd_or_bin(rtc_reg(RTC_YEAR), is_bcd);
    int c1 = bcd_or_bin(rtc_reg(RTC_CENTURY), is_bcd);

    rtc_wait_uip();
    int s2 = bcd_or_bin(rtc_reg(RTC_SECONDS), is_bcd);
    int m2 = bcd_or_bin(rtc_reg(RTC_MINUTES), is_bcd);
    int h2 = bcd_or_bin(rtc_reg(RTC_HOURS) & 0x7F, is_bcd);
    int d2 = bcd_or_bin(rtc_reg(RTC_DAY), is_bcd);
    int mo2 = bcd_or_bin(rtc_reg(RTC_MONTH), is_bcd);
    int y2 = bcd_or_bin(rtc_reg(RTC_YEAR), is_bcd);
    int c2 = bcd_or_bin(rtc_reg(RTC_CENTURY), is_bcd);

    if (s1 != s2 || m1 != m2 || h1 != h2 || d1 != d2 || mo1 != mo2 || y1 != y2)
        return -1;

    int year = (c1 > 0 && c1 < 100 ? c1 * 100 : 2000) + y1;
    // Sanity gate: a plausible year for this project's era. Garbage CMOS
    // (e.g. 0x00 / 0xFF floating bus) must not poison validity checks.
    if (year < 2020 || year > 2099) return -1;
    if (mo1 < 1 || mo1 > 12 || d1 < 1 || d1 > 31) return -1;
    if (h1 > 23 || m1 > 59 || s1 > 59) return -1;

    out->year = year;
    out->month = mo1;
    out->day = d1;
    out->hour = h1;
    out->minute = m1;
    out->second = s1;
    return 0;
}
