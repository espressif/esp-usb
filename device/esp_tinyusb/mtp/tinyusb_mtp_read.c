/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_read.h"
#include "mtp/tinyusb_mtp_transfer.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_read";

static int32_t mtp_start_read(tud_mtp_cb_data_t *cb_data, uint64_t offset, uint64_t length)
{
    uint32_t handle = cb_data->command_container->params[0];
    char *path = NULL;
    mtp_lock();
    tinyusb_mtp_ctx_t *ctx = mtp_context_get();
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
    if (object == NULL || object->directory) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    struct stat st;
    if (stat(object->path, &st) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to stat MTP object before read %s: %s", object->path, strerror(err));
        if (err == ENOENT || err == ENOTDIR) {
            mtp_free_object_locked(object);
        }
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_update_object_from_stat(object, &st);
    if (object->size > UINT32_MAX) {
        mtp_unlock();
        return MTP_RESP_OBJECT_TOO_LARGE;
    }
    path = strdup(object->path);
    if (path == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP read path");
        mtp_unlock();
        return MTP_RESP_STORE_FULL;
    }
    uint64_t object_size = object->size;
    struct tinyusb_mtp_storage_s *storage = object->storage;
    if (offset > object_size) {
        free(path);
        mtp_unlock();
        return MTP_RESP_INVALID_PARAMETER;
    }
    if (length > object_size - offset) {
        length = object_size - offset;
    }
    if (length > MTP_MAX_DATA_BYTES) {
        free(path);
        mtp_unlock();
        return MTP_RESP_OBJECT_TOO_LARGE;
    }
    if (offset > (uint64_t)LONG_MAX) {
        ESP_LOGW(TAG, "MTP read offset is too large: offset=%" PRIu64, offset);
        free(path);
        mtp_unlock();
        return MTP_RESP_OBJECT_TOO_LARGE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open MTP object for read %s: %s", path, strerror(errno));
        free(path);
        mtp_unlock();
        return MTP_RESP_ACCESS_DENIED;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "failed to seek MTP object %s: %s", path, strerror(errno));
        fclose(file);
        free(path);
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }

    mtp_clear_active_read_locked();
    mtp_active_read_t *read = &ctx->mux_protected.active_read;
    read->active = true;
    read->file = file;
    read->storage = storage;
    read->handle = handle;
    read->expected = length;
    read->remaining = length;
    read->path = path;
    path = NULL;

    mtp_container_info_t *container = &cb_data->io_container;
    size_t first_len = length < container->payload_bytes ? (size_t)length : container->payload_bytes;
    if (first_len > 0 && fread(container->payload, 1, first_len, file) != first_len) {
        ESP_LOGE(TAG, "failed to read first MTP object chunk from %s", read->path);
        mtp_clear_active_read_locked();
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }
    read->sent = first_len;
    read->remaining -= first_len;
    container->header->len = sizeof(mtp_container_header_t) + (uint32_t)length;
    MTP_TRACEI("MTP object read begin: handle=%" PRIu32 " offset=%" PRIu64 " bytes=%" PRIu64 " object_size=%" PRIu64 " first=%u path=%s",
               handle, offset, length, object_size, (unsigned)first_len, read->path);
    int32_t resp = tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
    if (resp != 0) {
        mtp_clear_active_read_locked();
    }
    mtp_unlock();
    return resp;
}

static int32_t mtp_continue_read(tud_mtp_cb_data_t *cb_data)
{
    mtp_lock();
    mtp_active_read_t *read = &mtp_context_get()->mux_protected.active_read;
    if (!read->active || read->file == NULL) {
        ESP_LOGW(TAG, "MTP read continuation without active object");
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    size_t chunk = read->remaining < container->payload_bytes ? (size_t)read->remaining : container->payload_bytes;
    if (chunk == 0) {
        mtp_unlock();
        return 0;
    }
    if (fread(container->payload, 1, chunk, read->file) != chunk) {
        ESP_LOGE(TAG, "failed to read MTP object chunk from %s", read->path ? read->path : "(unknown)");
        mtp_clear_active_read_locked();
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }
    read->sent += chunk;
    read->remaining -= chunk;
    int32_t resp = tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
    if (resp != 0) {
        mtp_clear_active_read_locked();
    }
    mtp_unlock();
    return resp;
}

int32_t mtp_op_get_object(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_COMMAND) {
        return mtp_start_read(cb_data, 0, UINT64_MAX);
    }
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_read(cb_data);
    }
    return 0;
}

int32_t mtp_op_get_partial_object(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_COMMAND) {
        return mtp_start_read(cb_data, cb_data->command_container->params[1], cb_data->command_container->params[2]);
    }
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_read(cb_data);
    }
    return 0;
}

int32_t mtp_op_get_partial_object64(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_COMMAND) {
        const mtp_container_command_t *command = cb_data->command_container;
        uint64_t offset = (uint64_t)command->params[1] | ((uint64_t)command->params[2] << 32);
        return mtp_start_read(cb_data, offset, command->params[3]);
    }
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_read(cb_data);
    }
    return 0;
}

#endif
