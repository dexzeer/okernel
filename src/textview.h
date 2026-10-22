#ifndef TEXTVIEW_H
#define TEXTVIEW_H

#include <stdint.h>

// Read-only text viewer windows (the recovery desktop's crash records, logs and hex dumps): any length, wrapped to the
// window, scrolled with the wheel, arrows, PgUp/PgDn, g/G; q closes.
int textview_open(const char* title, const char* text, uint32_t len);   // window id, -1 on failure (the text is copied)
int textview_owns(int win);
void textview_key(int win, char c);
void textview_wheel(int win, int notches);
void textview_close(int win);
void textview_redraw_if_resized(void);                                  // main loop: rewrap after a resize

#endif
