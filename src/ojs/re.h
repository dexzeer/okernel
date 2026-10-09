// re.h — ojs regular expressions: a compiler from pattern text to a
// small bytecode, and a backtracking matcher (ECMA-262 §22.2).
#ifndef OJS_RE_H
#define OJS_RE_H

#include "ojs_int.h"

#define RF_G 0x01   // global
#define RF_I 0x02   // ignoreCase
#define RF_M 0x04   // multiline
#define RF_S 0x08   // dotAll
#define RF_U 0x10   // unicode
#define RF_Y 0x20   // sticky
#define RF_D 0x40   // hasIndices
#define RF_V 0x80   // unicodeSets

// compiled program (gc bytes: no pointers inside)
struct re_prog {
    uint32_t flags;
    uint32_t ncaps;         // capture groups + 1 (group 0 = whole match)
    uint32_t nregs;         // loop registers
    uint32_t code_len;      // in uint32 units
    uint32_t names_len;     // bytes of the group name table (UTF-8, "\0"-separated, per group)
    uint32_t has_dup_names;
    uint32_t code[];        // code, then the name table
};

// flags text -> RF_* (-1: invalid)
int  re_parse_flags(const struct str* f);
// compile; NULL with a message in err on a syntax error (or OOM)
struct re_prog* re_compile(ojs* J, const struct str* pattern, int flags, char* err, int errcap);
// group i's name (UTF-8) or NULL
const char* re_group_name(const struct re_prog* p, uint32_t i);
// match at `start` (sticky) or the first position from `start` on;
// caps[2*ncaps] receive start/end pairs (-1 unmatched).
// 1 match, 0 no match, -1 exception.
int  re_exec(ojs* J, const struct re_prog* p, const struct str* s, uint32_t start, int32_t* caps, int sticky);
int  regexp_check_syntax(ojs* J, struct str* body, struct str* flags, char* err, int errcap);

#endif
