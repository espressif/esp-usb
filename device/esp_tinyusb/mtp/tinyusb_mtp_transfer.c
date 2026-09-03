/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_codec.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_transfer.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_xfer";

void mtp_clear_pending_write_locked(void)
{
    if (mtp_context_get()->mux_protected.pending_write.file) {
        fclose(mtp_context_get()->mux_protected.pending_write.file);
    }
    free(mtp_context_get()->mux_protected.pending_write.path);
    free(mtp_context_get()->mux_protected.pending_write.write_path);
    free(mtp_context_get()->mux_protected.pending_write.backup_path);
    memset(&mtp_context_get()->mux_protected.pending_write, 0, sizeof(mtp_context_get()->mux_protected.pending_write));
}

void mtp_clear_active_read_locked(void)
{
    if (mtp_context_get()->mux_protected.active_read.file) {
        fclose(mtp_context_get()->mux_protected.active_read.file);
    }
    free(mtp_context_get()->mux_protected.active_read.path);
    memset(&mtp_context_get()->mux_protected.active_read, 0, sizeof(mtp_context_get()->mux_protected.active_read));
}

void mtp_clear_active_edit_locked(void)
{
    memset(&mtp_context_get()->mux_protected.active_edit, 0, sizeof(mtp_context_get()->mux_protected.active_edit));
}

void mtp_clear_partial_write_locked(void)
{
    if (mtp_context_get()->mux_protected.partial_write.file) {
        if (fclose(mtp_context_get()->mux_protected.partial_write.file) != 0) {
            ESP_LOGE(TAG, "failed to close partial MTP write: %s", strerror(errno));
        }
    }
    memset(&mtp_context_get()->mux_protected.partial_write, 0, sizeof(mtp_context_get()->mux_protected.partial_write));
}

void mtp_clear_active_buffer_locked(void)
{
    free(mtp_context_get()->mux_protected.active_buffer.data);
    memset(&mtp_context_get()->mux_protected.active_buffer, 0, sizeof(mtp_context_get()->mux_protected.active_buffer));
}

void mtp_clear_pending_prop_set_locked(void)
{
    mtp_context_get()->mux_protected.pending_prop_set_active = false;
    mtp_context_get()->mux_protected.pending_prop_set_response = 0;
}

void mtp_clear_deferred_response_locked(void)
{
    mtp_context_get()->mux_protected.deferred_response_active = false;
    mtp_context_get()->mux_protected.deferred_response_op = 0;
    mtp_context_get()->mux_protected.deferred_response_code = 0;
}

void mtp_context_reset_transfers_locked(int32_t response)
{
    mtp_abort_pending_write_locked(response);
    mtp_clear_active_read_locked();
    mtp_clear_active_edit_locked();
    mtp_clear_partial_write_locked();
    mtp_clear_active_buffer_locked();
    mtp_clear_deferred_response_locked();
    mtp_clear_pending_prop_set_locked();
    mtp_context_get()->mux_protected.cancelled_transaction_pending = false;
    mtp_context_get()->mux_protected.cancelled_transaction_id = 0;
}

void mtp_transfer_detach_storage_locked(tinyusb_mtp_storage_t *storage)
{
    if (mtp_context_get()->mux_protected.pending_write.storage_id == storage->storage_id) {
        mtp_abort_pending_write_locked(MTP_RESP_STORE_NOT_AVAILABLE);
    }
    if (mtp_context_get()->mux_protected.active_read.storage == storage) {
        mtp_clear_active_read_locked();
    }
    if (mtp_context_get()->mux_protected.active_edit.active) {
        mtp_object_t *object = mtp_object_from_handle_locked(mtp_context_get()->mux_protected.active_edit.handle);
        if (object == NULL || object->storage == storage) {
            mtp_clear_active_edit_locked();
        }
    }
    if (mtp_context_get()->mux_protected.partial_write.active) {
        mtp_object_t *object = mtp_object_from_handle_locked(mtp_context_get()->mux_protected.partial_write.handle);
        if (object == NULL || object->storage == storage) {
            mtp_clear_partial_write_locked();
        }
    }
}

bool mtp_transfer_is_idle_locked(void)
{
    mtp_write_state_t state = mtp_context_get()->mux_protected.pending_write.state;
    return state != MTP_WRITE_WAITING_DATA && state != MTP_WRITE_RECEIVING_DATA && mtp_context_get()->mux_protected.pending_write.path == NULL &&
           !mtp_context_get()->mux_protected.active_read.active && !mtp_context_get()->mux_protected.partial_write.active &&
           !mtp_context_get()->mux_protected.active_buffer.active && !mtp_context_get()->mux_protected.deferred_response_active &&
           !mtp_context_get()->mux_protected.pending_prop_set_active && !mtp_context_get()->mux_protected.cancelled_transaction_pending;
}

int32_t mtp_end_edit_object_locked(uint32_t handle)
{
    mtp_object_t *object = NULL;
    int32_t response = mtp_get_active_edit_object_locked(handle, &object);
    if (response == MTP_RESP_OK) {
        response = mtp_update_object_stat_locked(object);
    }
    mtp_clear_partial_write_locked();
    if (mtp_context_get()->mux_protected.active_edit.active && mtp_context_get()->mux_protected.active_edit.handle == handle) {
        mtp_clear_active_edit_locked();
    }
    return response;
}

int32_t mtp_get_active_edit_object_locked(uint32_t handle, mtp_object_t **object)
{
    if (!mtp_context_get()->mux_protected.active_edit.active || mtp_context_get()->mux_protected.active_edit.handle != handle) {
        ESP_LOGW(TAG, "MTP object edit was not opened for handle %" PRIu32, handle);
        return MTP_RESP_ACCESS_DENIED;
    }

    mtp_object_t *found = mtp_object_from_handle_locked(handle);
    if (found == NULL || found->directory) {
        ESP_LOGW(TAG, "invalid MTP edit object handle %" PRIu32, handle);
        mtp_clear_active_edit_locked();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    *object = found;
    return MTP_RESP_OK;
}

int32_t mtp_begin_edit_object_locked(uint32_t handle)
{
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
    if (object == NULL || object->directory) {
        ESP_LOGW(TAG, "invalid MTP begin edit handle %" PRIu32, handle);
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    if (mtp_context_get()->mux_protected.active_edit.active && mtp_context_get()->mux_protected.active_edit.handle != handle) {
        ESP_LOGW(TAG, "MTP edit already active for handle %" PRIu32, mtp_context_get()->mux_protected.active_edit.handle);
        return MTP_RESP_DEVICE_BUSY;
    }

    int32_t ret = mtp_update_object_stat_locked(object);
    if (ret != MTP_RESP_OK) {
        return ret;
    }
    mtp_clear_partial_write_locked();
    mtp_context_get()->mux_protected.active_edit.active = true;
    mtp_context_get()->mux_protected.active_edit.handle = handle;
    MTP_TRACEI("MTP edit begin: handle=%" PRIu32 " path=%s size=%" PRIu64, handle, object->path, object->size);
    return MTP_RESP_OK;
}

int32_t mtp_truncate_object_locked(mtp_object_t *object, uint64_t length)
{
    if (length > (uint64_t)LONG_MAX) {
        ESP_LOGW(TAG, "MTP truncate length is too large: length=%" PRIu64, length);
        return MTP_RESP_OBJECT_TOO_LARGE;
    }
    if (truncate(object->path, (off_t)length) != 0) {
        ESP_LOGE(TAG, "failed to truncate MTP object %s: %s", object->path, strerror(errno));
        return mtp_write_errno_response(errno);
    }
    int32_t ret = mtp_update_object_stat_locked(object);
    if (ret == MTP_RESP_OK) {
        MTP_TRACEI("MTP truncate: handle=%" PRIu32 " length=%" PRIu64 " path=%s", object->handle, length, object->path);
    }
    return ret;
}

void mtp_abort_pending_write_locked(int32_t error_response)
{
    uint32_t handle = mtp_context_get()->mux_protected.pending_write.handle;
    const char *path = mtp_context_get()->mux_protected.pending_write.path;
    const char *created_path = mtp_context_get()->mux_protected.pending_write.write_path ? mtp_context_get()->mux_protected.pending_write.write_path : path;

    if (handle == MTP_OBJECT_HANDLE_INVALID && path == NULL && mtp_context_get()->mux_protected.pending_write.file == NULL) {
        mtp_clear_pending_write_locked();
        return;
    }
    ESP_LOGW(TAG, "aborting pending MTP write: response=0x%04" PRIx32, (uint32_t)error_response);
    if (mtp_context_get()->mux_protected.pending_write.file) {
        fclose(mtp_context_get()->mux_protected.pending_write.file);
        mtp_context_get()->mux_protected.pending_write.file = NULL;
    }
    if (created_path && mtp_context_get()->mux_protected.pending_write.created_path) {
        mtp_remove_created_path(created_path, mtp_context_get()->mux_protected.pending_write.directory);
    }
    if (handle != MTP_OBJECT_HANDLE_INVALID && mtp_context_get()->mux_protected.pending_write.created_object) {
        mtp_object_t *object = mtp_object_from_handle_locked(handle);
        if (object) {
            mtp_free_object_locked(object);
        }
    }
    mtp_context_get()->mux_protected.pending_write.error_response = error_response;
    mtp_clear_pending_write_locked();
}

bool mtp_should_defer_data_response(uint16_t op_code)
{
    return op_code == MTP_OP_SEND_OBJECT_INFO || op_code == MTP_OP_SEND_OBJECT || op_code == MTP_OP_SET_OBJECT_PROP_VALUE ||
           op_code == MTP_OP_ANDROID_SEND_PARTIAL_OBJECT;
}

bool mtp_data_phase_will_complete(const tud_mtp_cb_data_t *cb_data)
{
    uint32_t total_len = cb_data->io_container.header->len;
    return cb_data->total_xferred_bytes >= total_len || (cb_data->io_container.payload_bytes == 0 && cb_data->total_xferred_bytes > 0);
}

void mtp_defer_response_locked(uint16_t op_code, int32_t response_code)
{
    if (!mtp_context_get()->mux_protected.deferred_response_active) {
        mtp_context_get()->mux_protected.deferred_response_active = true;
        mtp_context_get()->mux_protected.deferred_response_op = op_code;
        mtp_context_get()->mux_protected.deferred_response_code = response_code;
    }
}

static int32_t mtp_take_deferred_response_locked(uint16_t op_code, int32_t default_response)
{
    if (!mtp_context_get()->mux_protected.deferred_response_active || mtp_context_get()->mux_protected.deferred_response_op != op_code) {
        return default_response;
    }
    int32_t response_code = mtp_context_get()->mux_protected.deferred_response_code;
    mtp_clear_deferred_response_locked();
    return response_code;
}

static int32_t mtp_commit_replacement_locked(void)
{
    const char *target_path = mtp_context_get()->mux_protected.pending_write.path;
    const char *write_path = mtp_context_get()->mux_protected.pending_write.write_path;
    const char *backup_path = mtp_context_get()->mux_protected.pending_write.backup_path;
    if (target_path == NULL || write_path == NULL || backup_path == NULL) {
        ESP_LOGE(TAG, "invalid MTP replacement state");
        return MTP_RESP_GENERAL_ERROR;
    }

    if (rename(target_path, backup_path) != 0) {
        ESP_LOGE(TAG, "failed to backup old MTP object %s to %s: %s", target_path, backup_path, strerror(errno));
        mtp_abort_pending_write_locked(MTP_RESP_ACCESS_DENIED);
        return MTP_RESP_ACCESS_DENIED;
    }

    if (rename(write_path, target_path) != 0) {
        ESP_LOGE(TAG, "failed to replace MTP object %s with %s: %s", target_path, write_path, strerror(errno));
        if (rename(backup_path, target_path) != 0) {
            ESP_LOGE(TAG, "failed to restore old MTP object %s from %s: %s", target_path, backup_path, strerror(errno));
        }
        mtp_remove_created_path(write_path, false);
        mtp_context_get()->mux_protected.pending_write.created_path = false;
        return MTP_RESP_GENERAL_ERROR;
    }

    if (unlink(backup_path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "failed to remove MTP replacement backup %s: %s", backup_path, strerror(errno));
    }
    mtp_context_get()->mux_protected.pending_write.created_path = false;
    return MTP_RESP_OK;
}

// Finish both SendObject data writes and zero-size SendObjectInfo creations.
static int32_t mtp_finish_pending_write_locked(void)
{
    mtp_pending_write_t *pending = &mtp_context_get()->mux_protected.pending_write;
    if (pending->handle == MTP_OBJECT_HANDLE_INVALID || pending->path == NULL) {
        ESP_LOGE(TAG, "invalid MTP pending write completion state");
        return MTP_RESP_GENERAL_ERROR;
    }

    if (pending->file) {
        fclose(pending->file);
        pending->file = NULL;
    }

    if (pending->replace_existing) {
        int32_t replace_resp = mtp_commit_replacement_locked();
        if (replace_resp != MTP_RESP_OK) {
            return replace_resp;
        }
    }

    struct stat st;
    if (stat(pending->path, &st) == 0) {
        mtp_object_t *object = mtp_object_from_handle_locked(pending->handle);
        if (object) {
            mtp_update_object_from_stat(object, &st);
        }
    } else {
        ESP_LOGW(TAG, "failed to stat written MTP object %s: %s", pending->path, strerror(errno));
    }
    MTP_TRACEI("MTP object write complete: handle=%" PRIu32 " bytes=%" PRIu64 " replace=%d path=%s", pending->handle, pending->written,
               pending->replace_existing, pending->path);
    mtp_clear_pending_write_locked();
    return MTP_RESP_OK;
}

int32_t mtp_start_buffered_data(tud_mtp_cb_data_t *cb_data, uint16_t op_code, uint8_t *data, uint32_t len)
{
    mtp_container_info_t *container = &cb_data->io_container;
    mtp_lock();
    mtp_clear_active_buffer_locked();
    mtp_context_get()->mux_protected.active_buffer.active = true;
    mtp_context_get()->mux_protected.active_buffer.op_code = op_code;
    mtp_context_get()->mux_protected.active_buffer.data = data;
    mtp_context_get()->mux_protected.active_buffer.len = len;

    // Keep the full response in active_buffer; only the first packet includes the MTP container header.
    uint32_t first_len = len < container->payload_bytes ? len : container->payload_bytes;
    if (first_len > 0) {
        memcpy(container->payload, data, first_len);
    }
    container->header->len = sizeof(mtp_container_header_t) + len;
    if (!tud_mtp_data_send(container)) {
        mtp_clear_active_buffer_locked();
        mtp_unlock();
        return MTP_RESP_DEVICE_BUSY;
    }
    mtp_unlock();
    return 0;
}

int32_t mtp_continue_buffered_data(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    mtp_lock();
    if (!mtp_context_get()->mux_protected.active_buffer.active || mtp_context_get()->mux_protected.active_buffer.op_code != command->header.code || mtp_context_get()->mux_protected.active_buffer.data == NULL) {
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    uint32_t offset = cb_data->total_xferred_bytes > sizeof(mtp_container_header_t) ? cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) : 0;
    if (offset >= mtp_context_get()->mux_protected.active_buffer.len) {
        mtp_unlock();
        return 0;
    }

    uint32_t chunk = mtp_context_get()->mux_protected.active_buffer.len - offset;
    if (chunk > container->payload_bytes) {
        chunk = container->payload_bytes;
    }
    memcpy(container->payload, mtp_context_get()->mux_protected.active_buffer.data + offset, chunk);
    if (!tud_mtp_data_send(container)) {
        mtp_clear_active_buffer_locked();
        mtp_unlock();
        return MTP_RESP_DEVICE_BUSY;
    }
    mtp_unlock();
    return 0;
}

static int32_t mtp_complete_send_object_info_locked(const tud_mtp_cb_data_t *cb_data, mtp_container_info_t *response)
{
    mtp_pending_write_t *pending = &mtp_context_get()->mux_protected.pending_write;
    int32_t ret = MTP_RESP_OK;

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, abort_pending, TAG,
                      "MTP SendObjectInfo transfer failed: result=%d", cb_data->xfer_result);

    int32_t deferred_resp = mtp_take_deferred_response_locked(MTP_OP_SEND_OBJECT_INFO, MTP_RESP_OK);
    ESP_GOTO_ON_FALSE(deferred_resp == MTP_RESP_OK, deferred_resp, abort_pending, TAG, "MTP SendObjectInfo deferred response=0x%04" PRIx32,
                      (uint32_t)deferred_resp);
    ESP_GOTO_ON_FALSE(pending->handle != MTP_OBJECT_HANDLE_INVALID, MTP_RESP_GENERAL_ERROR, done, TAG, "missing pending MTP object info");

    bool expects_object_data = pending->state == MTP_WRITE_WAITING_DATA;
    bool accepts_zero_size_send = !pending->directory && pending->expected_size == 0;
    uint32_t storage_id = pending->storage_id;
    uint32_t parent_handle = pending->parent_handle;
    uint32_t handle = pending->handle;
    if (!expects_object_data) {
        ret = mtp_finish_pending_write_locked();
        ESP_GOTO_ON_FALSE(ret == MTP_RESP_OK, ret, clear_pending, TAG, "failed to finish MTP object info write: response=0x%04" PRIx32,
                          (uint32_t)ret);
        pending->state = accepts_zero_size_send ? MTP_WRITE_ZERO_SIZE_COMPLETE : MTP_WRITE_IDLE;
    }
    (void)mtp_container_add_uint32(response, storage_id);
    (void)mtp_container_add_uint32(response, parent_handle);
    (void)mtp_container_add_uint32(response, handle);
    return ret;

abort_pending:
    if (cb_data->xfer_result != XFER_RESULT_SUCCESS) {
        mtp_clear_deferred_response_locked();
    }
    mtp_abort_pending_write_locked(ret);
    goto done;
clear_pending:
    if (pending->path != NULL) {
        mtp_clear_pending_write_locked();
    }
done:
    return ret;
}

static int32_t mtp_complete_send_object_locked(const tud_mtp_cb_data_t *cb_data)
{
    mtp_pending_write_t *pending = &mtp_context_get()->mux_protected.pending_write;
    int32_t ret = MTP_RESP_OK;

    if (pending->state == MTP_WRITE_ZERO_SIZE_COMPLETE) {
        pending->state = MTP_WRITE_IDLE;
        if (cb_data->xfer_result != XFER_RESULT_SUCCESS || cb_data->total_xferred_bytes != sizeof(mtp_container_header_t)) {
            ESP_LOGW(TAG, "invalid zero-size MTP SendObject transfer: result=%d bytes=%" PRIu32, cb_data->xfer_result, cb_data->total_xferred_bytes);
            return MTP_RESP_INCOMPLETE_TRANSFER;
        }
        return MTP_RESP_OK;
    }

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, abort_pending, TAG,
                      "MTP SendObject transfer failed: result=%d", cb_data->xfer_result);

    int32_t deferred_resp = mtp_take_deferred_response_locked(MTP_OP_SEND_OBJECT, MTP_RESP_OK);
    ESP_GOTO_ON_FALSE(deferred_resp == MTP_RESP_OK, deferred_resp, done, TAG, "MTP SendObject deferred response=0x%04" PRIx32,
                      (uint32_t)deferred_resp);
    ESP_GOTO_ON_FALSE(pending->state == MTP_WRITE_RECEIVING_DATA && pending->handle != MTP_OBJECT_HANDLE_INVALID && pending->path != NULL,
                      MTP_RESP_GENERAL_ERROR, done,
                      TAG, "missing pending MTP object data");

    ESP_GOTO_ON_FALSE(pending->written == pending->expected_size, MTP_RESP_INCOMPLETE_TRANSFER, abort_pending, TAG,
                      "incomplete MTP object write: expected=%" PRIu64 " written=%" PRIu64, pending->expected_size, pending->written);

    ret = mtp_finish_pending_write_locked();
    ESP_GOTO_ON_FALSE(ret == MTP_RESP_OK, ret, clear_pending, TAG, "failed to finish MTP object write: response=0x%04" PRIx32, (uint32_t)ret);
    return ret;

abort_pending:
    if (cb_data->xfer_result != XFER_RESULT_SUCCESS) {
        mtp_clear_deferred_response_locked();
    }
    mtp_abort_pending_write_locked(ret);
    return ret;
clear_pending:
    if (pending->path != NULL) {
        mtp_clear_pending_write_locked();
    }
done:
    return ret;
}

static int32_t mtp_complete_set_object_prop_value_locked(const tud_mtp_cb_data_t *cb_data)
{
    int32_t ret = MTP_RESP_OK;

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, clear_deferred, TAG,
                      "MTP SetObjectPropValue transfer failed: result=%d", cb_data->xfer_result);

    ret = mtp_take_deferred_response_locked(MTP_OP_SET_OBJECT_PROP_VALUE, 0);
    if (ret <= MTP_RESP_UNDEFINED) {
        ESP_GOTO_ON_FALSE(mtp_context_get()->mux_protected.pending_prop_set_active, MTP_RESP_GENERAL_ERROR, done, TAG,
                          "missing pending MTP property set response");
        ret = mtp_context_get()->mux_protected.pending_prop_set_response;
    }

done:
    mtp_clear_pending_prop_set_locked();
    return ret;
clear_deferred:
    mtp_clear_deferred_response_locked();
    goto done;
}

static int32_t mtp_complete_partial_write_locked(const tud_mtp_cb_data_t *cb_data)
{
    mtp_partial_write_t *partial = &mtp_context_get()->mux_protected.partial_write;
    int32_t ret = MTP_RESP_OK;

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, clear_deferred, TAG,
                      "MTP partial write transfer failed: result=%d", cb_data->xfer_result);

    int32_t deferred_resp = mtp_take_deferred_response_locked(MTP_OP_ANDROID_SEND_PARTIAL_OBJECT, MTP_RESP_OK);
    ESP_GOTO_ON_FALSE(deferred_resp == MTP_RESP_OK, deferred_resp, done, TAG, "MTP partial write deferred response=0x%04" PRIx32,
                      (uint32_t)deferred_resp);
    ESP_GOTO_ON_FALSE(partial->active && partial->written == partial->expected_size, MTP_RESP_INCOMPLETE_TRANSFER, done, TAG,
                      "incomplete MTP partial write: expected=%" PRIu32 " written=%" PRIu32, partial->expected_size, partial->written);
    ESP_GOTO_ON_FALSE(partial->file != NULL, MTP_RESP_GENERAL_ERROR, done, TAG, "missing partial MTP write file");
    if (fflush(partial->file) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to flush partial MTP write: %s", strerror(err));
        ret = mtp_write_errno_response(err);
        goto done;
    }
    FILE *file = partial->file;
    partial->file = NULL;
    if (fclose(file) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to close partial MTP write: %s", strerror(err));
        ret = mtp_write_errno_response(err);
        goto done;
    }
    mtp_object_t *object = mtp_object_from_handle_locked(partial->handle);
    ESP_GOTO_ON_FALSE(object != NULL, MTP_RESP_INVALID_OBJECT_HANDLE, done, TAG, "partial MTP write object disappeared");
    ret = mtp_update_object_stat_locked(object);
    if (ret == MTP_RESP_OK) {
        MTP_TRACEI("MTP partial write complete: handle=%" PRIu32 " offset=%" PRIu64 " len=%" PRIu32 " path=%s", partial->handle, partial->offset,
                   partial->written, object->path);
    }

done:
    mtp_clear_partial_write_locked();
    return ret;
clear_deferred:
    mtp_clear_deferred_response_locked();
    goto done;
}

static int32_t mtp_complete_read_locked(const tud_mtp_cb_data_t *cb_data)
{
    mtp_active_read_t *read = &mtp_context_get()->mux_protected.active_read;
    if (read->active) {
        MTP_TRACEI("MTP object read complete: handle=%" PRIu32 " sent=%" PRIu64 "/%" PRIu64 " result=%d path=%s", read->handle, read->sent,
                   read->expected, cb_data->xfer_result, read->path ? read->path : "(unknown)");
    } else {
        ESP_LOGW(TAG, "MTP object read complete without active state");
    }
    mtp_clear_active_read_locked();
    return (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
}

static int32_t mtp_complete_partial_read_locked(const tud_mtp_cb_data_t *cb_data, uint32_t *sent)
{
    mtp_active_read_t *read = &mtp_context_get()->mux_protected.active_read;
    *sent = cb_data->total_xferred_bytes > sizeof(mtp_container_header_t) ? cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) : 0;
    if (read->active) {
        *sent = read->sent > UINT32_MAX ? UINT32_MAX : (uint32_t)read->sent;
        MTP_TRACEI("MTP partial object read complete: handle=%" PRIu32 " sent=%" PRIu64 "/%" PRIu64 " result=%d path=%s", read->handle, read->sent,
                   read->expected, cb_data->xfer_result, read->path ? read->path : "(unknown)");
    } else {
        ESP_LOGW(TAG, "MTP partial object read complete without active state");
    }
    mtp_clear_active_read_locked();
    return (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
}

int32_t mtp_complete_data_locked(const tud_mtp_cb_data_t *cb_data, mtp_container_info_t *response)
{
    const mtp_container_command_t *command = cb_data->command_container;
    int32_t resp_code = MTP_RESP_GENERAL_ERROR;

    switch (command->header.code) {
    case MTP_OP_SEND_OBJECT_INFO:
        resp_code = mtp_complete_send_object_info_locked(cb_data, response);
        break;
    case MTP_OP_SEND_OBJECT:
        resp_code = mtp_complete_send_object_locked(cb_data);
        break;
    case MTP_OP_SET_OBJECT_PROP_VALUE:
        resp_code = mtp_complete_set_object_prop_value_locked(cb_data);
        break;
    case MTP_OP_ANDROID_SEND_PARTIAL_OBJECT:
        resp_code = mtp_complete_partial_write_locked(cb_data);
        break;
    case MTP_OP_GET_OBJECT:
        resp_code = mtp_complete_read_locked(cb_data);
        break;
    case MTP_OP_GET_PARTIAL_OBJECT:
    case MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64: {
        uint32_t sent = 0;
        resp_code = mtp_complete_partial_read_locked(cb_data, &sent);
        (void)mtp_container_add_uint32(response, sent);
        break;
    }
    default:
        mtp_clear_active_buffer_locked();
        resp_code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
        break;
    }
    return resp_code;
}

#endif
