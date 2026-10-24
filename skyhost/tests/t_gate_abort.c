#include "skyhost.h"

int main(void) {
    uint64_t regs[31] = {0};
    gate_register(0x4000, "svc", 0x7f, 0x1234);
    gate_dispatch(0x4000, regs);
    return 0;
}
