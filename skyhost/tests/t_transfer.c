#include "skyhost.h"
#include "svc.h"
#include <assert.h>
#include <sys/mman.h>

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

int main(void) {
    size_t size = 0x4000;
    uint8_t *source = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(source != MAP_FAILED);
    memset(source, 0x5a, size);
    uint32_t handle = 0;
    uint64_t regs[31] = {0};
    regs[0] = (uint64_t)&handle;
    regs[1] = (uint64_t)source;
    regs[2] = size;
    regs[3] = 3;
    svc_dispatch(0x15, regs, 0);
    assert(regs[0] == 0 && handle >= 0x10000);

    uint64_t target = 0;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&target;
    regs[1] = handle;
    regs[2] = size;
    regs[3] = 3;
    svc_dispatch(0x13, regs, 0);
    assert(regs[0] == 0 && target && target != (uint64_t)source);
    assert(!(target & 0x3fff));
    source[0x1234] = 0x6b;
    assert(*(uint8_t *)(target + 0x1234) == 0x6b);
    *(uint8_t *)(target + 0x1234) = 0x7c;
    assert(source[0x1234] == 0x7c);

    uint8_t info[40] = {0};
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)info;
    regs[2] = target;
    svc_dispatch(0x06, regs, 0);
    assert(regs[0] == 0);
    assert(*(uint64_t *)info == target);
    assert(*(uint64_t *)(info + 8) == size);
    assert(*(uint32_t *)(info + 0x10) == 0xF);

    memset(regs, 0, sizeof regs);
    regs[0] = handle;
    regs[1] = target;
    regs[2] = size;
    svc_dispatch(0x14, regs, 0);
    assert(regs[0] == 0);
    memset(info, 0, sizeof info);
    regs[0] = (uint64_t)info;
    regs[2] = target;
    svc_dispatch(0x06, regs, 0);
    assert(*(uint32_t *)(info + 0x10) == 0);

    memset(regs, 0, sizeof regs);
    regs[0] = handle;
    svc_dispatch(0x16, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = handle;
    svc_dispatch(0x16, regs, 0);
    assert(regs[0] == 0xE401);

    handle = 0;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&handle;
    regs[1] = (uint64_t)(source + 1);
    regs[2] = size;
    regs[3] = 3;
    svc_dispatch(0x15, regs, 0);
    assert(regs[0] == 0xD401);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&target;
    regs[1] = 0xFFFFFFFF;
    regs[2] = size;
    regs[3] = 3;
    svc_dispatch(0x13, regs, 0);
    assert(regs[0] == 0xD401);
    munmap(source, size);
    puts("T_TRANSFER OK create-map-shared-unmap-close alignment-permission");
    return 0;
}
