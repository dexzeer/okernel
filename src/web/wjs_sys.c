// wjs_sys.c — the platform hooks of the ojs engine (src/ojs/ojs_sys.h) for
// the web engine: its heap and the wall clock. The kernel runs in UTC.

#include "wcommon.h"
#include "../ojs/ojs_sys.h"

void* ojs_sys_malloc(size_t n) { return w_malloc(n ? n : 1); }
void* ojs_sys_realloc(void* p, size_t n) { return w_realloc(p, n ? n : 1); }
void  ojs_sys_free(void* p) { if (p) w_free(p); }

// Page JS memory. A realm may use up to half the kernel heap (96MB..512MB:
// 207MB at -m 512), and never takes memory that would leave the kernel with
// less than an eighth of its heap (48MB at least) free: tabs share the heap,
// and the OS must outlive any page.
#define REALM_LIMIT_MIN (96u << 20)
#define REALM_LIMIT_MAX (512u << 20)
#if defined(KERNEL) && KERNEL
static uint32_t kernel_reserve(uint32_t total) { uint32_t r = total / 8; return r < (48u << 20) ? (48u << 20) : r; }

size_t wjs_sys_realm_limit(void) {
    uint32_t total;
    heap_stats(&total, 0, 0, 0);
    uint32_t lim = total / 2;
    if (lim < REALM_LIMIT_MIN) lim = REALM_LIMIT_MIN;
    if (lim > REALM_LIMIT_MAX) lim = REALM_LIMIT_MAX;
    return lim;
}

int wjs_sys_mem_ok(size_t bytes, void* ud) {
    (void)ud;
    uint32_t total, used;
    heap_stats(&total, &used, 0, 0);
    uint32_t free = total > used ? total - used : 0;
    return bytes < free && free - bytes >= kernel_reserve(total);
}
#else
size_t wjs_sys_realm_limit(void) {
    const char* e = getenv("WJS_REALM_MB");   // host: emulate a smaller machine
    return e ? (size_t)atoi(e) << 20 : (size_t)256 << 20;
}
int wjs_sys_mem_ok(size_t bytes, void* ud) { (void)bytes; (void)ud; return 1; }
#endif

#if defined(KERNEL) && KERNEL
int64_t kclock_epoch_ms(void);
double ojs_sys_time_ms(void) { return (double)kclock_epoch_ms(); }
#else
#include <sys/time.h>
double ojs_sys_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (double)tv.tv_sec * 1000.0 + (double)(tv.tv_usec / 1000);
}
#endif
int ojs_sys_tz_offset(double utc_ms, int is_local) { (void)utc_ms; (void)is_local; return 0; }
