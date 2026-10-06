#ifndef WEB_CSS_INT_H
#define WEB_CSS_INT_H

// Internal definitions shared by css_*.c (not part of the engine API).

#include "css.h"
#include "wcommon.h"

// ---- longhand property ids -------------------------------------------------
enum {
    P_NONE = 0,
    P_DISPLAY, P_POSITION, P_FLOAT, P_CLEAR, P_BOX_SIZING, P_VISIBILITY, P_OVERFLOW_X,
    P_OVERFLOW_Y, P_Z_INDEX, P_OPACITY,
    P_WIDTH, P_HEIGHT, P_MIN_WIDTH, P_MIN_HEIGHT, P_MAX_WIDTH, P_MAX_HEIGHT,
    P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM, P_MARGIN_LEFT,
    P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_TOP, P_RIGHT, P_BOTTOM, P_LEFT,
    P_BTW, P_BRW, P_BBW, P_BLW,           // border widths
    P_BTS, P_BRS, P_BBS, P_BLS,           // border styles
    P_BTC, P_BRC, P_BBC, P_BLC,           // border colors
    P_RTL, P_RTR, P_RBR, P_RBL,           // radii
    P_COLOR, P_BG_COLOR, P_BG_IMAGE, P_BG_REPEAT, P_BG_SIZE, P_BG_POS_X, P_BG_POS_Y,
    P_FONT_FAMILY, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE, P_FONT_VARIANT, P_LINE_HEIGHT,
    P_TEXT_ALIGN, P_DECO_LINE, P_DECO_COLOR, P_DECO_STYLE, P_TEXT_TRANSFORM, P_TEXT_INDENT,
    P_TEXT_OVERFLOW, P_LETTER_SPACING, P_WORD_SPACING, P_WHITE_SPACE, P_WORD_BREAK,
    P_OVERFLOW_WRAP, P_VERTICAL_ALIGN, P_DIRECTION,
    P_LIST_STYLE_TYPE, P_LIST_STYLE_POSITION,
    P_FLEX_DIRECTION, P_FLEX_WRAP, P_JUSTIFY_CONTENT, P_ALIGN_ITEMS, P_ALIGN_SELF,
    P_ALIGN_CONTENT, P_JUSTIFY_ITEMS, P_JUSTIFY_SELF, P_FLEX_GROW, P_FLEX_SHRINK,
    P_FLEX_BASIS, P_ORDER, P_ROW_GAP, P_COLUMN_GAP,
    P_GRID_TEMPLATE_COLUMNS, P_GRID_TEMPLATE_ROWS, P_GRID_TEMPLATE_AREAS, P_GRID_AUTO_FLOW,
    P_GRID_AUTO_COLUMNS, P_GRID_AUTO_ROWS, P_GRID_COLUMN_START, P_GRID_COLUMN_END,
    P_GRID_ROW_START, P_GRID_ROW_END,
    P_BORDER_COLLAPSE, P_BORDER_SPACING, P_TABLE_LAYOUT, P_CAPTION_SIDE,
    P_CONTENT, P_BOX_SHADOW, P_TRANSFORM, P_ASPECT_RATIO, P_OBJECT_FIT, P_POINTER_EVENTS,
    P_CURSOR, P_OUTLINE_WIDTH, P_OUTLINE_STYLE, P_OUTLINE_COLOR,
    P_FILL, P_STROKE, P_STROKE_WIDTH,
    P_COUNT,
    P_CUSTOM = 0xFFF0   // --custom-property (name in raw text)
};

// ---- specified values ------------------------------------------------------------
enum {
    VK_NONE,     // invalid / unset
    VK_KW,       // a = keyword id (property-specific enum, or KW_* globals)
    VK_LEN,      // a = value * 1000, unit = U_*
    VK_PCT,      // a = percent * 1000
    VK_NUM,      // a = number * 1000
    VK_COLOR,    // a = ARGB; b = 1 -> currentColor
    VK_CALC,     // a = index into the sheet's calc table
    VK_RAW,      // raw text (raw/rawlen): grid templates, content, url, gradients...
    VK_VAR,      // raw text containing var(): substituted + reparsed at compute
    VK_AUTO,
    VK_LEN2      // two lengths (border-spacing, bg-size, radius x y): a,unit / b,unit2
};
// global keywords (stored in VK_KW a)
#define KW_INHERIT 1000
#define KW_INITIAL 1001
#define KW_UNSET   1002
#define KW_REVERT  1003

enum { U_PX, U_EM, U_REM, U_VW, U_VH, U_VMIN, U_VMAX, U_EX, U_CH, U_PT, U_PC, U_IN, U_CM,
       U_MM, U_Q, U_LH, U_DEG, U_RAD, U_TURN, U_GRAD, U_FR, U_S, U_MS, U_DPPX, U_X };

struct cdecl {
    uint16_t prop;
    uint8_t important;
    uint8_t kind;
    uint8_t unit, unit2;
    uint16_t _pad;
    int32_t a, b;
    uint32_t raw, rawlen;      // raw value text (sheet text pool)
    uint32_t name, namelen;    // P_CUSTOM: property name (sheet text pool)
};

// calc(): linear combination of units (each * 1000)
struct ccalc { int32_t px, pct, em, rem, vw, vh; };

// ---- selectors ------------------------------------------------------------------
enum { SK_TAG, SK_UNIV, SK_ID, SK_CLASS, SK_ATTR, SK_PSEUDO, SK_COMB, SK_END };
enum { CB_DESC, CB_CHILD, CB_ADJ, CB_SIB };
enum { AO_EXISTS, AO_EQ, AO_INCLUDES, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };
enum {
    PC_NONE, PC_FIRST_CHILD, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD,
    PC_FIRST_OF_TYPE, PC_LAST_OF_TYPE, PC_ONLY_OF_TYPE, PC_NTH_OF_TYPE, PC_NTH_LAST_OF_TYPE,
    PC_NOT, PC_IS, PC_WHERE, PC_HAS, PC_ROOT, PC_EMPTY, PC_LINK, PC_VISITED, PC_HOVER,
    PC_ACTIVE, PC_FOCUS, PC_FOCUS_VISIBLE, PC_FOCUS_WITHIN, PC_CHECKED, PC_DISABLED,
    PC_ENABLED, PC_REQUIRED, PC_OPTIONAL, PC_READ_ONLY, PC_READ_WRITE, PC_PLACEHOLDER_SHOWN,
    PC_LANG, PC_TARGET, PC_NEVER, PC_ALWAYS, PC_DEFAULT, PC_INDETERMINATE, PC_OPEN
};
enum { PE_NONE, PE_BEFORE, PE_AFTER, PE_MARKER, PE_PLACEHOLDER, PE_OTHER };

struct cpart {
    uint8_t kind;        // SK_*
    uint8_t op;          // combinator / attr op / pseudo-class id
    uint8_t ci;          // attribute value case-insensitive
    uint8_t _pad;
    uint16_t atom;       // tag/id/class/attr-name atom
    uint16_t _pad2;
    int32_t a, b;        // attr value (pool off, len) | nth (a, b) | sublist index
    int32_t sub;         // nested selector list (first selector index), -1 none
    int32_t nsub;        // count of selectors in the nested list
};

struct csel {
    int32_t first;       // index into parts[] (compounds left-to-right, SK_END terminated)
    uint32_t spec;       // specificity a<<20 | b<<10 | c
    uint8_t pseudo;      // PE_*
    uint8_t has_rel;     // :has relative selector: leading combinator present
};

struct crule {
    int32_t sel;         // selector index
    uint32_t spec;
    int32_t decl, ndecl; // range in decls[]
    int32_t mq;          // media/container condition index, -1 always
    uint32_t order;      // source order within the sheet
    uint8_t pseudo;
};

struct cmq {             // media query list (stored raw, evaluated per layout pass)
    uint32_t raw, rawlen;
};

struct wsheet {
    struct wbuf pool;        // copies of value/selector text
    struct cdecl* decls; int ndecl, capdecl;
    struct crule* rules; int nrule, caprule;
    struct csel* sels; int nsel, capsel;
    struct cpart* parts; int npart, cappart;
    struct ccalc* calcs; int ncalc, capcalc;
    struct cmq* mqs; int nmq, capmq;
    char base[256];
    int origin;
    // @import URLs (absolute), NUL separated
    struct wbuf imports;
    int nimports;
    // transient parse source: text being parsed (never the pool itself)
    // and the pool offset holding an identical copy of it
    const char* src;
    uint32_t src_off;
    int src_len;
    int outer_mq;        // <link media>/<style media> condition (-1 none)
};

// Parse into an existing sheet (atoms interned into `atoms`; NULL atoms
// table = static UA mode: only known names allowed).
void csheet_parse(struct wsheet* sh, struct watoms* atoms, const char* text, int len);
void csheet_set_media(struct wsheet* sh, const char* media, int len);
void csheet_free(struct wsheet* sh);
// Parse a declaration list (style attribute) into sh, returning first/count.
void csheet_parse_decls(struct wsheet* sh, struct watoms* atoms, const char* text, int len,
                        int* first, int* count);
// Parse one value for a property name into decls (used for var() substitution).
// Appends to sh->decls; returns the number of decls appended.
int  cparse_value(struct wsheet* sh, const char* name, int nlen, const char* val, int vlen,
                  int important);

// Selector matching (css_select.c)
struct cmatch_ctx {
    const struct wdom* d;
    const struct wsheet* sh;
};
int  csel_match(const struct wdom* d, const struct wsheet* sh, int sel, int el);

// Value helpers (css_values.c)
int  cv_number(const char* s, int len, int32_t* milli, int* used);
int  cv_unit(const char* s, int len);
int  cv_color(const char* s, int len, uint32_t* argb, int* current);
int  cv_ident_eq(const char* s, int len, const char* lit);
// Split the next whitespace-separated component (respecting () and quotes).
int  cv_next(const char* s, int len, int* pos, const char** cs, int* clen);
// Split by top-level commas.
int  cv_next_comma(const char* s, int len, int* pos, const char** cs, int* clen);
void cv_trim(const char** s, int* len);

#endif
