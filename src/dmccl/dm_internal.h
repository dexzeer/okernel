// dmccl internals: tokens, bytecode, the compiled program and the VM. Not for the engine's eyes (that is dm.h).
#ifndef DM_INTERNAL_H
#define DM_INTERNAL_H

#include "dm.h"
#include "dm_sys.h"

// ---- tokens ---------------------------------------------------------------------------------------------------------

enum dm_tok {
    T_EOF, T_NL, T_NUM, T_STR, T_ID,
    // words that mean something everywhere
    T_WHEN, T_DO, T_ENDIFCHAIN, T_FROM, T_IMPORT, T_IF, T_ELSEIF, T_ELSE, T_THEN, T_SUICIDE, T_NOT, T_WAITFOR, T_ABOVE,
    T_CHECKFOR, T_REPEAT, T_AS, T_FOREVER, T_STOP, T_THING, T_IS, T_AND, T_OR,
    // operators
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT,
    T_LPAREN, T_RPAREN, T_COMMA, T_COLON, T_DOT,
};

struct dm_token {
    uint8_t kind;
    uint16_t line;
    double num;              // T_NUM
    const char* text;        // T_ID / T_STR (NUL-terminated, owned by dm_tokens)
};

struct dm_tokens {
    struct dm_token* t;
    int n, cap;
    char* pool;              // identifier and string text
    size_t pool_n, pool_cap;
};

// Lexes `src`; returns 0 and fills the list, or -1 with a message in err.
int  dm_lex(const char* src, size_t len, struct dm_tokens* out, char* err, size_t err_cap);
void dm_tokens_free(struct dm_tokens* t);

// ---- bytecode -------------------------------------------------------------------------------------------------------

enum dm_op {
    OP_NIL, OP_TRUE, OP_FALSE, OP_NUM, OP_STR,          // push a constant (a = constant index)
    OP_POP, OP_DUP,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_AND, OP_OR, OP_NOT,
    OP_LOADG, OP_STOREG,                                // global variable a
    OP_LOADL, OP_STOREL,                                // loop variable a of this thread
    OP_CTX,                                             // me / who / hit / child / parent / time (a = DM_CTX_*)
    OP_GETM,                                            // [obj] -> [value]       name = string constant a
    OP_SETM,                                            // [obj, value] -> []
    OP_CALLM,                                           // [obj, args...] -> [result]   name = string constant a, n args
    OP_CALL,                                            // [args...] -> [result]        call table entry a, n args
    OP_JMP,                                             // ip += a (relative, so copied code still works)
    OP_JMPF,                                            // pop; if falsy ip += a
    OP_WAIT,                                            // pop seconds; sleep this thread
    OP_DEADLINE,                                        // pop seconds; push the absolute deadline (ms)
    OP_CHECK,                                           // [deadline, cond]: true -> ip += a (done); late -> fall to ip+2 (body);
                                                        //   else pop cond, sleep one step and continue at ip+1 (a JMP back)
    OP_REP_TEST,                                        // [limit, i]: i >= limit -> ip += a
    OP_REP_NEXT,                                        // i += 1, ip += a (back to the test)
    OP_REP_VAR,                                         // local a = i
    OP_SUICIDE, OP_END, OP_YIELD,
};

enum { DM_CTX_ME, DM_CTX_WHO, DM_CTX_HIT, DM_CTX_CHILD, DM_CTX_PARENT, DM_CTX_TIME, DM_CTX_COUNT };

struct dm_ins {
    uint8_t op;
    uint8_t n;               // argument count for calls
    uint16_t line;
    int32_t a;
};

// ---- the compiled program -------------------------------------------------------------------------------------------

enum { EV_RUN, EV_TICK, EV_EVERY, EV_KEY, EV_TOUCH, EV_DIES };
enum { PAT_NONE, PAT_VAR, PAT_TAG, PAT_ME };     // who a handler is about: nobody, a variable's entity, a tag name, the owner

struct dm_handler {
    uint8_t kind;
    uint8_t a_kind, b_kind;
    int a_arg, b_arg;        // PAT_VAR: global slot; PAT_TAG: string constant
    double every_ms;         // EV_EVERY
    int key;                 // EV_KEY: string constant
    int thing;               // -1 for the level
    int entry;               // first instruction
    int line;
};

struct dm_thing {
    char name[32];
    char base[32];
    int first, count;        // its handlers: [first, first + count)
};

struct dm_call {
    char module[24];         // "" for an unqualified call
    char name[32];
    int line;
};

#define DM_MAX_LOCALS 8

struct dm_program {
    struct dm_ins* code;     int ncode, capcode;
    double* nums;            int nnum, capnum;
    char** strs;             int nstr, capstr;        // string constants, deduplicated (so equal text = equal pointer)
    char (*globals)[32];     int nglob, capglob;
    struct dm_call* calls;   int ncalls, capcalls;
    char (*imports)[24];     int nimports, capimports;
    struct dm_handler* hs;   int nhs, caphs;
    struct dm_thing* things; int nthings, capthings;
};

// ---- the VM ---------------------------------------------------------------------------------------------------------

#define DM_STACK 40
#define DM_SLICE 6000            // instructions one thread may run before it must yield
#define DM_STEP_BUDGET 120000    // instructions all threads together may run in one dm_step
#define DM_MAX_THREADS 96
#define DM_MAX_INSTS 256
#define DM_MAX_TIMERS 128
#define DM_MAX_FIELDS 768

struct dm_str {
    struct dm_str* next;
    uint32_t len;
    uint8_t mark;
    uint8_t pinned;          // program constants
    char text[1];
};

struct dm_thread {
    uint8_t used;
    int handler;             // which handler started it (-1: none)
    int inst;                // thing instance it belongs to (-1: the level)
    int me, who, hit;        // entity ids (-1: none)
    int ip, sp;
    double wake_at;          // asleep until the VM clock reaches this
    dm_val st[DM_STACK];
    dm_val loc[DM_MAX_LOCALS];
};

struct dm_inst {             // a `thing` attached to an entity
    uint8_t used;
    int ent, thing;
};

struct dm_timer {            // an `every N` handler waiting for its next time
    uint8_t used;
    int handler, inst;
    double next_ms;
};

struct dm_field {            // `guy.coins = 3` for names the game does not know: kept here, per entity
    uint8_t used;
    int ent;
    const char* name;        // a program string constant (pointer compared)
    dm_val v;
};

struct dm_native_entry {
    char module[24], name[32];
    dm_native fn;
    void* ctx;
};

struct dm_vm {
    dm_program* prog;
    struct dm_host host;
    double now_ms;
    int halted;
    int started;
    char err[96];            // last dm_fail() message
    dm_val* globals;
    dm_native* call_fn;      // resolved: call table index -> native
    void** call_ctx;
    struct dm_native_entry* natives; int nnatives, capnatives;
    struct dm_thread* th;    // DM_MAX_THREADS
    struct dm_inst* inst;    // DM_MAX_INSTS
    struct dm_timer* timers; // DM_MAX_TIMERS
    struct dm_field* fields; // DM_MAX_FIELDS
    struct dm_str* strs;     // every live string
    size_t str_bytes, gc_at;
    int step_left;           // instruction budget of the running dm_step
    int cur;                 // thread being run (-1)
    uint32_t rng;            // random() state
    dm_val* str_const;       // program string constants as values
    uint8_t* tick_busy;      // per handler+inst: a `tick` thread is still running (indexed by handler)
};

// compiler entry used by dm_compile
int dm_compile_tokens(struct dm_tokens* toks, dm_program* out, char* err, size_t err_cap);

// utilities shared by the compiler and the VM
int dm_str_eq(const char* a, const char* b);

#endif
