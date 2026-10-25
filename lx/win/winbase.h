// Common definitions of the Windows-side system DLLs (kernel32, msvcrt, user32, gdi32): Windows types, a few structures and
// raw Linux system calls (these DLLs run in a task of the Linux subsystem; see src/lx_pe.c).
#ifndef WINBASE_H
#define WINBASE_H

typedef unsigned char BYTE, UCHAR, *PBYTE;
typedef unsigned short WORD, USHORT, WCHAR, *PWSTR_;
typedef unsigned int DWORD, UINT, ULONG, BOOL_U;
typedef int LONG, INT, BOOL;
typedef long long LONGLONG, INT64;
typedef unsigned long long ULONGLONG, UINT64, ULONG_PTR, DWORD_PTR, SIZE_T, UINT_PTR;
typedef long long LONG_PTR, INT_PTR, SSIZE_T, LPARAM, LRESULT;
typedef unsigned long long WPARAM;
typedef short SHORT;
typedef void *HANDLE, *LPVOID, *PVOID, *HMODULE, *HINSTANCE, *HWND, *HDC, *HBRUSH, *HPEN, *HFONT, *HBITMAP, *HGDIOBJ, *HICON, *HCURSOR, *HMENU, *HLOCAL, *HGLOBAL, *HRSRC, *FARPROC_;
typedef const void *LPCVOID;
typedef char CHAR, *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef WCHAR *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWSTR, *PCWSTR;
typedef DWORD *LPDWORD, *PDWORD;
typedef BOOL *LPBOOL;
typedef BYTE *LPBYTE;
typedef long long (*FARPROC)(void);
typedef DWORD COLORREF;
typedef WORD ATOM;
typedef LONG NTSTATUS;

#define TRUE 1
#define FALSE 0
#define NULL ((void*)0)
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define WINAPI
#define CALLBACK
#define APIENTRY
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define EXPORT __attribute__((visibility("default"), used))

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 1
#define FILE_ATTRIBUTE_HIDDEN 2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_DATA 13
#define ERROR_OUTOFMEMORY 14
#define ERROR_NO_MORE_FILES 18
#define ERROR_SHARING_VIOLATION 32
#define ERROR_HANDLE_EOF 38
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_BROKEN_PIPE 109
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_INVALID_NAME 123
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_MORE_DATA 234
#define WAIT_OBJECT_0 0
#define WAIT_ABANDONED 0x80
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFFu

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 1
#define PAGE_READONLY 2
#define PAGE_READWRITE 4
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef union { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA;
typedef struct {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;
typedef struct {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
typedef struct { SHORT X, Y; } COORD;
typedef struct { SHORT Left, Top, Right, Bottom; } SMALL_RECT;
typedef struct { COORD dwSize, dwCursorPosition; WORD wAttributes; SMALL_RECT srWindow; COORD dwMaximumWindowSize; } CONSOLE_SCREEN_BUFFER_INFO;
typedef struct { WORD wProcessorArchitecture, wReserved; DWORD dwPageSize; LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress; DWORD_PTR dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO;
typedef struct { DWORD dwLength, dwMemoryLoad; ULONGLONG ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual; } MEMORYSTATUSEX;
typedef struct { DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOA;
typedef struct { DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct { LONG Data1_; } OSVERSION_;
typedef struct { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; CHAR szCSDVersion[128]; } OSVERSIONINFOA;
typedef struct { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; WCHAR szCSDVersion[128]; } OSVERSIONINFOW;

typedef struct _CRITICAL_SECTION { volatile int lock; int count; DWORD owner; DWORD waiters; void *pad[3]; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
typedef struct { void *Ptr; } SRWLOCK, CONDITION_VARIABLE, INIT_ONCE;

// ---- raw Linux system calls -------------------------------------------------------------------------------------------
static inline long sc_(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d; register long r8 __asm__("r8") = e; register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}
#define SC0(n) sc_(n, 0, 0, 0, 0, 0, 0)
#define SC1(n, a) sc_(n, (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b) sc_(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c) sc_(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SC4(n, a, b, c, d) sc_(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define SC6(n, a, b, c, d, e, f) sc_(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

#define LX_read 0
#define LX_write 1
#define LX_open 2
#define LX_close 3
#define LX_stat 4
#define LX_fstat 5
#define LX_lseek 8
#define LX_mmap 9
#define LX_mprotect 10
#define LX_munmap 11
#define LX_pipe 22
#define LX_dup 32
#define LX_nanosleep 35
#define LX_getpid 39
#define LX_clone 56
#define LX_fork 57
#define LX_execve 59
#define LX_exit 60
#define LX_wait4 61
#define LX_kill 62
#define LX_fcntl 72
#define LX_ftruncate 77
#define LX_getcwd 79
#define LX_chdir 80
#define LX_rename 82
#define LX_mkdir 83
#define LX_rmdir 84
#define LX_unlink 87
#define LX_gettimeofday 96
#define LX_arch_prctl 158
#define LX_gettid 186
#define LX_futex 202
#define LX_getdents64 217
#define LX_clock_gettime 228
#define LX_exit_group 231
#define LX_ioctl 16
#define LX_dup2 33

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_DIRECTORY 0200000

// Win32 last error (TEB + 0x68)
static inline DWORD *teb_lasterror(void) { DWORD *p; __asm__ volatile("mov %%gs:0x30, %0" : "=r"(p)); return (DWORD*)((char*)p + 0x68); }
static inline void *teb(void) { void *p; __asm__ volatile("mov %%gs:0x30, %0" : "=r"(p)); return p; }
static inline void set_error(DWORD e) { *teb_lasterror() = e; }

// translate a negative Linux errno into a Win32 error code
static inline DWORD win_error_of(long e) {
    switch (-e) {
    case 2: return ERROR_FILE_NOT_FOUND; case 13: case 1: return ERROR_ACCESS_DENIED; case 9: return ERROR_INVALID_HANDLE;
    case 12: return ERROR_NOT_ENOUGH_MEMORY; case 17: return ERROR_ALREADY_EXISTS; case 20: return ERROR_PATH_NOT_FOUND;
    case 21: return ERROR_ACCESS_DENIED; case 22: return ERROR_INVALID_PARAMETER; case 39: return ERROR_DIR_NOT_EMPTY;
    case 24: return ERROR_TOO_MANY_OPEN_FILES; case 32: return ERROR_BROKEN_PIPE; case 36: return ERROR_INVALID_NAME;
    default: return ERROR_ACCESS_DENIED;
    }
}

#endif
