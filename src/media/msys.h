#ifndef MEDIA_MSYS_H
#define MEDIA_MSYS_H

// What the media code needs from its environment: memory, mem*() and a few libm
// functions. The same sources build in the kernel (freestanding) and on the host.

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define MEDIA_ALLOC(n) calloc(1, (n))
#define MEDIA_FREE(p)  free(p)
#define MEDIA_REALLOC(p, n) realloc((p), (n))
#else
#include "../string.h"
#include "../memory.h"
double sqrt(double);
double sin(double);
double cos(double);
double exp(double);
double log(double);
double floor(double);
static inline int abs(int x) { return x < 0 ? -x : x; }
#define MEDIA_ALLOC(n) kcalloc(1, (n))
#define MEDIA_FREE(p)  kfree(p)
#define MEDIA_REALLOC(p, n) krealloc((p), (n))
#endif

#endif
