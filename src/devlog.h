#ifndef DEVLOG_H
#define DEVLOG_H

#include <stdarg.h>

// A small on-screen log for drivers on machines without a readable serial port (the real laptop): every line is kept in
// a ring (also written to the serial port) and the shell commands `usb` / `touchpad` print the recent lines of their own
// subsystem (`tag`). Formats: %d %u %x (width, 0-padding) %s %c.
int devlog_fmt(char* out, int cap, const char* fmt, va_list ap);
void devlog(char tag, const char* fmt, ...);
// Appends the last `maxlines` lines with this tag to buf[at...] (one per line); returns the new length.
int devlog_tail(char tag, char* buf, int cap, int at, int maxlines);

#endif
