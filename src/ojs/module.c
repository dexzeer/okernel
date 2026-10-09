// module.c — ECMAScript modules (ECMA-262 §16.2): module records,
// export resolution, linking, evaluation (with top-level await), module
// namespace exotic objects, import.meta and dynamic import().
//
// Every module-scope binding is a cell (struct upval). Importing a binding
// shares the exporter's cell, which makes bindings live. The host supplies
// source text through ojs_module_hooks (resolve / load / dynamic_import).

#include "vm.h"
#include "gc_int.h"
#include "atoms.h"
#include "builtins.h"
#include "parse.h"

struct ftempl* compile_module(ojs* J, struct parser* P, struct ftempl** init_out, int* ncells);
jv promise_new_internal(ojs* J);
int promise_settle(ojs* J, jv promise, int rejected, jv v);
int promise_state(jv promise, jv* result);

enum { MS_NEW, MS_LINKING, MS_LINKED, MS_EVALUATING, MS_EVALUATED };

struct module_rec {
    struct gch h;
    struct str* name;
    struct ftempl* body;
    struct ftempl* init;
    struct upval** cells;       // ptrarr data
    uint32_t ncells;
    int status;
    int has_tla;
    int pad;
    jv requests;                // array of specifier strings
    jv imports;                 // internal array: (request, import name | undefined = namespace, cell) triples
    jv local_exports;           // (export name, cell) pairs
    jv indirect_exports;        // (export name, request, import name | undefined = namespace) triples
    jv star_exports;            // request indices
    jv resolved;                // array of module records (jv_from_ptr) per request
    jv ns;                      // namespace object
    jv meta;                    // import.meta
    jv error;                   // evaluation error (JV_HOLE: none)
    jv promise;                 // evaluation promise
};

static void mark_cells(ojs* J, struct upval** c) {
    if (c) gc_mark_ptr(J, (char*)c - offsetof(struct ptrarr, p));
}

void module_trace(ojs* J, struct gch* g) {
    struct module_rec* m = (struct module_rec*)g;
    gc_mark_ptr(J, m->name);
    gc_mark_ptr(J, m->body);
    gc_mark_ptr(J, m->init);
    mark_cells(J, m->cells);
    gc_mark_value(J, m->requests);
    gc_mark_value(J, m->imports);
    gc_mark_value(J, m->local_exports);
    gc_mark_value(J, m->indirect_exports);
    gc_mark_value(J, m->star_exports);
    gc_mark_value(J, m->resolved);
    gc_mark_value(J, m->ns);
    gc_mark_value(J, m->meta);
    gc_mark_value(J, m->error);
    gc_mark_value(J, m->promise);
}

void module_mark_roots(ojs* J) {
    for (int i = 0; i < J->nmodules; i++) gc_mark_ptr(J, J->modules[i]);
}

static struct module_rec* rec_of(jv v) { return (struct module_rec*)JV_PTR(v); }

static int register_module(ojs* J, struct module_rec* m) {
    if (J->nmodules >= J->capmodules) {
        int nc = J->capmodules ? J->capmodules * 2 : 16;
        struct module_rec** t = (struct module_rec**)ojs_sys_realloc(J->modules, (size_t)nc * sizeof *t);
        if (!t) { throw_oom(J); return -1; }
        J->modules = t;
        J->capmodules = nc;
    }
    J->modules[J->nmodules++] = m;
    return 0;
}

struct module_rec* module_find(ojs* J, const char* name) {
    for (int i = 0; i < J->nmodules; i++) {
        struct str* n = J->modules[i]->name;
        size_t l = strlen(name);
        if (str_len(n) == l && !str_wide(n) && !memcmp(n->u.c8, name, l)) return J->modules[i];
        if (str_wide(n) || str_len(n) != l) {
            char buf[1024];
            size_t k = str_to_utf8_buf(n, buf, sizeof buf);
            if (k < sizeof buf && k == l && !memcmp(buf, name, l)) return J->modules[i];
        }
    }
    return 0;
}

// ---------------------------------------------------------------- compile

static int arr_push(ojs* J, struct obj* a, jv v) {
    if (obj_elems_reserve(J, a, a->elen + 1) < 0) return -1;
    a->elems[a->elen++] = v;
    a->alen = a->elen;
    return 0;
}

static int request_index(ojs* J, struct obj* reqs, struct str* spec) {
    for (uint32_t i = 0; i < reqs->elen; i++) if (str_eq(jv_str(reqs->elems[i]), spec)) return (int)i;
    if (arr_push(J, reqs, jv_from_str(spec)) < 0) return -1;
    return (int)reqs->elen - 1;
}

static int decl_slot(struct scope* ms, struct str* name) {
    struct decl* d = scope_find(ms, name);
    return d ? d->slot : -1;
}

// the module's static import / export tables (ParseModule)
static int build_tables(ojs* J, struct module_rec* m, struct parser* P) {
    struct obj* reqs = obj_new_array(J, 0);
    struct obj* imps = obj_new_array(J, 0);
    struct obj* lex = obj_new_array(J, 0);
    struct obj* ind = obj_new_array(J, 0);
    struct obj* star = obj_new_array(J, 0);
    if (!reqs || !imps || !lex || !ind || !star) return -1;
    m->requests = jv_from_obj(reqs);
    m->imports = jv_from_obj(imps);
    m->local_exports = jv_from_obj(lex);
    m->indirect_exports = jv_from_obj(ind);
    m->star_exports = jv_from_obj(star);
    struct scope* ms = P->top->body_scope;
    // requests in source order
    for (struct node* s = P->program->a; s; s = s->next) {
        if (s->type == N_IMPORT_DECL || (s->type == N_EXPORT && s->str2 && (s->op == 1 || s->op == 3)))
            if (request_index(J, reqs, s->str2) < 0) return -1;
    }
    // import entries
    for (struct node* s = P->program->a; s; s = s->next) {
        if (s->type != N_IMPORT_DECL) continue;
        int rq = request_index(J, reqs, s->str2);
        for (struct node* sp = s->a; sp; sp = sp->next) {
            int slot = decl_slot(ms, sp->u.str);
            if (slot < 0) continue;
            jv iname = sp->str2 == J->A->ns_star ? JV_UNDEFINED : jv_from_str(sp->str2);
            if (arr_push(J, imps, jv_from_int(rq)) < 0 || arr_push(J, imps, iname) < 0 || arr_push(J, imps, jv_from_int(slot)) < 0) return -1;
        }
    }
    // export entries
    for (struct node* s = P->program->a; s; s = s->next) {
        if (s->type != N_EXPORT) continue;
        switch (s->op) {
        case 1:
            for (struct node* sp = s->a; sp; sp = sp->next) {
                if (s->str2) {
                    // export { a as b } from "m"
                    int rq = request_index(J, reqs, s->str2);
                    if (arr_push(J, ind, jv_from_str(sp->str2)) < 0 || arr_push(J, ind, jv_from_int(rq)) < 0 ||
                        arr_push(J, ind, jv_from_str(sp->u.str)) < 0) return -1;
                    continue;
                }
                struct decl* d = scope_find(ms, sp->u.str);
                if (d && d->kind == D_IMPORT && d->import_name != J->A->ns_star) {
                    // re-export of an imported binding: an indirect export
                    int rq = request_index(J, reqs, d->import_from);
                    if (arr_push(J, ind, jv_from_str(sp->str2)) < 0 || arr_push(J, ind, jv_from_int(rq)) < 0 ||
                        arr_push(J, ind, jv_from_str(d->import_name)) < 0) return -1;
                    continue;
                }
                int slot = d ? d->slot : -1;
                if (slot < 0) continue;
                if (arr_push(J, lex, jv_from_str(sp->str2)) < 0 || arr_push(J, lex, jv_from_int(slot)) < 0) return -1;
            }
            break;
        case 2: {
            // export default: a named declaration binds its own name
            struct str* local = J->A->star_default;
            if (s->a && s->a->type == N_FUNC && !(s->a->flags & NF_SHORTHAND) && s->a->u.fn->name) local = s->a->u.fn->name;
            if (s->a && s->a->type == N_CLASS && s->a->u.str) local = s->a->u.str;
            int slot = decl_slot(ms, local);
            if (slot >= 0 && (arr_push(J, lex, jv_from_str(J->A->default_)) < 0 || arr_push(J, lex, jv_from_int(slot)) < 0)) return -1;
            break;
        }
        case 3: {
            int rq = request_index(J, reqs, s->str2);
            if (s->u.str) {
                if (arr_push(J, ind, jv_from_str(s->u.str)) < 0 || arr_push(J, ind, jv_from_int(rq)) < 0 || arr_push(J, ind, JV_UNDEFINED) < 0) return -1;
            } else if (arr_push(J, star, jv_from_int(rq)) < 0) return -1;
            break;
        }
        case 4: {
            struct node* d = s->a;
            if (d->type == N_FUNC || d->type == N_CLASS) {
                struct str* nm = d->type == N_FUNC ? d->u.fn->name : d->u.str;
                int slot = decl_slot(ms, nm);
                if (slot >= 0 && (arr_push(J, lex, jv_from_str(nm)) < 0 || arr_push(J, lex, jv_from_int(slot)) < 0)) return -1;
            } else {
                // var / let / const: every bound name
                struct node* stack[64];
                int sp = 0;
                for (struct node* x = d->a; x; x = x->next) if (sp < 64) stack[sp++] = x->a;
                while (sp) {
                    struct node* n = stack[--sp];
                    if (!n) continue;
                    if (n->type == N_IDENT) {
                        int slot = decl_slot(ms, n->u.str);
                        if (slot >= 0 && (arr_push(J, lex, jv_from_str(n->u.str)) < 0 || arr_push(J, lex, jv_from_int(slot)) < 0)) return -1;
                    } else if (n->type == N_ASSIGN_PAT || n->type == N_REST) { if (sp < 64) stack[sp++] = n->a; }
                    else if (n->type == N_ARRAY_PAT) { for (struct node* e = n->a; e; e = e->next) if (sp < 64 && e->type != N_HOLE) stack[sp++] = e; }
                    else if (n->type == N_OBJECT_PAT) { for (struct node* q = n->a; q; q = q->next) if (sp < 64) stack[sp++] = q->b; }
                }
            }
            break;
        }
        }
    }
    // cells: TDZ for lexical bindings, undefined for vars and functions
    struct ptrarr* pa = ptrarr_new(J, m->ncells ? m->ncells : 1);
    if (!pa) return -1;
    m->cells = (struct upval**)pa->p;
    for (struct decl* d = ms->decls; d; d = d->next) {
        struct upval* u = (struct upval*)gc_alloc(J, GT_UPVAL, sizeof(struct upval));
        if (!u) return -1;
        u->loc = &u->closed;
        int tdz = d->kind == D_LET || d->kind == D_CONST || d->kind == D_CLASS || d->kind == D_IMPORT;
        u->closed = tdz ? JV_HOLE : JV_UNDEFINED;
        m->cells[d->slot] = u;
    }
    struct obj* res = obj_new_array(J, 0);
    if (!res) return -1;
    m->resolved = jv_from_obj(res);
    return 0;
}

static int code_has_await(struct ftempl* t) {
    for (uint32_t pc = 0; pc < t->code_len;) {
        int op = t->code[pc];
        if (op == OP_AWAIT) return 1;
        pc += (uint32_t)op_size(op);
    }
    return 0;
}

jv module_compile(ojs* J, struct str* src, const char* name) {
    struct parser P;
    struct parse_opts o;
    memset(&o, 0, sizeof o);
    o.kind = PARSE_MODULE;
    o.filename = name;
    if (parse_program(J, src, &o, &P) < 0) { parse_free(&P); return JV_EXC; }
    struct module_rec* m = (struct module_rec*)gc_alloc(J, GT_MODULE, sizeof(struct module_rec));
    if (!m) { parse_free(&P); return JV_EXC; }
    m->requests = m->imports = m->local_exports = m->indirect_exports = m->star_exports = JV_UNDEFINED;
    m->resolved = m->ns = m->meta = m->promise = JV_UNDEFINED;
    m->error = JV_HOLE;
    m->name = str_from_utf8(J, name, strlen(name));
    if (!m->name) { parse_free(&P); return JV_EXC; }
    int ncells;
    struct ftempl* init;
    struct ftempl* body = compile_module(J, &P, &init, &ncells);
    if (!body) { parse_free(&P); return JV_EXC; }
    m->body = body;
    m->init = init;
    m->ncells = (uint32_t)ncells;
    m->has_tla = code_has_await(body);
    srctext_compact(J, body, init, src);   // big UTF-16 sources live on as UTF-8
    int r = build_tables(J, m, &P);
    parse_free(&P);
    if (r < 0) return JV_EXC;
    // a module of the same name replaces the old registration
    for (int i = 0; i < J->nmodules; i++)
        if (str_eq(J->modules[i]->name, m->name)) { J->modules[i] = m; return jv_from_ptr(m); }
    if (register_module(J, m) < 0) return JV_EXC;
    return jv_from_ptr(m);
}

// ---------------------------------------------------------------- export resolution

#define RES_NONE ((struct module_rec*)0)
#define RES_AMBIG ((struct module_rec*)1)

struct resolution { struct module_rec* m; int cell; };   // cell -1: the module's namespace
struct rset { struct module_rec* m[256]; struct str* n[256]; int k; };

static struct module_rec* dep(struct module_rec* m, int rq) {
    struct obj* r = jv_obj(m->resolved);
    if ((uint32_t)rq >= r->elen) return 0;
    return rec_of(r->elems[rq]);
}

static int resolve_export(struct module_rec* m, struct str* name, struct rset* set, struct resolution* out) {
    for (int i = 0; i < set->k; i++) if (set->m[i] == m && str_eq(set->n[i], name)) { out->m = RES_NONE; return 0; }
    if (set->k < 256) { set->m[set->k] = m; set->n[set->k] = name; set->k++; }
    struct obj* le = jv_obj(m->local_exports);
    for (uint32_t i = 0; i < le->elen; i += 2)
        if (str_eq(jv_str(le->elems[i]), name)) { out->m = m; out->cell = jv_int(le->elems[i + 1]); return 0; }
    struct obj* ie = jv_obj(m->indirect_exports);
    for (uint32_t i = 0; i < ie->elen; i += 3) {
        if (!str_eq(jv_str(ie->elems[i]), name)) continue;
        struct module_rec* d = dep(m, jv_int(ie->elems[i + 1]));
        if (!d) { out->m = RES_NONE; return 0; }
        if (jv_is_undef(ie->elems[i + 2])) { out->m = d; out->cell = -1; return 0; }
        return resolve_export(d, jv_str(ie->elems[i + 2]), set, out);
    }
    if (str_eq_ascii(name, "default")) { out->m = RES_NONE; return 0; }
    struct resolution star = { RES_NONE, 0 };
    struct obj* se = jv_obj(m->star_exports);
    for (uint32_t i = 0; i < se->elen; i++) {
        struct module_rec* d = dep(m, jv_int(se->elems[i]));
        if (!d) continue;
        struct resolution r;
        resolve_export(d, name, set, &r);
        if (r.m == RES_AMBIG) { *out = r; return 0; }
        if (r.m == RES_NONE) continue;
        if (star.m == RES_NONE) star = r;
        else if (star.m != r.m || star.cell != r.cell) { out->m = RES_AMBIG; return 0; }
    }
    *out = star;
    return 0;
}

// GetExportedNames into the array `names` (deduplicated)
static int exported_names(ojs* J, struct module_rec* m, struct obj* names, struct module_rec** visited, int* nv) {
    for (int i = 0; i < *nv; i++) if (visited[i] == m) return 0;
    if (*nv < 256) visited[(*nv)++] = m;
    struct obj* le = jv_obj(m->local_exports);
    struct obj* ie = jv_obj(m->indirect_exports);
    for (uint32_t i = 0; i < le->elen; i += 2) if (arr_push(J, names, le->elems[i]) < 0) return -1;
    for (uint32_t i = 0; i < ie->elen; i += 3) if (arr_push(J, names, ie->elems[i]) < 0) return -1;
    struct obj* se = jv_obj(m->star_exports);
    for (uint32_t i = 0; i < se->elen; i++) {
        struct module_rec* d = dep(m, jv_int(se->elems[i]));
        if (!d) continue;
        struct obj* sub = obj_new_array(J, 0);
        if (!sub) return -1;
        if (exported_names(J, d, sub, visited, nv) < 0) return -1;
        for (uint32_t k = 0; k < sub->elen; k++) {
            struct str* n = jv_str(sub->elems[k]);
            if (str_eq_ascii(n, "default")) continue;
            int dup = 0;
            for (uint32_t q = 0; q < names->elen; q++) if (str_eq(jv_str(names->elems[q]), n)) { dup = 1; break; }
            if (!dup && arr_push(J, names, sub->elems[k]) < 0) return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- namespace objects

struct modns {
    struct obj base;
    struct module_rec* m;
    jv names;           // sorted export names
    jv cells;           // per name: jv_from_ptr(upval) or the namespace object of a sub module
};

static void modns_trace(ojs* J, struct obj* o) {
    struct modns* n = (struct modns*)o;
    gc_mark_ptr(J, n->m);
    gc_mark_value(J, n->names);
    gc_mark_value(J, n->cells);
}

static jv get_namespace(ojs* J, struct module_rec* m);

static int ns_index(struct modns* n, pkey k) {
    if (!pk_is_str(k)) {
        if (PK_IS_INDEX(k)) {
            // an index key matches an export spelled as digits
            struct obj* a = jv_obj(n->names);
            char buf[16];
            int l = ojs_snprintf(buf, sizeof buf, "%u", PK_INDEX(k));
            for (uint32_t i = 0; i < a->elen; i++) {
                struct str* s = jv_str(a->elems[i]);
                if (str_len(s) == (uint32_t)l && !str_wide(s) && !memcmp(s->u.c8, buf, (size_t)l)) return (int)i;
            }
        }
        return -1;
    }
    struct str* s = pk_str(k);
    struct obj* a = jv_obj(n->names);
    int lo = 0, hi = (int)a->elen - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = str_cmp(jv_str(a->elems[mid]), s);
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static jv ns_value(ojs* J, struct modns* n, int i) {
    jv c = jv_obj(n->cells)->elems[i];
    if (((struct gch*)JV_PTR(c))->type == GT_MODULE) return get_namespace(J, rec_of(c));   // export * as ns
    struct upval* u = (struct upval*)JV_PTR(c);
    jv v = *u->loc;
    if (v == JV_HOLE) return throw_ref(J, "Cannot access '%S' before initialization", jv_str(jv_obj(n->names)->elems[i]));
    return v;
}

static int ns_get_own(ojs* J, struct obj* o, pkey k, struct pdesc* d) {
    if (pk_is_sym(k)) return ord_get_own(J, o, k, d);
    struct modns* n = (struct modns*)o;
    int i = ns_index(n, k);
    if (i < 0) return 0;
    jv v = ns_value(J, n, i);
    if (v == JV_EXC) return -1;
    if (d) {
        d->has = PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE;
        d->attrs = PA_WRITABLE | PA_ENUMERABLE;
        d->value = v;
        d->get = d->set = JV_UNDEFINED;
    }
    return 1;
}

static int ns_define_own(ojs* J, struct obj* o, pkey k, const struct pdesc* d) {
    if (pk_is_sym(k)) return ord_define_own(J, o, k, d);
    struct pdesc cur;
    int h = ns_get_own(J, o, k, &cur);
    if (h <= 0) return h;
    if ((d->has & PD_CONFIGURABLE) && (d->attrs & PA_CONFIGURABLE)) return 0;
    if ((d->has & PD_ENUMERABLE) && !(d->attrs & PA_ENUMERABLE)) return 0;
    if (PD_IS_ACCESSOR(d)) return 0;
    if ((d->has & PD_WRITABLE) && !(d->attrs & PA_WRITABLE)) return 0;
    if (d->has & PD_VALUE) return same_value(J, d->value, cur.value);
    return 1;
}

static int ns_has(ojs* J, struct obj* o, pkey k) {
    if (pk_is_sym(k)) return ord_get_own(J, o, k, 0);
    return ns_index((struct modns*)o, k) >= 0;
}

static jv ns_get(ojs* J, struct obj* o, pkey k, jv receiver) {
    if (pk_is_sym(k)) {
        struct pdesc d;
        int r = ord_get_own(J, o, k, &d);
        return r > 0 ? d.value : JV_UNDEFINED;
    }
    struct modns* n = (struct modns*)o;
    int i = ns_index(n, k);
    if (i < 0) return JV_UNDEFINED;
    return ns_value(J, n, i);
}

static int ns_set(ojs* J, struct obj* o, pkey k, jv v, jv receiver) { return 0; }

static int ns_del(ojs* J, struct obj* o, pkey k) {
    if (pk_is_sym(k)) return ord_del(J, o, k);
    return ns_index((struct modns*)o, k) < 0;
}

static jv ns_own_keys(ojs* J, struct obj* o) {
    struct modns* n = (struct modns*)o;
    struct obj* names = jv_obj(n->names);
    jv sym = ord_own_keys(J, o);
    if (sym == JV_EXC) return JV_EXC;
    struct obj* s = jv_obj(sym);
    struct obj* a = obj_new_array(J, 0);
    if (!a || obj_elems_reserve(J, a, names->elen + s->elen + 1) < 0) return JV_EXC;
    memcpy(a->elems, names->elems, (size_t)names->elen * sizeof(jv));
    memcpy(a->elems + names->elen, s->elems, (size_t)s->elen * sizeof(jv));
    a->elen = a->alen = names->elen + s->elen;
    return jv_from_obj(a);
}

static struct obj* ns_get_proto(ojs* J, struct obj* o, int* err) { return 0; }
static int ns_set_proto(ojs* J, struct obj* o, struct obj* p) { return p == 0; }
static int ns_is_extensible(ojs* J, struct obj* o) { return 0; }
static int ns_prevent_ext(ojs* J, struct obj* o) { return 1; }

static jv get_namespace(ojs* J, struct module_rec* m) {
    if (!jv_is_undef(m->ns)) return m->ns;
    struct obj* names = obj_new_array(J, 0);
    if (!names) return JV_EXC;
    struct module_rec* visited[256];
    int nv = 0;
    if (exported_names(J, m, names, visited, &nv) < 0) return JV_EXC;
    // keep the names that resolve unambiguously, sorted by code units
    struct obj* keep = obj_new_array(J, 0);
    struct obj* cells = obj_new_array(J, 0);
    if (!keep || !cells) return JV_EXC;
    for (uint32_t i = 0; i < names->elen; i++) {
        struct str* nm = jv_str(names->elems[i]);
        int dup = 0;
        for (uint32_t q = 0; q < keep->elen; q++) if (str_eq(jv_str(keep->elems[q]), nm)) { dup = 1; break; }
        if (dup) continue;
        struct rset set;
        set.k = 0;
        struct resolution r;
        resolve_export(m, nm, &set, &r);
        if (r.m == RES_NONE || r.m == RES_AMBIG) continue;
        if (arr_push(J, keep, jv_from_str(nm)) < 0) return JV_EXC;
        jv c = r.cell < 0 ? jv_from_ptr(r.m) : jv_from_ptr(r.m->cells[r.cell]);
        if (arr_push(J, cells, c) < 0) return JV_EXC;
    }
    // insertion sort (names are few)
    for (uint32_t i = 1; i < keep->elen; i++) {
        jv a = keep->elems[i], b = cells->elems[i];
        uint32_t j = i;
        while (j > 0 && str_cmp(jv_str(keep->elems[j - 1]), jv_str(a)) > 0) {
            keep->elems[j] = keep->elems[j - 1];
            cells->elems[j] = cells->elems[j - 1];
            j--;
        }
        keep->elems[j] = a;
        cells->elems[j] = b;
    }
    struct modns* n = (struct modns*)obj_new(J, 0, OC_MODULE_NS, sizeof(struct modns));
    if (!n) return JV_EXC;
    n->m = m;
    n->names = jv_from_obj(keep);
    n->cells = jv_from_obj(cells);
    n->base.flags &= ~OF_EXTENSIBLE;
    if (ord_define_own(J, &n->base, pk_from_sym(J->wk[WK_TO_STRING_TAG]),
                       &(struct pdesc){ PD_VALUE | PD_WRITABLE | PD_ENUMERABLE | PD_CONFIGURABLE, 0, str_value(J, "Module"), JV_UNDEFINED, JV_UNDEFINED }) < 0)
        return JV_EXC;
    m->ns = jv_from_obj(&n->base);
    return m->ns;
}

// ---------------------------------------------------------------- loading and linking

// resolve `spec` from module `referrer` (name) and return the loaded record
static struct module_rec* load_dep(ojs* J, struct str* referrer, struct str* spec) {
    struct ojs_module_hooks* H = J->modhooks;
    char* ref = str_to_utf8(J, referrer, 0);
    char* sp = str_to_utf8(J, spec, 0);
    if (!ref || !sp) { ojs_sys_free(ref); ojs_sys_free(sp); throw_oom(J); return 0; }
    char* name = H && H->resolve ? H->resolve(J, ref, sp, H->op) : 0;
    if (!name && !J->has_exc) {
        // no resolver: the specifier is the name
        size_t l = strlen(sp);
        name = (char*)ojs_sys_malloc(l + 1);
        if (name) memcpy(name, sp, l + 1);
    }
    ojs_sys_free(ref);
    ojs_sys_free(sp);
    if (!name) { if (!J->has_exc) throw_oom(J); return 0; }
    struct module_rec* m = module_find(J, name);
    if (!m) {
        if (!H || !H->load) { throw_type(J, "Cannot find module '%s'", name); ojs_sys_free(name); return 0; }
        jv v = H->load(J, name, H->op);
        if (v == JV_EXC) { ojs_sys_free(name); return 0; }
        m = module_find(J, name);
        if (!m && JV_TAG(v) == TAG_PTR) m = rec_of(v);
        if (!m) { throw_type(J, "Cannot find module '%s'", name); ojs_sys_free(name); return 0; }
    }
    ojs_sys_free(name);
    return m;
}

static jv run_init(ojs* J, struct module_rec* m) {
    struct func* fn = closure_new(J, m->init, 0, 0);
    if (!fn) return JV_EXC;
    fn->upv = m->cells;
    fn->module = jv_from_ptr(m);
    return vm_run_func(J, fn, JV_UNDEFINED, JV_UNDEFINED);
}

static int link_module(ojs* J, struct module_rec* m) {
    if (m->status != MS_NEW) return 0;
    m->status = MS_LINKING;
    struct obj* reqs = jv_obj(m->requests);
    struct obj* res = jv_obj(m->resolved);
    for (uint32_t i = 0; i < reqs->elen; i++) {
        struct module_rec* d = load_dep(J, m->name, jv_str(reqs->elems[i]));
        if (!d) { m->status = MS_NEW; return -1; }
        if (res->elen <= i && arr_push(J, res, jv_from_ptr(d)) < 0) return -1;
        res->elems[i] = jv_from_ptr(d);
    }
    for (uint32_t i = 0; i < reqs->elen; i++)
        if (link_module(J, dep(m, (int)i)) < 0) { m->status = MS_NEW; return -1; }
    // InitializeEnvironment
    struct obj* ie = jv_obj(m->indirect_exports);
    for (uint32_t i = 0; i < ie->elen; i += 3) {
        struct rset set;
        set.k = 0;
        struct resolution r;
        resolve_export(m, jv_str(ie->elems[i]), &set, &r);
        if (r.m == RES_NONE || r.m == RES_AMBIG) {
            m->status = MS_NEW;
            throw_syntax(J, "The requested module does not provide an export named '%S'", jv_str(ie->elems[i]));
            return -1;
        }
    }
    struct obj* im = jv_obj(m->imports);
    for (uint32_t i = 0; i < im->elen; i += 3) {
        struct module_rec* d = dep(m, jv_int(im->elems[i]));
        jv iname = im->elems[i + 1];
        int cell = jv_int(im->elems[i + 2]);
        if (jv_is_undef(iname)) {
            jv ns = get_namespace(J, d);
            if (ns == JV_EXC) { m->status = MS_NEW; return -1; }
            m->cells[cell]->closed = ns;
            continue;
        }
        struct rset set;
        set.k = 0;
        struct resolution r;
        resolve_export(d, jv_str(iname), &set, &r);
        if (r.m == RES_NONE || r.m == RES_AMBIG) {
            m->status = MS_NEW;
            throw_syntax(J, "The requested module '%S' %s an export named '%S'", d->name,
                         r.m == RES_AMBIG ? "contains conflicting star exports for" : "does not provide", jv_str(iname));
            return -1;
        }
        if (r.cell < 0) {
            jv ns = get_namespace(J, r.m);
            if (ns == JV_EXC) { m->status = MS_NEW; return -1; }
            m->cells[cell]->closed = ns;
        } else m->cells[cell] = r.m->cells[r.cell];
    }
    if (run_init(J, m) == JV_EXC) { m->status = MS_NEW; return -1; }
    m->status = MS_LINKED;
    return 0;
}

// ---------------------------------------------------------------- evaluation

// post-order list of the modules to evaluate
static int collect(ojs* J, struct module_rec* m, struct obj* list) {
    if (m->status != MS_LINKED) return 0;
    m->status = MS_EVALUATING;
    struct obj* reqs = jv_obj(m->requests);
    for (uint32_t i = 0; i < reqs->elen; i++) if (collect(J, dep(m, (int)i), list) < 0) return -1;
    return arr_push(J, list, jv_from_ptr(m));
}

static jv run_body_sync(ojs* J, struct module_rec* m) {
    struct func* fn = closure_new(J, m->body, 0, 0);
    if (!fn) return JV_EXC;
    fn->upv = m->cells;
    fn->module = jv_from_ptr(m);
    return vm_run_func(J, fn, JV_UNDEFINED, JV_UNDEFINED);
}

static jv run_body_async(ojs* J, struct module_rec* m) {
    struct func* fn = closure_new(J, m->body, 0, 0);
    if (!fn) return JV_EXC;
    fn->upv = m->cells;
    fn->module = jv_from_ptr(m);
    return async_start(J, fn, JV_UNDEFINED, 0, 0, JV_UNDEFINED);
}

// evaluation continues through this native after an async module settles
// (data = state record [list, index, promise])
static void eval_step(ojs* J, struct irec* st);

static jv eval_cont(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    struct irec* st = (struct irec*)jv_obj(f->data);
    struct obj* list = jv_obj(st->v[0]);
    int idx = jv_int(st->v[1]);
    struct module_rec* m = rec_of(list->elems[idx]);
    if (magic) {
        // rejected: this module and everything after it fail
        for (uint32_t i = (uint32_t)idx; i < list->elen; i++) {
            struct module_rec* x = rec_of(list->elems[i]);
            x->status = MS_EVALUATED;
            x->error = argv[0];
        }
        promise_settle(J, st->v[2], 1, argv[0]);
        return JV_UNDEFINED;
    }
    m->status = MS_EVALUATED;
    st->v[1] = jv_from_int(idx + 1);
    eval_step(J, st);
    return JV_UNDEFINED;
}

static void eval_step(ojs* J, struct irec* st) {
    struct obj* list = jv_obj(st->v[0]);
    for (;;) {
        int idx = jv_int(st->v[1]);
        if ((uint32_t)idx >= list->elen) { promise_settle(J, st->v[2], 0, JV_UNDEFINED); return; }
        struct module_rec* m = rec_of(list->elems[idx]);
        // a dependency that failed earlier fails this module too
        struct obj* reqs = jv_obj(m->requests);
        for (uint32_t i = 0; i < reqs->elen; i++) {
            struct module_rec* d = dep(m, (int)i);
            if (d->error != JV_HOLE) {
                m->status = MS_EVALUATED;
                m->error = d->error;
                break;
            }
        }
        if (m->error != JV_HOLE) {
            for (uint32_t i = (uint32_t)idx; i < list->elen; i++) {
                struct module_rec* x = rec_of(list->elems[i]);
                x->status = MS_EVALUATED;
                if (x->error == JV_HOLE) x->error = m->error;
            }
            promise_settle(J, st->v[2], 1, m->error);
            return;
        }
        if (m->has_tla) {
            jv p = run_body_async(J, m);
            if (p == JV_EXC) {
                if (J->uncatchable) return;
                m->error = take_exc(J);
                continue;
            }
            struct obj* a = new_native(J, eval_cont, "", 1, 0);
            struct obj* b = new_native(J, eval_cont, "", 1, 1);
            if (!a || !b) { take_exc(J); return; }
            ((struct nfunc*)a)->data = ((struct nfunc*)b)->data = jv_from_obj(&st->base);
            perform_promise_then(J, p, jv_from_obj(a), jv_from_obj(b), 0);
            return;
        }
        jv r = run_body_sync(J, m);
        if (r == JV_EXC) {
            if (J->uncatchable) return;
            m->error = take_exc(J);
            continue;
        }
        m->status = MS_EVALUATED;
        st->v[1] = jv_from_int(idx + 1);
    }
}

// Link + Evaluate: the evaluation promise
jv module_evaluate(ojs* J, jv mv) {
    struct module_rec* m = rec_of(mv);
    if (!jv_is_undef(m->promise)) return m->promise;
    if (link_module(J, m) < 0) return JV_EXC;
    jv p = promise_new_internal(J);
    if (p == JV_EXC) return JV_EXC;
    m->promise = p;
    struct obj* list = obj_new_array(J, 0);
    if (!list) return JV_EXC;
    if (collect(J, m, list) < 0) return JV_EXC;
    if (!list->elen) {
        // already evaluated (or evaluating in a cycle)
        if (m->error != JV_HOLE) promise_settle(J, p, 1, m->error);
        else promise_settle(J, p, 0, JV_UNDEFINED);
        return p;
    }
    struct irec* st = irec_new(J, 3);
    if (!st) return JV_EXC;
    st->v[0] = jv_from_obj(list);
    st->v[1] = jv_from_int(0);
    st->v[2] = p;
    eval_step(J, st);
    if (J->uncatchable && J->has_exc) return JV_EXC;
    return p;
}

// ---------------------------------------------------------------- import.meta / import()

static struct module_rec* current_module(struct ojs_frame* f) {
    for (; f; f = f->prev)
        if (f->fn && JV_TAG(f->fn->module) == TAG_PTR) return rec_of(f->fn->module);
    return 0;
}

jv module_meta_object(ojs* J, struct module_rec* m) {
    if (jv_is_undef(m->meta)) {
        struct obj* o = obj_new(J, 0, OC_OBJECT, 0);
        if (!o) return JV_EXC;
        m->meta = jv_from_obj(o);
    }
    return m->meta;
}

jv module_import_meta(ojs* J, struct ojs_frame* f) {
    struct module_rec* m = current_module(f);
    if (!m) return throw_syntax(J, "Cannot use 'import.meta' outside a module");
    return module_meta_object(J, m);
}

// after evaluation: resolve the import() promise with the namespace
static jv import_ns_cb(ojs* J, jv this_v, int argc, jv* argv, int magic) {
    struct nfunc* f = (struct nfunc*)J->native_callee;
    struct irec* st = (struct irec*)jv_obj(f->data);   // [module ptr, promise]
    if (magic) { promise_settle(J, st->v[1], 1, argv[0]); return JV_UNDEFINED; }
    jv ns = get_namespace(J, rec_of(st->v[0]));
    if (ns == JV_EXC) { promise_settle(J, st->v[1], 1, take_exc(J)); return JV_UNDEFINED; }
    promise_settle(J, st->v[1], 0, ns);
    return JV_UNDEFINED;
}

// load + link + evaluate `spec` for `referrer`, settling `promise` with the namespace
static void finish_import(ojs* J, struct str* referrer, struct str* spec, jv promise) {
    struct module_rec* m = load_dep(J, referrer, spec);
    if (!m) { if (!J->uncatchable) promise_settle(J, promise, 1, take_exc(J)); return; }
    jv ep = module_evaluate(J, jv_from_ptr(m));
    if (ep == JV_EXC) { if (!J->uncatchable) promise_settle(J, promise, 1, take_exc(J)); return; }
    struct irec* st = irec_new(J, 2);
    struct obj* a = new_native(J, import_ns_cb, "", 1, 0);
    struct obj* b = new_native(J, import_ns_cb, "", 1, 1);
    if (!st || !a || !b) { take_exc(J); return; }
    st->v[0] = jv_from_ptr(m);
    st->v[1] = promise;
    ((struct nfunc*)a)->data = ((struct nfunc*)b)->data = jv_from_obj(&st->base);
    perform_promise_then(J, ep, jv_from_obj(a), jv_from_obj(b), 0);
}

jv module_dynamic_import(ojs* J, struct ojs_frame* f, jv spec, jv options) {
    jv p = promise_new_internal(J);
    if (p == JV_EXC) return JV_EXC;
    jv sv = to_string(J, spec);
    if (sv == JV_EXC) {
        if (J->uncatchable) return JV_EXC;
        promise_settle(J, p, 1, take_exc(J));
        return p;
    }
    if (!jv_is_undef(options)) {
        if (!jv_is_obj(options)) {
            throw_type(J, "The second argument of import() must be an object");
            promise_settle(J, p, 1, take_exc(J));
            return p;
        }
        jv w = obj_get(J, jv_obj(options), pkey_from_cstr(J, "with"), options);
        if (w == JV_EXC) { promise_settle(J, p, 1, take_exc(J)); return p; }
        if (!jv_is_undef(w) && !jv_is_obj(w)) {
            throw_type(J, "The 'with' option of import() must be an object");
            promise_settle(J, p, 1, take_exc(J));
            return p;
        }
        if (jv_is_obj(w)) {
            jv keys = obj_own_keys(J, jv_obj(w), OWNKEYS_STRINGS | OWNKEYS_ENUM_ONLY);
            if (keys == JV_EXC) { promise_settle(J, p, 1, take_exc(J)); return p; }
            struct obj* ka = jv_obj(keys);
            for (uint32_t i = 0; i < ka->elen; i++) {
                pkey k = pkey_from_value(J, ka->elems[i]);
                jv v = k ? obj_get(J, jv_obj(w), k, w) : JV_EXC;
                if (v == JV_EXC) { promise_settle(J, p, 1, take_exc(J)); return p; }
                if (!jv_is_str(v)) {
                    throw_type(J, "Import attribute values must be strings");
                    promise_settle(J, p, 1, take_exc(J));
                    return p;
                }
            }
        }
    }
    struct module_rec* cur = current_module(f);
    struct str* referrer = 0;
    if (cur) referrer = cur->name;
    else {
        for (struct ojs_frame* x = f; x && !referrer; x = x->prev) if (x->t && x->t->filename) referrer = x->t->filename;
        if (!referrer) referrer = J->A->empty;
    }
    struct ojs_module_hooks* H = J->modhooks;
    if (H && H->dynamic_import) {
        char* ref = str_to_utf8(J, referrer, 0);
        char* sp = str_to_utf8(J, jv_str(sv), 0);
        struct obj* rs = new_native(J, import_ns_cb, "", 1, 0);
        if (!ref || !sp) { ojs_sys_free(ref); ojs_sys_free(sp); return throw_oom(J); }
        (void)rs;
        // the host gets plain resolve / reject functions of the promise
        struct promise_cap cap;
        cap.promise = p;
        jv res, rej;
        jv make_resolvers(ojs* J, jv promise, jv* res, jv* rej);
        if (make_resolvers(J, p, &res, &rej) == JV_EXC) { ojs_sys_free(ref); ojs_sys_free(sp); return JV_EXC; }
        int taken = H->dynamic_import(J, ref, sp, res, rej, H->op);
        ojs_sys_free(ref);
        ojs_sys_free(sp);
        (void)cap;
        if (taken) return p;
    }
    finish_import(J, referrer, jv_str(sv), p);
    if (J->uncatchable && J->has_exc) return JV_EXC;
    return p;
}

// the host finishes a dynamic import it took over: load + evaluate, then
// call resolve(namespace) / reject(error)
void module_finish_dynamic_import(ojs* J, const char* referrer, const char* spec, jv resolve, jv reject) {
    jv p = promise_new_internal(J);
    if (p == JV_EXC) { take_exc(J); return; }
    struct str* r = str_from_utf8(J, referrer, strlen(referrer));
    struct str* s = str_from_utf8(J, spec, strlen(spec));
    if (!r || !s) { take_exc(J); return; }
    finish_import(J, r, s, p);
    jv args[2] = { resolve, reject };
    if (perform_promise_then(J, p, args[0], args[1], 0) < 0) take_exc(J);
}

// ---------------------------------------------------------------- embedding helpers

int module_request_count(jv mv) { return (int)jv_obj(rec_of(mv)->requests)->elen; }
jv module_request(jv mv, int i) { return jv_obj(rec_of(mv)->requests)->elems[i]; }
struct module_rec* module_of(jv mv) { return rec_of(mv); }
jv module_namespace(ojs* J, jv mv) { return get_namespace(J, rec_of(mv)); }

static const struct class_ops modns_ops = {
    .get_own = ns_get_own, .define_own = ns_define_own, .has = ns_has, .get = ns_get, .set = ns_set, .del = ns_del,
    .own_keys = ns_own_keys, .get_proto = ns_get_proto, .set_proto = ns_set_proto, .is_extensible = ns_is_extensible,
    .prevent_ext = ns_prevent_ext, .trace = modns_trace,
};

void module_classes(void) { class_ops[OC_MODULE_NS] = modns_ops; }
