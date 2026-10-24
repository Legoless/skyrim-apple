#include "skyhost.h"

enum { G_SVC, G_CNTPCT, G_CNTVCT };
typedef struct { uint64_t return_address, original; int kind, argument; } gate_t;

#define GATE_BITS 12
#define GATE_SLOTS (1u << GATE_BITS)
#define GATE_MASK (GATE_SLOTS - 1)
static gate_t g_gate[GATE_SLOTS];
static unsigned g_ngate;

static unsigned gate_hash(uint64_t address) {
    return (unsigned)(((address >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS));
}

void gate_register(uint64_t return_address, const char *kind, int argument, uint64_t original) {
    int gate_kind = !strcmp(kind, "svc") ? G_SVC
                  : !strcmp(kind, "cntpct") ? G_CNTPCT
                  : !strcmp(kind, "cntvct") ? G_CNTVCT : -1;
    if (gate_kind < 0) die("unsupported gate kind %s at %#llx", kind, (unsigned long long)return_address);
    if (!return_address) die("gate with null return address");
    if (gate_kind == G_SVC && (argument < 0 || argument > 0xFFFF)) die("invalid SVC number %#x", argument);
    if (gate_kind != G_SVC && (argument < 0 || argument > 30)) die("invalid counter destination x%d", argument);
    if (++g_ngate > GATE_SLOTS / 4) die("gate table overflow");
    unsigned index = gate_hash(return_address);
    while (g_gate[index].return_address) {
        if (g_gate[index].return_address == return_address)
            die("duplicate gate at %#llx", (unsigned long long)return_address);
        index = (index + 1) & GATE_MASK;
    }
    g_gate[index] = (gate_t){return_address, original, gate_kind, argument};
}

__attribute__((noreturn)) void sky_svc_unimplemented(int number, uint64_t original) {
    LOG("UNIMPL SVC %#x orig=%#llx checkpoint=%s handler=sky_svc_unimplemented", number,
        (unsigned long long)original, sky_checkpoint_name());
    LOG("RUN_STOP checkpoint=%s reason=UNIMPL_SVC_%#x", sky_checkpoint_name(), number);
    abort();
}

uint64_t gate_dispatch(uint64_t return_address, uint64_t *regs) {
    for (unsigned index = gate_hash(return_address);; index = (index + 1) & GATE_MASK) {
        const gate_t *gate = &g_gate[index];
        if (gate->return_address == return_address) {
            sky_gate_context(regs);
            if (gate->kind == G_SVC) return svc_dispatch(gate->argument, regs, gate->original);
            uint64_t value = now_ns() * 12 / 625;
            regs[gate->argument] = value;
            return value;
        }
        if (!gate->return_address) die("unknown gate at %#llx", (unsigned long long)return_address);
    }
}
