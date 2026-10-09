// parse.h — AST, scopes and the parser interface.
//
// The parser builds a tree of struct node (arena-allocated, freed after
// compilation) and records every declaration in its scope as it goes, so
// that the compiler can resolve identifiers against complete scopes
// (hoisting, TDZ, closures) without another pass. Early errors of
// ECMA-262 are reported by the parser.
#ifndef OJS_PARSE_H
#define OJS_PARSE_H

#include "lex.h"

enum ntype {
    // expressions
    N_IDENT = 1, N_NUM, N_STR, N_BIGINT, N_TEMPLATE, N_TAGGED, N_REGEXP, N_NULL, N_TRUE, N_FALSE, N_THIS,
    N_SUPER, N_NEW_TARGET, N_IMPORT_META, N_ARRAY, N_OBJECT, N_PROP, N_FUNC, N_CLASS, N_MEMBER_DEF,
    N_UNARY, N_UPDATE, N_BINARY, N_LOGICAL, N_ASSIGN, N_COND, N_CALL, N_NEW, N_MEMBER, N_OPTCHAIN,
    N_SEQ, N_SPREAD, N_YIELD, N_AWAIT, N_IMPORT_CALL, N_PRIVATE_IN, N_HOLE, N_CLASS_STATIC_BLOCK,
    // patterns
    N_ARRAY_PAT, N_OBJECT_PAT, N_ASSIGN_PAT, N_REST,
    // statements
    N_VAR, N_DECLARATOR, N_EXPR_STMT, N_BLOCK, N_EMPTY, N_IF, N_FOR, N_FOR_IN, N_FOR_OF, N_WHILE,
    N_DO_WHILE, N_CONTINUE, N_BREAK, N_RETURN, N_WITH, N_SWITCH, N_CASE, N_LABEL, N_THROW, N_TRY,
    N_DEBUGGER, N_IMPORT_DECL, N_IMPORT_SPEC, N_EXPORT, N_EXPORT_SPEC, N_PROGRAM,
};

// node flags
#define NF_PAREN       0x0001   // parenthesized expression
#define NF_OPTIONAL    0x0002   // member/call is `?.`
#define NF_COMPUTED    0x0004   // member/property key is computed ([...])
#define NF_PRIVATE     0x0008   // member/property key is a private name
#define NF_STATIC      0x0010   // class member
#define NF_SHORTHAND   0x0020   // { a } / { a = 1 }
#define NF_COVER_INIT  0x0040   // { a = 1 } in an object literal (must become a pattern)
#define NF_DELEGATE    0x0080   // yield*
#define NF_PREFIX      0x0100   // ++x
#define NF_AWAIT       0x0200   // for await
#define NF_ANNEXB      0x0400   // function declaration in a block (sloppy): also a var
#define NF_DIRECT_EVAL 0x0800   // call is a direct eval
#define NF_TAIL        0x1000   // (unused) call in tail position
#define NF_HAS_SPREAD  0x2000   // call/new/array literal arguments contain spread
#define NF_CONST_LEX   0x4000   // for-in/of head declaration is const

// property kinds (N_PROP.op / N_MEMBER_DEF.op)
enum { PK_INIT, PK_GET, PK_SET, PK_METHOD, PK_SPREAD, PK_PROTO, PK_FIELD, PK_ACCESSOR_AUTO };

// declaration kinds
enum { D_VAR = 1, D_LET, D_CONST, D_FUNC, D_CLASS, D_PARAM, D_CATCH, D_IMPORT, D_FUNCNAME, D_CLASSNAME,
       D_ARGUMENTS, D_THIS, D_NEWTARGET, D_HOME, D_WITH, D_EVALVARS, D_PRIVATE_BRAND, D_INTERNAL };
#define DF_CAPTURED   0x01      // referenced from an inner function
#define DF_TDZ        0x02      // let/const/class: uninitialized until the declaration runs
#define DF_LEX_FUNC   0x04      // function declaration in a block (lexical)
#define DF_IMPORT_NS  0x08      // import * as x
#define DF_ASSIGNED   0x10      // written after initialization
#define DF_PARAM_DUP  0x20

struct scope;
struct funcinfo;

struct decl {
    struct str* name;
    uint8_t kind;
    uint8_t flags;
    uint16_t pad;
    struct node* node;          // declaring node (function/class node for D_FUNC/D_CLASS)
    struct decl* next;          // scope order
    struct scope* scope;
    int slot;                   // compiler: local slot, or module cell index; -1 unassigned
    struct str* import_name;    // D_IMPORT: the exported name ('*namespace*' for namespace)
    struct str* import_from;    // D_IMPORT: module specifier
};

enum { SC_FUNC_PARAMS = 1, SC_FUNC_BODY, SC_BLOCK, SC_CATCH, SC_CLASS, SC_SCRIPT, SC_MODULE, SC_EVAL,
       SC_FUNC_NAME, SC_WITH, SC_STATIC_BLOCK };

struct scope {
    uint8_t kind;
    uint8_t has_eval;           // a direct eval may see this scope's names
    uint8_t is_loop_body;       // per-iteration bindings (for-let)
    uint8_t entered;            // compiler: slots assigned (a finally body is compiled once per exit path)
    struct scope* parent;
    struct funcinfo* fn;
    struct decl* decls;
    struct decl* decls_tail;
    int ndecls;
    struct decl** hash;         // lazily built name index
    int hsize;
    // var names declared in this scope or nested blocks (lexical conflict checks)
    struct str** varnames;
    int nvarnames, capvarnames;
    int first_slot;             // compiler bookkeeping
    struct node* owner;         // block/for/switch/catch/class node
};

// private names of a class body
struct private_name {
    struct str* name;
    int kind;                   // PK_FIELD / PK_METHOD / PK_GET / PK_SET / both accessors (PK_GET|..)
    int is_static;
    struct private_name* next;
    struct decl* d;             // binding holding the private name symbol
};

struct funcinfo {
    struct funcinfo* parent;
    struct node* node;          // the N_FUNC node
    struct node* params;        // list of param patterns (N_IDENT / patterns / N_ASSIGN_PAT / N_REST)
    struct node* body;          // list of statements (or an expression for concise arrows)
    struct str* name;           // name (declared or inferred)
    struct scope* param_scope;
    struct scope* body_scope;   // var scope (same as param_scope for simple params)
    uint32_t src_start, src_end;// source range for Function.prototype.toString
    int nparams_len;            // "length": params before the first default/rest
    int nparams;
    uint32_t flags;
    struct funcinfo* children;  // nested functions (compile order)
    struct funcinfo* last_child;// its tail (a bundle's wrapper has tens of thousands)
    struct funcinfo* next_sibling;
    struct ftempl* tmpl;        // compiler output
    struct node* class_node;    // the class whose constructor / field initializer this is
    int depth;
};
#define FI_ARROW        0x00001
#define FI_ASYNC        0x00002
#define FI_GENERATOR    0x00004
#define FI_METHOD       0x00008   // has a home object (super property access allowed)
#define FI_GETTER       0x00010
#define FI_SETTER       0x00020
#define FI_CLASS_CTOR   0x00040
#define FI_DERIVED      0x00080   // class constructor of a class with `extends`
#define FI_STRICT       0x00100
#define FI_SIMPLE_PARAMS 0x00200
#define FI_USES_ARGS    0x00400
#define FI_USES_THIS    0x00800   // this / super / new.target referenced (arrow: from outside)
#define FI_DIRECT_EVAL  0x01000   // contains a direct eval (itself or a nested arrow)
#define FI_EXPR_BODY    0x02000   // concise arrow body
#define FI_DECL         0x04000   // function declaration
#define FI_EXPR         0x08000   // function expression (own name binding)
#define FI_FIELD_INIT   0x10000   // class fields initializer
#define FI_STATIC_BLOCK 0x20000
#define FI_SCRIPT       0x40000   // top-level script
#define FI_MODULE       0x80000
#define FI_EVAL         0x100000
#define FI_USES_SUPER_CALL 0x200000
#define FI_USES_SUPER_PROP 0x400000
#define FI_HAS_YIELD    0x800000
#define FI_CLASS_FIELDS 0x1000000  // constructor must run field initializers
#define FI_NEW_FUNCTION 0x2000000  // body of new Function(...)
#define FI_USES_NEW_TARGET 0x4000000
#define FI_OWN_EVAL     0x8000000   // a direct eval call directly in this function (sloppy vars land here)

struct node {
    uint8_t type;
    uint8_t op;                 // operator token / declaration kind / property kind
    uint16_t flags;
    uint32_t pos;               // source offset
    uint32_t end;               // source end offset (functions, classes, templates)
    struct node *a, *b, *c, *d;
    struct node* next;          // list sibling
    union {
        double num;
        struct str* str;
        struct funcinfo* fn;
        struct decl* decl;
        int ival;
    } u;
    struct str* str2;           // raw template / regexp flags / import specifier / label
    struct scope* scope;        // scope at this node (identifiers) or owned scope (blocks)
    struct private_name* privs; // class: private names
};

struct arena_chunk;

struct parser {
    ojs* J;
    struct lexer L;
    struct arena_chunk* arena;
    struct funcinfo* F;         // current function
    struct scope* S;            // current scope
    struct node* program;
    struct funcinfo* top;
    // error
    int failed;
    char msg[256];
    uint32_t err_pos;
    const char* filename;
    // context
    int in_function_body;
    int allow_in;               // `in` allowed in relational expressions
    int in_class_field;         // arguments forbidden
    int in_static_block;
    int label_depth;
    struct label* labels;
    int iteration_depth;
    int switch_depth;
    uint32_t prev_end;          // end of the previous token (ASI, ranges)
    struct node* cover_init;    // first `{ a = 1 }` seen while parsing an expression (error unless reinterpreted)
    struct class_ctx* cls;      // enclosing class bodies (private names)
    // module
    int is_module;
    struct node* exports;       // N_EXPORT_SPEC list (local name -> export name)
    struct node* imports;
    struct str** export_names;
    int nexport_names, capexport_names;
    struct node* unresolved_exports;   // checked at the end (must be declared)
    // eval
    int eval_kind;              // 0 none, 1 indirect/global eval, 2 direct eval
    int gc_held;                // the collector is paused until parse_free (the AST and the
                                // compiler's constant lists hold strings it cannot see)
    struct ojs_eval_env* eval_env;
};

// parse a script / module / eval code / function body. On success returns
// 0 and P->top is the top-level function; on failure returns -1 with a
// SyntaxError pending (message + position in the exception).
enum { PARSE_SCRIPT, PARSE_MODULE, PARSE_EVAL, PARSE_FUNCTION };
struct parse_opts {
    int kind;
    int strict;
    const char* filename;
    // direct eval: what the enclosing code allows
    int allow_new_target, allow_super_prop, allow_super_call, allow_arguments, in_class_field;
    struct ojs_eval_env* eval_env;
    // new Function(): parameter text and body text are parsed separately
    int function_kind;          // 0 normal, 1 generator, 2 async, 3 async generator
};
int  parse_program(ojs* J, struct str* src, const struct parse_opts* o, struct parser* P);
// direct eval inside a class body may reference the class's private names
int  ojs_eval_env_has_private(struct ojs_eval_env* e, struct str* name);
void parse_free(struct parser* P);
struct decl* scope_find(struct scope* s, struct str* name);
void* arena_alloc(struct parser* P, size_t n);

#endif
