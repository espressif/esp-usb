/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_TINYUSB_MTP_ENABLED

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "tinyusb_mtp.h"
#include "tinyusb_mtp_impl.h"
#include "tinyusb_mtp_test.h"

static const char *TAG = "tinyusb_mtp_test";

static bool tinyusb_mtp_test_force_session_open(void)
{
    mtp_lock();
    bool session_was_open = s_mtp.mux_protected.session_open;
    s_mtp.mux_protected.session_open = true;
    mtp_unlock();
    return session_was_open;
}

static void tinyusb_mtp_test_restore_session(bool session_was_open)
{
    mtp_lock();
    s_mtp.mux_protected.session_open = session_was_open;
    mtp_unlock();
}

esp_err_t tinyusb_mtp_test_find_object(tinyusb_mtp_storage_handle_t storage, const char *path, uint32_t *object_handle)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(storage && path && object_handle, ESP_ERR_INVALID_ARG, TAG, "invalid test find args");
    struct stat st;
    ESP_RETURN_ON_FALSE(stat(path, &st) == 0, ESP_ERR_NOT_FOUND, TAG, "test object path missing: %s", path);

    mtp_lock();
    if (!mtp_storage_handle_is_valid_locked(storage)) {
        mtp_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    mtp_object_t *object = mtp_get_or_create_object(storage, MTP_ROOT_PARENT, path, &st);
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
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used && s_mtp.mux_protected.objects[i].handle == object_handle) {
            *parent_handle = s_mtp.mux_protected.objects[i].parent;
            mtp_unlock();
            return ESP_OK;
        }
    }
    mtp_unlock();
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t tinyusb_mtp_test_build_storage_path(tinyusb_mtp_storage_handle_t storage, const char *name, char *path, size_t path_size)
{
    int len = snprintf(path, path_size, "%s/%s", storage->base_path, name);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)path_size, ESP_ERR_INVALID_ARG, TAG, "MTP test storage path was truncated");
    return ESP_OK;
}

esp_err_t tinyusb_mtp_test_send_zero_size_object_info(tinyusb_mtp_storage_handle_t storage, uint32_t parent_handle, const char *name,
                                                      uint32_t *object_handle)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(storage && name && object_handle && mtp_name_is_safe(name), ESP_ERR_INVALID_ARG, TAG, "invalid zero-size object info args");

    uint8_t payload[sizeof(mtp_object_info_header_t) + 1U + UINT8_MAX * sizeof(uint16_t) + 3U * sizeof(uint32_t)] = { 0 };
    mtp_object_info_header_t info = {
        .storage_id = storage->storage_id,
        .object_format = MTP_OBJ_FORMAT_TEXT,
        .protection_status = MTP_PROTECTION_STATUS_NO_PROTECTION,
        .object_compressed_size = 0,
        .thumb_format = MTP_OBJ_FORMAT_UNDEFINED,
        .association_type = MTP_ASSOCIATION_UNDEFINED,
    };
    memcpy(payload, &info, sizeof(info));

    uint32_t name_len = 0;
    ESP_RETURN_ON_FALSE(mtp_utf8_to_mtp_string_payload(name, payload + sizeof(info), sizeof(payload) - sizeof(info), &name_len), ESP_ERR_INVALID_ARG,
                        TAG, "invalid zero-size object name");

    mtp_container_command_t command = {
        .header = {
            .len = sizeof(mtp_container_header_t) + 2U * sizeof(uint32_t),
            .code = MTP_OP_SEND_OBJECT_INFO,
        },
        .params = {
            storage->storage_id,
            parent_handle,
        },
    };
    mtp_container_header_t header = {
        .len = sizeof(mtp_container_header_t) + sizeof(info) + name_len,
        .code = MTP_OP_SEND_OBJECT_INFO,
    };
    tud_mtp_cb_data_t cb_data = {
        .phase = MTP_PHASE_DATA,
        .command_container = &command,
        .io_container = {
            .header = &header,
            .payload = payload,
            .payload_bytes = sizeof(payload),
        },
        .total_xferred_bytes = header.len,
    };

    bool session_was_open = tinyusb_mtp_test_force_session_open();
    int32_t ret = tud_mtp_data_xfer_cb(&cb_data);
    if (ret == 0) {
        header.len = sizeof(mtp_container_header_t);
        cb_data.xfer_result = XFER_RESULT_SUCCESS;
        ret = tud_mtp_data_complete_cb(&cb_data);
    }
    tinyusb_mtp_test_restore_session(session_was_open);
    ESP_RETURN_ON_FALSE(ret == 0 && header.code == MTP_RESP_OK, ESP_FAIL, TAG, "zero-size SendObjectInfo failed: response=0x%04" PRIx32,
                        ret == 0 ? (uint32_t)header.code : (uint32_t)ret);

    mtp_lock();
    bool pending_idle = !s_mtp.mux_protected.pending_write.active && s_mtp.mux_protected.pending_write.path == NULL;
    mtp_unlock();
    ESP_RETURN_ON_FALSE(pending_idle, ESP_FAIL, TAG, "zero-size SendObjectInfo left pending write active");

    char path[128];
    ESP_RETURN_ON_ERROR(tinyusb_mtp_test_build_storage_path(storage, name, path, sizeof(path)), TAG, "failed to build zero-size object path");
    return tinyusb_mtp_test_find_object(storage, path, object_handle);
}

esp_err_t tinyusb_mtp_test_delete_object(uint32_t object_handle)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_container_command_t command = {
        .header = {
            .code = MTP_OP_DELETE_OBJECT,
        },
        .params = {
            object_handle,
        },
    };
    mtp_container_header_t header = {
        .len = sizeof(mtp_container_header_t),
    };
    uint8_t payload[16] = { 0 };
    tud_mtp_cb_data_t cb_data = {
        .command_container = &command,
        .io_container = {
            .header = &header,
            .payload = payload,
            .payload_bytes = sizeof(payload),
        },
    };
    bool session_was_open = tinyusb_mtp_test_force_session_open();
    int32_t ret = mtp_delete_object(&cb_data);
    tinyusb_mtp_test_restore_session(session_was_open);
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t tinyusb_mtp_test_set_string_property(uint32_t object_handle, uint16_t prop_code, const char *value)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(value && mtp_name_is_safe(value), ESP_ERR_INVALID_ARG, TAG, "invalid MTP test property value");

    uint8_t payload[1U + UINT8_MAX * sizeof(uint16_t)] = { 0 };
    uint32_t payload_len = 0;
    ESP_RETURN_ON_FALSE(mtp_utf8_to_mtp_string_payload(value, payload, sizeof(payload), &payload_len), ESP_ERR_INVALID_ARG,
                        TAG, "invalid MTP UTF-8 test property value");

    mtp_container_command_t command = {
        .header = {
            .len = sizeof(mtp_container_header_t) + 2U * sizeof(uint32_t),
            .code = MTP_OP_SET_OBJECT_PROP_VALUE,
        },
        .params = {
            object_handle,
            prop_code,
        },
    };
    mtp_container_header_t header = {
        .len = sizeof(mtp_container_header_t) + payload_len,
        .code = MTP_OP_SET_OBJECT_PROP_VALUE,
    };
    tud_mtp_cb_data_t cb_data = {
        .phase = MTP_PHASE_DATA,
        .command_container = &command,
        .io_container = {
            .header = &header,
            .payload = payload,
            .payload_bytes = payload_len,
        },
        .total_xferred_bytes = sizeof(mtp_container_header_t) + payload_len,
    };

    bool session_was_open = tinyusb_mtp_test_force_session_open();
    int32_t ret = mtp_set_object_prop_value(&cb_data);
    tinyusb_mtp_test_restore_session(session_was_open);
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
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
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_lock();
    int32_t ret = mtp_begin_edit_object_locked(object_handle);
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_write_partial_object(uint32_t object_handle, uint64_t offset, const void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(data || size == 0, ESP_ERR_INVALID_ARG, TAG, "invalid MTP partial write test buffer");

    mtp_lock();
    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(object_handle, &object);
    if (ret == MTP_RESP_OK) {
        ret = mtp_write_object_range_locked(object, offset, data, size);
    }
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t tinyusb_mtp_test_truncate_object(uint32_t object_handle, uint64_t size)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

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
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_lock();
    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(object_handle, &object);
    if (ret == MTP_RESP_OK) {
        ret = mtp_update_object_stat_locked(object);
    }
    mtp_clear_partial_write();
    if (s_mtp.mux_protected.active_edit.active && s_mtp.mux_protected.active_edit.handle == object_handle) {
        mtp_clear_active_edit();
    }
    mtp_unlock();
    return ret == MTP_RESP_OK ? ESP_OK : ESP_FAIL;
}

#endif
