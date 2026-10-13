// dmccl -- the "dex mega cool coding language(tm)", C edition.
//
// A small scripting language made for game levels. The same sources build in the kernel (freestanding) and on the host
// (tools/dmccl_cli.c, tests/dmccl). The language is documented in src/dmccl/README.md.
//
//   when run do
//       world:spawn("guy", 60, 300)
//       child.abort then suicide
//   endifchain
//
// How it fits together:
//   dm_compile()   source text  -> dm_program   (lexer + one-pass compiler to a compact bytecode)
//   dm_vm_new()    dm_program   -> dm_vm        (the running world: globals, entity fields, handler threads)
//   dm_register()  gives the VM natives (module:function), dm_host gives it eyes and hands in the game
//   dm_step()      lets time pass: wakes sleepers, fires `tick`/`every`, runs threads under an instruction budget
//
// Nothing in here can hang the machine: every thread runs a bounded slice per step, then yields.
#ifndef DM_H
#define DM_H

#include <stddef.h>
#include <stdint.h>

typedef struct dm_vm dm_vm;
typedef struct dm_program dm_program;

typedef enum { DM_NIL, DM_NUM, DM_BOOL, DM_STR, DM_ENT } dm_type;

struct dm_str;   // immutable text owned by the VM (never free it yourself)

typedef struct dm_val {
    dm_type t;
    union { double n; int b; int ent; struct dm_str* s; } u;
} dm_val;

static inline dm_val dm_nil(void)         { dm_val v; v.t = DM_NIL;  v.u.n = 0;   return v; }
static inline dm_val dm_num(double n)     { dm_val v; v.t = DM_NUM;  v.u.n = n;   return v; }
static inline dm_val dm_bool(int b)       { dm_val v; v.t = DM_BOOL; v.u.n = 0; v.u.b = b != 0; return v; }
static inline dm_val dm_ent(int ent)      { dm_val v; v.t = DM_ENT;  v.u.n = 0; v.u.ent = ent; return v; }

// A string value (copied into the VM). NULL text gives nil.
dm_val      dm_str_new(dm_vm* vm, const char* text);
const char* dm_text(const dm_val* v);          // the characters of a DM_STR value ("" for anything else)

// ---- compiling -----------------------------------------------------------------------------------------------------

// Compile `len` bytes of source. On failure returns NULL and writes "line N: what went wrong" into err.
dm_program* dm_compile(const char* src, size_t len, char* err, size_t err_cap);
void        dm_program_free(dm_program* p);

// Facts about a compiled program (what the engine needs before it starts the game).
int         dm_thing_count(const dm_program* p);
const char* dm_thing_name(const dm_program* p, int i);
const char* dm_thing_base(const dm_program* p, const char* thing);   // the `is "ball"` kind, NULL if no such thing

// ---- the host -------------------------------------------------------------------------------------------------------

// Callbacks that let scripts reach the game. Every one is optional. The get/set/act/ctx ones return 1 when they know the
// name and 0 when they do not (the VM then falls back to its own fields, or reports an error).
struct dm_host {
    void* user;
    void (*print)(void* user, const char* line);                                  // log:print
    int  (*get)(dm_vm*, int ent, const char* name, dm_val* out);                  // guy.x
    int  (*set)(dm_vm*, int ent, const char* name, dm_val v);                     // guy.x = 5
    int  (*act)(dm_vm*, int ent, const char* verb, int argc, const dm_val* argv, dm_val* ret);   // child.abort / guy.hurt(5)
    int  (*ctx)(dm_vm*, int me, const char* name, dm_val* out);                   // child, parent (me/who/hit are the VM's)
    int  (*alive)(dm_vm*, int ent);                                               // truthiness of an entity value
    void (*suicide)(dm_vm*, int ent);                                             // `suicide`; ent < 0: the level itself
};

typedef int (*dm_native)(dm_vm* vm, int argc, const dm_val* argv, dm_val* ret, void* ctx);   // 0 = ok, -1 = dm_fail() was called

dm_vm* dm_vm_new(dm_program* prog, const struct dm_host* host);
void   dm_vm_free(dm_vm* vm);

void   dm_register(dm_vm* vm, const char* module, const char* name, dm_native fn, void* ctx);   // world:spawn -> ("world","spawn")
int    dm_link(dm_vm* vm, char* err, size_t err_cap);   // checks imports and that every called function exists; 0 = ok
int    dm_fail(dm_vm* vm, const char* msg);             // from a native: report an error (returns -1 for convenience)
void   dm_register_basics(dm_vm* vm);           // log:print, math(), floor/abs/min/max/sqrt/sin/cos/random/str

// ---- running ---------------------------------------------------------------------------------------------------------

void   dm_start(dm_vm* vm);                             // fires `when run` for the level (after dm_link)
int    dm_spawn_thing(dm_vm* vm, const char* thing, int ent);   // attach a `thing`'s handlers to entity `ent`; 0 if unknown
void   dm_step(dm_vm* vm, int dt_ms);                   // time passes (dt_ms of game time), scripts run
void   dm_ent_gone(dm_vm* vm, int ent);                 // the entity is dead: its threads, handlers and fields go away
int    dm_idle(const dm_vm* vm);                        // nothing running, nothing that can start by itself
int    dm_me(const dm_vm* vm);                          // from a native: the entity whose script is running (-1: the level)
int    dm_running(const dm_vm* vm);                     // 0 once the level halted itself (`suicide` at level scope)

// Events from the game.
void   dm_fire_touch(dm_vm* vm, int a, const char* a_tag, int b, const char* b_tag);   // `when A touches B`
void   dm_fire_dies(dm_vm* vm, int ent, const char* tag);                               // `when A dies`
void   dm_fire_key(dm_vm* vm, const char* key);                                         // `when key "space"`

// Variables of the level (for the host and for tests).
dm_val dm_global_get(dm_vm* vm, const char* name);
void   dm_global_set(dm_vm* vm, const char* name, dm_val v);

// Number to text the way scripts print it (42 -> "42", 0.5 -> "0.5"). out needs 32 bytes.
void   dm_fmt_num(double n, char* out);

int    dm_thread_count(const dm_vm* vm);                // live threads (tests: "did everything finish")

#endif
