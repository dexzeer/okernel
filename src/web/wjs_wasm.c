// wjs_wasm.c - natives behind the WebAssembly JS API (the API classes live in the prelude). Objects are handed to JS as
// small integer ids; values cross as ArrayBuffers of 8-byte little-endian slots (so i64 and float bit patterns survive),
// linear memory is an ArrayBuffer owned by the realm that the interpreter reads and writes directly.

#include "wjs_int.h"
#include "wasm.h"

#define JSX(ctx) ((struct wjs*)ojs_get_opaque(ctx))
#define NATIVE(name) static ojsv name(ojs* ctx, ojsv this_val, int argc, ojsv* argv)
#define UNUSED_THIS (void)this_val

struct wwasm {
    struct wasm_store* st;
    struct wasm_fn** fns; int nfns, capfns;
    struct wasm_mem** mems; int nmems, capmems;
    ojsv* membuf;                 // one ArrayBuffer per memory (GC root range)
    struct wasm_tab** tabs; int ntabs, captabs;
    struct wasm_glob** globs; int nglobs, capglobs;
    struct wasm_module** mods; int nmods, capmods;
    struct wasm_inst** insts; int ninsts, capinsts;
    struct wjs* js;
    ojsv hostfn;
};

static char type_char(uint8_t t) {
    switch (t) { case WT_I32: return 'i'; case WT_I64: return 'I'; case WT_F32: return 'f'; case WT_F64: return 'F'; case WT_FUNCREF: return 'r'; default: return 'e'; }
}
static int char_type(int c) {
    switch (c) { case 'i': return WT_I32; case 'I': return WT_I64; case 'f': return WT_F32; case 'F': return WT_F64; case 'r': return WT_FUNCREF; case 'e': return WT_EXTERNREF; }
    return 0;
}
static int sig_of(const struct wasm_type* t, char* out) {
    int n = 0;
    for (int i = 0; i < t->np; i++) out[n++] = type_char(t->p[i]);
    out[n++] = '>';
    for (int i = 0; i < t->nr; i++) out[n++] = type_char(t->r[i]);
    out[n] = 0;
    return n;
}

#define ARR_PUSH(arr, n, cap, T, v) do { if ((n) >= (cap)) { int nc = (cap) ? (cap) * 2 : 32; T* na = (T*)w_realloc((arr), (size_t)nc * sizeof(T)); if (!na) return 0; (arr) = na; (cap) = nc; } (arr)[(n)++] = (v); } while (0)

static uint32_t fn_id(struct wwasm* w, struct wasm_fn* f) {
    if (!f) return 0;
    if (!f->user) { ARR_PUSH(w->fns, w->nfns, w->capfns, struct wasm_fn*, f); f->user = (void*)(uintptr_t)w->nfns; }
    return (uint32_t)(uintptr_t)f->user;
}
static uint32_t tab_id(struct wwasm* w, struct wasm_tab* t) {
    if (!t->user) { ARR_PUSH(w->tabs, w->ntabs, w->captabs, struct wasm_tab*, t); t->user = (void*)(uintptr_t)w->ntabs; }
    return (uint32_t)(uintptr_t)t->user;
}
static uint32_t glob_id(struct wwasm* w, struct wasm_glob* g) {
    if (!g->user) { ARR_PUSH(w->globs, w->nglobs, w->capglobs, struct wasm_glob*, g); g->user = (void*)(uintptr_t)w->nglobs; }
    return (uint32_t)(uintptr_t)g->user;
}
struct wjs; ojsv* wjs_grow_roots(struct wjs* js, ojsv** arr, int cap, int nc);
static uint32_t mem_id(struct wwasm* w, struct wasm_mem* m) {
    if (!m->user) {
        if (w->nmems >= w->capmems) {
            int nc = w->capmems ? w->capmems * 2 : 64;
            struct wasm_mem** nm = (struct wasm_mem**)w_realloc(w->mems, (size_t)nc * sizeof(void*));
            if (!nm) return 0;
            w->mems = nm;
            if (!wjs_grow_roots(w->js, &w->membuf, w->capmems, nc)) return 0;
            w->capmems = nc;
        }
        w->mems[w->nmems++] = m;
        m->user = (void*)(uintptr_t)w->nmems;
    }
    return (uint32_t)(uintptr_t)m->user;
}
static struct wasm_fn* fn_of(struct wwasm* w, uint32_t id) { return id >= 1 && id <= (uint32_t)w->nfns ? w->fns[id - 1] : 0; }
static struct wasm_tab* tab_of(struct wwasm* w, uint32_t id) { return id >= 1 && id <= (uint32_t)w->ntabs ? w->tabs[id - 1] : 0; }
static struct wasm_glob* glob_of(struct wwasm* w, uint32_t id) { return id >= 1 && id <= (uint32_t)w->nglobs ? w->globs[id - 1] : 0; }
static struct wasm_mem* mem_of(struct wwasm* w, uint32_t id) { return id >= 1 && id <= (uint32_t)w->nmems ? w->mems[id - 1] : 0; }

// ---- host callbacks of the interpreter ----

static void slots_to_buf(const wval* v, const uint8_t* types, int n, uint8_t* out, struct wwasm* w) {
    for (int i = 0; i < n; i++) {
        uint64_t x = 0;
        switch (types[i]) {
        case WT_I32: case WT_F32: x = v[i].u32; break;
        case WT_I64: case WT_F64: x = v[i].u64; break;
        case WT_FUNCREF: x = fn_id(w, (struct wasm_fn*)v[i].ref); break;
        default: x = (uint32_t)(uintptr_t)v[i].ref; break;
        }
        memcpy(out + (size_t)i * 8, &x, 8);
    }
}
static void buf_to_slots(const uint8_t* in, const uint8_t* types, int n, wval* v, struct wwasm* w) {
    for (int i = 0; i < n; i++) {
        uint64_t x;
        memcpy(&x, in + (size_t)i * 8, 8);
        v[i].u64 = 0;
        switch (types[i]) {
        case WT_I32: case WT_F32: v[i].u32 = (uint32_t)x; break;
        case WT_I64: case WT_F64: v[i].u64 = x; break;
        case WT_FUNCREF: v[i].ref = fn_of(w, (uint32_t)x); break;
        default: v[i].ref = (void*)(uintptr_t)(uint32_t)x; break;
        }
    }
}

static int host_call(void* ud, struct wasm_fn* fn, const wval* args, wval* results) {
    struct wwasm* w = (struct wwasm*)ud;
    ojs* ctx = w->js->J;
    uint8_t tmp[260 * 8];
    int np = fn->type->np, nr = fn->type->nr;
    slots_to_buf(args, fn->type->p, np, tmp, w);
    ojsv ab = ojs_arraybuffer_copy(ctx, tmp, (size_t)np * 8);
    if (ojs_is_exception(ab)) return WTRAP_HOST;
    ojsv a[2] = { ojs_int(fn->host), ab };
    ojsv r = ojs_call(ctx, w->hostfn, OJS_UNDEFINED, 2, a);
    if (ojs_is_exception(r)) return WTRAP_HOST;
    if (nr) {
        size_t len = 0;
        uint8_t* p = ojs_is_object(r) ? ojs_bytes(ctx, r, &len) : 0;
        if (!p || len < (size_t)nr * 8) { ojs_throw_type_error(ctx, "host function returned the wrong number of results"); return WTRAP_HOST; }
        buf_to_slots(p, fn->type->r, nr, results, w);
    }
    return 0;
}

static int host_poll(void* ud) {
    struct wwasm* w = (struct wwasm*)ud;
    return wjs_interrupt_poll(w->js);
}

static int host_mem_grow(void* ud, struct wasm_mem* m, uint32_t new_pages) {
    struct wwasm* w = (struct wwasm*)ud;
    ojs* ctx = w->js->J;
    uint32_t id = mem_id(w, m);
    if (!id || new_pages > 32767) return -1;
    size_t bytes = (size_t)new_pages * 65536u;
    ojsv nb;
    ojsv len = ojs_number((double)bytes);
    if (!m->data) {
        ojsv ctor = ojs_get(ctx, ojs_global(ctx), "ArrayBuffer");
        if (ojs_is_exception(ctor)) { ojs_take_exception(ctx); return -1; }
        nb = ojs_new_instance(ctx, ctor, 1, &len);
    } else {
        ojsv old = w->membuf[id - 1];
        ojsv fn = ojs_get(ctx, old, "transfer");
        if (ojs_is_exception(fn)) { ojs_take_exception(ctx); return -1; }
        nb = ojs_call(ctx, fn, old, 1, &len);
    }
    if (ojs_is_exception(nb)) { ojs_take_exception(ctx); return -1; }
    size_t got = 0;
    uint8_t* p = ojs_bytes(ctx, nb, &got);
    if (!p && bytes) return -1;
    if (bytes && got < bytes) return -1;
    w->membuf[id - 1] = nb;
    m->data = p;
    m->len = (uint32_t)bytes;
    m->pages = new_pages;
    return 0;
}

static struct wwasm* get_w(struct wjs* js) {
    if (js->wasm) return js->wasm;
    struct wwasm* w = (struct wwasm*)w_calloc(1, sizeof *w);
    if (!w) return 0;
    struct wasm_host h = { host_call, host_poll, host_mem_grow, w };
    w->st = wasm_store_new(&h);
    if (!w->st) { w_free(w); return 0; }
    w->js = js;
    w->hostfn = OJS_UNDEFINED;
    ojs_add_root(js->J, &w->hostfn);
    js->wasm = w;
    return w;
}

void wjs_wasm_free(struct wjs* js) {
    struct wwasm* w = js->wasm;
    if (!w) return;
    wasm_store_free(w->st);
    w_free(w->fns); w_free(w->tabs); w_free(w->globs); w_free(w->mods); w_free(w->insts); w_free(w->mems); w_free(w->membuf);
    w_free(w);
    js->wasm = 0;
}

#define WW(var) struct wjs* js = JSX(ctx); UNUSED_THIS; struct wwasm* var = get_w(js); if (!var) return ojs_throw_oom(ctx)

static void msg_cat(char* d, int* n, int cap, const char* s) { while (*s && *n < cap - 1) d[(*n)++] = *s++; d[*n] = 0; }
static void msg_num(char* d, int* n, int cap, uint32_t v) { char t[12]; int k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k && *n < cap - 1) d[(*n)++] = t[--k]; d[*n] = 0; }
static ojsv str_arr2(ojs* ctx, const char* a, const char* b) { ojsv r = ojs_array(ctx); ojs_set_index(ctx, r, 0, ojs_string(ctx, a)); ojs_set_index(ctx, r, 1, ojs_string(ctx, b)); return r; }

// wasmSetHost(fn)
NATIVE(n_set_host) { WW(w); if (argc > 0) w->hostfn = argv[0]; return OJS_UNDEFINED; }

// wasmModule(bytes) -> id | error string
NATIVE(n_module) {
    WW(w);
    size_t len = 0;
    uint8_t* p = argc > 0 ? ojs_bytes(ctx, argv[0], &len) : 0;
    if (!p) return ojs_string(ctx, "BufferSource argument is empty or too short");
    struct wasm_module* m = 0;
    char err[200];
    if (wasm_module_new(w->st, p, len, &m, err, sizeof err) < 0) return ojs_string(ctx, err);
    if (w->nmods >= w->capmods) {
        int nc = w->capmods ? w->capmods * 2 : 16;
        struct wasm_module** na = (struct wasm_module**)w_realloc(w->mods, (size_t)nc * sizeof(void*));
        if (!na) return ojs_throw_oom(ctx);
        w->mods = na; w->capmods = nc;
    }
    w->mods[w->nmods++] = m;
    return ojs_int(w->nmods);
}

static struct wasm_module* mod_of(struct wwasm* w, ojs* ctx, int argc, ojsv* argv) {
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0 || id < 1 || id > w->nmods) return 0;
    return w->mods[id - 1];
}

// wasmImports(mod) -> [[module, name, kind, a, b, c], ...]
NATIVE(n_imports) {
    WW(w);
    struct wasm_module* m = mod_of(w, ctx, argc, argv);
    if (!m) return OJS_NULL;
    ojsv out = ojs_array(ctx);
    for (uint32_t i = 0; i < m->nimports; i++) {
        struct wasm_import* im = &m->imports[i];
        ojsv e = ojs_array(ctx);
        ojs_set_index(ctx, e, 0, ojs_string(ctx, im->module));
        ojs_set_index(ctx, e, 1, ojs_string(ctx, im->name));
        ojs_set_index(ctx, e, 2, ojs_int(im->kind));
        if (im->kind == WK_FUNC) { char sg[600]; sig_of(&m->types[im->type], sg); ojs_set_index(ctx, e, 3, ojs_string(ctx, sg)); }
        else if (im->kind == WK_TABLE) {
            char c[2] = { type_char(im->table.type), 0 };
            ojs_set_index(ctx, e, 3, ojs_string(ctx, c)); ojs_set_index(ctx, e, 4, ojs_number(im->table.lim.min)); ojs_set_index(ctx, e, 5, ojs_number(im->table.lim.has_max ? (double)im->table.lim.max : -1));
        } else if (im->kind == WK_MEM) {
            ojs_set_index(ctx, e, 3, ojs_number(im->mem.min)); ojs_set_index(ctx, e, 4, ojs_number(im->mem.has_max ? (double)im->mem.max : -1));
        } else {
            char c[2] = { type_char(im->gtype), 0 };
            ojs_set_index(ctx, e, 3, ojs_string(ctx, c)); ojs_set_index(ctx, e, 4, ojs_int(im->gmut));
        }
        ojs_set_index(ctx, out, i, e);
    }
    return out;
}

// wasmExports(mod) -> [[name, kind], ...]
NATIVE(n_exports) {
    WW(w);
    struct wasm_module* m = mod_of(w, ctx, argc, argv);
    if (!m) return OJS_NULL;
    ojsv out = ojs_array(ctx);
    for (uint32_t i = 0; i < m->nexports; i++) {
        ojsv e = ojs_array(ctx);
        ojs_set_index(ctx, e, 0, ojs_string(ctx, m->exports[i].name));
        ojs_set_index(ctx, e, 1, ojs_int(m->exports[i].kind));
        ojs_set_index(ctx, out, i, e);
    }
    return out;
}

// wasmCustom(mod, name) -> [ArrayBuffer, ...]
NATIVE(n_custom) {
    WW(w);
    struct wasm_module* m = mod_of(w, ctx, argc, argv);
    if (!m || argc < 2) return ojs_array(ctx);
    char* name = ojs_to_cstring(ctx, argv[1], 0);
    if (!name) return OJS_EXCEPTION;
    ojsv out = ojs_array(ctx);
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->ncustoms; i++)
        if (!strcmp(m->customs[i].name, name)) ojs_set_index(ctx, out, k++, ojs_arraybuffer_copy(ctx, m->customs[i].bytes, m->customs[i].len));
    ojs_free_cstring(ctx, name);
    return out;
}

// wasmHostFn(sig, hostId) -> fn id
NATIVE(n_host_fn) {
    WW(w);
    char* sg = argc > 0 ? ojs_to_cstring(ctx, argv[0], 0) : 0;
    int32_t hid = 0;
    if (!sg || argc < 2 || ojs_to_int32(ctx, &hid, argv[1]) < 0) { if (sg) ojs_free_cstring(ctx, sg); return ojs_int(0); }
    uint8_t p[260], r[260];
    int np = 0, nr = 0, after = 0;
    for (const char* c = sg; *c; c++) {
        if (*c == '>') { after = 1; continue; }
        int t = char_type(*c);
        if (!t) continue;
        if (after) { if (nr < 255) r[nr++] = (uint8_t)t; } else if (np < 255) p[np++] = (uint8_t)t;
    }
    ojs_free_cstring(ctx, sg);
    struct wasm_fn* f = wasm_host_fn_new(w->st, p, np, r, nr, hid, 0);
    if (!f) return ojs_int(0);
    return ojs_number(fn_id(w, f));
}

// wasmFnSig(fn) -> "ii>i"
NATIVE(n_fn_sig) {
    WW(w);
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_NULL;
    struct wasm_fn* f = fn_of(w, (uint32_t)id);
    if (!f) return OJS_NULL;
    char sg[600];
    sig_of(f->type, sg);
    return ojs_string(ctx, sg);
}

// wasmInstantiate(mod, [import ids...]) -> [0, exports] | [1, link message] | [2, trap message]
NATIVE(n_instantiate) {
    WW(w);
    struct wasm_module* m = mod_of(w, ctx, argc, argv);
    if (!m || argc < 2) return OJS_NULL;
    void** imps = (void**)w_calloc(m->nimports ? m->nimports : 1, sizeof(void*));
    if (!imps) return ojs_throw_oom(ctx);
    for (uint32_t i = 0; i < m->nimports; i++) {
        ojsv v = ojs_get_index(ctx, argv[1], i);
        int32_t id = 0;
        ojs_to_int32(ctx, &id, v);
        void* obj = 0;
        switch (m->imports[i].kind) {
        case WK_FUNC: obj = fn_of(w, (uint32_t)id); break;
        case WK_TABLE: obj = tab_of(w, (uint32_t)id); break;
        case WK_MEM: obj = mem_of(w, (uint32_t)id); break;
        case WK_GLOBAL: obj = glob_of(w, (uint32_t)id); break;
        }
        const char* why = obj ? wasm_import_ok(m, i, obj) : "import is missing";
        if (why) {
            char msg[300];
            int mn = 0;
            msg_cat(msg, &mn, sizeof msg, "Import #");
            msg_num(msg, &mn, sizeof msg, i);
            msg_cat(msg, &mn, sizeof msg, " module=\"");
            msg_cat(msg, &mn, sizeof msg, m->imports[i].module);
            msg_cat(msg, &mn, sizeof msg, "\" function=\"");
            msg_cat(msg, &mn, sizeof msg, m->imports[i].name);
            msg_cat(msg, &mn, sizeof msg, "\": ");
            msg_cat(msg, &mn, sizeof msg, why);
            w_free(imps);
            return str_arr2(ctx, "1", msg);
        }
        imps[i] = obj;
    }
    int trap = 0;
    char err[200];
    struct wasm_inst* in = wasm_instantiate(w->st, m, imps, &trap, err, sizeof err);
    w_free(imps);
    if (!in) return trap < 0 ? str_arr2(ctx, "1", err) : str_arr2(ctx, "2", "instantiation failed");
    if (trap > 0) return str_arr2(ctx, "2", wasm_trap_name(trap));
    // exports first (JS builds them), the start function runs after the caller has the objects it needs: do it here
    if (m->has_start) {
        wval none[1];
        int r = wasm_call(w->st, in->funcs[m->start], 0, none);
        if (r == WTRAP_HOST) return OJS_EXCEPTION;
        if (r == WTRAP_INTERRUPTED) return ojs_throw_range_error(ctx, "interrupted");
        if (r) return str_arr2(ctx, "2", wasm_trap_name(r));
    }
    ojsv list = ojs_array(ctx);
    for (uint32_t i = 0; i < m->nexports; i++) {
        struct wasm_export* e = &m->exports[i];
        ojsv ent = ojs_array(ctx);
        ojs_set_index(ctx, ent, 0, ojs_string(ctx, e->name));
        ojs_set_index(ctx, ent, 1, ojs_int(e->kind));
        switch (e->kind) {
        case WK_FUNC: {
            struct wasm_fn* f = in->funcs[e->idx];
            char sg[600];
            sig_of(f->type, sg);
            ojs_set_index(ctx, ent, 2, ojs_number(fn_id(w, f)));
            ojs_set_index(ctx, ent, 3, ojs_string(ctx, sg));
            ojs_set_index(ctx, ent, 4, ojs_number(e->idx));
            break;
        }
        case WK_TABLE: ojs_set_index(ctx, ent, 2, ojs_number(tab_id(w, in->tabs[e->idx]))); break;
        case WK_MEM: ojs_set_index(ctx, ent, 2, ojs_number(mem_id(w, in->mems[e->idx]))); break;
        case WK_GLOBAL: ojs_set_index(ctx, ent, 2, ojs_number(glob_id(w, in->globs[e->idx]))); break;
        }
        ojs_set_index(ctx, list, i, ent);
    }
    ojsv res = ojs_array(ctx);
    ojs_set_index(ctx, res, 0, ojs_string(ctx, "0"));
    ojs_set_index(ctx, res, 1, list);
    return res;
}

// wasmCall(fn, argsBuffer) -> ArrayBuffer (results) | trap message string
NATIVE(n_call) {
    WW(w);
    int32_t id = 0;
    if (argc < 2 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_NULL;
    struct wasm_fn* f = fn_of(w, (uint32_t)id);
    if (!f) return OJS_NULL;
    size_t len = 0;
    uint8_t* p = ojs_bytes(ctx, argv[1], &len);
    int np = f->type->np, nr = f->type->nr;
    if ((!p && np) || len < (size_t)np * 8) return OJS_NULL;
    wval args[260], res[260];
    buf_to_slots(p, f->type->p, np, args, w);
    int r = wasm_call(w->st, f, args, res);
    if (r == WTRAP_HOST) return OJS_EXCEPTION;
    if (r == WTRAP_INTERRUPTED) return ojs_throw_range_error(ctx, "interrupted");
    if (r) return ojs_string(ctx, wasm_trap_name(r));
    uint8_t out[260 * 8 + 8];
    slots_to_buf(res, f->type->r, nr, out, w);
    return ojs_arraybuffer_copy(ctx, out, (size_t)nr * 8);
}

// ---- memories ----
NATIVE(n_mem_new) {            // (initial, max|-1) -> id
    WW(w);
    double init = 0, max = -1;
    if (argc > 0) ojs_to_number(ctx, &init, argv[0]);
    if (argc > 1) ojs_to_number(ctx, &max, argv[1]);
    if (init < 0 || init > 65536) return ojs_int(0);
    struct wasm_mem* m = wasm_mem_new(w->st, (uint32_t)init, max < 0 ? 0 : (uint32_t)max, max >= 0);
    if (!m) return ojs_int(0);
    if (host_mem_grow(w, m, (uint32_t)init) < 0) return ojs_int(0);
    return ojs_number(mem_id(w, m));
}
NATIVE(n_mem_buf) {
    WW(w);
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0 || !mem_of(w, (uint32_t)id)) return OJS_NULL;
    return w->membuf[id - 1];
}
NATIVE(n_mem_grow) {           // (id, delta) -> old pages | -1
    WW(w);
    int32_t id = 0; double d = 0;
    if (argc < 2 || ojs_to_int32(ctx, &id, argv[0]) < 0 || ojs_to_number(ctx, &d, argv[1]) < 0) return ojs_int(-1);
    struct wasm_mem* m = mem_of(w, (uint32_t)id);
    if (!m || d < 0 || d > 65536) return ojs_int(-1);
    return ojs_int(wasm_mem_grow(w->st, m, (uint32_t)d));
}
NATIVE(n_mem_info) {           // (id) -> [pages, max|-1]
    WW(w);
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_NULL;
    struct wasm_mem* m = mem_of(w, (uint32_t)id);
    if (!m) return OJS_NULL;
    ojsv r = ojs_array(ctx);
    ojs_set_index(ctx, r, 0, ojs_number(m->pages));
    ojs_set_index(ctx, r, 1, ojs_number(m->has_max ? (double)m->max_pages : -1));
    return r;
}

// ---- tables ----
NATIVE(n_tab_new) {            // (type char, initial, max|-1, initial value) -> id
    WW(w);
    char* tc = argc > 0 ? ojs_to_cstring(ctx, argv[0], 0) : 0;
    double init = 0, max = -1, v = 0;
    if (argc > 1) ojs_to_number(ctx, &init, argv[1]);
    if (argc > 2) ojs_to_number(ctx, &max, argv[2]);
    if (argc > 3) ojs_to_number(ctx, &v, argv[3]);
    int t = tc ? char_type(tc[0]) : 0;
    if (tc) ojs_free_cstring(ctx, tc);
    if (!t || init < 0 || init > 10000000) return ojs_int(0);
    struct wasm_tab* tb = wasm_tab_new(w->st, (uint8_t)t, (uint32_t)init, max < 0 ? 0 : (uint32_t)max, max >= 0);
    if (!tb) return ojs_int(0);
    void* iv = t == WT_FUNCREF ? (void*)fn_of(w, (uint32_t)v) : (void*)(uintptr_t)(uint32_t)v;
    for (uint32_t i = 0; i < tb->n; i++) tb->el[i] = iv;
    return ojs_number(tab_id(w, tb));
}
NATIVE(n_tab_len) { WW(w); int32_t id = 0; if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0) return ojs_int(-1); struct wasm_tab* t = tab_of(w, (uint32_t)id); return t ? ojs_number(t->n) : ojs_int(-1); }
NATIVE(n_tab_info) {           // (id) -> [type char, length, max|-1]
    WW(w);
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_NULL;
    struct wasm_tab* t = tab_of(w, (uint32_t)id);
    if (!t) return OJS_NULL;
    char c[2] = { type_char(t->type), 0 };
    ojsv r = ojs_array(ctx);
    ojs_set_index(ctx, r, 0, ojs_string(ctx, c));
    ojs_set_index(ctx, r, 1, ojs_number(t->n));
    ojs_set_index(ctx, r, 2, ojs_number(t->has_max ? (double)t->max : -1));
    return r;
}
NATIVE(n_tab_get) {            // (id, index) -> value (fn id or extern handle), -1 out of range
    WW(w);
    int32_t id = 0; double i = 0;
    if (argc < 2 || ojs_to_int32(ctx, &id, argv[0]) < 0 || ojs_to_number(ctx, &i, argv[1]) < 0) return ojs_int(-1);
    struct wasm_tab* t = tab_of(w, (uint32_t)id);
    if (!t || i < 0 || i >= t->n) return ojs_int(-1);
    void* e = t->el[(uint32_t)i];
    return ojs_number(t->type == WT_FUNCREF ? fn_id(w, (struct wasm_fn*)e) : (uint32_t)(uintptr_t)e);
}
NATIVE(n_tab_set) {            // (id, index, value) -> bool
    WW(w);
    int32_t id = 0; double i = 0, v = 0;
    if (argc < 3 || ojs_to_int32(ctx, &id, argv[0]) < 0 || ojs_to_number(ctx, &i, argv[1]) < 0 || ojs_to_number(ctx, &v, argv[2]) < 0) return ojs_bool(0);
    struct wasm_tab* t = tab_of(w, (uint32_t)id);
    if (!t || i < 0 || i >= t->n) return ojs_bool(0);
    t->el[(uint32_t)i] = t->type == WT_FUNCREF ? (void*)fn_of(w, (uint32_t)v) : (void*)(uintptr_t)(uint32_t)v;
    return ojs_bool(1);
}
NATIVE(n_tab_grow) {           // (id, delta, init) -> old length | -1
    WW(w);
    int32_t id = 0; double d = 0, v = 0;
    if (argc < 3 || ojs_to_int32(ctx, &id, argv[0]) < 0 || ojs_to_number(ctx, &d, argv[1]) < 0 || ojs_to_number(ctx, &v, argv[2]) < 0) return ojs_int(-1);
    struct wasm_tab* t = tab_of(w, (uint32_t)id);
    if (!t || d < 0 || d > 10000000) return ojs_int(-1);
    void* iv = t->type == WT_FUNCREF ? (void*)fn_of(w, (uint32_t)v) : (void*)(uintptr_t)(uint32_t)v;
    return ojs_int(wasm_tab_grow(w->st, t, (uint32_t)d, iv));
}

// ---- globals ----
NATIVE(n_glob_new) {           // (type char, mutable, slotBuffer) -> id
    WW(w);
    char* tc = argc > 0 ? ojs_to_cstring(ctx, argv[0], 0) : 0;
    int t = tc ? char_type(tc[0]) : 0;
    if (tc) ojs_free_cstring(ctx, tc);
    int mut = argc > 1 && ojs_to_bool(ctx, argv[1]);
    size_t len = 0;
    uint8_t* p = argc > 2 ? ojs_bytes(ctx, argv[2], &len) : 0;
    if (!t || !p || len < 8) return ojs_int(0);
    wval v;
    uint8_t ty = (uint8_t)t;
    buf_to_slots(p, &ty, 1, &v, w);
    struct wasm_glob* g = wasm_glob_new(w->st, ty, mut, v);
    return g ? ojs_number(glob_id(w, g)) : ojs_int(0);
}
NATIVE(n_glob_get) {           // (id) -> ArrayBuffer slot
    WW(w);
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_NULL;
    struct wasm_glob* g = glob_of(w, (uint32_t)id);
    if (!g) return OJS_NULL;
    uint8_t out[9];
    slots_to_buf(&g->v, &g->type, 1, out, w);
    return ojs_arraybuffer_copy(ctx, out, 8);
}
NATIVE(n_glob_set) {           // (id, slotBuffer)
    WW(w);
    int32_t id = 0;
    size_t len = 0;
    if (argc < 2 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_UNDEFINED;
    struct wasm_glob* g = glob_of(w, (uint32_t)id);
    uint8_t* p = ojs_bytes(ctx, argv[1], &len);
    if (!g || !p || len < 8) return OJS_UNDEFINED;
    buf_to_slots(p, &g->type, 1, &g->v, w);
    return OJS_UNDEFINED;
}
NATIVE(n_glob_info) {          // (id) -> [type char, mutable]
    WW(w);
    int32_t id = 0;
    if (argc < 1 || ojs_to_int32(ctx, &id, argv[0]) < 0) return OJS_NULL;
    struct wasm_glob* g = glob_of(w, (uint32_t)id);
    if (!g) return OJS_NULL;
    char c[2] = { type_char(g->type), 0 };
    ojsv r = ojs_array(ctx);
    ojs_set_index(ctx, r, 0, ojs_string(ctx, c));
    ojs_set_index(ctx, r, 1, ojs_int(g->mut));
    return r;
}

static const struct ojs_func_entry wasm_funcs[] = {
    { "wasmSetHost", n_set_host, 1 }, { "wasmModule", n_module, 1 }, { "wasmImports", n_imports, 1 }, { "wasmExports", n_exports, 1 },
    { "wasmCustom", n_custom, 2 }, { "wasmHostFn", n_host_fn, 2 }, { "wasmFnSig", n_fn_sig, 1 }, { "wasmInstantiate", n_instantiate, 2 },
    { "wasmCall", n_call, 2 }, { "wasmMemNew", n_mem_new, 2 }, { "wasmMemBuf", n_mem_buf, 1 }, { "wasmMemGrow", n_mem_grow, 2 },
    { "wasmMemInfo", n_mem_info, 1 }, { "wasmTabNew", n_tab_new, 4 }, { "wasmTabLen", n_tab_len, 1 }, { "wasmTabInfo", n_tab_info, 1 },
    { "wasmTabGet", n_tab_get, 2 }, { "wasmTabSet", n_tab_set, 3 }, { "wasmTabGrow", n_tab_grow, 3 }, { "wasmGlobNew", n_glob_new, 3 },
    { "wasmGlobGet", n_glob_get, 1 }, { "wasmGlobSet", n_glob_set, 2 }, { "wasmGlobInfo", n_glob_info, 1 },
};

void wjs_wasm_install(struct wjs* js, ojsv natives) {
    ojs_set_functions(js->J, natives, wasm_funcs, (int)(sizeof wasm_funcs / sizeof wasm_funcs[0]));
}
