#ifndef QJS_STDLIB_H
#define QJS_STDLIB_H
// Freestanding libc shim for QuickJS inside the kernel (see qjs_libc.c).
#include <stddef.h>

void* malloc(size_t n);
void* calloc(size_t n, size_t s);
void* realloc(void* p, size_t n);
void  free(void* p);
void  abort(void) __attribute__((noreturn));
void  exit(int code) __attribute__((noreturn));
int   abs(int v);
long  labs(long v);
long long llabs(long long v);
int   atoi(const char* s);
long  strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
char* getenv(const char* name);
void  qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*));

#define alloca(n) __builtin_alloca(n)
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif
