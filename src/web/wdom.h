#ifndef WEB_WDOM_H
#define WEB_WDOM_H

#include <stdint.h>
#include "atoms_gen.h"

// Document object model for the web engine. One heap-allocated, growable
// store per document: nodes, attributes, a UTF-8 text arena, and a
// per-document atom table (tag/attribute names, class names and ids are
// interned to uint16 ids; known HTML names have fixed ids, see atoms_gen.h).
// Node links are int32 indices (-1 = none) — no fixed node cap.

#define WN_DOC     0
#define WN_ELEM    1
#define WN_TEXT    2
#define WN_COMMENT 3

#define NS_HTML 0
#define NS_SVG  1
#define NS_MATH 2

// node flags
#define WNF_WS        0x01   // text node is whitespace-only
#define WNF_REMOVED   0x02   // detached by script
#define WNF_FOCUSED   0x04   // form control has keyboard focus (UI state)
#define WNF_HOVER     0x08
#define WNF_FOCUS_WITHIN 0x10 // self or a descendant has focus

struct wnode {
    uint8_t  type;
    uint8_t  ns;
    uint16_t tag;        // atom (elements)
    uint16_t flags;
    uint16_t nclass;     // number of class atoms at cls
    int32_t  parent, first, last, next, prev;
    int32_t  attr;       // first attribute index (-1 none)
    uint32_t text;       // text/comment: arena offset; element: cls offset (into classes[])
    uint32_t tlen;       // text length (bytes)
    uint16_t id;         // id attribute atom (0 none)
    uint16_t _pad;
    int32_t  aux;        // engine slot (style index / box), owned by the renderer
};

struct wattr {
    uint16_t name;       // atom
    uint16_t _pad;
    int32_t  next;
    uint32_t val, vlen;  // text arena
};

struct watoms {
    char*     names;     // NUL-separated name store
    uint32_t  nlen, ncap;
    uint32_t* off;       // atom -> offset in names
    uint16_t* len;
    int       count, cap;
    int32_t*  hash;      // open addressing, -1 empty, size hcap (power of 2)
    int       hcap;
};

struct wdom {
    struct wnode* n;
    int nn, ncap;
    struct wattr* a;
    int na, acap;
    char* text;
    uint32_t tlen, tcap;
    uint16_t* classes;   // flattened class atom lists for elements
    uint32_t ncls, clscap;
    struct watoms atoms;
    int root;            // document node (always 0)
    int html, head, body;// convenience (or -1)
    int quirks;          // document is in quirks mode
    char title[128];     // UTF-8 <title> text (whitespace collapsed)
    char charset[24];    // detected encoding label
    int oom;             // an allocation failed; the tree may be incomplete
};

struct wdom* wdom_new(void);
void wdom_free(struct wdom* d);

// Atoms
int         watom_intern(struct watoms* t, const char* s, int len); // 0 on failure
int         watom_find(const struct watoms* t, const char* s, int len); // 0 if absent
const char* watom_name(const struct watoms* t, int atom, int* len);

// Construction
int  wdom_create_element(struct wdom* d, int ns, int tag_atom);
int  wdom_create_text(struct wdom* d, const char* s, int len);
int  wdom_create_comment(struct wdom* d, const char* s, int len);
void wdom_append(struct wdom* d, int parent, int child);
void wdom_insert_before(struct wdom* d, int parent, int child, int ref); // ref -1 = append
void wdom_remove(struct wdom* d, int child);   // detach from parent
// Append text to parent: merges with a trailing text node.
void wdom_append_text(struct wdom* d, int parent, const char* s, int len);
void wdom_insert_text_before(struct wdom* d, int parent, int ref, const char* s, int len);
// Attributes (name is an atom). set replaces an existing value.
void wdom_set_attr(struct wdom* d, int el, int name_atom, const char* v, int vlen);
int  wdom_has_attr(const struct wdom* d, int el, int name_atom);
// Value pointer (not NUL-terminated) + length, or NULL when absent.
const char* wdom_attr(const struct wdom* d, int el, int name_atom, int* len);
// Copy NUL-terminated (truncated to cap-1); returns length or -1 if absent.
int  wdom_attr_copy(const struct wdom* d, int el, int name_atom, char* out, int cap);
// Lookup by attribute name string (case-insensitive ASCII).
const char* wdom_attr_s(const struct wdom* d, int el, const char* name, int* len);
// Refresh the element's interned class list / id after class/id changes.
void wdom_index_attrs(struct wdom* d, int el);
int  wdom_has_class(const struct wdom* d, int el, int cls_atom);

// Queries
static inline int wdom_is(const struct wdom* d, int el, int tag) {
    return el >= 0 && d->n[el].type == WN_ELEM && d->n[el].tag == tag && d->n[el].ns == NS_HTML;
}
const char* wdom_tag_name(const struct wdom* d, int el, int* len);
int  wdom_find_id(const struct wdom* d, const char* id);
int  wdom_first_tag(const struct wdom* d, int tag);
// Next node in document (pre-order) after `node`, staying under `scope`.
int  wdom_next(const struct wdom* d, int node, int scope);
// Concatenated descendant text (UTF-8) into out; returns length.
int  wdom_text_content(const struct wdom* d, int node, char* out, int cap);
// Replace all children with a single text node.
void wdom_set_text_content(struct wdom* d, int el, const char* s, int len);

// Parse an HTML document (bytes in `charset` or sniffed when NULL/empty).
// Always returns a document (possibly empty) or NULL on allocation failure.
struct wdom* whtml_parse(const char* bytes, int len, const char* charset_hint);
// Parse a fragment into `parent` (innerHTML-style, body context).
void whtml_parse_fragment(struct wdom* d, int parent, const char* utf8, int len);

// Charset helpers: decode bytes to UTF-8 (newlines normalized to \n).
// Returns a heap buffer (caller frees) and its length in *out_len.
char* wcharset_decode(const char* in, int len, const char* label, int* out_len,
                      char* used_label, int used_cap);
// Sniff <meta charset> / http-equiv content-type from the first bytes.
int   wcharset_sniff(const char* in, int len, char* out, int cap);

// Debug: serialize the tree in html5lib "test" format into a heap buffer.
char* wdom_dump(const struct wdom* d, int* out_len);

#endif
