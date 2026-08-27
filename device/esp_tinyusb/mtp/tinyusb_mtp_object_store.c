/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_codec.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_store";

void mtp_free_object_locked(mtp_object_t *object)
{
    free(object->path);
    memset(object, 0, sizeof(*object));
}

void mtp_free_object_table_locked(void)
{
    if (mtp_context_get()->mux_protected.objects == NULL) {
        return;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used) {
            mtp_free_object_locked(&mtp_context_get()->mux_protected.objects[i]);
        }
    }
    free(mtp_context_get()->mux_protected.objects);
    mtp_context_get()->mux_protected.objects = NULL;
}

void mtp_free_storage_table_locked(void)
{
    if (mtp_context_get()->mux_protected.storages == NULL) {
        return;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        free(mtp_context_get()->mux_protected.storages[i].base_path);
        free(mtp_context_get()->mux_protected.storages[i].display_name);
    }
    free(mtp_context_get()->mux_protected.storages);
    mtp_context_get()->mux_protected.storages = NULL;
}

bool mtp_storage_handle_is_valid_locked(const struct tinyusb_mtp_storage_s *storage)
{
    if (storage == NULL || mtp_context_get()->mux_protected.storages == NULL) {
        return false;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (&mtp_context_get()->mux_protected.storages[i] == storage) {
            return mtp_context_get()->mux_protected.storages[i].used;
        }
    }
    return false;
}

size_t mtp_object_count_for_storage_locked(const tinyusb_mtp_storage_t *storage)
{
    size_t count = 0;
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && mtp_context_get()->mux_protected.objects[i].storage == storage) {
            count++;
        }
    }
    return count;
}

int32_t mtp_object_get_parent_locked(uint32_t handle, uint32_t *parent)
{
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
    if (object == NULL) {
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    *parent = object->parent;
    return MTP_RESP_OK;
}

void mtp_clear_objects_for_storage_locked(const struct tinyusb_mtp_storage_s *storage)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && mtp_context_get()->mux_protected.objects[i].storage == storage) {
            mtp_free_object_locked(&mtp_context_get()->mux_protected.objects[i]);
        }
    }
}

void mtp_update_object_from_stat(mtp_object_t *object, const struct stat *st)
{
    object->directory = S_ISDIR(st->st_mode);
    object->size = object->directory ? 0 : (uint64_t)st->st_size;
    object->mtime = st->st_mtime;
}

int32_t mtp_write_errno_response(int err)
{
    return err == ENOSPC ? MTP_RESP_STORE_FULL : MTP_RESP_ACCESS_DENIED;
}

void mtp_remove_created_path(const char *path, bool directory)
{
    if (directory) {
        if (rmdir(path) != 0 && errno != ENOENT) {
            ESP_LOGW(TAG, "failed to remove aborted MTP directory %s: %s", path, strerror(errno));
        }
        return;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "failed to remove aborted MTP file %s: %s", path, strerror(errno));
    }
}

int32_t mtp_update_object_stat_locked(mtp_object_t *object)
{
    struct stat st;
    if (stat(object->path, &st) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to stat edited MTP object %s: %s", object->path, strerror(err));
        if (err == ENOENT || err == ENOTDIR) {
            mtp_free_object_locked(object);
        }
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_update_object_from_stat(object, &st);
    return MTP_RESP_OK;
}

struct tinyusb_mtp_storage_s *mtp_storage_from_id_locked(uint32_t storage_id)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (mtp_context_get()->mux_protected.storages[i].used && mtp_context_get()->mux_protected.storages[i].storage_id == storage_id) {
            return &mtp_context_get()->mux_protected.storages[i];
        }
    }
    return NULL;
}

mtp_object_t *mtp_object_from_handle_locked(uint32_t handle)
{
    if (handle == MTP_OBJECT_HANDLE_INVALID || handle == MTP_ROOT_PARENT) {
        return NULL;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && mtp_context_get()->mux_protected.objects[i].handle == handle) {
            return &mtp_context_get()->mux_protected.objects[i];
        }
    }
    return NULL;
}

uint32_t mtp_normalize_parent_handle(uint32_t parent_handle)
{
    return (parent_handle == MTP_OBJECT_HANDLE_INVALID || parent_handle == MTP_ROOT_PARENT) ? MTP_ROOT_PARENT : parent_handle;
}

int32_t mtp_get_parent_storage_for_all_locked(uint32_t parent_handle, struct tinyusb_mtp_storage_s **storage)
{
    *storage = NULL;
    if (parent_handle == MTP_OBJECT_HANDLE_INVALID || parent_handle == MTP_ROOT_PARENT) {
        return 0;
    }

    mtp_object_t *parent = mtp_object_from_handle_locked(parent_handle);
    if (parent == NULL || !parent->directory) {
        return MTP_RESP_INVALID_PARENT_OBJECT;
    }
    *storage = parent->storage;
    return 0;
}

mtp_object_t *mtp_find_object_by_path_locked(const struct tinyusb_mtp_storage_s *storage, const char *path)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && mtp_context_get()->mux_protected.objects[i].storage == storage && strcmp(mtp_context_get()->mux_protected.objects[i].path, path) == 0) {
            return &mtp_context_get()->mux_protected.objects[i];
        }
    }
    return NULL;
}

static uint32_t mtp_next_handle(void)
{
    for (uint32_t tries = 0; tries < UINT32_MAX - 1U; tries++) {
        uint32_t handle = mtp_context_get()->mux_protected.next_object_handle++;
        if (mtp_context_get()->mux_protected.next_object_handle == MTP_OBJECT_HANDLE_INVALID || mtp_context_get()->mux_protected.next_object_handle == MTP_ROOT_PARENT) {
            mtp_context_get()->mux_protected.next_object_handle = 1;
        }
        if (handle != MTP_OBJECT_HANDLE_INVALID && handle != MTP_ROOT_PARENT && mtp_object_from_handle_locked(handle) == NULL) {
            return handle;
        }
    }
    ESP_LOGE(TAG, "object handle space exhausted");
    return MTP_OBJECT_HANDLE_INVALID;
}

static mtp_object_t *mtp_reclaim_missing_object_locked(void)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        mtp_object_t *object = &mtp_context_get()->mux_protected.objects[i];
        if (!object->used) {
            continue;
        }
        struct stat st;
        if (stat(object->path, &st) == 0 || (errno != ENOENT && errno != ENOTDIR)) {
            continue;
        }
        char *path = strdup(object->path);
        if (path == NULL) {
            ESP_LOGE(TAG, "failed to allocate stale MTP object path");
            continue;
        }
        tinyusb_mtp_storage_t *storage = object->storage;
        mtp_drop_objects_under_path_locked(storage, path);
        free(path);
        ESP_LOGI(TAG, "reclaimed stale MTP object cache entry");
        return object;
    }
    return NULL;
}

mtp_object_t *mtp_get_or_create_object_locked(struct tinyusb_mtp_storage_s *storage, uint32_t parent, const char *path, const struct stat *st)
{
    mtp_object_t *free_slot = NULL;
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        mtp_object_t *object = &mtp_context_get()->mux_protected.objects[i];
        if (!object->used) {
            if (free_slot == NULL) {
                free_slot = object;
            }
        } else if (object->storage == storage && strcmp(object->path, path) == 0) {
            object->parent = parent;
            mtp_update_object_from_stat(object, st);
            return object;
        }
    }

    if (free_slot == NULL) {
        free_slot = mtp_reclaim_missing_object_locked();
    }
    if (free_slot == NULL) {
        ESP_LOGE(TAG, "MTP object table full, cannot add %s", path);
        return NULL;
    }
    free_slot->path = strdup(path);
    if (free_slot->path == NULL) {
        ESP_LOGE(TAG, "failed to allocate object path: %s", path);
        return NULL;
    }
    free_slot->handle = mtp_next_handle();
    if (free_slot->handle == MTP_OBJECT_HANDLE_INVALID) {
        free(free_slot->path);
        free_slot->path = NULL;
        return NULL;
    }
    free_slot->used = true;
    free_slot->parent = parent;
    free_slot->storage = storage;
    mtp_update_object_from_stat(free_slot, st);
    return free_slot;
}

uint32_t mtp_parent_handle_to_dir_locked(struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle, const char **dir_path)
{
    if (parent_handle == MTP_OBJECT_HANDLE_INVALID || parent_handle == MTP_ROOT_PARENT) {
        *dir_path = storage->base_path;
        return 0;
    }

    mtp_object_t *parent = mtp_object_from_handle_locked(parent_handle);
    if (parent == NULL || parent->storage != storage || !parent->directory) {
        return MTP_RESP_INVALID_PARENT_OBJECT;
    }
    *dir_path = parent->path;
    return 0;
}

static uint32_t mtp_begin_scan_generation_locked(void)
{
    tinyusb_mtp_ctx_t *ctx = mtp_context_get();
    uint32_t generation = ctx->mux_protected.next_scan_generation++;
    if (generation != 0) {
        return generation;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        ctx->mux_protected.objects[i].scan_generation = 0;
    }
    ctx->mux_protected.next_scan_generation = 2;
    return 1;
}

static bool mtp_set_scan_path(char **path, size_t *capacity, const char *dir_path, const char *name)
{
    size_t dir_len = strlen(dir_path);
    size_t name_len = strlen(name);
    bool needs_separator = dir_len > 0 && dir_path[dir_len - 1] != '/';
    if (dir_len > SIZE_MAX - name_len - (needs_separator ? 2U : 1U)) {
        return false;
    }
    size_t required = dir_len + name_len + (needs_separator ? 2U : 1U);
    if (required > *capacity) {
        char *new_path = realloc(*path, required);
        if (new_path == NULL) {
            return false;
        }
        *path = new_path;
        *capacity = required;
    }
    memcpy(*path, dir_path, dir_len);
    size_t offset = dir_len;
    if (needs_separator) {
        (*path)[offset++] = '/';
    }
    memcpy(*path + offset, name, name_len + 1U);
    return true;
}

int32_t mtp_scan_children_locked(struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle, uint32_t object_format, uint32_t *handles, size_t max_handles,
                                 uint32_t *count)
{
    const char *dir_path = NULL;
    uint32_t parent_resp = mtp_parent_handle_to_dir_locked(storage, parent_handle, &dir_path);
    if (parent_resp != 0) {
        return (int32_t)parent_resp;
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "failed to open MTP directory %s: %s", dir_path, strerror(errno));
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    const uint32_t normalized_parent = mtp_normalize_parent_handle(parent_handle);
    const bool refresh_cache = handles != NULL;
    const uint32_t scan_generation = refresh_cache ? mtp_begin_scan_generation_locked() : 0;

    struct dirent *entry = NULL;
    char *path = NULL;
    size_t path_capacity = 0;
    uint32_t found = 0;
    int scan_error = 0;
    while (true) {
        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            scan_error = errno;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (mtp_name_is_internal_temp(entry->d_name)) {
            continue;
        }

        if (!mtp_set_scan_path(&path, &path_capacity, dir_path, entry->d_name)) {
            ESP_LOGE(TAG, "failed to allocate MTP scan path");
            free(path);
            closedir(dir);
            return MTP_RESP_STORE_FULL;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "failed to stat MTP object %s: %s", path, strerror(errno));
            continue;
        }

        const bool is_dir = S_ISDIR(st.st_mode);
        const uint16_t fmt = mtp_format_from_name(entry->d_name, is_dir);
        bool format_matches = object_format == 0 || object_format == 0xFFFFFFFFU || (uint16_t)object_format == fmt;
        if (format_matches) {
            if (handles && found >= max_handles) {
                ESP_LOGE(TAG, "MTP object handle result table full while scanning %s", dir_path);
                free(path);
                closedir(dir);
                return MTP_RESP_STORE_FULL;
            }
            if (handles) {
                mtp_object_t *object = mtp_get_or_create_object_locked(storage, normalized_parent, path, &st);
                if (object == NULL) {
                    free(path);
                    closedir(dir);
                    return MTP_RESP_GENERAL_ERROR;
                }
                handles[found] = object->handle;
                object->scan_generation = scan_generation;
            }
            found++;
        } else if (refresh_cache) {
            mtp_object_t *cached = mtp_find_object_by_path_locked(storage, path);
            if (cached != NULL) {
                cached->parent = normalized_parent;
                mtp_update_object_from_stat(cached, &st);
                cached->scan_generation = scan_generation;
            }
        }
    }

    free(path);
    closedir(dir);
    if (scan_error != 0) {
        ESP_LOGE(TAG, "failed to scan MTP directory %s: %s", dir_path, strerror(scan_error));
        return MTP_RESP_STORE_NOT_AVAILABLE;
    }

    if (refresh_cache) {
        for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
            mtp_object_t *object = &mtp_context_get()->mux_protected.objects[i];
            if (object->used && object->scan_generation != scan_generation && object->storage == storage && object->parent == normalized_parent) {
                mtp_drop_objects_under_path_locked(storage, object->path);
            }
        }
    }
    *count = found;
    return 0;
}

int32_t mtp_scan_tree_locked(struct tinyusb_mtp_storage_s *storage, uint32_t object_format, uint32_t *handles, size_t max_handles, uint32_t *count)
{
    uint32_t *all_handles = malloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS * sizeof(*all_handles));
    if (all_handles == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP tree scan buffer");
        return MTP_RESP_STORE_FULL;
    }

    uint32_t total = 0;
    int32_t ret = mtp_scan_children_locked(storage, MTP_ROOT_PARENT, 0, all_handles, CONFIG_TINYUSB_MTP_MAX_OBJECTS, &total);
    for (uint32_t index = 0; ret == 0 && index < total; index++) {
        mtp_object_t *object = mtp_object_from_handle_locked(all_handles[index]);
        if (object == NULL) {
            ESP_LOGE(TAG, "missing MTP object while scanning storage tree: handle=%" PRIu32, all_handles[index]);
            ret = MTP_RESP_GENERAL_ERROR;
            break;
        }
        if (!object->directory) {
            continue;
        }

        uint32_t child_count = 0;
        ret = mtp_scan_children_locked(storage, object->handle, 0, all_handles + total, CONFIG_TINYUSB_MTP_MAX_OBJECTS - total, &child_count);
        total += child_count;
    }

    uint32_t found = 0;
    for (uint32_t index = 0; ret == 0 && index < total; index++) {
        mtp_object_t *object = mtp_object_from_handle_locked(all_handles[index]);
        if (object == NULL) {
            ret = MTP_RESP_GENERAL_ERROR;
            break;
        }
        uint16_t format = mtp_format_from_name(mtp_basename(object->path), object->directory);
        if (object_format != 0 && object_format != MTP_ROOT_PARENT && (uint16_t)object_format != format) {
            continue;
        }
        if (handles != NULL && found >= max_handles) {
            ret = MTP_RESP_STORE_FULL;
            break;
        }
        if (handles != NULL) {
            handles[found] = object->handle;
        }
        found++;
    }

    free(all_handles);
    if (ret == 0) {
        *count = found;
    }
    return ret;
}

esp_err_t mtp_recursive_delete_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat delete target %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            ESP_LOGE(TAG, "failed to delete file %s: %s", path, strerror(errno));
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "failed to open delete directory %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char *child = mtp_join_path(path, entry->d_name);
        if (child == NULL) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        esp_err_t child_ret = mtp_recursive_delete_path(child);
        free(child);
        if (child_ret != ESP_OK) {
            ret = child_ret;
            break;
        }
    }
    closedir(dir);

    if (ret != ESP_OK) {
        return ret;
    }
    if (rmdir(path) != 0) {
        ESP_LOGE(TAG, "failed to delete directory %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

void mtp_drop_objects_under_path_locked(const struct tinyusb_mtp_storage_s *storage, const char *path)
{
    // Free descendants first so path may safely point into the object table.
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && mtp_context_get()->mux_protected.objects[i].storage == storage && strcmp(mtp_context_get()->mux_protected.objects[i].path, path) != 0 &&
                mtp_path_is_child_of(mtp_context_get()->mux_protected.objects[i].path, path)) {
            mtp_free_object_locked(&mtp_context_get()->mux_protected.objects[i]);
        }
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && mtp_context_get()->mux_protected.objects[i].storage == storage && strcmp(mtp_context_get()->mux_protected.objects[i].path, path) == 0) {
            mtp_free_object_locked(&mtp_context_get()->mux_protected.objects[i]);
            break;
        }
    }
}

static void mtp_drop_cached_children_under_path(const struct tinyusb_mtp_storage_s *storage, const char *path, const mtp_object_t *keep)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (mtp_context_get()->mux_protected.objects[i].used && &mtp_context_get()->mux_protected.objects[i] != keep && mtp_context_get()->mux_protected.objects[i].storage == storage && mtp_path_is_child_of(mtp_context_get()->mux_protected.objects[i].path, path)) {
            mtp_free_object_locked(&mtp_context_get()->mux_protected.objects[i]);
        }
    }
}

int32_t mtp_rename_object_locked(mtp_object_t *object, const char *new_name)
{
    if (!mtp_name_is_safe(new_name) || mtp_name_is_internal_temp(new_name)) {
        ESP_LOGW(TAG, "invalid MTP rename target: %s", new_name ? new_name : "(null)");
        return MTP_RESP_INVALID_OBJECT_PROP_VALUE;
    }

    const char *parent_dir = NULL;
    uint32_t parent_resp = mtp_parent_handle_to_dir_locked(object->storage, object->parent, &parent_dir);
    if (parent_resp != 0) {
        return (int32_t)parent_resp;
    }

    char *new_path = mtp_join_path(parent_dir, new_name);
    if (new_path == NULL) {
        return MTP_RESP_STORE_FULL;
    }
    if (strcmp(object->path, new_path) == 0) {
        free(new_path);
        return MTP_RESP_OK;
    }

    struct stat st;
    if (stat(new_path, &st) == 0) {
        ESP_LOGW(TAG, "MTP rename target already exists: %s", new_path);
        free(new_path);
        return MTP_RESP_ACCESS_DENIED;
    }
    if (errno != ENOENT) {
        ESP_LOGE(TAG, "failed to stat MTP rename target %s: %s", new_path, strerror(errno));
        free(new_path);
        return MTP_RESP_ACCESS_DENIED;
    }

    char *old_path = strdup(object->path);
    if (old_path == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP rename source path");
        free(new_path);
        return MTP_RESP_STORE_FULL;
    }
    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "failed to rename MTP object %s to %s: %s", old_path, new_path, strerror(errno));
        free(old_path);
        free(new_path);
        return MTP_RESP_ACCESS_DENIED;
    }

    // Drop cached descendants after directory rename; future scans will recreate them with fresh paths.
    if (object->directory) {
        mtp_drop_cached_children_under_path(object->storage, old_path, object);
    }
    free(object->path);
    object->path = new_path;
    MTP_TRACEI("MTP rename: handle=%" PRIu32 " from=%s to=%s", object->handle, old_path, object->path);
    if (stat(object->path, &st) == 0) {
        mtp_update_object_from_stat(object, &st);
    }
    free(old_path);
    return MTP_RESP_OK;
}

int32_t mtp_move_object_locked(mtp_object_t *object, struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle)
{
    if (object->storage != storage) {
        ESP_LOGW(TAG, "cross-storage MTP move is not supported: handle=%" PRIu32, object->handle);
        return MTP_RESP_ACCESS_DENIED;
    }

    const char *parent_dir = NULL;
    uint32_t parent_resp = mtp_parent_handle_to_dir_locked(storage, parent_handle, &parent_dir);
    if (parent_resp != 0) {
        return (int32_t)parent_resp;
    }
    if (object->directory && mtp_path_is_child_of(parent_dir, object->path)) {
        ESP_LOGW(TAG, "invalid MTP directory move target: %s", parent_dir);
        return MTP_RESP_INVALID_PARENT_OBJECT;
    }

    const char *name = strrchr(object->path, '/');
    name = name ? name + 1 : object->path;
    char *new_path = mtp_join_path(parent_dir, name);
    if (new_path == NULL) {
        return MTP_RESP_STORE_FULL;
    }
    if (strcmp(object->path, new_path) == 0) {
        free(new_path);
        return MTP_RESP_OK;
    }

    struct stat st;
    if (stat(new_path, &st) == 0) {
        ESP_LOGW(TAG, "MTP move target already exists: %s", new_path);
        free(new_path);
        return MTP_RESP_ACCESS_DENIED;
    }
    if (errno != ENOENT) {
        ESP_LOGE(TAG, "failed to stat MTP move target %s: %s", new_path, strerror(errno));
        free(new_path);
        return MTP_RESP_ACCESS_DENIED;
    }

    char *old_path = strdup(object->path);
    if (old_path == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP move source path");
        free(new_path);
        return MTP_RESP_STORE_FULL;
    }
    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "failed to move MTP object %s to %s: %s", old_path, new_path, strerror(errno));
        free(old_path);
        free(new_path);
        return MTP_RESP_ACCESS_DENIED;
    }

    // Drop cached descendants after directory move; future scans recreate them with fresh paths.
    if (object->directory) {
        mtp_drop_cached_children_under_path(storage, old_path, object);
    }
    free(object->path);
    object->path = new_path;
    object->parent = mtp_normalize_parent_handle(parent_handle);
    MTP_TRACEI("MTP move: handle=%" PRIu32 " from=%s to=%s", object->handle, old_path, object->path);
    if (stat(object->path, &st) == 0) {
        mtp_update_object_from_stat(object, &st);
    }
    free(old_path);
    return MTP_RESP_OK;
}


#endif
