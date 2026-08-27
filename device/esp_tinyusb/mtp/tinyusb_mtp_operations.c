/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_codec.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_operations.h"
#include "mtp/tinyusb_mtp_transfer.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_ops";

int32_t mtp_op_get_device_info(tud_mtp_cb_data_t *cb_data)
{
    mtp_container_info_t *container = &cb_data->io_container;
    mtp_lock();
    (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.manufacturer);
    (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.model);
    (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.version);
    (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.serial);
    mtp_unlock();
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

int32_t mtp_op_open_close_session(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    mtp_lock();
    if (command->header.code == MTP_OP_OPEN_SESSION) {
        if (mtp_context_get()->mux_protected.session_open) {
            mtp_unlock();
            return MTP_RESP_SESSION_ALREADY_OPEN;
        }
        mtp_context_get()->mux_protected.session_open = true;
    } else {
        if (!mtp_context_get()->mux_protected.session_open) {
            mtp_unlock();
            return MTP_RESP_SESSION_NOT_OPEN;
        }
        mtp_context_get()->mux_protected.session_open = false;
        mtp_context_reset_transfers_locked(MTP_RESP_TRANSACTION_CANCELLED);
    }
    mtp_unlock();
    return MTP_RESP_OK;
}

int32_t mtp_op_get_storage_ids(tud_mtp_cb_data_t *cb_data)
{
    uint32_t ids[CONFIG_TINYUSB_MTP_MAX_STORAGES] = { 0 };
    uint32_t count = 0;
    mtp_lock();
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (mtp_context_get()->mux_protected.storages[i].used) {
            ids[count++] = mtp_context_get()->mux_protected.storages[i].storage_id;
        }
    }
    mtp_unlock();

    (void)mtp_container_add_auint32(&cb_data->io_container, count, ids);
    return tud_mtp_data_send(&cb_data->io_container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

int32_t mtp_op_get_storage_info(tud_mtp_cb_data_t *cb_data)
{
    uint32_t storage_id = cb_data->command_container->params[0];
    char *base_path = NULL;
    char *display_name = NULL;
    bool removable = false;

    mtp_lock();
    struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id_locked(storage_id);
    if (storage) {
        base_path = strdup(storage->base_path);
        display_name = strdup(storage->display_name);
        removable = storage->removable;
    }
    mtp_unlock();
    if (storage == NULL) {
        return MTP_RESP_INVALID_STORAGE_ID;
    }
    if (base_path == NULL || display_name == NULL) {
        ESP_LOGE(TAG, "failed to copy MTP storage info strings");
        free(base_path);
        free(display_name);
        return MTP_RESP_STORE_FULL;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(base_path, &total, &free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to query FATFS info for MTP storage %s: %s", base_path, esp_err_to_name(ret));
        free(base_path);
        free(display_name);
        return MTP_RESP_STORE_NOT_AVAILABLE;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    (void)mtp_container_add_uint16(container, removable ? MTP_STORAGE_TYPE_REMOVABLE_RAM : MTP_STORAGE_TYPE_FIXED_RAM);
    (void)mtp_container_add_uint16(container, MTP_FILESYSTEM_TYPE_GENERIC_HIERARCHICAL);
    (void)mtp_container_add_uint16(container, MTP_ACCESS_CAPABILITY_READ_WRITE);
    (void)mtp_container_add_uint64(container, total);
    (void)mtp_container_add_uint64(container, free_bytes);
    (void)mtp_container_add_uint32(container, 0xFFFFFFFFU);
    (void)mtp_container_add_utf8_string(container, display_name);
    (void)mtp_container_add_utf8_string(container, base_path);
    int32_t resp = tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
    free(base_path);
    free(display_name);
    return resp;
}

static int32_t mtp_scan_storage_locked(tinyusb_mtp_storage_t *storage, uint32_t object_format, uint32_t parent, uint32_t *handles, size_t max_handles,
                                       uint32_t *count)
{
    return parent == MTP_ROOT_PARENT ? mtp_scan_tree_locked(storage, object_format, handles, max_handles, count) :
           mtp_scan_children_locked(storage, parent, object_format, handles, max_handles, count);
}

static int32_t mtp_scan_request_locked(uint32_t storage_id, uint32_t object_format, uint32_t parent, uint32_t *handles, size_t max_handles, uint32_t *count)
{
    if (storage_id == MTP_ROOT_PARENT) {
        struct tinyusb_mtp_storage_s *parent_storage = NULL;
        int32_t ret = mtp_get_parent_storage_for_all_locked(parent, &parent_storage);
        if (ret != 0) {
            return ret;
        }
        if (parent_storage) {
            return mtp_scan_storage_locked(parent_storage, object_format, parent, handles, max_handles, count);
        }
        *count = 0;
        tinyusb_mtp_ctx_t *ctx = mtp_context_get();
        for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
            if (!ctx->mux_protected.storages[i].used) {
                continue;
            }
            uint32_t sub_count = 0;
            uint32_t *sub_handles = handles ? handles + *count : NULL;
            size_t sub_capacity = handles ? max_handles - *count : 0;
            ret = mtp_scan_storage_locked(&ctx->mux_protected.storages[i], object_format, parent, sub_handles, sub_capacity, &sub_count);
            if (ret != 0) {
                return ret;
            }
            *count += sub_count;
        }
        return 0;
    }

    struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id_locked(storage_id);
    if (storage == NULL) {
        return MTP_RESP_INVALID_STORAGE_ID;
    }
    return mtp_scan_storage_locked(storage, object_format, parent, handles, max_handles, count);
}

int32_t mtp_op_get_num_objects(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    uint32_t count = 0;
    mtp_lock();
    int32_t ret = mtp_scan_request_locked(command->params[0], command->params[1], command->params[2], NULL, 0, &count);
    mtp_unlock();
    if (ret != 0) {
        return ret;
    }
    (void)mtp_container_add_uint32(&cb_data->io_container, count);
    return MTP_RESP_OK;
}

int32_t mtp_op_get_object_handles(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_buffered_data(cb_data);
    }
    if (cb_data->phase != MTP_PHASE_COMMAND) {
        return 0;
    }

    const mtp_container_command_t *command = cb_data->command_container;
    const uint32_t payload_capacity = sizeof(uint32_t) + CONFIG_TINYUSB_MTP_MAX_OBJECTS * sizeof(uint32_t);
    uint8_t *payload = malloc(payload_capacity);
    if (payload == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP handle response");
        return MTP_RESP_STORE_FULL;
    }

    uint32_t count = 0;
    uint32_t *handles = (uint32_t *)(void *)(payload + sizeof(count));
    mtp_lock();
    int32_t ret = mtp_scan_request_locked(command->params[0], command->params[1], command->params[2], handles, CONFIG_TINYUSB_MTP_MAX_OBJECTS, &count);
    mtp_unlock();
    if (ret != 0) {
        free(payload);
        return ret;
    }
    memcpy(payload, &count, sizeof(count));
    uint32_t payload_len = sizeof(count) + count * sizeof(*handles);
    return mtp_start_buffered_data(cb_data, MTP_OP_GET_OBJECT_HANDLES, payload, payload_len);
}

int32_t mtp_op_get_object_info(tud_mtp_cb_data_t *cb_data)
{
    uint32_t handle = cb_data->command_container->params[0];
    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
    if (object == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    struct stat st;
    if (stat(object->path, &st) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to stat MTP object info %s: %s", object->path, strerror(err));
        if (err == ENOENT || err == ENOTDIR) {
            mtp_free_object_locked(object);
        }
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_update_object_from_stat(object, &st);
    if (!object->directory && object->size > UINT32_MAX) {
        mtp_unlock();
        return MTP_RESP_OBJECT_TOO_LARGE;
    }

    const char *name = mtp_basename(object->path);
    char name_copy[MTP_MAX_NAME_BYTES + 1];
    (void)snprintf(name_copy, sizeof(name_copy), "%s", name);
    mtp_object_info_header_t info = {
        .storage_id = object->storage->storage_id,
        .object_format = mtp_format_from_name(name_copy, object->directory),
        .protection_status = MTP_PROTECTION_STATUS_NO_PROTECTION,
        .object_compressed_size = object->directory ? 0 : (uint32_t)object->size,
        .thumb_format = MTP_OBJ_FORMAT_UNDEFINED,
        .thumb_compressed_size = 0,
        .thumb_pix_width = 0,
        .thumb_pix_height = 0,
        .image_pix_width = 0,
        .image_pix_height = 0,
        .image_bit_depth = 0,
        .parent_object = object->parent,
        .association_type = object->directory ? MTP_ASSOCIATION_GENERIC_FOLDER : MTP_ASSOCIATION_UNDEFINED,
        .association_desc = 0,
        .sequence_number = 0,
    };
    MTP_TRACEI("MTP object info read: handle=%" PRIu32 " storage=0x%08" PRIx32 " parent=%" PRIu32 " size=%" PRIu64 " dir=%d path=%s",
               handle, info.storage_id, info.parent_object, object->size, object->directory, object->path);
    mtp_unlock();

    mtp_container_info_t *container = &cb_data->io_container;
    (void)mtp_container_add_raw(container, &info, sizeof(info));
    (void)mtp_container_add_utf8_string(container, name_copy);
    (void)mtp_container_add_cstring(container, "");
    (void)mtp_container_add_cstring(container, "");
    (void)mtp_container_add_cstring(container, "");
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

int32_t mtp_op_delete_object(tud_mtp_cb_data_t *cb_data)
{
    mtp_lock();
    if (!mtp_context_get()->mux_protected.session_open) {
        mtp_unlock();
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    uint32_t handle = cb_data->command_container->params[0];
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
    if (object == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    struct tinyusb_mtp_storage_s *storage = object->storage;
    char *path = strdup(object->path);

    if (path == NULL) {
        mtp_unlock();
        ESP_LOGE(TAG, "failed to allocate delete path for handle %" PRIu32, handle);
        return MTP_RESP_STORE_FULL;
    }
    esp_err_t ret = mtp_recursive_delete_path(path);
    if (ret != ESP_OK) {
        mtp_unlock();
        free(path);
        return MTP_RESP_ACCESS_DENIED;
    }

    if (mtp_context_get()->mux_protected.active_edit.active) {
        mtp_object_t *edit_object = mtp_object_from_handle_locked(mtp_context_get()->mux_protected.active_edit.handle);
        if (edit_object != NULL && edit_object->storage == storage && mtp_path_is_child_of(edit_object->path, path)) {
            ESP_LOGI(TAG, "clearing MTP edit for deleted handle %" PRIu32, edit_object->handle);
            mtp_clear_active_edit_locked();
            mtp_clear_partial_write_locked();
        }
    }
    mtp_drop_objects_under_path_locked(storage, path);
    MTP_TRACEI("MTP delete: handle=%" PRIu32 " path=%s", handle, path);
    mtp_unlock();
    free(path);
    return MTP_RESP_OK;
}

int32_t mtp_op_move_object(tud_mtp_cb_data_t *cb_data)
{
    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }
    if (cb_data->phase != MTP_PHASE_COMMAND) {
        return MTP_RESP_OK;
    }

    const mtp_container_command_t *command = cb_data->command_container;
    if (command->header.len < sizeof(mtp_container_header_t) + 3U * sizeof(uint32_t)) {
        ESP_LOGW(TAG, "invalid MTP MoveObject parameter count");
        return MTP_RESP_INVALID_PARAMETER;
    }

    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle_locked(command->params[0]);
    struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id_locked(command->params[1]);
    int32_t ret = MTP_RESP_OK;
    if (object == NULL) {
        ret = MTP_RESP_INVALID_OBJECT_HANDLE;
    } else if (storage == NULL) {
        ret = MTP_RESP_INVALID_STORAGE_ID;
    } else {
        ret = mtp_move_object_locked(object, storage, command->params[2]);
    }
    mtp_unlock();
    return ret;
}


#endif
