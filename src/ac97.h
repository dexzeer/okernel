#ifndef AC97_H
#define AC97_H

#include <stdint.h>

// Intel AC'97 audio controller (ICH-style: NAM mixer + NABM bus master, PCM-out engine), polled. Used by hda.c as the second
// backend behind the same ring and queue logic. Why it exists: QEMU's HD Audio model feeds the card from a timer and re-synchronises
// to the audio backend in 1-4 ms steps, which on a Windows host makes the card's position run 1.0-1.24x fast (it varies from run to
// run) and throws away a 42 ms buffer whenever it overruns (measured, see AGENTS.md). QEMU's AC97 model is pull-driven - it takes
// exactly what the backend can play - so its position is the real playback position.

// Find the controller, program a ring of 32 equal buffers over `ring` (`ring_bytes` long, a multiple of 128, physically contiguous)
// with the descriptor list in `bdl` (256 bytes, 4-byte aligned) and start the PCM-out DMA. Returns 0 on success.
int ac97_init(void* ring, uint32_t ring_bytes, void* bdl);
// Bytes of the ring the DMA has consumed in the current lap, 0 .. ring_bytes-1.
uint32_t ac97_pos(void);
// Keep the "last valid index" one buffer behind the current one so the DMA never halts; restart it if it did. Call at least every
// few hundred milliseconds (the timer tick).
void ac97_tick(void);
void ac97_set_volume(int percent);
int ac97_present(void);              // initialised and running
int ac97_exists(void);               // an audio controller of class 4.1 is on the PCI bus

#endif
