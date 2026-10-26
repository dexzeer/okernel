// kernel32 (and the ntdll part of it): Windows x64 structured exception handling, all in user mode.
//
// A hardware fault of a Windows program is not turned into a signal: the kernel copies the registers to the user stack and
// continues at KiFaultEntry (registered with arch_prctl 0x1005, see src/lx.c win_exception). From the registers a CONTEXT and an
// EXCEPTION_RECORD are built, then RtlDispatchException walks the stack frame by frame with the unwind tables of the images
// (.pdata/.xdata; real Windows binaries have them, our own DLLs get them from the compiler's CFI, tools/pe/unwind.py) and asks each
// frame's language handler (for C: __C_specific_handler with the scope table of __try/__except/__finally) what to do. An
// __except that accepts the exception makes RtlUnwindEx run the termination handlers of the frames in between and continue in
// the __except block with the stack of its function. Software exceptions (RaiseException) take the same way.
#include "k32.h"
extern void ExitProcess(DWORD code);

typedef unsigned long long DWORD64;
typedef struct { DWORD64 Low; LONGLONG High; } __attribute__((aligned(16))) M128A;

typedef struct __attribute__((aligned(16))) {
    DWORD64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
    DWORD ContextFlags, MxCsr;
    WORD SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
    DWORD EFlags;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
    DWORD64 gp[16];                         // rax rcx rdx rbx rsp rbp rsi rdi r8..r15: the order of the unwind register numbers
    DWORD64 Rip;
    BYTE fltsave_hdr[0xA0];
    M128A xmm[16];
    BYTE fltsave_rest[96];
    M128A VectorRegister[26];
    DWORD64 VectorControl, DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
} CONTEXT;

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    void *ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS;
typedef struct { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION;

typedef struct {
    DWORD64 ControlPc, ImageBase;
    RUNTIME_FUNCTION *FunctionEntry;
    DWORD64 EstablisherFrame, TargetIp;
    CONTEXT *ContextRecord;
    void *LanguageHandler;
    void *HandlerData;
    void *HistoryTable;
    DWORD ScopeIndex, Fill0;
} DISPATCHER_CONTEXT;

typedef unsigned char BOOLEAN;
typedef int (*EXC_HANDLER)(EXCEPTION_RECORD *, void *frame, CONTEXT *, DISPATCHER_CONTEXT *);

#define EXCEPTION_NONCONTINUABLE 1
#define EXCEPTION_UNWINDING 2
#define EXCEPTION_EXIT_UNWIND 4
#define EXCEPTION_TARGET_UNWIND 0x20
#define UNW_FLAG_EHANDLER 1
#define UNW_FLAG_UHANDLER 2
#define UNW_FLAG_CHAININFO 4
#define STATUS_ACCESS_VIOLATION 0xC0000005u
#define STATUS_NONCONTINUABLE_EXCEPTION 0xC0000025u
#define STATUS_UNWIND 0xC0000027u
#define STATUS_INVALID_DISPOSITION 0xC0000026u

enum { ExceptionContinueExecution = 0, ExceptionContinueSearch = 1, ExceptionNestedException = 2, ExceptionCollidedUnwind = 3 };

void RtlCaptureContext(CONTEXT *ctx);
void RtlRestoreContext(CONTEXT *ctx, EXCEPTION_RECORD *rec);

// ---- the two assembler entry points ------------------------------------------------------------------------------------------

__asm__(
    ".text\n"
    ".globl RtlCaptureContext\n"
    "RtlCaptureContext:\n"
    "    pushfq\n"
    "    mov %rax, 0x78(%rcx)\n"
    "    mov %rcx, 0x80(%rcx)\n"
    "    mov %rdx, 0x88(%rcx)\n"
    "    mov %rbx, 0x90(%rcx)\n"
    "    lea 16(%rsp), %rax\n"
    "    mov %rax, 0x98(%rcx)\n"
    "    mov %rbp, 0xA0(%rcx)\n"
    "    mov %rsi, 0xA8(%rcx)\n"
    "    mov %rdi, 0xB0(%rcx)\n"
    "    mov %r8, 0xB8(%rcx)\n"
    "    mov %r9, 0xC0(%rcx)\n"
    "    mov %r10, 0xC8(%rcx)\n"
    "    mov %r11, 0xD0(%rcx)\n"
    "    mov %r12, 0xD8(%rcx)\n"
    "    mov %r13, 0xE0(%rcx)\n"
    "    mov %r14, 0xE8(%rcx)\n"
    "    mov %r15, 0xF0(%rcx)\n"
    "    mov 8(%rsp), %rax\n"
    "    mov %rax, 0xF8(%rcx)\n"
    "    mov (%rsp), %rax\n"
    "    mov %eax, 0x44(%rcx)\n"
    "    stmxcsr 0x34(%rcx)\n"
    "    movups %xmm0, 0x1A0(%rcx)\n"
    "    movups %xmm1, 0x1B0(%rcx)\n"
    "    movups %xmm2, 0x1C0(%rcx)\n"
    "    movups %xmm3, 0x1D0(%rcx)\n"
    "    movups %xmm4, 0x1E0(%rcx)\n"
    "    movups %xmm5, 0x1F0(%rcx)\n"
    "    movups %xmm6, 0x200(%rcx)\n"
    "    movups %xmm7, 0x210(%rcx)\n"
    "    movups %xmm8, 0x220(%rcx)\n"
    "    movups %xmm9, 0x230(%rcx)\n"
    "    movups %xmm10, 0x240(%rcx)\n"
    "    movups %xmm11, 0x250(%rcx)\n"
    "    movups %xmm12, 0x260(%rcx)\n"
    "    movups %xmm13, 0x270(%rcx)\n"
    "    movups %xmm14, 0x280(%rcx)\n"
    "    movups %xmm15, 0x290(%rcx)\n"
    "    movl $0x10000F, 0x30(%rcx)\n"
    "    movw $0x33, 0x38(%rcx)\n"
    "    movw $0x2B, 0x3A(%rcx)\n"
    "    movw $0x2B, 0x3C(%rcx)\n"
    "    movw $0x53, 0x3E(%rcx)\n"
    "    movw $0x2B, 0x40(%rcx)\n"
    "    movw $0x2B, 0x42(%rcx)\n"
    "    popfq\n"
    "    ret\n"
    ".globl RtlRestoreContext\n"
    "RtlRestoreContext:\n"
    "    movups 0x1A0(%rcx), %xmm0\n"
    "    movups 0x1B0(%rcx), %xmm1\n"
    "    movups 0x1C0(%rcx), %xmm2\n"
    "    movups 0x1D0(%rcx), %xmm3\n"
    "    movups 0x1E0(%rcx), %xmm4\n"
    "    movups 0x1F0(%rcx), %xmm5\n"
    "    movups 0x200(%rcx), %xmm6\n"
    "    movups 0x210(%rcx), %xmm7\n"
    "    movups 0x220(%rcx), %xmm8\n"
    "    movups 0x230(%rcx), %xmm9\n"
    "    movups 0x240(%rcx), %xmm10\n"
    "    movups 0x250(%rcx), %xmm11\n"
    "    movups 0x260(%rcx), %xmm12\n"
    "    movups 0x270(%rcx), %xmm13\n"
    "    movups 0x280(%rcx), %xmm14\n"
    "    movups 0x290(%rcx), %xmm15\n"
    "    ldmxcsr 0x34(%rcx)\n"
    "    mov 0x98(%rcx), %rsp\n"
    "    mov 0x44(%rcx), %eax\n"
    "    andl $0xFFFFFEFF, %eax\n"
    "    orl $0x200, %eax\n"
    "    mov 0xF8(%rcx), %r11\n"
    "    push %r11\n"
    "    push %rax\n"
    "    mov 0x78(%rcx), %rax\n"
    "    mov 0x88(%rcx), %rdx\n"
    "    mov 0x90(%rcx), %rbx\n"
    "    mov 0xA0(%rcx), %rbp\n"
    "    mov 0xA8(%rcx), %rsi\n"
    "    mov 0xB0(%rcx), %rdi\n"
    "    mov 0xB8(%rcx), %r8\n"
    "    mov 0xC0(%rcx), %r9\n"
    "    mov 0xC8(%rcx), %r10\n"
    "    mov 0xD0(%rcx), %r11\n"
    "    mov 0xD8(%rcx), %r12\n"
    "    mov 0xE0(%rcx), %r13\n"
    "    mov 0xE8(%rcx), %r14\n"
    "    mov 0xF0(%rcx), %r15\n"
    "    mov 0x80(%rcx), %rcx\n"
    "    popfq\n"
    "    ret\n"
    // the kernel continues a faulting Windows thread here: rcx = {struct lx_regs, cr2}
    ".globl KiFaultEntry\n"
    "KiFaultEntry:\n"
    "    and $-16, %rsp\n"
    "    sub $320, %rsp\n"
    "    lea 32(%rsp), %rdx\n"
    "    movups %xmm0, 0(%rdx)\n"
    "    movups %xmm1, 16(%rdx)\n"
    "    movups %xmm2, 32(%rdx)\n"
    "    movups %xmm3, 48(%rdx)\n"
    "    movups %xmm4, 64(%rdx)\n"
    "    movups %xmm5, 80(%rdx)\n"
    "    movups %xmm6, 96(%rdx)\n"
    "    movups %xmm7, 112(%rdx)\n"
    "    movups %xmm8, 128(%rdx)\n"
    "    movups %xmm9, 144(%rdx)\n"
    "    movups %xmm10, 160(%rdx)\n"
    "    movups %xmm11, 176(%rdx)\n"
    "    movups %xmm12, 192(%rdx)\n"
    "    movups %xmm13, 208(%rdx)\n"
    "    movups %xmm14, 224(%rdx)\n"
    "    movups %xmm15, 240(%rdx)\n"
    "    stmxcsr 256(%rdx)\n"
    "    call KiFaultC\n"
    "    ud2\n"
);

// ---- finding the unwind data of an address ------------------------------------------------------------------------------------

struct dyn_table { RUNTIME_FUNCTION *fn; DWORD count; DWORD64 base; };
static struct dyn_table dyn[16];

static DWORD64 module_of(DWORD64 pc) {
    static DWORD64 last_base, last_end;
    if (pc >= last_base && pc < last_end) return last_base;
    DWORD64 b = (DWORD64)SC1(0x1003, pc);
    if (!b) return 0;
    const BYTE *m = (const BYTE*)b;
    DWORD size = *(const DWORD*)(m + *(const DWORD*)(m + 0x3C) + 24 + 56);
    last_base = b; last_end = b + size;
    return b;
}

RUNTIME_FUNCTION *RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *hist);
EXPORT RUNTIME_FUNCTION *RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *hist) {
    (void)hist;
    for (int i = 0; i < 16; i++) {
        if (!dyn[i].fn) continue;
        for (DWORD k = 0; k < dyn[i].count; k++)
            if (pc >= dyn[i].base + dyn[i].fn[k].BeginAddress && pc < dyn[i].base + dyn[i].fn[k].EndAddress) { if (base) *base = dyn[i].base; return &dyn[i].fn[k]; }
    }
    DWORD64 b = module_of(pc);
    if (!b) { if (base) *base = 0; return 0; }
    const BYTE *m = (const BYTE*)b;
    const BYTE *opt = m + *(const DWORD*)(m + 0x3C) + 24;
    if (*(const WORD*)opt != 0x20B) return 0;
    const DWORD *dd = (const DWORD*)(opt + 112);
    DWORD rva = dd[6], size = dd[7];                           // exception directory (index 3)
    if (base) *base = b;
    if (!rva || !size) return 0;
    RUNTIME_FUNCTION *t = (RUNTIME_FUNCTION*)(m + rva);
    DWORD lo = 0, hi = size / 12;
    DWORD off = (DWORD)(pc - b);
    while (lo < hi) {
        DWORD mid = (lo + hi) / 2;
        if (off < t[mid].BeginAddress) hi = mid;
        else if (off >= t[mid].EndAddress) lo = mid + 1;
        else return &t[mid];
    }
    return 0;
}
EXPORT DWORD64 RtlPcToFileHeader(DWORD64 pc, DWORD64 *base) { DWORD64 b = module_of(pc); if (base) *base = b; return b; }
EXPORT BOOL RtlAddFunctionTable(RUNTIME_FUNCTION *fn, DWORD count, DWORD64 base) {
    for (int i = 0; i < 16; i++) if (!dyn[i].fn) { dyn[i].fn = fn; dyn[i].count = count; dyn[i].base = base; return TRUE; }
    return FALSE;
}
EXPORT BOOL RtlDeleteFunctionTable(RUNTIME_FUNCTION *fn) {
    for (int i = 0; i < 16; i++) if (dyn[i].fn == fn) { dyn[i].fn = 0; return TRUE; }
    return FALSE;
}

// ---- unwinding one frame -------------------------------------------------------------------------------------------------------

// UNWIND_INFO: byte 0 = version (3 bits) | flags << 3, 1 = prolog size, 2 = number of codes, 3 = frame register | offset << 4,
// then the codes (2 bytes each), then (if flagged) the handler RVA and its data, or the chained RUNTIME_FUNCTION.
#define UWOP_PUSH_NONVOL 0
#define UWOP_ALLOC_LARGE 1
#define UWOP_ALLOC_SMALL 2
#define UWOP_SET_FPREG 3
#define UWOP_SAVE_NONVOL 4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_EPILOG 6
#define UWOP_SAVE_XMM128 8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10

static int slots_of(int op, int info) {
    switch (op) {
    case UWOP_ALLOC_LARGE: return info ? 3 : 2;
    case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: return 2;
    case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: return 3;
    default: return 1;
    }
}

EXPORT void *RtlVirtualUnwind(DWORD type, DWORD64 base, DWORD64 pc, RUNTIME_FUNCTION *fe, CONTEXT *ctx, void **handler_data, DWORD64 *establisher, void *ctx_ptrs) {
    (void)ctx_ptrs;
    void *handler = 0;
    if (handler_data) *handler_data = 0;
    if (!fe) {                                                       // a leaf: nothing was pushed, the return address is on top
        if (establisher) *establisher = ctx->gp[4];
        ctx->Rip = *(DWORD64*)ctx->gp[4]; ctx->gp[4] += 8;
        return 0;
    }
    DWORD64 off = pc - (base + fe->BeginAddress);
    const BYTE *info = (const BYTE*)(base + fe->UnwindData);
    int first = 1;
    DWORD64 frame = 0;
    for (;;) {
        int flags = info[0] >> 3, prolog = info[1], count = info[2], fpreg = info[3] & 15, fpoff = (info[3] >> 4) * 16;
        const BYTE *codes = info + 4;
        if (first) {
            frame = ctx->gp[4];
            if (fpreg && off >= (DWORD64)prolog) frame = ctx->gp[fpreg] - fpoff;       // the frame register is set once the prolog is done
            else if (fpreg) {
                // inside the prolog: the frame register counts only if its SET_FPREG already ran
                for (int i = 0; i < count; ) { int op = codes[2 * i + 1] & 15, inf = codes[2 * i + 1] >> 4; if (op == UWOP_SET_FPREG && off >= codes[2 * i]) { frame = ctx->gp[fpreg] - fpoff; break; } i += slots_of(op, inf); }
            }
            if (establisher) *establisher = frame;
        }
        for (int i = 0; i < count; ) {
            int co = codes[2 * i], op = codes[2 * i + 1] & 15, inf = codes[2 * i + 1] >> 4, n = slots_of(op, inf);
            int apply = first ? off >= (DWORD64)co : 1;
            if (apply) {
                const WORD *w = (const WORD*)(codes + 2 * i + 2);
                switch (op) {
                case UWOP_PUSH_NONVOL: ctx->gp[inf] = *(DWORD64*)ctx->gp[4]; ctx->gp[4] += 8; break;
                case UWOP_ALLOC_LARGE: ctx->gp[4] += inf ? ((DWORD64)w[0] | ((DWORD64)w[1] << 16)) : (DWORD64)w[0] * 8; break;
                case UWOP_ALLOC_SMALL: ctx->gp[4] += (DWORD64)inf * 8 + 8; break;
                case UWOP_SET_FPREG: ctx->gp[4] = ctx->gp[fpreg] - fpoff; break;
                case UWOP_SAVE_NONVOL: ctx->gp[inf] = *(DWORD64*)(ctx->gp[4] + (DWORD64)w[0] * 8); break;
                case UWOP_SAVE_NONVOL_FAR: ctx->gp[inf] = *(DWORD64*)(ctx->gp[4] + ((DWORD64)w[0] | ((DWORD64)w[1] << 16))); break;
                case UWOP_SAVE_XMM128: ctx->xmm[inf] = *(M128A*)(ctx->gp[4] + (DWORD64)w[0] * 16); break;
                case UWOP_SAVE_XMM128_FAR: ctx->xmm[inf] = *(M128A*)(ctx->gp[4] + (((DWORD64)w[0] | ((DWORD64)w[1] << 16)) * 1)); break;
                case UWOP_PUSH_MACHFRAME: {
                    DWORD64 sp = ctx->gp[4];
                    if (inf) sp += 8;                                  // an error code
                    ctx->Rip = *(DWORD64*)sp; ctx->gp[4] = *(DWORD64*)(sp + 24);
                    return 0;
                }
                default: break;
                }
            }
            i += n;
        }
        const BYTE *after = codes + ((count + 1) & ~1) * 2;
        if (flags & UNW_FLAG_CHAININFO) {                            // the rest of the description is in another entry
            const RUNTIME_FUNCTION *ch = (const RUNTIME_FUNCTION*)after;
            info = (const BYTE*)(base + ch->UnwindData);
            first = 0;
            continue;
        }
        if (flags & type) {
            handler = (void*)(base + *(const DWORD*)after);
            if (handler_data) *handler_data = (void*)(after + 4);
        }
        break;
    }
    ctx->Rip = *(DWORD64*)ctx->gp[4]; ctx->gp[4] += 8;
    return handler;
}

// ---- dispatching ----------------------------------------------------------------------------------------------------------------

struct dstate { CONTEXT *orig; EXCEPTION_RECORD *rec; struct dstate *prev; };
static inline struct dstate **dstate_slot(void) { char *t = (char*)teb(); return (struct dstate**)(t + 0x28); }   // TEB.ArbitraryUserPointer

#define MAX_VEH 16
static EXC_HANDLER veh[MAX_VEH], vch[MAX_VEH];
EXPORT void *AddVectoredExceptionHandler(ULONG first, void *h) {
    (void)first;
    for (int i = 0; i < MAX_VEH; i++) if (!veh[i]) { veh[i] = (EXC_HANDLER)h; return h; }
    return 0;
}
EXPORT ULONG RemoveVectoredExceptionHandler(void *h) { for (int i = 0; i < MAX_VEH; i++) if (veh[i] == (EXC_HANDLER)h) { veh[i] = 0; return 1; } return 0; }
EXPORT void *AddVectoredContinueHandler(ULONG first, void *h) {
    (void)first;
    for (int i = 0; i < MAX_VEH; i++) if (!vch[i]) { vch[i] = (EXC_HANDLER)h; return h; }
    return 0;
}
EXPORT ULONG RemoveVectoredContinueHandler(void *h) { for (int i = 0; i < MAX_VEH; i++) if (vch[i] == (EXC_HANDLER)h) { vch[i] = 0; return 1; } return 0; }

static int stack_ok(DWORD64 sp) {
    char *t = (char*)teb();
    DWORD64 hi = *(DWORD64*)(t + 8), lo = *(DWORD64*)(t + 16);
    if (!hi) return sp != 0;
    return sp >= lo && sp < hi;
}

EXPORT BOOL RtlDispatchException(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
    struct dstate st = { ctx, rec, *dstate_slot() };
    *dstate_slot() = &st;
    BOOL result = FALSE;
    EXCEPTION_POINTERS ep = { rec, ctx };
    for (int i = 0; i < MAX_VEH; i++) {
        if (!veh[i]) continue;
        LONG r = ((LONG (*)(EXCEPTION_POINTERS*))veh[i])(&ep);
        if (r == -1) { result = TRUE; goto out; }
    }
    {
        CONTEXT uc = *ctx;
        CONTEXT frame_ctx;
        int depth = 0;
        for (;;) {
            if (uc.Rip == 0 || !stack_ok(uc.gp[4]) || ++depth > 4096) break;
            DWORD64 base = 0, frame = 0;
            RUNTIME_FUNCTION *fe = RtlLookupFunctionEntry(uc.Rip, &base, 0);
            frame_ctx = uc;
            void *hdata = 0;
            void *handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, uc.Rip, fe, &uc, &hdata, &frame, 0);
            if (handler) {
                DISPATCHER_CONTEXT dc;
                memset(&dc, 0, sizeof dc);
                dc.ControlPc = frame_ctx.Rip; dc.ImageBase = base; dc.FunctionEntry = fe; dc.EstablisherFrame = frame;
                dc.ContextRecord = depth == 1 ? ctx : &frame_ctx; dc.LanguageHandler = handler; dc.HandlerData = hdata;
                int disp = ((EXC_HANDLER)handler)(rec, (void*)frame, dc.ContextRecord, &dc);
                if (disp == ExceptionContinueExecution) {
                    if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) { result = FALSE; goto out; }
                    result = TRUE; goto out;
                }
                // ContinueSearch, and the nested/collided answers, go on with the caller
            }
        }
    }
out:
    if (result) for (int i = 0; i < MAX_VEH; i++) if (vch[i]) ((LONG (*)(EXCEPTION_POINTERS*))vch[i])(&ep);
    *dstate_slot() = st.prev;
    return result;
}

// Unwind to TargetFrame (0 = all the way): the termination handlers of the frames in between run, then execution continues at
// TargetIp with rax = ReturnValue. The walk starts at the context of the exception (not at the current stack, which holds the
// dispatcher and the handlers that are being abandoned).
EXPORT void RtlUnwindEx(void *target_frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval, CONTEXT *scratch, void *hist) {
    (void)hist;
    EXCEPTION_RECORD local;
    if (!rec) { memset(&local, 0, sizeof local); local.ExceptionCode = STATUS_UNWIND; rec = &local; }
    CONTEXT cur;
    struct dstate *st = *dstate_slot();
    if (st && st->orig) cur = *st->orig;
    else { RtlCaptureContext(&cur); }
    if (!st) {                                                    // not inside a dispatch: start where this call was made from
        DWORD64 base; RUNTIME_FUNCTION *fe = RtlLookupFunctionEntry(cur.Rip, &base, 0);
        RtlVirtualUnwind(0, base, cur.Rip, fe, &cur, 0, &(DWORD64){0}, 0);
    }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING | (target_frame ? 0 : EXCEPTION_EXIT_UNWIND);
    CONTEXT *uc = scratch ? scratch : &cur;
    if (uc != &cur) *uc = cur;
    int depth = 0;
    for (;;) {
        if (uc->Rip == 0 || !stack_ok(uc->gp[4]) || ++depth > 4096) break;
        DWORD64 base = 0, frame = 0;
        RUNTIME_FUNCTION *fe = RtlLookupFunctionEntry(uc->Rip, &base, 0);
        CONTEXT frame_ctx = *uc;
        void *hdata = 0;
        void *handler = RtlVirtualUnwind(UNW_FLAG_UHANDLER, base, uc->Rip, fe, uc, &hdata, &frame, 0);
        int at_target = target_frame && frame == (DWORD64)target_frame;
        if (handler) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof dc);
            dc.ControlPc = frame_ctx.Rip; dc.ImageBase = base; dc.FunctionEntry = fe; dc.EstablisherFrame = frame;
            dc.TargetIp = (DWORD64)target_ip; dc.ContextRecord = &frame_ctx; dc.LanguageHandler = handler; dc.HandlerData = hdata;
            DWORD saved = rec->ExceptionFlags;
            if (at_target) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
            ((EXC_HANDLER)handler)(rec, (void*)frame, &frame_ctx, &dc);
            rec->ExceptionFlags = saved;
        }
        if (at_target) {                                           // land in the function that owns the handler, with its own stack
            frame_ctx.Rip = (DWORD64)target_ip;
            frame_ctx.gp[0] = (DWORD64)retval;
            RtlRestoreContext(&frame_ctx, rec);
        }
    }
    if (!target_frame) { RtlRestoreContext(uc, rec); }
    // the target was never found: nothing sensible is left
    ExitProcess(STATUS_INVALID_DISPOSITION);
}
EXPORT void RtlUnwind(void *target_frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval) { RtlUnwindEx(target_frame, target_ip, rec, retval, 0, 0); }

// the language handler of C's __try / __except / __finally: HandlerData = {DWORD count; {Begin, End, Handler, Jump} x count} (RVAs)
struct scope { DWORD begin, end, handler, jump; };
EXPORT int __k32_c_specific_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
    const DWORD *tab = (const DWORD*)dc->HandlerData;
    DWORD count = tab[0];
    const struct scope *sc = (const struct scope*)(tab + 1);
    DWORD64 base = dc->ImageBase;
    DWORD rva = (DWORD)(dc->ControlPc - base);
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        for (DWORD i = 0; i < count; i++) {
            if (rva < sc[i].begin || rva >= sc[i].end) continue;
            if ((rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) && dc->TargetIp >= base + sc[i].begin && dc->TargetIp < base + sc[i].end) break;
            if (sc[i].jump == 0 && sc[i].handler > 1) ((void (*)(BOOLEAN, void*))(base + sc[i].handler))(1, frame);      // __finally
        }
        return ExceptionContinueSearch;
    }
    EXCEPTION_POINTERS ep = { rec, ctx };
    for (DWORD i = 0; i < count; i++) {
        if (rva < sc[i].begin || rva >= sc[i].end || sc[i].jump == 0) continue;
        LONG r = 1;
        if (sc[i].handler != 1) r = ((LONG (*)(EXCEPTION_POINTERS*, void*))(base + sc[i].handler))(&ep, frame);
        if (r < 0) return ExceptionContinueExecution;
        if (r > 0) RtlUnwindEx(frame, (void*)(base + sc[i].jump), rec, (void*)(ULONG_PTR)rec->ExceptionCode, dc->ContextRecord, dc->HistoryTable);
    }
    return ExceptionContinueSearch;
}

// ---- raising and the last resort ------------------------------------------------------------------------------------------------

typedef long (*TOPLEVEL)(EXCEPTION_POINTERS *ep);
static TOPLEVEL toplevel;
EXPORT TOPLEVEL SetUnhandledExceptionFilter(TOPLEVEL f) { TOPLEVEL o = toplevel; toplevel = f; return o; }

static void put_hex(char *b, int *n, DWORD64 v) {
    b[(*n)++] = '0'; b[(*n)++] = 'x';
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) { int d = (int)((v >> s) & 15); if (d || started || s == 0) { started = 1; b[(*n)++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } }
}
EXPORT long UnhandledExceptionFilter(EXCEPTION_POINTERS *ep) {
    if (toplevel) { long r = toplevel(ep); if (r != 0) return r; }
    char b[160]; int n = 0;
    const char *m = "unhandled exception ";
    while (*m) b[n++] = *m++;
    put_hex(b, &n, ep->ExceptionRecord->ExceptionCode);
    const char *a = " at "; while (*a) b[n++] = *a++;
    put_hex(b, &n, (DWORD64)ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {      // what it touched
        const char *w = ep->ExceptionRecord->ExceptionInformation[0] == 8 ? " (fetch " : ep->ExceptionRecord->ExceptionInformation[0] ? " (write " : " (read ";
        while (*w) b[n++] = *w++;
        put_hex(b, &n, ep->ExceptionRecord->ExceptionInformation[1]);
        b[n++] = ')';
    }
    b[n++] = 10;
    SC3(LX_write, 2, b, n);
    {                                                              // a short call trace, innermost first
        CONTEXT uc = *ep->ContextRecord;
        for (int depth = 0; depth < 10 && uc.Rip && stack_ok(uc.gp[4]); depth++) {
            DWORD64 base = 0, frame = 0; void *hd = 0;
            RUNTIME_FUNCTION *fe = RtlLookupFunctionEntry(uc.Rip, &base, 0);
            n = 0; b[n++] = ' '; b[n++] = ' '; put_hex(b, &n, uc.Rip); b[n++] = 10;
            SC3(LX_write, 2, b, n);
            RtlVirtualUnwind(0, base, uc.Rip, fe, &uc, &hd, &frame, 0);
        }
    }
    return 1;                                                      // EXCEPTION_EXECUTE_HANDLER: the process ends
}

static void dispatch_or_die(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
    if (RtlDispatchException(rec, ctx)) {
        RtlRestoreContext(ctx, rec);
    }
    EXCEPTION_POINTERS ep = { rec, ctx };
    long r = UnhandledExceptionFilter(&ep);
    if (r == -1) RtlRestoreContext(ctx, rec);
    ExitProcess(rec->ExceptionCode);
}

EXPORT void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) {
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = code; rec.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    if (n > 15) n = 15;
    rec.NumberParameters = n;
    for (DWORD i = 0; i < n; i++) rec.ExceptionInformation[i] = args[i];
    CONTEXT ctx;
    volatile int resumed = 0;
    RtlCaptureContext(&ctx);
    if (resumed) return;                                           // a handler continued the execution: back here
    resumed = 1;
    rec.ExceptionAddress = (void*)ctx.Rip;
    if (RtlDispatchException(&rec, &ctx)) {
        if (rec.ExceptionFlags & EXCEPTION_NONCONTINUABLE) { ULONG_PTR none = 0; RaiseException(STATUS_NONCONTINUABLE_EXCEPTION, EXCEPTION_NONCONTINUABLE, 0, &none); }
        RtlRestoreContext(&ctx, &rec);
    }
    EXCEPTION_POINTERS ep = { &rec, &ctx };
    long r = UnhandledExceptionFilter(&ep);
    if (r == -1) RtlRestoreContext(&ctx, &rec);
    ExitProcess(code);
}
EXPORT void RtlRaiseException(EXCEPTION_RECORD *rec) { RaiseException(rec->ExceptionCode, rec->ExceptionFlags, rec->NumberParameters, rec->ExceptionInformation); }

// A hardware fault (src/lx.c win_exception): turn the register copy into an exception and dispatch it.
struct fault_frame {
    DWORD64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    DWORD64 vec, err, rip, cs, rflags, rsp, ss, cr2, pad;
};

__attribute__((noreturn)) EXPORT void KiFaultC(struct fault_frame *f, BYTE *xmm_area) {
    CONTEXT ctx;
    EXCEPTION_RECORD rec;
    memset(&ctx, 0, sizeof ctx);
    memset(&rec, 0, sizeof rec);
    ctx.gp[0] = f->rax; ctx.gp[1] = f->rcx; ctx.gp[2] = f->rdx; ctx.gp[3] = f->rbx; ctx.gp[4] = f->rsp; ctx.gp[5] = f->rbp;
    ctx.gp[6] = f->rsi; ctx.gp[7] = f->rdi; ctx.gp[8] = f->r8; ctx.gp[9] = f->r9; ctx.gp[10] = f->r10; ctx.gp[11] = f->r11;
    ctx.gp[12] = f->r12; ctx.gp[13] = f->r13; ctx.gp[14] = f->r14; ctx.gp[15] = f->r15;
    ctx.Rip = f->rip; ctx.EFlags = (DWORD)f->rflags; ctx.ContextFlags = 0x10000F;
    ctx.SegCs = 0x33; ctx.SegSs = 0x2B; ctx.SegDs = ctx.SegEs = ctx.SegGs = 0x2B; ctx.SegFs = 0x53;
    memcpy(ctx.xmm, xmm_area, 256);
    ctx.MxCsr = *(DWORD*)(xmm_area + 256);
    rec.ExceptionAddress = (void*)f->rip;
    switch (f->vec) {
    case 14:
        rec.ExceptionCode = STATUS_ACCESS_VIOLATION; rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (f->err & 16) ? 8 : (f->err & 2) ? 1 : 0;           // fetch / write / read
        rec.ExceptionInformation[1] = f->cr2;
        break;
    case 13: rec.ExceptionCode = STATUS_ACCESS_VIOLATION; rec.NumberParameters = 2; rec.ExceptionInformation[1] = ~0ull; break;
    case 0:  rec.ExceptionCode = 0xC0000094u; break;                                      // integer divide by zero
    case 4:  rec.ExceptionCode = 0xC0000095u; break;                                      // integer overflow
    case 5:  rec.ExceptionCode = 0xC000008Cu; break;                                      // array bounds
    case 3:  rec.ExceptionCode = 0x80000003u; rec.ExceptionAddress = (void*)(f->rip - 1); break;
    case 1:  rec.ExceptionCode = 0x80000004u; break;
    case 16: case 19: rec.ExceptionCode = 0xC0000090u; break;                             // floating point
    case 17: rec.ExceptionCode = 0x80000002u; break;                                      // datatype misalignment
    default: rec.ExceptionCode = 0xC000001Du; break;                                      // illegal instruction
    }
    dispatch_or_die(&rec, &ctx);
    for (;;) SC1(LX_exit_group, 3);
}

