// What dmccl needs from its environment: memory, mem*()/str*() and a few libm functions. The same sources build in the
// kernel (freestanding) and on the host.
#ifndef DM_SYS_H
#define DM_SYS_H

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define DM_ALLOC(n)         calloc(1, (n))
#define DM_REALLOC(p, n)    realloc((p), (n))
#define DM_FREE(p)          free(p)
#else
#include "../string.h"
#include "../memory.h"
double floor(double);
double fmod(double, double);
// the kernel's string.h has no strcpy/strcat: keep our own, named so they cannot clash with a later libc-like header
static inline char* dm_strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
static inline char* dm_strcat(char* d, const char* s) { char* r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
#define strcpy dm_strcpy
#define strcat dm_strcat
#define DM_ALLOC(n)         kcalloc(1, (n))
#define DM_REALLOC(p, n)    krealloc((p), (n))
#define DM_FREE(p)          kfree(p)
#endif

#endif
