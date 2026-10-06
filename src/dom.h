#ifndef DOM_H
#define DOM_H

#include <stdint.h>

// Bounded document object model for one browser tab. The tree is the single
// source of truth for rendering, layout, CSS matching and the JS DOM bridge;
// the old flat html_token stream is derived from it for legacy consumers.
//
// Sizes are chosen to fit real pages while staying well inside the kernel's
// identity-mapped heap: ~72KB nodes + ~18KB attrs + 96KB decoded text +
// 16KB interned names ≈ 200KB per tab.
// Sized for full real-world articles (Wikipedia is ~680KB of HTML / ~150KB of
// decoded text, thousands of elements). Per tab ~256KB text + ~192KB nodes +
// ~48KB attrs + 16KB names ≈ 512KB; MAX_OKAIS*OKAI_MAX_TABS caps the total.
#define DOM_MAX_NODES   16000
#define DOM_MAX_ATTRS   24000
#define DOM_MAX_TEXT    524288
#define DOM_MAX_NAMES   131072

#define DOM_NODE_ROOT    0
#define DOM_NODE_ELEMENT 1
#define DOM_NODE_TEXT    2

#define DOM_NONE 0xFFFF

struct dom_attr {
    uint32_t name_off;   // names arena
    uint8_t  name_len;
    uint16_t _pad;
    uint32_t val_off;    // text arena (charset + entities already decoded)
    uint16_t val_len;
    uint16_t next;       // next attribute index, DOM_NONE = end
};

struct dom_node {
    uint8_t  type;
    uint32_t tag_off;    // names arena (element tag, lowercased)
    uint8_t  tag_len;
    uint16_t attr_head;  // first attr index, DOM_NONE = none
    uint16_t parent;
    uint16_t first_child;
    uint16_t last_child;
    uint16_t next_sib;
    uint32_t text_off;   // text arena
    uint16_t text_len;
    uint16_t flags;
};

#define DOM_F_WS 1  // text node contains only whitespace

struct dom {
    struct dom_node nodes[DOM_MAX_NODES];
    struct dom_attr attrs[DOM_MAX_ATTRS];
    char text[DOM_MAX_TEXT];
    char names[DOM_MAX_NAMES];
    int node_count, attr_count, text_len, name_len;
    int root;
    int truncated;     // a cap was hit; the document is incomplete
    int trunc_why;     // debug: 1=names 2=nodes 3=text 4=attrs 5=open-stack
};

void dom_reset(struct dom* d);
// Tokenize + build the tree. Returns the root node index (>=0) or -1 if the
// document was empty/unbuildable. Never writes past the arenas; `truncated`
// is set when a cap stops the build.
int  dom_build(struct dom* d, const char* html, int len);

// Attribute lookup by lowercase name. Copies the value NUL-terminated into
// `out` and returns its length, or -1 when absent.
int  dom_attr_get(const struct dom* d, int node, const char* name, char* out, int cap);
// Set (or add) attribute name=value on node (`name` must be lowercase).
// Reuses the old slot when the value fits, else appends to the text arena
// (and to the names arena for a brand-new attribute). Returns 0 on success,
// -1 when arenas/tables are full. Used for form-field editing: the layout
// re-reads attributes on re-render, so edits display and submit directly.
int  dom_attr_set(struct dom* d, int node, const char* name, const char* val);
// Tag compare against a lowercase name. 0 when node is not that element.
int  dom_tag_is(const struct dom* d, int node, const char* tag);
// Copy the element tag NUL-terminated (empty for non-elements).
void dom_tag_copy(const struct dom* d, int node, char* out, int cap);
// Text node contents (pointer into the arena, not NUL-terminated) + length.
const char* dom_text(const struct dom* d, int node, int* len);
// NUL-terminated copy of a text node's contents.
void dom_text_copy(const struct dom* d, int node, char* out, int cap);
// First element matching tag (document order), or -1.
int  dom_first_tag(const struct dom* d, const char* tag);
// First element with the given id, or -1.
int  dom_find_id(const struct dom* d, const char* id);
// First element with `tag` carrying class `cls`, or -1.
int  dom_first_tag_class(const struct dom* d, const char* tag, const char* cls);

// Render-relevant classification (both case-insensitive on the passed name).
int  dom_is_void(const char* tag, int len);
int  dom_is_block(const char* tag, int len);
int  dom_is_rawtext(const char* tag, int len);  // contents are not markup

#endif