// A Windows program that uses the C runtime and the Win32 API: formatted output, memory, files, sorting, math, time, threads
// and synchronisation. Prints "CRTDEMO PASS" when every check holds.
#include "crt.h"
#include "crtmain.h"

extern HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, DWORD (*fn)(void*), void *param, DWORD flags, DWORD *tid);
extern DWORD WaitForSingleObject(HANDLE h, DWORD ms);
extern BOOL CloseHandle(HANDLE h);
extern HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name);
extern BOOL SetEvent(HANDLE h);
extern void InitializeCriticalSection(LPCRITICAL_SECTION cs);
extern void EnterCriticalSection(LPCRITICAL_SECTION cs);
extern void LeaveCriticalSection(LPCRITICAL_SECTION cs);
extern DWORD GetCurrentThreadId(void);
extern DWORD TlsAlloc(void);
extern BOOL TlsSetValue(DWORD i, LPVOID v);
extern LPVOID TlsGetValue(DWORD i);
extern void Sleep(DWORD ms);
extern DWORD GetTickCount(void);
extern LPVOID VirtualAlloc(LPVOID a, SIZE_T s, DWORD t, DWORD p);
extern BOOL VirtualFree(LPVOID a, SIZE_T s, DWORD t);
extern HANDLE CreateFileA(LPCSTR n, DWORD a, DWORD sh, SECURITY_ATTRIBUTES *sa, DWORD d, DWORD f, HANDLE t);
extern BOOL WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, void *o);
extern BOOL ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD w, void *o);
extern DWORD GetFileSize(HANDLE h, LPDWORD hi);
extern BOOL DeleteFileA(LPCSTR n);
extern HANDLE FindFirstFileA(LPCSTR p, WIN32_FIND_DATAA *fd);
extern BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
extern BOOL FindClose(HANDLE h);
extern BOOL CreateDirectoryA(LPCSTR n, SECURITY_ATTRIBUTES *sa);
extern DWORD GetLastError(void);
extern DWORD GetFileAttributesA(LPCSTR n);
extern DWORD GetCurrentDirectoryA(DWORD n, LPSTR b);
extern DWORD GetEnvironmentVariableA(LPCSTR n, LPSTR b, DWORD sz);
extern BOOL CreateProcessA(LPCSTR app, LPSTR cmd, SECURITY_ATTRIBUTES *pa, SECURITY_ATTRIBUTES *ta, BOOL inh, DWORD flags, LPVOID env, LPCSTR dir, STARTUPINFOA *si, PROCESS_INFORMATION *pi);
extern BOOL GetExitCodeProcess(HANDLE h, DWORD *c);
extern HMODULE LoadLibraryA(LPCSTR n);
extern FARPROC GetProcAddress(HMODULE m, LPCSTR n);

static int fails;
#define CHECK(c, what) do { int ok_ = !!(c); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) fails++; } while (0)

static int cmp_int(const void *a, const void *b) { return *(const int*)a - *(const int*)b; }

static CRITICAL_SECTION cs;
static volatile int counter;
static HANDLE done_event;
static DWORD tls_slot;
static volatile int tls_seen[4];

static DWORD worker(void *arg) {
    int id = (int)(ULONG_PTR)arg;
    TlsSetValue(tls_slot, (void*)(ULONG_PTR)(1000 + id));
    for (int i = 0; i < 500; i++) { EnterCriticalSection(&cs); counter++; LeaveCriticalSection(&cs); }
    Sleep(5);
    tls_seen[id] = (int)(ULONG_PTR)TlsGetValue(tls_slot);
    if (id == 3) SetEvent(done_event);
    return (DWORD)(id * 11);
}

int main(int argc, char **argv) {
    char buf[256];
    CHECK(argc >= 1, "argc/argv");
    snprintf(buf, sizeof buf, "%d|%5d|%-5d|%05d|%x|%X|%#x|%o|%c|%s|%10s|%-10s|", 42, 42, 42, 42, 255, 255, 255, 8, 'z', "str", "right", "left");
    CHECK(strcmp(buf, "42|   42|42   |00042|ff|FF|0xff|10|z|str|     right|left      |") == 0, "printf integers and strings");
    snprintf(buf, sizeof buf, "%lld %llu %ld %hd", -9007199254740993ll, 18446744073709551615ull, -123456789l, (short)-5);
    CHECK(strcmp(buf, "-9007199254740993 18446744073709551615 -123456789 -5") == 0, "printf 64-bit");
    snprintf(buf, sizeof buf, "%f|%.2f|%10.3f|%e|%g|%g|%g", 3.14159265, 2.5, -1.0, 12345.6789, 0.0001, 100000.0, 1e-5);
    CHECK(strcmp(buf, "3.141593|2.50|    -1.000|1.234568e+04|0.0001|100000|1e-05") == 0, "printf floating point");
    if (strcmp(buf, "3.141593|2.50|    -1.000|1.234568e+04|0.0001|100000|1e-05") != 0) printf("  got: %s\n", buf);
    snprintf(buf, sizeof buf, "%.0f %.1f %.3f %f", 0.5, 0.25, 1e10, 123456789.125);
    CHECK(strcmp(buf, "0 0.2 10000000000.000 123456789.125000") == 0, "printf rounding");
    if (strcmp(buf, "0 0.2 10000000000.000 123456789.125000") != 0) printf("  got: %s\n", buf);

    int a = 0, b = 0; char word[32]; double d = 0;
    int n = sscanf("12 -7 hello 2.5", "%d %d %s %lf", &a, &b, word, &d);
    CHECK(n == 4 && a == 12 && b == -7 && strcmp(word, "hello") == 0 && d == 2.5, "sscanf");

    int arr[] = { 9, 3, 7, 1, 8, 2, 6, 4, 5, 0, 15, 11, 14, 12, 13, 10 };
    qsort(arr, 16, sizeof arr[0], cmp_int);
    int sorted = 1; for (int i = 0; i < 16; i++) if (arr[i] != i) sorted = 0;
    CHECK(sorted, "qsort");
    int key = 11; CHECK(bsearch(&key, arr, 16, sizeof arr[0], cmp_int) == &arr[11], "bsearch");

    char *p = (char*)malloc(100000); memset(p, 0x5A, 100000); p = (char*)realloc(p, 300000);
    CHECK(p && p[99999] == 0x5A, "malloc/realloc keeps data"); free(p);

    CHECK(fabs(sqrt(2.0) - 1.4142135623730951) < 1e-12, "sqrt");
    CHECK(fabs(sin(1.0) - 0.8414709848078965) < 1e-12 && fabs(cos(1.0) - 0.5403023058681398) < 1e-12, "sin/cos");
    CHECK(fabs(exp(1.0) - 2.718281828459045) < 1e-12 && fabs(log(10.0) - 2.302585092994046) < 1e-12, "exp/log");
    CHECK(fabs(pow(2.0, 0.5) - 1.4142135623730951) < 1e-12 && pow(2.0, 10.0) == 1024.0, "pow");
    CHECK(floor(-1.5) == -2.0 && ceil(1.2) == 2.0 && fmod(7.5, 2.0) == 1.5, "floor/ceil/fmod");

    time_t t = 86400ll * 365; struct tm *tm = gmtime(&t);
    char tb[64]; strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", tm);
    CHECK(strcmp(tb, "1971-01-01 00:00:00") == 0, "gmtime/strftime");

    // files through the C runtime
    FILE *f = fopen("/tmp/crtdemo.txt", "w");
    CHECK(f != 0, "fopen for writing");
    if (f) { fprintf(f, "line one\nnumber %d\n", 2024); fclose(f); }
    f = fopen("/tmp/crtdemo.txt", "r");
    char l1[64] = "", l2[64] = "";
    if (f) { fgets(l1, sizeof l1, f); fgets(l2, sizeof l2, f); fclose(f); }
    CHECK(strcmp(l1, "line one\n") == 0 && strcmp(l2, "number 2024\n") == 0, "fgets reads what fprintf wrote");

    // files through the Win32 API
    HANDLE h = CreateFileA("C:\\tmp\\w32.bin", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    DWORD w = 0;
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h, "windows", 7, &w, 0) && w == 7, "CreateFile/WriteFile with a Windows path");
    CloseHandle(h);
    h = CreateFileA("c:\\TMP\\W32.BIN", GENERIC_READ, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    char rb[16] = {0}; DWORD rd = 0;
    CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, rb, 15, &rd, 0) && rd == 7 && GetFileSize(h, 0) == 7 && strcmp(rb, "windows") == 0, "paths are case-insensitive");
    CloseHandle(h);
    CHECK(GetFileAttributesA("C:\\tmp") & FILE_ATTRIBUTE_DIRECTORY, "GetFileAttributes of a directory");
    WIN32_FIND_DATAA fd; int found = 0;
    HANDLE fh = FindFirstFileA("C:\\tmp\\*.bin", &fd);
    if (fh != INVALID_HANDLE_VALUE) { do { if (_stricmp(fd.cFileName, "w32.bin") == 0) found = 1; } while (FindNextFileA(fh, &fd)); FindClose(fh); }
    CHECK(found, "FindFirstFile/FindNextFile with a wildcard");
    CHECK(DeleteFileA("C:\\tmp\\w32.bin") && GetFileAttributesA("C:\\tmp\\w32.bin") == INVALID_FILE_ATTRIBUTES, "DeleteFile");

    // memory
    char *v = (char*)VirtualAlloc(0, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(v != 0, "VirtualAlloc"); if (v) { v[0] = 1; v[(1 << 20) - 1] = 2; VirtualFree(v, 1 << 20, MEM_RELEASE); }

    // threads, critical sections, events, TLS
    InitializeCriticalSection(&cs);
    done_event = CreateEventA(0, TRUE, FALSE, 0);
    tls_slot = TlsAlloc();
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(0, 0, worker, (void*)(ULONG_PTR)i, 0, 0);
    int joined = 1; DWORD codes = 0;
    for (int i = 0; i < 4; i++) { if (WaitForSingleObject(th[i], 10000) != WAIT_OBJECT_0) joined = 0; DWORD c = 0; extern BOOL GetExitCodeThread(HANDLE, DWORD*); GetExitCodeThread(th[i], &c); codes += c; CloseHandle(th[i]); }
    CHECK(joined && counter == 2000, "four threads with a critical section");
    CHECK(codes == 0 + 11 + 22 + 33, "thread exit codes");
    CHECK(WaitForSingleObject(done_event, 100) == WAIT_OBJECT_0, "event signalled by a thread");
    CHECK(tls_seen[0] == 1000 && tls_seen[1] == 1001 && tls_seen[2] == 1002 && tls_seen[3] == 1003, "TLS slots are per thread");
    DWORD t0 = GetTickCount(); Sleep(50); DWORD t1 = GetTickCount();
    CHECK(t1 - t0 >= 40 && t1 - t0 < 400, "Sleep and GetTickCount");

    // a child process: this program again with an argument
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    char cmd[300]; GetEnvironmentVariableA("PATH", buf, sizeof buf);
    strcpy(cmd, argv[0]); strcat(cmd, " child");
    if (argc == 1) {
        BOOL ok = CreateProcessA(argv[0], cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi);
        DWORD code = 0;
        if (ok) { WaitForSingleObject(pi.hProcess, 20000); GetExitCodeProcess(pi.hProcess, &code); }
        CHECK(ok && code == 42, "CreateProcess, WaitForSingleObject and the exit code");
    }

    HMODULE k = LoadLibraryA("kernel32.dll");
    FARPROC gp = k ? GetProcAddress(k, "GetCurrentProcessId") : 0;
    CHECK(gp != 0 && ((DWORD (*)(void))gp)() > 0, "LoadLibrary and GetProcAddress");

    if (argc > 1) return 42;
    printf("%s\n", fails ? "CRTDEMO FAIL" : "CRTDEMO PASS");
    return fails != 0;
}
