#include "fs.h"
#include "skyhost.h"
#include <assert.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <mach-o/getsect.h>

/* Pause actual POSIX boundaries in this test translation unit. Production
 * filesystem code has no test hooks or scheduling delays. */
static int lifecycle_open(const char *path, int flags, ...);
static DIR *lifecycle_opendir(const char *path);
static int lifecycle_close(int fd);
#define open lifecycle_open
#define opendir lifecycle_opendir
#define close lifecycle_close
#include "../fs.c"
#undef open
#undef opendir
#undef close

enum { PAUSE_NONE, PAUSE_OPEN, PAUSE_OPENDIR, PAUSE_CLOSE };
enum { OP_OPEN, OP_OPENDIR, OP_CLOSE, OP_UNMOUNT };
static pthread_mutex_t g_pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pause_cond = PTHREAD_COND_INITIALIZER;
static int g_pause_kind, g_pause_fd, g_pause_reached, g_pause_resume;

static void pause_boundary(int kind, int fd, const char *path) {
    pthread_mutex_lock(&g_pause_lock);
    if (g_pause_kind == kind && (kind == PAUSE_CLOSE ? fd == g_pause_fd :
        path && (kind == PAUSE_OPENDIR || strstr(path, "/lifetime.bin")))) {
        g_pause_reached = 1;
        pthread_cond_broadcast(&g_pause_cond);
        while (!g_pause_resume) pthread_cond_wait(&g_pause_cond, &g_pause_lock);
    }
    pthread_mutex_unlock(&g_pause_lock);
}

static int lifecycle_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    pause_boundary(PAUSE_OPEN, -1, path);
    return open(path, flags, mode);
}

static DIR *lifecycle_opendir(const char *path) {
    pause_boundary(PAUSE_OPENDIR, -1, path);
    return opendir(path);
}

static int lifecycle_close(int fd) {
    int result = close(fd);
    pause_boundary(PAUSE_CLOSE, fd, NULL);
    return result;
}

typedef struct {
    int kind, started, completed;
    uint64_t handle;
    uint32_t result, extension_result;
    const sky_account_uid_t *uid;
} lifecycle_operation_t;

static void *lifecycle_worker(void *argument) {
    lifecycle_operation_t *operation = argument;
    pthread_mutex_lock(&g_pause_lock);
    operation->started = 1;
    pthread_cond_broadcast(&g_pause_cond);
    pthread_mutex_unlock(&g_pause_lock);
    switch (operation->kind) {
    case OP_OPEN: operation->result = sky_fs_open_file(&operation->handle, "save:/lifetime.bin", 3); break;
    case OP_OPENDIR: operation->result = sky_fs_open_directory(&operation->handle, "save:/Saves", 3); break;
    case OP_CLOSE: sky_fs_close_file(operation->handle); break;
    case OP_UNMOUNT:
        operation->result = sky_fs_unmount("save");
        operation->extension_result = sky_fs_extend_save_data(operation->uid, 0x8000000, 0x1800000);
        break;
    default: assert(!"invalid lifecycle operation");
    }
    pthread_mutex_lock(&g_pause_lock);
    operation->completed = 1;
    pthread_cond_broadcast(&g_pause_cond);
    pthread_mutex_unlock(&g_pause_lock);
    return NULL;
}

static void arm_pause(int kind, int fd) {
    pthread_mutex_lock(&g_pause_lock);
    g_pause_kind = kind;
    g_pause_fd = fd;
    g_pause_reached = g_pause_resume = 0;
    pthread_mutex_unlock(&g_pause_lock);
}

static void await_pause(void) {
    pthread_mutex_lock(&g_pause_lock);
    while (!g_pause_reached) pthread_cond_wait(&g_pause_cond, &g_pause_lock);
    pthread_mutex_unlock(&g_pause_lock);
}

static void assert_pending(lifecycle_operation_t *operation) {
    pthread_mutex_lock(&g_pause_lock);
    while (!operation->started) pthread_cond_wait(&g_pause_cond, &g_pause_lock);
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_nsec += 50000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    int result = 0;
    while (!operation->completed && result != ETIMEDOUT)
        result = pthread_cond_timedwait(&g_pause_cond, &g_pause_lock, &deadline);
    assert(!operation->completed && result == ETIMEDOUT);
    pthread_mutex_unlock(&g_pause_lock);
}

static void release_pause(void) {
    pthread_mutex_lock(&g_pause_lock);
    g_pause_kind = PAUSE_NONE;
    g_pause_resume = 1;
    pthread_cond_broadcast(&g_pause_cond);
    pthread_mutex_unlock(&g_pause_lock);
}

static void test_mount_lifetimes(const sky_account_uid_t *uid) {
    assert(sky_fs_create_file("save:/lifetime.bin", 1) == 0);
    for (int directory = 0; directory < 2; directory++) {
        lifecycle_operation_t opening = {.kind = directory ? OP_OPENDIR : OP_OPEN};
        lifecycle_operation_t unmounting = {.kind = OP_UNMOUNT, .uid = uid};
        pthread_t opener, unmounter;
        arm_pause(directory ? PAUSE_OPENDIR : PAUSE_OPEN, -1);
        assert(!pthread_create(&opener, NULL, lifecycle_worker, &opening));
        await_pause();
        assert(!pthread_create(&unmounter, NULL, lifecycle_worker, &unmounting));
        assert_pending(&unmounting);
        release_pause();
        assert(!pthread_join(opener, NULL) && !pthread_join(unmounter, NULL));
        assert(opening.result == 0 && opening.handle);
        assert(unmounting.result == 0xe02 && unmounting.extension_result == 0xe02);
        if (directory) sky_fs_close_directory(opening.handle);
        else sky_fs_close_file(opening.handle);
    }
    uint64_t old_handle = 0;
    assert(sky_fs_open_file(&old_handle, "save:/lifetime.bin", 3) == 0);
    lifecycle_operation_t closing = {.kind = OP_CLOSE, .handle = old_handle};
    lifecycle_operation_t reopening = {.kind = OP_OPEN};
    pthread_t closer, reopener;
    arm_pause(PAUSE_CLOSE, (int)old_handle);
    assert(!pthread_create(&closer, NULL, lifecycle_worker, &closing));
    await_pause(); /* OS fd is free, while its old tracking is not yet retired. */
    assert(!pthread_create(&reopener, NULL, lifecycle_worker, &reopening));
    assert_pending(&reopening);
    release_pause();
    assert(!pthread_join(closer, NULL) && !pthread_join(reopener, NULL));
    assert(reopening.result == 0 && reopening.handle == old_handle);
    char value = 'x';
    assert(sky_fs_write_file(reopening.handle, 0, &value, 1, NULL) == 0);
    assert(sky_fs_unmount("save") == 0xe02);
    sky_fs_close_file(reopening.handle);
    assert(sky_fs_delete_file("save:/lifetime.bin") == 0);
}

extern const struct mach_header_64 _mh_execute_header;
static void *force_function(const char *name) {
    typedef struct { const char *name, *why; void *function; } force_entry_t;
    unsigned long size = 0;
    const force_entry_t *entries = (const force_entry_t *)getsectiondata(
        &_mh_execute_header, "__DATA", "swforce", &size);
    assert(entries && size % sizeof *entries == 0);
    for (unsigned long index = 0; index < size / sizeof *entries; index++)
        if (!strcmp(entries[index].name, name)) return entries[index].function;
    assert(!"missing forced filesystem entry point");
    return NULL;
}

static void write_fixture(const char *directory) {
    char path[1024];
    assert(snprintf(path, sizeof path, "%s/data.bin", directory) > 0);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    uint8_t bytes[10];
    for (int index = 0; index < 10; index++) bytes[index] = (uint8_t)index;
    assert(write(fd, bytes, sizeof bytes) == (ssize_t)sizeof bytes);
    assert(!close(fd));
}

static void test_source_save_config(void) {
    char directory[] = "/tmp/sky-save-profile-XXXXXX";
    assert(mkdtemp(directory));
    setenv("SKY_SAVE_DIR", directory, 1);
    sky_account_uid_t uid = {{SKY_ACCOUNT_OFFLINE_UID_LO, SKY_ACCOUNT_OFFLINE_UID_HI}};
    int64_t size = -1, journal = -1;
    sky_profile_configure_main("unknown", NULL);
    assert(sky_fs_get_save_data_size(&size, &journal, &uid) == RES_FAIL);
    assert(size == -1 && journal == -1);
    sky_save_config_t save = {0x20000, 0x10000, 0x40000, 0x20000};
    sky_profile_configure_main("unknown", &save);
    assert(!sky_profile_has_update_ui());
    assert(sky_fs_ensure_save_data(&uid) == 0);
    assert(sky_fs_get_save_data_size(&size, &journal, &uid) == 0);
    assert(size == save.data_size && journal == save.journal_size);
    assert(sky_fs_extend_save_data(&uid, save.data_max + 1, save.journal_max) == RES_EXTENSION_SIZE_TOO_LARGE);
    assert(sky_fs_extend_save_data(&uid, save.data_max, save.journal_max) == 0);
    assert(sky_fs_get_save_data_size(&size, &journal, &uid) == 0);
    assert(size == save.data_max && journal == save.journal_max);
    sky_profile_configure_main(NULL, NULL);
    assert(sky_fs_get_save_data_size(&size, &journal, &uid) == RES_FAIL);
    assert(!rmdir(directory));
    sky_profile_configure_main(SKY_UPDATE_MAIN_BUILD_ID, NULL);
    puts("T_FS_PROFILE OK source-initial-capacity source-extension-max unknown-missing-metadata-fails");
}

int main(int argc, char **argv) {
    sky_profile_configure_main(SKY_UPDATE_MAIN_BUILD_ID, NULL);
    if (argc == 4 && !strcmp(argv[1], "capacity")) {
        sky_account_uid_t uid = {{SKY_ACCOUNT_OFFLINE_UID_LO, SKY_ACCOUNT_OFFLINE_UID_HI}};
        int64_t data = -1, journal = -1;
        assert(sky_fs_get_save_data_size(&data, &journal, &uid) == 0);
        assert(data == strtoll(argv[2], NULL, 10) && journal == strtoll(argv[3], NULL, 10));
        return 0;
    }
    test_source_save_config();
    char directory[] = "/tmp/sky-fs-test-XXXXXX";
    assert(mkdtemp(directory));
    write_fixture(directory);
    setenv("SKY_ROMFS", "/definitely/not/a/real/directory", 1);
    assert(sky_fs_mount_rom("rom", NULL, 0) != 0);
    setenv("SKY_ROMFS", directory, 1);
    assert(sky_fs_mount_rom("rom", NULL, 0) == 0);
    assert(sky_fs_mount_rom("rom", NULL, 0) == 0);
    assert(sky_fs_mount_rom("save", NULL, 0) != 0);
    size_t cache_size = 0;
    assert(sky_fs_query_cache_size(&cache_size) == 0 && cache_size == 0x1000);
    assert(sky_fs_enable_global_cache() == 0);
    assert(!sky_fs_is_enabled_access_log());

    const char *invalid[] = {
        "/etc/passwd",
        "rom:/../secret",
        "rom:/data/../../secret",
        "rom:/data\\data.bin",
        "save:/data.bin",
    };
    for (size_t index = 0; index < sizeof invalid / sizeof invalid[0]; index++) {
        uint64_t handle = 0;
        assert(sky_fs_open_file(&handle, invalid[index], 1) != 0);
    }
    uint64_t handle = 0;
    assert(sky_fs_open_file(&handle, "rom:/data.bin", 3) != 0);

    assert(sky_fs_open_file(&handle, "rom:/data.bin", 1) == 0);
    assert(handle > 0);
    int64_t size = -1;
    assert(sky_fs_get_file_size(&size, handle) == 0 && size == 10);
    uint8_t buffer[16] = {0};
    size_t completed = 0;
    assert(sky_fs_read_file(&completed, handle, 2, buffer, 4) == 0);
    assert(completed == 4);
    for (int index = 0; index < 4; index++) assert(buffer[index] == index + 2);
    memset(buffer, 0, sizeof buffer);
    assert(sky_fs_read_file(&completed, handle, 8, buffer, 8) == 0);
    assert(completed == 2 && buffer[0] == 8 && buffer[1] == 9);
    assert(sky_fs_read_file(&completed, handle, 20, buffer, 1) == 0 && completed == 0);
    assert(sky_fs_read_file(&completed, 0, 0, buffer, 1) != 0);

    /* SDK internal file users call all four public overloads. Their handle is
     * the host fd, which must never enter the SDK's opaque FileAccessor. */
    typedef uint32_t (*read_option_fn)(size_t *, uint64_t, int64_t, void *, size_t, const uint32_t *);
    typedef uint32_t (*read_exact_fn)(uint64_t, int64_t, void *, size_t);
    typedef uint32_t (*read_exact_option_fn)(uint64_t, int64_t, void *, size_t, const uint32_t *);
    read_option_fn read_option = (read_option_fn)force_function("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE");
    read_exact_fn read_exact = (read_exact_fn)force_function("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm");
    read_exact_option_fn read_exact_option = (read_exact_option_fn)force_function("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE");
    const uint32_t read_none = 0, unsupported_option = 1;
    memset(buffer, 0xff, sizeof buffer);
    assert(read_option(&completed, handle, 3, buffer, 4, &read_none) == 0 && completed == 4);
    assert(buffer[0] == 3 && buffer[3] == 6 && buffer[4] == 0xff);
    assert(read_option(&completed, handle, 8, buffer, 8, &read_none) == 0 && completed == 2);
    assert(read_exact(handle, 2, buffer, 4) == 0 && buffer[0] == 2 && buffer[3] == 5);
    assert(read_exact_option(handle, 4, buffer, 4, &read_none) == 0 && buffer[0] == 4 && buffer[3] == 7);
    assert(read_exact(handle, 8, buffer, 8) == 0x177a02);
    assert(read_exact_option(handle, 20, buffer, 1, &read_none) == 0x177a02);
    assert(read_exact_option(handle, 20, NULL, 0, &read_none) == 0);
    assert(read_option(&completed, handle, 0, buffer, 1, &unsupported_option) != 0);
    assert(read_option(&completed, handle, 0, buffer, 1, NULL) != 0);
    sky_fs_close_file(handle);
    sky_fs_close_file(0);
    assert(sky_fs_mount_rom("data", NULL, 0) == 0);
    assert(sky_fs_open_file(&handle, "data:/data.bin", 1) == 0);
    sky_fs_close_file(handle);

    handle = 0;
    assert(sky_fs_open_file(&handle, "rom:/missing.bin", 1) == 0x202);

    int32_t type = -1;
    assert(sky_fs_get_entry_type(&type, "rom:/data.bin") == 0 && type == 1);
    assert(sky_fs_get_entry_type(&type, "data:/") == 0 && type == 0);
    type = -1;
    assert(sky_fs_get_entry_type(&type, "rom:/missing.bin") == 0x202 && type == -1);
    assert(sky_fs_get_entry_type(NULL, "rom:/data.bin") != 0);
    assert(sky_fs_get_entry_type(&type, "rom:/../secret") == 0x202);

    char save_root[1024];
    assert(snprintf(save_root, sizeof save_root, "%s/save", directory) > 0);
    setenv("SKY_SAVE_DIR", save_root, 1);
    sky_account_uid_t save_uid = {{SKY_ACCOUNT_OFFLINE_UID_LO, SKY_ACCOUNT_OFFLINE_UID_HI}};
    assert(sky_fs_ensure_save_data(&save_uid) == 0);
    assert(sky_fs_mount_save_data("save", &save_uid) == 0);
    assert(sky_fs_mount_save_data("save", &save_uid) == 0x7802);
    assert(sky_fs_get_entry_type(&type, "save:/Saves") == 0x202);
    assert(sky_fs_create_directory("save:/Saves") == 0);
    assert(sky_fs_create_directory("save:/Saves") == 0x402);
    assert(sky_fs_get_entry_type(&type, "save:/Saves") == 0 && type == 0);
    assert(sky_fs_create_directory("rom:/not-allowed") != 0);
    int64_t save_size = -1, save_journal = -1;
    assert(sky_fs_get_save_data_size(&save_size, &save_journal, &save_uid) == 0);
    assert(save_size == 0x2800000 && save_journal == 0x1800000);
    sky_account_uid_t other_uid = {{2, 0}};
    assert(sky_fs_ensure_save_data(&other_uid) != 0);
    assert(sky_fs_mount_save_data("save", &other_uid) != 0);
    assert(sky_fs_create_directory("save:/missing/child") == 0x202);

    uint64_t directory_handle = 0;
    assert(sky_fs_open_directory(&directory_handle, "save:/Saves", 3) == 0 && directory_handle);
    assert(sky_fs_unmount("save") == 0xe02);
    int64_t directory_count = -1;
    assert(sky_fs_get_directory_entry_count(&directory_count, directory_handle) == 0 && directory_count == 0);
    assert(sky_fs_read_directory(&directory_count, NULL, directory_handle, 0) == 0 && directory_count == 0);
    sky_fs_close_directory(directory_handle);
    sky_fs_close_directory(0);
    assert(sky_fs_read_directory(&directory_count, NULL, 0, 0) != 0);

    assert(sky_fs_open_directory(&directory_handle, "rom:/", 3) == 0 && directory_handle);
    assert(sky_fs_get_directory_entry_count(&directory_count, directory_handle) == 0 && directory_count == 2);
    sky_fs_directory_entry_t entries[2];
    assert(sizeof entries[0] == 0x310);
    assert(sky_fs_read_directory(&directory_count, entries, directory_handle, 2) == 0 && directory_count == 2);
    int saw_file = 0, saw_directory = 0;
    for (size_t index = 0; index < 2; index++) {
        if (entries[index].type == 1 && !strcmp(entries[index].name, "data.bin") && entries[index].size == 10)
            saw_file = 1;
        if (entries[index].type == 0 && !strcmp(entries[index].name, "save") && entries[index].size == 0)
            saw_directory = 1;
    }
    assert(saw_file && saw_directory);
    sky_fs_close_directory(directory_handle);

    assert(sky_fs_create_file("save:/created.bin", 10) == 0);
    char created_host[1024];
    assert(snprintf(created_host, sizeof created_host, "%s/created.bin", save_root) > 0);
    struct stat created_status;
    assert(!stat(created_host, &created_status) && created_status.st_size == 10);
    assert(sky_fs_create_file("save:/created.bin", 4) == 0x402);
    assert(!stat(created_host, &created_status) && created_status.st_size == 10);
    assert(sky_fs_create_directory("save:/created.bin") == 0x402);
    assert(sky_fs_delete_file("save:/created.bin") == 0);
    assert(sky_fs_delete_file("save:/created.bin") == 0x202);
    assert(sky_fs_create_file("save:/created.bin", 4) == 0);
    assert(sky_fs_delete_file("save:/created.bin") == 0);
    assert(sky_fs_create_file("save:/missing/child", 0) == 0x202);
    assert(sky_fs_create_file("rom:/not-allowed.bin", 0) != 0);
    assert(sky_fs_delete_file("rom:/data.bin") != 0);

    assert(sky_fs_create_file("save:/write.bin", 4) == 0);
    assert(sky_fs_open_file(&handle, "save:/write.bin", 3) == 0);
    assert(sky_fs_unmount("save") == 0xe02);
    uint8_t write_bytes[4] = {4, 5, 6, 7};
    uint32_t write_option = 1;
    assert(sky_fs_write_file(handle, 2, write_bytes, sizeof write_bytes, &write_option) == 0);
    assert(sky_fs_get_file_size(&size, handle) == 0 && size == 6);
    memset(buffer, 0, sizeof buffer);
    assert(sky_fs_read_file(&completed, handle, 0, buffer, 6) == 0 && completed == 6);
    assert(buffer[0] == 0 && buffer[1] == 0);
    for (int index = 0; index < 4; index++) assert(buffer[index + 2] == write_bytes[index]);
    assert(sky_fs_commit_save_data("save") == 0);
    assert(sky_fs_commit_save_data("rom") != 0);
    sky_fs_close_file(handle);
    assert(sky_fs_write_file(handle, 0, write_bytes, 1, NULL) != 0);
    assert(sky_fs_commit_save_data("save") == 0);
    typedef void (*unmount_fn)(const char *);
    typedef uint32_t (*extend_fn)(const sky_account_uid_t *, int64_t, int64_t);
    typedef uint32_t (*get_size_fn)(int64_t *, int64_t *, const sky_account_uid_t *);
    unmount_fn unmount_save = (unmount_fn)force_function("_ZN2nn2fs7UnmountEPKc");
    extend_fn extend_save = (extend_fn)force_function("_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll");
    get_size_fn get_size = (get_size_fn)force_function("_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE");
    assert(extend_save(&save_uid, 0x6800000, 0x1800000) == 0xe02);
    unmount_save("save");
    assert(sky_fs_unmount("save") == 0x35f202);
    assert(sky_fs_unmount("missing") == 0x35f202);
    assert(sky_fs_unmount(NULL) == 0x35f202);
    assert(sky_fs_open_file(&handle, "save:/write.bin", 1) == 0x202);
    assert(sky_fs_commit_save_data("save") != 0);
    /* New Game requests total capacities, not deltas. Persist the exact SDK
     * data/journal pair and preserve the existing save across remounts. */
    assert(extend_save(&save_uid, 0x6800000, 0x1800000) == 0);
    assert(get_size(&save_size, &save_journal, &save_uid) == 0);
    assert(save_size == 0x6800000 && save_journal == 0x1800000);
    assert(extend_save(&save_uid, 0x6800000, 0x1800000) == 0);
    assert(extend_save(&save_uid, 0x2800000, 0x1800000) == 0x2f5c02);
    assert(extend_save(&save_uid, 0x6800000, 0) == 0x2f5c02);
    assert(extend_save(&save_uid, -1, 0) == 0x2f5c02);
    assert(extend_save(&save_uid, INT64_MAX, 1) == 0x2f5c02);
    assert(extend_save(&save_uid, 0x1e100000, 0x1800000) == 0x2f6402);
    assert(extend_save(&save_uid, 0x6800000, 0x1900000) == 0x2f6402);
    assert(extend_save(&other_uid, 0x6800000, 0x1800000) != 0);
    assert(get_size(&save_size, &save_journal, &save_uid) == 0);
    assert(save_size == 0x6800000 && save_journal == 0x1800000);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl(argv[0], argv[0], "capacity", "109051904", "25165824", NULL);
        _exit(127);
    }
    int child_status = 0;
    assert(waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    assert(sky_fs_ensure_save_data(&save_uid) == 0);
    assert(get_size(&save_size, &save_journal, &save_uid) == 0);
    assert(save_size == 0x6800000 && save_journal == 0x1800000);
    assert(sky_fs_mount_save_data("save", &save_uid) == 0);
    assert(sky_fs_open_file(&handle, "save:/write.bin", 1) == 0);
    memset(buffer, 0, sizeof buffer);
    assert(sky_fs_read_file(&completed, handle, 0, buffer, 6) == 0 && completed == 6);
    for (int index = 0; index < 4; index++) assert(buffer[index + 2] == write_bytes[index]);
    sky_fs_close_file(handle);
    assert(sky_fs_open_directory(&directory_handle, "save:/", 3) == 0);
    assert(sky_fs_get_directory_entry_count(&directory_count, directory_handle) == 0 && directory_count == 2);
    sky_fs_close_directory(directory_handle);
    test_mount_lifetimes(&save_uid);
    unmount_save("save");
    /* A corrupt capacity record must fail without altering the save files or
     * replacing the malformed metadata with an invented successful size. */
    const char corrupt[] = "invalid";
    assert(setxattr(save_root, "org.skyrim-apple.save-capacity-v1", corrupt, sizeof corrupt, 0, 0) == 0);
    save_size = save_journal = -1;
    assert(get_size(&save_size, &save_journal, &save_uid) != 0 && save_size == -1 && save_journal == -1);
    assert(extend_save(&save_uid, 0x8000000, 0x1800000) != 0);
    assert(sky_fs_mount_save_data("save", &save_uid) == 0);
    assert(sky_fs_get_entry_type(&type, "save:/write.bin") == 0 && type == 1);
    assert(sky_fs_create_file("save:/Saves/7107-118-78000000_Save1_bad.sav0", 4) == 0);
    assert(sky_fs_create_file("save:/Saves/20261019160000_Save1_good.sav0", 4) == 0);
    assert(sky_fs_unmount("save") == 0);
    assert(sky_fs_mount_save_data("save", &save_uid) == 0);
    assert(sky_fs_get_entry_type(&type, "save:/Saves/7107-118-78000000_Save1_bad.sav0") == 0x202);
    assert(sky_fs_get_entry_type(&type, "save:/Saves/20261019160000_Save1_good.sav0") == 0 && type == 1);
    assert(sky_fs_delete_file("save:/Saves/20261019160000_Save1_good.sav0") == 0);
    assert(sky_fs_unmount("save") == 0);
    assert(sky_fs_unmount("rom") == 0);
    assert(sky_fs_open_file(&handle, "rom:/data.bin", 1) == 0x202);
    assert(sky_fs_open_file(&handle, "data:/data.bin", 1) == 0);
    sky_fs_close_file(handle);
    assert(sky_fs_unmount("data") == 0);
    puts("T_FS OK open/read/size/close entry-type short-read missing traversal mount-scope save-ensure-mount-create directory-io file-create-delete write-commit");
    puts("T_FS_SAVE OK unmount-remount total-capacity data-journal persisted-restart preserved-files mounted-handles failures");
    puts("T_FS_LIFETIME OK concurrent-open-unmount directory-open-unmount close-fd-reuse");
    return 0;
}
