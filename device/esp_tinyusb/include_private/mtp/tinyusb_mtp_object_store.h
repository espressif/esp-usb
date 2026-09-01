/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "mtp/tinyusb_mtp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mtp_storage_handle_is_valid_locked(const tinyusb_mtp_storage_t *storage);
tinyusb_mtp_storage_t *mtp_storage_from_id_locked(uint32_t storage_id);
mtp_object_t *mtp_object_from_handle_locked(uint32_t handle);
mtp_object_t *mtp_find_object_by_path_locked(const tinyusb_mtp_storage_t *storage, const char *path);
mtp_object_t *mtp_get_or_create_object_locked(tinyusb_mtp_storage_t *storage, uint32_t parent, const char *path, const struct stat *st);
uint32_t mtp_parent_handle_to_dir_locked(tinyusb_mtp_storage_t *storage, uint32_t parent_handle, const char **dir_path);
int32_t mtp_scan_children_locked(tinyusb_mtp_storage_t *storage, uint32_t parent_handle, uint32_t object_format, uint32_t *handles,
                                 size_t max_handles, uint32_t *count);
int32_t mtp_scan_tree_locked(tinyusb_mtp_storage_t *storage, uint32_t object_format, uint32_t *handles, size_t max_handles, uint32_t *count);
int32_t mtp_update_object_stat_locked(mtp_object_t *object);
int32_t mtp_write_errno_response(int err);
void mtp_remove_created_path(const char *path, bool directory);
int32_t mtp_get_parent_storage_for_all_locked(uint32_t parent_handle, tinyusb_mtp_storage_t **storage);
int32_t mtp_rename_object_locked(mtp_object_t *object, const char *new_name);
int32_t mtp_move_object_locked(mtp_object_t *object, tinyusb_mtp_storage_t *storage, uint32_t parent_handle);
esp_err_t mtp_recursive_delete_path(const char *path);
void mtp_free_object_locked(mtp_object_t *object);
void mtp_free_object_table_locked(void);
void mtp_free_storage_table_locked(void);
void mtp_clear_objects_for_storage_locked(const tinyusb_mtp_storage_t *storage);
void mtp_drop_objects_under_path_locked(const tinyusb_mtp_storage_t *storage, const char *path);
uint32_t mtp_normalize_parent_handle(uint32_t parent_handle);
char *mtp_make_internal_path(const char *dir, uint32_t handle, const char *prefix);
bool mtp_name_is_internal_temp(const char *name);
void mtp_update_object_from_stat(mtp_object_t *object, const struct stat *st);
size_t mtp_object_count_for_storage_locked(const tinyusb_mtp_storage_t *storage);
int32_t mtp_object_get_parent_locked(uint32_t handle, uint32_t *parent);

#ifdef __cplusplus
}
#endif
