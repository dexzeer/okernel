#ifndef QJS_STDINT_H
#define QJS_STDINT_H
// QuickJS is compiled hosted (builtins on) with -nostdinc; gcc's own
// stdint.h would #include_next a libc header in hosted mode.
#include <stdint-gcc.h>
#endif
