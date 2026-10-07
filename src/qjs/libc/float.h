#ifndef QJS_FLOAT_H
#define QJS_FLOAT_H
#include_next <float.h>
// QuickJS runs with the x87 precision control at 53 bits (set on every
// engine entry), so double expressions round like FLT_EVAL_METHOD 0. musl's
// libm picks its rounding tricks (floor/ceil/rint/__rem_pio2) from this.
#undef FLT_EVAL_METHOD
#define FLT_EVAL_METHOD 0
#endif
