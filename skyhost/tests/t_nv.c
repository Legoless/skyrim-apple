#include "nv.h"
#include "skyhost.h"
#include <assert.h>

int main(void) {
    sky_nv_initialize_graphics(NULL, 0);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_nv_initialize"));
    puts("T_NV OK offscreen-initialize");
    return 0;
}
