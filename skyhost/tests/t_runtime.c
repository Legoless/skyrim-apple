#include "skyhost.h"
#include "svc.h"
#include "thread.h"
#include <assert.h>
#include <sched.h>
#include <time.h>

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

static uint64_t g_test_thread_argument;
static uint32_t g_test_lock, g_test_key;
static uint32_t g_test_busy_lock, g_test_busy_key, g_test_busy_stage;

static pthread_key_t g_exit_cleanup_key;
typedef struct { uint64_t id, core; uint32_t cleanup_done; } sequential_thread_result_t;
static void delayed_exit_cleanup(void *argument) {
    sequential_thread_result_t *result = argument;
    const struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
    __atomic_store_n(&result->cleanup_done, 1, __ATOMIC_SEQ_CST);
}
static void sequential_thread_entry(uint64_t argument) {
    sequential_thread_result_t *result = (sequential_thread_result_t *)argument;
    assert(!pthread_setspecific(g_exit_cleanup_key, result));
    uint64_t regs[31] = {0};
    svc_dispatch(0x10, regs, 0);
    result->core = regs[0];
    assert(result->core < 3);
    regs[1] = 0xffff8000u;
    svc_dispatch(0x25, regs, 0);
    assert(regs[0] == 0);
    result->id = regs[1];
    regs[1] = 0xffff8000u;
    svc_dispatch(0x25, regs, 0);
    assert(regs[0] == 0 && regs[1] == result->id);
    svc_dispatch(0x0A, regs, 0);
}

static void test_thread_handle_lifecycle(void) {
    assert(!pthread_key_create(&g_exit_cleanup_key, delayed_exit_cleanup));
    uint8_t *stack = malloc(0x10000);
    assert(stack);
    uint64_t previous_id = 0;
    uint32_t previous_handle = 0;
    for (unsigned iteration = 0; iteration < 128; iteration++) {
        sequential_thread_result_t result = {0};
        uint64_t regs[31] = {0};
        regs[1] = (uint64_t)sequential_thread_entry;
        regs[2] = (uint64_t)&result;
        regs[3] = (uint64_t)stack + 0x10000;
        regs[4] = 32;
        regs[5] = iteration & 1 ? (uint64_t)-1 : (uint64_t)-2;
        svc_dispatch(0x08, regs, 0);
        assert(regs[0] == 0);
        uint32_t handle = (uint32_t)regs[1];
        assert(handle > previous_handle);
        regs[0] = 0;
        regs[1] = handle;
        svc_dispatch(0x25, regs, 0);
        assert(regs[0] == 0 && regs[1] > previous_id);
        uint64_t assigned_id = regs[1];
        regs[2] = handle;
        svc_dispatch(0x0E, regs, 0);
        assert(regs[0] == 0 && regs[1] == 0 && regs[2] == (iteration & 1 ? 7u : 1u));
        regs[0] = handle;
        svc_dispatch(0x09, regs, 0);
        assert(regs[0] == 0);
        uint32_t index = UINT32_MAX;
        regs[0] = (uint64_t)&index;
        regs[1] = (uint64_t)&handle;
        regs[2] = 1;
        regs[3] = 1000000000;
        svc_dispatch(0x18, regs, 0);
        assert(regs[0] == 0 && regs[1] == 0);
        assert(result.id == assigned_id && result.core < 3);
        assert(__atomic_load_n(&result.cleanup_done, __ATOMIC_SEQ_CST) == 1);
        regs[0] = handle;
        svc_dispatch(0x16, regs, 0);
        assert(regs[0] == 0);
        regs[1] = handle;
        svc_dispatch(0x25, regs, 0);
        assert(regs[0] == 0xe401);
        previous_handle = handle;
        previous_id = assigned_id;
    }
    assert(!pthread_key_delete(g_exit_cleanup_key));
    puts("T_THREAD_LIFECYCLE OK current-pseudohandle unique-identities normalized-core 128-sequential-threads");
}

typedef struct {
    uint32_t *address;
    uint32_t type, value, result, completed;
} address_job_t;

static uint32_t address_svc(int number, const void *address, uint32_t type, uint64_t value, int64_t last) {
    uint64_t regs[31] = {(uint64_t)address, type, value, (uint64_t)last};
    svc_dispatch(number, regs, 0);
    return (uint32_t)regs[0];
}

static void address_guest_entry(uint64_t argument) {
    address_job_t *job = (address_job_t *)argument;
    job->result = address_svc(0x34, job->address, job->type, job->value, -1);
    __atomic_store_n(&job->completed, 1, __ATOMIC_SEQ_CST);
    uint64_t regs[31] = {0};
    svc_dispatch(0x0A, regs, 0);
}

static uint32_t start_address_job_at_priority(address_job_t *job, uint32_t priority, uint8_t **out_stack) {
    uint8_t *stack = malloc(0x10000);
    assert(stack);
    if (out_stack) *out_stack = stack;
    uint64_t regs[31] = {0};
    regs[1] = (uint64_t)address_guest_entry;
    regs[2] = (uint64_t)job;
    regs[3] = (uint64_t)stack + 0x10000;
    regs[4] = priority;
    regs[5] = 1;
    svc_dispatch(0x08, regs, 0);
    assert(regs[0] == 0);
    uint32_t handle = (uint32_t)regs[1];
    regs[0] = handle;
    svc_dispatch(0x09, regs, 0);
    assert(regs[0] == 0);
    return handle;
}

static uint32_t start_address_job(address_job_t *job) {
    return start_address_job_at_priority(job, 32, NULL);
}

static void join_address_job(uint32_t handle, address_job_t *job) {
    uint32_t index = UINT32_MAX;
    uint64_t regs[31] = {(uint64_t)&index, (uint64_t)&handle, 1, 1000000000};
    svc_dispatch(0x18, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0);
    assert(__atomic_load_n(&job->completed, __ATOMIC_SEQ_CST) == 1 && job->result == 0);
    regs[0] = handle;
    svc_dispatch(0x16, regs, 0);
    assert(regs[0] == 0);
}

static void await_word(uint32_t *word, uint32_t expected) {
    uint64_t deadline = now_ns() + 2000000000;
    while (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected && now_ns() < deadline) sched_yield();
    assert(__atomic_load_n(word, __ATOMIC_SEQ_CST) == expected);
}

#define TEST_WAITERS_BIT 0x40000000u
enum { ORDER_A, ORDER_B, ORDER_C, ORDER_D, ORDER_COUNT };

typedef struct {
    uint32_t *mutex, *key;
    uint32_t handle, acquired, permit, released;
    uint8_t *stack;
} ordered_sync_job_t;

static void signal_cv(uint32_t *key, int count) {
    uint64_t regs[31] = {(uint64_t)key, (uint64_t)count};
    svc_dispatch(0x1D, regs, 0);
    assert(regs[0] == 0);
}

static void unlock_mutex(uint32_t *mutex) {
    uint64_t regs[31] = {(uint64_t)mutex};
    svc_dispatch(0x1B, regs, 0);
    assert(regs[0] == 0);
}

static void ordered_sync_entry(uint64_t argument) {
    ordered_sync_job_t *job = (ordered_sync_job_t *)argument;
    uint64_t regs[31] = {(uint64_t)job->mutex, (uint64_t)job->key, job->handle, (uint64_t)-1};
    svc_dispatch(0x1C, regs, 0);
    assert(regs[0] == 0);
    assert((__atomic_load_n(job->mutex, __ATOMIC_SEQ_CST) & ~TEST_WAITERS_BIT) == job->handle);
    __atomic_store_n(&job->acquired, 1, __ATOMIC_SEQ_CST);
    /* Retain ownership until the parent has inspected the selected owner tag.
     * This makes the assertions independent of native thread completion order. */
    await_word(&job->permit, 1);
    assert((__atomic_load_n(job->mutex, __ATOMIC_SEQ_CST) & ~TEST_WAITERS_BIT) == job->handle);
    unlock_mutex(job->mutex);
    __atomic_store_n(&job->released, 1, __ATOMIC_SEQ_CST);
    svc_dispatch(0x0A, regs, 0);
}

static void create_ordered_sync_job(ordered_sync_job_t *job, uint32_t *mutex,
                                    uint32_t *key, uint32_t priority) {
    *job = (ordered_sync_job_t){.mutex = mutex, .key = key, .stack = malloc(0x10000)};
    assert(job->stack);
    uint64_t regs[31] = {0};
    regs[1] = (uint64_t)ordered_sync_entry;
    regs[2] = (uint64_t)job;
    regs[3] = (uint64_t)job->stack + 0x10000;
    regs[4] = priority;
    regs[5] = 1;
    svc_dispatch(0x08, regs, 0);
    assert(regs[0] == 0);
    job->handle = (uint32_t)regs[1];
    assert(job->handle && !(job->handle & TEST_WAITERS_BIT));
}

static void queue_ordered_sync_job(ordered_sync_job_t *job) {
    assert(__atomic_load_n(job->mutex, __ATOMIC_SEQ_CST) == 0);
    __atomic_store_n(job->mutex, job->handle, __ATOMIC_SEQ_CST);
    uint64_t regs[31] = {job->handle};
    svc_dispatch(0x09, regs, 0);
    assert(regs[0] == 0);
    await_word(job->mutex, 0);
    /* WaitProcessWideKeyAtomic releases this mutex before registering the CV
     * waiter, while holding the synchronization lock throughout. An unrelated
     * signal acquires that same lock and proves registration has completed. */
    uint32_t unrelated_key = 0;
    signal_cv(&unrelated_key, 1);
    assert(unrelated_key == 0);
}

static void set_guest_priority(uint32_t handle, uint32_t priority) {
    uint64_t regs[31] = {handle, priority};
    svc_dispatch(0x0D, regs, 0);
    assert(regs[0] == 0);
    regs[1] = handle;
    svc_dispatch(0x0C, regs, 0);
    assert(regs[0] == 0 && regs[1] == priority);
}

static void change_queued_priority(ordered_sync_job_t jobs[ORDER_COUNT], int changed) {
    /* A no-op must preserve C's place before A. An actual change away and back
     * must move C behind A when both again have priority 44. */
    set_guest_priority(jobs[ORDER_C].handle, 44);
    if (changed) {
        set_guest_priority(jobs[ORDER_C].handle, 40);
        set_guest_priority(jobs[ORDER_C].handle, 44);
    }
}

static void join_ordered_sync_jobs(ordered_sync_job_t jobs[ORDER_COUNT]) {
    for (int index = 0; index < ORDER_COUNT; index++) {
        ordered_sync_job_t *job = &jobs[index];
        uint32_t waited = UINT32_MAX;
        uint64_t regs[31] = {(uint64_t)&waited, (uint64_t)&job->handle, 1, 1000000000};
        svc_dispatch(0x18, regs, 0);
        assert(regs[0] == 0 && regs[1] == 0);
        assert(__atomic_load_n(&job->acquired, __ATOMIC_SEQ_CST) == 1);
        assert(__atomic_load_n(&job->released, __ATOMIC_SEQ_CST) == 1);
        regs[0] = job->handle;
        svc_dispatch(0x16, regs, 0);
        assert(regs[0] == 0);
        free(job->stack);
    }
}

static void test_cv_waiter_order(int changed) {
    uint32_t mutexes[ORDER_COUNT] = {0}, key = 0;
    const uint32_t priorities[ORDER_COUNT] = {44, 28, 44, 32};
    const int insertion[ORDER_COUNT] = {ORDER_C, ORDER_B, ORDER_A, ORDER_D};
    const int unchanged_order[ORDER_COUNT] = {ORDER_B, ORDER_D, ORDER_C, ORDER_A};
    const int changed_order[ORDER_COUNT] = {ORDER_B, ORDER_D, ORDER_A, ORDER_C};
    const int *order = changed ? changed_order : unchanged_order;
    ordered_sync_job_t jobs[ORDER_COUNT];
    for (int index = 0; index < ORDER_COUNT; index++)
        create_ordered_sync_job(&jobs[index], &mutexes[index], &key, priorities[index]);
    for (int index = 0; index < ORDER_COUNT; index++) queue_ordered_sync_job(&jobs[insertion[index]]);
    assert(key == 1);
    change_queued_priority(jobs, changed);
    uint32_t selected = 0;
    for (int step = 0; step < ORDER_COUNT; step++) {
        signal_cv(&key, 1);
        selected |= 1u << order[step];
        /* Each selected waiter owns its separate mutex before the SVC returns.
         * None may release it yet, so these words identify the exact selection. */
        for (int index = 0; index < ORDER_COUNT; index++)
            assert(__atomic_load_n(&mutexes[index], __ATOMIC_SEQ_CST) ==
                   (selected & (1u << index) ? jobs[index].handle : 0));
        assert(key == (step + 1 == ORDER_COUNT ? 0u : 1u));
    }
    for (int index = 0; index < ORDER_COUNT; index++)
        __atomic_store_n(&jobs[index].permit, 1, __ATOMIC_SEQ_CST);
    join_ordered_sync_jobs(jobs);
    for (int index = 0; index < ORDER_COUNT; index++) assert(mutexes[index] == 0);
    puts(changed ? "T_CV_ORDER OK priority-change reinserts-equal-tail B,D,A,C" :
                   "T_CV_ORDER OK priority FIFO no-op-preserves-place B,D,C,A");
}

static void test_mutex_waiter_order(int changed) {
    uint32_t mutex = 0, keys[ORDER_COUNT] = {0};
    const uint32_t priorities[ORDER_COUNT] = {44, 28, 44, 32};
    const int transfer[ORDER_COUNT] = {ORDER_C, ORDER_B, ORDER_A, ORDER_D};
    const int unchanged_order[ORDER_COUNT] = {ORDER_B, ORDER_D, ORDER_C, ORDER_A};
    const int changed_order[ORDER_COUNT] = {ORDER_B, ORDER_D, ORDER_A, ORDER_C};
    const int *order = changed ? changed_order : unchanged_order;
    ordered_sync_job_t jobs[ORDER_COUNT];
    for (int index = 0; index < ORDER_COUNT; index++)
        create_ordered_sync_job(&jobs[index], &mutex, &keys[index], priorities[index]);
    /* The original CV order is A,B,C,D. Requeueing C before A below must assign
     * fresh mutex insertion order, rather than retain their original CV order. */
    for (int index = 0; index < ORDER_COUNT; index++) queue_ordered_sync_job(&jobs[index]);
    uint32_t parent = sky_svc_main_thread_handle();
    __atomic_store_n(&mutex, parent, __ATOMIC_SEQ_CST);
    for (int index = 0; index < ORDER_COUNT; index++) {
        signal_cv(&keys[transfer[index]], 1);
        assert(keys[transfer[index]] == 0);
        assert(__atomic_load_n(&mutex, __ATOMIC_SEQ_CST) == (parent | TEST_WAITERS_BIT));
        for (int waiter = 0; waiter < ORDER_COUNT; waiter++)
            assert(__atomic_load_n(&jobs[waiter].acquired, __ATOMIC_SEQ_CST) == 0);
    }
    change_queued_priority(jobs, changed);
    unlock_mutex(&mutex);
    for (int step = 0; step < ORDER_COUNT; step++) {
        ordered_sync_job_t *job = &jobs[order[step]];
        uint32_t expected = job->handle | (step + 1 < ORDER_COUNT ? TEST_WAITERS_BIT : 0);
        assert(__atomic_load_n(&mutex, __ATOMIC_SEQ_CST) == expected);
        await_word(&job->acquired, 1);
        assert(__atomic_load_n(&job->released, __ATOMIC_SEQ_CST) == 0);
        __atomic_store_n(&job->permit, 1, __ATOMIC_SEQ_CST);
        await_word(&job->released, 1);
    }
    assert(__atomic_load_n(&mutex, __ATOMIC_SEQ_CST) == 0);
    join_ordered_sync_jobs(jobs);
    puts(changed ? "T_MUTEX_ORDER OK queued-priority-change reinserts-equal-tail B,D,A,C" :
                   "T_MUTEX_ORDER OK priority FIFO CV-transfer-order owner-tags no-op-preserves-place B,D,C,A");
}

static void test_address_arbitration(void) {
    uint32_t word = 7;
    assert(address_svc(0x34, &word, 2, 6, 0) == 0xFA01);
    assert(address_svc(0x34, &word, 2, 7, 0) == 0xEA01);
    assert(address_svc(0x34, &word, 2, 7, 1000000) == 0xEA01);
    assert(address_svc(0x34, &word, 0, 7, 0) == 0xFA01);
    word = UINT32_MAX;
    assert(address_svc(0x34, &word, 0, 0, 0) == 0xEA01);
    assert(address_svc(0x34, &word, 1, 0, 0) == 0xEA01 && word == UINT32_MAX - 1);
    assert(address_svc(0x34, &word, 7, 0, 0) == 0xF001);
    assert(address_svc(0x34, (uint8_t *)&word + 1, 2, 0, 0) == 0xCC01);
    assert(address_svc(0x34, NULL, 2, 0, 0) == 0xD401);
    uint64_t wide = 0x123456789abcdef0;
    assert(address_svc(0x34, &wide, 3, wide - 1, 0) == 0xFA01);
    assert(address_svc(0x34, &wide, 3, wide, 0) == 0xEA01);
    word = 4;
    assert(address_svc(0x35, &word, 1, 3, 1) == 0xFA01 && word == 4);
    assert(address_svc(0x35, &word, 1, 4, 1) == 0 && word == 5);
    assert(address_svc(0x35, &word, 2, 5, 1) == 0 && word == 6);
    assert(address_svc(0x35, &word, 7, 0, 1) == 0xF001);
    assert(address_svc(0x35, (uint8_t *)&word + 1, 0, 0, 1) == 0xCC01);

    word = 3;
    address_job_t jobs[2] = {{&word, 1, 100, 0, 0}, {&word, 1, 100, 0, 0}};
    uint32_t handles[2] = {start_address_job(&jobs[0]), start_address_job(&jobs[1])};
    /* Decrement happens under the same lock as registration, so the final
     * decrement proves both waiters are queued before a signal can acquire it. */
    await_word(&word, 1);
    assert(address_svc(0x35, &word, 2, 1, 1) == 0 && word == 1);
    uint64_t deadline = now_ns() + 2000000000;
    unsigned completed = 0;
    do {
        completed = __atomic_load_n(&jobs[0].completed, __ATOMIC_SEQ_CST) +
                    __atomic_load_n(&jobs[1].completed, __ATOMIC_SEQ_CST);
        sched_yield();
    } while (!completed && now_ns() < deadline);
    assert(completed == 1);
    uint32_t other_word = 1;
    assert(address_svc(0x35, &other_word, 0, 0, 0) == 0);
    assert(address_svc(0x35, &word, 2, 1, 1) == 0 && word == 0);
    for (int index = 0; index < 2; index++) join_address_job(handles[index], &jobs[index]);

    word = 3;
    memset(jobs, 0, sizeof jobs);
    for (int index = 0; index < 2; index++) {
        jobs[index].address = &word;
        jobs[index].type = 1;
        jobs[index].value = 100;
        handles[index] = start_address_job(&jobs[index]);
    }
    await_word(&word, 1);
    assert(address_svc(0x35, &word, 0, 0, 0) == 0 && word == 1);
    for (int index = 0; index < 2; index++) join_address_job(handles[index], &jobs[index]);

    word = 0;
    address_job_t one = {&word, 1, 1, 0, 0};
    uint32_t handle = start_address_job(&one);
    await_word(&word, UINT32_MAX);
    assert(address_svc(0x35, &word, 1, UINT32_MAX, 1) == 0 && word == 0);
    join_address_job(handle, &one);
    puts("T_ADDRESS_ARBITRATION OK predicates-timeout-decrement-signal-count-broadcast-atomic-update");
}

static void test_address_waiter_order(int changed) {
    uint32_t word = 3, unrelated_word = 0;
    address_job_t jobs[2] = {{&word, 1, 100, 0, 0}, {&word, 1, 100, 0, 0}};
    uint8_t *stacks[2];
    uint32_t handles[2];
    handles[0] = start_address_job_at_priority(&jobs[0], changed ? 28 : 44, &stacks[0]);
    await_word(&word, 2);
    /* The decrement and queue insertion hold the same synchronization lock.
     * This unrelated signal is a registration barrier after each decrement. */
    assert(address_svc(0x35, &unrelated_word, 0, 0, 0) == 0);
    handles[1] = start_address_job_at_priority(&jobs[1], 44, &stacks[1]);
    await_word(&word, 1);
    assert(address_svc(0x35, &unrelated_word, 0, 0, 0) == 0);
    set_guest_priority(handles[0], 44);
    int first = changed ? 1 : 0;
    assert(address_svc(0x35, &word, 0, 0, 1) == 0);
    /* Signal only one waiter before observing its return. There is no native
     * completion-order comparison between simultaneously awakened workers. */
    await_word(&jobs[first].completed, 1);
    assert(__atomic_load_n(&jobs[1 - first].completed, __ATOMIC_SEQ_CST) == 0);
    assert(address_svc(0x35, &word, 0, 0, 1) == 0);
    for (int index = 0; index < 2; index++) {
        join_address_job(handles[index], &jobs[index]);
        free(stacks[index]);
    }
    puts(changed ? "T_ADDRESS_ORDER OK queued-priority-change reinserts-equal-tail B,A" :
                   "T_ADDRESS_ORDER OK no-op-priority preserves-FIFO A,B");
}

static void test_guest_entry(uint64_t argument) {
    g_test_thread_argument = argument;
    uint64_t regs[31] = {0};
    regs[0] = (uint64_t)&g_test_lock;
    regs[1] = (uint64_t)&g_test_key;
    regs[2] = 0x1002;
    regs[3] = -1;
    svc_dispatch(0x1C, regs, 0);
    assert(regs[0] == 0);
    g_test_thread_argument++;
    svc_dispatch(0x0A, regs, 0);
}

static void test_busy_guest_entry(uint64_t argument) {
    (void)argument;
    uint64_t regs[31] = {0};
    regs[0] = (uint64_t)&g_test_busy_lock;
    regs[1] = (uint64_t)&g_test_busy_key;
    regs[2] = 0x2003;
    regs[3] = -1;
    __atomic_store_n(&g_test_busy_stage, 1, __ATOMIC_SEQ_CST);
    svc_dispatch(0x1C, regs, 0);
    assert(regs[0] == 0);
    __atomic_store_n(&g_test_busy_stage, 2, __ATOMIC_SEQ_CST);
    svc_dispatch(0x0A, regs, 0);
}

static void create_keys(void) {
    assert(!pthread_key_create(&g_key18, NULL));
    assert(!pthread_key_create(&g_keytls, NULL));
    assert(!pthread_key_create(&g_keytlr, NULL));
    assert(!pthread_key_create(&g_keygate, NULL));
    assert(!pthread_key_create(&g_keyres[0], NULL));
    assert(!pthread_key_create(&g_keyres[1], NULL));
    assert(!pthread_key_create(&g_keyaspect, NULL));
    assert(!pthread_key_create(&g_keyaspectui, NULL));
}

int main(void) {
    create_keys();
    guest_thread_init();
    uint8_t *tls = pthread_getspecific(g_keytls);
    uint8_t *tlr = pthread_getspecific(g_keytlr);
    assert(tls && tlr && pthread_getspecific(g_keygate));
    assert(*(void **)(tlr + 0x1f8) == tlr + 0x200);
    assert(sky_svc_main_thread_init() == 0x1001);
    assert(*(uint32_t *)(tlr + 0x200 + 0x1b0) == 0x1001);

    uint64_t self_regs[31] = {0};
    self_regs[2] = 0xffff8000u;
    svc_dispatch(0x0E, self_regs, 0);
    assert(self_regs[0] == 0 && self_regs[1] == 0 && self_regs[2] == 1);
    for (int iteration = 0; iteration < 8; iteration++) {
        self_regs[1] = 0xffff8000u;
        svc_dispatch(0x25, self_regs, 0);
        assert(self_regs[0] == 0 && self_regs[1] == 0x51);
    }
    uint32_t self_handle = 0xffff8000u, waited_self = UINT32_MAX;
    self_regs[0] = (uint64_t)&waited_self;
    self_regs[1] = (uint64_t)&self_handle;
    self_regs[2] = 1;
    self_regs[3] = 0;
    svc_dispatch(0x18, self_regs, 0);
    assert(self_regs[0] == 0xea01);
    self_regs[0] = self_handle;
    svc_dispatch(0x16, self_regs, 0);
    assert(self_regs[0] == 0xe401);

    uint64_t regs[31] = {0};
    regs[1] = 0x15;
    svc_dispatch(0x29, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0x100000000ull);
    memset(regs, 0, sizeof regs);
    regs[1] = 1;
    svc_dispatch(0x29, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0x0ffffffff0000000ull);
    memset(regs, 0, sizeof regs);
    regs[1] = 0x16;
    svc_dispatch(0x29, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0);
    memset(regs, 0, sizeof regs);
    regs[1] = 0x10;
    svc_dispatch(0x29, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0x2000000);
    memset(regs, 0, sizeof regs);
    regs[1] = 3;
    svc_dispatch(0x29, regs, 0);
    assert(regs[0] == 0 && regs[1] >= 0x40200000);
    memset(regs, 0, sizeof regs);
    regs[1] = 4;
    svc_dispatch(0x29, regs, 0);
    uint8_t *heap = (uint8_t *)regs[1];

    uint8_t *argument = calloc(1, 4096);
    uint8_t memory_info[40] = {0};
    assert(argument);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)argument;
    svc_dispatch(0x06, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0);
    assert(*(uint64_t *)memory_info == ((uintptr_t)argument & ~0xFFFull));
    assert(*(uint32_t *)(memory_info + 0x10) == 3);
    assert(*(uint32_t *)(memory_info + 0x18) == 3);

    memset(regs, 0, sizeof regs);
    regs[1] = 2;
    svc_dispatch(0x29, regs, 0);
    uint8_t *alias = (uint8_t *)regs[1];
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)alias;
    regs[1] = 0x1000;
    svc_dispatch(0x2C, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)alias;
    regs[1] = 0x1000;
    svc_dispatch(0x2C, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)(alias + 0x4000);
    regs[1] = 0x1000;
    svc_dispatch(0x2C, regs, 0);
    assert(regs[0] == 0);
    alias[0] = 0x5A;
    alias[0x4000] = 0xA5;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)(alias + 0x2000);
    svc_dispatch(0x06, regs, 0);
    assert(*(uint64_t *)memory_info == (uintptr_t)(alias + 0x1000));
    assert(*(uint64_t *)(memory_info + 8) == 0x3000);
    assert(*(uint32_t *)(memory_info + 0x10) == 0);
    assert(*(uint32_t *)(memory_info + 0x18) == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)(alias + 0x1000);
    regs[1] = 0x4000;
    svc_dispatch(0x2C, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)(alias + 0x2000);
    svc_dispatch(0x06, regs, 0);
    assert(*(uint64_t *)memory_info == (uintptr_t)alias);
    assert(*(uint64_t *)(memory_info + 8) == 0x5000);
    assert(*(uint32_t *)(memory_info + 0x10) == 2);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)(alias + 0x2000);
    regs[1] = 0x1000;
    svc_dispatch(0x2D, regs, 0);
    assert(regs[0] == 0);
    /* Unmapping a 4K subrange must not revoke live neighbors sharing its 16K host page. */
    alias[0x1000] = 0x3C;
    alias[0x3000] = 0xC3;
    assert(alias[0] == 0x5A && alias[0x1000] == 0x3C && alias[0x3000] == 0xC3 && alias[0x4000] == 0xA5);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)(alias + 0x2000);
    svc_dispatch(0x06, regs, 0);
    assert(*(uint64_t *)memory_info == (uintptr_t)(alias + 0x2000));
    assert(*(uint64_t *)(memory_info + 8) == 0x1000);
    assert(*(uint32_t *)(memory_info + 0x10) == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)(alias + 0x1000);
    svc_dispatch(0x06, regs, 0);
    assert(*(uint64_t *)memory_info == (uintptr_t)alias);
    assert(*(uint64_t *)(memory_info + 8) == 0x2000);
    assert(*(uint32_t *)(memory_info + 0x10) == 2);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)(alias + 0x3000);
    svc_dispatch(0x06, regs, 0);
    assert(*(uint64_t *)memory_info == (uintptr_t)(alias + 0x3000));
    assert(*(uint64_t *)(memory_info + 8) == 0x2000);
    assert(*(uint32_t *)(memory_info + 0x10) == 2);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)alias;
    regs[1] = 0x2000;
    svc_dispatch(0x2D, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)(alias + 0x3000);
    regs[1] = 0x2000;
    svc_dispatch(0x2D, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)(alias + 0x2000);
    svc_dispatch(0x06, regs, 0);
    assert(*(uint64_t *)memory_info == (uintptr_t)(alias - 0x10000000));
    assert(*(uint64_t *)(memory_info + 8) == (uint64_t)(heap - (alias - 0x10000000)));
    assert(*(uint32_t *)(memory_info + 0x10) == 0);

    memset(regs, 0, sizeof regs);
    regs[1] = 14;
    svc_dispatch(0x29, regs, 0);
    uint8_t *stack = (uint8_t *)regs[1];
    uint8_t *source = alias + 0x10000;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)source;
    regs[1] = 0x1000;
    svc_dispatch(0x2C, regs, 0);
    assert(regs[0] == 0);
    source[0] = 0x6A;
    source[1] = 0x7B;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)stack;
    regs[1] = (uint64_t)source;
    regs[2] = 0x1000;
    svc_dispatch(0x04, regs, 0);
    assert(regs[0] == 0);
    assert(stack[0] == 0x6A && stack[1] == 0x7B);
    assert(source[0] == 0x6A && source[1] == 0x7B);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)source;
    svc_dispatch(0x06, regs, 0);
    assert(*(uint32_t *)(memory_info + 0x10) == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)stack;
    svc_dispatch(0x06, regs, 0);
    assert(*(uint32_t *)(memory_info + 0x10) == 0xB);
    stack[1] = 0x7C;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)stack;
    regs[1] = (uint64_t)source;
    regs[2] = 0x1000;
    svc_dispatch(0x05, regs, 0);
    assert(regs[0] == 0);
    assert(source[0] == 0x6A && source[1] == 0x7C);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)source;
    svc_dispatch(0x06, regs, 0);
    assert(*(uint32_t *)(memory_info + 0x10) == 2);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)memory_info;
    regs[2] = (uint64_t)stack;
    svc_dispatch(0x06, regs, 0);
    assert(*(uint32_t *)(memory_info + 0x10) == 0);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)source;
    regs[1] = 0x1000;
    svc_dispatch(0x2D, regs, 0);
    assert(regs[0] == 0);

    memset(regs, 0, sizeof regs);
    regs[0] = 0x1001;
    regs[1] = 32;
    svc_dispatch(0x0D, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[1] = 0x1001;
    svc_dispatch(0x0C, regs, 0);
    assert(regs[0] == 0 && regs[1] == 32);

    memset(regs, 0, sizeof regs);
    regs[0] = 0x1001;
    regs[1] = 2;
    regs[2] = 5;
    svc_dispatch(0x0F, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[2] = 0x1001;
    svc_dispatch(0x0E, regs, 0);
    assert(regs[0] == 0 && regs[1] == 2 && regs[2] == 5);
    regs[0] = 0xffff8000u;
    regs[1] = -1;
    regs[2] = 2;
    svc_dispatch(0x0F, regs, 0);
    assert(regs[0] == 0);
    svc_dispatch(0x10, regs, 0);
    assert(regs[0] == 1);
    regs[0] = 0xffff8000u;
    regs[1] = -2;
    regs[2] = 0;
    svc_dispatch(0x0F, regs, 0);
    assert(regs[0] == 0);
    svc_dispatch(0x10, regs, 0);
    assert(regs[0] == 0);
    regs[2] = 0xffff8000u;
    svc_dispatch(0x0E, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0 && regs[2] == 1);

    static const char debug_message[] = "synthetic-debug";
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)debug_message;
    regs[1] = sizeof debug_message - 1;
    svc_dispatch(0x27, regs, 0);
    assert(regs[0] == 0);
    memset(regs, 0, sizeof regs);
    regs[1] = 1;
    svc_dispatch(0x27, regs, 0);
    assert(regs[0] == 0xD401);

    size_t guest_stack_size = 0x10000;
    uint8_t *guest_stack = malloc(guest_stack_size);
    assert(guest_stack);
    g_test_thread_argument = 0;
    g_test_lock = 0;
    g_test_key = 0;
    memset(regs, 0, sizeof regs);
    regs[1] = (uint64_t)test_guest_entry;
    regs[2] = 0x12345678;
    regs[3] = (uint64_t)(guest_stack + guest_stack_size);
    regs[4] = 32;
    regs[5] = 1;
    svc_dispatch(0x08, regs, 0);
    uint32_t handle = (uint32_t)regs[1];
    assert(regs[0] == 0 && handle > 0x1001);
    memset(regs, 0, sizeof regs);
    regs[0] = handle;
    svc_dispatch(0x09, regs, 0);
    assert(regs[0] == 0);
    for (int spin = 0; spin < 1000000 && !g_test_key; spin++) sched_yield();
    assert(g_test_key == 1);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&g_test_key;
    regs[1] = 1;
    svc_dispatch(0x1D, regs, 0);
    assert(regs[0] == 0);
    uint32_t waited_index = 0xFFFFFFFF;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&waited_index;
    regs[1] = (uint64_t)&handle;
    regs[2] = 1;
    regs[3] = 1000000000;
    svc_dispatch(0x18, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0);
    assert(g_test_thread_argument == 0x12345679);
    memset(regs, 0, sizeof regs);
    regs[0] = handle;
    svc_dispatch(0x16, regs, 0);
    assert(regs[0] == 0);

    g_test_busy_lock = 0x1001;
    g_test_busy_key = 0;
    g_test_busy_stage = 0;
    memset(regs, 0, sizeof regs);
    regs[1] = (uint64_t)test_busy_guest_entry;
    regs[2] = 0;
    regs[3] = (uint64_t)(guest_stack + guest_stack_size);
    regs[4] = 32;
    regs[5] = 1;
    svc_dispatch(0x08, regs, 0);
    uint32_t busy_handle = (uint32_t)regs[1];
    assert(regs[0] == 0 && busy_handle > handle);
    memset(regs, 0, sizeof regs);
    regs[0] = busy_handle;
    svc_dispatch(0x09, regs, 0);
    assert(regs[0] == 0);
    for (int spin = 0; spin < 1000000 &&
         (__atomic_load_n(&g_test_busy_key, __ATOMIC_SEQ_CST) != 1 || g_test_busy_lock != 0); spin++) sched_yield();
    assert(g_test_busy_key == 1 && g_test_busy_lock == 0);
    assert(__atomic_load_n(&g_test_busy_stage, __ATOMIC_SEQ_CST) == 1);
    g_test_busy_lock = 0x1001;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&g_test_busy_key;
    regs[1] = 1;
    svc_dispatch(0x1D, regs, 0);
    assert(regs[0] == 0);
    assert(g_test_busy_key == 0);
    assert(g_test_busy_lock == 0x40001001);
    assert(__atomic_load_n(&g_test_busy_stage, __ATOMIC_SEQ_CST) == 1);
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&g_test_busy_lock;
    svc_dispatch(0x1B, regs, 0);
    assert(regs[0] == 0 && g_test_busy_lock == 0x2003);
    waited_index = 0xFFFFFFFF;
    memset(regs, 0, sizeof regs);
    regs[0] = (uint64_t)&waited_index;
    regs[1] = (uint64_t)&busy_handle;
    regs[2] = 1;
    regs[3] = 1000000000;
    svc_dispatch(0x18, regs, 0);
    assert(regs[0] == 0 && regs[1] == 0);
    assert(__atomic_load_n(&g_test_busy_stage, __ATOMIC_SEQ_CST) == 2);
    memset(regs, 0, sizeof regs);
    regs[0] = busy_handle;
    svc_dispatch(0x16, regs, 0);
    assert(regs[0] == 0);

    test_address_arbitration();
    test_address_waiter_order(0);
    test_address_waiter_order(1);
    test_cv_waiter_order(0);
    test_cv_waiter_order(1);
    test_mutex_waiter_order(0);
    test_mutex_waiter_order(1);
    test_thread_handle_lifecycle();
    puts("T_RUNTIME OK tls/tlr main-handle getinfo querymemory split/merge movememory thread-create sync-wait sync-cycle priority/core");
    return 0;
}
