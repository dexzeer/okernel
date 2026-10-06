// usys.h — ring-3 user ABI for okernel userland.
//
// Everything in userland talks to the kernel ONLY through these wrappers
// (INT 0x80, syscall numbers 0-17). Raw `int $0x80` asm lives here and
// nowhere else, so the ring-0/ring-3 boundary is one file, not scattered
//statics in every program.
//
// Deliberately NOT exposed: SYS_MMAP_USER (5), which maps an arbitrary
// physical page. That is a driver-level operation and stays ring-0-only.
#ifndef USYS_H
#define USYS_H

#define USYS_PRINT  0   // const char* msg (NUL-terminated, <=4K)
#define USYS_EXIT   1   // int status (never returns)
#define USYS_WRITE  2   // int fd, const char* buf, unsigned len
#define USYS_GETPID 3   // returns pid
#define USYS_YIELD  4   // voluntarily give up the time slice
#define USYS_READ   6   // int fd, char* buf, unsigned len
#define USYS_OPEN   7   // const char* path, unsigned flags
#define USYS_CLOSE  8   // int fd
#define USYS_FORK   9   // returns child pid (0 in child)
#define USYS_EXEC   10  // const char* path (returns only on failure)
#define USYS_SBRK   11  // unsigned inc (returns old break, 0 on failure)
#define USYS_PIPE   12  // int* fds (returns 0, -1 on failure)
#define USYS_DUP    13  // int oldfd (returns new fd, -1 on failure)
#define USYS_WAIT   14  // int pid (returns reaped pid, -1 none, -2 parked)
#define USYS_KILL   15  // int pid, int signo
#define USYS_MMAP   16  // unsigned virt (0 = auto; returns addr, 0 on failure)
#define USYS_MUNMAP 17  // unsigned virt

int usys_print(const char *msg);
void usys_exit(int code) __attribute__((noreturn));
int usys_write(int fd, const char *buf, unsigned len);
int usys_getpid(void);
int usys_yield(void);
int usys_read(int fd, char *buf, unsigned len);
int usys_open(const char *path, unsigned flags);
int usys_close(int fd);
int usys_fork(void);
int usys_exec(const char *path);
unsigned usys_sbrk(unsigned inc);
int usys_pipe(int *fds);
int usys_dup(int oldfd);
int usys_wait(int pid);
int usys_kill(int pid, int signo);
unsigned usys_mmap(unsigned virt);
void usys_munmap(unsigned virt);

#endif
