#include "nfp.h"
#include "skyhost.h"
#include <assert.h>

int main(void) {
    assert(sky_nfp_initialize() == 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nfp_initialize"));
    uint64_t event[4], original[4];
    memset(event, 0xa5, sizeof event);
    memcpy(original, event, sizeof event);
    assert(sky_nfp_attach_availability_change_event(event) == 0x2A2u);
    assert(!memcmp(event, original, sizeof event));
    puts("T_NFP OK offline-init-no-devices availability-failure");
    return 0;
}
