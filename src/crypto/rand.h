#ifndef RAND_H
#define RAND_H

#include <stdint.h>

// ChaCha20-based CPRNG for TLS key generation in the kernel.
// Seeded from RDTSC + IRQ jitter + e1000 MAC.
// NOT safe for cryptographic key material until at least 32 entropy samples
// have been mixed in via rand_stir().

// Seed the CPRNG. Safe to call multiple times; each call mixes in entropy.
void rand_seed(const uint8_t entropy[32]);

// Stir a single entropy sample (e.g., from timer tick or IRQ).
void rand_stir(const uint8_t entropy[32]);

// Generate random bytes. Blocks (returns 0) if not yet seeded with
// sufficient entropy (rand_ready() == 0).
int rand_bytes(uint8_t* out, uint32_t len);

// Returns 1 if the CPRNG has been seeded with at least 32 bytes of
// mixed entropy, 0 otherwise.
int rand_ready(void);

#endif
