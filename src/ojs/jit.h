// jit.h — the baseline compiler: bytecode -> i386 machine code (jit.c).
//
// A function that has been called often enough (or whose loop has run often enough) is translated instruction by instruction into native
// code that works on the SAME frame the interpreter uses: locals and operand stack stay in memory (so the collector, closures'
// upvalues, `arguments` and the debugger-ish parts of the VM see nothing new), the stack depth of every instruction is known statically
// (jit.c checks it while it analyses the function) so the code addresses the slots directly, and everything the compiler does not
// translate itself runs in vm_step, the interpreter body compiled to execute exactly one instruction. A guard that fails (a double
// where an int was expected, an inline cache miss) takes the same road: the interpreter re-executes the instruction from scratch.
#ifndef OJS_JIT_H
#define OJS_JIT_H

#include "vm.h"

struct jitcode {
    uint8_t* mem;               // the executable block
    size_t size;
    void* entry;                // jv entry(ojs* J, struct ojs_frame* f, void* target): runs the frame from `target` to the end
    void* start;                // native address of the first instruction
    uint32_t n;                 // instructions with native code
    uint32_t* pcs;              // their bytecode offsets (ascending)
    void** addr;                // and native addresses
};

// 0: off, 1: tiered (hot functions), 2: everything at its first call (tests)
extern int ojs_jit_mode;
extern uint32_t ojs_jit_threshold;      // calls (or loop iterations) before a function is compiled in mode 1

int   jit_compile(ojs* J, struct ftempl* t);       // 0: t->jit is set; -1: not compiled (unsupported instructions, no memory, ...)
void  jit_free(struct jitcode* c);
void* jit_native_at(const struct jitcode* c, uint32_t pc_off);     // 0: no code for that instruction

// statically computed operand stack depth before the instruction at pc_off (-1: unknown / unreachable); the interpreter checks it in
// builds with OJS_JIT_CHECK. Returns -2 if the function cannot be analysed.
int   jit_depth_at(struct ftempl* t, uint32_t pc_off);

// statistics
struct jit_stats { uint32_t compiled, rejected; uint64_t code_bytes, instructions, native, stepped; };
extern struct jit_stats ojs_jit_stats;

// called by compiled code (vm.c)
void* jit_step(ojs* J, struct ojs_frame* f, void* resume, const uint8_t* resume_pc, jv* res, int mode);

#endif
