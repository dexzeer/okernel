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
// NOTE: secure_zero used to live here; it moved to src/crypto/memwipe.h
// (cryptoholes round 4, #8) so security code never depends on which
// string.h the include path resolves. Include "memwipe.h" instead.

#endif
