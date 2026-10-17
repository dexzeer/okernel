#ifndef AUDIO_RATE_H
#define AUDIO_RATE_H

#include <stdint.h>

// Device-rate adaptation for the sound card (used by hda.c; pure integer code, host-tested by tests/test_arate.c).
//
// The card is supposed to consume 48000 frames per second. In an emulator it does not always: the position counter was measured at
// 1.18x the kernel's clock on a Windows host (QEMU's audio backend, host timer granularity). Everything above the driver (the media
// engine) assumes 48 kHz and slaves the video to the card's position, so a card that really consumes frames faster plays everything
// too fast. The driver therefore compares the card's position with the CPU's cycle counter (real time) over windows of a few seconds;
// when two consecutive windows agree that the card runs more than 5% off nominal it
//   - resamples what is written (linear interpolation) by that factor, so the sound has the right pitch and speed again, and
//   - reports positions in NOMINAL frames (device frames / factor, integrated so a change of the factor never makes the clock jump),
// which makes the card look like a perfect 48 kHz one to its users. A card within 5% is passed through untouched.

struct arate {
    uint32_t rate_q16;        // device frames consumed per nominal frame (65536 = exact)
    uint32_t inv_q16;         // nominal frames per device frame (65536 * 65536 / rate)
    uint64_t nom_q16;         // nominal frames played, integrated up to dev_last (Q16)
    uint64_t dev_last;        // device frames played at the last integration
    // measurement
    uint64_t win_t0_ms, win_dev0;
    uint32_t last_meas_q16;   // the previous window's result (0 = none)
    int win_valid;
    int forced;               // test hook: the factor is fixed
    int observe_only;         // measure and report (measured_q16) but never change the factor
    uint32_t measured_q16;    // the last result two agreeing windows produced (0 = none yet)
    // resampler state (linear interpolation, positions in input frames, Q16, relative to the current chunk: index -1 = prev)
    int32_t pos_q16;
    int16_t prev[2];
    int primed;
};

void arate_init(struct arate* a);
// Report the card's position (device frames played since the stream started) and a real-time stamp in ms (monotonic); about
// every 10 ms is plenty. Integrates the nominal clock and runs the measurement.
void arate_update(struct arate* a, uint64_t dev_frames, uint64_t real_ms);
// Nominal frames played, given the current device position (>= the one of the last update).
uint64_t arate_nominal(const struct arate* a, uint64_t dev_frames);
// Device frames -> nominal frames (queue lengths).
uint32_t arate_to_nominal(const struct arate* a, uint32_t dev_frames);
// Nominal frames that fit in `dev_space` device frames.
uint32_t arate_fit(const struct arate* a, uint32_t dev_space);
int arate_active(const struct arate* a);
// Resample `n` nominal stereo frames into `out` (at most `cap` device frames; the caller sized n with arate_fit). Returns the
// device frames produced. With an exact card this is a plain copy.
uint32_t arate_resample(struct arate* a, const int16_t* in, uint32_t n, int16_t* out, uint32_t cap);
void arate_reset_stream(struct arate* a);       // flush: forget the interpolation history
// Test hook: force the factor (q16), 0 = measure again.
void arate_force(struct arate* a, uint32_t rate_q16);

// Smoothing of the position a card reports. An emulated card moves its position in bursts (QEMU's HD Audio model: a few ticks at 1.4x
// speed, then ticks without any movement); a video paced by that shows frames at irregular intervals. The smoothed clock runs at real
// time (`now_us`, microseconds from a trustworthy cycle counter), is pulled toward the card's position with a time constant of 250 ms,
// never goes backwards and never gets more than 20 ms ahead of the card. A jump of the card's position (flush, seek, a long stall)
// restarts it.
struct aclk {
    int valid;
    uint64_t us;          // time of the last call
    uint64_t est_q8;      // the estimate in frames, Q8
    uint64_t last;        // the last value returned
};

void aclk_reset(struct aclk* c);
uint64_t aclk_smooth(struct aclk* c, uint64_t raw_frames, uint64_t now_us);

#endif
