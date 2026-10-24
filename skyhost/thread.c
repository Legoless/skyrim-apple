#include "skyhost.h"
#include "thread.h"

#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400

extern void dm_gate(void);
static pthread_key_t g_keyown;
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;

static void own_free(void *pointer) {
    void **own = pointer;
    free(own[0]);
    free(own[1]);
    free(own);
}

static void own_key_init(void) {
    if (pthread_key_create(&g_keyown, own_free)) die("create guest ownership key");
}

void guest_thread_init(void) {
    pthread_once(&g_keyown_once, own_key_init);
    if (pthread_setspecific(g_keygate, (void *)dm_gate) || pthread_setspecific(g_key18, NULL))
        die("initialize guest gate/x18 slots");
    if (pthread_getspecific(g_keyown)) return;

    void **own = calloc(2, sizeof *own);
    uint8_t *tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
    if (!own) die("allocate guest ownership record");
    own[0] = calloc(1, TLS_SIZE);
    own[1] = tlr;
    if (!own[0] || !tlr) die("allocate guest TLS/TLR");
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;
    if (pthread_setspecific(g_keytls, own[0]) || pthread_setspecific(g_keytlr, tlr) ||
        pthread_setspecific(g_keyown, own))
        die("install guest TLS/TLR slots");
}
