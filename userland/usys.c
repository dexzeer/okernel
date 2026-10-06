// usys.c — the single ring-3 trap site for okernel userland.
//
// Freestanding i386: no libc, no includes. Each wrapper puts the syscall
// number in EAX, args in EBX/ECX/EDX, traps, and takes the return from EAX.
// Clobbers mirror the historical per-program wrappers (the trap may trash
// ECX/EDX on paths that don't take them as inputs).
#include "usys.h"

int usys_print(const char *msg) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(USYS_PRINT), "b"(msg)
                     : "memory", "ecx", "edx");
    return r;
}

void usys_exit(int code) {
    __asm__ volatile("int $0x80" : : "a"(USYS_EXIT), "b"(code) : "memory");
    while (1) { }
}

int usys_write(int fd, const char *buf, unsigned len) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(USYS_WRITE), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return r;
}

int usys_getpid(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_GETPID)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_yield(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_YIELD)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_read(int fd, char *buf, unsigned len) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(USYS_READ), "b"(fd), "c"(buf), "d"(len)
                     : "memory");
    return r;
}

int usys_open(const char *path, unsigned flags) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(USYS_OPEN), "b"(path), "c"(flags)
                     : "memory", "edx");
    return r;
}

int usys_close(int fd) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_CLOSE), "b"(fd)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_fork(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_FORK)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_exec(const char *path) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_EXEC), "b"(path)
                     : "memory", "ecx", "edx");
    return r;
}

unsigned usys_sbrk(unsigned inc) {
    unsigned r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_SBRK), "b"(inc)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_pipe(int *fds) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_PIPE), "b"(fds)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_dup(int oldfd) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_DUP), "b"(oldfd)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_wait(int pid) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_WAIT), "b"(pid)
                     : "memory", "ecx", "edx");
    return r;
}

int usys_kill(int pid, int signo) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r) : "a"(USYS_KILL), "b"(pid), "c"(signo)
                     : "memory", "edx");
    return r;
}

unsigned usys_mmap(unsigned virt) {
    unsigned r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(USYS_MMAP), "b"(virt)
                     : "memory", "ecx", "edx");
    return r;
}

void usys_munmap(unsigned virt) {
    __asm__ volatile("int $0x80" : : "a"(USYS_MUNMAP), "b"(virt)
                     : "memory", "ecx", "edx");
}
