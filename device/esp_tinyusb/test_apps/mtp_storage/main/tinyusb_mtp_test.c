/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_TINYUSB_MTP_ENABLED

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "tinyusb_mtp.h"
#include "mtp/tinyusb_mtp_codec.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_transfer.h"
#include "mtp/tinyusb_mtp_transport.h"
#include "mtp/tinyusb_mtp_types.h"
#include "tinyusb_mtp_test.h"

static const char *TAG = "tinyusb_mtp_test";
static bool s_mtp_test_capture_data_send;
static bool s_mtp_test_capture_response_send;
static uint16_t s_mtp_test_response_code;

bool __real_tud_mtp_data_send(mtp_container_info_t *container);
bool __real_tud_mtp_response_send(mtp_container_info_t *container);

bool __wrap_tud_mtp_data_send(mtp_container_info_t *container)
{
    if (s_mtp_test_capture_data_send) {
        return true;
    }
    return __real_tud_mtp_data_send(container);
}

bool __wrap_tud_mtp_response_send(mtp_container_info_t *container)
{
    if (s_mtp_test_capture_response_send) {
        s_mtp_test_response_code = container->header->code;
        return true;
    }
    return __real_tud_mtp_response_send(container);
}

static int32_t tinyusb_mtp_test_write_range_locked(mtp_object_t *object, uint64_t offset, const void *data, size_t size)
{
    if (offset > (uint64_t)LONG_MAX || size > (size_t)((uint64_t)LONG_MAX - offset)) {
        return MTP_RESP_OBJECT_TOO_LARGE;
    }
    FILE *file = fopen(object->path, "r+b");
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open test edit object %s: %s", object->path, strerror(errno));
        return mtp_write_errno_response(errno);
    }
    bool written = fseek(file, (long)offset, SEEK_SET) == 0;
    if (written && size > 0) {
        written = fwrite(data, 1, size, file) == size;
    }
    written = written && fflush(file) == 0;
    int err = errno;
    if (fclose(file) != 0) {
        written = false;
        err = errno;
    }
    if (!written) {
        err = err ? err : EIO;
        ESP_LOGE(TAG, "failed to write test edit object %s: %s", object->path, strerror(err));
        return mtp_write_errno_response(err);
    }
    return mtp_update_object_stat_locked(object);
}

static bool tinyusb_mtp_test_force_session_open(void)
{
    mtp_lock();
    bool session_was_open = mtp_session_set_open_locked(true);
    mtp_unlock();
    return session_was_open;
}

static void tinyusb_mtp_test_restore_session(bool session_was_open)
{
    mtp_lock();
    (void)mtp_session_set_open_locked(session_was_open);
    mtp_unlock();
}

esp_err_t tinyusb_mtp_test_execute(const tinyusb_mtp_test_transaction_t *transaction, uint8_t *io_buffer, uint32_t io_capacity,
                                   tinyusb_mtp_test_result_t *result)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(transaction && result && transaction->param_count <= TINYUSB_MTP_TEST_MAX_PARAMS, ESP_ERR_INVALID_ARG, TAG,
                        "invalid MTP test transaction");
    ESP_RETURN_ON_FALSE(transaction->phase == MTP_PHASE_COMMAND || transaction->phase == MTP_PHASE_DATA, ESP_ERR_INVALID_ARG, TAG,
                        "invalid MTP test phase");
    ESP_RETURN_ON_FALSE(io_buffer || io_capacity == 0, ESP_ERR_INVALID_ARG, TAG, "invalid MTP test I/O buffer");
    ESP_RETURN_ON_FALSE(transaction->payload_len <= io_capacity, ESP_ERR_INVALID_SIZE, TAG, "MTP test payload exceeds I/O buffer");

    mtp_container_command_t command = {
        .header = {
            .len = sizeof(mtp_container_header_t) + transaction->param_count * sizeof(uint32_t),
            .code = transaction->operation,
        },
    };
    memcpy(command.params, transaction->params, transaction->param_count * sizeof(uint32_t));
    mtp_container_header_t header = {
        .len = sizeof(mtp_container_header_t) + transaction->payload_len,
        .code = transaction->operation,
    };
    tud_mtp_cb_data_t cb_data = {
        .phase = transaction->phase,
        .command_container = &command,
        .io_container = {
            .header = &header,
            .payload = io_buffer,
            .payload_bytes = io_capacity,
        },
        .total_xferred_bytes = header.len,
    };

    bool session_was_open = tinyusb_mtp_test_force_session_open();
    s_mtp_test_capture_data_send = transaction->phase == MTP_PHASE_COMMAND;
    int32_t response = mtp_dispatch(&cb_data);
    s_mtp_test_capture_data_send = false;
    uint32_t data_len = transaction->phase == MTP_PHASE_COMMAND && header.len >= sizeof(header) ? header.len - sizeof(header) : 0;
    uint8_t response_param_count = 0;
    if (response > MTP_RESP_UNDEFINED && transaction->phase == MTP_PHASE_DATA && mtp_should_defer_data_response(transaction->operation) &&
            mtp_data_phase_will_complete(&cb_data)) {
        mtp_lock();
        mtp_defer_response_locked(transaction->operation, response);
        mtp_unlock();
        response = 0;
    }
    if (response == 0 && transaction->complete_data) {
        cb_data.xfer_result = XFER_RESULT_SUCCESS;
        cb_data.total_xferred_bytes = header.len;
        header.len = sizeof(header);
        mtp_lock();
        response = mtp_complete_data_locked(&cb_data, &cb_data.io_container);
        mtp_unlock();
        if (header.len >= sizeof(header)) {
            uint32_t response_bytes = header.len - sizeof(header);
            response_param_count = response_bytes / sizeof(uint32_t);
        }
    }
    tinyusb_mtp_test_restore_session(session_was_open);

    result->response_code = response;
    result->data_len = data_len;
    result->response_param_count = response_param_count;
    return ESP_OK;
}

esp_err_t tinyusb_mtp_test_find_object(tinyusb_mtp_storage_handle_t storage, const char *path, uint32_t *object_handle)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(storage && path && object_handle, ESP_ERR_INVALID_ARG, TAG, "invalid test find args");
    struct stat st;
    ESP_RETURN_ON_FALSE(stat(path, &st) == 0, ESP_ERR_NOT_FOUND, TAG, "test object path missing: %s", path);

    mtp_lock();
    if (!mtp_storage_handle_is_valid_locked(storage)) {
        mtp_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    mtp_object_t *object = mtp_get_or_create_object_locked(storage, MTP_ROOT_PARENT, path, &st);
    if (object == NULL) {
        mtp_unlock();
        return ESP_ERR_NO_MEM;
    }
    *object_handle = object->handle;
    mtp_unlock();
    return ESP_OK;
}

esp_err_t tinyusb_mtp_test_get_parent_handle(uint32_t object_handle, uint32_t *parent_handle)
{
    ESP_RETURN_ON_FALSE(parent_handle, ESP_ERR_INVALID_ARG, TAG, "invalid test parent output");

    mtp_lock();
    int32_t response = mtp_object_get_parent_locked(object_handle, parent_handle);
    mtp_unlock();
    return response == MTP_RESP_OK ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t tinyusb_mtp_test_send_object_info(tinyusb_mtp_storage_handle_t storage, uint32_t command_storage_id, uint32_t dataset_storage_id,
                                            uint32_t parent_handle, const char *name, uint32_t object_size, uint32_t *object_handle)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(storage && name && object_handle && mtp_name_is_safe(name), ESP_ERR_INVALID_ARG, TAG, "invalid object info args");

    uint32_t registered_storage_id = storage->storage_id;
    if (command_storage_id == TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED) {
        command_storage_id = registered_storage_id;
    }
    if (dataset_storage_id == TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED) {
        dataset_storage_id = registered_storage_id;
    }

    uint8_t payload[sizeof(mtp_object_info_header_t) + 1U + UINT8_MAX * sizeof(uint16_t) + 3U * sizeof(uint32_t)] = { 0 };
    mtp_object_info_header_t info = {
        .storage_id = dataset_storage_id,
        .object_format = MTP_OBJ_FORMAT_TEXT,
        .protection_status = MTP_PROTECTION_STATUS_NO_PROTECTION,
        .object_compressed_size = object_size,
        .thumb_format = MTP_OBJ_FORMAT_UNDEFINED,
        .association_type = MTP_ASSOCIATION_UNDEFINED,
    };
    memcpy(payload, &info, sizeof(info));

    uint32_t name_len = 0;
    ESP_RETURN_ON_FALSE(mtp_utf8_to_mtp_string_payload(name, payload + sizeof(info), sizeof(payload) - sizeof(info), &name_len), ESP_ERR_INVALID_ARG,
                        TAG, "invalid zero-size object name");

    tinyusb_mtp_test_transaction_t transaction = {
        .operation = MTP_OP_SEND_OBJECT_INFO,
        .phase = MTP_PHASE_DATA,
        .param_count = 2,
        .params = {
            command_storage_id,
            parent_handle,
        },
        .payload_len = sizeof(info) + name_len,
        .complete_data = true,
    };
    tinyusb_mtp_test_result_t result;
    ESP_RETURN_ON_ERROR(tinyusb_mtp_test_execute(&transaction, payload, sizeof(payload), &result), TAG, "failed to execute SendObjectInfo");
    ESP_RETURN_ON_FALSE(result.response_code == MTP_RESP_OK && result.response_param_count == 3, ESP_FAIL, TAG,
                        "SendObjectInfo failed: response=0x%04" PRIx32, (uint32_t)result.response_code);
    memcpy(object_handle, payload + 2U * sizeof(uint32_t), sizeof(*object_handle));
    return ESP_OK;
}

esp_err_t tinyusb_mtp_test_delete_object(uint32_t object_handle)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    uint8_t response[sizeof(uint32_t)] = { 0 };
    tinyusb_mtp_test_result_t result;
    tinyusb_mtp_test_transaction_t transaction = {
        .operation = MTP_OP_DELETE_OBJECT,
        .phase = MTP_PHASE_COMMAND,
        .param_count = 1,
        .params = {
            object_handle,
        },
    };
    ESP_RETURN_ON_ERROR(tinyusb_mtp_test_execute(&transaction, response, sizeof(response), &result), TAG, "failed to execute DeleteObject");
    return result.response_code == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_cancel_read_and_restart(uint32_t cancelled_handle, uint32_t next_handle)
{
    uint8_t io_buffer[64] = { 0 };
    mtp_container_command_t command = {
        .header = {
            .len = sizeof(mtp_container_header_t) + sizeof(uint32_t),
            .code = MTP_OP_GET_OBJECT,
            .transaction_id = 101,
        },
        .params = { cancelled_handle },
    };
    mtp_container_header_t header = { .len = sizeof(header), .code = MTP_OP_GET_OBJECT };
    tud_mtp_cb_data_t transfer = {
        .phase = MTP_PHASE_COMMAND,
        .command_container = &command,
        .io_container = {
            .header = &header,
            .payload = io_buffer,
            .payload_bytes = sizeof(io_buffer),
        },
    };
    bool session_was_open = tinyusb_mtp_test_force_session_open();
    esp_err_t ret = ESP_FAIL;

    s_mtp_test_capture_data_send = true;
    int32_t response = tud_mtp_command_received_cb(&transfer);
    s_mtp_test_capture_data_send = false;
    if (response != 0) {
        goto cleanup;
    }

    uint8_t cancel_payload[sizeof(uint16_t) + sizeof(uint32_t)];
    uint16_t cancel_code = MTP_EVENT_CANCEL_TRANSACTION;
    memcpy(cancel_payload, &cancel_code, sizeof(cancel_code));
    memcpy(cancel_payload + sizeof(cancel_code), &command.header.transaction_id, sizeof(command.header.transaction_id));
    tud_mtp_request_cb_data_t cancel = {
        .bufsize = sizeof(cancel_payload),
        .buf = cancel_payload,
    };
    if (!tud_mtp_request_cancel_cb(&cancel)) {
        goto cleanup;
    }

    transfer.phase = MTP_PHASE_DATA;
    transfer.total_xferred_bytes = sizeof(header) + sizeof(io_buffer);
    s_mtp_test_response_code = 0;
    s_mtp_test_capture_response_send = true;
    response = tud_mtp_data_xfer_cb(&transfer);
    s_mtp_test_capture_response_send = false;
    if (response != 0 || s_mtp_test_response_code != MTP_RESP_TRANSACTION_CANCELLED) {
        goto cleanup;
    }

    command.header.transaction_id++;
    command.params[0] = next_handle;
    transfer.phase = MTP_PHASE_COMMAND;
    transfer.total_xferred_bytes = 0;
    header.len = sizeof(header);
    s_mtp_test_capture_data_send = true;
    response = tud_mtp_command_received_cb(&transfer);
    s_mtp_test_capture_data_send = false;
    if (response == 0) {
        ret = ESP_OK;
    }

cleanup:
    s_mtp_test_capture_data_send = false;
    s_mtp_test_capture_response_send = false;
    (void)tud_mtp_request_cancel_cb(NULL);
    tinyusb_mtp_test_restore_session(session_was_open);
    return ret;
}

static esp_err_t tinyusb_mtp_test_set_string_property(uint32_t object_handle, uint16_t prop_code, const char *value)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(value && mtp_name_is_safe(value), ESP_ERR_INVALID_ARG, TAG, "invalid MTP test property value");

    uint8_t payload[1U + UINT8_MAX * sizeof(uint16_t)] = { 0 };
    uint32_t payload_len = 0;
    ESP_RETURN_ON_FALSE(mtp_utf8_to_mtp_string_payload(value, payload, sizeof(payload), &payload_len), ESP_ERR_INVALID_ARG,
                        TAG, "invalid MTP UTF-8 test property value");

    tinyusb_mtp_test_transaction_t transaction = {
        .operation = MTP_OP_SET_OBJECT_PROP_VALUE,
        .phase = MTP_PHASE_DATA,
        .param_count = 2,
        .params = {
            object_handle,
            prop_code,
        },
        .payload_len = payload_len,
        .complete_data = true,
    };
    tinyusb_mtp_test_result_t result;
    ESP_RETURN_ON_ERROR(tinyusb_mtp_test_execute(&transaction, payload, sizeof(payload), &result), TAG, "failed to execute SetObjectPropValue");
    return result.response_code == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_set_object_name(uint32_t object_handle, const char *name)
{
    return tinyusb_mtp_test_set_string_property(object_handle, MTP_OBJ_PROP_NAME, name);
}

esp_err_t tinyusb_mtp_test_set_object_file_name(uint32_t object_handle, const char *name)
{
    return tinyusb_mtp_test_set_string_property(object_handle, MTP_OBJ_PROP_OBJECT_FILE_NAME, name);
}

esp_err_t tinyusb_mtp_test_begin_edit_object(uint32_t object_handle)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_lock();
    int32_t ret = mtp_begin_edit_object_locked(object_handle);
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_write_partial_object(uint32_t object_handle, uint64_t offset, const void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(data || size == 0, ESP_ERR_INVALID_ARG, TAG, "invalid MTP partial write test buffer");

    mtp_lock();
    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(object_handle, &object);
    if (ret == MTP_RESP_OK) {
        ret = tinyusb_mtp_test_write_range_locked(object, offset, data, size);
    }
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_truncate_object(uint32_t object_handle, uint64_t size)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_lock();
    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(object_handle, &object);
    if (ret == MTP_RESP_OK) {
        ret = mtp_truncate_object_locked(object, size);
    }
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_end_edit_object(uint32_t object_handle)
{
    ESP_RETURN_ON_FALSE(mtp_context_is_installed(), ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_lock();
    int32_t ret = mtp_end_edit_object_locked(object_handle);
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

#endif
