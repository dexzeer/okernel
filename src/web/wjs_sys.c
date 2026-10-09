// wjs_sys.c — the platform hooks of the ojs engine (src/ojs/ojs_sys.h) for
// the web engine: its heap and the wall clock. The kernel runs in UTC.

#include "wcommon.h"
#include "../ojs/ojs_sys.h"

void* ojs_sys_malloc(size_t n) { return w_malloc(n ? n : 1); }
void* ojs_sys_realloc(void* p, size_t n) { return w_realloc(p, n ? n : 1); }
void  ojs_sys_free(void* p) { if (p) w_free(p); }

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
