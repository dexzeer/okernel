// gc_int.h — collector hooks shared between gc.c and the rest of the engine.
#ifndef OJS_GC_INT_H
#define OJS_GC_INT_H

#include "ojs_int.h"

struct gbytes { struct gch h; uint32_t n; uint32_t pad; uint8_t d[]; };
struct ptrarr { struct gch h; uint32_t n; uint32_t pad; void* p[]; };

void* bytes_new(ojs* J, size_t n);                     // gc-managed raw bytes (returns data)
struct ptrarr* ptrarr_new(ojs* J, uint32_t n);
size_t gc_size_of(ojs* J, const void* p);
int  gc_is_marked(const void* p);
size_t gc_live_bytes(ojs* J);
void gc_scan_conservative(ojs* J, const void* lo, const void* hi);   // mark whatever the words point into
size_t gc_live_of_type(ojs* J, int type);

static inline struct gbytes* bytes_of(void* d) { return (struct gbytes*)((char*)d - offsetof(struct gbytes, d)); }
static inline void gc_mark_bytes(ojs* J, void* d) { if (d) gc_mark_ptr(J, bytes_of(d)); }
static inline void gc_mark_valarr(ojs* J, jv* v) { if (v) gc_mark_ptr(J, valarr_of(v)); }
static inline uint32_t valarr_cap(jv* v) { return valarr_of(v)->cap; }

// tracing (by owner module)
void shape_trace(ojs* J, struct shape* s);
void obj_trace(ojs* J, struct obj* o);
void ftempl_trace(ojs* J, struct gch* g);
void upval_trace(ojs* J, struct gch* g);
void maptab_trace(ojs* J, struct gch* g);
void module_trace(ojs* J, struct gch* g);
void frame_trace(ojs* J, struct gch* g);
void gc_finalize(ojs* J, struct gch* g);

// roots owned by other modules
void atoms_mark_common(ojs* J);
void vm_mark_roots(ojs* J);
void module_mark_roots(ojs* J);

// weak structures
int  weak_process_ephemerons(ojs* J);   // marks values of live keys; 1 if anything was marked
void finreg_process(ojs* J);
void weak_clear_dead(ojs* J);

#endif
