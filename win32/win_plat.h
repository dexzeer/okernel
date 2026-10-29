#ifndef OKAI_WIN_PLAT_H
#define OKAI_WIN_PLAT_H

// okai on Windows: the platform layer.
//
// The engine (src/web), ojs (src/ojs), TLS/HTTP (src/net, src/crypto) and the
// browser shell (src/okai.c) are the same sources the kernel builds. This
// directory supplies what they expect from the kernel: a screen backbuffer
// with damage tracking (graphics_*), one window (window_*), the heap/log/
// clock names, TCP + DNS over Winsock, and the sound card over waveOut.
//
// Screen model: the backbuffer is win_client_w x (win_client_h + TASKBAR_H).
// okai opens its window at (0, 0, SCREEN_W, SCREEN_H - TASKBAR_H) -- exactly
// the client area -- and never paints the TASKBAR_H rows underneath, so the
// blit only copies what is shown.

#include <windows.h>
#include <stdint.h>

extern HWND win_hwnd;
extern int gfx_w, gfx_h;              // backbuffer size (SCREEN_W/SCREEN_H)
extern int win_client_w, win_client_h; // the Windows client area

void win_gfx_init(int client_w, int client_h);
void win_gfx_free(void);
void win_gfx_resize(int client_w, int client_h);
void win_gfx_present(void);           // copy the dirty rows to the window
void win_gfx_present_all(void);       // copy the whole client area
void win_gfx_invalidate(void);        // the window lost its pixels: present all
HDC  win_gfx_memdc(void);             // the backbuffer's memory DC (WM_PRINTCLIENT)

void win_log_init(void);
void win_logf(const char* fmt, ...);
void win_crash_log(uint32_t code, const void* addr, uint32_t eip, uint32_t esp,
                   uint32_t ebp, const uint32_t* stack, int nstack);

// The allocator (win_heap.c): all of malloc/free and kmalloc/kfree go through
// this core, so OKAI_HCHK=1 can guard every block and report the allocation
// site of an overrun before ntdll turns it into 0xC0000409.
void  win_heap_init(void);
void  win_heap_check(void);
void  win_heap_stats(void);
int   win_heap_guarded(void);
void* win_h_alloc(size_t n, void* site);
void  win_h_free(void* p, void* site);
void* win_h_realloc(void* p, size_t n, void* site);

void win_net_init(void);
void win_net_shutdown(void);
void win_net_pump(void);              // finish connects, push queued bytes, take FINs

void win_window_resize(int client_w, int client_h);   // WM_SIZE
void win_window_fit_full(void);                       // cover the whole client area

void win_window_resize(int client_w, int client_h);

void win_audio_init(void);
int audio_init(void);
void win_audio_shutdown(void);

// Entropy: the boot seed, the 10 Hz timer stir and input timing.
void win_rand_init(void);
void win_rand_timer(void);            // TIMER-class sample (called by the tick thread)
void win_rand_input(void);            // INPUT-class sample (any key / pointer event)

extern volatile int win_quit;
extern volatile uint32_t tick_count;  // 100 Hz, advanced by the tick thread

#endif
