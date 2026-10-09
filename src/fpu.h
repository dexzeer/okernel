#ifndef FPU_H
#define FPU_H

#include <stdint.h>

// x87 + SSE state. The kernel is compiled without SSE (-mno-sse); SSE is used
// only by hand-picked helpers (simd.c) that run in thread context. Because any
// thread may use XMM registers, context switches save the whole state with
// FXSAVE (x87 + MXCSR + XMM0-7). Interrupt handlers must never touch XMM.

// Enable the FPU/SSE on the calling CPU (CR0.EM=0, MP, NE, TS=0; CR4.OSFXSR and
// OSXMMEXCPT when the CPU has FXSR+SSE+SSE2) and reset it. Every CPU calls it.
void fpu_init_cpu(void);

// The CPU supports FXSAVE and SSE2 (the kernel enabled it).
int fpu_has_sse2(void);
int fpu_has_fxsave(void);

// Context-switch save/restore of the full FP state. `buf` is 512 bytes, 16-aligned
// (FXSAVE format). Falls back to FNSAVE (108 bytes at the start of buf) when the
// CPU has no FXSR.
void fpu_save(void* buf);
void fpu_restore(const void* buf);
// Fresh state for a thread that has never run (FNINIT + default MXCSR).
void fpu_reset(void);

#endif
