#ifndef RAND_H
#define RAND_H

#include <stdint.h>

// Fortuna-lite ChaCha20 CPRNG for TLS key material.
//
// Honest entropy model (review 2026-09-10 #4 — the old design counted
// SAMPLES: 32 stirs of anything, even zeros or a public MAC, declared
// readiness):
//   * Two pools (fast/slow). Samples accumulate raw; reseeds compress a
//     pool through SHA-256 into the key (XOR-then-PRF combiner).
//   * Readiness requires a reseed compressing >= 64 bytes from >= 2
//     INDEPENDENT source classes — never a sample count. Until then
//     rand_bytes() fails closed (returns 0 → TLS aborts with TLS_FAIL_RNG).
//   * The e1000 MAC is NOT entropy (public, static): it enters only via
//     rand_personalize() as domain separation, never counted.
//   * RDRAND/RDSEED, when the CPU has them, feed as one class among
//     several — never the sole source.
//   * Single-CPU invariant: all entry points hold cli across the state
//     transition (the timer ISR stirs while main-loop code generates).
//     No crypto/RNG state is touched by other IRQ handlers.
//   * Snapshot note: each rand_bytes call mixes fresh RDTSC into the nonce,
//     so a VM snapshot/restore diverges the stream immediately. Not a
//     substitute for reseeding post-restore, but cheap divergence.

// Entropy source classes (bitmask; diversity across classes gates readiness).
#define RAND_SRC_BOOT   0x01  // RDTSC/RTC at boot (weak alone)
#define RAND_SRC_TIMER  0x02  // timer-IRQ RDTSC deltas
#define RAND_SRC_INPUT  0x04  // keyboard/NIC IRQ timing
#define RAND_SRC_RDRAND 0x08  // hardware RNG (when present)

// Seed the CPRNG (BOOT-class sample + immediate reseed attempt).
void rand_seed(const uint8_t entropy[32]);

// Stir one sample (TIMER class; what the timer ISR calls).
void rand_stir(const uint8_t entropy[32]);

// Stir one sample attributed to `src` (RAND_SRC_* bit).
void rand_stir_src(const uint8_t entropy[32], int src);

// Mix non-secret domain separation (e.g. MAC) into the state. NEVER counts
// toward readiness — personalization only.
void rand_personalize(const uint8_t* data, uint32_t len);

// Probe RDRAND/RDSEED via CPUID; on success pull 32B and stir as RDRAND
// class. Returns 1 if hardware entropy was mixed, 0 if absent.
// Call once at boot (before relying on readiness).
int rand_hw_init(void);

// Generate random bytes. Fails closed (returns 0) until seeded.
// Mixes fresh RDTSC into the nonce per call (snapshot divergence) and
// reseeds opportunistically from accumulated pools.
int rand_bytes(uint8_t* out, uint32_t len);

// Returns 1 once honestly seeded (see model above), 0 otherwise.
int rand_ready(void);

#endif
