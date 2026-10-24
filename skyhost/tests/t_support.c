#include "skyhost.h"
#include <assert.h>
#include <unistd.h>

static void *checkpoint_thread(void *context) {
    (void)context;
    for (unsigned index = 0; index < 10000; index++) sky_post_entry_service(0xb);
    return NULL;
}

int main(void) {
    FILE *capture = tmpfile();
    assert(capture);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
    sky_post_entry_service(4);
    assert(!strcmp(sky_checkpoint_name(), "map-only"));
    sky_post_entry_enable();
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, checkpoint_thread, NULL));
    for (unsigned index = 0; index < 10000; index++) sky_post_entry_service(0x2c);
    assert(!pthread_join(worker, NULL));
    sky_post_entry_service(0xb);
    assert(!strcmp(sky_checkpoint_name(), "post_nnMain_svc_0xb"));
    assert(sky_ui_is_in_game() == 0);
    sky_set_ui_in_game(1);
    assert(sky_ui_is_in_game() == 1);
    sky_set_ui_in_game(2);
    assert(sky_ui_is_in_game() == 2);
    sky_set_ui_in_game(0);
    assert(sky_ui_is_in_game() == 0);
    fflush(stderr);
    assert(ftell(capture) < 10000); /* Logs stay bounded during polling. */
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    fclose(capture);
    puts("T_SUPPORT OK concurrent-service-checkpoints bounded-diagnostics");
    return 0;
}
