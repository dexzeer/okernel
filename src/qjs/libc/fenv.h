#ifndef QJS_FENV_H
#define QJS_FENV_H
// x87 rounding modes (QuickJS does not change them; dtoa.c is exact).
#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00
int fegetround(void);
int fesetround(int mode);
#endif
