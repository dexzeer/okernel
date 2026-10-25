// Minimal runtime for the programs of the Linux subsystem's own root file system: raw system calls, no C library.
#ifndef LXSYS_H
#define LXSYS_H

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned char u8;

static inline i64 sc0(i64 n) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory"); return r; }
static inline i64 sc1(i64 n, i64 a) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory"); return r; }
static inline i64 sc2(i64 n, i64 a, i64 b) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory"); return r; }
static inline i64 sc3(i64 n, i64 a, i64 b, i64 c) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static inline i64 sc4(i64 n, i64 a, i64 b, i64 c, i64 d) {
    i64 r; register i64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory"); return r;
}
static inline i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    i64 r; register i64 r10 __asm__("r10") = d; register i64 r8 __asm__("r8") = e; register i64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory"); return r;
}

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_pipe 22
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_exit_group 231

static inline i64 sys_write(int fd, const void *b, u64 n) { return sc3(SYS_write, fd, (i64)b, (i64)n); }
static inline i64 sys_read(int fd, void *b, u64 n) { return sc3(SYS_read, fd, (i64)b, (i64)n); }
static inline void sys_exit(int c) { sc1(SYS_exit_group, c); for (;;) {} }

static inline u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static inline void puts_fd(int fd, const char *s) { sys_write(fd, s, slen(s)); }
static inline void puts_(const char *s) { puts_fd(1, s); }

static inline void put_dec(i64 v) {
    char b[24]; int i = 23; b[i] = 0;
    int neg = v < 0; u64 u = neg ? (u64)-v : (u64)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    puts_(b + i);
}

#endif
