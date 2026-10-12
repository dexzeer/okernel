#include "prof.h"
#include "serial.h"

volatile int prof_on;
uint64_t prof_cyc[PZ_COUNT];
uint32_t prof_cnt[PZ_COUNT];

static const char* const names[PZ_COUNT] = {
    "keys", "okai", "windows", "taskbar", "flush", "halt", "job", "paint", "scroll", "blit",
    "net", "fetch", "tls", "rtw", "klog", "xhci", "i2c", "j_load", "j_fetch", "j_js", "j_event", "j_media"
};

void prof_report(void) {
    if (!prof_on) return;
    serial_puts("[prof]");
    for (int i = 0; i < PZ_COUNT; i++) {
        uint32_t mc = (uint32_t)(prof_cyc[i] >> 20);       // ~Mcycles (2^20)
        serial_printf(" %s=%u/%u", names[i], mc, prof_cnt[i]);
        prof_cyc[i] = 0;
        prof_cnt[i] = 0;
    }
    serial_puts("\n");
}
