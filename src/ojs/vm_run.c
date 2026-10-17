// vm_run.c — the bytecode interpreter (included by vm.c).

#include "vm_helpers.c"

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static jv tdz_error(ojs* J, jv name) {
    if (jv_is_str(name)) return throw_ref(J, "Cannot access '%S' before initialization", str_flat(J, name));
    return throw_ref(J, "Cannot access binding before initialization");
}

static int interrupt_check(ojs* J) {
    if (!J->interrupt) return 0;
    J->interrupt_counter = 4096;
    if (J->interrupt(J, J->interrupt_op)) {
        throw_error(J, NE_RANGE, "interrupted");
        J->uncatchable = 1;
        return -1;
    }
    return 0;
}

// unscopables: is `name` blocked on with object o?
static int with_has(ojs* J, struct obj* o, pkey k) {
    int h = obj_has(J, o, k);
    if (h <= 0) return h;
    if (o->flags & OF_EVALVARS) return 1;
    jv u = obj_get(J, o, pk_from_sym(J->wk[WK_UNSCOPABLES]), jv_from_obj(o));
    if (u == JV_EXC) return -1;
    if (jv_is_obj(u)) {
        jv b = obj_get(J, jv_obj(u), k, u);
        if (b == JV_EXC) return -1;
        if (to_boolean(b)) return 0;
    }
    return 1;
}

#ifdef OJS_OPSTATS
unsigned long long ojs_opstat[256];
__attribute__((destructor)) static void opstat_dump(void) {
    static const char* const names[] = {
#define OPN(name, fmt) #name,
        OPS(OPN)
#undef OPN
    };
    unsigned long long tot = 0;
    for (int i = 0; i < OP__COUNT; i++) tot += ojs_opstat[i];
    fprintf(stderr, "[opstats] %llu instructions\n", tot);
    for (int pass = 0; pass < 40; pass++) {
        int best = -1;
        for (int i = 0; i < OP__COUNT; i++) if (ojs_opstat[i] && (best < 0 || ojs_opstat[i] > ojs_opstat[best])) best = i;
        if (best < 0) break;
        fprintf(stderr, "[opstats] %-18s %12llu %5.1f%%\n", names[best], ojs_opstat[best], 100.0 * (double)ojs_opstat[best] / (double)tot);
        ojs_opstat[best] = 0;
    }
}
#endif


// The interpreter body is compiled twice: vm_run (a whole function activation) and vm_step (exactly one instruction, for compiled code:
// jit.c leaves everything it does not compile itself to it).
#define VM_STEP 0
#include "vm_body.c"
#undef VM_STEP
#define VM_STEP 1
#include "vm_body.c"
#undef VM_STEP
