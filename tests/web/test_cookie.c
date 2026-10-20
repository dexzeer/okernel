// Cookie jar: Expires parsing (src/web/wcookie.c).
// parse_http_date once had a 0-based month fed to the 1-based days-from-civil formula, so every Expires
// landed ~30 days early: a cookie expiring in 90 minutes (go-away's .go-away-*-state on invidious/nadeko)
// was "already expired" on arrival and dropped, and the proof-of-work challenge looped forever.
#include <stdio.h>
#include <string.h>
#include "web/wcookie.h"

static long long g_now;
static long long fake_now(void) { return g_now; }

// independent reference: Howard Hinnant's days_from_civil, month 1..12
static long long epoch(int y, int m, int d, int hh, int mm) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60;
}

static const char* const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* const DAY[7] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };   // day names are ignored by the parser
static int fails, checks;

// set a cookie expiring at (y,m,d hh:mm) while the clock says `now`; 1 if the jar kept it
static int kept(long long now, int y, int m, int d, int hh, int mm) {
    char hdr[160];
    while (wcookie_count()) {   // empty the jar by expiring everything
        g_now = 1LL << 40;
        char tmp[64];
        wcookie_header("https://example.test/", 0, tmp, sizeof tmp);
        if (wcookie_count()) break;
    }
    g_now = now;
    snprintf(hdr, sizeof hdr, "k=v; Path=/; Expires=%s, %02d %s %d %02d:%02d:00 GMT", DAY[d % 7], d, MON[m - 1], y, hh, mm);
    wcookie_set_http("https://example.test/", hdr, (int)strlen(hdr));
    char out[64];
    return wcookie_header("https://example.test/", 0, out, sizeof out) > 0;
}

static void expect(int got, int want, const char* what, int m) {
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s (month %d): kept=%d want %d\n", what, m, got, want);
    }
}

int main(void) {
    wcookie_now = fake_now;
    // every month: a day ahead is kept, a day ago is dropped
    for (int m = 1; m <= 12; m++) {
        long long now = epoch(2026, m, 15, 12, 0);
        expect(kept(now, 2026, m, 16, 12, 0), 1, "expires tomorrow", m);
        expect(kept(now, 2026, m, 14, 12, 0), 0, "expired yesterday", m);
    }
    // the go-away state cookie: 20 Oct 2026 12:36 UTC, Expires 14:00 the same day
    long long now = epoch(2026, 10, 20, 12, 36);
    expect(kept(now, 2026, 10, 20, 14, 0), 1, "expires in 84 minutes", 10);
    expect(kept(now, 2026, 10, 20, 11, 0), 0, "expired 96 minutes ago", 10);
    // year boundaries and a leap day
    expect(kept(epoch(2027, 12, 31, 23, 0), 2028, 1, 1, 1, 0), 1, "across new year", 12);
    expect(kept(epoch(2028, 2, 28, 12, 0), 2028, 2, 29, 12, 0), 1, "leap day", 2);
    expect(kept(epoch(2028, 2, 29, 12, 0), 2028, 3, 1, 12, 0), 1, "leap day to march", 3);
    printf("cookie expires: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
