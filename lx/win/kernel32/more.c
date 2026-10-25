// The small API sets real programs touch at startup: registry (in memory), COM basics, shlwapi paths, WinRT strings,
// thread pool timers, SList, DPI. Everything here is deliberately simple; programs that need more fail at the call.
#include "k32.h"

extern DWORD GetFileAttributesW(LPCWSTR p);
extern int lstrlenW(LPCWSTR s);
extern DWORD GetTickCount(void);
extern void SetLastError(DWORD e);

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_INVALID_PARAMETER 87
#define E_NOTIMPL ((LONG)0x80004001)
#define E_OUTOFMEMORY ((LONG)0x8007000E)
#define E_INVALIDARG ((LONG)0x80070057)
#define REGDB_E_CLASSNOTREG ((LONG)0x80040154)

// ---- registry: a tree in memory (no file behind it) ------------------------------------------------------------------------------------
struct rvalue { WCHAR *name; DWORD type, size; BYTE *data; struct rvalue *next; };
struct rkey { WCHAR name[64]; struct rkey *parent, *child, *next; struct rvalue *values; int refs; };
static struct rkey roots[8];            // HKEY_CLASSES_ROOT .. HKEY_CURRENT_CONFIG (0x80000000 + i)
static int roots_init;

static int wcsieq(const WCHAR *a, const WCHAR *b) {
    for (;; a++, b++) {
        WCHAR x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static struct rkey *key_of(HANDLE h) {
    ULONG_PTR v = (ULONG_PTR)h;
    if (!roots_init) { roots_init = 1; for (int i = 0; i < 8; i++) { roots[i].refs = 1; } }
    if (v >= 0xFFFFFFFF80000000ull || (v >= 0x80000000ull && v < 0x80000008ull)) { int i = (int)(v & 7); return &roots[i]; }
    return (struct rkey*)h;
}

// follows or creates "a\b\c" below k
static struct rkey *key_walk(struct rkey *k, LPCWSTR path, int create) {
    while (k && path && *path) {
        WCHAR part[64]; int n = 0;
        while (*path == '\\') path++;
        while (*path && *path != '\\' && n < 63) part[n++] = *path++;
        part[n] = 0;
        if (!n) break;
        struct rkey *c = k->child;
        while (c && !wcsieq(c->name, part)) c = c->next;
        if (!c) {
            if (!create) return 0;
            c = (struct rkey*)k32_alloc(sizeof *c);
            if (!c) return 0;
            memset(c, 0, sizeof *c);
            memcpy(c->name, part, (n + 1) * 2);
            c->parent = k; c->next = k->child; k->child = c;
        }
        k = c;
    }
    return k;
}

static HANDLE hk_for(struct rkey *r) { return (r >= roots && r < roots + 8) ? (HANDLE)(ULONG_PTR)(0x80000000u + (DWORD)(r - roots)) : (HANDLE)r; }

EXPORT LONG RegOpenKeyExW(HANDLE hk, LPCWSTR sub, DWORD opt, DWORD sam, HANDLE *out) {
    struct rkey *k = key_of(hk);
    struct rkey *r = (sub && *sub) ? key_walk(k, sub, 0) : k;
    if (!r) return ERROR_FILE_NOT_FOUND;
    *out = hk_for(r);
    return ERROR_SUCCESS;
}

EXPORT LONG RegCreateKeyExW(HANDLE hk, LPCWSTR sub, DWORD res, LPWSTR cls, DWORD opt, DWORD sam, void *sa, HANDLE *out, DWORD *disp) {
    struct rkey *k = key_of(hk);
    struct rkey *r = key_walk(k, sub, 0);
    if (disp) *disp = r ? 2 : 1;
    if (!r) r = key_walk(k, sub, 1);
    if (!r) return E_OUTOFMEMORY;
    *out = hk_for(r);
    return ERROR_SUCCESS;
}

EXPORT LONG RegCreateKeyW(HANDLE hk, LPCWSTR sub, HANDLE *out) { return RegCreateKeyExW(hk, sub, 0, 0, 0, 0, 0, out, 0); }
EXPORT LONG RegCloseKey(HANDLE hk) { (void)hk; return ERROR_SUCCESS; }

static const WCHAR empty_w[1] = { 0 };
static struct rvalue *val_find(struct rkey *k, LPCWSTR name) {
    for (struct rvalue *v = k->values; v; v = v->next) if (wcsieq(v->name, name ? name : empty_w)) return v;
    return 0;
}

EXPORT LONG RegSetValueExW(HANDLE hk, LPCWSTR name, DWORD res, DWORD type, const BYTE *data, DWORD size) {
    struct rkey *k = key_of(hk);
    struct rvalue *v = val_find(k, name);
    if (!v) {
        v = (struct rvalue*)k32_alloc(sizeof *v);
        if (!v) return E_OUTOFMEMORY;
        memset(v, 0, sizeof *v);
        int n = name ? lstrlenW(name) : 0;
        v->name = (WCHAR*)k32_alloc((n + 1) * 2);
        if (n) memcpy(v->name, name, n * 2);
        v->name[n] = 0;
        v->next = k->values; k->values = v;
    }
    k32_free(v->data);
    v->data = (BYTE*)k32_alloc(size + 2);
    if (size) memcpy(v->data, data, size);
    v->data[size] = 0; v->data[size + 1] = 0;
    v->type = type; v->size = size;
    return ERROR_SUCCESS;
}

EXPORT LONG RegSetKeyValueW(HANDLE hk, LPCWSTR sub, LPCWSTR name, DWORD type, const void *data, DWORD size) {
    HANDLE h = hk;
    if (sub && *sub) { LONG r = RegCreateKeyExW(hk, sub, 0, 0, 0, 0, 0, &h, 0); if (r) return r; }
    return RegSetValueExW(h, name, 0, type, (const BYTE*)data, size);
}

EXPORT LONG RegQueryValueExW(HANDLE hk, LPCWSTR name, DWORD *res, DWORD *type, BYTE *data, DWORD *size) {
    struct rvalue *v = val_find(key_of(hk), name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    if (type) *type = v->type;
    if (!size) return ERROR_SUCCESS;
    if (data) {
        if (*size < v->size) { *size = v->size; return ERROR_MORE_DATA; }
        memcpy(data, v->data, v->size);
    }
    *size = v->size;
    return ERROR_SUCCESS;
}

EXPORT LONG RegGetValueW(HANDLE hk, LPCWSTR sub, LPCWSTR name, DWORD flags, DWORD *type, void *data, DWORD *size) {
    HANDLE h = hk;
    if (sub && *sub) { LONG r = RegOpenKeyExW(hk, sub, 0, 0, &h); if (r) return r; }
    return RegQueryValueExW(h, name, 0, type, (BYTE*)data, size);
}

EXPORT LONG RegEnumValueW(HANDLE hk, DWORD idx, LPWSTR name, DWORD *nlen, DWORD *res, DWORD *type, BYTE *data, DWORD *dlen) {
    struct rkey *k = key_of(hk);
    struct rvalue *v = k->values;
    int n = 0;
    for (struct rvalue *p = v; p; p = p->next) n++;
    if ((int)idx >= n) return ERROR_NO_MORE_ITEMS;
    v = k->values;
    for (int i = 0; i < n - 1 - (int)idx; i++) v = v->next;        // the list is newest first: report in creation order
    int l = lstrlenW(v->name);
    if ((DWORD)l + 1 > *nlen) return ERROR_MORE_DATA;
    memcpy(name, v->name, (l + 1) * 2);
    *nlen = (DWORD)l;
    if (type) *type = v->type;
    if (dlen) {
        if (data) { if (*dlen < v->size) { *dlen = v->size; return ERROR_MORE_DATA; } memcpy(data, v->data, v->size); }
        *dlen = v->size;
    }
    return ERROR_SUCCESS;
}

EXPORT LONG RegQueryInfoKeyW(HANDLE hk, LPWSTR cls, DWORD *clen, DWORD *res, DWORD *nsub, DWORD *maxsub, DWORD *maxcls, DWORD *nvals, DWORD *maxname, DWORD *maxdata, DWORD *sec, FILETIME *lw) {
    struct rkey *k = key_of(hk);
    DWORD ns = 0, nv = 0, mn = 0, md = 0;
    for (struct rkey *c = k->child; c; c = c->next) ns++;
    for (struct rvalue *v = k->values; v; v = v->next) { nv++; DWORD l = (DWORD)lstrlenW(v->name); if (l > mn) mn = l; if (v->size > md) md = v->size; }
    if (clen) *clen = 0;
    if (nsub) *nsub = ns;
    if (maxsub) *maxsub = 63;
    if (maxcls) *maxcls = 0;
    if (nvals) *nvals = nv;
    if (maxname) *maxname = mn;
    if (maxdata) *maxdata = md;
    if (sec) *sec = 0;
    if (lw) { lw->dwLowDateTime = lw->dwHighDateTime = 0; }
    return ERROR_SUCCESS;
}

EXPORT LONG RegDeleteKeyExW(HANDLE hk, LPCWSTR sub, DWORD sam, DWORD res) {
    struct rkey *k = key_of(hk);
    struct rkey *t = key_walk(k, sub, 0);
    if (!t || t == k) return ERROR_FILE_NOT_FOUND;
    struct rkey **pp = &t->parent->child;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    return ERROR_SUCCESS;
}

// ---- COM: no classes are registered ---------------------------------------------------------------------------------------------------------
EXPORT LONG CoInitializeEx(void *res, DWORD flags) { (void)res; (void)flags; return 0; }
EXPORT LONG CoInitialize(void *res) { (void)res; return 0; }
EXPORT void CoUninitialize(void) {}
EXPORT void *CoTaskMemAlloc(SIZE_T n) { return k32_alloc(n ? n : 1); }
EXPORT void CoTaskMemFree(void *p) { k32_free(p); }
EXPORT LONG CoCreateInstance(const void *clsid, void *outer, DWORD ctx, const void *iid, void **out) { (void)clsid; (void)outer; (void)ctx; (void)iid; if (out) *out = 0; return REGDB_E_CLASSNOTREG; }
EXPORT LONG CoCreateFreeThreadedMarshaler(void *outer, void **out) { (void)outer; if (out) *out = 0; return E_NOTIMPL; }
EXPORT LONG PropVariantClear(void *pv) { if (pv) memset(pv, 0, 16); return 0; }
EXPORT LONG CoWaitForMultipleHandles(DWORD flags, DWORD ms, DWORD n, HANDLE *h, DWORD *idx) { (void)flags; (void)ms; (void)n; (void)h; if (idx) *idx = 0; return 0; }
EXPORT LONG CoCreateGuid(BYTE *g) {
    static unsigned long long s;
    if (!s) s = (unsigned long long)GetTickCount() * 6364136223846793005ull + 1442695040888963407ull;
    for (int i = 0; i < 16; i += 8) { s = s * 6364136223846793005ull + 1442695040888963407ull; unsigned long long v = s >> 7; memcpy(g + i, &v, 8); }
    g[7] = (BYTE)((g[7] & 0x0F) | 0x40); g[8] = (BYTE)((g[8] & 0x3F) | 0x80);
    return 0;
}

// ---- WinRT: strings work, activation does not ------------------------------------------------------------------------------------------------
struct hstr { DWORD len; WCHAR *buf; };
EXPORT LONG WindowsCreateString(const WCHAR *src, DWORD len, void **out) {
    if (!len) { *out = 0; return 0; }
    struct hstr *h = (struct hstr*)k32_alloc(sizeof *h + (len + 1) * 2);
    if (!h) return E_OUTOFMEMORY;
    h->len = len; h->buf = (WCHAR*)(h + 1);
    memcpy(h->buf, src, len * 2);
    h->buf[len] = 0;
    *out = h;
    return 0;
}
EXPORT LONG WindowsCreateStringReference(const WCHAR *src, DWORD len, void *hdr, void **out) {
    struct hstr *h = (struct hstr*)hdr;
    h->len = len; h->buf = (WCHAR*)src;
    *out = h;
    return 0;
}
EXPORT LONG WindowsDeleteString(void *h) { if (h) k32_free(h); return 0; }
EXPORT const WCHAR *WindowsGetStringRawBuffer(void *h, DWORD *len) {
    static const WCHAR empty[1] = { 0 };
    struct hstr *s = (struct hstr*)h;
    if (len) *len = s ? s->len : 0;
    return s ? s->buf : empty;
}
EXPORT LONG RoGetActivationFactory(void *cls, const void *iid, void **out) { (void)cls; (void)iid; if (out) *out = 0; return REGDB_E_CLASSNOTREG; }
EXPORT LONG SetRestrictedErrorInfo(void *p) { (void)p; return 0; }
EXPORT LONG RoGetMatchingRestrictedErrorInfo(LONG hr, void **out) { (void)hr; if (out) *out = 0; return E_NOTIMPL; }

// ---- shlwapi / shcore ------------------------------------------------------------------------------------------------------------------------
EXPORT BOOL PathFileExistsW(LPCWSTR p) { return GetFileAttributesW(p) != 0xFFFFFFFFu; }
EXPORT BOOL PathIsFileSpecW(LPCWSTR p) { for (; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') return 0; return 1; }
EXPORT LPWSTR PathFindExtensionW(LPCWSTR p) {
    LPCWSTR dot = 0, s = p;
    for (; *s; s++) { if (*s == '.') dot = s; else if (*s == '\\' || *s == '/') dot = 0; }
    return (LPWSTR)(dot ? dot : s);
}
EXPORT LONG SHStrDupW(LPCWSTR s, LPWSTR *out) {
    int n = lstrlenW(s);
    *out = (LPWSTR)k32_alloc((n + 1) * 2);
    if (!*out) return E_OUTOFMEMORY;
    memcpy(*out, s, (n + 1) * 2);
    return 0;
}
EXPORT LONG GetDpiForMonitor(void *mon, int type, UINT *x, UINT *y) { (void)mon; (void)type; if (x) *x = 96; if (y) *y = 96; return 0; }

// ---- thread pool timers: accepted and never fired ---------------------------------------------------------------------------------------------------
EXPORT void *CreateThreadpoolTimer(void *cb, void *ctx, void *env) { (void)cb; (void)ctx; (void)env; return k32_alloc(16); }
EXPORT void SetThreadpoolTimer(void *t, void *due, DWORD period, DWORD window) { (void)t; (void)due; (void)period; (void)window; }
EXPORT void WaitForThreadpoolTimerCallbacks(void *t, BOOL cancel) { (void)t; (void)cancel; }
EXPORT void CloseThreadpoolTimer(void *t) { k32_free(t); }

EXPORT void InitializeSListHead(void *h) { memset(h, 0, 16); }

// ---- winspool / urlmon / propsys / advapi32 file encryption: present, failing ------------------------------------------------------------------------
EXPORT BOOL OpenPrinterW(LPWSTR name, HANDLE *out, void *def) { (void)name; (void)def; if (out) *out = 0; return 0; }
EXPORT BOOL ClosePrinter(HANDLE h) { (void)h; return 1; }
EXPORT BOOL GetPrinterDriverW(HANDLE h, LPWSTR env, DWORD lvl, BYTE *buf, DWORD n, DWORD *need) { (void)h; (void)env; (void)lvl; (void)buf; (void)n; if (need) *need = 0; return 0; }
EXPORT LONG FindMimeFromData(void *bc, LPCWSTR url, void *buf, DWORD size, LPCWSTR proposed, DWORD flags, LPWSTR *out, DWORD res) { (void)bc; (void)url; (void)buf; (void)size; (void)proposed; (void)flags; (void)res; if (out) *out = 0; return (LONG)0x80004005; }
EXPORT LONG PSGetPropertyDescriptionListFromString(LPCWSTR s, const void *iid, void **out) { (void)s; (void)iid; if (out) *out = 0; return E_NOTIMPL; }
EXPORT LONG PropVariantToStringVectorAlloc(const void *pv, LPWSTR **out, ULONG *n) { (void)pv; if (out) *out = 0; if (n) *n = 0; return E_NOTIMPL; }
EXPORT BOOL DecryptFileW(LPCWSTR f, DWORD r) { (void)f; (void)r; return 0; }
EXPORT DWORD DuplicateEncryptionInfoFile(LPCWSTR a, LPCWSTR b, DWORD c, DWORD d, void *sa) { (void)a; (void)b; (void)c; (void)d; (void)sa; return 1; }
