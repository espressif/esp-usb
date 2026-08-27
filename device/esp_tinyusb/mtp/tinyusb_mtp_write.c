/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_codec.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_transfer.h"
#include "mtp/tinyusb_mtp_write.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_write";

int32_t mtp_op_begin_edit_object(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    mtp_lock();
    int32_t ret = mtp_begin_edit_object_locked(command->params[0]);
    mtp_unlock();
    return ret;
}

int32_t mtp_op_send_partial_object(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    mtp_container_info_t *container = &cb_data->io_container;
    uint32_t handle = command->params[0];

    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    if (cb_data->phase == MTP_PHASE_COMMAND) {
        uint64_t offset = (uint64_t)command->params[1] | ((uint64_t)command->params[2] << 32);
        uint32_t data_len = command->params[3];
        if (data_len > MTP_MAX_DATA_BYTES) {
            return MTP_RESP_OBJECT_TOO_LARGE;
        }
        mtp_lock();
        mtp_clear_partial_write_locked();
        mtp_object_t *object = NULL;
        int32_t ret = mtp_get_active_edit_object_locked(handle, &object);
        if (ret == MTP_RESP_OK && offset > (uint64_t)LONG_MAX) {
            ESP_LOGW(TAG, "MTP partial write offset is too large: offset=%" PRIu64, offset);
            ret = MTP_RESP_OBJECT_TOO_LARGE;
        }
        if (ret == MTP_RESP_OK) {
            container->header->len = sizeof(mtp_container_header_t) + data_len;
            if (data_len == 0) {
                mtp_update_object_stat_locked(object);
                mtp_unlock();
                return MTP_RESP_OK;
            }
            FILE *file = fopen(object->path, "r+b");
            if (file == NULL) {
                ESP_LOGE(TAG, "failed to open MTP edit object %s: %s", object->path, strerror(errno));
                ret = mtp_write_errno_response(errno);
            } else if (fseek(file, (long)offset, SEEK_SET) != 0) {
                int err = errno;
                ESP_LOGE(TAG, "failed to seek MTP edit object %s: %s", object->path, strerror(err));
                fclose(file);
                ret = mtp_write_errno_response(err);
            } else {
                mtp_partial_write_t *partial = &mtp_context_get()->mux_protected.partial_write;
                partial->active = true;
                partial->file = file;
                partial->handle = handle;
                partial->offset = offset;
                partial->expected_size = data_len;
                MTP_TRACEI("MTP partial write begin: handle=%" PRIu32 " offset=%" PRIu64 " len=%" PRIu32, handle, offset, data_len);
            }
            if (ret == MTP_RESP_OK && !tud_mtp_data_receive(container)) {
                ESP_LOGE(TAG, "failed to arm MTP partial object receive");
                mtp_clear_partial_write_locked();
                ret = MTP_RESP_DEVICE_BUSY;
            }
        }
        mtp_unlock();
        return ret == MTP_RESP_OK ? 0 : ret;
    }

    if (cb_data->phase != MTP_PHASE_DATA) {
        return 0;
    }

    mtp_lock();
    mtp_partial_write_t *partial = &mtp_context_get()->mux_protected.partial_write;
    if (!partial->active || partial->file == NULL || partial->handle != handle) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    uint32_t remaining = partial->expected_size - partial->written;
    size_t to_write = container->payload_bytes < remaining ? container->payload_bytes : remaining;
    int32_t ret = MTP_RESP_OK;
    if (to_write > 0 && fwrite(container->payload, 1, to_write, partial->file) != to_write) {
        int err = errno;
        ESP_LOGE(TAG, "failed to write partial MTP object handle=%" PRIu32 ": %s", handle, strerror(err));
        ret = mtp_write_errno_response(err);
    } else {
        partial->written += (uint32_t)to_write;
        if (partial->written < partial->expected_size) {
            ret = tud_mtp_data_receive(container) ? 0 : MTP_RESP_DEVICE_BUSY;
        }
    }
    if (ret > MTP_RESP_UNDEFINED && ret != MTP_RESP_OK) {
        mtp_clear_partial_write_locked();
    }
    mtp_unlock();
    return ret;
}

int32_t mtp_op_truncate_object(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    uint32_t handle = command->params[0];
    uint64_t length = (uint64_t)command->params[1] | ((uint64_t)command->params[2] << 32);
    mtp_lock();
    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(handle, &object);
    if (ret == MTP_RESP_OK) {
        ret = mtp_truncate_object_locked(object, length);
    }
    mtp_unlock();
    return ret;
}

int32_t mtp_op_end_edit_object(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    uint32_t handle = command->params[0];
    mtp_lock();
    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(handle, &object);
    if (ret == MTP_RESP_OK) {
        MTP_TRACEI("MTP edit end: handle=%" PRIu32 " path=%s size=%" PRIu64, handle, object->path, object->size);
    }
    ret = ret == MTP_RESP_OK ? mtp_end_edit_object_locked(handle) : ret;
    mtp_unlock();
    return ret;
}

static tinyusb_mtp_storage_t *mtp_resolve_send_storage_locked(uint32_t command_storage_id, uint32_t dataset_storage_id)
{
    uint32_t storage_id = command_storage_id;
    if (storage_id == MTP_STORAGE_ID_UNSPECIFIED || storage_id == MTP_STORAGE_ID_ALL) {
        storage_id = dataset_storage_id;
    }
    if (storage_id != MTP_STORAGE_ID_UNSPECIFIED && storage_id != MTP_STORAGE_ID_ALL) {
        return mtp_storage_from_id_locked(storage_id);
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (mtp_context_get()->mux_protected.storages[i].used) {
            return &mtp_context_get()->mux_protected.storages[i];
        }
    }
    return NULL;
}

int32_t mtp_op_send_object_info(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    if (cb_data->phase == MTP_PHASE_COMMAND) {
        if (!tud_mtp_data_receive(&cb_data->io_container)) {
            ESP_LOGE(TAG, "failed to receive MTP object info");
            return MTP_RESP_DEVICE_BUSY;
        }
        return 0;
    }
    if (cb_data->phase != MTP_PHASE_DATA) {
        return 0;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    if (container->header->len < sizeof(mtp_container_header_t) + sizeof(mtp_object_info_header_t) + 1) {
        ESP_LOGW(TAG, "invalid MTP object info length: %" PRIu32, container->header->len);
        return MTP_RESP_INVALID_DATASET;
    }
    uint32_t payload_total = container->header->len - sizeof(mtp_container_header_t);
    if (payload_total > container->payload_bytes) {
        ESP_LOGW(TAG, "multi-packet MTP object info is not supported: total=%" PRIu32 " chunk=%" PRIu32, payload_total, container->payload_bytes);
        return MTP_RESP_INVALID_DATASET;
    }

    mtp_object_info_header_t *info = (mtp_object_info_header_t *)cb_data->io_container.payload;
    mtp_lock();
    struct tinyusb_mtp_storage_s *storage = mtp_resolve_send_storage_locked(command->params[0], info->storage_id);
    if (storage == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_STORAGE_ID;
    }

    const char *parent_dir = NULL;
    uint32_t parent_handle = command->params[1];
    uint32_t parent_resp = mtp_parent_handle_to_dir_locked(storage, parent_handle, &parent_dir);
    if (parent_resp != 0) {
        mtp_unlock();
        return (int32_t)parent_resp;
    }

    char name[MTP_MAX_NAME_BYTES + 1];
    const uint8_t *name_payload = cb_data->io_container.payload + sizeof(mtp_object_info_header_t);
    if (!mtp_utf16_to_utf8_name(name_payload, payload_total - sizeof(mtp_object_info_header_t), name, sizeof(name))) {
        ESP_LOGW(TAG, "invalid MTP object name");
        mtp_unlock();
        return MTP_RESP_INVALID_PARAMETER;
    }

    char *path = mtp_join_path(parent_dir, name);
    if (path == NULL) {
        mtp_unlock();
        return MTP_RESP_STORE_FULL;
    }

    mtp_abort_pending_write_locked(MTP_RESP_TRANSACTION_CANCELLED);

    const bool is_dir = info->object_format == MTP_OBJ_FORMAT_ASSOCIATION || info->association_type == MTP_ASSOCIATION_GENERIC_FOLDER;
    if (!is_dir && info->object_compressed_size > MTP_MAX_DATA_BYTES) {
        free(path);
        mtp_unlock();
        return MTP_RESP_OBJECT_TOO_LARGE;
    }

    struct stat st;
    bool created_path = false;
    bool replace_existing = false;
    char *write_path = NULL;
    char *backup_path = NULL;
    if (is_dir) {
        if (stat(path, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                ESP_LOGW(TAG, "MTP directory target is not a directory: %s", path);
                free(path);
                mtp_unlock();
                return MTP_RESP_ACCESS_DENIED;
            }
        } else if (mkdir(path, 0777) == 0) {
            created_path = true;
        } else {
            ESP_LOGE(TAG, "failed to create MTP directory %s: %s", path, strerror(errno));
            free(path);
            mtp_unlock();
            return MTP_RESP_ACCESS_DENIED;
        }
    } else if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "MTP file target is a directory: %s", path);
            free(path);
            mtp_unlock();
            return MTP_RESP_ACCESS_DENIED;
        }
        replace_existing = true;
    } else {
        FILE *file = fopen(path, "wb");
        if (file == NULL) {
            ESP_LOGE(TAG, "failed to create MTP file %s: %s", path, strerror(errno));
            free(path);
            mtp_unlock();
            return MTP_RESP_ACCESS_DENIED;
        }
        fclose(file);
        created_path = true;
    }

    bool created_object = created_path || mtp_find_object_by_path_locked(storage, path) == NULL;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat MTP object %s: %s", path, strerror(errno));
        if (created_path) {
            mtp_remove_created_path(path, is_dir);
        }
        free(path);
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }
    uint32_t object_parent = mtp_normalize_parent_handle(parent_handle);
    mtp_object_t *object = mtp_get_or_create_object_locked(storage, object_parent, path, &st);
    if (object == NULL) {
        if (created_path) {
            mtp_remove_created_path(path, is_dir);
        }
        memset(&mtp_context_get()->mux_protected.pending_write, 0, sizeof(mtp_context_get()->mux_protected.pending_write));
        free(path);
        mtp_unlock();
        return MTP_RESP_STORE_FULL;
    }

    if (replace_existing) {
        write_path = mtp_make_internal_path(parent_dir, object->handle, MTP_TEMP_NAME_PREFIX);
        if (write_path == NULL) {
            if (created_object) {
                mtp_free_object_locked(object);
            }
            free(path);
            mtp_unlock();
            return MTP_RESP_STORE_FULL;
        }
        backup_path = mtp_make_internal_path(parent_dir, object->handle, MTP_BACKUP_NAME_PREFIX);
        if (backup_path == NULL) {
            if (created_object) {
                mtp_free_object_locked(object);
            }
            free(write_path);
            free(path);
            mtp_unlock();
            return MTP_RESP_STORE_FULL;
        }
        FILE *file = fopen(write_path, "wb");
        if (file == NULL) {
            ESP_LOGE(TAG, "failed to create MTP replacement temp file %s: %s", write_path, strerror(errno));
            if (created_object) {
                mtp_free_object_locked(object);
            }
            free(backup_path);
            free(write_path);
            free(path);
            mtp_unlock();
            return MTP_RESP_ACCESS_DENIED;
        }
        fclose(file);
        created_path = true;
    }

    mtp_context_get()->mux_protected.pending_write.handle = object->handle;
    mtp_context_get()->mux_protected.pending_write.storage_id = storage->storage_id;
    mtp_context_get()->mux_protected.pending_write.parent_handle = object_parent;
    mtp_context_get()->mux_protected.pending_write.expected_size = info->object_compressed_size;
    mtp_context_get()->mux_protected.pending_write.path = strdup(path);
    mtp_context_get()->mux_protected.pending_write.write_path = write_path;
    mtp_context_get()->mux_protected.pending_write.backup_path = backup_path;
    mtp_context_get()->mux_protected.pending_write.state = !is_dir && info->object_compressed_size > 0 ? MTP_WRITE_WAITING_DATA : MTP_WRITE_INFO_PREPARED;
    mtp_context_get()->mux_protected.pending_write.created_path = created_path;
    mtp_context_get()->mux_protected.pending_write.created_object = created_object;
    mtp_context_get()->mux_protected.pending_write.replace_existing = replace_existing;
    mtp_context_get()->mux_protected.pending_write.directory = is_dir;
    if (mtp_context_get()->mux_protected.pending_write.path == NULL) {
        ESP_LOGE(TAG, "failed to allocate pending write path");
        if (created_object) {
            mtp_free_object_locked(object);
        }
        if (created_path) {
            mtp_remove_created_path(write_path ? write_path : path, is_dir);
        }
        free(backup_path);
        free(write_path);
        memset(&mtp_context_get()->mux_protected.pending_write, 0, sizeof(mtp_context_get()->mux_protected.pending_write));
        free(path);
        mtp_unlock();
        return MTP_RESP_STORE_FULL;
    }
    write_path = NULL;
    backup_path = NULL;
    if (!is_dir && !created_path) {
        ESP_LOGW(TAG, "MTP file target was not prepared for write: %s", path);
        if (created_object) {
            mtp_free_object_locked(object);
        }
        free(mtp_context_get()->mux_protected.pending_write.path);
        free(mtp_context_get()->mux_protected.pending_write.write_path);
        free(mtp_context_get()->mux_protected.pending_write.backup_path);
        memset(&mtp_context_get()->mux_protected.pending_write, 0, sizeof(mtp_context_get()->mux_protected.pending_write));
        free(path);
        mtp_unlock();
        return MTP_RESP_ACCESS_DENIED;
    }
    MTP_TRACEI("MTP object info write: handle=%" PRIu32 " storage=0x%08" PRIx32 " parent=%" PRIu32 " size=%" PRIu64 " dir=%d replace=%d path=%s",
               object->handle, storage->storage_id, mtp_context_get()->mux_protected.pending_write.parent_handle, mtp_context_get()->mux_protected.pending_write.expected_size, is_dir, replace_existing, path);
    free(path);
    mtp_unlock();
    return 0;
}

int32_t mtp_op_send_object(tud_mtp_cb_data_t *cb_data)
{
    mtp_container_info_t *container = &cb_data->io_container;

    mtp_lock();
    mtp_pending_write_t *pending = &mtp_context_get()->mux_protected.pending_write;
    bool zero_size_send = pending->state == MTP_WRITE_ZERO_SIZE_COMPLETE;
    if (!zero_size_send && (pending->state != MTP_WRITE_WAITING_DATA && pending->state != MTP_WRITE_RECEIVING_DATA)) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    if (cb_data->phase == MTP_PHASE_COMMAND) {
        if (zero_size_send) {
            container->header->len = sizeof(mtp_container_header_t);
            int32_t resp = tud_mtp_data_receive(container) ? 0 : MTP_RESP_DEVICE_BUSY;
            if (resp != 0) {
                pending->state = MTP_WRITE_IDLE;
                ESP_LOGE(TAG, "failed to receive zero-size MTP object");
            }
            mtp_unlock();
            return resp;
        }
        container->header->len = sizeof(mtp_container_header_t) + (uint32_t)pending->expected_size;
        int32_t resp = tud_mtp_data_receive(container) ? 0 : MTP_RESP_DEVICE_BUSY;
        if (resp != 0) {
            mtp_abort_pending_write_locked(resp);
        } else {
            pending->state = MTP_WRITE_RECEIVING_DATA;
            MTP_TRACEI("MTP object write begin: handle=%" PRIu32 " bytes=%" PRIu64 " path=%s",
                       pending->handle, pending->expected_size, pending->path);
        }
        mtp_unlock();
        return resp;
    }

    if (zero_size_send) {
        mtp_unlock();
        return 0;
    }

    if (pending->file == NULL) {
        const char *write_path = pending->write_path ? pending->write_path : pending->path;
        pending->file = fopen(write_path, "wb");
        if (pending->file == NULL) {
            ESP_LOGE(TAG, "failed to open MTP object for write %s: %s", write_path, strerror(errno));
            mtp_abort_pending_write_locked(MTP_RESP_ACCESS_DENIED);
            mtp_unlock();
            return MTP_RESP_ACCESS_DENIED;
        }
    }

    uint64_t remaining = pending->expected_size - pending->written;
    size_t to_write = container->payload_bytes;
    if ((uint64_t)to_write > remaining) {
        to_write = (size_t)remaining;
    }

    if (to_write > 0) {
        if (fwrite(container->payload, 1, to_write, pending->file) != to_write) {
            const char *write_path = pending->write_path ? pending->write_path : pending->path;
            ESP_LOGE(TAG, "failed to write MTP object %s: %s", write_path, strerror(errno));
            mtp_abort_pending_write_locked(MTP_RESP_STORE_FULL);
            mtp_unlock();
            return MTP_RESP_STORE_FULL;
        }
        pending->written += to_write;
    }

    if (pending->written < pending->expected_size) {
        if (!tud_mtp_data_receive(container)) {
            mtp_abort_pending_write_locked(MTP_RESP_DEVICE_BUSY);
            mtp_unlock();
            return MTP_RESP_DEVICE_BUSY;
        }
    }
    mtp_unlock();
    return 0;
}

#endif
