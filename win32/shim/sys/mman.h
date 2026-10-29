#ifndef OKAI_WIN_SYS_MMAN_H
#define OKAI_WIN_SYS_MMAN_H

// Minimal <sys/mman.h> for the Windows port. Only ojs' JIT uses it (jit.c
// allocates whole pages of RWX machine code on the host build); the port backs
// it with VirtualAlloc. The JIT itself is off by default (OJS_JIT=1 enables it).

#include <stddef.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void* addr, size_t len);

#endif
