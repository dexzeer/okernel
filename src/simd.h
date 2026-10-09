#ifndef SIMD_H
#define SIMD_H

#include <stdint.h>

// SSE2 bulk helpers (thread context only: never call them from an interrupt
// handler, which must not touch XMM state - see fpu.h). They fall back to
// plain memcpy/stosl when the CPU has no SSE2, so callers need no checks.

// Copy n bytes; any alignment, regions must not overlap.
void simd_copy(void* dst, const void* src, uint32_t n);
// Same, with non-temporal stores for the bulk: for big copies into write-
// combining video memory (or any destination that will not be read soon).
void simd_copy_stream(void* dst, const void* src, uint32_t n);
// n 32-bit pixels of one value.
void simd_fill32(uint32_t* dst, uint32_t v, uint32_t n);

// Correctness checks (random sizes/alignments vs. reference loops, FXSAVE
// round trip) and a throughput comparison against rep movs. Returns failures.
int simd_selftest(void (*out)(const char* line, void* ud), void* ud);

#endif
