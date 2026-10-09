// lex.h — ECMAScript tokenizer (ECMA-262 §12).
#ifndef OJS_LEX_H
#define OJS_LEX_H

#include "ojs_int.h"

enum tok {
    T_EOF = 0, T_ERROR,
    T_IDENT, T_PRIVATE, T_NUM, T_BIGINT, T_STRING, T_TEMPLATE, T_REGEXP,
    // punctuators
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_DOT, T_ELLIPSIS, T_SEMI, T_COMMA,
    T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE, T_SEQ, T_SNE, T_PLUS, T_MINUS, T_STAR, T_PERCENT, T_STARSTAR,
    T_INC, T_DEC, T_SHL, T_SAR, T_SHR, T_AMP, T_PIPE, T_CARET, T_NOT, T_TILDE, T_AND, T_OR, T_NULLISH,
    T_QUESTION, T_OPTCHAIN, T_COLON, T_ASSIGN, T_PLUS_ASSIGN, T_MINUS_ASSIGN, T_STAR_ASSIGN,
    T_PERCENT_ASSIGN, T_STARSTAR_ASSIGN, T_SHL_ASSIGN, T_SAR_ASSIGN, T_SHR_ASSIGN, T_AMP_ASSIGN,
    T_PIPE_ASSIGN, T_CARET_ASSIGN, T_AND_ASSIGN, T_OR_ASSIGN, T_NULLISH_ASSIGN, T_ARROW, T_SLASH,
    T_SLASH_ASSIGN, T_AT,
    // reserved words (only ones that are always reserved; contextual words are T_IDENT)
    T_AWAIT_KW_UNUSED, T_BREAK, T_CASE, T_CATCH, T_CLASS, T_CONST, T_CONTINUE, T_DEBUGGER, T_DEFAULT,
    T_DELETE, T_DO, T_ELSE, T_ENUM, T_EXPORT, T_EXTENDS, T_FALSE, T_FINALLY, T_FOR, T_FUNCTION, T_IF,
    T_IMPORT, T_IN, T_INSTANCEOF, T_NEW, T_NULL, T_RETURN, T_SUPER, T_SWITCH, T_THIS, T_THROW, T_TRUE,
    T_TRY, T_TYPEOF, T_VAR, T_VOID, T_WHILE, T_WITH,
    T_COUNT
};

struct token {
    int type;
    uint32_t start, end;        // source offsets (code units)
    int nl_before;              // a line terminator precedes the token
    int escaped;                // identifier/keyword written with \u escapes
    struct str* str;            // identifier name / string value (atom) / template cooked (NULL: invalid escape)
    struct str* raw;            // template raw / regexp body
    struct str* flags;          // regexp flags
    double num;
    int legacy_octal;           // number literal 0777 / string with \0nn escape (strict errors)
    int template_tail;          // template token ends the template (no ${)
    int bad_escape;             // \8 \9 or octal escape inside a string (strict / template errors)
};

struct lexer {
    ojs* J;
    struct str* src;
    uint32_t pos, len;
    int module;                 // HTML comments are not allowed in modules
    int strict;
    int seen_token;             // a token was produced (`-->` rule)
    struct token t;             // current token
    char err[200];
    uint32_t err_pos;
    // line table (computed on demand)
    uint32_t* line_starts;
    uint32_t nlines;
};

void lex_init(struct lexer* L, ojs* J, struct str* src, int module);
void lex_free(struct lexer* L);
int  lex_next(struct lexer* L, int regex_ok);            // advance; returns token type
int  lex_template_continue(struct lexer* L);             // after '}' of a substitution
void lex_rescan_regex(struct lexer* L);                  // current '/' or '/=' token is a regex
void lex_line_col(struct lexer* L, uint32_t pos, int* line, int* col);
int  lex_is_keyword(struct str* s);                      // token type for reserved words, 0 otherwise
const char* tok_name(int t);
static inline uint32_t lex_cu(struct lexer* L, uint32_t i) { return i < L->len ? str_at(L->src, i) : 0xFFFFFFFFu; }

#endif
