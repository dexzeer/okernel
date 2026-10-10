#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

// Audio output: Intel High Definition Audio (hda.c), the controller QEMU emulates
// and every PC since ~2005 has. One output path, fixed format: 48 kHz, signed 16
// bit, 2 channels. Decoders write interleaved frames; the controller plays a
// cyclic DMA ring and `audio_clock()` says how many frames have actually been
// played (the master clock for audio/video sync).

#define AUDIO_RATE 48000

// Probe the controller and codec and start the stream (silence). 0 on success;
// nonzero leaves audio unavailable and everything else working.
int audio_init(void);
int audio_ready(void);

// Queue interleaved stereo frames (L,R,L,R...). Non-blocking: returns how many
// frames were accepted (fewer than asked when the ring is full).
uint32_t audio_write(const int16_t* frames, uint32_t nframes);
// Frames that can be queued right now.
uint32_t audio_space(void);
// Frames queued but not yet played.
uint32_t audio_queued(void);
// Frames played since the stream started (monotonic).
uint64_t audio_clock(void);

// Timer-tick hook (IRQ context, no XMM, no allocation): tracks the play position
// and wipes what has been played so an underrun plays silence, not a loop.
void audio_tick(void);

// Master volume 0..100 (codec amplifier gain where the codec has one).
void audio_set_volume(int percent);

// One line per fact about the controller / codec tree (for the `audio` command).
void audio_describe(void (*out)(const char* line, void* ud), void* ud);

// Self test: play a tone and report what was queued/played (the recording
// side is checked by tests/headless/test_audio.py).
void audio_beep(int hz, int ms);

#endif
