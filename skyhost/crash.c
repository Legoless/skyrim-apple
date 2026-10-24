#include "crash.h"
#include "runtime.h"
#include "skyhost.h"
#include "svc.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

static int g_havok_diagnostic;

static int diagnostic_read(uint64_t address, void *out, size_t size) {
    vm_size_t copied = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)out, &copied) == KERN_SUCCESS && copied == size;
}

static void havok_diagnostic(const ucontext_t *context) {
    if (!g_havok_diagnostic) return;
    const uint64_t *regs = context->uc_mcontext->__ss.__x;
    module_t *module = module_of((const void *)context->uc_mcontext->__ss.__pc);
    if (!module || strcmp(module->name, "main") ||
        context->uc_mcontext->__ss.__pc - (uint64_t)module->base != 0x10a1c8c) return;
    uint64_t memory_system = regs[20], router = regs[19];
    void *tls = pthread_getspecific(g_keytls), *tlr = pthread_getspecific(g_keytlr);
    uint64_t thread_object = 0;
    if (tlr) (void)diagnostic_read((uint64_t)tlr + 0x1f8, &thread_object, sizeof thread_object);
    LOG("HAVOK_DIAGNOSTIC memory_system=%#llx router=%#llx tls=%p tlr=%p thread_object=%#llx",
        (unsigned long long)memory_system, (unsigned long long)router, tls, tlr, (unsigned long long)thread_object);
    for (unsigned index = 0; index < 16; index++) {
        uint64_t address = memory_system + 0x718 + index * 0x1d0, words[8];
        if (!diagnostic_read(address, words, sizeof words)) {
            LOG("HAVOK_SLOT index=%u address=%#llx unreadable=1", index, (unsigned long long)address);
            continue;
        }
        LOG("HAVOK_SLOT index=%u address=%#llx used=%u words=%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx",
            index, (unsigned long long)address, ((const uint8_t *)words)[0x20],
            (unsigned long long)words[0], (unsigned long long)words[1], (unsigned long long)words[2], (unsigned long long)words[3],
            (unsigned long long)words[4], (unsigned long long)words[5], (unsigned long long)words[6], (unsigned long long)words[7]);
    }
    uint64_t words[8];
    if (diagnostic_read(router, words, sizeof words))
        LOG("HAVOK_ROUTER words=%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx",
            (unsigned long long)words[0], (unsigned long long)words[1], (unsigned long long)words[2], (unsigned long long)words[3],
            (unsigned long long)words[4], (unsigned long long)words[5], (unsigned long long)words[6], (unsigned long long)words[7]);
}

static void print_address(const char *label, uint64_t address) {
    module_t *module = module_of((const void *)address);
    if (module) fprintf(stderr, " %s=%s+%#llx", label, module->name,
                        (unsigned long long)(address - (uint64_t)module->base));
    else fprintf(stderr, " %s=%#llx", label, (unsigned long long)address);
}

void crash_backtrace(uint64_t frame_pointer) {
    fprintf(stderr, "[skyhost] BT");
    for (int index = 0; index < 40 && frame_pointer && !(frame_pointer & 7); index++) {
        uint64_t frame[2];
        vm_size_t size = 0;
        if (vm_read_overwrite(mach_task_self(), frame_pointer, sizeof frame,
                              (vm_address_t)frame, &size) != KERN_SUCCESS || size != sizeof frame)
            break;
        print_address("ret", frame[1]);
        if (frame[0] <= frame_pointer) break;
        frame_pointer = frame[0];
    }
    fputc('\n', stderr);
}

static void on_signal(int signal_number, siginfo_t *info, void *context_pointer) {
    ucontext_t *context = context_pointer;
    if (signal_number == SIGALRM) {
        sky_svc_thread_diagnose();
        uint64_t pc = context->uc_mcontext->__ss.__pc;
        module_t *module = module_of((const void *)pc);
        if (module && !strcmp(module->name, "main") && !strcmp(sky_checkpoint_name(), "module_init_main"))
            fprintf(stderr, "[skyhost] RUN_STOP checkpoint=module_init_main reason=MODULE_INIT_INCOMPLETE pc=main+%#llx\n",
                    (unsigned long long)(pc - (uint64_t)module->base));
        else
            fprintf(stderr, "[skyhost] RUN_TIMEOUT checkpoint=%s\n", sky_checkpoint_name());
    } else {
        fprintf(stderr, "[skyhost] RUN_CRASH checkpoint=%s sig=%d", sky_checkpoint_name(), signal_number);
    }
    if (signal_number != SIGALRM) print_address("addr", (uint64_t)info->si_addr);
    print_address("pc", context->uc_mcontext->__ss.__pc);
    print_address("lr", context->uc_mcontext->__ss.__lr);
    fprintf(stderr, " sp=%#llx\n", (unsigned long long)context->uc_mcontext->__ss.__sp);
    for (int index = 0; index < 29; index++)
        fprintf(stderr, "x%d=%#llx%s", index, (unsigned long long)context->uc_mcontext->__ss.__x[index],
                index % 4 == 3 ? "\n" : " ");
    crash_backtrace(context->uc_mcontext->__ss.__fp);
    runtime_havok_trace_dump();
    if (signal_number == SIGTRAP) havok_diagnostic(context);
    _exit(signal_number == SIGALRM ? 124 : 128 + signal_number);
}

/* Opt-in sampling profiler (SKY_SAMPLE_PROFILE=<report seconds>). A host thread
 * suspends every other thread every 2 ms and records (a) its leaf PC and (b) its
 * guest site: the guest PC when running guest code, otherwise the first guest
 * return address on its frame-pointer chain (the game call that is blocked in
 * host code). The guest main thread gets its own site table. While any thread
 * is suspended it only uses Mach calls and static storage (no malloc/stdio). */
enum { SAMPLE_BUCKETS = 16384, SAMPLE_TOP = 25 };
typedef struct { uint64_t pc; uint32_t count; } sample_bucket_t;
typedef struct { sample_bucket_t b[SAMPLE_BUCKETS]; } sample_table_t;
static sample_table_t g_sample_leaf, g_sample_site, g_sample_main, g_sample_main_host, g_sample_main_leaf;
static uint64_t g_sample_total, g_sample_main_total;
#define SAMPLE_BLOCKED (1ull << 63)

static void sample_add(sample_table_t *table, uint64_t key) {
    uint32_t h = (uint32_t)(((key & ~SAMPLE_BLOCKED) >> 2) * 2654435761u) & (SAMPLE_BUCKETS - 1);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & (SAMPLE_BUCKETS - 1)) {
        if (table->b[h].pc == key) { table->b[h].count++; return; }
        if (!table->b[h].pc) { table->b[h] = (sample_bucket_t){key, 1}; return; }
    }
}

static uint64_t sample_guest_site(uint64_t pc, uint64_t fp, uint64_t lr, uint64_t *host_entry) {
    *host_entry = 0;
    if (module_of((const void *)pc)) return pc & ~0xfull;
    if (module_of((const void *)lr)) return lr | SAMPLE_BLOCKED;
    uint64_t last_host = lr & 0x0000ffffffffffffull;
    for (int depth = 0; depth < 24 && fp && !(fp & 7); depth++) {
        uint64_t frame[2];
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof frame, (vm_address_t)frame, &got) != KERN_SUCCESS ||
            got != sizeof frame)
            break;
        uint64_t ret = frame[1] & 0x0000ffffffffffffull;
        if (module_of((const void *)ret)) { *host_entry = last_host; return ret | SAMPLE_BLOCKED; }
        last_host = ret;
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    return 0;
}

static void sample_describe(char *out, size_t size, uint64_t key) {
    uint64_t pc = key & ~SAMPLE_BLOCKED;
    const char *kind = (key & SAMPLE_BLOCKED) ? "blocked-at" : "running";
    module_t *module = module_of((const void *)pc);
    Dl_info info;
    if (module)
        snprintf(out, size, "%s %s+%#llx", kind, module->name, (unsigned long long)(pc - (uint64_t)module->base));
    else if (dladdr((const void *)pc, &info) && info.dli_sname)
        snprintf(out, size, "host:%s+%#llx", info.dli_sname, (unsigned long long)(pc - (uint64_t)info.dli_saddr));
    else
        snprintf(out, size, "pc=%#llx", (unsigned long long)pc);
}

static void sample_report_table(const char *label, sample_table_t *table) {
    sample_bucket_t top[SAMPLE_TOP];
    memset(top, 0, sizeof top);
    for (int i = 0; i < SAMPLE_BUCKETS; i++) {
        sample_bucket_t b = table->b[i];
        if (!b.pc) continue;
        for (int t = 0; t < SAMPLE_TOP; t++) {
            if (b.count > top[t].count) {
                memmove(&top[t + 1], &top[t], (SAMPLE_TOP - t - 1) * sizeof top[0]);
                top[t] = b;
                break;
            }
        }
    }
    for (int t = 0; t < SAMPLE_TOP && top[t].count; t++) {
        char where[160];
        sample_describe(where, sizeof where, top[t].pc);
        LOG("SAMPLE_TOP table=%s rank=%d count=%u %s", label, t, top[t].count, where);
    }
    memset(table, 0, sizeof *table);
}

static void sample_report(double seconds) {
    LOG("SAMPLE_REPORT seconds=%.1f samples=%llu main_samples=%llu", seconds,
        (unsigned long long)g_sample_total, (unsigned long long)g_sample_main_total);
    sample_report_table("main", &g_sample_main);
    sample_report_table("main_host", &g_sample_main_host);
    sample_report_table("main_leaf", &g_sample_main_leaf);
    sample_report_table("site", &g_sample_site);
    sample_report_table("leaf", &g_sample_leaf);
    g_sample_total = g_sample_main_total = 0;
}

static void *sampler_main(void *arg) {
    double interval = *(double *)arg;
    thread_act_t self = mach_thread_self();
    uint64_t started = now_ns();
    for (;;) {
        pthread_t main_pthread = sky_guest_main_thread();
        thread_act_t main_port = main_pthread ? pthread_mach_thread_np(main_pthread) : MACH_PORT_NULL;
        thread_act_array_t threads;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS) {
            for (mach_msg_type_number_t i = 0; i < count; i++) {
                if (threads[i] != self && thread_suspend(threads[i]) == KERN_SUCCESS) {
                    arm_thread_state64_t state;
                    mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
                    if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &state_count) ==
                        KERN_SUCCESS) {
                        uint64_t pc = (uint64_t)arm_thread_state64_get_pc(state);
                        uint64_t fp = (uint64_t)arm_thread_state64_get_fp(state);
                        uint64_t lr = (uint64_t)arm_thread_state64_get_lr(state);
                        sample_add(&g_sample_leaf, pc & ~0xfull);
                        uint64_t host_entry = 0;
                        uint64_t site = sample_guest_site(pc, fp, lr, &host_entry);
                        if (site) sample_add(&g_sample_site, site);
                        if (threads[i] == main_port) {
                            sample_add(&g_sample_main, site ? site : (pc & ~0xfull));
                            if (host_entry) sample_add(&g_sample_main_host, host_entry);
                            sample_add(&g_sample_main_leaf, pc & ~0xfull);
                            g_sample_main_total++;
                        }
                        g_sample_total++;
                    }
                    thread_resume(threads[i]);
                }
                mach_port_deallocate(mach_task_self(), threads[i]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof threads[0]);
        }
        usleep(2000);
        double elapsed = (double)(now_ns() - started) / 1e9;
        if (elapsed >= interval) {
            sample_report(elapsed);
            started = now_ns();
        }
    }
    return NULL;
}

static void sampler_start(void) {
    static double interval;
    const char *value = getenv("SKY_SAMPLE_PROFILE");
    if (!value || !*value) return;
    interval = atof(value);
    if (interval <= 0) interval = 10;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, sampler_main, &interval)) pthread_detach(thread);
    LOG("SAMPLE_PROFILER_STARTED interval=%.1f", interval);
}

void crash_install(void) {
    const char *diagnostic = getenv("SKY_HAVOK_DIAG");
    g_havok_diagnostic = diagnostic && !strcmp(diagnostic, "1");
    struct sigaction action = {.sa_sigaction = on_signal, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL) ||
        sigaction(SIGILL, &action, NULL) || sigaction(SIGTRAP, &action, NULL) ||
        sigaction(SIGABRT, &action, NULL) ||
        sigaction(SIGALRM, &action, NULL))
        die("install crash diagnostics");
    sampler_start();
}
