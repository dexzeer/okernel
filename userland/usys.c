// usys.c — the single ring-3 trap site for okernel userland.
//
// Freestanding x86-64: no libc, no includes. The SYSCALL instruction: number in RAX, arguments in RDI/RSI/RDX, the answer in RAX; the CPU clobbers
// RCX and R11.
#include "usys.h"

static inline long sys3(long nr, unsigned long a0, unsigned long a1, unsigned long a2) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a0), "S"(a1), "d"(a2) : "memory", "rcx", "r11");
    return r;
}

int usys_print(const char *msg) { return (int)sys3(USYS_PRINT, (unsigned long)msg, 0, 0); }

void usys_exit(int code) {
    sys3(USYS_EXIT, (unsigned long)code, 0, 0);
    while (1) { }
}

int usys_write(int fd, const char *buf, unsigned len) { return (int)sys3(USYS_WRITE, (unsigned long)fd, (unsigned long)buf, len); }
int usys_getpid(void) { return (int)sys3(USYS_GETPID, 0, 0, 0); }
int usys_yield(void) { return (int)sys3(USYS_YIELD, 0, 0, 0); }
int usys_read(int fd, char *buf, unsigned len) { return (int)sys3(USYS_READ, (unsigned long)fd, (unsigned long)buf, len); }
int usys_open(const char *path, unsigned flags) { return (int)sys3(USYS_OPEN, (unsigned long)path, flags, 0); }
int usys_close(int fd) { return (int)sys3(USYS_CLOSE, (unsigned long)fd, 0, 0); }
int usys_fork(void) { return (int)sys3(USYS_FORK, 0, 0, 0); }
int usys_exec(const char *path) { return (int)sys3(USYS_EXEC, (unsigned long)path, 0, 0); }
unsigned long usys_sbrk(unsigned long inc) { return (unsigned long)sys3(USYS_SBRK, inc, 0, 0); }
int usys_pipe(int *fds) { return (int)sys3(USYS_PIPE, (unsigned long)fds, 0, 0); }
int usys_dup(int oldfd) { return (int)sys3(USYS_DUP, (unsigned long)oldfd, 0, 0); }
int usys_wait(int pid) { return (int)sys3(USYS_WAIT, (unsigned long)pid, 0, 0); }
int usys_kill(int pid, int signo) { return (int)sys3(USYS_KILL, (unsigned long)pid, (unsigned long)signo, 0); }
unsigned long usys_mmap(unsigned long virt) { return (unsigned long)sys3(USYS_MMAP, virt, 0, 0); }
void usys_munmap(unsigned long virt) { sys3(USYS_MUNMAP, virt, 0, 0); }
int usys_panic(int mode) { return (int)sys3(USYS_PANIC, (unsigned long)mode, 0, 0); }
