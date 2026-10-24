#include "profile.h"
#include "runtime.h"
#include "skyhost.h"
#include "macos_window.h"
#include "input_control.h"

extern void dm_gate(void);

static int usage(const char *program) {
    fprintf(stderr, "usage: %s --map-only <rewrite-dir> | --run <rewrite-dir>\n", program);
    return 2;
}

static void quit_from_input(void *context) {
    (void)context;
    exit(0);
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "--map-only") && strcmp(argv[1], "--run"))) return usage(argv[0]);
    LOG("PROFILE name=%s title_id=%s modules=%s,%s", sky_profile.name, sky_profile.title_id,
        sky_profile.modules[0], sky_profile.modules[1]);
    load_modules(argv[2], sky_profile.modules, (int)sky_profile.module_count);
    if (!strcmp(argv[1], "--run")) {
        if (sky_input_control_start(quit_from_input, NULL) < 0) die("start stdin controller");
        const char *headless = getenv("SKY_HEADLESS");
        return headless && !strcmp(headless, "1") ? runtime_run() : sky_macos_run(runtime_run);
    }
    if (pthread_setspecific(g_keygate, (void *)dm_gate)) die("install gate entry in current thread");
    loader_hash_log();
    unsigned gates = 0;
    for (int index = 0; index < g_nmod; index++) gates += g_mod[index].gates;
    LOG("MAP_ONLY_OK modules=%s,%s gates=%u", sky_profile.modules[0], sky_profile.modules[1], gates);
    return 0;
}
