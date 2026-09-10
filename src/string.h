#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int   strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
// Secure wipe (review #30): volatile stores so the compiler cannot
// optimize the clearing away (freestanding builds have no memset_s).
// Header-inline so host-test builds (which link libc, not string.c) get
// it with zero link impact; -O2 inlines it in the kernel.
static inline void secure_zero(void* p, unsigned int n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
}

#endif
