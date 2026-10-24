#include "bcat.h"
#include "skyhost.h"
#include <assert.h>

int main(void) {
    sky_bcat_initialize();
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_bcat_initialize"));
    puts("T_BCAT OK offline-init-no-download");
    return 0;
}
