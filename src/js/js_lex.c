/*
 * okai JS — lexer (CScriptLex port).
 *
 * Char-based lexer with position tracking. Mirrors the C++ getNextToken
 * tokenizer: comments, identifiers/keywords, int/hex/float/exp numbers,
 * single/double-quoted strings with escapes, and multi-char operators.
 *
 * MIT — derived from TinyJS (C) 2009 Pur3 Ltd, Gordon Williams.
 */
#include "js.h"
#include <string.h>

static int js_is_id_start(char c) {
    return c == '#' || c == '_' || c == '$' ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int js_is_id(char c) {
    return js_is_id_start(c) || (c >= '0' && c <= '9');
}
static int js_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ---- token string (for errors) ---- */
const char *js_lex_token_str(int tk) {
    static char buf[64];
    return js_lex_token_str_buf(tk, buf);
}
const char *js_lex_token_str_buf(int tk, char *buf) {
    switch (tk) {
        case JS_EOF:      return "<EOF>";
        case JS_ID:       return "Identifier";
        case JS_INT:      return "Integer";
        case JS_FLOAT:    return "Float";
        case JS_STR:      return "String";
        case JS_EQUAL:        return "'=='";
        case JS_TYPEEQUAL:    return "'==='";
        case JS_NEQUAL:       return "'!='";
        case JS_NTYPEEQUAL:   return "'!=='";
        case JS_LEQUAL:       return "'<='";
        case JS_LSHIFT:       return "'<<'";
        case JS_LSHIFTEQUAL:  return "'<<='";
        case JS_GEQUAL:       return "'>='";
        case JS_RSHIFT:       return "'>>'";
        case JS_RSHIFTUNSIGNED: return "'>>>'";
        case JS_RSHIFTEQUAL:  return "'>>='";
        case JS_PLUSEQUAL:    return "'+='";
        case JS_MINUSEQUAL:   return "'-='";
        case JS_PLUSPLUS:     return "'++'";
        case JS_MINUSMINUS:   return "'--'";
        case JS_ANDEQUAL:     return "'&='";
        case JS_ANDAND:       return "'&&'";
        case JS_OREQUAL:      return "'|='";
        case JS_OROR:         return "'||'";
        case JS_XOREQUAL:     return "'^='";
        case JS_R_IF:         return "'if'";
        case JS_R_ELSE:       return "'else'";
        case JS_R_DO:         return "'do'";
        case JS_R_WHILE:      return "'while'";
        case JS_R_FOR:        return "'for'";
        case JS_R_BREAK:      return "'break'";
        case JS_R_CONTINUE:   return "'continue'";
        case JS_R_FUNCTION:   return "'function'";
        case JS_R_RETURN:     return "'return'";
        case JS_R_VAR:        return "'var'";
        case JS_R_TRUE:       return "'true'";
        case JS_R_FALSE:      return "'false'";
        case JS_R_NULL:       return "'null'";
        case JS_R_UNDEFINED:  return "'undefined'";
        case JS_R_NEW:        return "'new'";
        default:
            if (tk >= 32 && tk < 127) { buf[0] = (char)tk; buf[1] = 0; return buf; }
            js_sprintf(buf, sizeof(buf), "<%d>", tk);
            return buf;
    }
}

/* ---- character source ---- */
static char js_lex_next(js_lex *l) {
    l->currCh = l->nextCh;
    if (l->dataPos < l->dataEnd) {
        l->nextCh = l->data[l->dataPos];
        l->dataPos++;
    } else {
        l->nextCh = 0;
        l->dataPos++;   /* keep advancing so tokenStart=dataPos-2 stays consistent at EOF */
    }
    return l->currCh;
}

static void js_tok_clear(js_lex *l) { if (l->tkStr) l->tkStr[0] = 0; }
static void js_tok_append(js_lex *l, char c) {
    int len = l->tkStr ? (int)strlen(l->tkStr) : 0;
    if (len + 2 > l->tkStrCap) {
        l->tkStrCap = l->tkStrCap ? l->tkStrCap * 2 : 32;
        l->tkStr = (char*)js_realloc(l->tkStr, l->tkStrCap);
    }
    l->tkStr[len] = c;
    l->tkStr[len + 1] = 0;
}

void js_lex_next_token(js_lex *l) {
    l->tk = JS_EOF;
    js_tok_clear(l);
    do {
        /* whitespace */
        while (l->currCh && js_is_whitespace(l->currCh)) js_lex_next(l);
        /* line comment */
        if (l->currCh == '/' && l->nextCh == '/') {
            while (l->currCh && l->currCh != '\n') js_lex_next(l);
            if (l->currCh) js_lex_next(l);
            continue;
        }
        /* block comment */
        if (l->currCh == '/' && l->nextCh == '*') {
            js_lex_next(l); js_lex_next(l);
            while (l->currCh && !(l->currCh == '*' && l->nextCh == '/'))
                js_lex_next(l);
            if (l->currCh) { js_lex_next(l); js_lex_next(l); }
            continue;
        }
        l->tokenStart = l->dataPos - 2;
        /* identifier / keyword */
        if (js_is_id_start(l->currCh)) {
            while (js_is_id(l->currCh)) { js_tok_append(l, l->currCh); js_lex_next(l); }
            if      (js_strcmp(l->tkStr, "if") == 0)          l->tk = JS_R_IF;
            else if (js_strcmp(l->tkStr, "else") == 0)        l->tk = JS_R_ELSE;
            else if (js_strcmp(l->tkStr, "do") == 0)          l->tk = JS_R_DO;
            else if (js_strcmp(l->tkStr, "while") == 0)       l->tk = JS_R_WHILE;
            else if (js_strcmp(l->tkStr, "for") == 0)         l->tk = JS_R_FOR;
            else if (js_strcmp(l->tkStr, "break") == 0)       l->tk = JS_R_BREAK;
            else if (js_strcmp(l->tkStr, "continue") == 0)    l->tk = JS_R_CONTINUE;
            else if (js_strcmp(l->tkStr, "function") == 0)    l->tk = JS_R_FUNCTION;
            else if (js_strcmp(l->tkStr, "return") == 0)      l->tk = JS_R_RETURN;
            else if (js_strcmp(l->tkStr, "var") == 0)         l->tk = JS_R_VAR;
            else if (js_strcmp(l->tkStr, "true") == 0)        { l->tk = JS_R_TRUE; }
            else if (js_strcmp(l->tkStr, "false") == 0)       { l->tk = JS_R_FALSE; }
            else if (js_strcmp(l->tkStr, "null") == 0)        { l->tk = JS_R_NULL; }
            else if (js_strcmp(l->tkStr, "undefined") == 0)   { l->tk = JS_R_UNDEFINED; }
            else if (js_strcmp(l->tkStr, "new") == 0)         { l->tk = JS_R_NEW; }
            else l->tk = JS_ID;
            break;
        }
        /* number */
        if (l->currCh >= '0' && l->currCh <= '9') {
            int isHex = 0, isFloat = 0;
            if (l->currCh == '0' && (l->nextCh == 'x' || l->nextCh == 'X')) isHex = 1;
            if (!isHex) {
                while (l->currCh >= '0' && l->currCh <= '9') { js_tok_append(l, l->currCh); js_lex_next(l); }
                if (l->currCh == '.') {
                    isFloat = 1;
                    js_tok_append(l, l->currCh); js_lex_next(l);
                    while (l->currCh >= '0' && l->currCh <= '9') { js_tok_append(l, l->currCh); js_lex_next(l); }
                }
                if (l->currCh == 'e' || l->currCh == 'E') {
                    isFloat = 1;
                    js_tok_append(l, l->currCh); js_lex_next(l);
                    if (l->currCh == '+' || l->currCh == '-') { js_tok_append(l, l->currCh); js_lex_next(l); }
                    while (l->currCh >= '0' && l->currCh <= '9') { js_tok_append(l, l->currCh); js_lex_next(l); }
                }
            } else {
                js_tok_append(l, l->currCh); js_lex_next(l); /* 0 */
                js_tok_append(l, l->currCh); js_lex_next(l); /* x */
                while ((l->currCh >= '0' && l->currCh <= '9') ||
                       (l->currCh >= 'a' && l->currCh <= 'f') ||
                       (l->currCh >= 'A' && l->currCh <= 'F')) {
                    js_tok_append(l, l->currCh); js_lex_next(l);
                }
            }
            l->tk = isFloat ? JS_FLOAT : JS_INT;
            break;
        }
        /* string */
        if (l->currCh == '"' || l->currCh == '\'') {
            char quote = l->currCh;
            js_lex_next(l); /* skip opening quote */
            while (l->currCh && l->currCh != quote) {
                if (l->currCh == '\\') {
                    js_lex_next(l);
                    char c = l->currCh;
                    switch (l->currCh) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'v': c = '\v'; break;
                        case '0': c = '\0'; break;
                        case 'x': {
                            /* hex escape \xHH */
                            int v = 0, i;
                            js_lex_next(l);
                            for (i = 0; i < 2 && ((l->currCh>='0'&&l->currCh<='9')||(l->currCh>='a'&&l->currCh<='f')||(l->currCh>='A'&&l->currCh<='F')); i++) {
                                v = v*16 + (l->currCh>='0'&&l->currCh<='9' ? l->currCh-'0'
                                     : (l->currCh>='a'&&l->currCh<='f' ? l->currCh-'a'+10 : l->currCh-'A'+10));
                                js_lex_next(l);
                            }
                            c = (char)v;
                            break;
                        }
                        default: break; /* pass through the char (incl. escaped quote) */
                    }
                    js_tok_append(l, c);
                    js_lex_next(l);
                } else {
                    js_tok_append(l, l->currCh);
                    js_lex_next(l);
                }
            }
            js_lex_next(l); /* skip closing quote */
            l->tk = JS_STR;
            break;
        }
        /* multi-char operators */
        if (l->currCh == '=' && l->nextCh == '=') {
            js_lex_next(l); js_lex_next(l);
            if (l->currCh == '=') { js_lex_next(l); l->tk = JS_TYPEEQUAL; }
            else l->tk = JS_EQUAL;
            break;
        }
        if (l->currCh == '!' && l->nextCh == '=') {
            js_lex_next(l); js_lex_next(l);
            if (l->currCh == '=') { js_lex_next(l); l->tk = JS_NTYPEEQUAL; }
            else l->tk = JS_NEQUAL;
            break;
        }
        if (l->currCh == '<' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_LEQUAL; break; }
        if (l->currCh == '<' && l->nextCh == '<') {
            js_lex_next(l); js_lex_next(l);
            if (l->currCh == '=') { js_lex_next(l); l->tk = JS_LSHIFTEQUAL; }
            else l->tk = JS_LSHIFT;
            break;
        }
        if (l->currCh == '>' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_GEQUAL; break; }
        if (l->currCh == '>' && l->nextCh == '>') {
            js_lex_next(l); js_lex_next(l);
            if (l->currCh == '>') { js_lex_next(l); l->tk = JS_RSHIFTUNSIGNED; }
            else if (l->currCh == '=') { js_lex_next(l); l->tk = JS_RSHIFTEQUAL; }
            else l->tk = JS_RSHIFT;
            break;
        }
        if (l->currCh == '+' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_PLUSEQUAL; break; }
        if (l->currCh == '-' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_MINUSEQUAL; break; }
        if (l->currCh == '+' && l->nextCh == '+') { js_lex_next(l); js_lex_next(l); l->tk = JS_PLUSPLUS; break; }
        if (l->currCh == '-' && l->nextCh == '-') { js_lex_next(l); js_lex_next(l); l->tk = JS_MINUSMINUS; break; }
        if (l->currCh == '&' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_ANDEQUAL; break; }
        if (l->currCh == '&' && l->nextCh == '&') { js_lex_next(l); js_lex_next(l); l->tk = JS_ANDAND; break; }
        if (l->currCh == '|' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_OREQUAL; break; }
        if (l->currCh == '|' && l->nextCh == '|') { js_lex_next(l); js_lex_next(l); l->tk = JS_OROR; break; }
        if (l->currCh == '^' && l->nextCh == '=') { js_lex_next(l); js_lex_next(l); l->tk = JS_XOREQUAL; break; }
        /* single char */
        l->tk = l->currCh;
        js_lex_next(l);
        break;
    } while (1);
    l->tokenLastEnd = l->tokenEnd;
    l->tokenEnd = l->dataPos - 3;
}

void js_lex_match(js_lex *l, int expected) {
    if (l->tk != expected) {
        char msg[256];
        char ebuf[64], gbuf[64];
        js_sprintf(msg, sizeof(msg), "expected %s but got %s (at '%s')",
                   js_lex_token_str_buf(expected, ebuf),
                   js_lex_token_str_buf(l->tk, gbuf), l->tkStr);
        js_throw_current(msg);
    }
    js_lex_next_token(l);
}

void js_lex_position(js_lex *l, char *buf, int len) {
    int line = 1, col = 0;
    int i;
    for (i = 0; i < l->tokenLastEnd; i++) {
        if (l->data[i] == '\n') { line++; col = 0; }
        else col++;
    }
    js_sprintf(buf, len, "line %d, col %d", line, col);
}

/* ---- construction ---- */
static void js_lex_init(js_lex *l, int owned) {
    l->currCh = l->nextCh = 0;
    l->tk = JS_EOF;
    l->tokenStart = l->tokenEnd = l->tokenLastEnd = 0;
    l->tkStr = 0; l->tkStrCap = 0;
    l->dataPos = 0;
    l->dataOwned = owned;
}

void js_lex_reset(js_lex *l) {
    l->dataPos = l->dataStart;
    l->tokenLastEnd = l->dataStart;
    l->currCh = l->nextCh = 0;
    js_lex_next(l);
    js_lex_next(l);
    js_lex_next_token(l);
}

js_lex *js_lex_create(const char *data) {
    js_lex *l = (js_lex*)js_malloc(sizeof(js_lex));
    js_lex_init(l, 0);
    l->data = (char*)js_strdup(data ? data : "");
    l->dataOwned = 1; /* we own the dup'd data */
    l->dataStart = 0;
    l->dataEnd = (int)strlen(l->data);
    js_lex_reset(l);
    return l;
}

js_lex *js_lex_create_sub(js_lex *owner, int start, int len) {
    js_lex *l = (js_lex*)js_malloc(sizeof(js_lex));
    js_lex_init(l, 0);
    l->data = owner->data;          /* borrowed */
    l->dataOwned = 0;
    l->dataStart = start;
    l->dataEnd = start + len;
    js_lex_reset(l);
    return l;
}

void js_lex_free(js_lex *l) {
    if (!l) return;
    if (l->dataOwned && l->data) js_free(l->data);
    if (l->tkStr) js_free(l->tkStr);
    js_free(l);
}
