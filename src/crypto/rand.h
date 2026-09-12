#ifndef RAND_H
#define RAND_H

#include <stdint.h>

// Fortuna-lite ChaCha20 CPRNG for TLS key material.
//
// Honest entropy model (review 2026-09-10 #4, hardened cryptoholes #1):
// the old design counted SAMPLES; the first hardening counted labelled
// BYTES from >= 2 classes. Labels alone are still not entropy (two weak
// classes of predictable RDTSC bytes are just labelled predictability),
// so readiness is now TIERED on hardware presence + fresh per-class
// bytes in the declaring reseed window:
//   * Bytes gate: the declaring reseed must compress >= 64B (>= 128B with
//     no hardware RNG) of pool0-window bytes.
//   * Freshness gate: >= 2 classes with >= 32B EACH in that same window
//     (one weak sample piggybacking on ancient bulk does not count).
//   * Lifetime gate: >= 2 classes ever seen including a HARDWARE class
//     (RDRAND/RDSEED) when the CPU has one; >= 3 classes (BOOT+TIMER+
//     INPUT) when it does not. The hardware bit, once stirred at boot,
//     persists — later windows need not re-prove it.
//   * Until a reseed satisfies all three, rand_bytes() fails closed
//     (returns 0 → TLS aborts with TLS_FAIL_RNG).
//   * The e1000 MAC is NOT entropy (public, static): it enters only via
//     rand_personalize() as domain separation, never counted.
//   * RDRAND/RDSEED, when CPUID reports them, feed as their own classes
//     (RDSEED tried first: conditioned output, stronger contract).
//   * Single-CPU invariant: all entry points hold cli across the state
//     transition (the timer ISR stirs while main-loop code generates).
//     No crypto/RNG state is touched by other IRQ handlers.
//   * Snapshot note: each rand_bytes call mixes fresh RDTSC into the nonce,
//     so a VM snapshot/restore diverges the stream immediately. Not a
//     substitute for reseeding post-restore, but cheap divergence.
// Residual risk (documented, accepted): without a hardware RNG the model
// rests on RDTSC/RTC boot values + IRQ timing jitter across three kernel-
// observed classes. In a fully deterministic VM (fixed TSC, no input,
// no network) this degrades to weak — the failure mode is fail-CLOSED
// (no output) whenever even that bar is unmet, never silent predictability.

// Entropy source classes (bitmask; diversity across classes gates readiness).
#define RAND_SRC_BOOT   0x01  // RDTSC/RTC at boot (weak alone)
#define RAND_SRC_TIMER  0x02  // timer-IRQ RDTSC deltas
#define RAND_SRC_INPUT  0x04  // keyboard/NIC IRQ timing
#define RAND_SRC_RDRAND 0x08  // hardware RNG (when present)
#define RAND_SRC_RDSEED 0x10  // conditioned hardware RNG (when present)
#define RAND_NCLASS     5

// ---- API surface split (cryptoholes round 4, #6) ----
// PUBLIC: safe for any kernel consumer (no caller-asserted provenance).
// Generate random bytes. Fails closed (returns 0) until seeded.
// Mixes fresh RDTSC into the nonce per call (snapshot divergence) and
// reseeds opportunistically from accumulated pools.
int rand_bytes(uint8_t* out, uint32_t len);

// Returns 1 once honestly seeded (see model above), 0 otherwise.
int rand_ready(void);

// Mix non-secret domain separation (e.g. MAC) into the state. NEVER counts
// toward readiness — personalization only.
void rand_personalize(const uint8_t* data, uint32_t len);

// ---- TRUSTED-INTERNAL: kernel/boot callers ONLY ----
// rand_seed() / rand_stir_src() take the source CLASSIFICATION from the
// caller, and nothing authenticates that provenance — a caller claiming
// RAND_SRC_RDSEED for timer deltas would inflate the lifetime gate. This
// is acceptable (not merely tolerated) because the boundary is static:
// this is a freestanding kernel with no userspace and no module loading,
// so every call site is enumerable at build time (2026-09-12 audit):
//   boot/desktop.c  — RAND_SRC_BOOT  (seed file, RDTSC/RTC jitter loop)
//   timer ISR       — RAND_SRC_TIMER (via rand_stir() fixed-class wrapper)
//   keyboard.c/NIC  — RAND_SRC_INPUT (IRQ timing; e1000 passes literal
//                     0x04 == RAND_SRC_INPUT via weak extern to avoid the
//                     rand.h dependency in non-RNG builds)
//   rand.c itself   — RAND_SRC_RDRAND/RDSEED (CPUID-gated pulls)
// No path lets network input or (nonexistent) ring-3 choose a class. If a
// new call site is ever added, it must be added to this list and its
// claimed class must match the physical source. The readiness gates
// (bytes/freshness/lifetime above) remain the backstop: misattribution
// can only ever weaken output the misattributing context itself uses.

// Seed the CPRNG (BOOT-class sample + immediate reseed attempt).
void rand_seed(const uint8_t entropy[32]);

// Stir one sample (TIMER class; what the timer ISR calls). Fixed
// attribution — safe for any kernel caller, listed here with its kin.
void rand_stir(const uint8_t entropy[32]);

// Stir one sample attributed to `src` (RAND_SRC_* bit). Trusted callers
// only (see above).
void rand_stir_src(const uint8_t entropy[32], int src);

// Probe RDRAND/RDSEED via CPUID; on success pull 32B and stir as RDRAND
// class. Returns 1 if hardware entropy was mixed, 0 if absent.
// Call once at boot (before relying on readiness).
int rand_hw_init(void);

#endif
