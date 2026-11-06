// ALSA's kernel side for the Linux subsystem: /dev/snd/controlC0 and /dev/snd/pcmC0D0p, on the machine's sound card (audio.h).
// One playback stream, in the card's one format (48 kHz, 16 bit, stereo): real alsa-lib negotiates down to that through
// HW_REFINE and programs (or SDL) convert. RW access only; the status pages cannot be mapped, alsa-lib then uses SYNC_PTR.
// The card always runs, so a stream never "underruns": when nothing is queued the hardware pointer simply meets the
// application's, and a later write carries on. The interface is written from the kernel's uapi header layouts.
#include "lx.h"
#include "audio.h"
#include "serial.h"
#include "string.h"

#define LX_EBADFD 77
#define PCM_VERSION 0x0002000B     // 2.0.11: older than the protocol extras (STATUS_EXT, USER_PVERSION)
#define CTL_VERSION 0x00020007

enum { ST_OPEN, ST_SETUP, ST_PREPARED, ST_RUNNING, ST_XRUN, ST_DRAINING, ST_PAUSED };

// ioctl numbers on x86-64
#define CTL_PVERSION      0x80045500u
#define CTL_CARD_INFO     0x81785501u
#define CTL_ELEM_LIST     0xC0505510u
#define CTL_SUBSCRIBE     0xC0045516u
#define CTL_PCM_NEXT      0x80045530u
#define CTL_PCM_INFO      0xC1205531u
#define CTL_PCM_PREFER    0x40045532u
#define PCM_PVERSION      0x80044100u
#define PCM_INFO          0x81204101u
#define PCM_TSTAMP        0x40044102u
#define PCM_TTSTAMP       0x40044103u
#define PCM_HW_REFINE     0xC2604110u
#define PCM_HW_PARAMS     0xC2604111u
#define PCM_HW_FREE       0x00004112u
#define PCM_SW_PARAMS     0xC0884113u
#define PCM_STATUS        0x80984120u
#define PCM_DELAY         0x80084121u
#define PCM_HWSYNC        0x00004122u
#define PCM_SYNC_PTR      0xC0884123u
#define PCM_PREPARE       0x00004140u
#define PCM_RESET         0x00004141u
#define PCM_START         0x00004142u
#define PCM_DROP          0x00004143u
#define PCM_DRAIN         0x00004144u
#define PCM_REWIND        0x40084146u
#define PCM_WRITEI        0x40184150u
#define PCM_FORWARD       0x40084149u

#define HWP_SIZE 608
#define MASK_OFF(i) (4 + (i) * 32)         // ACCESS 0, FORMAT 1, SUBFORMAT 2
#define IV_OFF(i) (260 + (i) * 12)         // SAMPLE_BITS 0, FRAME_BITS 1, CHANNELS 2, RATE 3, PERIOD_TIME 4, PERIOD_SIZE 5, PERIOD_BYTES 6, PERIODS 7, BUFFER_TIME 8, BUFFER_SIZE 9, BUFFER_BYTES 10, TICK_TIME 11
enum { I_SAMPLE, I_FRAME, I_CHANNELS, I_RATE, I_PTIME, I_PSIZE, I_PBYTES, I_PERIODS, I_BTIME, I_BSIZE, I_BBYTES, I_TICK, I_COUNT };
struct iv { uint32_t min, max, flags; };   // flags: 1 openmin, 2 openmax, 4 integer, 8 empty
#define OFF_CMASK 516
#define OFF_INFO 520
#define OFF_MSBITS 524
#define OFF_RATE_NUM 528
#define OFF_RATE_DEN 532

#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE 2
#define INFO_INTERLEAVED 0x100u
#define RATE 48000u
#define MIN_PERIOD 128u
#define MAX_PERIOD 16384u
#define MIN_BUFFER 512u
#define MAX_BUFFER 32768u               // the card's ring holds 128 KB

static struct {
    int open;
    int state;
    uint32_t period, buffer;
    uint64_t appl, hw;                  // frames, never wrapping
    uint64_t avail_min;
    uint32_t underruns;
} P;

int lx_snd_present(void) { return audio_ready(); }

static uint64_t update_hw(void) {
    uint64_t q = audio_queued();
    uint64_t h = P.appl > q ? P.appl - q : 0;
    if (h > P.hw) P.hw = h;
    return P.hw;
}

static uint32_t avail_frames(void) {
    update_hw();
    uint64_t pending = P.appl - P.hw;
    return pending >= P.buffer ? 0 : (uint32_t)(P.buffer - pending);
}

// ---- hw params ------------------------------------------------------------------------------------------------------

static int iv_load(const uint8_t *b, int i, struct iv *v) {
    memcpy(v, b + IV_OFF(i), sizeof *v);
    if (v->flags & 8) return -1;
    if (v->flags & 1) { v->min++; v->flags &= ~1u; }           // every value here is an integer
    if ((v->flags & 2) && v->max) { v->max--; v->flags &= ~2u; }
    return v->min > v->max ? -1 : 0;
}

static void clampv(struct iv *v, uint64_t lo, uint64_t hi) {
    if (hi > 0xFFFFFFFFull) hi = 0xFFFFFFFFull;
    if (v->min < lo) v->min = (uint32_t)lo;
    if (v->max > hi) v->max = (uint32_t)hi;
}

static uint64_t us_down(uint64_t frames) { return frames * 1000000ull / RATE; }
static uint64_t us_up(uint64_t frames) { return (frames * 1000000ull + RATE - 1) / RATE; }
static uint64_t fr_down(uint64_t us) { return us * RATE / 1000000ull; }
static uint64_t fr_up(uint64_t us) { return (us * RATE + 999999ull) / 1000000ull; }

// Narrows everything to what the card can do, until nothing changes. -1 = nothing left.
static int refine(uint8_t *b) {
    uint32_t want[3][1] = { { 1u << ACCESS_RW_INTERLEAVED }, { 1u << FORMAT_S16_LE }, { 1u } };
    uint32_t changed = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t m[8], n[8];
        memcpy(m, b + MASK_OFF(i), 32);
        memset(n, 0, 32);
        n[0] = m[0] & want[i][0];
        if (!n[0]) return -1;
        if (memcmp(m, n, 32)) { memcpy(b + MASK_OFF(i), n, 32); changed |= 1u << i; }
    }
    struct iv v[I_COUNT], o[I_COUNT];
    for (int i = 0; i < I_COUNT; i++) { if (iv_load(b, i, &v[i])) return -1; memcpy(&o[i], b + IV_OFF(i), sizeof o[i]); }
    clampv(&v[I_SAMPLE], 16, 16); clampv(&v[I_FRAME], 32, 32); clampv(&v[I_CHANNELS], 2, 2); clampv(&v[I_RATE], RATE, RATE);
    clampv(&v[I_PSIZE], MIN_PERIOD, MAX_PERIOD); clampv(&v[I_BSIZE], MIN_BUFFER, MAX_BUFFER);
    clampv(&v[I_PERIODS], 2, 128);
    v[I_TICK].min = 0; v[I_TICK].max = 0;
    for (int pass = 0; pass < 8; pass++) {
        clampv(&v[I_PSIZE], v[I_PBYTES].min / 4, v[I_PBYTES].max / 4);
        clampv(&v[I_PBYTES], (uint64_t)v[I_PSIZE].min * 4, (uint64_t)v[I_PSIZE].max * 4);
        clampv(&v[I_BSIZE], v[I_BBYTES].min / 4, v[I_BBYTES].max / 4);
        clampv(&v[I_BBYTES], (uint64_t)v[I_BSIZE].min * 4, (uint64_t)v[I_BSIZE].max * 4);
        clampv(&v[I_PSIZE], fr_down(v[I_PTIME].min), fr_up(v[I_PTIME].max));
        clampv(&v[I_PTIME], us_down(v[I_PSIZE].min), us_up(v[I_PSIZE].max));
        clampv(&v[I_BSIZE], fr_down(v[I_BTIME].min), fr_up(v[I_BTIME].max));
        clampv(&v[I_BTIME], us_down(v[I_BSIZE].min), us_up(v[I_BSIZE].max));
        clampv(&v[I_BSIZE], (uint64_t)v[I_PSIZE].min * v[I_PERIODS].min, (uint64_t)v[I_PSIZE].max * v[I_PERIODS].max);
        if (v[I_PSIZE].min && v[I_PERIODS].max) {
            clampv(&v[I_PSIZE], v[I_BSIZE].min / v[I_PERIODS].max, v[I_BSIZE].max / v[I_PERIODS].min);
            clampv(&v[I_PERIODS], v[I_BSIZE].min / v[I_PSIZE].max, v[I_BSIZE].max / v[I_PSIZE].min);
        }
        for (int i = 0; i < I_COUNT; i++) if (v[i].min > v[i].max) return -1;
    }
    for (int i = 0; i < I_COUNT; i++) {
        v[i].flags = 4;
        if (i == I_PTIME || i == I_BTIME) v[i].flags = 0;
        if (o[i].min != v[i].min || o[i].max != v[i].max) changed |= 1u << (i + 8);
        memcpy(b + IV_OFF(i), &v[i], sizeof v[i]);
    }
    uint32_t info = INFO_INTERLEAVED, msbits = 16, num = RATE, den = 1;
    memcpy(b + OFF_CMASK, &changed, 4);
    memcpy(b + OFF_INFO, &info, 4);
    memcpy(b + OFF_MSBITS, &msbits, 4);
    memcpy(b + OFF_RATE_NUM, &num, 4);
    memcpy(b + OFF_RATE_DEN, &den, 4);
    return 0;
}

static void pin(uint8_t *b, int i, uint32_t val) {
    struct iv v;
    memcpy(&v, b + IV_OFF(i), sizeof v);
    v.min = v.max = val; v.flags = i == I_PTIME || i == I_BTIME ? 0 : 4;
    memcpy(b + IV_OFF(i), &v, sizeof v);
}

static int64_t hw_params(struct lx_task *t, uint64_t arg, int commit) {
    static uint8_t b[HWP_SIZE];
    if (lx_copy_in(t, b, arg, HWP_SIZE)) return -LX_EFAULT;
    if (refine(b)) return -LX_EINVAL;
    if (commit) {
        struct iv ps, bs;
        memcpy(&ps, b + IV_OFF(I_PSIZE), sizeof ps);
        memcpy(&bs, b + IV_OFF(I_BSIZE), sizeof bs);
        uint32_t period = ps.min, buffer = bs.max;                         // what is still open: small periods, a big buffer
        if (buffer < period * 2) buffer = period * 2;
        buffer -= buffer % period;
        pin(b, I_PSIZE, period); pin(b, I_PBYTES, period * 4); pin(b, I_PTIME, (uint32_t)us_up(period));
        pin(b, I_BSIZE, buffer); pin(b, I_BBYTES, buffer * 4); pin(b, I_BTIME, (uint32_t)us_up(buffer));
        pin(b, I_PERIODS, buffer / period);
        P.period = period; P.buffer = buffer;
        P.appl = P.hw = 0;
        P.avail_min = period;
        P.state = ST_PREPARED;
        serial_printf("[snd] stream: period %u buffer %u frames\n", period, buffer);
    }
    return lx_copy_out(t, arg, b, HWP_SIZE) ? -LX_EFAULT : 0;
}

// ---- transfer -------------------------------------------------------------------------------------------------------

static int wait_space(struct lx_task *t) { (void)t; return !P.open || avail_frames() > 0; }
static int wait_drained(struct lx_task *t) { (void)t; return !P.open || audio_queued() == 0; }

static int64_t writei(struct lx_task *t, struct lx_file *f, uint64_t arg) {
    struct { int64_t result; uint64_t buf; uint64_t frames; } x;
    static int16_t tmp[2 * 2048];
    if (lx_copy_in(t, &x, arg, sizeof x)) return -LX_EFAULT;
    if (P.state < ST_PREPARED) return -LX_EBADFD;
    uint64_t done = 0;
    if (P.state == ST_RUNNING && x.frames && audio_queued() == 0) {         // the card ran dry before this write
        extern uint32_t tick_count;
        static uint32_t said_at;
        P.underruns++;
        if (tick_count - said_at >= 100) { said_at = tick_count; serial_printf("[snd] underruns=%u\n", P.underruns); }
    }
    while (done < x.frames) {
        uint32_t room = avail_frames();
        uint32_t k = audio_space();
        if (k > room) k = room;
        if (k > 2048) k = 2048;
        if ((uint64_t)k > x.frames - done) k = (uint32_t)(x.frames - done);
        if (!k) break;
        if (lx_copy_in(t, tmp, x.buf + done * 4, (uint64_t)k * 4)) return done ? 0 : -LX_EFAULT;
        k = audio_write(tmp, k);
        if (!k) break;
        P.appl += k; done += k;
        P.state = ST_RUNNING;
    }
    if (!done && x.frames) {
        if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
        lx_block(t, wait_space, 0);
        return 0;
    }
    x.result = (int64_t)done;
    return lx_copy_out(t, arg, &x, sizeof x) ? -LX_EFAULT : 0;
}

// ---- files ----------------------------------------------------------------------------------------------------------

int lx_snd_open_pcm(void) {
    if (!audio_ready()) return -LX_ENODEV;
    if (P.open) return -LX_EBUSY;
    memset(&P, 0, sizeof P);
    P.open = 1; P.state = ST_OPEN;
    return 0;
}

void lx_snd_close_pcm(void) {
    if (P.open && P.state >= ST_PREPARED && P.appl) audio_flush();    // closing stops the stream; a program that wants the end heard drains first
    P.open = 0;
}

int lx_snd_pcm_events(void) {            // poll(): writable when a period fits
    if (!P.open || P.state < ST_PREPARED) return 0;
    uint32_t a = avail_frames();
    return a && (uint64_t)a >= P.avail_min;
}

static void fill_pcm_info(uint8_t *b) {   // struct snd_pcm_info, 288 bytes
    uint32_t dev, sub; int stream;
    memcpy(&dev, b, 4); memcpy(&sub, b + 4, 4); memcpy(&stream, b + 8, 4);
    memset(b, 0, 288);
    memcpy(b, &dev, 4); memcpy(b + 4, &sub, 4); memcpy(b + 8, &stream, 4);
    memcpy(b + 16, "okernel", 7);
    memcpy(b + 80, "okernel sound", 13);
    memcpy(b + 160, "subdevice #0", 12);
    uint32_t one = 1;
    memcpy(b + 200, &one, 4);
    memcpy(b + 204, &one, 4);
}

int64_t lx_snd_ctl_ioctl(struct lx_task *t, uint64_t req, uint64_t arg) {
    switch ((uint32_t)req) {
    case CTL_PVERSION: { uint32_t v = CTL_VERSION; return lx_copy_out(t, arg, &v, 4) ? -LX_EFAULT : 0; }
    case CTL_CARD_INFO: {
        static uint8_t b[376];
        memset(b, 0, sizeof b);
        memcpy(b + 8, "okernel", 7);                         // id
        memcpy(b + 24, "okernel", 7);                        // driver: no vendor alias, alsa-lib's default is plain hw
        memcpy(b + 40, "okernel sound", 13);                 // name
        memcpy(b + 72, "okernel sound", 13);                 // longname
        return lx_copy_out(t, arg, b, sizeof b) ? -LX_EFAULT : 0;
    }
    case CTL_ELEM_LIST: {                                    // no mixer controls
        uint8_t b[80];
        if (lx_copy_in(t, b, arg, sizeof b)) return -LX_EFAULT;
        memset(b + 8, 0, 8);                                 // used, count
        return lx_copy_out(t, arg, b, sizeof b) ? -LX_EFAULT : 0;
    }
    case CTL_SUBSCRIBE: return 0;
    case CTL_PCM_NEXT: {
        int v;
        if (lx_copy_in(t, &v, arg, 4)) return -LX_EFAULT;
        v = v < 0 ? 0 : -1;
        return lx_copy_out(t, arg, &v, 4) ? -LX_EFAULT : 0;
    }
    case CTL_PCM_INFO: {
        static uint8_t b[288];
        if (lx_copy_in(t, b, arg, 288)) return -LX_EFAULT;
        uint32_t dev, sub; int stream;
        memcpy(&dev, b, 4); memcpy(&sub, b + 4, 4); memcpy(&stream, b + 8, 4);
        if (dev != 0 || sub != 0 || stream != 0) return -LX_ENOENT;          // playback only
        fill_pcm_info(b);
        return lx_copy_out(t, arg, b, 288) ? -LX_EFAULT : 0;
    }
    case CTL_PCM_PREFER: return 0;
    default: return -LX_ENOTTY;
    }
}

int64_t lx_snd_pcm_ioctl(struct lx_task *t, struct lx_file *f, uint64_t req, uint64_t arg) {
    if (!P.open) return -LX_EBADFD;
    switch ((uint32_t)req) {
    case PCM_PVERSION: { uint32_t v = PCM_VERSION; return lx_copy_out(t, arg, &v, 4) ? -LX_EFAULT : 0; }
    case PCM_INFO: {
        static uint8_t b[288];
        memset(b, 0, 288);
        fill_pcm_info(b);
        return lx_copy_out(t, arg, b, 288) ? -LX_EFAULT : 0;
    }
    case PCM_TSTAMP: case PCM_TTSTAMP: return 0;
    case PCM_HW_REFINE: return hw_params(t, arg, 0);
    case PCM_HW_PARAMS: return hw_params(t, arg, 1);
    case PCM_HW_FREE: P.state = ST_OPEN; return 0;
    case PCM_SW_PARAMS: {
        static uint8_t b[136];
        uint64_t am;
        if (lx_copy_in(t, b, arg, sizeof b)) return -LX_EFAULT;
        memcpy(&am, b + 16, 8);                              // tstamp_mode, period_step, sleep_min, avail_min
        P.avail_min = am ? am : 1;
        return 0;                                            // nothing to write back
    }
    case PCM_HWSYNC: update_hw(); return 0;
    case PCM_DELAY: {
        int64_t d = (int64_t)(P.appl - update_hw());
        return lx_copy_out(t, arg, &d, 8) ? -LX_EFAULT : 0;
    }
    case PCM_STATUS: {                                       // struct snd_pcm_status
        uint8_t b[152];
        uint64_t hw = update_hw(), appl = P.appl, delay = appl - hw, avail = avail_frames();
        int st = P.state;
        memset(b, 0, sizeof b);
        memcpy(b, &st, 4);
        memcpy(b + 40, &appl, 8); memcpy(b + 48, &hw, 8); memcpy(b + 56, &delay, 8); memcpy(b + 64, &avail, 8); memcpy(b + 72, &avail, 8);
        return lx_copy_out(t, arg, b, sizeof b) ? -LX_EFAULT : 0;
    }
    case PCM_SYNC_PTR: {                                     // flags at 0, status at 8, control at 72
        uint8_t b[136];
        if (lx_copy_in(t, b, arg, sizeof b)) return -LX_EFAULT;
        uint64_t hw = update_hw();                           // no mapped access: only WRITEI moves appl, what the program sends here adds nothing
        int st = P.state;
        memset(b + 8, 0, 64);
        memcpy(b + 8, &st, 4);
        memcpy(b + 16, &hw, 8);
        memcpy(b + 72, &P.appl, 8); memcpy(b + 80, &P.avail_min, 8);
        return lx_copy_out(t, arg, b, sizeof b) ? -LX_EFAULT : 0;
    }
    case PCM_PREPARE:
        if (P.state == ST_OPEN) return -LX_EBADFD;
        P.state = ST_PREPARED;
        return 0;
    case PCM_RESET: update_hw(); return 0;
    case PCM_START: if (P.state < ST_PREPARED) return -LX_EBADFD; P.state = ST_RUNNING; return 0;
    case PCM_DROP:
        if (P.state < ST_PREPARED) return -LX_EBADFD;
        audio_flush();
        update_hw();
        P.hw = P.appl;
        P.state = ST_SETUP;
        return 0;
    case PCM_DRAIN:
        if (P.state < ST_PREPARED) return -LX_EBADFD;
        if (audio_queued() != 0) {
            if (f->flags & LX_O_NONBLOCK) return -LX_EAGAIN;
            lx_block(t, wait_drained, 0);
            return 0;
        }
        update_hw();
        P.state = ST_SETUP;
        return 0;
    case PCM_REWIND: case PCM_FORWARD: return -LX_EINVAL;
    case PCM_WRITEI: return writei(t, f, arg);
    default: return -LX_ENOTTY;
    }
}
