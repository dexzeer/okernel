// On-screen driver log (see devlog.h).
#include "devlog.h"
#include "serial.h"
#include "klog.h"

#define LOGN 256
#define LOGW 120                 // tag + text + terminator; fits the 85-column terminal

static char LOG[LOGN][LOGW];
static int log_n;

int devlog_fmt(char* out, int cap, const char* fmt, va_list ap) {
    char* o = out;
    char* end = out + cap - 1;
    for (const char* p = fmt; *p && o < end; p++) {
        if (*p != '%') { *o++ = *p; continue; }
        p++;
        int zero = 0, width = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        char c = *p;
        if (!c) break;
        if (c == 's') {
            const char* s = va_arg(ap, const char*);
            while (s && *s && o < end) *o++ = *s++;
        } else if (c == 'c') {
            *o++ = (char)va_arg(ap, int);
        } else if (c == 'd' || c == 'u' || c == 'x') {
            uint32_t v;
            int neg = 0;
            if (c == 'd') { int d = va_arg(ap, int); if (d < 0) { neg = 1; v = (uint32_t)(-d); } else v = (uint32_t)d; }
            else v = va_arg(ap, uint32_t);
            uint32_t base = c == 'x' ? 16 : 10;
            char t[12];
            int k = 0;
            if (!v) t[k++] = '0';
            while (v) { t[k++] = "0123456789abcdef"[v % base]; v /= base; }
            if (neg && o < end) *o++ = '-';
            for (int i = k; i < width && o < end; i++) *o++ = zero ? '0' : ' ';
            while (k && o < end) *o++ = t[--k];
        } else {
            *o++ = c;
        }
    }
    *o = 0;
    return (int)(o - out);
}

void devlog(char tag, const char* fmt, ...) {
    char line[LOGW];
    va_list ap;
    va_start(ap, fmt);
    devlog_fmt(line, LOGW - 1, fmt, ap);
    va_end(ap);
    char* slot = LOG[log_n % LOGN];
    slot[0] = tag;
    int i = 0;
    while (line[i] && i < LOGW - 2) { slot[i + 1] = line[i]; i++; }
    slot[i + 1] = 0;
    log_n++;
    klog_write(tag, line);
    serial_putchar('['); serial_putchar(tag); serial_puts("] ");
    serial_puts(line);
    serial_puts("\n");
}

int devlog_tail(char tag, char* buf, int cap, int at, int maxlines) {
    int idx[LOGN], n = 0;
    int first = log_n > LOGN ? log_n - LOGN : 0;
    for (int i = first; i < log_n; i++) if (LOG[i % LOGN][0] == tag) idx[n++] = i;
    int from = n > maxlines ? n - maxlines : 0;
    for (int k = from; k < n; k++) {
        const char* s = LOG[idx[k] % LOGN] + 1;
        while (*s && at < cap - 2) buf[at++] = *s++;
        if (at < cap - 1) buf[at++] = '\n';
    }
    buf[at] = 0;
    return at;
}
