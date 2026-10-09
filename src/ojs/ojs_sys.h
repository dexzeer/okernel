// ojs_sys.h — the little the engine needs from its platform.
//
// Kernel: src/string.h (rep movs memcpy & co) and the heap hooks the
// embedder installs (kmalloc). Host tests: the C library.

#ifndef OJS_SYS_H
#define OJS_SYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#if defined(KERNEL) && KERNEL
#include "../string.h"
#else
#include <string.h>
#endif

// heap hooks (ojs_set_allocator / defaults in api.c)
void* ojs_sys_malloc(size_t n);
void* ojs_sys_realloc(void* p, size_t n);
void  ojs_sys_free(void* p);

// formatted output into a fixed buffer (own implementation in util.c:
// %s %d %u %x %c %%, plus %S = struct str*, %V = jv as debug text)
int ojs_vsnprintf(char* out, int cap, const char* fmt, va_list ap);
int ojs_snprintf(char* out, int cap, const char* fmt, ...);

#endif
