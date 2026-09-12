#ifndef MEMWIPE_H
#define MEMWIPE_H

// Secret wiping, independent of build configuration (cryptoholes round 4,
// #8): this header must work whether <string.h> resolves to the kernel's
// freestanding header (-Isrc builds) or the system libc header (reviewer /
// minimal builds that only add -Isrc/crypto). Depending on include-path
// luck for a security primitive is exactly the divergence the review
// flagged, so the wipe lives HERE — included unconditionally, no KERNEL
// gate, no reliance on which string.h won the search path. Previously
// this lived in src/string.h and tls_client.c's unconditional
// tls_state_wipe() call implicitly depended on -Isrc resolving first.

// Volatile stores so the compiler cannot optimize the clearing away
// (freestanding builds have no memset_s). Header-inline so host-test
// builds (which link libc, not string.c) get it with zero link impact;
// -O2 inlines it in the kernel. Assembly-verified 2026-09-12: inlines to
// per-byte movb stores under the real kernel flags (-O2 -fno-pic
// -fno-pie -mno-red-zone -m32); never a memset call, never elided.
static inline void secure_zero(void* p, unsigned int n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

#endif
