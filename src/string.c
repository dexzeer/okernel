// Minimal libc string functions for the freestanding kernel.
// The crypto (TLS) stack and other modules use memcpy/memset/strlen; in a
// -nostdlib -ffreestanding build nothing provides them, so we do here.
#include "string.h"

void* memcpy(void* dst, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (unsigned int i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void* memset(void* dst, int c, unsigned int n) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char v = (unsigned char)c;
    for (unsigned int i = 0; i < n; i++) d[i] = v;
    return dst;
}

int memcmp(const void* a, const void* b, unsigned int n) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (unsigned int i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

unsigned int strlen(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

int strncmp(const char* a, const char* b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return 0;
}
