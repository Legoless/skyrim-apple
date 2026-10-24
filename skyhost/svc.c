#include "profile.h"
#include "skyhost.h"
#include "svc.h"
#include "thread.h"

#include <errno.h>
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
typedef vm_address_t sky_vm_address_t;
typedef vm_size_t sky_vm_size_t;
#define sky_vm_region vm_region_64
#define sky_vm_remap vm_remap
#define sky_vm_deallocate vm_deallocate
#else
#include <mach/mach_vm.h>
typedef mach_vm_address_t sky_vm_address_t;
typedef mach_vm_size_t sky_vm_size_t;
#define sky_vm_region mach_vm_region
#define sky_vm_remap mach_vm_remap
#define sky_vm_deallocate mach_vm_deallocate
#endif
#include <mach/vm_statistics.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

#define RESERVE_SIZE (16ull << 30)
#define REPORTED_MEMORY_TOTAL (4ull << 30)
#define R_BAD_HANDLE 0xE401u
#define MAIN_HANDLE 0x1001u
#define CURRENT_THREAD_HANDLE 0xFFFF8000u

static uint8_t *g_as, *g_alias, *g_stack, *g_heap;
static size_t g_alias_cap, g_stack_cap, g_heap_cap, g_heap_cur;

#define MAX_THREADS 64
typedef struct sky_thread_t sky_thread_t;
struct sky_thread_t {
    uint32_t handle, priority, core, mask;
    uint64_t id, entry, argument, stack_top;
    pthread_t thread;
    int started, exited, closed, reaping, joining, joined;
    int wait_kind, woke;
    uint64_t wait_addr, wait_key, wait_tag;
    uint64_t wait_sequence;
    int64_t wait_result;
    sky_thread_t *wait_next;
};
static sky_thread_t g_threads[MAX_THREADS];
static int g_nthreads;
static uint32_t g_next_handle = MAIN_HANDLE, g_main_handle;
static uint64_t g_next_thread_id = 0x52;
static pthread_key_t g_keythr;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_thread_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sync_cond = PTHREAD_COND_INITIALIZER;
static sky_thread_t *g_wait_list;
static uint64_t g_wait_sequence;

#define WAITERS_BIT 0x40000000u
#define R_TIMEOUT 0xEA01u
#define R_INVALID_ADDRESS 0xCC01u
#define R_INVALID_MEMORY 0xD401u
#define R_INVALID_ENUM 0xF001u
#define R_INVALID_STATE 0xFA01u

#define MAX_MAP 1024
typedef struct { uint64_t base, size; uint32_t state; } map_entry_t;
static map_entry_t g_map[MAX_MAP];
static int g_nmap;

#define MAX_TRANSFER 64
#define TRANSFER_HANDLE_BASE 0x10000u
typedef struct {
    uint32_t handle;
    uint8_t *source;
    size_t size;
    uint64_t mapped;
    size_t mapped_size;
} sky_transfer_t;
static sky_transfer_t g_transfers[MAX_TRANSFER];

static sky_transfer_t *find_transfer(uint32_t handle) {
    if (handle < TRANSFER_HANDLE_BASE || handle >= TRANSFER_HANDLE_BASE + MAX_TRANSFER) return NULL;
    sky_transfer_t *transfer = &g_transfers[handle - TRANSFER_HANDLE_BASE];
    return transfer->handle ? transfer : NULL;
}

static int map_find(uint64_t address, uint64_t size) {
    for (int index = 0; index < g_nmap; index++)
        if (address < g_map[index].base + g_map[index].size && g_map[index].base < address + size)
            return index + 1;
    return 0;
}

static void map_add(uint64_t base, uint64_t size, uint32_t state) {
    uint64_t end = base + size;
    for (int index = 0; index < g_nmap;) {
        map_entry_t *entry = &g_map[index];
        if (entry->state == state && entry->base <= end && base <= entry->base + entry->size) {
            if (entry->base < base) base = entry->base;
            if (entry->base + entry->size > end) end = entry->base + entry->size;
            g_map[index] = g_map[--g_nmap];
        } else {
            index++;
        }
    }
    if (g_nmap >= MAX_MAP) die("guest mapping table overflow");
    g_map[g_nmap++] = (map_entry_t){base, end - base, state};
}

static int map_remove(uint64_t address, uint64_t size) {
    uint64_t end = address + size, cursor = address;
    while (cursor < end) {
        int found = 0;
        for (int index = 0; index < g_nmap; index++) {
            if (g_map[index].base <= cursor && g_map[index].base + g_map[index].size > cursor) {
                uint64_t entry_end = g_map[index].base + g_map[index].size;
                cursor = entry_end < end ? entry_end : end;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    map_entry_t updated[MAX_MAP + 2];
    int count = 0;
    for (int index = 0; index < g_nmap; index++) {
        map_entry_t entry = g_map[index];
        if (entry.base >= end || entry.base + entry.size <= address) {
            updated[count++] = entry;
            continue;
        }
        if (entry.base < address) updated[count++] = (map_entry_t){entry.base, address - entry.base, entry.state};
        if (entry.base + entry.size > end)
            updated[count++] = (map_entry_t){end, entry.base + entry.size - end, entry.state};
    }
    if (count > MAX_MAP) die("guest mapping table overflow during unmap");
    g_nmap = count;
    memcpy(g_map, updated, count * sizeof *updated);
    return 1;
}

static size_t image_bytes(void) {
    size_t total = 0;
    for (int index = 0; index < g_nmod; index++) total += g_mod[index].size;
    return total;
}

static void memory_init(void) {
    if (g_as) return;
    g_as = mmap(NULL, RESERVE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest address space");
    g_alias = g_as + 0x10000000;
    g_alias_cap = 0x380000000;
    g_stack = g_as + 0x390000000;
    g_stack_cap = 0x10000000;
    g_heap = g_as + 0x3A0000000;
    g_heap_cap = RESERVE_SIZE - 0x3A0000000;
    LOG("GUEST_AS base=%p size=%#llx alias=%p stack=%p heap=%p", g_as,
        (unsigned long long)RESERVE_SIZE, g_alias, g_stack, g_heap);
}

static void thread_key_init(void) {
    if (pthread_key_create(&g_keythr, NULL)) die("create guest thread key");
}

static void thread_object_init(sky_thread_t *thread) {
    if (pthread_setspecific(g_keythr, thread)) die("install current guest thread");
    uint8_t *tlr = pthread_getspecific(g_keytlr);
    if (!tlr) die("thread object setup before guest TLS");
    uint8_t *object = tlr + 0x200;
    *(uint32_t *)(object + 0x1b0) = thread->handle;
    *(uint16_t *)(object + 0x44) = (uint16_t)thread->priority;
}

static sky_thread_t *current_thread(void) { return pthread_getspecific(g_keythr); }

static sky_thread_t *find_thread(uint32_t handle) {
    if (handle == CURRENT_THREAD_HANDLE) return current_thread();
    for (int index = 0; index < g_nthreads; index++)
        if (g_threads[index].handle == handle && !g_threads[index].closed && !g_threads[index].reaping)
            return &g_threads[index];
    return NULL;
}

void sky_svc_thread_diagnose(void) {
    sky_thread_t *current = current_thread();
    LOG("THREAD_DIAG current=%#x count=%d", current ? current->handle : 0, g_nthreads);
    for (int index = 0; index < g_nthreads; index++) {
        sky_thread_t *thread = &g_threads[index];
        uint32_t address_value = 0, key_value = 0;
        vm_size_t size = 0;
        if (thread->wait_addr)
            vm_read_overwrite(mach_task_self(), thread->wait_addr, sizeof address_value,
                              (vm_address_t)&address_value, &size);
        size = 0;
        if (thread->wait_key)
            vm_read_overwrite(mach_task_self(), thread->wait_key, sizeof key_value,
                              (vm_address_t)&key_value, &size);
        LOG("THREAD_STATE handle=%#x started=%d exited=%d closed=%d wait_kind=%d wait_addr=%#llx value=%#x wait_key=%#llx key_value=%#x entry=%#llx",
            thread->handle, thread->started, thread->exited, thread->closed, thread->wait_kind,
            (unsigned long long)thread->wait_addr, address_value,
            (unsigned long long)thread->wait_key, key_value, (unsigned long long)thread->entry);
    }
}

uint32_t sky_svc_main_thread_init(void) {
    pthread_once(&g_thread_once, thread_key_init);
    if (!g_nthreads) {
        sky_thread_t *main = &g_threads[0];
        g_nthreads = 1;
        main->handle = MAIN_HANDLE;
        main->id = 0x51;
        main->priority = 44;
        main->core = 0;
        main->mask = 1;
        main->started = 1;
        g_next_handle = MAIN_HANDLE + 1;
        g_main_handle = MAIN_HANDLE;
    }
    thread_object_init(&g_threads[0]);
    return MAIN_HANDLE;
}

uint32_t sky_svc_main_thread_handle(void) { return g_main_handle; }
uint32_t sky_svc_current_thread_handle(void) {
    sky_thread_t *thread = current_thread();
    return thread ? thread->handle : 0;
}

extern void dm_thread_enter(uint64_t entry, uint64_t argument, uint64_t stack_top);

static void *thread_main(void *argument) {
    sky_thread_t *thread = argument;
    pthread_mutex_lock(&g_thread_lock);
    while (!thread->started) pthread_cond_wait(&g_thread_cond, &g_thread_lock);
    pthread_mutex_unlock(&g_thread_lock);
    guest_thread_init();
    thread_object_init(thread);
    LOG("THREAD_ENTRY handle=%#x entry=%#llx stack=%#llx", thread->handle,
        (unsigned long long)thread->entry, (unsigned long long)thread->stack_top);
    dm_thread_enter(thread->entry, thread->argument, thread->stack_top);
    return NULL;
}

static uint32_t thread_create_guest(uint64_t *regs) {
    pthread_once(&g_thread_once, thread_key_init);
    int32_t requested_core = (int32_t)regs[5];
    if (requested_core < -2 || requested_core > 2) {
        regs[0] = 0xE201; /* InvalidCoreId */
        return regs[0];
    }
    pthread_mutex_lock(&g_thread_lock);
    sky_thread_t *thread = NULL;
    for (int index = 1; index < g_nthreads; index++) {
        sky_thread_t *candidate = &g_threads[index];
        if (!candidate->closed || !candidate->exited || candidate->reaping || candidate->joining) continue;
        candidate->reaping = 1;
        pthread_t previous = candidate->thread;
        pthread_mutex_unlock(&g_thread_lock);
        /* ExitThread publishes exit before pthread TLS cleanup finishes.
         * Joining guarantees its record and guest stack can be reused safely. */
        if (!candidate->joined && pthread_join(previous, NULL)) die("join exited guest thread");
        pthread_mutex_lock(&g_thread_lock);
        thread = candidate;
        break;
    }
    if (!thread) {
        if (g_nthreads >= MAX_THREADS) {
            pthread_mutex_unlock(&g_thread_lock);
            regs[0] = 0xCE01; /* OutOfResource */
            return regs[0];
        }
        thread = &g_threads[g_nthreads++];
    }
    memset(thread, 0, sizeof *thread);
    thread->handle = g_next_handle++;
    thread->id = g_next_thread_id++;
    thread->entry = regs[1];
    thread->argument = regs[2];
    thread->stack_top = regs[3] & ~0xFull;
    thread->priority = (uint32_t)regs[4];
    thread->core = requested_core >= 0 ? (uint32_t)requested_core : 0;
    thread->mask = requested_core >= 0 || requested_core == -2 ? 1u << thread->core : 7;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1 << 20);
    int result = pthread_create(&thread->thread, &attributes, thread_main, thread);
    pthread_attr_destroy(&attributes);
    pthread_mutex_unlock(&g_thread_lock);
    if (result) die("pthread_create guest thread");
    LOG("THREAD_CREATE handle=%#x id=%#llx core=%u mask=%#x requested_core=%d priority=%u", thread->handle,
        (unsigned long long)thread->id, thread->core, thread->mask, requested_core, thread->priority);
    regs[0] = 0;
    regs[1] = thread->handle;
    return 0;
}

static uint32_t thread_start(uint64_t handle) {
    pthread_mutex_lock(&g_thread_lock);
    sky_thread_t *thread = find_thread(handle);
    if (thread) {
        thread->started = 1;
        LOG("THREAD_START handle=%#llx", (unsigned long long)handle);
        pthread_cond_broadcast(&g_thread_cond);
    }
    pthread_mutex_unlock(&g_thread_lock);
    return thread ? 0 : R_BAD_HANDLE;
}

__attribute__((noreturn)) void dm_thread_returned(void) { die("guest thread entry returned"); }

static __attribute__((noreturn)) void thread_exit(void) {
    sky_thread_t *thread = current_thread();
    if (!thread) die("ExitThread without current guest thread");
    LOG("THREAD_EXIT handle=%#x", thread->handle);
    pthread_mutex_lock(&g_thread_lock);
    thread->exited = 1;
    pthread_cond_broadcast(&g_thread_cond);
    pthread_mutex_unlock(&g_thread_lock);
    pthread_exit(NULL);
}

static uint32_t thread_wait(uint64_t *regs) {
    uint64_t *out = (uint64_t *)regs[0];
    const uint32_t *handles = (const uint32_t *)regs[1];
    int count = (int)regs[2];
    int64_t timeout = (int64_t)regs[3];
    if (!out || !handles || count <= 0) return R_BAD_HANDLE;
    struct timespec deadline = {0};
    if (timeout > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000000000;
        deadline.tv_nsec += timeout % 1000000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    pthread_mutex_lock(&g_thread_lock);
    for (;;) {
        for (int index = 0; index < count; index++) {
            sky_thread_t *thread = find_thread(handles[index]);
            if (!thread) {
                pthread_mutex_unlock(&g_thread_lock);
                return R_BAD_HANDLE;
            }
            if (thread->exited) {
                if (thread->joining) continue;
                if (!thread->joined) {
                    thread->joining = 1;
                    pthread_t native = thread->thread;
                    pthread_mutex_unlock(&g_thread_lock);
                    if (pthread_join(native, NULL)) die("wait for guest thread cleanup");
                    pthread_mutex_lock(&g_thread_lock);
                    thread->joined = 1;
                    thread->joining = 0;
                    pthread_cond_broadcast(&g_thread_cond);
                }
                /* WaitThread completion permits the SDK to unmap the guest
                 * stack. It must therefore include native TLS destructors,
                 * not merely the initial ExitThread notification. */
                regs[0] = 0;
                regs[1] = (uint32_t)index;
                pthread_mutex_unlock(&g_thread_lock);
                return 0;
            }
        }
        if (timeout == 0) {
            pthread_mutex_unlock(&g_thread_lock);
            return 0xEA01;
        }
        int result = timeout < 0 ? pthread_cond_wait(&g_thread_cond, &g_thread_lock)
                                 : pthread_cond_timedwait(&g_thread_cond, &g_thread_lock, &deadline);
        if (result == ETIMEDOUT) {
            pthread_mutex_unlock(&g_thread_lock);
            return 0xEA01;
        }
        if (result) die("guest thread wait failed");
    }
}

static void sync_unlink(sky_thread_t *thread) {
    for (sky_thread_t **link = &g_wait_list; *link; link = &(*link)->wait_next) {
        if (*link == thread) {
            *link = thread->wait_next;
            break;
        }
    }
    thread->wait_next = NULL;
}

static void sync_wake(sky_thread_t *thread, int64_t result) {
    sync_unlink(thread);
    thread->wait_kind = 0;
    thread->woke = 1;
    thread->wait_result = result;
    pthread_cond_broadcast(&g_sync_cond);
}

static int64_t sync_block(sky_thread_t *thread, int kind, int64_t timeout) {
    thread->wait_kind = kind;
    thread->wait_sequence = ++g_wait_sequence;
    thread->woke = 0;
    thread->wait_next = g_wait_list;
    g_wait_list = thread;
    if (timeout == 0) {
        sync_unlink(thread);
        thread->wait_kind = 0;
        return R_TIMEOUT;
    }
    struct timespec deadline = {0};
    if (timeout > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout / 1000000000;
        deadline.tv_nsec += timeout % 1000000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }
    while (!thread->woke) {
        int result = timeout < 0 ? pthread_cond_wait(&g_sync_cond, &g_sync_lock)
                                 : pthread_cond_timedwait(&g_sync_cond, &g_sync_lock, &deadline);
        if (result == ETIMEDOUT && !thread->woke) {
            sync_unlink(thread);
            thread->wait_kind = 0;
            return R_TIMEOUT;
        }
        if (result && result != ETIMEDOUT) die("guest synchronization wait failed");
    }
    return thread->wait_result;
}

/* The Horizon wait trees select lower numeric priorities first. Their equal
 * priority comparator inserts new nodes after existing equals, so use the
 * latest queue insertion sequence as the tie breaker. Caller holds g_sync_lock. */
static sky_thread_t *sync_next_waiter(int kind, uint64_t key) {
    sky_thread_t *next = NULL;
    for (sky_thread_t *thread = g_wait_list; thread; thread = thread->wait_next) {
        if (thread->wait_kind != kind || (kind == 2 ? thread->wait_key : thread->wait_addr) != key) continue;
        if (!next || thread->priority < next->priority ||
            (thread->priority == next->priority && thread->wait_sequence < next->wait_sequence))
            next = thread;
    }
    return next;
}

static uint32_t sync_load32(uint64_t address) { return __atomic_load_n((uint32_t *)address, __ATOMIC_SEQ_CST); }
static void sync_store32(uint64_t address, uint32_t value) { __atomic_store_n((uint32_t *)address, value, __ATOMIC_SEQ_CST); }
static int sync_cas32(uint64_t address, uint32_t *expected, uint32_t value) {
    return __atomic_compare_exchange_n((uint32_t *)address, expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static int address_accessible(uint64_t address, size_t size, int writable) {
    if (!address || address > UINT64_MAX - size) return 0;
    sky_vm_address_t region = address;
    sky_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t result = sky_vm_region(mach_task_self(), &region, &region_size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    vm_prot_t required = VM_PROT_READ | (writable ? VM_PROT_WRITE : 0);
    return result == KERN_SUCCESS && region <= address &&
        size <= region_size && address - region <= region_size - size &&
        (info.protection & required) == required;
}

static uint32_t address_wait(uint64_t *regs) {
    uint64_t address = regs[0];
    unsigned type = (unsigned)regs[1];
    size_t size = type == 3 ? 8 : 4;
    if (address & (size - 1)) return R_INVALID_ADDRESS;
    if (type > 3) return R_INVALID_ENUM;
    if (!address_accessible(address, size, type == 1)) return R_INVALID_MEMORY;
    sky_thread_t *thread = current_thread();
    if (!thread) die("WaitForAddress without current guest thread");
    pthread_mutex_lock(&g_sync_lock);
    int should_wait;
    if (type == 3) {
        should_wait = __atomic_load_n((uint64_t *)address, __ATOMIC_SEQ_CST) == regs[2];
    } else {
        uint32_t value = sync_load32(address);
        if (type == 1) {
            while ((int32_t)value < (int32_t)regs[2] &&
                   !sync_cas32(address, &value, value - 1)) {}
        }
        should_wait = type == 2 ? value == (uint32_t)regs[2] : (int32_t)value < (int32_t)regs[2];
    }
    uint32_t result = R_INVALID_STATE;
    if (should_wait) {
        thread->wait_addr = address;
        thread->wait_key = 0;
        result = (uint32_t)sync_block(thread, 3, (int64_t)regs[3]);
    }
    pthread_mutex_unlock(&g_sync_lock);
    return result;
}

static uint32_t address_signal(uint64_t *regs) {
    uint64_t address = regs[0];
    unsigned type = (unsigned)regs[1];
    uint32_t value = (uint32_t)regs[2];
    int count = (int32_t)regs[3];
    if (address & 3) return R_INVALID_ADDRESS;
    if (type > 2) return R_INVALID_ENUM;
    if (type && !address_accessible(address, 4, 1)) return R_INVALID_MEMORY;
    pthread_mutex_lock(&g_sync_lock);
    int waiters = 0;
    for (sky_thread_t *thread = g_wait_list; thread; thread = thread->wait_next)
        if (thread->wait_kind == 3 && thread->wait_addr == address) waiters++;
    if (type) {
        uint32_t updated = value + 1;
        if (type == 2 && waiters) updated = count <= 0 || waiters <= count ? value - 1 : value;
        uint32_t expected = value;
        if (!sync_cas32(address, &expected, updated)) {
            pthread_mutex_unlock(&g_sync_lock);
            return R_INVALID_STATE;
        }
    }
    for (int woken = 0; woken < waiters && (count <= 0 || woken < count); woken++) {
        sky_thread_t *next = sync_next_waiter(3, address);
        if (next) sync_wake(next, 0);
    }
    pthread_mutex_unlock(&g_sync_lock);
    return 0;
}

static void lock_handoff(uint64_t address) {
    sky_thread_t *next = sync_next_waiter(1, address);
    if (!next) {
        sync_store32(address, 0);
        return;
    }
    int more = 0;
    for (sky_thread_t *thread = g_wait_list; thread; thread = thread->wait_next)
        if (thread != next && thread->wait_kind == 1 && thread->wait_addr == address) more = 1;
    sync_store32(address, next->wait_tag | (more ? WAITERS_BIT : 0));
    sync_wake(next, 0);
}

static void lock_acquire_for(sky_thread_t *thread, uint64_t address, uint32_t tag) {
    uint32_t value = sync_load32(address);
    for (;;) {
        if (!value) {
            if (sync_cas32(address, &value, tag)) {
                sync_wake(thread, 0);
                return;
            }
            continue;
        }
        if (sync_cas32(address, &value, value | WAITERS_BIT)) break;
    }
    thread->wait_addr = address;
    thread->wait_tag = tag;
    thread->wait_kind = 1;
    /* Signaling transfers a CV waiter into a new mutex wait-tree insertion. */
    thread->wait_sequence = ++g_wait_sequence;
}

static uint64_t get_info_value(uint64_t id) {
    memory_init();
    switch (id) {
    case 0: return 7;
    case 1: return 0x0ffffffff0000000ull;
    case 2: return (uint64_t)g_alias;
    case 3: return g_alias_cap;
    case 4: return (uint64_t)g_heap;
    case 5: return g_heap_cap;
    case 6: return REPORTED_MEMORY_TOTAL;
    case 7: return image_bytes() + g_heap_cur;
    case 8: return 0;
    case 11: {
        uint64_t value;
        arc4random_buf(&value, sizeof value);
        return value;
    }
    case 12: return (uint64_t)g_as;
    case 13: return RESERVE_SIZE;
    case 14: return (uint64_t)g_stack;
    case 15: return g_stack_cap;
    case 16: return 0x2000000;
    case 17: return 0x400000;
    case 18: return strtoull(sky_profile.title_id, NULL, 16);
    case 21: return REPORTED_MEMORY_TOTAL;
    case 22: return image_bytes() + g_heap_cur;
    case 23: return 1;
    default:
        LOG("UNIMPL GetInfo id=%#llx handler=get_info_value", (unsigned long long)id);
        LOG("RUN_STOP checkpoint=%s reason=UNIMPL_GETINFO_%#llx", sky_checkpoint_name(), (unsigned long long)id);
        abort();
    }
}

static void query_memory(uint64_t *regs) {
    memory_init();
    uint64_t output = regs[0], address = regs[2];
    uint8_t *pointer = (uint8_t *)address;
    uint64_t base, size, state, permission;
    int mapping = map_find(address, 1);
    if (pointer >= g_heap && pointer < g_heap + g_heap_cur) {
        base = (uint64_t)g_heap;
        size = g_heap_cur;
        state = 5;
        permission = 3;
    } else if (mapping) {
        base = g_map[mapping - 1].base;
        size = g_map[mapping - 1].size;
        state = g_map[mapping - 1].state;
        permission = 3;
    } else if (pointer >= g_as && pointer < g_as + RESERVE_SIZE) {
        uint64_t low = (uint64_t)g_as, high = (uint64_t)g_as + RESERVE_SIZE;
        if (pointer >= g_heap) low = (uint64_t)g_heap + g_heap_cur;
        else if (high > (uint64_t)g_heap) high = (uint64_t)g_heap;
        for (int index = 0; index < g_nmap; index++) {
            uint64_t end = g_map[index].base + g_map[index].size;
            if (end <= address && end > low) low = end;
            if (g_map[index].base > address && g_map[index].base < high) high = g_map[index].base;
        }
        base = low;
        size = high - low;
        state = 0;
        permission = 0;
    } else {
        uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self());
        uint64_t stack_size = pthread_get_stacksize_np(pthread_self());
        if (address < top && address >= top - stack_size) {
            base = top - stack_size;
            size = stack_size;
            state = 0xB;
            permission = 3;
        } else {
            base = address & ~0xFFFull;
            size = 0x1000;
            state = 3;
            permission = 3;
        }
    }
    if (output) {
        uint64_t *memory_info = (uint64_t *)output;
        memory_info[0] = base;
        memory_info[1] = size;
        ((uint32_t *)output)[4] = (uint32_t)state;
        ((uint32_t *)output)[5] = 0;
        ((uint32_t *)output)[6] = (uint32_t)permission;
        ((uint32_t *)output)[7] = 0;
        ((uint32_t *)output)[8] = 0;
    }
    if (getenv("SKY_SVC_LOG"))
        LOG("SVC QueryMemory addr=%#llx base=%#llx size=%#llx state=%#llx perm=%#llx", (unsigned long long)address,
            (unsigned long long)base, (unsigned long long)size, (unsigned long long)state,
            (unsigned long long)permission);
    regs[0] = 0;
    regs[1] = 0;
}

static uint64_t move_memory(int number, uint64_t *regs) {
    memory_init();
    uint8_t *destination = (uint8_t *)regs[0], *source = (uint8_t *)regs[1];
    size_t size = regs[2];
    if (!size || size % 0x1000 || (uintptr_t)destination % 0x1000 || (uintptr_t)source % 0x1000 ||
        destination < g_stack || destination + size > g_stack + g_stack_cap || source < g_alias ||
        source + size > g_alias + g_alias_cap || !map_find(regs[number == 0x04 ? 1 : 0], size))
        return 0xD401;
    uint64_t destination_low = regs[0] & ~0x3FFFull, destination_high = (regs[0] + size + 0x3FFF) & ~0x3FFFull;
    if (number == 0x04) {
        if (mprotect((void *)destination_low, destination_high - destination_low, PROT_READ | PROT_WRITE))
            die("MapMemory destination mprotect");
        memcpy(destination, source, size);
        if (!map_remove(regs[1], size)) die("MapMemory source registry mismatch");
        map_add(regs[0], size, 0xB);
    } else {
        uint64_t source_low = regs[1] & ~0x3FFFull, source_high = (regs[1] + size + 0x3FFF) & ~0x3FFFull;
        if (mprotect((void *)source_low, source_high - source_low, PROT_READ | PROT_WRITE))
            die("UnmapMemory source mprotect");
        memcpy(source, destination, size);
        if (!map_remove(regs[0], size)) die("UnmapMemory destination registry mismatch");
        map_add(regs[1], size, 2);
    }
    /* Apple 16K pages can contain other live 4K subregions, so copied ranges stay RW. */
    if (getenv("SKY_SVC_LOG")) LOG("SVC MoveMemory svc=%#x destination=%p source=%p size=%#zx", number, destination, source, size);
    return 0;
}

static uint32_t transfer_create(uint64_t *regs) {
    uint32_t *out = (uint32_t *)regs[0];
    uint8_t *source = (uint8_t *)regs[1];
    size_t size = regs[2];
    if (!out || !source || !size || size % 0x1000 || (uintptr_t)source % 0x1000 || regs[3] > 3)
        return 0xD401;
    vm_size_t copied = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)source, 1, (vm_address_t)source, &copied) != KERN_SUCCESS ||
        vm_read_overwrite(mach_task_self(), (vm_address_t)(source + size - 1), 1,
                          (vm_address_t)(source + size - 1), &copied) != KERN_SUCCESS)
        return 0xD401;
    for (int index = 0; index < MAX_TRANSFER; index++) {
        sky_transfer_t *transfer = &g_transfers[index];
        if (transfer->handle) continue;
        transfer->handle = TRANSFER_HANDLE_BASE + index;
        transfer->source = source;
        transfer->size = size;
        transfer->mapped = 0;
        transfer->mapped_size = 0;
        *out = transfer->handle;
        LOG("TRANSFER_CREATE handle=%#x address=%p size=%#zx perm=%#llx", transfer->handle, source, size,
            (unsigned long long)regs[3]);
        return 0;
    }
    return 0xD401;
}

static uint64_t shared_alloc(size_t size) {
    memory_init();
    uint64_t candidate = (uint64_t)g_alias;
    for (int pass = 0; pass < g_nmap + 2; pass++) {
        candidate = (candidate + 0x3FFF) & ~0x3FFFull;
        int mapping = map_find(candidate, size);
        if (!mapping) {
            if (candidate + size <= (uint64_t)(g_alias + g_alias_cap) && candidate + size >= candidate)
                return candidate;
            return 0;
        }
        candidate = (g_map[mapping - 1].base + g_map[mapping - 1].size + 0x3FFF) & ~0x3FFFull;
    }
    return 0;
}

static uint32_t transfer_map(uint64_t *regs) {
    uint64_t *out = (uint64_t *)regs[0];
    sky_transfer_t *transfer = find_transfer((uint32_t)regs[1]);
    size_t size = regs[2];
    if (!out || !transfer || transfer->mapped || !size || size != transfer->size || regs[3] > 3) return 0xD401;
    uint64_t target = shared_alloc(size);
    if (!target) return 0xD401;
    vm_prot_t protection = (regs[3] & 1 ? PROT_READ : 0) | (regs[3] & 2 ? PROT_WRITE : 0);
    sky_vm_address_t address = target;
    kern_return_t result = sky_vm_remap(mach_task_self(), &address, size, 0, VM_FLAGS_OVERWRITE,
                                         mach_task_self(), (sky_vm_address_t)transfer->source, FALSE,
                                         &protection, &protection, VM_INHERIT_SHARE);
    if (result != KERN_SUCCESS || address != target) {
        sky_vm_deallocate(mach_task_self(), target, size);
        return 0xD401;
    }
    transfer->mapped = target;
    transfer->mapped_size = size;
    map_add(target, size, 0xF);
    *out = target;
    LOG("TRANSFER_MAP handle=%#x target=%#llx size=%#zx perm=%#llx", transfer->handle,
        (unsigned long long)target, size, (unsigned long long)regs[3]);
    return 0;
}

static uint32_t transfer_unmap(uint64_t *regs) {
    sky_transfer_t *transfer = find_transfer((uint32_t)regs[0]);
    if (!transfer) return R_BAD_HANDLE;
    uint64_t address = regs[1];
    size_t size = regs[2];
    if (!address || address != transfer->mapped || size != transfer->mapped_size) return 0xD401;
    if (sky_vm_deallocate(mach_task_self(), address, size) != KERN_SUCCESS) return 0xD401;
    if (!map_remove(address, size)) die("UnmapTransferMemory registry mismatch");
    transfer->mapped = 0;
    transfer->mapped_size = 0;
    LOG("TRANSFER_UNMAP handle=%#x address=%#llx", transfer->handle, (unsigned long long)address);
    return 0;
}

static uint32_t transfer_close(uint32_t handle) {
    sky_transfer_t *transfer = find_transfer(handle);
    if (!transfer) return R_BAD_HANDLE;
    if (transfer->mapped) {
        if (sky_vm_deallocate(mach_task_self(), transfer->mapped, transfer->mapped_size) != KERN_SUCCESS)
            return 0xD401;
        if (!map_remove(transfer->mapped, transfer->mapped_size)) die("CloseTransferMemory registry mismatch");
    }
    LOG("TRANSFER_CLOSE handle=%#x", handle);
    memset(transfer, 0, sizeof *transfer);
    return 0;
}

static uint32_t output_debug_string(uint64_t *regs) {
    uint64_t address = regs[0], length = regs[1];
    if (!address || length > 0x100000) return 0xD401;
    char *text = calloc(1, length + 1);
    if (!text) die("allocate guest debug string");
    if (length) {
        vm_size_t copied = 0;
        if (vm_read_overwrite(mach_task_self(), address, length, (vm_address_t)text, &copied) != KERN_SUCCESS ||
            copied != length) {
            free(text);
            return 0xD401;
        }
    }
    LOG("GUEST_DEBUG %s", text);
    free(text);
    sky_checkpoint("post_nnMain_debug_output");
    LOG("POST_NNMAIN_CHECKPOINT debug_output length=%llu", (unsigned long long)length);
    return 0;
}

extern __attribute__((noreturn)) void sky_svc_unimplemented(int number, uint64_t original);

uint64_t svc_dispatch(int number, uint64_t *regs, uint64_t original) {
    sky_post_entry_service(number);
    switch (number) {
    case 0x04:
    case 0x05:
        regs[0] = (uint32_t)move_memory(number, regs);
        return regs[0];
    case 0x06:
        query_memory(regs);
        return regs[0];
    case 0x27:
        regs[0] = output_debug_string(regs);
        return regs[0];
    case 0x2C:
    case 0x2D: {
        memory_init();
        uint8_t *address = (uint8_t *)regs[0];
        size_t size = regs[1];
        if (!size || size % 0x1000 || (uintptr_t)address % 0x1000 || address < g_alias ||
            address + size > g_alias + g_alias_cap) {
            if (getenv("SKY_SVC_LOG")) LOG("SVC PhysicalMemory invalid svc=%#x address=%p size=%#zx", number, address, size);
            regs[0] = 0xD401;
            return regs[0];
        }
        if (number == 0x2D && !map_remove(regs[0], size)) {
            if (getenv("SKY_SVC_LOG")) LOG("SVC PhysicalMemory unmapped-range address=%p size=%#zx", address, size);
            regs[0] = 0xD401;
            return regs[0];
        }
        uint64_t low = regs[0] & ~0x3FFFull, high = (regs[0] + size + 0x3FFF) & ~0x3FFFull;
        if (number == 0x2C) {
            if (mprotect((void *)low, high - low, PROT_READ | PROT_WRITE)) die("MapPhysicalMemory mprotect");
            map_add(regs[0], size, 2);
        } else {
            /* Apple 16K pages can contain other live 4K subregions: only revoke
               host pages fully inside the guest range; edge pages stay RW. */
            uint64_t inner_low = (regs[0] + 0x3FFF) & ~0x3FFFull;
            uint64_t inner_high = (regs[0] + size) & ~0x3FFFull;
            if (inner_high > inner_low) {
                madvise((void *)inner_low, inner_high - inner_low, MADV_FREE);
                if (mprotect((void *)inner_low, inner_high - inner_low, PROT_NONE))
                    die("UnmapPhysicalMemory mprotect");
            }
        }
        if (getenv("SKY_SVC_LOG")) {
            module_t *m = module_of((const void *)regs[30]);
            LOG("SVC PhysicalMemory svc=%#x address=%p size=%#zx thr=%#x lr=%s+%#llx", number, address, size,
                sky_svc_current_thread_handle(), m ? m->name : "?",
                m ? (unsigned long long)(regs[30] - (uint64_t)m->base) : 0ull);
        }
        regs[0] = 0;
        return regs[0];
    }
    case 0x29:
        regs[1] = get_info_value(regs[1]);
        regs[0] = 0;
        return regs[0];
    case 0x08:
        return thread_create_guest(regs);
    case 0x09:
        regs[0] = thread_start(regs[0]);
        return regs[0];
    case 0x0A:
        thread_exit();
    case 0x0B: {
        int64_t nanoseconds = (int64_t)regs[0];
        if (nanoseconds > 0) {
            struct timespec duration = {nanoseconds / 1000000000, nanoseconds % 1000000000};
            nanosleep(&duration, NULL);
        } else {
            sched_yield();
        }
        regs[0] = 0;
        return regs[0];
    }
    case 0x0C: {
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = find_thread(regs[1]);
        regs[0] = thread ? 0 : R_BAD_HANDLE;
        if (thread) regs[1] = thread->priority;
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x0D: {
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = find_thread(regs[0]);
        if (thread) {
            /* Keep priority and queue insertion order coherent with selection.
             * Reinsert only after a real priority change; a no-op keeps its tie. */
            pthread_mutex_lock(&g_sync_lock);
            uint32_t priority = (uint32_t)regs[1];
            if (thread->priority != priority) {
                thread->priority = priority;
                if (thread->wait_kind) thread->wait_sequence = ++g_wait_sequence;
            }
            pthread_mutex_unlock(&g_sync_lock);
        }
        regs[0] = thread ? 0 : R_BAD_HANDLE;
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x0E: {
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = find_thread(regs[2]);
        regs[0] = thread ? 0 : R_BAD_HANDLE;
        if (thread) {
            regs[1] = thread->core;
            regs[2] = thread->mask;
        }
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x0F: {
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = find_thread(regs[0]);
        int32_t requested = (int32_t)regs[1];
        uint64_t mask = regs[2];
        if (!thread) regs[0] = R_BAD_HANDLE;
        else if (requested < -3 || requested > 2) regs[0] = 0xE201;
        else {
            if (requested == -2) { mask = 1; requested = 0; }
            if (!mask || (mask & ~7ull) || (requested >= 0 && !(mask & (1u << requested)))) {
                regs[0] = 0xE801; /* InvalidCombination */
            } else {
                uint32_t core = requested >= 0 ? (uint32_t)requested : thread->core;
                if (!(mask & (1u << core))) core = (uint32_t)__builtin_ctz((unsigned)mask);
                thread->core = core;
                thread->mask = (uint32_t)mask;
                regs[0] = 0;
            }
        }
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x10: {
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = current_thread();
        regs[0] = thread ? thread->core : R_BAD_HANDLE;
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x13:
        regs[0] = (uint32_t)transfer_map(regs);
        return regs[0];
    case 0x14:
        regs[0] = (uint32_t)transfer_unmap(regs);
        return regs[0];
    case 0x15:
        regs[0] = (uint32_t)transfer_create(regs);
        return regs[0];
    case 0x16: {
        if (find_transfer((uint32_t)regs[0])) {
            regs[0] = transfer_close((uint32_t)regs[0]);
            return regs[0];
        }
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = (uint32_t)regs[0] == CURRENT_THREAD_HANDLE ? NULL : find_thread(regs[0]);
        if (thread) thread->closed = 1;
        regs[0] = thread ? 0 : R_BAD_HANDLE;
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x18: {
        uint32_t result = thread_wait(regs);
        regs[0] = result;
        return result;
    }
    case 0x1A: {
        sky_thread_t *thread = current_thread();
        if (!thread) die("ArbitrateLock without current guest thread");
        pthread_mutex_lock(&g_sync_lock);
        if (sync_load32(regs[1]) == ((uint32_t)regs[0] | WAITERS_BIT)) {
            thread->wait_addr = regs[1];
            thread->wait_tag = (uint32_t)regs[2];
            regs[0] = sync_block(thread, 1, -1);
        } else {
            regs[0] = 0;
        }
        pthread_mutex_unlock(&g_sync_lock);
        return regs[0];
    }
    case 0x1B:
        pthread_mutex_lock(&g_sync_lock);
        lock_handoff(regs[0]);
        pthread_mutex_unlock(&g_sync_lock);
        regs[0] = 0;
        return regs[0];
    case 0x1C: {
        sky_thread_t *thread = current_thread();
        if (!thread) die("WaitProcessWideKeyAtomic without current guest thread");
        pthread_mutex_lock(&g_sync_lock);
        sync_store32(regs[1], 1);
        thread->wait_key = regs[1];
        thread->wait_addr = regs[0];
        thread->wait_tag = (uint32_t)regs[2];
        lock_handoff(regs[0]);
        int64_t result = sync_block(thread, 2, (int64_t)regs[3]);
        pthread_mutex_unlock(&g_sync_lock);
        regs[0] = (uint32_t)result;
        return regs[0];
    }
    case 0x1D: {
        uint64_t key = regs[0];
        int remaining = (int)regs[1], woken = 0;
        pthread_mutex_lock(&g_sync_lock);
        while (remaining <= 0 || woken < remaining) {
            sky_thread_t *thread = sync_next_waiter(2, key);
            if (!thread) break;
            lock_acquire_for(thread, thread->wait_addr, thread->wait_tag);
            woken++;
        }
        int left = 0;
        for (sky_thread_t *thread = g_wait_list; thread; thread = thread->wait_next)
            if (thread->wait_kind == 2 && thread->wait_key == key) left = 1;
        if (!left) sync_store32(key, 0);
        pthread_mutex_unlock(&g_sync_lock);
        regs[0] = 0;
        return regs[0];
    }
    case 0x25: {
        pthread_mutex_lock(&g_thread_lock);
        sky_thread_t *thread = find_thread(regs[1]);
        regs[0] = thread ? 0 : R_BAD_HANDLE;
        if (thread) regs[1] = thread->id;
        pthread_mutex_unlock(&g_thread_lock);
        return regs[0];
    }
    case 0x34:
        regs[0] = address_wait(regs);
        return regs[0];
    case 0x35:
        regs[0] = address_signal(regs);
        return regs[0];
    default: {
        extern void crash_backtrace(uint64_t frame_pointer);
        LOG("UNIMPL_CONTEXT svc=%#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", number,
            (unsigned long long)regs[0], (unsigned long long)regs[1],
            (unsigned long long)regs[2], (unsigned long long)regs[3]);
        crash_backtrace(regs[29]);
        sky_svc_unimplemented(number, original);
    }
    }
}
