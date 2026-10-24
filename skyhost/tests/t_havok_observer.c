#include "../runtime.c"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

void *module_sym(module_t *module, const char *name) { (void)module; (void)name; return NULL; }
module_t *module_of(const void *pointer) { (void)pointer; return NULL; }

static void clear_trace(void) {
    for (unsigned index = 0; index < HAVOK_TRACE_CAPACITY; index++) {
        atomic_store(&g_havok_trace[index].busy, 0);
        atomic_store(&g_havok_trace[index].completed, 0);
    }
    atomic_store(&g_havok_trace_issued, 0);
    atomic_store(&g_havok_trace_dropped, 0);
}

static _Atomic unsigned g_finished;
static void *concurrent_trace(void *argument) {
    unsigned marker = (unsigned)(uintptr_t)argument;
    uint8_t system[0x2300] = {0};
    uint32_t router[0x80 / 4] = {0};
    router[0x70 / 4] = marker;
    system[0x738 + marker * 0x1d0] = 1;
    for (unsigned index = 0; index < 1024; index++)
        trace_havok_buffer(TRACE_INIT_EXIT, system, router, "concurrent", 0x12340000 | marker,
            ((uint64_t)(0x12340000 | marker) << 32) | index);
    atomic_fetch_add(&g_finished, 1);
    return NULL;
}

static void validate_concurrent_record(const uint64_t values[TRACE_FIELDS]) {
    assert(values[TRACE_PHASE] == TRACE_INIT_EXIT);
    assert((values[TRACE_FLAGS] & ~UINT64_C(7)) == 0x12340000);
    assert(values[TRACE_SLOT] == (values[TRACE_FLAGS] & 7));
    assert(values[TRACE_MASK] == (1u << values[TRACE_SLOT]));
    assert(values[TRACE_RESULT] >> 32 == values[TRACE_FLAGS]);
    assert((values[TRACE_RESULT] & UINT32_MAX) < 1024);
}

static void test_concurrent_publication(void) {
    clear_trace();
    pthread_t writers[8];
    for (unsigned index = 0; index < 8; index++)
        assert(!pthread_create(&writers[index], NULL, concurrent_trace, (void *)(uintptr_t)index));
    while (atomic_load(&g_finished) != 8) {
        uint64_t end = atomic_load(&g_havok_trace_issued);
        for (uint64_t sequence = end > 8 ? end - 7 : 1; sequence <= end; sequence++) {
            uint64_t values[TRACE_FIELDS];
            if (trace_snapshot(sequence, values)) validate_concurrent_record(values);
        }
    }
    for (unsigned index = 0; index < 8; index++) assert(!pthread_join(writers[index], NULL));
    assert(atomic_load(&g_havok_trace_issued) == 8192);
    unsigned retained = 0;
    for (uint64_t sequence = 8192 - HAVOK_TRACE_CAPACITY + 1; sequence <= 8192; sequence++) {
        uint64_t values[TRACE_FIELDS];
        if (trace_snapshot(sequence, values)) {
            validate_concurrent_record(values);
            retained++;
        }
    }
    assert(retained && retained <= HAVOK_TRACE_CAPACITY);
    uint64_t values[TRACE_FIELDS];
    assert(!trace_snapshot(1, values));  /* A wrapped entry cannot look current. */
    clear_trace();
    atomic_store(&g_havok_trace_issued, 1);
    assert(!trace_snapshot(1, values)); /* Never expose an unpublished payload. */
    clear_trace();
}

static void test_fatal_dump(int signal_number) {
    int descriptors[2];
    assert(!pipe(descriptors));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(descriptors[0]);
        assert(dup2(descriptors[1], STDERR_FILENO) == STDERR_FILENO);
        close(descriptors[1]);
        crash_install();
        raise(signal_number);
        _exit(99);
    }
    close(descriptors[1]);
    char output[16384];
    size_t length = 0;
    ssize_t count;
    while ((count = read(descriptors[0], output + length, sizeof output - 1 - length)) > 0) {
        length += (size_t)count;
        assert(length < sizeof output - 1);
    }
    assert(count == 0);
    output[length] = 0;
    close(descriptors[0]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 128 + signal_number);
    assert(strstr(output, "RUN_CRASH") && strstr(output, "HAVOK_TRACE_DUMP issued=4 retained=4"));
    assert(strstr(output, "seq=1 phase=init_enter") && strstr(output, "seq=4 phase=quit_exit"));
    assert(strstr(output, "result=0x1234") && strstr(output, "result=0x4321"));
}

__attribute__((naked, noinline)) static uint64_t fixture_init(void *system __attribute__((unused)),
    void *router __attribute__((unused)), const char *name __attribute__((unused)), uint32_t flags __attribute__((unused))) {
    __asm__("stp x29,x30,[sp,#-64]!\n"
            "stp x24,x23,[sp,#16]\n"
            "stp x22,x21,[sp,#32]\n"
            "stp x20,x19,[sp,#48]\n"
            "mov x0,#0x1234\n"
            "ldp x20,x19,[sp,#48]\n"
            "ldp x22,x21,[sp,#32]\n"
            "ldp x24,x23,[sp,#16]\n"
            "ldp x29,x30,[sp],#64\nret\n");
}

__attribute__((naked, noinline)) static uint64_t fixture_quit(void *system __attribute__((unused)),
    void *router __attribute__((unused)), uint32_t flags __attribute__((unused))) {
    __asm__("stp x30,x23,[sp,#-48]!\n"
            "stp x22,x21,[sp,#16]\n"
            "stp x20,x19,[sp,#32]\n"
            "ldrsw x23,[x1,#112]\n"
            "mov x0,#0x4321\n"
            "ldp x20,x19,[sp,#32]\n"
            "ldp x22,x21,[sp,#16]\n"
            "ldp x30,x23,[sp],#48\nret\n");
}

int main(void) {
    assert(!pthread_key_create(&g_key18, NULL));
    assert(!pthread_key_create(&g_keytls, NULL));
    assert(!pthread_key_create(&g_keytlr, NULL));
    assert(!pthread_key_create(&g_keygate, NULL));
    guest_thread_init();
    sky_svc_main_thread_init();
    size_t page = (size_t)getpagesize();
    uint8_t *code = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(code != MAP_FAILED);
    memcpy(code + 4, fixture_init, 40);  /* Exercise a merely 4-byte-aligned entry. */
    memcpy(code + 128, fixture_quit, 36);
    assert(!mprotect(code, page, PROT_READ | PROT_EXEC));
    module_t module = {.base = code, .size = page, .text_end = page};
    uint8_t system[0x2300] = {0}, router[0x80] = {0};
    system[0x738] = system[0x738 + 2 * 0x1d0] = 1;
    uint8_t original_system[sizeof system], original_router[sizeof router];
    memcpy(original_system, system, sizeof system);
    memcpy(original_router, router, sizeof router);
    havok_init_fn init = (havok_init_fn)(code + 4);
    havok_quit_fn quit = (havok_quit_fn)(code + 128);
    assert(init(system, router, "synthetic observer", 3) == 0x1234);
    assert(quit(system, router, 3) == 0x4321);
    const uint32_t init_prologue[4] = {0xa9bc7bfd, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    const uint32_t quit_prologue[4] = {0xa9bd5ffe, 0xa90157f6, 0xa9024ff4, 0xb9807037};
    g_havok_original_init = (havok_init_fn)trace_install_function(&module, 4, init_prologue, trace_havok_init);
    g_havok_original_quit = (havok_quit_fn)trace_install_function(&module, 128, quit_prologue, trace_havok_quit);
    test_concurrent_publication();
    int descriptors[2], saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0 && !pipe(descriptors));
    assert(dup2(descriptors[1], STDERR_FILENO) == STDERR_FILENO);
    close(descriptors[1]);
    assert(init(system, router, "synthetic observer", 3) == 0x1234);
    assert(quit(system, router, 3) == 0x4321);
    assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    close(saved_stderr);
    char unexpected;
    assert(read(descriptors[0], &unexpected, 1) == 0); /* Recording is silent. */
    close(descriptors[0]);
    uint64_t observed[TRACE_FIELDS];
    assert(trace_snapshot(2, observed));
    assert(observed[TRACE_HANDLE] == sky_svc_current_thread_handle());
    assert(observed[TRACE_SLOT] == 0 && observed[TRACE_MASK] == 5 && observed[TRACE_RESULT] == 0x1234);
    assert(observed[TRACE_SYSTEM] == (uintptr_t)system && observed[TRACE_ROUTER] == (uintptr_t)router);
    assert(observed[TRACE_TLS] == (uintptr_t)pthread_getspecific(g_keytls));
    test_fatal_dump(SIGTRAP);
    test_fatal_dump(SIGABRT);
    assert(!memcmp(system, original_system, sizeof system));
    assert(!memcmp(router, original_router, sizeof router));
    assert(!munmap(code, page));
    puts("T_HAVOK_OBSERVER OK original-prologues return-values read-only-state aligned-and-unaligned-entry");
    puts("T_HAVOK_BUFFER OK silent-recording wrap concurrent-publication unpublished-rejection fatal-sigtrap-sigabrt-dump");
    return 0;
}
