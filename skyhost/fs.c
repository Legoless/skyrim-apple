#include "fs.h"
#include "skyhost.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#define RES_OK 0u
#define RES_FAIL 0x2A2u
#define RES_PATH_NOT_FOUND 0x202u
#define RES_PATH_ALREADY_EXISTS 0x402u
#define RES_TARGET_LOCKED 0xE02u
#define RES_NOT_ENOUGH_FREE_SPACE 0x3C02u
#define RES_MOUNT_ALREADY_EXISTS 0x7802u
#define RES_INVALID_SIZE 0x2F5C02u
#define RES_EXTENSION_SIZE_TOO_LARGE 0x2F6402u
#define RES_NOT_MOUNTED 0x35F202u
#define RES_UNEXPECTED_END_OF_FILE 0x177A02u
#define MODE_READ 1
#define MODE_WRITE 2
#define MODE_APPEND 4
#define MAX_FD_PATH 4096

typedef struct { const char *name, *why; void *function; } sky_force_ent;
#define SKY_FORCE(mangled, why, return_type, function_name, ...)                         \
    static return_type function_name(__VA_ARGS__);                                      \
    __attribute__((used, section("__DATA,swforce")))                                    \
    static const sky_force_ent sky_force_##function_name = {mangled, why, (void *)function_name}; \
    static return_type function_name(__VA_ARGS__)

static char g_rom_root[1024], g_save_root[1024];
static int g_rom_mounted, g_save_mounted;
static unsigned g_save_open_directories;
static pthread_mutex_t g_save_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_fd_path[MAX_FD_PATH];
static uint8_t g_fd_writable[MAX_FD_PATH], g_fd_save[MAX_FD_PATH];
static uint32_t ensure_save_capacity(const char *root);

static void sanitize(const char *text, char *out, size_t size) {
    size_t index = 0;
    if (!text) text = "(null)";
    while (*text && index + 1 < size) {
        unsigned char c = (unsigned char)*text++;
        out[index++] = c >= 0x20 && c <= 0x7e ? (char)c : '?';
    }
    if (size) out[index] = 0;
}

static uint32_t fs_error(const char *operation, const char *guest_path, uint32_t result) {
    char sanitized[1024];
    sanitize(guest_path, sanitized, sizeof sanitized);
    LOG("%s path=%s", operation, sanitized);
    return result;
}

uint32_t sky_fs_enable_global_cache(void) {
    sky_checkpoint("post_nnMain_fs_cache_disabled");
    LOG("FS_CACHE disabled=host-direct-read");
    return RES_OK;
}

unsigned sky_fs_is_enabled_access_log(void) {
    sky_checkpoint("post_nnMain_fs_access_log_disabled");
    LOG("FS_ACCESS_LOG disabled");
    return 0;
}

uint32_t sky_fs_query_cache_size(size_t *out) {
    if (!out) return fs_error("FS_CACHE_SIZE_ERROR", "null-output", RES_FAIL);
    *out = 0x1000;
    return RES_OK;
}

static uint32_t mount_rom_locked(const char *name, void *cache, size_t size) {
    (void)cache;
    (void)size;
    if (!name || (strcmp(name, "rom") && strcmp(name, "data"))) return fs_error("FS_MOUNT_ERROR unsupported-mount", name, RES_FAIL);
    const char *root = getenv("SKY_ROMFS");
    if (!root || !*root) return fs_error("FS_MOUNT_ERROR missing-SKY_ROMFS", "", RES_FAIL);
    struct stat status;
    if (stat(root, &status) || !S_ISDIR(status.st_mode) || access(root, R_OK))
        return fs_error("FS_MOUNT_ERROR invalid-SKY_ROMFS", root, RES_FAIL);
    int length = snprintf(g_rom_root, sizeof g_rom_root, "%s", root);
    if (length < 0 || (size_t)length >= sizeof g_rom_root)
        return fs_error("FS_MOUNT_ERROR SKY_ROMFS-too-long", root, RES_FAIL);
    g_rom_mounted |= !strcmp(name, "rom") ? 1 : 2;
    sky_checkpoint("post_nnMain_fs_mount");
    LOG("FS_MOUNT %s root=%s", name, root);
    return RES_OK;
}

uint32_t sky_fs_mount_rom(const char *name, void *cache, size_t size) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = mount_rom_locked(name, cache, size);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

static int valid_save_uid(const sky_account_uid_t *uid) {
    return uid && uid->words[0] == SKY_ACCOUNT_OFFLINE_UID_LO && uid->words[1] == SKY_ACCOUNT_OFFLINE_UID_HI;
}

static uint32_t configured_save_root(char *out, size_t out_size, int create, const char *operation) {
    const char *root = getenv("SKY_SAVE_DIR");
    if (!root || !*root) return fs_error(operation, "missing-SKY_SAVE_DIR", RES_FAIL);
    int length = snprintf(out, out_size, "%s", root);
    if (length < 0 || (size_t)length >= out_size) return fs_error(operation, "SKY_SAVE_DIR-too-long", RES_FAIL);
    if (create && mkdir(out, 0700) && errno != EEXIST) return fs_error(operation, out, RES_FAIL);
    struct stat status;
    if (stat(out, &status) || !S_ISDIR(status.st_mode) || access(out, R_OK | W_OK | X_OK))
        return fs_error(operation, out, RES_FAIL);
    return RES_OK;
}

uint32_t sky_fs_ensure_save_data(const sky_account_uid_t *uid) {
    if (!valid_save_uid(uid)) return fs_error("SAVE_ENSURE_ERROR invalid-uid", "", RES_FAIL);
    char root[1024];
    uint32_t result = configured_save_root(root, sizeof root, 1, "SAVE_ENSURE_ERROR");
    if (result != RES_OK) return result;
    result = ensure_save_capacity(root);
    if (result != RES_OK) return result;
    sky_checkpoint("post_nnMain_save_ensure");
    LOG("SAVE_ENSURE uid_lo=%#llx uid_hi=%#llx root=%s",
        (unsigned long long)uid->words[0], (unsigned long long)uid->words[1], root);
    return RES_OK;
}

static void sanitize_saves_directory(const char *save_root) {
    char saves_path[1024];
    int n = snprintf(saves_path, sizeof saves_path, "%s/Saves", save_root);
    if (n < 0 || (size_t)n >= sizeof saves_path) return;
    DIR *dir = opendir(saves_path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;
        const char *ext = strstr(name, ".sav");
        if (!ext) continue;
        int valid_prefix = 1;
        for (int i = 0; i < 14; i++) {
            if (name[i] < '0' || name[i] > '9') {
                valid_prefix = 0;
                break;
            }
        }
        if (valid_prefix && name[14] != '_') valid_prefix = 0;
        if (!valid_prefix) {
            if (unlinkat(dirfd(dir), name, 0) == 0) {
                LOG("FS_SAVE_SANITIZE unlinked_malformed=%s", name);
            }
        }
    }
    closedir(dir);
}

uint32_t sky_fs_mount_save_data(const char *name, const sky_account_uid_t *uid) {
    if (!name || strcmp(name, "save")) return fs_error("FS_MOUNT_ERROR unsupported-save-mount", name, RES_FAIL);
    if (!valid_save_uid(uid)) return fs_error("FS_MOUNT_ERROR invalid-save-uid", "", RES_FAIL);
    pthread_mutex_lock(&g_save_lock);
    if (g_save_mounted) {
        pthread_mutex_unlock(&g_save_lock);
        return fs_error("FS_MOUNT_ERROR already-mounted", name, RES_MOUNT_ALREADY_EXISTS);
    }
    uint32_t result = configured_save_root(g_save_root, sizeof g_save_root, 0, "FS_MOUNT_ERROR invalid-SKY_SAVE_DIR");
    if (result == RES_OK) {
        sanitize_saves_directory(g_save_root);
        g_save_mounted = 1;
    }
    pthread_mutex_unlock(&g_save_lock);
    if (result != RES_OK) return result;
    sky_checkpoint("post_nnMain_save_mount");
    LOG("FS_MOUNT save root=%s mode=writable", g_save_root);
    return RES_OK;
}

uint32_t sky_fs_unmount(const char *name) {
    if (!name) return fs_error("FS_UNMOUNT_ERROR invalid-name", name, RES_NOT_MOUNTED);
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = RES_OK;
    if (!strcmp(name, "save")) {
        if (!g_save_mounted) result = RES_NOT_MOUNTED;
        else {
            for (int fd = 0; fd < MAX_FD_PATH; fd++)
                if (g_fd_save[fd]) result = RES_TARGET_LOCKED;
            if (g_save_open_directories) result = RES_TARGET_LOCKED;
            if (result == RES_OK) g_save_mounted = 0;
        }
    } else {
        int bit = !strcmp(name, "rom") ? 1 : !strcmp(name, "data") ? 2 : 0;
        if (!bit || !(g_rom_mounted & bit)) result = RES_NOT_MOUNTED;
        else g_rom_mounted &= ~bit;
    }
    pthread_mutex_unlock(&g_save_lock);
    if (result != RES_OK) return fs_error("FS_UNMOUNT_ERROR", name, result);
    sky_checkpoint("post_nnMain_fs_unmount");
    LOG("FS_UNMOUNT name=%s", name);
    return RES_OK;
}

/* The caller holds g_save_lock until its POSIX operation and any handle
 * registration finish, so unmount cannot invalidate an in-flight resolution. */
static uint32_t resolve_path(const char *guest, char *host, size_t host_size, char *relative, size_t relative_size) {
    if (!guest || guest[0] == '/') return fs_error("FS_PATH_ERROR absolute-escape", guest, RES_PATH_NOT_FOUND);
    const char *colon = strchr(guest, ':');
    size_t prefix_length = colon ? (size_t)(colon - guest) : 0;
    int is_rom = prefix_length == 3 && !strncmp(guest, "rom", 3);
    int is_data = prefix_length == 4 && !strncmp(guest, "data", 4);
    int is_save = prefix_length == 4 && !strncmp(guest, "save", 4);
    if (!colon || colon == guest || (!is_rom && !is_data && !is_save))
        return fs_error("FS_PATH_ERROR mount-scope", guest, RES_PATH_NOT_FOUND);
    const char *root;
    if (is_save) {
        if (!g_save_mounted) return fs_error("FS_PATH_ERROR save-not-mounted", guest, RES_PATH_NOT_FOUND);
        root = g_save_root;
    } else {
        if (!(g_rom_mounted & (is_rom ? 1 : 2))) return fs_error("FS_PATH_ERROR not-mounted", guest, RES_PATH_NOT_FOUND);
        root = g_rom_root;
    }
    const char *rest = colon + 1;
    if (snprintf(host, host_size, "%s", root) < 0) return RES_FAIL;
    relative[0] = 0;
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/');
        if (!end) end = rest + strlen(rest);
        size_t length = (size_t)(end - rest);
        if (!length || length >= 0x301) return fs_error("FS_PATH_ERROR component-length", guest, RES_PATH_NOT_FOUND);
        char component[0x301];
        memcpy(component, rest, length);
        component[length] = 0;
        if (!strcmp(component, "..") || strchr(component, '\\'))
            return fs_error("FS_PATH_ERROR traversal", guest, RES_PATH_NOT_FOUND);
        for (size_t index = 0; index < length; index++)
            if ((unsigned char)component[index] < 0x20 || (unsigned char)component[index] == 0x7f)
                return fs_error("FS_PATH_ERROR control-character", guest, RES_PATH_NOT_FOUND);
        if (strcmp(component, ".")) {
            size_t host_length = strlen(host), relative_length = strlen(relative);
            if (host_length + length + 2 > host_size || relative_length + length + 2 > relative_size)
                return fs_error("FS_PATH_ERROR path-too-long", guest, RES_PATH_NOT_FOUND);
            host[host_length] = '/';
            memcpy(host + host_length + 1, component, length + 1);
            relative[relative_length] = '/';
            memcpy(relative + relative_length + 1, component, length + 1);
        }
        rest = end;
    }
    return RES_OK;
}

static uint32_t open_file_locked(uint64_t *out, const char *path, int mode) {
    if (!out) return fs_error("FS_OPEN_ERROR null-output", path, RES_FAIL);
    char host[1024], relative[1024];
    uint32_t result = resolve_path(path, host, sizeof host, relative, sizeof relative);
    if (result != RES_OK) return result;
    if (mode <= 0 || mode > 7) return fs_error("FS_OPEN_ERROR invalid-mode", relative, RES_FAIL);
    int is_save = !strncmp(path, "save:", 5);
    int write_mode = mode & (MODE_WRITE | MODE_APPEND);
    if (write_mode && !is_save) return fs_error("FS_OPEN_READONLY", relative, RES_FAIL);
    int flags = write_mode ? ((mode & MODE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (mode & MODE_APPEND) flags |= O_APPEND;
    int fd = open(host, flags);
    if (fd < 0) {
        int error = errno;
        if (error == ENOENT || error == ENOTDIR) return fs_error("FS_OPEN_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
        return fs_error("FS_OPEN_ERROR", relative, RES_FAIL);
    }
    struct stat status;
    if (fstat(fd, &status)) {
        int error = errno;
        close(fd);
        errno = error;
        return fs_error("FS_OPEN_ERROR", relative, RES_FAIL);
    }
    if (S_ISDIR(status.st_mode)) {
        close(fd);
        return fs_error("FS_OPEN_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
    }
    if (fd >= MAX_FD_PATH) {
        close(fd);
        return fs_error("FS_OPEN_ERROR fd-table-full", relative, RES_FAIL);
    }
    g_fd_writable[fd] = 0;
    g_fd_save[fd] = 0;
    free(g_fd_path[fd]);
    g_fd_path[fd] = strdup(relative);
    if (!g_fd_path[fd]) {
        close(fd);
        return fs_error("FS_OPEN_ERROR path-allocation", relative, RES_FAIL);
    }
    g_fd_writable[fd] = write_mode != 0;
    g_fd_save[fd] = is_save;
    *out = (uint64_t)fd;
    sky_checkpoint("post_nnMain_fs_open");
    LOG("FS_OPEN path=%s fd=%d", relative, fd);
    return RES_OK;
}

uint32_t sky_fs_open_file(uint64_t *out, const char *path, int mode) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = open_file_locked(out, path, mode);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

uint32_t sky_fs_read_file(size_t *out, uint64_t handle, int64_t offset, void *buffer, size_t size) {
    if (!out || !handle || offset < 0 || (!buffer && size)) return fs_error("FS_READ_ERROR invalid-argument", "", RES_FAIL);
    size_t completed = 0;
    while (completed < size) {
        ssize_t result = pread((int)handle, (uint8_t *)buffer + completed, size - completed, offset + (int64_t)completed);
        if (result < 0) {
            if (errno == EINTR) continue;
            int error = errno;
            const char *path = handle < MAX_FD_PATH && g_fd_path[handle] ? g_fd_path[handle] : "unknown-fd";
            errno = error;
            return fs_error("FS_READ_ERROR", path, RES_FAIL);
        }
        if (!result) break;
        completed += (size_t)result;
    }
    *out = completed;
    const char *path = handle < MAX_FD_PATH && g_fd_path[handle] ? g_fd_path[handle] : "unknown-fd";
    LOG("FS_READ path=%s offset=%lld requested=%zu read=%zu", path, (long long)offset, size, completed);
    return RES_OK;
}

uint32_t sky_fs_read_file_with_option(size_t *out, uint64_t handle, int64_t offset,
    void *buffer, size_t size, const uint32_t *option) {
    if (!option || *option)
        return fs_error("FS_READ_ERROR unsupported-option", "", RES_FAIL);
    return sky_fs_read_file(out, handle, offset, buffer, size);
}

uint32_t sky_fs_read_file_exact(uint64_t handle, int64_t offset, void *buffer,
    size_t size, const uint32_t *option) {
    size_t completed = 0;
    uint32_t result = sky_fs_read_file_with_option(&completed, handle, offset, buffer, size, option);
    if (result != RES_OK) return result;
    /* Both exact-size SDK overloads compare the reported length and return
     * 0x177a02 on short reads (sdk+0x9dbb8 / sdk+0x9dc9c). */
    return completed == size ? RES_OK : RES_UNEXPECTED_END_OF_FILE;
}

uint32_t sky_fs_get_file_size(int64_t *out, uint64_t handle) {
    if (!out || !handle) return fs_error("FS_SIZE_ERROR invalid-handle", "", RES_FAIL);
    struct stat status;
    if (fstat((int)handle, &status)) return fs_error("FS_SIZE_ERROR", "fstat", RES_FAIL);
    *out = (int64_t)status.st_size;
    const char *path = handle < MAX_FD_PATH && g_fd_path[handle] ? g_fd_path[handle] : "unknown-fd";
    LOG("FS_SIZE path=%s size=%lld", path, (long long)*out);
    return RES_OK;
}

static uint32_t get_entry_type_locked(int32_t *out, const char *path) {
    if (!out) return fs_error("FS_ENTRY_TYPE_ERROR null-output", path, RES_FAIL);
    char host[1024], relative[1024];
    uint32_t result = resolve_path(path, host, sizeof host, relative, sizeof relative);
    if (result != RES_OK) return result;
    struct stat status;
    if (stat(host, &status)) {
        int error = errno;
        if (error == ENOENT || error == ENOTDIR) return fs_error("FS_ENTRY_TYPE_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
        return fs_error("FS_ENTRY_TYPE_ERROR", relative, RES_FAIL);
    }
    *out = S_ISDIR(status.st_mode) ? 0 : 1;
    sky_checkpoint("post_nnMain_fs_entry_type");
    LOG("FS_ENTRY_TYPE path=%s type=%s", relative, *out ? "file" : "directory");
    return RES_OK;
}

uint32_t sky_fs_get_entry_type(int32_t *out, const char *path) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = get_entry_type_locked(out, path);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

void sky_fs_close_file(uint64_t handle) {
    if (!handle) return;
    pthread_mutex_lock(&g_save_lock);
    const char *path = handle < MAX_FD_PATH && g_fd_path[handle] ? g_fd_path[handle] : "unknown-fd";
    LOG("FS_CLOSE path=%s", path);
    close((int)handle);
    if (handle < MAX_FD_PATH) {
        free(g_fd_path[handle]);
        g_fd_path[handle] = NULL;
        g_fd_writable[handle] = 0;
        g_fd_save[handle] = 0;
    }
    pthread_mutex_unlock(&g_save_lock);
}

SKY_FORCE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", "Direct POSIX reads make the optional global cache unnecessary",
          uint32_t, sky_enable_global_cache_entry, void) { return sky_fs_enable_global_cache(); }
SKY_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEj", "FS access logging is disabled instead of using an IPC-backed logger",
          unsigned, sky_access_log_entry, unsigned ignored) { (void)ignored; return sky_fs_is_enabled_access_log(); }
SKY_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", "Direct reads need only a minimal cache-size response",
          uint32_t, sky_query_cache_entry, size_t *out) { return sky_fs_query_cache_size(out); }
SKY_FORCE("_ZN2nn2fs8MountRomEPKcPvm", "Mount rom through the configured read-only host directory",
          uint32_t, sky_mount_rom_entry, const char *name, void *cache, size_t size) {
    return sky_fs_mount_rom(name, cache, size);
}
SKY_FORCE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "Open a normalized rom path with POSIX open",
          uint32_t, sky_open_file_entry, uint64_t *out, const char *path, int mode) {
    return sky_fs_open_file(out, path, mode);
}
SKY_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "Read an open host fd with pread and correct short-read counts",
          uint32_t, sky_read_file_entry, size_t *out, uint64_t handle, int64_t offset, void *buffer, size_t size) {
    return sky_fs_read_file(out, handle, offset, buffer, size);
}
SKY_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", "Read a host fd with an explicit ReadOption without entering SDK FileAccessor methods",
          uint32_t, sky_read_file_option_entry, size_t *out, uint64_t handle, int64_t offset,
          void *buffer, size_t size, const uint32_t *option) {
    return sky_fs_read_file_with_option(out, handle, offset, buffer, size, option);
}
SKY_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", "Require an exact-size host read with the SDK short-read error",
          uint32_t, sky_read_file_exact_entry, uint64_t handle, int64_t offset, void *buffer, size_t size) {
    const uint32_t option = 0;
    return sky_fs_read_file_exact(handle, offset, buffer, size, &option);
}
SKY_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE", "Require an exact-size host read with an explicit ReadOption",
          uint32_t, sky_read_file_exact_option_entry, uint64_t handle, int64_t offset,
          void *buffer, size_t size, const uint32_t *option) {
    return sky_fs_read_file_exact(handle, offset, buffer, size, option);
}
SKY_FORCE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", "Get size with fstat",
          uint32_t, sky_get_file_size_entry, int64_t *out, uint64_t handle) {
    return sky_fs_get_file_size(out, handle);
}
SKY_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", "Get file or directory type with stat",
          uint32_t, sky_get_entry_type_entry, int32_t *out, const char *path) {
    return sky_fs_get_entry_type(out, path);
}
SKY_FORCE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", "Close the host fd",
          void, sky_close_file_entry, uint64_t handle) { sky_fs_close_file(handle); }

static uint32_t create_directory_locked(const char *path) {
    char host[1024], relative[1024];
    uint32_t result = resolve_path(path, host, sizeof host, relative, sizeof relative);
    if (result != RES_OK) return result;
    if (strncmp(path, "save:", 5)) return fs_error("FS_CREATE_DIRECTORY_READONLY", relative, RES_FAIL);
    if (mkdir(host, 0700)) {
        int error = errno;
        if (error == EEXIST) return fs_error("FS_CREATE_DIRECTORY_EXISTS", relative, RES_PATH_ALREADY_EXISTS);
        if (error == ENOENT || error == ENOTDIR) return fs_error("FS_CREATE_DIRECTORY_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
        return fs_error("FS_CREATE_DIRECTORY_ERROR", relative, RES_FAIL);
    }
    sky_checkpoint("post_nnMain_save_directory");
    LOG("FS_CREATE_DIRECTORY path=%s", relative);
    return RES_OK;
}

uint32_t sky_fs_create_directory(const char *path) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = create_directory_locked(path);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

SKY_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "Ensure the local UID save directory in the writable run area",
          uint32_t, sky_ensure_save_data_entry, const sky_account_uid_t *uid) {
    return sky_fs_ensure_save_data(uid);
}
SKY_FORCE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "Mount save from the separate writable run directory",
          uint32_t, sky_mount_save_data_entry, const char *name, const sky_account_uid_t *uid) {
    return sky_fs_mount_save_data(name, uid);
}
SKY_FORCE("_ZN2nn2fs7UnmountEPKc", "Unmount the host filesystem before save expansion instead of using the SDK mount table",
          void, sky_unmount_entry, const char *name) {
    uint32_t result = sky_fs_unmount(name);
    if (result != RES_OK) die("FS_UNMOUNT result=%#x", result);
}
SKY_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "Create directories only inside the writable save mount",
          uint32_t, sky_create_directory_entry, const char *path) {
    return sky_fs_create_directory(path);
}

/* Keep host quota metadata out of the guest directory namespace. POSIX files
 * remain the actual backing storage; this records the SDK's two logical sizes,
 * not free space or a claim that the host uses a Switch journal image. */
#define SAVE_CAPACITY_ATTRIBUTE "org.skyrim-apple.save-capacity-v1"
typedef struct { uint64_t version; int64_t size, journal_size; } sky_save_capacity_t;

static uint32_t load_save_capacity(const char *root, sky_save_capacity_t *capacity) {
    sky_save_config_t save;
    if (!sky_profile_save_config(&save))
        return fs_error("SAVE_SIZE_ERROR missing-Control-NACP-metadata-rerun-setup", root, RES_FAIL);
    *capacity = (sky_save_capacity_t){.version = 1, .size = save.data_size, .journal_size = save.journal_size};
    ssize_t length = getxattr(root, SAVE_CAPACITY_ATTRIBUTE, capacity, sizeof *capacity, 0, 0);
    if (length < 0 && errno == ENOATTR) {
        *capacity = (sky_save_capacity_t){.version = 1, .size = save.data_size, .journal_size = save.journal_size};
        return RES_OK;
    }
    if (length != sizeof *capacity || capacity->version != 1 || capacity->size < 0 || capacity->journal_size < 0)
        return fs_error("SAVE_SIZE_ERROR invalid-capacity-metadata", root, RES_FAIL);
    return RES_OK;
}

static uint32_t store_save_capacity(const char *root, const sky_save_capacity_t *capacity, int options) {
    int fd = open(root, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fs_error("SAVE_SIZE_ERROR open-capacity-root", root, RES_FAIL);
    uint32_t result = RES_OK;
    if (fsetxattr(fd, SAVE_CAPACITY_ATTRIBUTE, capacity, sizeof *capacity, 0, options) || fsync(fd)) result = RES_FAIL;
    close(fd);
    return result;
}

static uint32_t ensure_save_capacity(const char *root) {
    pthread_mutex_lock(&g_save_lock);
    sky_save_capacity_t capacity;
    uint32_t result = load_save_capacity(root, &capacity);
    if (result == RES_OK) {
        ssize_t length = getxattr(root, SAVE_CAPACITY_ATTRIBUTE, NULL, 0, 0, 0);
        if (length < 0 && errno == ENOATTR) result = store_save_capacity(root, &capacity, XATTR_CREATE);
        else if (length < 0) result = RES_FAIL;
    }
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

uint32_t sky_fs_get_save_data_size(int64_t *out_size, int64_t *out_journal_size, const sky_account_uid_t *uid) {
    if (!out_size || !out_journal_size || !valid_save_uid(uid)) return fs_error("SAVE_SIZE_ERROR invalid-argument", "", RES_FAIL);
    char root[1024];
    uint32_t result = configured_save_root(root, sizeof root, 0, "SAVE_SIZE_ERROR");
    if (result != RES_OK) return result;
    sky_save_capacity_t capacity;
    pthread_mutex_lock(&g_save_lock);
    result = load_save_capacity(root, &capacity);
    pthread_mutex_unlock(&g_save_lock);
    if (result != RES_OK) return result;
    *out_size = capacity.size;
    *out_journal_size = capacity.journal_size;
    sky_checkpoint("post_nnMain_save_size");
    LOG("SAVE_SIZE uid_lo=%#llx uid_hi=%#llx size=%lld journal=%lld",
        (unsigned long long)uid->words[0], (unsigned long long)uid->words[1],
        (long long)capacity.size, (long long)capacity.journal_size);
    return RES_OK;
}

uint32_t sky_fs_extend_save_data(const sky_account_uid_t *uid, int64_t size, int64_t journal_size) {
    if (!valid_save_uid(uid)) return fs_error("SAVE_EXTEND_ERROR invalid-uid", "", RES_FAIL);
    if (size < 0 || journal_size < 0 || size > INT64_MAX - journal_size)
        return fs_error("SAVE_EXTEND_ERROR invalid-size", "", RES_INVALID_SIZE);
    sky_save_config_t save;
    if (!sky_profile_save_config(&save))
        return fs_error("SAVE_EXTEND_ERROR missing-Control-NACP-metadata-rerun-setup", "", RES_FAIL);
    if (size > save.data_max || journal_size > save.journal_max)
        return fs_error("SAVE_EXTEND_ERROR exceeds-NACP-maximum", "", RES_EXTENSION_SIZE_TOO_LARGE);
    char root[1024];
    uint32_t result = configured_save_root(root, sizeof root, 0, "SAVE_EXTEND_ERROR");
    if (result != RES_OK) return result;
    pthread_mutex_lock(&g_save_lock);
    sky_save_capacity_t capacity;
    if (g_save_mounted) result = RES_TARGET_LOCKED;
    else result = load_save_capacity(root, &capacity);
    if (result == RES_OK && (size < capacity.size || journal_size < capacity.journal_size)) result = RES_INVALID_SIZE;
    if (result == RES_OK) {
        struct statvfs volume;
        if (statvfs(root, &volume)) result = RES_FAIL;
        else {
            uint64_t growth = (uint64_t)(size - capacity.size) + (uint64_t)(journal_size - capacity.journal_size);
            __uint128_t available = (__uint128_t)volume.f_bavail * volume.f_frsize;
            if ((__uint128_t)growth > available) result = RES_NOT_ENOUGH_FREE_SPACE;
        }
    }
    if (result == RES_OK) {
        capacity.size = size;
        capacity.journal_size = journal_size;
        result = store_save_capacity(root, &capacity, 0);
    }
    pthread_mutex_unlock(&g_save_lock);
    if (result != RES_OK) return fs_error("SAVE_EXTEND_ERROR", root, result);
    sky_checkpoint("post_nnMain_save_extend");
    LOG("SAVE_EXTEND size=%lld journal=%lld backing=host-posix", (long long)size, (long long)journal_size);
    return RES_OK;
}

SKY_FORCE("_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", "Read the persisted data and journal capacities of the host save",
          uint32_t, sky_get_save_data_size_entry, int64_t *out_size, int64_t *out_journal_size, const sky_account_uid_t *uid) {
    return sky_fs_get_save_data_size(out_size, out_journal_size, uid);
}
SKY_FORCE("_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", "Extend the unmounted host save's logical data and journal capacities",
          uint32_t, sky_extend_save_data_entry, const sky_account_uid_t *uid, int64_t size, int64_t journal_size) {
    return sky_fs_extend_save_data(uid, size, journal_size);
}

typedef struct {
    DIR *dir;
    int mode;
    char *path;
    int is_save;
} sky_directory_handle_t;

_Static_assert(sizeof(sky_fs_directory_entry_t) == 0x310, "Skyrim DirectoryEntry size");
_Static_assert(offsetof(sky_fs_directory_entry_t, type) == 0x304, "Skyrim DirectoryEntry type offset");
_Static_assert(offsetof(sky_fs_directory_entry_t, size) == 0x308, "Skyrim DirectoryEntry size offset");

static sky_directory_handle_t *directory_handle(uint64_t value) {
    return (sky_directory_handle_t *)(uintptr_t)value;
}

static int directory_next(sky_directory_handle_t *handle, sky_fs_directory_entry_t *out) {
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(handle->dir);
        if (!entry) return errno ? -1 : 0;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat status;
        if (fstatat(dirfd(handle->dir), entry->d_name, &status, 0)) continue;
        int is_directory = S_ISDIR(status.st_mode);
        if (!(handle->mode & (is_directory ? 1 : 2))) continue;
        memset(out, 0, sizeof *out);
        snprintf(out->name, sizeof out->name, "%s", entry->d_name);
        out->type = is_directory ? 0 : 1;
        out->size = is_directory ? 0 : (int64_t)status.st_size;
        return 1;
    }
}

static uint32_t open_directory_locked(uint64_t *out, const char *path, int mode) {
    if (!out) return fs_error("FS_OPEN_DIRECTORY_ERROR null-output", path, RES_FAIL);
    char host[1024], relative[1024];
    uint32_t result = resolve_path(path, host, sizeof host, relative, sizeof relative);
    if (result != RES_OK) return result;
    if (mode < 1 || mode > 3) return fs_error("FS_OPEN_DIRECTORY_ERROR invalid-mode", relative, RES_FAIL);
    DIR *dir = opendir(host);
    if (!dir) {
        int error = errno;
        if (error == ENOENT || error == ENOTDIR) return fs_error("FS_OPEN_DIRECTORY_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
        return fs_error("FS_OPEN_DIRECTORY_ERROR", relative, RES_FAIL);
    }
    sky_directory_handle_t *handle = calloc(1, sizeof *handle);
    if (!handle) {
        closedir(dir);
        return fs_error("FS_OPEN_DIRECTORY_ERROR allocation", relative, RES_FAIL);
    }
    handle->path = strdup(relative);
    if (!handle->path) {
        closedir(dir);
        free(handle);
        return fs_error("FS_OPEN_DIRECTORY_ERROR path-allocation", relative, RES_FAIL);
    }
    handle->dir = dir;
    handle->mode = mode;
    handle->is_save = !strncmp(path, "save:", 5);
    if (handle->is_save) g_save_open_directories++;
    *out = (uint64_t)(uintptr_t)handle;
    sky_checkpoint("post_nnMain_directory_open");
    LOG("FS_OPEN_DIRECTORY path=%s handle=%#llx", relative, (unsigned long long)*out);
    return RES_OK;
}

uint32_t sky_fs_open_directory(uint64_t *out, const char *path, int mode) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = open_directory_locked(out, path, mode);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

uint32_t sky_fs_get_directory_entry_count(int64_t *out, uint64_t value) {
    sky_directory_handle_t *handle = directory_handle(value);
    if (!out || !handle || !handle->dir) return fs_error("FS_DIRECTORY_COUNT_ERROR invalid-handle", "", RES_FAIL);
    int64_t count = 0;
    sky_fs_directory_entry_t entry;
    rewinddir(handle->dir);
    for (;;) {
        int next = directory_next(handle, &entry);
        if (next < 0) {
            rewinddir(handle->dir);
            return fs_error("FS_DIRECTORY_COUNT_ERROR", handle->path, RES_FAIL);
        }
        if (!next) break;
        count++;
    }
    rewinddir(handle->dir);
    *out = count;
    sky_checkpoint("post_nnMain_directory_count");
    LOG("FS_DIRECTORY_COUNT path=%s count=%lld", handle->path, (long long)count);
    return RES_OK;
}

uint32_t sky_fs_read_directory(int64_t *out, sky_fs_directory_entry_t *entries, uint64_t value, int64_t maximum) {
    sky_directory_handle_t *handle = directory_handle(value);
    if (!out || !handle || !handle->dir || maximum < 0 || (maximum && !entries))
        return fs_error("FS_READ_DIRECTORY_ERROR invalid-argument", "", RES_FAIL);
    int64_t count = 0;
    while (count < maximum) {
        int next = directory_next(handle, &entries[count]);
        if (next < 0) return fs_error("FS_READ_DIRECTORY_ERROR", handle->path, RES_FAIL);
        if (!next) break;
        count++;
    }
    *out = count;
    sky_checkpoint("post_nnMain_directory_read");
    LOG("FS_READ_DIRECTORY path=%s count=%lld", handle->path, (long long)count);
    return RES_OK;
}

void sky_fs_close_directory(uint64_t value) {
    sky_directory_handle_t *handle = directory_handle(value);
    if (!handle) return;
    pthread_mutex_lock(&g_save_lock);
    LOG("FS_CLOSE_DIRECTORY path=%s", handle->path);
    if (handle->dir) closedir(handle->dir);
    if (handle->is_save) g_save_open_directories--;
    free(handle->path);
    free(handle);
    sky_checkpoint("post_nnMain_directory_close");
    pthread_mutex_unlock(&g_save_lock);
}

SKY_FORCE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", "Open a normalized directory with POSIX opendir",
          uint32_t, sky_open_directory_entry, uint64_t *out, const char *path, int mode) {
    return sky_fs_open_directory(out, path, mode);
}
SKY_FORCE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", "Count real directory entries with mode filtering",
          uint32_t, sky_directory_count_entry, int64_t *out, uint64_t handle) {
    return sky_fs_get_directory_entry_count(out, handle);
}
SKY_FORCE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", "Read real entries in Skyrim's 0x310-byte layout",
          uint32_t, sky_read_directory_entry, int64_t *out, sky_fs_directory_entry_t *entries, uint64_t handle, int64_t maximum) {
    return sky_fs_read_directory(out, entries, handle, maximum);
}
SKY_FORCE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", "Close the POSIX directory handle",
          void, sky_close_directory_entry, uint64_t handle) { sky_fs_close_directory(handle); }

static uint32_t delete_file_locked(const char *path) {
    char host[1024], relative[1024];
    uint32_t result = resolve_path(path, host, sizeof host, relative, sizeof relative);
    if (result != RES_OK) return result;
    if (strncmp(path, "save:", 5)) return fs_error("FS_DELETE_FILE_READONLY", relative, RES_FAIL);
    if (unlink(host)) {
        int error = errno;
        if (error == ENOENT || error == ENOTDIR) return fs_error("FS_DELETE_FILE_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
        return fs_error("FS_DELETE_FILE_ERROR", relative, RES_FAIL);
    }
    sky_checkpoint("post_nnMain_delete_file");
    LOG("FS_DELETE_FILE path=%s", relative);
    return RES_OK;
}

uint32_t sky_fs_delete_file(const char *path) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = delete_file_locked(path);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

static uint32_t create_file_locked(const char *path, int64_t size) {
    if (size < 0) return fs_error("FS_CREATE_FILE_ERROR invalid-size", path, RES_FAIL);
    char host[1024], relative[1024];
    uint32_t result = resolve_path(path, host, sizeof host, relative, sizeof relative);
    if (result != RES_OK) return result;
    if (strncmp(path, "save:", 5)) return fs_error("FS_CREATE_FILE_READONLY", relative, RES_FAIL);
    int fd = open(host, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        int error = errno;
        if (error == ENOENT || error == ENOTDIR) return fs_error("FS_CREATE_FILE_NOT_FOUND", relative, RES_PATH_NOT_FOUND);
        if (error == EEXIST) return fs_error("FS_CREATE_FILE_EXISTS", relative, RES_PATH_ALREADY_EXISTS);
        return fs_error("FS_CREATE_FILE_ERROR", relative, RES_FAIL);
    }
    if (ftruncate(fd, (off_t)size)) {
        int error = errno;
        close(fd);
        unlink(host);
        errno = error;
        return fs_error("FS_CREATE_FILE_ERROR", relative, RES_FAIL);
    }
    if (close(fd)) return fs_error("FS_CREATE_FILE_ERROR", relative, RES_FAIL);
    sky_checkpoint("post_nnMain_create_file");
    LOG("FS_CREATE_FILE path=%s size=%lld", relative, (long long)size);
    return RES_OK;
}

uint32_t sky_fs_create_file(const char *path, int64_t size) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = create_file_locked(path, size);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

SKY_FORCE("_ZN2nn2fs10DeleteFileEPKc", "Delete files only inside the writable save mount",
          uint32_t, sky_delete_file_entry, const char *path) { return sky_fs_delete_file(path); }
SKY_FORCE("_ZN2nn2fs10CreateFileEPKcl", "Create a new file with an exact size only inside the writable save mount",
          uint32_t, sky_create_file_entry, const char *path, int64_t size) {
    return sky_fs_create_file(path, size);
}

uint32_t sky_fs_write_file(uint64_t handle, int64_t offset, const void *buffer, size_t size, const void *option) {
    if (!handle || handle >= MAX_FD_PATH || !g_fd_writable[handle] || offset < 0 || (!buffer && size))
        return fs_error("FS_WRITE_ERROR invalid-argument", "", RES_FAIL);
    const char *path = g_fd_path[handle] ? g_fd_path[handle] : "unknown-fd";
    size_t completed = 0;
    while (completed < size) {
        ssize_t result = pwrite((int)handle, (const uint8_t *)buffer + completed, size - completed,
                                offset + (int64_t)completed);
        if (result < 0) {
            if (errno == EINTR) continue;
            return fs_error("FS_WRITE_ERROR", path, RES_FAIL);
        }
        if (!result) return fs_error("FS_WRITE_ERROR zero-write", path, RES_FAIL);
        completed += (size_t)result;
    }
    if (option && (*(const uint32_t *)option & 1) && fsync((int)handle))
        return fs_error("FS_WRITE_FLUSH_ERROR", path, RES_FAIL);
    sky_checkpoint("post_nnMain_write_file");
    LOG("FS_WRITE path=%s offset=%lld requested=%zu wrote=%zu", path, (long long)offset, size, completed);
    return RES_OK;
}

static uint32_t commit_save_data_locked(const char *name) {
    if (!name || strcmp(name, "save") || !g_save_mounted) return fs_error("SAVE_COMMIT_ERROR unsupported-mount", name, RES_FAIL);
    for (int fd = 0; fd < MAX_FD_PATH; fd++) {
        if (!g_fd_save[fd]) continue;
        if (fsync(fd)) return fs_error("SAVE_COMMIT_ERROR", g_fd_path[fd] ? g_fd_path[fd] : "unknown-fd", RES_FAIL);
    }
    sky_checkpoint("post_nnMain_save_commit");
    LOG("SAVE_COMMIT name=save implementation=host-posix");
    return RES_OK;
}

uint32_t sky_fs_commit_save_data(const char *name) {
    pthread_mutex_lock(&g_save_lock);
    uint32_t result = commit_save_data_locked(name);
    pthread_mutex_unlock(&g_save_lock);
    return result;
}

SKY_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "Write an open save fd with pwrite and honor the flush option",
          uint32_t, sky_write_file_entry, uint64_t handle, int64_t offset, const void *buffer, size_t size, const void *option) {
    return sky_fs_write_file(handle, offset, buffer, size, option);
}
SKY_FORCE("_ZN2nn2fs14CommitSaveDataEPKc", "Commit open save files with fsync instead of an IPC transaction",
          uint32_t, sky_commit_save_data_entry, const char *name) { return sky_fs_commit_save_data(name); }
