#ifndef TLS_DBG_H
#define TLS_DBG_H

// Dual-mode debug logging for the TLS stack.
//
// The TLS crypto is shared between two builds:
//   - host tests (normal glibc): use fprintf(stderr, ...)
//   - the freestanding kernel: no <stdio.h>, so route to serial_printf()
//
// KERNEL is defined in the kernel Makefile (CFLAGS += -DKERNEL). Host tests
// compile without it and keep the real fprintf path.

#ifdef KERNEL
#include "../serial.h"
#define tls_dbg(...) serial_printf(__VA_ARGS__)
#else
#include <stdio.h>
#define tls_dbg(...) fprintf(stderr, __VA_ARGS__)
#endif

#endif
