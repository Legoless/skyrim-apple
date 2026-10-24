#include "crash.h"
#include "profile.h"
#include "runtime.h"
#include "skyhost.h"
#include "svc.h"
#include "thread.h"

#include <mach/mach.h>
#include <TargetConditionals.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

typedef void (*guest_fn0)(void);
typedef void (*guest_fn2)(uint64_t, uint64_t);

static uint64_t g_nnmain_start, g_nnmain_end;
static int g_nnmain_entered;
static char g_stage[128];

static void checkpoint(const char *name) {
    sky_checkpoint(name);
    LOG("CHECKPOINT %s", name);
}

static module_t *module_named(const char *name) {
    for (int index = 0; index < g_nmod; index++)
        if (!strcmp(g_mod[index].name, name)) return &g_mod[index];
    die("required module %s is not loaded", name);
}

static void *required_symbol(const char *module_name, const char *name) {
    module_t *module = module_named(module_name);
    void *result = module_sym(module, name);
    if (!result) die("required symbol %s is missing from %s", name, module_name);
    return result;
}

static const Elf64_Sym *required_symbol_record(const char *module_name, const char *name) {
    module_t *module = module_named(module_name);
    for (unsigned index = 1; index < module->nsym; index++) {
        const Elf64_Sym *symbol = &module->symtab[index];
        if (symbol->st_shndx && !strcmp(module->strtab + symbol->st_name, name)) return symbol;
    }
    die("required symbol record %s is missing from %s", name, module_name);
}

static void call0(const char *module, const char *name) {
    ((guest_fn0)required_symbol(module, name))();
}

#if TARGET_OS_IPHONE
void runtime_havok_trace_dump(void) { }

static void install_havok_observation(void) {
    const char *enabled = getenv("SKY_HAVOK_TRACE");
    if (enabled && (!strcmp(enabled, "1") || !strcmp(enabled, "buffered") || !strcmp(enabled, "stdio")))
        LOG("HAVOK_TRACE_UNAVAILABLE platform=ios reason=signed-code-is-immutable");
}
#else
typedef uint64_t (*havok_init_fn)(void *, void *, const char *, uint32_t);
typedef uint64_t (*havok_quit_fn)(void *, void *, uint32_t);
static havok_init_fn g_havok_original_init;
static havok_quit_fn g_havok_original_quit;
static int g_havok_trace_stdio;

#define HAVOK_TRACE_CAPACITY 256
enum { TRACE_PHASE, TRACE_HANDLE, TRACE_NATIVE, TRACE_SYSTEM, TRACE_ROUTER, TRACE_NAME,
       TRACE_FLAGS, TRACE_SLOT, TRACE_MASK, TRACE_TLS, TRACE_TLR, TRACE_RESULT, TRACE_FIELDS };
enum { TRACE_INIT_ENTER, TRACE_INIT_EXIT, TRACE_QUIT_ENTER, TRACE_QUIT_EXIT };
static const char *const g_havok_phases[] = {"init_enter", "init_exit", "quit_enter", "quit_exit"};
typedef struct {
    _Atomic unsigned busy;
    _Atomic uint64_t completed;
    _Atomic uint64_t fields[TRACE_FIELDS];
} havok_trace_record_t;
static havok_trace_record_t g_havok_trace[HAVOK_TRACE_CAPACITY];
static _Atomic uint64_t g_havok_trace_issued, g_havok_trace_dropped;
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "Havok observer requires lock-free publication");

/* A slot cannot be reused by two writers. All payload fields are atomic so a
 * fatal-signal dump can safely reject a record overwritten during its read.
 * The hot observer does not allocate, lock, print, or call Mach APIs. */
static void trace_havok_buffer(unsigned phase, void *system, void *router, const char *name,
    uint32_t flags, uint64_t result) {
    uint64_t sequence = atomic_fetch_add_explicit(&g_havok_trace_issued, 1, memory_order_relaxed) + 1;
    havok_trace_record_t *record = &g_havok_trace[(sequence - 1) % HAVOK_TRACE_CAPACITY];
    unsigned idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&record->busy, &idle, 1, memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&g_havok_trace_dropped, 1, memory_order_relaxed);
        return;
    }
    /* A delayed old reservation must not replace a newer completed lap. */
    if (atomic_load_explicit(&record->completed, memory_order_seq_cst) >= sequence) {
        atomic_store_explicit(&record->busy, 0, memory_order_release);
        atomic_fetch_add_explicit(&g_havok_trace_dropped, 1, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&record->completed, 0, memory_order_seq_cst);
    uint32_t mask = 0;
    for (unsigned index = 0; index < 16; index++)
        if (*(volatile const uint8_t *)((uintptr_t)system + 0x738 + index * 0x1d0)) mask |= 1u << index;
    const uint64_t values[TRACE_FIELDS] = {
        phase, sky_svc_current_thread_handle(), (uintptr_t)pthread_self(), (uintptr_t)system,
        (uintptr_t)router, (uintptr_t)name, flags,
        *(volatile const uint32_t *)((uintptr_t)router + 0x70), mask,
        (uintptr_t)pthread_getspecific(g_keytls), (uintptr_t)pthread_getspecific(g_keytlr), result,
    };
    for (unsigned index = 0; index < TRACE_FIELDS; index++)
        atomic_store_explicit(&record->fields[index], values[index], memory_order_seq_cst);
    atomic_store_explicit(&record->completed, sequence, memory_order_seq_cst);
    atomic_store_explicit(&record->busy, 0, memory_order_release);
}

static int trace_snapshot(uint64_t sequence, uint64_t values[TRACE_FIELDS]) {
    havok_trace_record_t *record = &g_havok_trace[(sequence - 1) % HAVOK_TRACE_CAPACITY];
    if (atomic_load_explicit(&record->completed, memory_order_seq_cst) != sequence) return 0;
    for (unsigned index = 0; index < TRACE_FIELDS; index++)
        values[index] = atomic_load_explicit(&record->fields[index], memory_order_seq_cst);
    return atomic_load_explicit(&record->completed, memory_order_seq_cst) == sequence;
}

static int trace_read(uint64_t address, void *out, size_t size) {
    vm_size_t copied = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)out, &copied) == KERN_SUCCESS && copied == size;
}

static void trace_label(uint64_t name, char label[96]) {
    memset(label, 0, 96);
    if (name && trace_read(name, label, 95)) {
        for (size_t index = 0; index < 95 && label[index]; index++)
            if ((unsigned char)label[index] < 0x20 || (unsigned char)label[index] > 0x7e) label[index] = '?';
    } else strcpy(label, name ? "(unreadable)" : "(none)");
}

void runtime_havok_trace_dump(void) {
    uint64_t end = atomic_load_explicit(&g_havok_trace_issued, memory_order_acquire);
    if (!end) return;
    uint64_t begin = end > HAVOK_TRACE_CAPACITY ? end - HAVOK_TRACE_CAPACITY + 1 : 1;
    LOG("HAVOK_TRACE_DUMP issued=%llu retained=%llu overwritten=%llu dropped=%llu",
        (unsigned long long)end, (unsigned long long)(end - begin + 1),
        (unsigned long long)(begin - 1), (unsigned long long)atomic_load(&g_havok_trace_dropped));
    for (uint64_t sequence = begin; sequence <= end; sequence++) {
        uint64_t values[TRACE_FIELDS];
        if (!trace_snapshot(sequence, values)) {
            LOG("HAVOK_TRACE seq=%llu pending-or-overwritten=1", (unsigned long long)sequence);
            continue;
        }
        char label[96];
        trace_label(values[TRACE_NAME], label);
        LOG("HAVOK_TRACE seq=%llu phase=%s handle=%#llx native_pthread=%#llx system=%#llx router=%#llx name=%s name_pointer=%#llx flags=%#llx slot=%llu used_mask=%#llx tls=%#llx tlr=%#llx result=%#llx",
            (unsigned long long)sequence, g_havok_phases[values[TRACE_PHASE]],
            (unsigned long long)values[TRACE_HANDLE], (unsigned long long)values[TRACE_NATIVE],
            (unsigned long long)values[TRACE_SYSTEM], (unsigned long long)values[TRACE_ROUTER], label,
            (unsigned long long)values[TRACE_NAME], (unsigned long long)values[TRACE_FLAGS],
            (unsigned long long)values[TRACE_SLOT], (unsigned long long)values[TRACE_MASK],
            (unsigned long long)values[TRACE_TLS], (unsigned long long)values[TRACE_TLR],
            (unsigned long long)values[TRACE_RESULT]);
    }
}

static void trace_havok(unsigned phase, void *system, void *router, const char *name, uint32_t flags, uint64_t result) {
    if (!g_havok_trace_stdio) {
        trace_havok_buffer(phase, system, router, name, flags, result);
        return;
    }
    uint32_t mask = 0, slot = UINT32_MAX;
    for (unsigned index = 0; index < 16; index++) {
        uint8_t used = 0;
        if (trace_read((uint64_t)system + 0x738 + index * 0x1d0, &used, 1) && used) mask |= 1u << index;
    }
    (void)trace_read((uint64_t)router + 0x70, &slot, sizeof slot);
    char label[96];
    trace_label((uint64_t)name, label);
    uint64_t native_id = 0;
    pthread_threadid_np(NULL, &native_id);
    fprintf(stderr, "[skyhost] HAVOK_TRACE phase=%s handle=%#x native=%llu system=%p router=%p name=%s flags=%#x slot=%u used_mask=%#x tls=%p tlr=%p\n",
        g_havok_phases[phase], sky_svc_current_thread_handle(), (unsigned long long)native_id,
        system, router, label, flags, slot, mask, pthread_getspecific(g_keytls), pthread_getspecific(g_keytlr));
}

__attribute__((noinline)) static uint64_t trace_havok_init(void *system, void *router, const char *name, uint32_t flags) {
    trace_havok(TRACE_INIT_ENTER, system, router, name, flags, 0);
    uint64_t result = g_havok_original_init(system, router, name, flags);
    trace_havok(TRACE_INIT_EXIT, system, router, name, flags, result);
    return result;
}

__attribute__((noinline)) static uint64_t trace_havok_quit(void *system, void *router, uint32_t flags) {
    trace_havok(TRACE_QUIT_ENTER, system, router, NULL, flags, 0);
    uint64_t result = g_havok_original_quit(system, router, flags);
    trace_havok(TRACE_QUIT_EXIT, system, router, NULL, flags, result);
    return result;
}

static void trace_jump(uint8_t *at, const void *target) {
    const uint32_t code[] = {0x58000050, 0xd61f0200}; /* ldr x16,pc+8; br x16 */
    memcpy(at, code, sizeof code);
    uintptr_t address = (uintptr_t)target;
    memcpy(at + 8, &address, sizeof address);
}

static void *trace_install_function(module_t *module, uint64_t offset, const uint32_t expected[4], void *observer) {
    if (offset > module->text_end || module->text_end - offset < 16 || memcmp(module->base + offset, expected, 16))
        die("Havok observation prologue mismatch at main+%#llx", (unsigned long long)offset);
    size_t page_size = (size_t)getpagesize();
    uint8_t *trampoline = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (trampoline == MAP_FAILED) die("allocate Havok observation trampoline");
    memcpy(trampoline, module->base + offset, 16);
    trace_jump(trampoline + 16, module->base + offset + 16);
    __builtin___clear_cache((char *)trampoline, (char *)trampoline + 32);
    if (mprotect(trampoline, page_size, PROT_READ | PROT_EXEC)) die("protect Havok observation trampoline");
    uintptr_t page = ((uintptr_t)module->base + offset) & ~(page_size - 1);
    if (mprotect((void *)page, page_size, PROT_READ | PROT_WRITE)) die("open Havok observation page");
    trace_jump(module->base + offset, observer);
    __builtin___clear_cache((char *)module->base + offset, (char *)module->base + offset + 16);
    if (mprotect((void *)page, page_size, PROT_READ | PROT_EXEC)) die("seal Havok observation page");
    return trampoline;
}

static void install_havok_observation(void) {
    const char *enabled = getenv("SKY_HAVOK_TRACE");
    if (!enabled || (strcmp(enabled, "1") && strcmp(enabled, "buffered") && strcmp(enabled, "stdio"))) return;
    if (!sky_profile_has_update_ui()) {
        LOG("HAVOK_TRACE_UNAVAILABLE unverified-main-build");
        return;
    }
    g_havok_trace_stdio = !strcmp(enabled, "stdio");
    module_t *main = module_named("main");
    const uint32_t init_prologue[4] = {0xa9bc7bfd, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    const uint32_t quit_prologue[4] = {0xa9bd5ffe, 0xa90157f6, 0xa9024ff4, 0xb9807037};
    g_havok_original_init = (havok_init_fn)trace_install_function(main, 0x10a197c, init_prologue, trace_havok_init);
    g_havok_original_quit = (havok_quit_fn)trace_install_function(main, 0x10a1ca4, quit_prologue, trace_havok_quit);
    LOG("HAVOK_TRACE_INSTALLED init=main+0x10a197c quit=main+0x10a1ca4 mode=%s capacity=%u",
        g_havok_trace_stdio ? "stdio" : "buffered", HAVOK_TRACE_CAPACITY);
}
#endif

static void module_checkpoint(const char *prefix, const char *module, const char *suffix) {
    snprintf(g_stage, sizeof g_stage, "%s%s%s", prefix, module, suffix);
    checkpoint(g_stage);
}

static void run_module_inits(void) {
    for (int index = g_nmod - 1; index >= 0; index--) {
        module_t *module = &g_mod[index];
        if (!module->init) {
            module_checkpoint("module_init_", module->name, "_absent");
            continue;
        }
        module_checkpoint("module_init_", module->name, "");
        LOG("MODULE_INIT module=%s init=%#llx init_array_entries=%llu", module->name,
            (unsigned long long)module->init, (unsigned long long)(module->init_arraysz / 8));
        ((guest_fn0)(module->base + module->init))();
        module_checkpoint("module_init_", module->name, "_ok");
    }
}

typedef struct {
    uint8_t *bytes;
    size_t argv_offset, packed_base;
    size_t packed_offsets[2];
} argument_block_t;

static argument_block_t make_argument_block(void) {
    unsigned count = sky_profile.argument_count;
    if (!count || count > 2) die("unsupported Skyrim profile argument count %u", count);
    size_t string_bytes = 0;
    for (unsigned index = 0; index < count; index++) {
        const char *argument = sky_profile.arguments[index];
        if (!argument || !*argument || strchr(argument, ' ')) die("unsupported profile argument %s", argument);
        string_bytes += strlen(argument) + 1;
    }
    argument_block_t block = {0};
    block.argv_offset = (0x29 + string_bytes * 2) & ~(size_t)7;
    block.packed_base = 0x20 + string_bytes;
    size_t total = block.argv_offset + (count + 1) * 8;
    if (block.argv_offset > UINT32_MAX - count * 8) die("argument block too large");
    block.bytes = calloc(1, total);
    if (!block.bytes) die("allocate Skyrim argument block");
    uint32_t header = (uint32_t)(count * 8 + block.argv_offset);
    memcpy(block.bytes, &header, sizeof header);
    uint32_t strings32 = (uint32_t)string_bytes;
    memcpy(block.bytes + 4, &strings32, sizeof strings32);

    size_t source = 0x20;
    for (unsigned index = 0; index < count; index++) {
        const char *argument = sky_profile.arguments[index];
        size_t length = strlen(argument) + 1;
        memcpy(block.bytes + source, argument, length);
        source += length;
        block.packed_offsets[index] = index ? block.packed_offsets[index - 1] + strlen(sky_profile.arguments[index - 1]) + 1 : 0;
    }
    return block;
}

static void validate_arguments(const argument_block_t *block) {
    uint64_t *argv = (uint64_t *)(block->bytes + block->argv_offset);
    for (unsigned index = 0; index < sky_profile.argument_count; index++) {
        const char *expected = sky_profile.arguments[index];
        size_t packed = block->packed_base + block->packed_offsets[index];
        uint64_t expected_address = (uint64_t)block->bytes + packed;
        if (argv[index] != expected_address || strcmp((const char *)block->bytes + packed, expected)) {
            LOG("ARGUMENTS_MISMATCH index=%u argv_offset=%#zx actual=%#llx expected=%#llx packed=%s header0=%#x header1=%#x",
                index, block->argv_offset, (unsigned long long)argv[index],
                (unsigned long long)expected_address, (const char *)block->bytes + packed,
                *(uint32_t *)block->bytes, *(uint32_t *)(block->bytes + 4));
            die("original nnosInitialize produced invalid argv[%u]", index);
        }
    }
    if (argv[sky_profile.argument_count]) die("original nnosInitialize did not terminate argv");
    LOG("ARGUMENTS_OK argc=%u argv0=%s", sky_profile.argument_count,
        (const char *)block->bytes + block->packed_base + block->packed_offsets[0]);
}

static void runtime_gate_context(const uint64_t *regs) {
    if (g_nnmain_entered || !g_nnmain_start) return;
    uint64_t frame_pointer = regs[29];
    for (int index = 0; index < 48 && frame_pointer && !(frame_pointer & 7); index++) {
        uint64_t frame[2];
        vm_size_t size = 0;
        if (vm_read_overwrite(mach_task_self(), frame_pointer, sizeof frame,
                              (vm_address_t)frame, &size) != KERN_SUCCESS || size != sizeof frame)
            return;
        module_t *module = module_of((const void *)frame[1]);
        uint64_t address = frame[1];
        if (module && address >= g_nnmain_start && address < g_nnmain_end) {
            g_nnmain_entered = 1;
            sky_post_entry_enable();
            checkpoint("nnMain_entered");
            LOG("NNMAIN_ENTERED evidence=%s+%#llx", module->name,
                (unsigned long long)(address - (uint64_t)module->base));
        }
        if (frame[0] <= frame_pointer) return;
        frame_pointer = frame[0];
    }
}

int runtime_run(void) {
    crash_install();
    sky_gate_context_register(runtime_gate_context);
    const char *timeout = getenv("SKY_RUNTIME_TIMEOUT");
    unsigned seconds = 0;
    if (timeout) {
        char *end = NULL;
        errno = 0;
        unsigned long value = strtoul(timeout, &end, 0);
        if (errno || !*timeout || *timeout == '-' || *end || value > UINT_MAX)
            die("SKY_RUNTIME_TIMEOUT must be a nonnegative number of seconds");
        seconds = (unsigned)value;
    }
    alarm(seconds);

    const Elf64_Sym *nnmain = required_symbol_record("main", "nnMain");
    module_t *main_module = module_named("main");
    sky_set_main_module_base((uintptr_t)main_module->base);
    sky_set_guest_main_thread(pthread_self());
    g_nnmain_start = (uint64_t)main_module->base + nnmain->st_value;
    g_nnmain_end = g_nnmain_start + nnmain->st_size;
    if (!nnmain->st_size) die("nnMain has no extent for entry validation");

    checkpoint("guest_tls");
    guest_thread_init();
    checkpoint("guest_tls_ok");
    checkpoint("main_handle");
    uint32_t main_handle = sky_svc_main_thread_init();
    LOG("MAIN_HANDLE value=%#x", main_handle);
    checkpoint("main_handle_ok");
    install_havok_observation();

    checkpoint("arguments_prepare");
    argument_block_t arguments = make_argument_block();
    checkpoint("arguments_prepared");
    checkpoint("libc0");
    call0("sdk", "_init_libc0");
    checkpoint("libc0_ok");
    checkpoint("nnosInitialize");
    ((guest_fn2)required_symbol("sdk", "nnosInitialize"))(main_handle, (uint64_t)arguments.bytes);
    checkpoint("nnosInitialize_ok");
    validate_arguments(&arguments);
    checkpoint("arguments_ok");

    checkpoint("libc1");
    call0("sdk", "_init_libc1");
    checkpoint("libc1_ok");
    checkpoint("nninitInitializeSdkModule");
    call0("sdk", "nninitInitializeSdkModule");
    checkpoint("nninitInitializeSdkModule_ok");
    checkpoint("nninitStartup");
    call0("main", "nninitStartup");
    checkpoint("nninitStartup_ok");
    checkpoint("libc2");
    call0("sdk", "_init_libc2");
    checkpoint("libc2_ok");
    checkpoint("module_constructors");
    run_module_inits();
    checkpoint("module_constructors_ok");

    checkpoint("nnMain_call");
    call0("main", "nnMain");
    alarm(0);
    if (!g_nnmain_entered) {
        g_nnmain_entered = 1;
        sky_post_entry_enable();
        checkpoint("nnMain_entered");
        LOG("NNMAIN_ENTERED evidence=return-from-original-main+%#llx", (unsigned long long)nnmain->st_value);
    }
    LOG("RUN_OK reason=original-nnMain-returned");
    return 0;
}
