// b_date.c — Date (ECMA-262 §21.4).
//
// Time values are milliseconds since 1970-01-01T00:00:00Z as doubles.
// The embedder supplies the current time and the local time zone offset
// (ojs_sys_time_ms / ojs_sys_tz_offset); the kernel runs in UTC.

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"

double km_floor(double), km_trunc(double), km_fabs(double);

struct dateobj { struct obj base; double t; };

#define MS_PER_DAY 86400000.0
#define NANV (0.0 / 0.0)

// ---------------------------------------------------------------- time math

static double fmod_pos(double a, double b) {
    double r = a - km_floor(a / b) * b;
    return r;
}

static double day_of(double t) { return km_floor(t / MS_PER_DAY); }
static double time_within_day(double t) { return fmod_pos(t, MS_PER_DAY); }

// days from 1970-01-01 of y-m-d (proleptic Gregorian; m 1..12); exact for any year
static double days_from_civil(double y, int m, double d) {
    y -= m <= 2;
    double era = km_floor(y / 400);
    double yoe = y - era * 400;
    int mp = (m + 9) % 12;
    double doy = km_floor((153.0 * mp + 2) / 5) + d - 1;
    double doe = yoe * 365 + km_floor(yoe / 4) - km_floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(double z, double* yo, int* mo, int* dout) {
    z += 719468;
    double era = km_floor(z / 146097);
    double doe = z - era * 146097;
    double yoe = km_floor((doe - km_floor(doe / 1460) + km_floor(doe / 36524) - km_floor(doe / 146096)) / 365);
    double y = yoe + era * 400;
    double doy = doe - (365 * yoe + km_floor(yoe / 4) - km_floor(yoe / 100));
    double mp = km_floor((5 * doy + 2) / 153);
    int d = (int)(doy - km_floor((153 * mp + 2) / 5) + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *yo = y + (m <= 2);
    *mo = m;
    *dout = d;
}

static int is_finite(double x) { return x == x && x != 1.0 / 0.0 && x != -1.0 / 0.0; }

static double to_integer(double x) { return km_trunc(x); }

// MakeTime / MakeDay / MakeDate / TimeClip
static double make_time(double h, double m, double s, double ms) {
    if (!is_finite(h) || !is_finite(m) || !is_finite(s) || !is_finite(ms)) return NANV;
    return to_integer(h) * 3600000.0 + to_integer(m) * 60000.0 + to_integer(s) * 1000.0 + to_integer(ms);
}

static double make_day(double year, double month, double date) {
    if (!is_finite(year) || !is_finite(month) || !is_finite(date)) return NANV;
    double y = to_integer(year), m = to_integer(month), dt = to_integer(date);
    double ym = y + km_floor(m / 12);
    if (!is_finite(ym) || km_fabs(ym) > 400000) return NANV;
    int mn = (int)fmod_pos(m, 12);
    return days_from_civil(ym, mn + 1, 1) + dt - 1;
}

static double make_date(double day, double time) {
    if (!is_finite(day) || !is_finite(time)) return NANV;
    double tv = day * MS_PER_DAY + time;
    return is_finite(tv) ? tv : NANV;
}

static double time_clip(double t) {
    if (!is_finite(t) || km_fabs(t) > 8.64e15) return NANV;
    return to_integer(t) + 0.0;   // -0 -> +0
}

// local time zone (embedder hooks; weak defaults = UTC)
__attribute__((weak)) double ojs_sys_time_ms(void) { return 0; }
__attribute__((weak)) int ojs_sys_tz_offset(double utc_ms, int is_local) { (void)utc_ms; (void)is_local; return 0; }

static double local_offset(double t, int is_local) { return (double)ojs_sys_tz_offset(t, is_local) * 60000.0; }
static double local_time(double t) { return t + local_offset(t, 0); }
static double utc_of(double t) { return is_finite(t) ? t - local_offset(t, 1) : NANV; }

struct fields { double y; int mon, day, wday, h, m, s, ms; };

static void split(double t, struct fields* f) {
    double day = day_of(t);
    civil_from_days(day, &f->y, &f->mon, &f->day);
    f->mon -= 1;
    f->wday = (int)fmod_pos(day + 4, 7);
    double tw = time_within_day(t);
    f->h = (int)(tw / 3600000.0);
    f->m = (int)fmod_pos(km_floor(tw / 60000.0), 60);
    f->s = (int)fmod_pos(km_floor(tw / 1000.0), 60);
    f->ms = (int)fmod_pos(tw, 1000);
}

// ---------------------------------------------------------------- objects

static int this_date(ojs* J, jv v, struct dateobj** out) {
    if (jv_is_obj(v) && obj_class(jv_obj(v)) == OC_DATE) { *out = (struct dateobj*)jv_obj(v); return 0; }
    throw_type(J, "this is not a Date object.");
    return -1;
}

jv date_new(ojs* J, double t) {
    struct dateobj* d = (struct dateobj*)obj_new(J, J->I.date_proto, OC_DATE, sizeof(struct dateobj));
    if (!d) return JV_EXC;
    d->t = time_clip(t);
    return jv_from_obj(&d->base);
}

// ---------------------------------------------------------------- formatting

static const char* const WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* const MN[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int put_year(char* b, double y, int iso) {
    long long yy = (long long)y;
    if (iso) {
        if (yy >= 0 && yy <= 9999) return ojs_snprintf(b, 16, "%d%d%d%d", (int)(yy / 1000), (int)(yy / 100 % 10), (int)(yy / 10 % 10), (int)(yy % 10));
        long long a = yy < 0 ? -yy : yy;
        return ojs_snprintf(b, 16, "%c%d%d%d%d%d%d", yy < 0 ? '-' : '+', (int)(a / 100000 % 10), (int)(a / 10000 % 10),
                            (int)(a / 1000 % 10), (int)(a / 100 % 10), (int)(a / 10 % 10), (int)(a % 10));
    }
    if (yy < 0) {
        long long a = -yy;
        if (a < 1000) return ojs_snprintf(b, 16, "-%d%d%d%d", (int)(a / 1000 % 10), (int)(a / 100 % 10), (int)(a / 10 % 10), (int)(a % 10));
        return ojs_snprintf(b, 16, "-%d", (int)a);
    }
    if (yy < 1000) return ojs_snprintf(b, 16, "%d%d%d%d", (int)(yy / 1000 % 10), (int)(yy / 100 % 10), (int)(yy / 10 % 10), (int)(yy % 10));
    return ojs_snprintf(b, 16, "%d", (int)yy);
}

static int two(char* b, int v) { b[0] = (char)('0' + v / 10 % 10); b[1] = (char)('0' + v % 10); return 2; }

static int fmt_date(char* b, const struct fields* f) {   // "Tue Mar 01 2022"
    int p = ojs_snprintf(b, 32, "%s %s ", WD[f->wday], MN[f->mon]);
    p += two(b + p, f->day);
    b[p++] = ' ';
    p += put_year(b + p, f->y, 0);
    return p;
}

static int fmt_time(char* b, const struct fields* f) {   // "10:00:00"
    int p = two(b, f->h);
    b[p++] = ':';
    p += two(b + p, f->m);
    b[p++] = ':';
    p += two(b + p, f->s);
    return p;
}

static int fmt_tz(char* b, double t) {   // " GMT+0100"
    int off = (int)(local_offset(t, 0) / 60000.0);
    int a = off < 0 ? -off : off;
    int p = ojs_snprintf(b, 16, " GMT%c", off < 0 ? '-' : '+');
    p += two(b + p, a / 60);
    p += two(b + p, a % 60);
    return p;
}

static jv cstr(ojs* J, const char* b, int n) {
    struct str* s = str_new8(J, (const uint8_t*)b, (uint32_t)n);
    return s ? jv_from_str(s) : JV_EXC;
}

// kinds: 0 toString, 1 toDateString, 2 toTimeString, 3 toISOString, 4 toUTCString,
// 5 toLocaleString, 6 toLocaleDateString, 7 toLocaleTimeString
static jv date_format(ojs* J, double t, int kind) {
    char b[96];
    int p = 0;
    if (t != t) {
        if (kind == 3) return throw_range(J, "Invalid time value");
        return str_value(J, "Invalid Date");
    }
    struct fields f;
    if (kind == 3 || kind == 4) split(t, &f);
    else split(local_time(t), &f);
    switch (kind) {
    case 0: case 5:
        p = fmt_date(b, &f);
        b[p++] = ' ';
        p += fmt_time(b + p, &f);
        p += fmt_tz(b + p, t);
        if (kind == 0) { memcpy(b + p, " (Coordinated Universal Time)", 30); p += 29; if (local_offset(t, 0) != 0) p -= 29; }
        break;
    case 1: case 6: p = fmt_date(b, &f); break;
    case 2: case 7:
        p = fmt_time(b, &f);
        p += fmt_tz(b + p, t);
        if (kind == 2 && local_offset(t, 0) == 0) { memcpy(b + p, " (Coordinated Universal Time)", 30); p += 29; }
        break;
    case 3:
        p = put_year(b, f.y, 1);
        b[p++] = '-'; p += two(b + p, f.mon + 1);
        b[p++] = '-'; p += two(b + p, f.day);
        b[p++] = 'T'; p += fmt_time(b + p, &f);
        b[p++] = '.';
        b[p++] = (char)('0' + f.ms / 100); b[p++] = (char)('0' + f.ms / 10 % 10); b[p++] = (char)('0' + f.ms % 10);
        b[p++] = 'Z';
        break;
    case 4:
        p = ojs_snprintf(b, 32, "%s, ", WD[f.wday]);
        p += two(b + p, f.day);
        p += ojs_snprintf(b + p, 16, " %s ", MN[f.mon]);
        p += put_year(b + p, f.y, 0);
        b[p++] = ' ';
        p += fmt_time(b + p, &f);
        memcpy(b + p, " GMT", 4);
        p += 4;
        break;
    }
    return cstr(J, b, p);
}

// ---------------------------------------------------------------- parsing

struct dp { const struct str* s; uint32_t i, n; };
static uint32_t dc(struct dp* p) { return p->i < p->n ? str_at(p->s, p->i) : 0; }

static int digits(struct dp* p, int min, int max, long long* out) {
    long long v = 0;
    int k = 0;
    while (k < max && dc(p) >= '0' && dc(p) <= '9') { v = v * 10 + (dc(p) - '0'); p->i++; k++; }
    if (k < min) return 0;
    *out = v;
    return k;
}

// the ISO 8601 subset of §21.4.1.32 (Date Time String Format)
static double parse_iso(const struct str* s) {
    struct dp p = { s, 0, str_len(s) };
    long long y, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0;
    int sign = 1;
    if (dc(&p) == '+' || dc(&p) == '-') {
        sign = dc(&p) == '-' ? -1 : 1;
        p.i++;
        if (digits(&p, 6, 6, &y) != 6) return NANV;
        if (sign < 0 && y == 0) return NANV;   // -000000 is invalid
    } else if (digits(&p, 4, 4, &y) != 4) return NANV;
    int date_only = 1, has_tz = 0;
    double tz = 0;
    if (dc(&p) == '-') {
        p.i++;
        if (digits(&p, 2, 2, &mo) != 2 || mo < 1 || mo > 12) return NANV;
        if (dc(&p) == '-') {
            p.i++;
            if (digits(&p, 2, 2, &d) != 2 || d < 1 || d > 31) return NANV;
        }
    }
    if (dc(&p) == 'T') {
        p.i++;
        date_only = 0;
        if (digits(&p, 2, 2, &h) != 2 || dc(&p) != ':') return NANV;
        p.i++;
        if (digits(&p, 2, 2, &mi) != 2) return NANV;
        if (dc(&p) == ':') {
            p.i++;
            if (digits(&p, 2, 2, &sec) != 2) return NANV;
            if (dc(&p) == '.') {
                p.i++;
                long long frac;
                int k = digits(&p, 1, 9, &frac);
                if (!k) return NANV;
                while (k > 3) { frac /= 10; k--; }
                while (k < 3) { frac *= 10; k++; }
                ms = frac;
                while (dc(&p) >= '0' && dc(&p) <= '9') p.i++;
            }
        }
        if (h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms))) return NANV;
        if (dc(&p) == 'Z') { p.i++; has_tz = 1; }
        else if (dc(&p) == '+' || dc(&p) == '-') {
            int ts = dc(&p) == '-' ? -1 : 1;
            p.i++;
            long long th, tm;
            if (digits(&p, 2, 2, &th) != 2 || dc(&p) != ':') return NANV;
            p.i++;
            if (digits(&p, 2, 2, &tm) != 2 || th > 23 || tm > 59) return NANV;
            tz = ts * (th * 60 + tm) * 60000.0;
            has_tz = 1;
        }
    } else if (dc(&p) == 'Z') {
        p.i++;
        has_tz = 1;
    }
    if (p.i != p.n) return NANV;
    double yy = (double)(sign * y);
    // day must exist in the month
    int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = ((long long)yy % 4 == 0 && (long long)yy % 100 != 0) || (long long)yy % 400 == 0;
    if (d > mdays[mo - 1] + (mo == 2 && leap)) return NANV;
    double day = make_day(yy, (double)(mo - 1), (double)d);
    double t = make_date(day, make_time((double)h, (double)mi, (double)sec, (double)ms));
    if (date_only || has_tz) t -= tz;
    else t = utc_of(t);
    return time_clip(t);
}

static int month_of(const struct str* s, uint32_t i) {
    for (int m = 0; m < 12; m++) {
        int ok = 1;
        for (int k = 0; k < 3; k++) if (i + (uint32_t)k >= str_len(s) || (str_at(s, i + (uint32_t)k) | 0x20) != (uint32_t)(MN[m][k] | 0x20)) { ok = 0; break; }
        if (ok) return m;
    }
    return -1;
}

// the formats toString / toUTCString produce, plus common variants
// ("Mar 1 2022", "1 March 2022 10:00", "2022/03/01 10:00:00 GMT+0100", ...)
static double parse_legacy(const struct str* s) {
    struct dp p = { s, 0, str_len(s) };
    double y = NANV;
    int mon = -1;
    long long day = -1, h = 0, mi = 0, sec = 0, nums[3];
    int nn = 0;
    int has_tz = 0, pm = -1;
    double tz = 0;
    while (p.i < p.n) {
        uint32_t c = dc(&p);
        if (c == ' ' || c == ',' || c == '\t' || c == '\n') { p.i++; continue; }
        if (c == '(') {   // comment
            while (p.i < p.n && dc(&p) != ')') p.i++;
            p.i++;
            continue;
        }
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            uint32_t st = p.i;
            while (p.i < p.n && ((dc(&p) | 0x20) >= 'a' && (dc(&p) | 0x20) <= 'z')) p.i++;
            uint32_t len = p.i - st;
            int m = month_of(s, st);
            if (m >= 0 && len >= 3) { mon = m; continue; }
            if (len == 2 && (str_at(s, st) | 0x20) == 'a' && (str_at(s, st + 1) | 0x20) == 'm') { pm = 0; continue; }
            if (len == 2 && (str_at(s, st) | 0x20) == 'p' && (str_at(s, st + 1) | 0x20) == 'm') { pm = 1; continue; }
            if ((len == 3 && (str_at(s, st) | 0x20) == 'g' && (str_at(s, st + 1) | 0x20) == 'm' && (str_at(s, st + 2) | 0x20) == 't') ||
                (len == 3 && (str_at(s, st) | 0x20) == 'u' && (str_at(s, st + 1) | 0x20) == 't' && (str_at(s, st + 2) | 0x20) == 'c') ||
                (len == 1 && (str_at(s, st) | 0x20) == 'z')) {
                has_tz = 1;
                continue;
            }
            // weekday names and unknown words are ignored
            continue;
        }
        if ((c == '+' || c == '-') && has_tz) {
            int ts = c == '-' ? -1 : 1;
            p.i++;
            long long v = 0;
            int k = digits(&p, 1, 4, &v);
            if (!k) return NANV;
            long long hh = k <= 2 ? v : v / 100, mm = k <= 2 ? 0 : v % 100;
            if (dc(&p) == ':') { p.i++; long long m2; if (!digits(&p, 2, 2, &m2)) return NANV; hh = v; mm = m2; }
            tz = ts * (double)(hh * 60 + mm) * 60000.0;
            continue;
        }
        if (c >= '0' && c <= '9') {
            long long v = 0;
            digits(&p, 1, 9, &v);
            if (dc(&p) == ':') {   // time
                p.i++;
                h = v;
                if (!digits(&p, 2, 2, &mi)) return NANV;
                if (dc(&p) == ':') { p.i++; if (!digits(&p, 2, 2, &sec)) return NANV; }
                if (dc(&p) == '.') { p.i++; long long f; digits(&p, 1, 9, &f); }
                continue;
            }
            if (dc(&p) == '/' || dc(&p) == '-') {   // numeric date: m/d/y or y/m/d
                p.i++;
                long long b2, c3 = -1;
                if (!digits(&p, 1, 4, &b2)) return NANV;
                if (dc(&p) == '/' || dc(&p) == '-') { p.i++; if (!digits(&p, 1, 6, &c3)) return NANV; }
                if (v > 31) { y = (double)v; mon = (int)b2 - 1; day = c3 < 0 ? 1 : c3; }
                else { mon = (int)v - 1; day = b2; if (c3 >= 0) y = (double)(c3 < 100 ? c3 + (c3 < 50 ? 2000 : 1900) : c3); }
                continue;
            }
            if (nn < 3) nums[nn++] = v;
            continue;
        }
        if (c == '-' || c == '+') {   // signed year
            int sg = c == '-' ? -1 : 1;
            p.i++;
            long long v = 0;
            if (!digits(&p, 1, 6, &v)) return NANV;
            y = (double)(sg * v);
            continue;
        }
        p.i++;
    }
    for (int i = 0; i < nn; i++) {
        long long v = nums[i];
        if (day < 0 && v >= 1 && v <= 31 && (mon >= 0 || i == 0)) day = v;
        else if (y != y) y = (double)(v < 100 && nn > 1 ? v + (v < 50 ? 2000 : 1900) : v);
        else if (day < 0) day = v;
    }
    if (y != y || mon < 0) return NANV;
    if (day < 0) day = 1;
    if (pm == 1 && h < 12) h += 12;
    if (pm == 0 && h == 12) h = 0;
    if (h > 24 || mi > 59 || sec > 59 || day > 31) return NANV;
    double t = make_date(make_day(y, mon, (double)day), make_time((double)h, (double)mi, (double)sec, 0));
    if (has_tz) t -= tz;
    else t = utc_of(t);
    return time_clip(t);
}

double date_parse_str(const struct str* s) {
    double t = parse_iso(s);
    if (t == t) return t;
    return parse_legacy(s);
}

// ---------------------------------------------------------------- constructor & statics

static double now(void) { return km_floor(ojs_sys_time_ms()); }

static jv date_ctor(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv nt = J->native_new_target;
    if (jv_is_undef(nt)) return date_format(J, now(), 0);
    double tv;
    if (argc == 0) tv = now();
    else if (argc == 1) {
        jv v = argv[0];
        if (jv_is_obj(v) && obj_class(jv_obj(v)) == OC_DATE) tv = ((struct dateobj*)jv_obj(v))->t;
        else {
            jv p = to_primitive(J, v, 0);
            if (p == JV_EXC) return JV_EXC;
            if (jv_is_str(p)) {
                struct str* s = str_flat(J, p);
                if (!s) return JV_EXC;
                tv = date_parse_str(s);
            } else if (to_number_d(J, p, &tv) < 0) return JV_EXC;
        }
        tv = time_clip(tv);
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 7 && i < argc; i++) if (to_number_d(J, argv[i], &f[i]) < 0) return JV_EXC;
        if (is_finite(f[0])) { double yi = to_integer(f[0]); if (yi >= 0 && yi <= 99) f[0] = 1900 + yi; }
        tv = time_clip(utc_of(make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
    }
    jv o = ordinary_create_from_ctor(J, nt, J->I.date_proto, OC_DATE, sizeof(struct dateobj));
    if (o == JV_EXC) return JV_EXC;
    ((struct dateobj*)jv_obj(o))->t = tv;
    return o;
}

static jv date_now(ojs* J, jv this_v, int argc, jv* argv, int magic) { return jv_number(now()); }

static jv date_parse(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct str* s = to_str(J, argv[0]);
    if (!s) return JV_EXC;
    return jv_number(date_parse_str(s));
}

static jv date_utc(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    double f[7] = { NANV, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < 7 && i < argc; i++) if (to_number_d(J, argv[i], &f[i]) < 0) return JV_EXC;
    if (is_finite(f[0])) { double yi = to_integer(f[0]); if (yi >= 0 && yi <= 99) f[0] = 1900 + yi; }
    return jv_number(time_clip(make_date(make_day(f[0], f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
}

// ---------------------------------------------------------------- getters

enum { G_YEAR, G_MONTH, G_DATE, G_DAY, G_HOURS, G_MINUTES, G_SECONDS, G_MS, G_TZ, G_TIME, G_YEAR2 };

// magic: field | 0x10 for UTC
static jv date_get(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct dateobj* d;
    if (this_date(J, this_v, &d) < 0) return JV_EXC;
    double t = d->t;
    int field = magic & 15;
    if (field == G_TIME) return jv_number(t);
    if (t != t) return jv_from_dbl(NANV);
    if (field == G_TZ) return jv_number((t - local_time(t)) / 60000.0);
    struct fields f;
    split((magic & 0x10) ? t : local_time(t), &f);
    switch (field) {
    case G_YEAR: return jv_number(f.y);
    case G_YEAR2: return jv_number(f.y - 1900);
    case G_MONTH: return jv_from_int(f.mon);
    case G_DATE: return jv_from_int(f.day);
    case G_DAY: return jv_from_int(f.wday);
    case G_HOURS: return jv_from_int(f.h);
    case G_MINUTES: return jv_from_int(f.m);
    case G_SECONDS: return jv_from_int(f.s);
    default: return jv_from_int(f.ms);
    }
}

// ---------------------------------------------------------------- setters

enum { S_MS, S_SECONDS, S_MINUTES, S_HOURS, S_DATE, S_MONTH, S_FULLYEAR, S_TIME, S_YEAR };

static jv date_set(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct dateobj* d;
    if (this_date(J, this_v, &d) < 0) return JV_EXC;
    int utc = (magic & 0x10) != 0, field = magic & 15;
    if (field == S_TIME) {
        double v;
        if (to_number_d(J, argv[0], &v) < 0) return JV_EXC;
        d->t = time_clip(v);
        return jv_number(d->t);
    }
    double t = d->t;
    // read all arguments first (they may have side effects)
    double a[4];
    int na = 0;
    int maxa = field == S_MS ? 1 : field == S_SECONDS ? 2 : field == S_MINUTES ? 3 : field == S_HOURS ? 4 :
               field == S_DATE ? 1 : field == S_MONTH ? 2 : field == S_FULLYEAR ? 3 : 1;
    for (int i = 0; i < maxa && (i == 0 || i < argc); i++) {
        if (to_number_d(J, argv[i], &a[i]) < 0) return JV_EXC;
        na++;
    }
    if (field == S_YEAR) {
        double y = a[0];
        if (y != y) { d->t = NANV; return jv_from_dbl(NANV); }
        double yi = to_integer(y);
        if (yi >= 0 && yi <= 99) yi += 1900;
        double lt = t != t ? 0 : local_time(t);
        struct fields f;
        split(lt, &f);
        double nd = make_date(make_day(yi, f.mon, f.day), time_within_day(lt));
        d->t = time_clip(utc_of(nd));
        return jv_number(d->t);
    }
    if (t != t) {
        if (field != S_FULLYEAR) return jv_from_dbl(NANV);
        t = 0;   // setFullYear on an invalid date starts from +0
        if (!utc) t = utc_of(0) == utc_of(0) ? 0 : 0;
    }
    double lt = utc ? t : local_time(t);
    if (d->t != d->t && field == S_FULLYEAR) lt = 0;
    struct fields f;
    split(lt, &f);
    double y = f.y, mon = f.mon, day = f.day, h = f.h, mi = f.m, s = f.s, ms = f.ms;
    switch (field) {
    case S_MS: ms = a[0]; break;
    case S_SECONDS: s = a[0]; if (na > 1) ms = a[1]; break;
    case S_MINUTES: mi = a[0]; if (na > 1) s = a[1]; if (na > 2) ms = a[2]; break;
    case S_HOURS: h = a[0]; if (na > 1) mi = a[1]; if (na > 2) s = a[2]; if (na > 3) ms = a[3]; break;
    case S_DATE: day = a[0]; break;
    case S_MONTH: mon = a[0]; if (na > 1) day = a[1]; break;
    case S_FULLYEAR: y = a[0]; if (na > 1) mon = a[1]; if (na > 2) day = a[2]; break;
    }
    double nd = make_date(make_day(y, mon, day), make_time(h, mi, s, ms));
    d->t = time_clip(utc ? nd : utc_of(nd));
    return jv_number(d->t);
}

// ---------------------------------------------------------------- conversions

static jv date_to_string(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct dateobj* d;
    if (this_date(J, this_v, &d) < 0) return JV_EXC;
    return date_format(J, d->t, magic);
}

static jv date_value_of(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct dateobj* d;
    if (this_date(J, this_v, &d) < 0) return JV_EXC;
    return jv_number(d->t);
}

static jv date_to_json(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    jv o = to_object(J, this_v);
    if (o == JV_EXC) return JV_EXC;
    jv tv = to_primitive(J, o, 1);
    if (tv == JV_EXC) return JV_EXC;
    if (jv_is_number(tv) && !is_finite(jv_num(tv))) return JV_NULL;
    return invoke(J, o, A(toISOString), 0, 0);
}

static jv date_to_primitive(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    if (!jv_is_obj(this_v)) return throw_type(J, "Date.prototype[Symbol.toPrimitive] called on non-object");
    jv h = argv[0];
    int hint;
    if (jv_is_str(h) && str_eq_ascii(str_flat(J, h), "string")) hint = 2;
    else if (jv_is_str(h) && str_eq_ascii(str_flat(J, h), "default")) hint = 2;
    else if (jv_is_str(h) && str_eq_ascii(str_flat(J, h), "number")) hint = 1;
    else return throw_type(J, "Invalid hint");
    // OrdinaryToPrimitive
    pkey order[2] = { hint == 2 ? A(toString) : A(valueOf), hint == 2 ? A(valueOf) : A(toString) };
    for (int i = 0; i < 2; i++) {
        jv f = obj_get(J, jv_obj(this_v), order[i], this_v);
        if (f == JV_EXC) return JV_EXC;
        if (is_callable(f)) {
            jv r = ojs_call_v(J, f, this_v, 0, 0);
            if (r == JV_EXC) return JV_EXC;
            if (!jv_is_obj(r)) return r;
        }
    }
    return throw_type(J, "Cannot convert object to primitive value");
}

static const struct bdef date_statics[] = {
    FN("now", date_now, 0, 0),
    FN("parse", date_parse, 1, 0),
    FN("UTC", date_utc, 7, 0),
};

static const struct bdef date_proto_fns[] = {
    FN("getDate", date_get, 0, G_DATE), FN("getDay", date_get, 0, G_DAY), FN("getFullYear", date_get, 0, G_YEAR),
    FN("getHours", date_get, 0, G_HOURS), FN("getMilliseconds", date_get, 0, G_MS), FN("getMinutes", date_get, 0, G_MINUTES),
    FN("getMonth", date_get, 0, G_MONTH), FN("getSeconds", date_get, 0, G_SECONDS), FN("getTime", date_get, 0, G_TIME),
    FN("getTimezoneOffset", date_get, 0, G_TZ), FN("getUTCDate", date_get, 0, 0x10 | G_DATE),
    FN("getUTCDay", date_get, 0, 0x10 | G_DAY), FN("getUTCFullYear", date_get, 0, 0x10 | G_YEAR),
    FN("getUTCHours", date_get, 0, 0x10 | G_HOURS), FN("getUTCMilliseconds", date_get, 0, 0x10 | G_MS),
    FN("getUTCMinutes", date_get, 0, 0x10 | G_MINUTES), FN("getUTCMonth", date_get, 0, 0x10 | G_MONTH),
    FN("getUTCSeconds", date_get, 0, 0x10 | G_SECONDS), FN("getYear", date_get, 0, G_YEAR2),
    FN("setDate", date_set, 1, S_DATE), FN("setFullYear", date_set, 3, S_FULLYEAR), FN("setHours", date_set, 4, S_HOURS),
    FN("setMilliseconds", date_set, 1, S_MS), FN("setMinutes", date_set, 3, S_MINUTES), FN("setMonth", date_set, 2, S_MONTH),
    FN("setSeconds", date_set, 2, S_SECONDS), FN("setTime", date_set, 1, S_TIME),
    FN("setUTCDate", date_set, 1, 0x10 | S_DATE), FN("setUTCFullYear", date_set, 3, 0x10 | S_FULLYEAR),
    FN("setUTCHours", date_set, 4, 0x10 | S_HOURS), FN("setUTCMilliseconds", date_set, 1, 0x10 | S_MS),
    FN("setUTCMinutes", date_set, 3, 0x10 | S_MINUTES), FN("setUTCMonth", date_set, 2, 0x10 | S_MONTH),
    FN("setUTCSeconds", date_set, 2, 0x10 | S_SECONDS), FN("setYear", date_set, 1, S_YEAR),
    FN("toDateString", date_to_string, 0, 1), FN("toISOString", date_to_string, 0, 3), FN("toJSON", date_to_json, 1, 0),
    FN("toLocaleDateString", date_to_string, 0, 6), FN("toLocaleString", date_to_string, 0, 5),
    FN("toLocaleTimeString", date_to_string, 0, 7), FN("toString", date_to_string, 0, 0),
    FN("toTimeString", date_to_string, 0, 2), FN("valueOf", date_value_of, 0, 0),
};

int b_date_init(ojs* J) {
    struct obj* p = obj_new(J, J->I.object_proto, OC_OBJECT, 0);
    if (!p) return -1;
    J->I.date_proto = p;
    struct obj* c = def_ctor(J, date_ctor, "Date", 7, 0, p);
    if (!c) return -1;
    J->I.date_ctor = c;
    if (DEF_FNS(c, date_statics) < 0 || DEF_FNS(p, date_proto_fns) < 0) return -1;
    struct obj* utc = new_native(J, date_to_string, "toUTCString", 0, 4);
    if (!utc) return -1;
    if (def_value(J, p, "toUTCString", jv_from_obj(utc), PA_HIDDEN) < 0 || def_value(J, p, "toGMTString", jv_from_obj(utc), PA_HIDDEN) < 0) return -1;
    struct obj* tp = new_native(J, date_to_primitive, "[Symbol.toPrimitive]", 1, 0);
    if (!tp || obj_define_value(J, p, pk_from_sym(J->wk[WK_TO_PRIMITIVE]), jv_from_obj(tp), PA_CONFIGURABLE) < 0) return -1;
    return 0;
}
