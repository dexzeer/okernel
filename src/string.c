// Minimal libc string functions for the freestanding kernel.
// The crypto (TLS) stack and other modules use memcpy/memset/strlen; in a
// -nostdlib -ffreestanding build nothing provides them, so we do here.
#include "string.h"

// The bulk primitives are rep-string loops: the old byte loops were the
// single hottest code in the desktop (a 7MB page-surface scroll took ~1s
// under TCG). Inline asm also keeps GCC from turning a loop back into a
// call to memcpy itself. The SysV ABI guarantees DF=0 on entry; memmove's
// backward path sets it and always clears it again.
void* memcpy(void* dst, const void* src, size_t n) {
    void* d = dst; const void* s = src;
    size_t dw = n >> 2, tail = n & 3;
    __asm__ volatile("rep movsl\n\tmovq %3, %%rcx\n\trep movsb"
                     : "+D"(d), "+S"(s), "+c"(dw)
                     : "r"(tail) : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    // Overlapping, dst above src: copy backward. Tail bytes first (from the
    // top), then dwords.
    size_t tail = n & 3, dw = n >> 2;
    const unsigned char* sp = s + n - 1;
    unsigned char* dp = d + n - 1;
    __asm__ volatile("std\n\trep movsb\n\t"
                     "subq $3, %%rsi\n\tsubq $3, %%rdi\n\t"
                     "movq %3, %%rcx\n\trep movsl\n\tcld"
                     : "+D"(dp), "+S"(sp), "+c"(tail)
                     : "r"(dw) : "memory", "cc");
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* d = dst;
    unsigned int v = (unsigned char)c;
    v |= v << 8; v |= v << 16;
    size_t dw = n >> 2, tail = n & 3;
    __asm__ volatile("rep stosl\n\tmovq %3, %%rcx\n\trep stosb"
                     : "+D"(d), "+c"(dw), "+a"(v)
                     : "r"(tail) : "memory");
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) return (void*)(p + i);
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (unsigned int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return 0;
}

char* strrchr(const char* s, int c) {
    const char* found = 0;
    while (*s) {
        if (*s == (char)c) found = s;
        s++;
    }
    return (char*)found;
}

char* strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char* a = h;
        const char* b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strcat(char* dst, const char* src) {
    char* d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    unsigned int i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* d = dst;
    while (*d) d++;
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}

