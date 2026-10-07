#ifndef QJS_STDIO_H
#define QJS_STDIO_H
// Freestanding libc shim: every stream is the serial log.
#include <stddef.h>
#include <stdarg.h>

typedef struct qjs_file { int fd; } FILE;
extern FILE* const qjs_stdout;
extern FILE* const qjs_stderr;
#define stdout qjs_stdout
#define stderr qjs_stderr
#define EOF (-1)

int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE* f, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE* f, const char* fmt, va_list ap);
int vprintf(const char* fmt, va_list ap);
int sprintf(char* buf, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int putchar(int c);
int fputc(int c, FILE* f);
int putc(int c, FILE* f);
int fputs(const char* s, FILE* f);
int puts(const char* s);
size_t fwrite(const void* p, size_t sz, size_t n, FILE* f);
int fflush(FILE* f);

#endif
