/*
 * okai JS — shim layer
 *
 * "tinyjs ok edition (tm)": a substantially rewritten C port of Gordon
 * Williams' TinyJS (gfwilliams/tiny-js) + MarcoLizza/tiny-js, adapted to a
 * freestanding C kernel. This file isolates every dependency that differs
 * between the host (libc, for testing) and the kernel (kmalloc/serial_printf,
 * no libc). Flip JS_KERNEL to switch.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#ifndef JS_OS_H
#define JS_OS_H

#include <stdarg.h>

#ifndef JS_KERNEL
#include <setjmp.h>
#else
/* Freestanding kernel: no libc, so provide our own jmp_buf + setjmp/longjmp
 * (i386) and map the engine's setjmp/longjmp calls onto them. The impls
 * live in js_os.c (JS_KERNEL section) as naked asm functions. */
typedef int jmp_buf[6]; /* slots: ebx, esi, edi, ebp, eip, esp */
int  js_setjmp(jmp_buf env) __attribute__((returns_twice));
void js_longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define setjmp(env)  js_setjmp(env)
#define longjmp(e,v) js_longjmp((e),(v))
#endif

/* ---- allocation ---- */
void *js_malloc(unsigned int size);
void  js_free(void *p);
void *js_realloc(void *p, unsigned int size);

/* ---- output (errors / console) ---- */
void js_printf(const char *fmt, ...);

/* ---- string / number helpers (no libc in kernel mode) ---- */
char *js_strdup(const char *s);
char *js_strcpy(char *dst, const char *src);
char *js_strncpy(char *dst, const char *src, int n);
char *js_strcat(char *dst, const char *src);
void  js_strlcat(char *dst, const char *src, int n);
int   js_strcmp(const char *a, const char *b);
char *js_strstr(const char *hay, const char *needle);
long  js_strtol(const char *s, char **end, int base);
double js_strtod(const char *s, char **end);
/* double -> decimal string, returns chars written (excl. NUL). buf must hold buflen. */
int   js_dtoa(double v, char *buf, int buflen);
int   js_sprintf(char *buf, int buflen, const char *fmt, ...);

void js_assert_fail(const char *expr, const char *file, int line);
#define js_assert(x) do { if(!(x)) js_assert_fail(#x, __FILE__, __LINE__); } while(0)

#endif /* JS_OS_H */
