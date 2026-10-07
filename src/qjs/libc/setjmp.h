#ifndef QJS_SETJMP_H
#define QJS_SETJMP_H
// Included (not used) by dtoa.c.
typedef void* jmp_buf[5];
#define setjmp(b) __builtin_setjmp(b)
#define longjmp(b, v) __builtin_longjmp(b, 1)
#endif
