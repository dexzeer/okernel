// Internal header of our msvcrt.dll (also usable by programs that link against it).
#ifndef CRT_H
#define CRT_H
#include "winbase.h"

typedef unsigned long long size_t_;
#define size_t SIZE_T
typedef long long ptrdiff_t_;
typedef long long time_t;
typedef long long clock_t_;
typedef long long off_t_;

typedef struct _FILE {
    int fd;
    int flags;                  // 1 read, 2 write, 4 eof, 8 error, 16 line buffered, 32 unbuffered, 64 string stream, 128 append
    unsigned char *buf;
    int rpos, rlen, wlen, cap;
    int unget;
    int owned_buf;
} FILE;

#define _IOREAD 1
#define _IOWRT 2
#define _IOEOF 4
#define _IOERR 8
#define _IOLBF 16
#define _IONBF 32
#define EOF (-1)
#define BUFSIZ 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define RAND_MAX 0x7fff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#define ENOENT 2
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define ERANGE 34
#define EDOM 33

struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };

typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_end(a) __builtin_va_end(a)
#define va_arg(a, t) __builtin_va_arg(a, t)

// kernel32 pieces msvcrt uses
extern LPVOID HeapAlloc(HANDLE h, DWORD flags, SIZE_T n);
extern LPVOID HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T n);
extern BOOL HeapFree(HANDLE h, DWORD flags, LPVOID p);
extern SIZE_T HeapSize(HANDLE h, DWORD flags, LPCVOID p);
extern HANDLE GetProcessHeap(void);
extern void ExitProcess(UINT code);

int *_errno(void);
int printf(const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void *malloc(size_t n);
void free(void *p);
// ---- declarations of what msvcrt.dll offers (the part programs here use) ----
void *calloc(size_t n, size_t m); void *realloc(void *p, size_t n);
void *memcpy(void *d, const void *s, size_t n); void *memmove(void *d, const void *s, size_t n); void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n); void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s); char *strcpy(char *d, const char *s); char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s); char *strncat(char *d, const char *s, size_t n);
int strcmp(const char *a, const char *b); int strncmp(const char *a, const char *b, size_t n); int _stricmp(const char *a, const char *b); int _strnicmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c); char *strrchr(const char *s, int c); char *strstr(const char *h, const char *n); char *strtok(char *s, const char *d);
char *_strdup(const char *s); char *strerror(int e);
size_t wcslen(const WCHAR *s);
int isalpha(int c); int isdigit(int c); int isalnum(int c); int isspace(int c); int isupper(int c); int islower(int c); int isxdigit(int c); int ispunct(int c);
int toupper(int c); int tolower(int c);
int abs(int x); long long _strtoi64(const char *s, char **e, int b); long strtol(const char *s, char **e, int b); unsigned long strtoul(const char *s, char **e, int b);
double strtod(const char *s, char **e); int atoi(const char *s); double atof(const char *s); char *getenv(const char *n);
void srand(unsigned s); int rand(void); void exit(int c); void abort(void); int atexit(void (*f)(void));
void qsort(void *b, size_t n, size_t sz, int (*cmp)(const void*, const void*)); void *bsearch(const void *k, const void *b, size_t n, size_t sz, int (*cmp)(const void*, const void*));
double fabs(double x); double sqrt(double x); double floor(double x); double ceil(double x); double fmod(double x, double y);
double sin(double x); double cos(double x); double tan(double x); double atan(double x); double atan2(double y, double x);
double exp(double x); double log(double x); double log10(double x); double pow(double x, double y);
time_t time(time_t *t); struct tm *gmtime(const time_t *t); struct tm *localtime(const time_t *t); time_t mktime(struct tm *tm);
size_t strftime(char *b, size_t n, const char *f, const struct tm *tm); long clock(void);
FILE *fopen(const char *p, const char *m); int fclose(FILE *f); int fflush(FILE *f);
int fprintf(FILE *f, const char *fmt, ...); int sprintf(char *b, const char *fmt, ...); int snprintf(char *b, size_t n, const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...); int fscanf(FILE *f, const char *fmt, ...); int scanf(const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap); int vsprintf(char *b, const char *fmt, va_list ap);
char *fgets(char *s, int n, FILE *f); int fputs(const char *s, FILE *f); int puts(const char *s); int fputc(int c, FILE *f); int putchar(int c); int fgetc(FILE *f); int getchar(void);
size_t fread(void *p, size_t sz, size_t n, FILE *f); size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int fseek(FILE *f, long off, int w); long ftell(FILE *f); void rewind(FILE *f); int remove(const char *p); int rename(const char *a, const char *b);
extern FILE *__acrt_iob_func(unsigned i);
#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#endif
