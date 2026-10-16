#include "prof.h"
#include "serial.h"

volatile int prof_on;
uint64_t prof_cyc[PZ_COUNT];
uint32_t prof_cnt[PZ_COUNT];

static const char* const names[PZ_COUNT] = {
    "keys", "okai", "windows", "taskbar", "flush", "halt", "job", "paint", "scroll", "blit",
    "net", "fetch", "tls", "rtw", "klog", "xhci", "i2c", "j_load", "j_fetch", "j_js", "j_event", "j_media"
};

extern uint32_t tick_count;
volatile uint32_t stall_beat;
volatile int stall_halted;
#define STALL_SLOTS 48
struct stall_ent { uint32_t eip, w0, w1, n; };
static struct stall_ent stall_tab[STALL_SLOTS];
static uint32_t stall_total, stall_t0;

void stall_sample(uint32_t eip, uint32_t cs, uint32_t esp) {
    if (!prof_on || stall_halted || (uint32_t)(tick_count - stall_beat) < 10) return;
    if (cs != 0x08) eip = 0;                                  // ring 3: no address worth keeping
    if (!stall_total) stall_t0 = stall_beat;
    stall_total++;
    // who called a leaf function (memset, memcpy): code addresses found near the top of the stack
    uint32_t w0 = 0, w1 = 0;
    if (esp >= 0xC0000000u && esp < 0xFFFFF000u) {                  // the first two words of the top of the stack that look like kernel code addresses
        for (int i = 0; i < 12; i++) {
            uint32_t w = ((const volatile uint32_t*)esp)[i];
            if (w < 0xC0100000u || w >= 0xC0900000u) continue;
            if (!w0) w0 = w; else { w1 = w; break; }
        }
    }
    for (int i = 0; i < STALL_SLOTS; i++) {
        if (stall_tab[i].n && stall_tab[i].eip == eip && stall_tab[i].w0 == w0) { stall_tab[i].n++; return; }
        if (!stall_tab[i].n) { stall_tab[i].eip = eip; stall_tab[i].w0 = w0; stall_tab[i].w1 = w1; stall_tab[i].n = 1; return; }
    }
}

void stall_report(void) {
    if (!stall_total) return;
    uint32_t total = stall_total, t0 = stall_t0;
    struct stall_ent top[6] = {{0, 0, 0, 0}};
    for (int i = 0; i < STALL_SLOTS; i++) {                     // six biggest
        uint32_t n = stall_tab[i].n;
        if (!n) continue;
        for (int k = 0; k < 6; k++)
            if (n > top[k].n) { for (int j = 5; j > k; j--) top[j] = top[j - 1]; top[k] = stall_tab[i]; break; }
        stall_tab[i].n = 0;
    }
    stall_total = 0;
    serial_printf("[stall] %u ms (%u samples) since tick %u:", (tick_count - t0) * 10, total, t0);
    for (int k = 0; k < 6 && top[k].n; k++) serial_printf(" %x<%x,%x>*%u", top[k].eip, top[k].w0, top[k].w1, top[k].n);
    serial_puts("\n");
}

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
