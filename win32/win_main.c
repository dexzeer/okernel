// okai on Windows -- the application: one Win32 window, the browser main loop
// from src/desktop.c, and input routing translated to okai's API.
//
// The kernel's main loop does: key queue -> mouse -> okai_poll -> windows ->
// flush, and halts when okai_wants_cpu() says there is nothing to do. This is
// the same loop with the Win32 message queue in place of the PS/2 queue and
// the DIB blit in place of the framebuffer flush.

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "okai.h"
#include "graphics.h"
#include "window.h"
#include "theme.h"
#include "serial.h"
#include "web/wdoc.h"
#include "web/wjs.h"
#include "win_plat.h"
#include "net/fetch.h"

volatile int win_quit;
HWND win_hwnd;

extern void win_mouse_move(int x, int y);
extern void win_mouse_buttons(int b);
extern void win_mouse_wheel(int notches);
extern void win_sys_init(void);

static int need_full_repaint = 1;
static uint32_t last_chrome_paint;
static int trace_frames;      // OKAI_TRACE=1: log the main-loop stages

// A crash in the browser must leave a record: which exception, where, and the
// call chain (addresses: resolve with `i686-w64-mingw32-nm -n okai-debug.exe`).
// The unhandled filter is enough for a fault in our own code, but an exception
// inside a SYSTEM callback (waveOut, window procedure) is turned into
// 0xC000041D by the callback dispatcher and never reaches it -- so a vectored
// handler records first-chance faults too.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    uint32_t* sp = (uint32_t*)ep->ContextRecord->Esp;
    win_crash_log(r->ExceptionCode, r->ExceptionAddress, ep->ContextRecord->Eip,
                  ep->ContextRecord->Esp, ep->ContextRecord->Ebp, sp, sp ? 24 : 0);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI first_chance(EXCEPTION_POINTERS* ep) {
    static int seen;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Skip the debug-print exceptions and normal flow control.
    if (code == 0x40010006u || code == 0x4001000Au || code == 0x80000003u) return EXCEPTION_CONTINUE_SEARCH;
    if (seen < 8) {
        seen++;
        uint32_t* sp = (uint32_t*)ep->ContextRecord->Esp;
        win_crash_log((uint32_t)code, ep->ExceptionRecord->ExceptionAddress,
                      ep->ContextRecord->Eip, ep->ContextRecord->Esp,
                      ep->ContextRecord->Ebp, sp, sp ? 16 : 0);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- the two hooks okai calls into the desktop -------------------------------

// Long engine passes (layout, scripts) call this through W_PROGRESS/engine_pump:
// keep the network and the media engine moving without touching the page.
void desktop_bg_poll(void) {
    win_net_pump();
    fetch_poll();
}

// Show a window now (a video frame was painted into its surface).
void desktop_present_window(int win_id) {
    if (window_draw(win_id)) {
        graphics_flush();
        okai_note_flush();
    }
}

// ---- input ------------------------------------------------------------------

static int in_rect(const int r[4], int x, int y) {
    return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
}

static void route_key(struct okai* b, unsigned char c) {
    okai_handle_key(0, (char)c);
    b->chrome_dirty = 1;
}

static void on_left_click(int mx, int my) {
    int wid = window_from_point(mx, my);
    if (wid < 0) return;
    int br = okai_find_by_win(wid);
    if (br < 0) return;
    struct okai* ok = okai_get(br);
    if (!ok) return;

    // Caption buttons (okai draws them; window_ctrl_rect is the shared geometry).
    int r[4];
    if (window_ctrl_rect(wid, WIN_CTRL_CLOSE, r) && in_rect(r, mx, my)) {
        PostMessageA(win_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (window_ctrl_rect(wid, WIN_CTRL_MIN, r) && in_rect(r, mx, my)) {
        ShowWindow(win_hwnd, SW_MINIMIZE);
        return;
    }
    if (window_ctrl_rect(wid, WIN_CTRL_MAX, r) && in_rect(r, mx, my)) {
        ShowWindow(win_hwnd, IsZoomed(win_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return;
    }

    // Lock icon: toggle the security popup; any other click closes it.
    if (okai_lock_hit(br, mx, my)) {
        ok->show_security = !ok->show_security;
        ok->chrome_dirty = 1;
        serial_printf("[okai] security popup %s\n", ok->show_security ? "open" : "closed");
        return;
    }
    if (ok->show_security) { ok->show_security = 0; ok->chrome_dirty = 1; }

    // Tab strip: switch / close.
    int on_close = 0;
    int ti = okai_tab_hit(br, mx, my, &on_close);
    if (ti >= 0) {
        if (on_close) okai_close_tab(br, ti);
        else okai_switch_tab(br, ti);
        return;
    }

    // Toolbar navigation buttons.
    int nav = okai_check_nav_click(br, mx, my);
    if (nav != NAV_NONE) {
        serial_printf("[okai] nav action=%d (mx=%d my=%d)\n", nav, mx, my);
        if (nav == NAV_BACK) okai_nav_back(br);
        else if (nav == NAV_FWD) okai_nav_fwd(br);
        else if (nav == NAV_RELOAD) okai_nav_reload(br);
        else if (nav == NAV_HOME) okai_nav_home(br);
        else if (nav == NAV_NEWTAB) okai_new_tab(br, OKAI_HOME_URL);
        return;
    }

    // Chrome band (tab strip + toolbar): only the address bar does something.
    struct window* w = window_get(wid);
    int grid_top = (w ? w->y : 0) + WIN_BORDER + (w && w->no_titlebar ? 0 : WIN_TITLE_H);
    if (my <= grid_top + CHROME_TAB_H + CHROME_TOOL_H) {
        if (okai_addr_bar_hit(br, mx, my) && !ok->addr_bar_focused) {
            serial_printf("[okai] address bar focused\n");
            ok->addr_bar_focused = 1;
            ok->addr_input_len = 0;
            ok->addr_input[0] = 0;
            ok->chrome_dirty = 1;
        }
        return;
    }

    okai_content_click(br, mx, my);
}

// ---- the window -------------------------------------------------------------

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            win_window_resize(LOWORD(lp), HIWORD(lp));
            if (okai_get(0) && okai_tab_of(okai_get(0))->doc) okai_render_content(0);
            need_full_repaint = 1;
        }
        return 0;

    case WM_NCCALCSIZE:
        // Frame, no caption: the client area IS the window (the browser draws
        // its own 2px frame), so okai's chrome sits flush against the edge
        // like it does on Okapi. Resizing comes from WM_NCHITTEST below.
        if (wp) return 0;
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
        else SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        mm->ptMaxPosition.x = wa.left;
        mm->ptMaxPosition.y = wa.top;
        mm->ptMaxSize.x = wa.right - wa.left;
        mm->ptMaxSize.y = wa.bottom - wa.top;
        mm->ptMinTrackSize.x = 480;
        mm->ptMinTrackSize.y = 360;
        return 0;
    }

    case WM_NCHITTEST: {
        // No non-client area: decide here. Outer edge = resize, tab-strip
        // background = drag (like a Firefox tab strip), everything else = client.
        RECT r;
        GetWindowRect(hwnd, &r);
        int x = (short)LOWORD(lp) - r.left, y = (short)HIWORD(lp) - r.top;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (!IsZoomed(hwnd)) {
            int b = 6;
            int L = x < b, R = x >= w - b, T = y < b, B = y >= h - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
        }
        struct okai* b = okai_get(0);
        if (b && y >= 0 && y < WIN_BORDER + CHROME_TAB_H) {
            int on_close = 0, cr[4];
            int interactive = okai_tab_hit(0, x, y, &on_close) >= 0 ||
                              okai_check_nav_click(0, x, y) == NAV_NEWTAB ||
                              (window_ctrl_rect(b->win_id, WIN_CTRL_CLOSE, cr) && in_rect(cr, x, y)) ||
                              (window_ctrl_rect(b->win_id, WIN_CTRL_MIN, cr) && in_rect(cr, x, y)) ||
                              (window_ctrl_rect(b->win_id, WIN_CTRL_MAX, cr) && in_rect(cr, x, y));
            if (!interactive) return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        win_gfx_present_all();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_PRINTCLIENT: {
        // Screenshot tools (PrintWindow) and DWM previews ask for the client
        // area instead of forcing a WM_PAINT: blit the backbuffer into the
        // DC they hand us. Without this, PrintWindow can stall the window.
        HDC dc = (HDC)wp, src = win_gfx_memdc();
        if (dc && src) BitBlt(dc, 0, 0, win_client_w, win_client_h, src, 0, 0, SRCCOPY);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // the backbuffer covers everything

    case WM_NCLBUTTONDOWN:
        serial_printf("[okai-win] nclbuttondown ht=%d at %d,%d\n", (int)wp,
                      (short)LOWORD(lp), (short)HIWORD(lp));
        break;                                   // DefWindowProc runs the move/size loop

    case WM_ENTERSIZEMOVE:
        serial_printf("[okai-win] enter size/move\n");
        return 0;

    case WM_EXITSIZEMOVE:
        serial_printf("[okai-win] exit size/move\n");
        return 0;

    case WM_SYSCOMMAND:
        serial_printf("[okai-win] syscommand %u\n", (unsigned)(wp & 0xFFF0));
        break;

    case WM_MOUSEMOVE:
        win_mouse_move((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_LBUTTONDOWN:
        // No SetCapture: a stuck app must never feel like it owns the mouse
        // (it also keeps Alt-Tab/the desktop usable if the browser hangs).
        if (getenv("OKAI_TRACE")) serial_printf("[okai-win] lbdown %d,%d\n", (short)LOWORD(lp), (short)HIWORD(lp));
        win_mouse_move((short)LOWORD(lp), (short)HIWORD(lp));
        win_mouse_buttons(1);
        win_rand_input();
        on_left_click((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_LBUTTONUP:
        win_mouse_buttons(0);
        return 0;

    case WM_MOUSEWHEEL: {
        POINT p;
        p.x = (short)LOWORD(lp);
        p.y = (short)HIWORD(lp);
        ScreenToClient(hwnd, &p);
        // Windows: positive delta = wheel away from the user = scroll UP.
        // okai (like the kernel's PS/2 path) counts positive = down.
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        int notches = d / WHEEL_DELTA;
        if (!notches) notches = d > 0 ? 1 : -1;
        notches = -notches;
        win_mouse_wheel(notches);
        if (okai_get(0)) okai_handle_wheel_at(0, notches, p.x, p.y);
        return 0;
    }

    case WM_CHAR: {
        struct okai* b = okai_get(0);
        unsigned char c = (unsigned char)wp;
        if (!b) return 0;
        if (c == '\r') c = '\n';
        if (c == 0x1B) c = 27;
        route_key(b, c);
        return 0;
    }

    case WM_KEYDOWN: {
        struct okai* b = okai_get(0);
        if (!b) return 0;
        int c = -1;
        switch (wp) {
        case VK_DOWN:   c = 0x10; break;   // kernel keyboard: Ctrl-P = down
        case VK_UP:     c = 0x11; break;   // Ctrl-Q = up
        case VK_NEXT:   c = 0x04; break;   // Ctrl-D = page down
        case VK_PRIOR:  c = 0x12; break;   // Ctrl-R = page up
        case VK_HOME:   c = 'k'; break;
        case VK_END:    c = 'j'; break;
        }
        if (c >= 0) { win_rand_input(); route_key(b, (unsigned char)c); }
        return 0;
    }

    case WM_SETFOCUS:
        if (okai_get(0)) okai_get(0)->chrome_dirty = 1;
        return 0;

    case WM_CLOSE:
        win_quit = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---- main loop --------------------------------------------------------------

static void browser_loop(const char* url) {
    okai_init();
    if (okai_open(url) < 0) {
        serial_printf("[okai-win] okai_open failed\n");
        return;
    }
    // No taskbar here: give the browser's window the whole client area (it asks
    // for SCREEN_H - TASKBAR_H, which is Okapi's convention) and re-render for
    // the taller viewport.
    win_window_fit_full();
    okai_render_content(0);
    win_logf("[okai-win] opened %s\n", url);

    while (!win_quit) {
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { win_quit = 1; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (win_quit) break;

        // A TIMER-class entropy sample per tick, like the kernel's timer IRQ.
        { static uint32_t last; if (tick_count != last) { last = tick_count; win_rand_timer(); } }

        // Heap checker (OKAI_HCHK=1): verify every live block ~2x a second, so
        // an overrun is reported at its own allocation site instead of being
        // found later by ntdll (0xC0000409, which nothing can catch).
        { static uint32_t last; if ((uint32_t)(tick_count - last) >= 50) { last = tick_count; win_heap_check(); } }

        win_net_pump();
        fetch_poll();
        okai_poll();
        if (trace_frames) win_logf("[frame] okai_poll done\n");

        struct okai* b = okai_get(0);
        if (!b) break;
        int wid = b->win_id;
        int painted = window_draw(wid);
        if (trace_frames) win_logf("[frame] window_draw=%d\n", painted);
        int want = painted || b->chrome_dirty || okai_is_animating(0) ||
                   okai_ui_needs_paint(0) || (tick_count - last_chrome_paint >= 120) ||
                   need_full_repaint;
        if (want) {
            if (trace_frames) win_logf("[frame] chrome paint\n");
            okai_paint_overlays(0);
            b->chrome_dirty = 0;
            last_chrome_paint = tick_count;
            need_full_repaint = 0;
        }
        graphics_flush();
        if (trace_frames) win_logf("[frame] flushed\n");
        trace_frames = 0;
        okai_note_flush();

        switch (okai_wants_cpu()) {
        case 1: break;                              // work due now: straight on
        case 2: Sleep(1); break;                    // waiting for the network
        case 3: Sleep(1); break;                    // timed wait
        default:                                    // idle: sleep until input
            MsgWaitForMultipleObjectsEx(0, 0, 30, QS_ALLINPUT, 0);
            break;
        }
    }
    okai_close(0);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev;
    win_heap_init();
    win_log_init();
    win_logf("[okai-win] start\n");
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, first_chance);
    { const char* t = getenv("OKAI_TRACE"); if (t && *t && *t != '0') trace_frames = 1; }

    // One browser at a time. Two instances are indistinguishable on screen and
    // a stray one is exactly how a window ends up in front of everything.
    HANDLE one = CreateMutexA(0, TRUE, "okai-single-instance");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowA("okai", 0);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        MessageBoxA(0, "okai is already running.", "okai", MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = 0;
    wc.lpszClassName = "okai";
    if (!RegisterClassExA(&wc)) {
        MessageBoxA(0, "RegisterClassEx failed", "okai", MB_ICONERROR);
        return 1;
    }

    // A normal window, never fullscreen or maximized on start: 70% of the work
    // area, capped at 1600x900 (the engine lays out for the client area, so a
    // huge window costs page-load time for nothing).
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int cw = (work.right - work.left) * 7 / 10;
    int ch = (work.bottom - work.top) * 7 / 10;
    if (cw > 1600) cw = 1600;
    if (ch > 900) ch = 900;
    if (cw < 640) cw = 640;
    if (ch < 480) ch = 480;

    RECT r = { 0, 0, cw, ch };
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    // CW_USEDEFAULT is ignored for WS_POPUP windows (it would sit at 0,0).
    int wx = work.left + (work.right - work.left - ww) / 2;
    int wy = work.top + (work.bottom - work.top - wh) / 2;
    if (wx < work.left) wx = work.left;
    if (wy < work.top) wy = work.top;
    HWND hwnd = CreateWindowExA(0, "okai", "okai", style, wx, wy, ww, wh, 0, 0, hinst, 0);
    if (!hwnd) {
        MessageBoxA(0, "CreateWindow failed", "okai", MB_ICONERROR);
        return 1;
    }
    win_hwnd = hwnd;

    RECT cr;
    GetClientRect(hwnd, &cr);
    win_gfx_init(cr.right - cr.left, cr.bottom - cr.top);
    // okai_open() creates the browser's own window (like the kernel: the
    // browser owns it). Creating one here too left a second, empty window in
    // slot 0 that swallowed every click -- hit tests resolved to it, and
    // okai_find_by_win() then found no browser.

    win_sys_init();
    win_net_init();
    audio_init();

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    const char* url = (cmdline && *cmdline) ? cmdline : OKAI_HOME_URL;
    // strip surrounding quotes; a Windows path becomes a file:// URL nothing
    // serves yet, so only a real URL / okai: page is accepted here
    char start[OKAI_URL_LEN];
    int n = 0;
    while (*url == ' ' || *url == '\t' || *url == '"') url++;
    while (url[n] && url[n] != '"' && n < OKAI_URL_LEN - 1) { start[n] = url[n]; n++; }
    start[n] = 0;
    if (!n) snprintf(start, sizeof start, "%s", OKAI_HOME_URL);

    browser_loop(start);

    win_audio_shutdown();
    win_net_shutdown();
    win_gfx_free();
    win_logf("[okai-win] exit\n");
    return 0;
}
