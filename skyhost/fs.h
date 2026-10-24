#pragma once

#include "account.h"
#include <stddef.h>
#include <stdint.h>

uint32_t sky_fs_enable_global_cache(void);
unsigned sky_fs_is_enabled_access_log(void);
uint32_t sky_fs_query_cache_size(size_t *out);
uint32_t sky_fs_mount_rom(const char *name, void *cache, size_t size);
uint32_t sky_fs_open_file(uint64_t *out, const char *path, int mode);
uint32_t sky_fs_read_file(size_t *out, uint64_t handle, int64_t offset, void *buffer, size_t size);
uint32_t sky_fs_read_file_with_option(size_t *out, uint64_t handle, int64_t offset,
    void *buffer, size_t size, const uint32_t *option);
uint32_t sky_fs_read_file_exact(uint64_t handle, int64_t offset, void *buffer,
    size_t size, const uint32_t *option);
uint32_t sky_fs_get_file_size(int64_t *out, uint64_t handle);
uint32_t sky_fs_get_entry_type(int32_t *out, const char *path);
uint32_t sky_fs_ensure_save_data(const sky_account_uid_t *uid);
uint32_t sky_fs_mount_save_data(const char *name, const sky_account_uid_t *uid);
uint32_t sky_fs_unmount(const char *name);
uint32_t sky_fs_create_directory(const char *path);
uint32_t sky_fs_get_save_data_size(int64_t *out_size, int64_t *out_journal_size, const sky_account_uid_t *uid);
uint32_t sky_fs_extend_save_data(const sky_account_uid_t *uid, int64_t size, int64_t journal_size);

typedef struct {
    char name[0x301];
    uint8_t padding[3];
    int32_t type;
    int64_t size;
} sky_fs_directory_entry_t;

uint32_t sky_fs_open_directory(uint64_t *out, const char *path, int mode);
uint32_t sky_fs_get_directory_entry_count(int64_t *out, uint64_t handle);
uint32_t sky_fs_read_directory(int64_t *out, sky_fs_directory_entry_t *entries, uint64_t handle, int64_t maximum);
void sky_fs_close_directory(uint64_t handle);
uint32_t sky_fs_delete_file(const char *path);
uint32_t sky_fs_create_file(const char *path, int64_t size);
uint32_t sky_fs_write_file(uint64_t handle, int64_t offset, const void *buffer, size_t size, const void *option);
uint32_t sky_fs_commit_save_data(const char *name);
void sky_fs_close_file(uint64_t handle);
