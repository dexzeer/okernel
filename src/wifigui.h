#ifndef WIFIGUI_H
#define WIFIGUI_H

// The Wi-Fi window: scan, pick a network, type the password, connect. Drawn into a pixel surface with the shared
// anti-aliased UI primitives (ui_draw.h) and driven by the desktop main loop (clicks, keys, wheel, poll).
// `wifigui demo` runs it against a simulated adapter (rtw_ui_demo) so it can be tested without hardware.
void wifigui_open(int demo);
int wifigui_win(void);                  // window id, or -1 when closed
void wifigui_close(void);
void wifigui_click(int x, int y);       // surface coordinates (the window's content origin is 0,0)
void wifigui_key(char c);
void wifigui_wheel(int notches);
void wifigui_poll(void);                // every main-loop pass: refresh state, repaint when something changed

#endif
