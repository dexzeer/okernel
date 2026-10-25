// Internals shared by the files of our kernel32.dll
#ifndef K32_H
#define K32_H
#include "winbase.h"

enum { OB_FREE = 0, OB_FILE, OB_EVENT, OB_MUTEX, OB_SEM, OB_THREAD, OB_PROCESS, OB_FIND, OB_PIPE, OB_STDCON };

struct obj {
    int type;
    int fd;                       // OB_FILE / OB_PIPE / OB_STDCON: the Linux descriptor
    volatile int state;           // events: signalled; mutexes: owner (0 free); semaphores: count
    int manual;                   // events: manual reset
    int max;                      // semaphores: maximum count
    int recursion;                // mutexes
    int pid;                      // processes: pid; threads: tid
    volatile int tidword;         // threads: CLONE_CHILD_CLEARTID word (non-zero while running)
    DWORD exitcode;
    void *stack; SIZE_T stacksize; void *tebmem;
    char *pattern;                // OB_FIND: directory listing
    int dirfd; char dirpath[272]; int dirpos; char wild[272];
    int is_console;
};

HANDLE k32_new_obj(int type, struct obj **out);
struct obj *k32_obj(HANDLE h);
HANDLE k32_handle_for_fd(int fd, int type);
int k32_free_obj(HANDLE h);

// path.c
int k32_to_linux_path(const char *win, char *out, int cap);       // "C:\a\b" -> "/a/b"; relative paths stay relative
int k32_wide_to_utf8(const WCHAR *w, int wlen, char *out, int cap);   // wlen < 0: NUL terminated; returns bytes incl. NUL
int k32_utf8_to_wide(const char *s, int slen, WCHAR *out, int cap);   // returns WCHARs incl. NUL

// heap.c
void *k32_alloc(SIZE_T n);
void k32_free(void *p);
void *k32_realloc(void *p, SIZE_T n);
SIZE_T k32_size(void *p);

// misc
void k32_spin_lock(volatile int *l);
void k32_spin_unlock(volatile int *l);
void k32_futex_wait(volatile int *addr, int val, long timeout_ms);
void k32_futex_wake(volatile int *addr, int n);
long long k32_now_ns(int clk);
void k32_tls_init_thread(void *teb);
DWORD k32_tid(void);

static inline char *strcpy_(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static inline char *strncpy_(char *d, const char *s, SIZE_T n) { SIZE_T i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; if (n) d[i] = 0; return d; }
void *memcpy(void *d, const void *s, SIZE_T n);
void *memset(void *d, int c, SIZE_T n);
SIZE_T strlen(const char *s);

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

#endif
