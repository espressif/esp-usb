/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_TINYUSB_MTP_ENABLED

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb_mtp.h"
#include "tinyusb_mtp_impl.h"
#include "tusb.h"

static const char *TAG = "tinyusb_mtp";

#define MTP_DEFAULT_MANUFACTURER        CONFIG_TINYUSB_DESC_MANUFACTURER_STRING
#define MTP_DEFAULT_MODEL               CONFIG_TINYUSB_DESC_PRODUCT_STRING
#define MTP_DEFAULT_VERSION             "1.0"
#define MTP_DEFAULT_SERIAL              CONFIG_TINYUSB_DESC_SERIAL_STRING
#define MTP_DEFAULT_FRIENDLY_NAME       "ESP TinyUSB MTP"
#define MTP_STORAGE_ID(index)           ((((uint32_t)(index) + 1U) << 16) | 1U)
#define MTP_MAX_NAME_CHARS              255U
#define MTP_MAX_NAME_BYTES              (MTP_MAX_NAME_CHARS * 3U)
#define MTP_MAX_DATA_BYTES              (UINT32_MAX - sizeof(mtp_container_header_t))
#define MTP_TEMP_NAME_PREFIX            ".mtp_tmp_"
#define MTP_BACKUP_NAME_PREFIX          ".mtp_bak_"

// Android direct file I/O opcodes are used by libmtp/GVFS for editor-style writes.
#ifndef MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64
#define MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64   0x95C1U
#endif
#ifndef MTP_OP_ANDROID_SEND_PARTIAL_OBJECT
#define MTP_OP_ANDROID_SEND_PARTIAL_OBJECT     0x95C2U
#endif
#ifndef MTP_OP_ANDROID_TRUNCATE_OBJECT
#define MTP_OP_ANDROID_TRUNCATE_OBJECT         0x95C3U
#endif
#ifndef MTP_OP_ANDROID_BEGIN_EDIT_OBJECT
#define MTP_OP_ANDROID_BEGIN_EDIT_OBJECT       0x95C4U
#endif
#ifndef MTP_OP_ANDROID_END_EDIT_OBJECT
#define MTP_OP_ANDROID_END_EDIT_OBJECT         0x95C5U
#endif
#define MTP_OP_SET_OBJECT_REFERENCES_CODE      0x9811U

#if CONFIG_TINYUSB_MTP_TRACE_WRITES
#define MTP_TRACEI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define MTP_TRACEI(...) do {} while (0)
#endif

typedef int32_t (*mtp_op_handler_t)(tud_mtp_cb_data_t *cb_data);

typedef struct {
    uint16_t op_code;
    mtp_op_handler_t handler;
} mtp_op_handler_entry_t;

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
    uint32_t count;
} mtp_payload_builder_t;

typedef struct {
    uint32_t handle;
    uint32_t depth;
} mtp_prop_list_visit_t;

tinyusb_mtp_ctx_t s_mtp;

static int32_t mtp_get_device_info(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_open_close_session(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_storage_ids(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_storage_info(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_num_objects(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_handles(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_info(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_partial_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_partial_object64(tud_mtp_cb_data_t *cb_data);
int32_t mtp_delete_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_send_object_info(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_send_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_begin_edit_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_send_partial_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_truncate_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_end_edit_object(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_device_property(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_props_supported(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_prop_desc(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_prop_value(tud_mtp_cb_data_t *cb_data);
int32_t mtp_set_object_prop_value(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_prop_list(tud_mtp_cb_data_t *cb_data);
static int32_t mtp_get_object_references(tud_mtp_cb_data_t *cb_data);
static mtp_object_t *mtp_object_from_handle(uint32_t handle);

static const mtp_op_handler_entry_t s_handlers[] = {
    { MTP_OP_GET_DEVICE_INFO, mtp_get_device_info },
    { MTP_OP_OPEN_SESSION, mtp_open_close_session },
    { MTP_OP_CLOSE_SESSION, mtp_open_close_session },
    { MTP_OP_GET_STORAGE_IDS, mtp_get_storage_ids },
    { MTP_OP_GET_STORAGE_INFO, mtp_get_storage_info },
    { MTP_OP_GET_NUM_OBJECTS, mtp_get_num_objects },
    { MTP_OP_GET_OBJECT_HANDLES, mtp_get_object_handles },
    { MTP_OP_GET_OBJECT_INFO, mtp_get_object_info },
    { MTP_OP_GET_OBJECT, mtp_get_object },
    { MTP_OP_GET_PARTIAL_OBJECT, mtp_get_partial_object },
    { MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64, mtp_get_partial_object64 },
    { MTP_OP_DELETE_OBJECT, mtp_delete_object },
    { MTP_OP_SEND_OBJECT_INFO, mtp_send_object_info },
    { MTP_OP_SEND_OBJECT, mtp_send_object },
    { MTP_OP_ANDROID_BEGIN_EDIT_OBJECT, mtp_begin_edit_object },
    { MTP_OP_ANDROID_SEND_PARTIAL_OBJECT, mtp_send_partial_object },
    { MTP_OP_ANDROID_TRUNCATE_OBJECT, mtp_truncate_object },
    { MTP_OP_ANDROID_END_EDIT_OBJECT, mtp_end_edit_object },
    { MTP_OP_GET_DEVICE_PROP_DESC, mtp_get_device_property },
    { MTP_OP_GET_DEVICE_PROP_VALUE, mtp_get_device_property },
    { MTP_OP_GET_OBJECT_PROPS_SUPPORTED, mtp_get_object_props_supported },
    { MTP_OP_GET_OBJECT_PROP_DESC, mtp_get_object_prop_desc },
    { MTP_OP_GET_OBJECT_PROP_VALUE, mtp_get_object_prop_value },
    { MTP_OP_SET_OBJECT_PROP_VALUE, mtp_set_object_prop_value },
    { MTP_OP_GET_OBJECT_PROP_LIST, mtp_get_object_prop_list },
    { MTP_OP_GET_OBJECT_PROP_REFERENCES, mtp_get_object_references },
};

static const uint16_t s_supported_object_props[] = {
    MTP_OBJ_PROP_STORAGE_ID,
    MTP_OBJ_PROP_OBJECT_FORMAT,
    MTP_OBJ_PROP_PROTECTION_STATUS,
    MTP_OBJ_PROP_OBJECT_SIZE,
    MTP_OBJ_PROP_ASSOCIATION_TYPE,
    MTP_OBJ_PROP_OBJECT_FILE_NAME,
    MTP_OBJ_PROP_DATE_CREATED,
    MTP_OBJ_PROP_DATE_MODIFIED,
    MTP_OBJ_PROP_PARENT_OBJECT,
    MTP_OBJ_PROP_PERSISTENT_UID,
    MTP_OBJ_PROP_NAME,
    MTP_OBJ_PROP_DATE_ADDED,
    MTP_OBJ_PROP_NON_CONSUMABLE,
    MTP_OBJ_PROP_DISPLAY_NAME,
};

static const char *mtp_operation_name(uint16_t op_code)
{
    switch (op_code) {
    case MTP_OP_GET_DEVICE_INFO:
        return "GetDeviceInfo";
    case MTP_OP_OPEN_SESSION:
        return "OpenSession";
    case MTP_OP_CLOSE_SESSION:
        return "CloseSession";
    case MTP_OP_GET_STORAGE_IDS:
        return "GetStorageIDs";
    case MTP_OP_GET_STORAGE_INFO:
        return "GetStorageInfo";
    case MTP_OP_GET_NUM_OBJECTS:
        return "GetNumObjects";
    case MTP_OP_GET_OBJECT_HANDLES:
        return "GetObjectHandles";
    case MTP_OP_GET_OBJECT_INFO:
        return "GetObjectInfo";
    case MTP_OP_GET_OBJECT:
        return "GetObject";
    case MTP_OP_GET_PARTIAL_OBJECT:
        return "GetPartialObject";
    case MTP_OP_DELETE_OBJECT:
        return "DeleteObject";
    case MTP_OP_SEND_OBJECT_INFO:
        return "SendObjectInfo";
    case MTP_OP_SEND_OBJECT:
        return "SendObject";
    case MTP_OP_MOVE_OBJECT:
        return "MoveObject";
    case MTP_OP_COPY_OBJECT:
        return "CopyObject";
    case MTP_OP_GET_DEVICE_PROP_DESC:
        return "GetDevicePropDesc";
    case MTP_OP_GET_DEVICE_PROP_VALUE:
        return "GetDevicePropValue";
    case MTP_OP_GET_OBJECT_PROPS_SUPPORTED:
        return "GetObjectPropsSupported";
    case MTP_OP_GET_OBJECT_PROP_DESC:
        return "GetObjectPropDesc";
    case MTP_OP_GET_OBJECT_PROP_VALUE:
        return "GetObjectPropValue";
    case MTP_OP_SET_OBJECT_PROP_VALUE:
        return "SetObjectPropValue";
    case MTP_OP_GET_OBJECT_PROP_LIST:
        return "GetObjectPropList";
    case MTP_OP_SET_OBJECT_PROP_LIST:
        return "SetObjectPropList";
    case MTP_OP_SEND_OBJECT_PROP_LIST:
        return "SendObjectPropList";
    case MTP_OP_GET_OBJECT_PROP_REFERENCES:
        return "GetObjectPropReferences";
    case MTP_OP_SET_OBJECT_REFERENCES_CODE:
        return "SetObjectReferences";
    case MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64:
        return "AndroidGetPartialObject64";
    case MTP_OP_ANDROID_SEND_PARTIAL_OBJECT:
        return "AndroidSendPartialObject";
    case MTP_OP_ANDROID_TRUNCATE_OBJECT:
        return "AndroidTruncateObject";
    case MTP_OP_ANDROID_BEGIN_EDIT_OBJECT:
        return "AndroidBeginEditObject";
    case MTP_OP_ANDROID_END_EDIT_OBJECT:
        return "AndroidEndEditObject";
    default:
        return "Unknown";
    }
}

#if CONFIG_TINYUSB_MTP_TRACE_WRITES
static const char *mtp_phase_name(uint8_t phase)
{
    switch (phase) {
    case MTP_PHASE_COMMAND:
        return "Command";
    case MTP_PHASE_DATA:
        return "Data";
    case MTP_PHASE_RESPONSE:
        return "Response";
    case MTP_PHASE_ERROR:
        return "Error";
    default:
        return "Unknown";
    }
}

static const char *mtp_response_name(int32_t response)
{
    switch (response) {
    case 0:
        return "DataPhase";
    case MTP_RESP_OK:
        return "OK";
    case MTP_RESP_GENERAL_ERROR:
        return "GeneralError";
    case MTP_RESP_SESSION_NOT_OPEN:
        return "SessionNotOpen";
    case MTP_RESP_OPERATION_NOT_SUPPORTED:
        return "OperationNotSupported";
    case MTP_RESP_INVALID_STORAGE_ID:
        return "InvalidStorageID";
    case MTP_RESP_INVALID_OBJECT_HANDLE:
        return "InvalidObjectHandle";
    case MTP_RESP_DEVICE_BUSY:
        return "DeviceBusy";
    case MTP_RESP_STORE_FULL:
        return "StoreFull";
    case MTP_RESP_OBJECT_WRITE_PROTECTED:
        return "ObjectWriteProtected";
    case MTP_RESP_STORE_NOT_AVAILABLE:
        return "StoreNotAvailable";
    case MTP_RESP_INCOMPLETE_TRANSFER:
        return "IncompleteTransfer";
    case MTP_RESP_INVALID_OBJECT_PROP_VALUE:
        return "InvalidObjectPropValue";
    case MTP_RESP_INVALID_DATASET:
        return "InvalidDataset";
    case MTP_RESP_OBJECT_TOO_LARGE:
        return "ObjectTooLarge";
    case MTP_RESP_OBJECT_PROP_NOT_SUPPORTED:
        return "ObjectPropNotSupported";
    default:
        return "Response";
    }
}

static bool mtp_operation_should_trace(uint16_t op_code)
{
    switch (op_code) {
    case MTP_OP_DELETE_OBJECT:
    case MTP_OP_GET_OBJECT_INFO:
    case MTP_OP_GET_OBJECT:
    case MTP_OP_GET_PARTIAL_OBJECT:
    case MTP_OP_GET_OBJECT_PROPS_SUPPORTED:
    case MTP_OP_GET_OBJECT_PROP_DESC:
    case MTP_OP_GET_OBJECT_PROP_VALUE:
    case MTP_OP_GET_OBJECT_PROP_LIST:
    case MTP_OP_SEND_OBJECT_INFO:
    case MTP_OP_SEND_OBJECT:
    case MTP_OP_MOVE_OBJECT:
    case MTP_OP_COPY_OBJECT:
    case MTP_OP_SET_OBJECT_PROP_VALUE:
    case MTP_OP_SET_OBJECT_PROP_LIST:
    case MTP_OP_SEND_OBJECT_PROP_LIST:
    case MTP_OP_GET_OBJECT_PROP_REFERENCES:
    case MTP_OP_SET_OBJECT_REFERENCES_CODE:
    case MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64:
    case MTP_OP_ANDROID_SEND_PARTIAL_OBJECT:
    case MTP_OP_ANDROID_TRUNCATE_OBJECT:
    case MTP_OP_ANDROID_BEGIN_EDIT_OBJECT:
    case MTP_OP_ANDROID_END_EDIT_OBJECT:
        return true;
    default:
        return false;
    }
}

static bool mtp_response_should_trace(int32_t response)
{
    return response == MTP_RESP_OPERATION_NOT_SUPPORTED || response == MTP_RESP_INVALID_DATASET || response == MTP_RESP_ACCESS_DENIED ||
           response == MTP_RESP_INVALID_OBJECT_HANDLE || response == MTP_RESP_OBJECT_PROP_NOT_SUPPORTED || response == MTP_RESP_OBJECT_TOO_LARGE;
}
#endif

static void mtp_trace_request_result(const tud_mtp_cb_data_t *cb_data, int32_t response)
{
#if CONFIG_TINYUSB_MTP_TRACE_WRITES
    const mtp_container_command_t *command = cb_data->command_container;
    uint16_t op_code = command->header.code;
    if (!mtp_operation_should_trace(op_code) && !mtp_response_should_trace(response)) {
        return;
    }

    uint32_t param_count = 0;
    if (command->header.len > sizeof(mtp_container_header_t)) {
        param_count = (command->header.len - sizeof(mtp_container_header_t)) / sizeof(uint32_t);
        if (param_count > 5) {
            param_count = 5;
        }
    }

    // Format only the parameters included in the command container to avoid stale values.
    char params[96] = { 0 };
    size_t used = 0;
    for (uint32_t i = 0; i < param_count; i++) {
        int len = snprintf(params + used, sizeof(params) - used, "%s0x%08" PRIx32, i == 0 ? "" : ",", command->params[i]);
        if (len <= 0 || (size_t)len >= sizeof(params) - used) {
            break;
        }
        used += (size_t)len;
    }

    uint32_t response_code = response > 0 ? (uint32_t)response : 0;
    ESP_LOGI(TAG, "MTP trace: phase=%s op=%s(0x%04x) tid=%" PRIu32 " len=%" PRIu32 " xfer=%" PRIu32 " params=%" PRIu32 " [%s] resp=%s(0x%04" PRIx32 ")",
             mtp_phase_name(cb_data->phase), mtp_operation_name(op_code), op_code, command->header.transaction_id, command->header.len,
             cb_data->total_xferred_bytes, param_count, params, mtp_response_name(response), response_code);
#else
    (void)cb_data;
    (void)response;
#endif
}

static char *mtp_strdup_or_default(const char *value, const char *fallback)
{
    const char *src = (value && value[0]) ? value : fallback;
    char *copy = strdup(src);
    if (copy == NULL) {
        ESP_LOGE(TAG, "failed to allocate string copy");
    }
    return copy;
}

void mtp_lock(void)
{
    if (s_mtp.constant.lock == NULL) {
        ESP_LOGE(TAG, "MTP lock is not initialized");
        abort();
    }
    xSemaphoreTake(s_mtp.constant.lock, portMAX_DELAY);
}

void mtp_unlock(void)
{
    if (s_mtp.constant.lock == NULL) {
        ESP_LOGE(TAG, "MTP lock is not initialized");
        abort();
    }
    xSemaphoreGive(s_mtp.constant.lock);
}

static bool mtp_session_is_open(void)
{
    mtp_lock();
    bool session_open = s_mtp.mux_protected.session_open;
    mtp_unlock();
    return session_open;
}

static void mtp_free_driver_strings(void)
{
    free(s_mtp.constant.manufacturer);
    free(s_mtp.constant.model);
    free(s_mtp.constant.version);
    free(s_mtp.constant.serial);
    free(s_mtp.constant.friendly_name);
    s_mtp.constant.manufacturer = NULL;
    s_mtp.constant.model = NULL;
    s_mtp.constant.version = NULL;
    s_mtp.constant.serial = NULL;
    s_mtp.constant.friendly_name = NULL;
}

static void mtp_clear_pending_write(void)
{
    if (s_mtp.mux_protected.pending_write.file) {
        fclose(s_mtp.mux_protected.pending_write.file);
    }
    free(s_mtp.mux_protected.pending_write.path);
    free(s_mtp.mux_protected.pending_write.write_path);
    free(s_mtp.mux_protected.pending_write.backup_path);
    memset(&s_mtp.mux_protected.pending_write, 0, sizeof(s_mtp.mux_protected.pending_write));
}

static void mtp_clear_active_read(void)
{
    if (s_mtp.mux_protected.active_read.file) {
        fclose(s_mtp.mux_protected.active_read.file);
    }
    free(s_mtp.mux_protected.active_read.path);
    memset(&s_mtp.mux_protected.active_read, 0, sizeof(s_mtp.mux_protected.active_read));
}

void mtp_clear_active_edit(void)
{
    memset(&s_mtp.mux_protected.active_edit, 0, sizeof(s_mtp.mux_protected.active_edit));
}

void mtp_clear_partial_write(void)
{
    memset(&s_mtp.mux_protected.partial_write, 0, sizeof(s_mtp.mux_protected.partial_write));
}

static void mtp_clear_active_buffer(void)
{
    free(s_mtp.mux_protected.active_buffer.data);
    memset(&s_mtp.mux_protected.active_buffer, 0, sizeof(s_mtp.mux_protected.active_buffer));
}

static void mtp_clear_pending_prop_set(void)
{
    s_mtp.mux_protected.pending_prop_set_active = false;
    s_mtp.mux_protected.pending_prop_set_response = 0;
}

static void mtp_clear_deferred_response(void)
{
    s_mtp.mux_protected.deferred_response_active = false;
    s_mtp.mux_protected.deferred_response_op = 0;
    s_mtp.mux_protected.deferred_response_code = 0;
}

static void mtp_free_object(mtp_object_t *object)
{
    free(object->path);
    memset(object, 0, sizeof(*object));
}

static void mtp_free_object_table(void)
{
    if (s_mtp.mux_protected.objects == NULL) {
        return;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used) {
            mtp_free_object(&s_mtp.mux_protected.objects[i]);
        }
    }
    free(s_mtp.mux_protected.objects);
    s_mtp.mux_protected.objects = NULL;
}

static void mtp_free_storage_table(void)
{
    if (s_mtp.mux_protected.storages == NULL) {
        return;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        free(s_mtp.mux_protected.storages[i].base_path);
        free(s_mtp.mux_protected.storages[i].display_name);
    }
    free(s_mtp.mux_protected.storages);
    s_mtp.mux_protected.storages = NULL;
}

bool mtp_storage_handle_is_valid_locked(const struct tinyusb_mtp_storage_s *storage)
{
    if (storage == NULL || s_mtp.mux_protected.storages == NULL) {
        return false;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (&s_mtp.mux_protected.storages[i] == storage) {
            return s_mtp.mux_protected.storages[i].used;
        }
    }
    return false;
}

static void mtp_clear_objects_for_storage(const struct tinyusb_mtp_storage_s *storage)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used && s_mtp.mux_protected.objects[i].storage == storage) {
            mtp_free_object(&s_mtp.mux_protected.objects[i]);
        }
    }
}

static const char *mtp_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void mtp_copy_display_name(const char *filename, bool directory, char *buffer, size_t buffer_size)
{
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    int len = snprintf(buffer, buffer_size, "%s", filename ? filename : "");
    if (len < 0 || len >= (int)buffer_size || directory) {
        return;
    }

    // Name and DisplayName are metadata fields; expose a user-facing stem without changing ObjectFileName.
    char *dot = strrchr(buffer, '.');
    if (dot != NULL && dot != buffer) {
        *dot = '\0';
    }
}

static void mtp_make_persistent_uid(const mtp_object_t *object, uint8_t uid[16])
{
    // Windows maps this to a stable WPD object identity during a device session.
    uint32_t words[4] = {
        object->storage ? object->storage->storage_id : 0,
        object->handle,
        (object->storage ? object->storage->storage_id : 0) ^ 0x4d545055U,
        object->handle ^ 0xa5a5a5a5U,
    };
    memcpy(uid, words, sizeof(words));
}

static bool mtp_path_is_child_of(const char *path, const char *parent)
{
    size_t parent_len = strlen(parent);
    return strncmp(path, parent, parent_len) == 0 && (path[parent_len] == '/' || path[parent_len] == '\0');
}

static char *mtp_join_path(const char *base, const char *name)
{
    const size_t base_len = strlen(base);
    const bool needs_slash = base_len > 0 && base[base_len - 1] != '/';
    const size_t len = base_len + (needs_slash ? 1 : 0) + strlen(name) + 1;
    char *path = malloc(len);
    if (path == NULL) {
        ESP_LOGE(TAG, "failed to allocate path for %s/%s", base, name);
        return NULL;
    }
    snprintf(path, len, "%s%s%s", base, needs_slash ? "/" : "", name);
    return path;
}

static char *mtp_make_internal_path(const char *dir, uint32_t handle, const char *prefix)
{
    for (uint32_t attempt = 0; attempt < 16; attempt++) {
        char name[40];
        int len = snprintf(name, sizeof(name), "%s%08" PRIx32 "_%02" PRIu32 ".tmp", prefix, handle, attempt);
        if (len <= 0 || len >= (int)sizeof(name)) {
            ESP_LOGE(TAG, "failed to format MTP internal name");
            return NULL;
        }

        char *path = mtp_join_path(dir, name);
        if (path == NULL) {
            return NULL;
        }
        struct stat st;
        if (stat(path, &st) != 0 && errno == ENOENT) {
            return path;
        }
        free(path);
    }

    ESP_LOGE(TAG, "failed to allocate unique MTP internal path under %s", dir);
    return NULL;
}

bool mtp_name_is_safe(const char *name)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
    }
    return true;
}

static bool mtp_name_is_internal_temp(const char *name)
{
    return name != NULL && (strncmp(name, MTP_TEMP_NAME_PREFIX, strlen(MTP_TEMP_NAME_PREFIX)) == 0 ||
                            strncmp(name, MTP_BACKUP_NAME_PREFIX, strlen(MTP_BACKUP_NAME_PREFIX)) == 0);
}

static uint16_t mtp_format_from_name(const char *name, bool directory)
{
    if (directory) {
        return MTP_OBJ_FORMAT_ASSOCIATION;
    }

    const char *ext = strrchr(name, '.');
    if (ext == NULL) {
        return MTP_OBJ_FORMAT_UNDEFINED;
    }
    ext++;
    if (strcasecmp(ext, "txt") == 0 || strcasecmp(ext, "log") == 0 || strcasecmp(ext, "json") == 0 || strcasecmp(ext, "lua") == 0) {
        return MTP_OBJ_FORMAT_TEXT;
    }
    if (strcasecmp(ext, "png") == 0) {
        return MTP_OBJ_FORMAT_PNG;
    }
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return MTP_OBJ_FORMAT_EXIF_JPEG;
    }
    if (strcasecmp(ext, "mp3") == 0) {
        return MTP_OBJ_FORMAT_MP3;
    }
    if (strcasecmp(ext, "wav") == 0) {
        return MTP_OBJ_FORMAT_WAV;
    }
    if (strcasecmp(ext, "mp4") == 0) {
        return MTP_OBJ_FORMAT_MP4;
    }
    return MTP_OBJ_FORMAT_UNDEFINED;
}

static bool mtp_utf8_decode_char(const char **src, uint32_t *codepoint)
{
    const uint8_t *s = (const uint8_t *)*src;
    if (s[0] < 0x80) {
        *codepoint = s[0];
        *src += 1;
        return true;
    }

    uint32_t cp = 0;
    size_t len = 0;
    if ((s[0] & 0xe0) == 0xc0) {
        cp = s[0] & 0x1f;
        len = 2;
        if (cp == 0) {
            return false;
        }
    } else if ((s[0] & 0xf0) == 0xe0) {
        cp = s[0] & 0x0f;
        len = 3;
    } else if ((s[0] & 0xf8) == 0xf0) {
        cp = s[0] & 0x07;
        len = 4;
    } else {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        return false;
    }

    *codepoint = cp;
    *src += len;
    return true;
}

static bool mtp_utf8_append_codepoint(char *dst, size_t dst_size, size_t *out, uint32_t codepoint)
{
    if (codepoint < 0x80) {
        if (*out + 1 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)codepoint;
    } else if (codepoint < 0x800) {
        if (*out + 2 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)(0xc0 | (codepoint >> 6));
        dst[(*out)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
        if (*out + 3 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)(0xe0 | (codepoint >> 12));
        dst[(*out)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        dst[(*out)++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
        if (*out + 4 >= dst_size) {
            return false;
        }
        dst[(*out)++] = (char)(0xf0 | (codepoint >> 18));
        dst[(*out)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        dst[(*out)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        dst[(*out)++] = (char)(0x80 | (codepoint & 0x3f));
    }
    return true;
}

static uint32_t mtp_utf8_next_codepoint_or_replacement(const char **src)
{
    const char *start = *src;
    uint32_t codepoint = 0;
    if (mtp_utf8_decode_char(src, &codepoint)) {
        return codepoint;
    }
    *src = start + 1;
    return '_';
}

static size_t mtp_utf8_count_utf16_units(const char *value)
{
    size_t units = 0;
    const char *cursor = value ? value : "";
    while (*cursor && units < UINT8_MAX - 1U) {
        const char *before = cursor;
        uint32_t codepoint = mtp_utf8_next_codepoint_or_replacement(&cursor);
        size_t add_units = codepoint > 0xffff ? 2U : 1U;
        if (units + add_units > UINT8_MAX - 1U) {
            cursor = before;
            break;
        }
        units += add_units;
    }
    return units;
}

static uint32_t mtp_container_add_utf8_string(mtp_container_info_t *container, const char *value)
{
    size_t units = mtp_utf8_count_utf16_units(value);
    if (units == 0) {
        return mtp_container_add_uint8(container, 0);
    }

    uint32_t added = mtp_container_add_uint8(container, (uint8_t)units + 1U);
    size_t written_units = 0;
    const char *cursor = value ? value : "";
    while (*cursor && written_units < units) {
        uint32_t codepoint = mtp_utf8_next_codepoint_or_replacement(&cursor);
        if (codepoint > 0xffff) {
            codepoint -= 0x10000;
            added += mtp_container_add_uint16(container, (uint16_t)(0xd800 | (codepoint >> 10)));
            added += mtp_container_add_uint16(container, (uint16_t)(0xdc00 | (codepoint & 0x3ff)));
            written_units += 2U;
        } else {
            added += mtp_container_add_uint16(container, (uint16_t)codepoint);
            written_units++;
        }
    }
    added += mtp_container_add_uint16(container, 0);
    return added;
}

bool mtp_utf8_to_mtp_string_payload(const char *value, uint8_t *payload, size_t payload_size, uint32_t *payload_len)
{
    if (value == NULL || payload == NULL || payload_len == NULL || payload_size < 1) {
        return false;
    }

    size_t units = 0;
    const char *cursor = value;
    while (*cursor) {
        uint32_t codepoint = 0;
        if (!mtp_utf8_decode_char(&cursor, &codepoint)) {
            return false;
        }
        size_t add_units = codepoint > 0xffff ? 2U : 1U;
        if (units + add_units > UINT8_MAX - 1U) {
            return false;
        }
        units += add_units;
    }

    if (units == 0) {
        payload[0] = 0;
        *payload_len = 1;
        return true;
    }

    size_t required = 1U + (units + 1U) * sizeof(uint16_t);
    if (required > payload_size) {
        return false;
    }
    payload[0] = (uint8_t)units + 1U;

    size_t out = 1;
    cursor = value;
    while (*cursor) {
        uint32_t codepoint = 0;
        if (!mtp_utf8_decode_char(&cursor, &codepoint)) {
            return false;
        }
        if (codepoint > 0xffff) {
            codepoint -= 0x10000;
            uint16_t high = (uint16_t)(0xd800 | (codepoint >> 10));
            uint16_t low = (uint16_t)(0xdc00 | (codepoint & 0x3ff));
            payload[out++] = (uint8_t)high;
            payload[out++] = (uint8_t)(high >> 8);
            payload[out++] = (uint8_t)low;
            payload[out++] = (uint8_t)(low >> 8);
        } else {
            payload[out++] = (uint8_t)codepoint;
            payload[out++] = (uint8_t)(codepoint >> 8);
        }
    }
    payload[out++] = 0;
    payload[out++] = 0;
    *payload_len = (uint32_t)out;
    return true;
}

static bool mtp_utf16_to_utf8_name(const uint8_t *src, size_t src_size, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0 || src_size < 1) {
        return false;
    }

    uint8_t count = *src++;
    if (count == 0) {
        dst[0] = '\0';
        return false;
    }
    if ((size_t)count > (src_size - 1) / sizeof(uint16_t)) {
        ESP_LOGW(TAG, "truncated MTP UTF-16 object name");
        return false;
    }

    size_t out = 0;
    for (uint8_t i = 0; i + 1 < count; i++) {
        uint16_t ch = (uint16_t)src[i * 2U] | ((uint16_t)src[i * 2U + 1U] << 8);
        if (ch == 0) {
            break;
        }

        uint32_t codepoint = ch;
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 2U >= count) {
                return false;
            }
            uint16_t low = (uint16_t)src[(i + 1U) * 2U] | ((uint16_t)src[(i + 1U) * 2U + 1U] << 8);
            if (low < 0xdc00 || low > 0xdfff) {
                return false;
            }
            codepoint = 0x10000 + (((uint32_t)ch - 0xd800) << 10) + ((uint32_t)low - 0xdc00);
            i++;
        } else if (ch >= 0xdc00 && ch <= 0xdfff) {
            return false;
        }

        if (!mtp_utf8_append_codepoint(dst, dst_size, &out, codepoint)) {
            ESP_LOGW(TAG, "MTP UTF-16 object name is too long after UTF-8 conversion");
            return false;
        }
    }
    dst[out] = '\0';
    return mtp_name_is_safe(dst);
}

static void mtp_update_object_from_stat(mtp_object_t *object, const struct stat *st)
{
    object->directory = S_ISDIR(st->st_mode);
    object->size = object->directory ? 0 : (uint64_t)st->st_size;
    object->mtime = st->st_mtime;
}

static int32_t mtp_write_errno_response(int err)
{
    return err == ENOSPC ? MTP_RESP_STORE_FULL : MTP_RESP_ACCESS_DENIED;
}

int32_t mtp_update_object_stat_locked(mtp_object_t *object)
{
    struct stat st;
    if (stat(object->path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat edited MTP object %s: %s", object->path, strerror(errno));
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_update_object_from_stat(object, &st);
    return MTP_RESP_OK;
}

int32_t mtp_get_active_edit_object_locked(uint32_t handle, mtp_object_t **object)
{
    if (!s_mtp.mux_protected.active_edit.active || s_mtp.mux_protected.active_edit.handle != handle) {
        ESP_LOGW(TAG, "MTP object edit was not opened for handle %" PRIu32, handle);
        return MTP_RESP_ACCESS_DENIED;
    }

    mtp_object_t *found = mtp_object_from_handle(handle);
    if (found == NULL || found->directory) {
        ESP_LOGW(TAG, "invalid MTP edit object handle %" PRIu32, handle);
        mtp_clear_active_edit();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    *object = found;
    return MTP_RESP_OK;
}

int32_t mtp_begin_edit_object_locked(uint32_t handle)
{
    mtp_object_t *object = mtp_object_from_handle(handle);
    if (object == NULL || object->directory) {
        ESP_LOGW(TAG, "invalid MTP begin edit handle %" PRIu32, handle);
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    if (s_mtp.mux_protected.active_edit.active && s_mtp.mux_protected.active_edit.handle != handle) {
        ESP_LOGW(TAG, "MTP edit already active for handle %" PRIu32, s_mtp.mux_protected.active_edit.handle);
        return MTP_RESP_DEVICE_BUSY;
    }

    int32_t ret = mtp_update_object_stat_locked(object);
    if (ret != MTP_RESP_OK) {
        return ret;
    }
    mtp_clear_partial_write();
    s_mtp.mux_protected.active_edit.active = true;
    s_mtp.mux_protected.active_edit.handle = handle;
    MTP_TRACEI("MTP edit begin: handle=%" PRIu32 " path=%s size=%" PRIu64, handle, object->path, object->size);
    return MTP_RESP_OK;
}

int32_t mtp_write_object_range_locked(mtp_object_t *object, uint64_t offset, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return MTP_RESP_OK;
    }
    if (offset > (uint64_t)LONG_MAX || len > (size_t)((uint64_t)LONG_MAX - offset)) {
        ESP_LOGW(TAG, "MTP partial write range is too large: offset=%" PRIu64 " len=%u", offset, (unsigned)len);
        return MTP_RESP_OBJECT_TOO_LARGE;
    }

    FILE *file = fopen(object->path, "r+b");
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open MTP edit object %s: %s", object->path, strerror(errno));
        return mtp_write_errno_response(errno);
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to seek MTP edit object %s: %s", object->path, strerror(err));
        fclose(file);
        return mtp_write_errno_response(err);
    }
    if (fwrite(data, 1, len, file) != len) {
        int err = errno;
        ESP_LOGE(TAG, "failed to write MTP edit object %s: %s", object->path, strerror(err));
        fclose(file);
        return mtp_write_errno_response(err);
    }
    if (fflush(file) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to flush MTP edit object %s: %s", object->path, strerror(err));
        fclose(file);
        return mtp_write_errno_response(err);
    }
    if (fclose(file) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to close MTP edit object %s: %s", object->path, strerror(err));
        return mtp_write_errno_response(err);
    }
    int32_t ret = mtp_update_object_stat_locked(object);
    if (ret == MTP_RESP_OK) {
        MTP_TRACEI("MTP partial write: handle=%" PRIu32 " offset=%" PRIu64 " len=%u path=%s", object->handle, offset, (unsigned)len, object->path);
    }
    return ret;
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

static void mtp_time_to_date_string(time_t value, char *buf, size_t buf_size)
{
    if (buf_size == 0) {
        return;
    }
    buf[0] = '\0';
    if (value <= 0) {
        return;
    }

    struct tm tm_value;
    if (gmtime_r(&value, &tm_value) == NULL) {
        return;
    }
    if (strftime(buf, buf_size, "%Y%m%dT%H%M%S", &tm_value) == 0) {
        buf[0] = '\0';
    }
}

static struct tinyusb_mtp_storage_s *mtp_storage_from_id(uint32_t storage_id)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (s_mtp.mux_protected.storages[i].used && s_mtp.mux_protected.storages[i].storage_id == storage_id) {
            return &s_mtp.mux_protected.storages[i];
        }
    }
    return NULL;
}

static mtp_object_t *mtp_object_from_handle(uint32_t handle)
{
    if (handle == MTP_OBJECT_HANDLE_INVALID || handle == MTP_ROOT_PARENT) {
        return NULL;
    }
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used && s_mtp.mux_protected.objects[i].handle == handle) {
            return &s_mtp.mux_protected.objects[i];
        }
    }
    return NULL;
}

static uint32_t mtp_normalize_parent_handle(uint32_t parent_handle)
{
    return (parent_handle == MTP_OBJECT_HANDLE_INVALID || parent_handle == MTP_ROOT_PARENT) ? MTP_ROOT_PARENT : parent_handle;
}

static void mtp_remove_created_path(const char *path, bool directory)
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

static void mtp_abort_pending_write_locked(int32_t error_response)
{
    uint32_t handle = s_mtp.mux_protected.pending_write.handle;
    const char *path = s_mtp.mux_protected.pending_write.path;
    const char *created_path = s_mtp.mux_protected.pending_write.write_path ? s_mtp.mux_protected.pending_write.write_path : path;

    if (handle == MTP_OBJECT_HANDLE_INVALID && path == NULL && s_mtp.mux_protected.pending_write.file == NULL) {
        mtp_clear_pending_write();
        return;
    }
    ESP_LOGW(TAG, "aborting pending MTP write: response=0x%04" PRIx32, (uint32_t)error_response);
    if (s_mtp.mux_protected.pending_write.file) {
        fclose(s_mtp.mux_protected.pending_write.file);
        s_mtp.mux_protected.pending_write.file = NULL;
    }
    if (created_path && s_mtp.mux_protected.pending_write.created_path) {
        mtp_remove_created_path(created_path, s_mtp.mux_protected.pending_write.directory);
    }
    if (handle != MTP_OBJECT_HANDLE_INVALID && s_mtp.mux_protected.pending_write.created_object) {
        mtp_object_t *object = mtp_object_from_handle(handle);
        if (object) {
            mtp_free_object(object);
        }
    }
    s_mtp.mux_protected.pending_write.error_response = error_response;
    mtp_clear_pending_write();
}

static int32_t mtp_get_parent_storage_for_all(uint32_t parent_handle, struct tinyusb_mtp_storage_s **storage)
{
    *storage = NULL;
    if (parent_handle == MTP_OBJECT_HANDLE_INVALID || parent_handle == MTP_ROOT_PARENT) {
        return 0;
    }

    mtp_object_t *parent = mtp_object_from_handle(parent_handle);
    if (parent == NULL || !parent->directory) {
        return MTP_RESP_INVALID_PARENT_OBJECT;
    }
    *storage = parent->storage;
    return 0;
}

static mtp_object_t *mtp_find_object_by_path(const struct tinyusb_mtp_storage_s *storage, const char *path)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used && s_mtp.mux_protected.objects[i].storage == storage && strcmp(s_mtp.mux_protected.objects[i].path, path) == 0) {
            return &s_mtp.mux_protected.objects[i];
        }
    }
    return NULL;
}

static uint32_t mtp_next_handle(void)
{
    for (uint32_t tries = 0; tries < UINT32_MAX - 1U; tries++) {
        uint32_t handle = s_mtp.mux_protected.next_object_handle++;
        if (s_mtp.mux_protected.next_object_handle == MTP_OBJECT_HANDLE_INVALID || s_mtp.mux_protected.next_object_handle == MTP_ROOT_PARENT) {
            s_mtp.mux_protected.next_object_handle = 1;
        }
        if (handle != MTP_OBJECT_HANDLE_INVALID && handle != MTP_ROOT_PARENT && mtp_object_from_handle(handle) == NULL) {
            return handle;
        }
    }
    ESP_LOGE(TAG, "object handle space exhausted");
    return MTP_OBJECT_HANDLE_INVALID;
}

mtp_object_t *mtp_get_or_create_object(struct tinyusb_mtp_storage_s *storage, uint32_t parent, const char *path, const struct stat *st)
{
    mtp_object_t *object = mtp_find_object_by_path(storage, path);
    if (object) {
        object->parent = parent;
        mtp_update_object_from_stat(object, st);
        return object;
    }

    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (!s_mtp.mux_protected.objects[i].used) {
            object = &s_mtp.mux_protected.objects[i];
            object->path = strdup(path);
            if (object->path == NULL) {
                ESP_LOGE(TAG, "failed to allocate object path: %s", path);
                return NULL;
            }
            object->handle = mtp_next_handle();
            if (object->handle == MTP_OBJECT_HANDLE_INVALID) {
                free(object->path);
                object->path = NULL;
                return NULL;
            }
            object->used = true;
            object->parent = parent;
            object->storage = storage;
            mtp_update_object_from_stat(object, st);
            return object;
        }
    }

    ESP_LOGE(TAG, "MTP object table full, cannot add %s", path);
    return NULL;
}

static uint32_t mtp_parent_handle_to_dir(struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle, const char **dir_path)
{
    if (parent_handle == MTP_OBJECT_HANDLE_INVALID || parent_handle == MTP_ROOT_PARENT) {
        *dir_path = storage->base_path;
        return 0;
    }

    mtp_object_t *parent = mtp_object_from_handle(parent_handle);
    if (parent == NULL || parent->storage != storage || !parent->directory) {
        return MTP_RESP_INVALID_PARENT_OBJECT;
    }
    *dir_path = parent->path;
    return 0;
}

static int32_t mtp_scan_children(struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle, uint32_t object_format, uint32_t *handles,
                                 size_t max_handles, uint32_t *count)
{
    const char *dir_path = NULL;
    uint32_t parent_resp = mtp_parent_handle_to_dir(storage, parent_handle, &dir_path);
    if (parent_resp != 0) {
        return (int32_t)parent_resp;
    }

    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "failed to open MTP directory %s: %s", dir_path, strerror(errno));
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    struct dirent *entry = NULL;
    uint32_t found = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (mtp_name_is_internal_temp(entry->d_name)) {
            continue;
        }

        char *path = mtp_join_path(dir_path, entry->d_name);
        if (path == NULL) {
            closedir(dir);
            return MTP_RESP_STORE_FULL;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "failed to stat MTP object %s: %s", path, strerror(errno));
            free(path);
            continue;
        }

        const bool is_dir = S_ISDIR(st.st_mode);
        const uint16_t fmt = mtp_format_from_name(entry->d_name, is_dir);
        if (object_format == 0 || object_format == 0xFFFFFFFFU || (uint16_t)object_format == fmt) {
            mtp_object_t *object = mtp_get_or_create_object(storage, mtp_normalize_parent_handle(parent_handle), path, &st);
            if (object == NULL) {
                free(path);
                closedir(dir);
                return MTP_RESP_STORE_FULL;
            }
            if (handles && found >= max_handles) {
                ESP_LOGE(TAG, "MTP object handle result table full while scanning %s", dir_path);
                free(path);
                closedir(dir);
                return MTP_RESP_STORE_FULL;
            }
            if (handles) {
                handles[found] = object->handle;
            }
            found++;
        }
        free(path);
    }

    closedir(dir);
    *count = found;
    return 0;
}

static esp_err_t mtp_recursive_delete_path(const char *path)
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

static void mtp_drop_objects_under_path(const struct tinyusb_mtp_storage_s *storage, const char *path)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used && s_mtp.mux_protected.objects[i].storage == storage && mtp_path_is_child_of(s_mtp.mux_protected.objects[i].path, path)) {
            mtp_free_object(&s_mtp.mux_protected.objects[i]);
        }
    }
}

static void mtp_drop_cached_children_under_path(const struct tinyusb_mtp_storage_s *storage, const char *path, const mtp_object_t *keep)
{
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        if (s_mtp.mux_protected.objects[i].used && &s_mtp.mux_protected.objects[i] != keep && s_mtp.mux_protected.objects[i].storage == storage && mtp_path_is_child_of(s_mtp.mux_protected.objects[i].path, path)) {
            mtp_free_object(&s_mtp.mux_protected.objects[i]);
        }
    }
}

static bool mtp_should_defer_data_response(uint16_t op_code)
{
    return op_code == MTP_OP_SEND_OBJECT_INFO || op_code == MTP_OP_SEND_OBJECT || op_code == MTP_OP_SET_OBJECT_PROP_VALUE ||
           op_code == MTP_OP_ANDROID_SEND_PARTIAL_OBJECT;
}

static bool mtp_data_phase_will_complete(const tud_mtp_cb_data_t *cb_data)
{
    uint32_t total_len = cb_data->io_container.header->len;
    return cb_data->total_xferred_bytes >= total_len || (cb_data->io_container.payload_bytes == 0 && cb_data->total_xferred_bytes > 0);
}

static void mtp_defer_response_locked(uint16_t op_code, int32_t response_code)
{
    if (!s_mtp.mux_protected.deferred_response_active) {
        s_mtp.mux_protected.deferred_response_active = true;
        s_mtp.mux_protected.deferred_response_op = op_code;
        s_mtp.mux_protected.deferred_response_code = response_code;
    }
}

static int32_t mtp_take_deferred_response_locked(uint16_t op_code, int32_t default_response)
{
    if (!s_mtp.mux_protected.deferred_response_active || s_mtp.mux_protected.deferred_response_op != op_code) {
        return default_response;
    }
    int32_t response_code = s_mtp.mux_protected.deferred_response_code;
    mtp_clear_deferred_response();
    return response_code;
}

static int32_t mtp_rename_object_locked(mtp_object_t *object, const char *new_name)
{
    if (!mtp_name_is_safe(new_name) || mtp_name_is_internal_temp(new_name)) {
        ESP_LOGW(TAG, "invalid MTP rename target: %s", new_name ? new_name : "(null)");
        return MTP_RESP_INVALID_OBJECT_PROP_VALUE;
    }

    const char *parent_dir = NULL;
    uint32_t parent_resp = mtp_parent_handle_to_dir(object->storage, object->parent, &parent_dir);
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

static int32_t mtp_commit_replacement_locked(void)
{
    const char *target_path = s_mtp.mux_protected.pending_write.path;
    const char *write_path = s_mtp.mux_protected.pending_write.write_path;
    const char *backup_path = s_mtp.mux_protected.pending_write.backup_path;
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
        s_mtp.mux_protected.pending_write.created_path = false;
        return MTP_RESP_GENERAL_ERROR;
    }

    if (unlink(backup_path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "failed to remove MTP replacement backup %s: %s", backup_path, strerror(errno));
    }
    s_mtp.mux_protected.pending_write.created_path = false;
    return MTP_RESP_OK;
}

// Finish both SendObject data writes and zero-size SendObjectInfo creations.
static int32_t mtp_finish_pending_write_locked(void)
{
    mtp_pending_write_t *pending = &s_mtp.mux_protected.pending_write;
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
        mtp_object_t *object = mtp_object_from_handle(pending->handle);
        if (object) {
            mtp_update_object_from_stat(object, &st);
        }
    } else {
        ESP_LOGW(TAG, "failed to stat written MTP object %s: %s", pending->path, strerror(errno));
    }
    MTP_TRACEI("MTP object write complete: handle=%" PRIu32 " bytes=%" PRIu64 " replace=%d path=%s", pending->handle, pending->written,
               pending->replace_existing, pending->path);
    mtp_clear_pending_write();
    return MTP_RESP_OK;
}

static int32_t mtp_start_buffered_data(tud_mtp_cb_data_t *cb_data, uint16_t op_code, uint8_t *data, uint32_t len)
{
    mtp_container_info_t *container = &cb_data->io_container;
    mtp_lock();
    mtp_clear_active_buffer();
    s_mtp.mux_protected.active_buffer.active = true;
    s_mtp.mux_protected.active_buffer.op_code = op_code;
    s_mtp.mux_protected.active_buffer.data = data;
    s_mtp.mux_protected.active_buffer.len = len;

    // Keep the full response in active_buffer; only the first packet includes the MTP container header.
    uint32_t first_len = len < container->payload_bytes ? len : container->payload_bytes;
    if (first_len > 0) {
        memcpy(container->payload, data, first_len);
    }
    container->header->len = sizeof(mtp_container_header_t) + len;
    if (!tud_mtp_data_send(container)) {
        mtp_clear_active_buffer();
        mtp_unlock();
        return MTP_RESP_DEVICE_BUSY;
    }
    mtp_unlock();
    return 0;
}

static int32_t mtp_continue_buffered_data(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    mtp_lock();
    if (!s_mtp.mux_protected.active_buffer.active || s_mtp.mux_protected.active_buffer.op_code != command->header.code || s_mtp.mux_protected.active_buffer.data == NULL) {
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    uint32_t offset = cb_data->total_xferred_bytes > sizeof(mtp_container_header_t) ? cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) : 0;
    if (offset >= s_mtp.mux_protected.active_buffer.len) {
        mtp_unlock();
        return 0;
    }

    uint32_t chunk = s_mtp.mux_protected.active_buffer.len - offset;
    if (chunk > container->payload_bytes) {
        chunk = container->payload_bytes;
    }
    memcpy(container->payload, s_mtp.mux_protected.active_buffer.data + offset, chunk);
    if (!tud_mtp_data_send(container)) {
        mtp_clear_active_buffer();
        mtp_unlock();
        return MTP_RESP_DEVICE_BUSY;
    }
    mtp_unlock();
    return 0;
}

static int32_t mtp_dispatch(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    for (size_t i = 0; i < sizeof(s_handlers) / sizeof(s_handlers[0]); i++) {
        if (s_handlers[i].op_code == command->header.code) {
            return s_handlers[i].handler(cb_data);
        }
    }
    ESP_LOGW(TAG, "unsupported MTP operation %s (0x%04x)", mtp_operation_name(command->header.code), command->header.code);
    return MTP_RESP_OPERATION_NOT_SUPPORTED;
}

esp_err_t tinyusb_mtp_install_driver(const tinyusb_mtp_driver_config_t *config)
{
    ESP_RETURN_ON_FALSE(!s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver already installed");

    memset(&s_mtp, 0, sizeof(s_mtp));
    s_mtp.constant.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mtp.constant.lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create MTP lock");

    // Allocate large MTP tables on install so disabled MTP does not reserve static DRAM.
    s_mtp.mux_protected.storages = calloc(CONFIG_TINYUSB_MTP_MAX_STORAGES, sizeof(*s_mtp.mux_protected.storages));
    s_mtp.mux_protected.objects = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(*s_mtp.mux_protected.objects));
    if (s_mtp.mux_protected.storages == NULL || s_mtp.mux_protected.objects == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP tables: storages=%d objects=%d", CONFIG_TINYUSB_MTP_MAX_STORAGES, CONFIG_TINYUSB_MTP_MAX_OBJECTS);
        mtp_free_object_table();
        mtp_free_storage_table();
        vSemaphoreDelete(s_mtp.constant.lock);
        memset(&s_mtp, 0, sizeof(s_mtp));
        return ESP_ERR_NO_MEM;
    }

    s_mtp.constant.manufacturer = mtp_strdup_or_default(config ? config->manufacturer : NULL, MTP_DEFAULT_MANUFACTURER);
    s_mtp.constant.model = mtp_strdup_or_default(config ? config->model : NULL, MTP_DEFAULT_MODEL);
    s_mtp.constant.version = mtp_strdup_or_default(config ? config->version : NULL, MTP_DEFAULT_VERSION);
    s_mtp.constant.serial = mtp_strdup_or_default(config ? config->serial : NULL, MTP_DEFAULT_SERIAL);
    s_mtp.constant.friendly_name = mtp_strdup_or_default(config ? config->friendly_name : NULL, MTP_DEFAULT_FRIENDLY_NAME);
    if (!s_mtp.constant.manufacturer || !s_mtp.constant.model || !s_mtp.constant.version || !s_mtp.constant.serial || !s_mtp.constant.friendly_name) {
        mtp_free_driver_strings();
        mtp_free_object_table();
        mtp_free_storage_table();
        vSemaphoreDelete(s_mtp.constant.lock);
        memset(&s_mtp, 0, sizeof(s_mtp));
        return ESP_ERR_NO_MEM;
    }

    s_mtp.mux_protected.next_object_handle = 1;
    s_mtp.installed = true;
    ESP_LOGI(TAG, "MTP driver installed");
    return ESP_OK;
}

esp_err_t tinyusb_mtp_uninstall_driver(void)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");

    mtp_lock();
    s_mtp.installed = false;
    mtp_abort_pending_write_locked(MTP_RESP_TRANSACTION_CANCELLED);
    mtp_clear_active_read();
    mtp_clear_active_edit();
    mtp_clear_partial_write();
    mtp_clear_active_buffer();
    mtp_clear_deferred_response();
    mtp_clear_pending_prop_set();
    mtp_free_object_table();
    mtp_free_storage_table();
    mtp_free_driver_strings();
    mtp_unlock();
    SemaphoreHandle_t lock = s_mtp.constant.lock;
    vSemaphoreDelete(lock);
    memset(&s_mtp, 0, sizeof(s_mtp));
    return ESP_OK;
}

esp_err_t tinyusb_mtp_register_storage(const tinyusb_mtp_storage_config_t *config, tinyusb_mtp_storage_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(config && config->base_path && config->base_path[0] == '/', ESP_ERR_INVALID_ARG, TAG, "invalid MTP storage path");

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(config->base_path, &total, &free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MTP storage path is not a mounted FATFS path: %s (%s)", config->base_path, esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    mtp_lock();
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (s_mtp.mux_protected.storages[i].used && strcmp(s_mtp.mux_protected.storages[i].base_path, config->base_path) == 0) {
            mtp_unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }

    struct tinyusb_mtp_storage_s *storage = NULL;
    size_t index = 0;
    for (; index < CONFIG_TINYUSB_MTP_MAX_STORAGES; index++) {
        if (!s_mtp.mux_protected.storages[index].used) {
            storage = &s_mtp.mux_protected.storages[index];
            break;
        }
    }
    if (storage == NULL) {
        mtp_unlock();
        ESP_LOGE(TAG, "MTP storage table full, cannot register %s", config->base_path);
        return ESP_FAIL;
    }

    storage->base_path = strdup(config->base_path);
    storage->display_name = mtp_strdup_or_default(config->display_name, config->base_path);
    if (storage->base_path == NULL || storage->display_name == NULL) {
        free(storage->base_path);
        free(storage->display_name);
        memset(storage, 0, sizeof(*storage));
        mtp_unlock();
        return ESP_ERR_NO_MEM;
    }
    storage->removable = config->removable;
    storage->storage_id = MTP_STORAGE_ID(index);
    storage->used = true;
    if (handle) {
        *handle = storage;
    }
    ESP_LOGI(TAG, "MTP storage registered: id=0x%08" PRIx32 " path=%s name=%s total=%" PRIu64 " free=%" PRIu64,
             storage->storage_id, storage->base_path, storage->display_name, total, free_bytes);
    mtp_unlock();
    return ESP_OK;
}

esp_err_t tinyusb_mtp_unregister_storage(tinyusb_mtp_storage_handle_t handle)
{
    ESP_RETURN_ON_FALSE(s_mtp.installed, ESP_ERR_INVALID_STATE, TAG, "MTP driver is not installed");
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid MTP storage handle");

    mtp_lock();
    if (!mtp_storage_handle_is_valid_locked(handle)) {
        ESP_LOGW(TAG, "invalid MTP storage handle");
        mtp_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mtp.mux_protected.pending_write.storage_id == handle->storage_id) {
        mtp_abort_pending_write_locked(MTP_RESP_STORE_NOT_AVAILABLE);
    }
    if (s_mtp.mux_protected.active_read.storage == handle) {
        mtp_clear_active_read();
    }
    if (s_mtp.mux_protected.active_edit.active) {
        mtp_object_t *edit_obj = mtp_object_from_handle(s_mtp.mux_protected.active_edit.handle);
        if (edit_obj == NULL || edit_obj->storage == handle) {
            mtp_clear_active_edit();
        }
    }
    if (s_mtp.mux_protected.partial_write.active) {
        mtp_object_t *pw_obj = mtp_object_from_handle(s_mtp.mux_protected.partial_write.handle);
        if (pw_obj == NULL || pw_obj->storage == handle) {
            mtp_clear_partial_write();
        }
    }
    mtp_clear_active_buffer();
    mtp_clear_deferred_response();
    mtp_clear_pending_prop_set();
    mtp_clear_objects_for_storage(handle);
    free(handle->base_path);
    free(handle->display_name);
    memset(handle, 0, sizeof(*handle));
    mtp_unlock();
    return ESP_OK;
}

bool tud_mtp_request_cancel_cb(tud_mtp_request_cb_data_t *cb_data)
{
    (void)cb_data;
    if (!s_mtp.installed) {
        return true;
    }
    mtp_lock();
    mtp_abort_pending_write_locked(MTP_RESP_TRANSACTION_CANCELLED);
    mtp_clear_active_read();
    mtp_clear_active_edit();
    mtp_clear_partial_write();
    mtp_clear_active_buffer();
    mtp_clear_deferred_response();
    mtp_clear_pending_prop_set();
    mtp_unlock();
    return true;
}

bool tud_mtp_request_device_reset_cb(tud_mtp_request_cb_data_t *cb_data)
{
    (void)cb_data;
    if (!s_mtp.installed) {
        return true;
    }
    mtp_lock();
    s_mtp.mux_protected.session_open = false;
    mtp_abort_pending_write_locked(MTP_RESP_TRANSACTION_CANCELLED);
    mtp_clear_active_read();
    mtp_clear_active_edit();
    mtp_clear_partial_write();
    mtp_clear_active_buffer();
    mtp_clear_deferred_response();
    mtp_clear_pending_prop_set();
    mtp_unlock();
    return true;
}

int32_t tud_mtp_request_get_device_status_cb(tud_mtp_request_cb_data_t *cb_data)
{
    uint16_t *buf16 = (uint16_t *)(uintptr_t)cb_data->buf;
    buf16[0] = 4;
    buf16[1] = MTP_RESP_OK;
    return 4;
}

int32_t tud_mtp_command_received_cb(tud_mtp_cb_data_t *cb_data)
{
    int32_t resp_code = s_mtp.installed ? mtp_dispatch(cb_data) : MTP_RESP_DEVICE_BUSY;
    mtp_trace_request_result(cb_data, resp_code);
    if (resp_code > MTP_RESP_UNDEFINED) {
        cb_data->io_container.header->code = (uint16_t)resp_code;
        tud_mtp_response_send(&cb_data->io_container);
    }
    return resp_code;
}

int32_t tud_mtp_data_xfer_cb(tud_mtp_cb_data_t *cb_data)
{
    int32_t resp_code = s_mtp.installed ? mtp_dispatch(cb_data) : MTP_RESP_DEVICE_BUSY;
    mtp_trace_request_result(cb_data, resp_code);
    if (resp_code > MTP_RESP_UNDEFINED) {
        uint16_t op_code = cb_data->command_container->header.code;
        if (cb_data->phase == MTP_PHASE_DATA && mtp_should_defer_data_response(op_code) && mtp_data_phase_will_complete(cb_data)) {
            mtp_lock();
            mtp_defer_response_locked(op_code, resp_code);
            mtp_unlock();
            return 0;
        }
        cb_data->io_container.header->code = (uint16_t)resp_code;
        tud_mtp_response_send(&cb_data->io_container);
    }
    return resp_code < 0 ? resp_code : 0;
}

static int32_t mtp_complete_send_object_info_locked(const tud_mtp_cb_data_t *cb_data, mtp_container_info_t *response)
{
    mtp_pending_write_t *pending = &s_mtp.mux_protected.pending_write;
    int32_t ret = MTP_RESP_OK;

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, abort_pending, TAG,
                      "MTP SendObjectInfo transfer failed: result=%d", cb_data->xfer_result);

    int32_t deferred_resp = mtp_take_deferred_response_locked(MTP_OP_SEND_OBJECT_INFO, MTP_RESP_OK);
    ESP_GOTO_ON_FALSE(deferred_resp == MTP_RESP_OK, deferred_resp, abort_pending, TAG, "MTP SendObjectInfo deferred response=0x%04" PRIx32,
                      (uint32_t)deferred_resp);
    ESP_GOTO_ON_FALSE(pending->handle != MTP_OBJECT_HANDLE_INVALID, MTP_RESP_GENERAL_ERROR, done, TAG, "missing pending MTP object info");

    bool expects_object_data = pending->active;
    uint32_t storage_id = pending->storage_id;
    uint32_t parent_handle = pending->parent_handle;
    uint32_t handle = pending->handle;
    if (!expects_object_data) {
        ret = mtp_finish_pending_write_locked();
        ESP_GOTO_ON_FALSE(ret == MTP_RESP_OK, ret, clear_pending, TAG, "failed to finish MTP object info write: response=0x%04" PRIx32,
                          (uint32_t)ret);
    }
    (void)mtp_container_add_uint32(response, storage_id);
    (void)mtp_container_add_uint32(response, parent_handle);
    (void)mtp_container_add_uint32(response, handle);
    return ret;

abort_pending:
    if (cb_data->xfer_result != XFER_RESULT_SUCCESS) {
        mtp_clear_deferred_response();
    }
    mtp_abort_pending_write_locked(ret);
    goto done;
clear_pending:
    if (pending->path != NULL) {
        mtp_clear_pending_write();
    }
done:
    return ret;
}

static int32_t mtp_complete_send_object_locked(const tud_mtp_cb_data_t *cb_data)
{
    mtp_pending_write_t *pending = &s_mtp.mux_protected.pending_write;
    int32_t ret = MTP_RESP_OK;

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, abort_pending, TAG,
                      "MTP SendObject transfer failed: result=%d", cb_data->xfer_result);

    int32_t deferred_resp = mtp_take_deferred_response_locked(MTP_OP_SEND_OBJECT, MTP_RESP_OK);
    ESP_GOTO_ON_FALSE(deferred_resp == MTP_RESP_OK, deferred_resp, done, TAG, "MTP SendObject deferred response=0x%04" PRIx32,
                      (uint32_t)deferred_resp);
    ESP_GOTO_ON_FALSE(pending->active && pending->handle != MTP_OBJECT_HANDLE_INVALID && pending->path != NULL, MTP_RESP_GENERAL_ERROR, done,
                      TAG, "missing pending MTP object data");

    ESP_GOTO_ON_FALSE(pending->written == pending->expected_size, MTP_RESP_INCOMPLETE_TRANSFER, abort_pending, TAG,
                      "incomplete MTP object write: expected=%" PRIu64 " written=%" PRIu64, pending->expected_size, pending->written);

    ret = mtp_finish_pending_write_locked();
    ESP_GOTO_ON_FALSE(ret == MTP_RESP_OK, ret, clear_pending, TAG, "failed to finish MTP object write: response=0x%04" PRIx32, (uint32_t)ret);
    return ret;

abort_pending:
    if (cb_data->xfer_result != XFER_RESULT_SUCCESS) {
        mtp_clear_deferred_response();
    }
    mtp_abort_pending_write_locked(ret);
    return ret;
clear_pending:
    if (pending->path != NULL) {
        mtp_clear_pending_write();
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
        ESP_GOTO_ON_FALSE(s_mtp.mux_protected.pending_prop_set_active, MTP_RESP_GENERAL_ERROR, done, TAG,
                          "missing pending MTP property set response");
        ret = s_mtp.mux_protected.pending_prop_set_response;
    }

done:
    mtp_clear_pending_prop_set();
    return ret;
clear_deferred:
    mtp_clear_deferred_response();
    goto done;
}

static int32_t mtp_complete_partial_write_locked(const tud_mtp_cb_data_t *cb_data)
{
    mtp_partial_write_t *partial = &s_mtp.mux_protected.partial_write;
    int32_t ret = MTP_RESP_OK;

    ESP_GOTO_ON_FALSE(cb_data->xfer_result == XFER_RESULT_SUCCESS, MTP_RESP_GENERAL_ERROR, clear_deferred, TAG,
                      "MTP partial write transfer failed: result=%d", cb_data->xfer_result);

    int32_t deferred_resp = mtp_take_deferred_response_locked(MTP_OP_ANDROID_SEND_PARTIAL_OBJECT, MTP_RESP_OK);
    ESP_GOTO_ON_FALSE(deferred_resp == MTP_RESP_OK, deferred_resp, done, TAG, "MTP partial write deferred response=0x%04" PRIx32,
                      (uint32_t)deferred_resp);
    ESP_GOTO_ON_FALSE(partial->active && partial->written == partial->expected_size, MTP_RESP_INCOMPLETE_TRANSFER, done, TAG,
                      "incomplete MTP partial write: expected=%" PRIu32 " written=%" PRIu32, partial->expected_size, partial->written);

done:
    mtp_clear_partial_write();
    return ret;
clear_deferred:
    mtp_clear_deferred_response();
    goto done;
}

static int32_t mtp_complete_read_locked(const tud_mtp_cb_data_t *cb_data)
{
    mtp_active_read_t *read = &s_mtp.mux_protected.active_read;
    if (read->active) {
        MTP_TRACEI("MTP object read complete: handle=%" PRIu32 " sent=%" PRIu64 "/%" PRIu64 " result=%d path=%s", read->handle, read->sent,
                   read->expected, cb_data->xfer_result, read->path ? read->path : "(unknown)");
    } else {
        ESP_LOGW(TAG, "MTP object read complete without active state");
    }
    mtp_clear_active_read();
    return (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
}

static int32_t mtp_complete_partial_read_locked(const tud_mtp_cb_data_t *cb_data, uint32_t *sent)
{
    mtp_active_read_t *read = &s_mtp.mux_protected.active_read;
    *sent = cb_data->total_xferred_bytes > sizeof(mtp_container_header_t) ? cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) : 0;
    if (read->active) {
        *sent = read->sent > UINT32_MAX ? UINT32_MAX : (uint32_t)read->sent;
        MTP_TRACEI("MTP partial object read complete: handle=%" PRIu32 " sent=%" PRIu64 "/%" PRIu64 " result=%d path=%s", read->handle, read->sent,
                   read->expected, cb_data->xfer_result, read->path ? read->path : "(unknown)");
    } else {
        ESP_LOGW(TAG, "MTP partial object read complete without active state");
    }
    mtp_clear_active_read();
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
        mtp_clear_active_buffer();
        resp_code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
        break;
    }
    return resp_code;
}

int32_t tud_mtp_data_complete_cb(tud_mtp_cb_data_t *cb_data)
{
    mtp_container_info_t *response = &cb_data->io_container;
    int32_t resp_code = MTP_RESP_DEVICE_BUSY;

    if (s_mtp.installed) {
        mtp_lock();
        resp_code = mtp_complete_data_locked(cb_data, response);
        mtp_unlock();
    }

    response->header->code = (uint16_t)resp_code;
    mtp_trace_request_result(cb_data, resp_code);
    tud_mtp_response_send(response);
    return 0;
}

static int32_t mtp_get_device_info(tud_mtp_cb_data_t *cb_data)
{
    mtp_container_info_t *container = &cb_data->io_container;
    mtp_lock();
    (void)mtp_container_add_utf8_string(container, s_mtp.constant.manufacturer);
    (void)mtp_container_add_utf8_string(container, s_mtp.constant.model);
    (void)mtp_container_add_utf8_string(container, s_mtp.constant.version);
    (void)mtp_container_add_utf8_string(container, s_mtp.constant.serial);
    mtp_unlock();
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

static int32_t mtp_open_close_session(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    mtp_lock();
    if (command->header.code == MTP_OP_OPEN_SESSION) {
        if (s_mtp.mux_protected.session_open) {
            mtp_unlock();
            return MTP_RESP_SESSION_ALREADY_OPEN;
        }
        s_mtp.mux_protected.session_open = true;
    } else {
        if (!s_mtp.mux_protected.session_open) {
            mtp_unlock();
            return MTP_RESP_SESSION_NOT_OPEN;
        }
        s_mtp.mux_protected.session_open = false;
        mtp_abort_pending_write_locked(MTP_RESP_TRANSACTION_CANCELLED);
        mtp_clear_active_read();
        mtp_clear_active_edit();
        mtp_clear_partial_write();
        mtp_clear_active_buffer();
        mtp_clear_deferred_response();
        mtp_clear_pending_prop_set();
    }
    mtp_unlock();
    return MTP_RESP_OK;
}

static int32_t mtp_get_storage_ids(tud_mtp_cb_data_t *cb_data)
{
    uint32_t ids[CONFIG_TINYUSB_MTP_MAX_STORAGES] = { 0 };
    uint32_t count = 0;
    mtp_lock();
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (s_mtp.mux_protected.storages[i].used) {
            ids[count++] = s_mtp.mux_protected.storages[i].storage_id;
        }
    }
    mtp_unlock();

    (void)mtp_container_add_auint32(&cb_data->io_container, count, ids);
    return tud_mtp_data_send(&cb_data->io_container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

static int32_t mtp_get_storage_info(tud_mtp_cb_data_t *cb_data)
{
    uint32_t storage_id = cb_data->command_container->params[0];
    char *base_path = NULL;
    char *display_name = NULL;
    bool removable = false;

    mtp_lock();
    struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id(storage_id);
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

static int32_t mtp_get_num_objects(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    uint32_t storage_id = command->params[0];
    uint32_t object_format = command->params[1];
    uint32_t parent = command->params[2];
    uint32_t total = 0;

    if (storage_id == MTP_ROOT_PARENT) {
        mtp_lock();
        struct tinyusb_mtp_storage_s *parent_storage = NULL;
        int32_t parent_ret = mtp_get_parent_storage_for_all(parent, &parent_storage);
        if (parent_ret > MTP_RESP_UNDEFINED) {
            mtp_unlock();
            return parent_ret;
        }
        if (parent_storage) {
            int32_t ret = mtp_scan_children(parent_storage, parent, object_format, NULL, 0, &total);
            if (ret > MTP_RESP_UNDEFINED) {
                mtp_unlock();
                return ret;
            }
        } else {
            for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
                if (!s_mtp.mux_protected.storages[i].used) {
                    continue;
                }
                uint32_t count = 0;
                int32_t ret = mtp_scan_children(&s_mtp.mux_protected.storages[i], parent, object_format, NULL, 0, &count);
                if (ret > MTP_RESP_UNDEFINED) {
                    mtp_unlock();
                    return ret;
                }
                total += count;
            }
        }
        mtp_unlock();
    } else {
        mtp_lock();
        struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id(storage_id);
        if (storage == NULL) {
            mtp_unlock();
            return MTP_RESP_INVALID_STORAGE_ID;
        }
        int32_t ret = mtp_scan_children(storage, parent, object_format, NULL, 0, &total);
        mtp_unlock();
        if (ret > MTP_RESP_UNDEFINED) {
            return ret;
        }
    }

    (void)mtp_container_add_uint32(&cb_data->io_container, total);
    return MTP_RESP_OK;
}

static int32_t mtp_get_object_handles(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_buffered_data(cb_data);
    }
    if (cb_data->phase != MTP_PHASE_COMMAND) {
        return 0;
    }

    const mtp_container_command_t *command = cb_data->command_container;
    uint32_t storage_id = command->params[0];
    uint32_t object_format = command->params[1];
    uint32_t parent = command->params[2];
    uint32_t *handles = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(uint32_t));
    if (handles == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP handle list");
        return MTP_RESP_STORE_FULL;
    }

    uint32_t count = 0;
    mtp_lock();
    if (storage_id == MTP_ROOT_PARENT) {
        struct tinyusb_mtp_storage_s *parent_storage = NULL;
        int32_t parent_ret = mtp_get_parent_storage_for_all(parent, &parent_storage);
        if (parent_ret > MTP_RESP_UNDEFINED) {
            mtp_unlock();
            free(handles);
            return parent_ret;
        }
        if (parent_storage) {
            int32_t ret = mtp_scan_children(parent_storage, parent, object_format, handles, CONFIG_TINYUSB_MTP_MAX_OBJECTS, &count);
            if (ret > MTP_RESP_UNDEFINED) {
                mtp_unlock();
                free(handles);
                return ret;
            }
        } else {
            for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
                if (!s_mtp.mux_protected.storages[i].used) {
                    continue;
                }
                uint32_t sub_count = 0;
                int32_t ret = mtp_scan_children(&s_mtp.mux_protected.storages[i], parent, object_format, handles + count, CONFIG_TINYUSB_MTP_MAX_OBJECTS - count, &sub_count);
                if (ret > MTP_RESP_UNDEFINED) {
                    mtp_unlock();
                    free(handles);
                    return ret;
                }
                count += sub_count;
            }
        }
    } else {
        struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id(storage_id);
        if (storage == NULL) {
            mtp_unlock();
            free(handles);
            return MTP_RESP_INVALID_STORAGE_ID;
        }
        int32_t ret = mtp_scan_children(storage, parent, object_format, handles, CONFIG_TINYUSB_MTP_MAX_OBJECTS, &count);
        if (ret > MTP_RESP_UNDEFINED) {
            mtp_unlock();
            free(handles);
            return ret;
        }
    }
    mtp_unlock();

    uint32_t payload_len = sizeof(uint32_t) + count * sizeof(uint32_t);
    uint8_t *payload = malloc(payload_len);
    if (payload == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP handle response");
        free(handles);
        return MTP_RESP_STORE_FULL;
    }
    memcpy(payload, &count, sizeof(count));
    memcpy(payload + sizeof(count), handles, count * sizeof(uint32_t));
    free(handles);
    return mtp_start_buffered_data(cb_data, MTP_OP_GET_OBJECT_HANDLES, payload, payload_len);
}

static int32_t mtp_get_object_info(tud_mtp_cb_data_t *cb_data)
{
    uint32_t handle = cb_data->command_container->params[0];
    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle(handle);
    if (object == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    struct stat st;
    if (stat(object->path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat MTP object info %s: %s", object->path, strerror(errno));
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

static int32_t mtp_start_read(tud_mtp_cb_data_t *cb_data, uint64_t offset, uint64_t length)
{
    uint32_t handle = cb_data->command_container->params[0];
    char *path = NULL;
    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle(handle);
    if (object == NULL || object->directory) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    struct stat st;
    if (stat(object->path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat MTP object before read %s: %s", object->path, strerror(errno));
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

    mtp_clear_active_read();
    s_mtp.mux_protected.active_read.active = true;
    s_mtp.mux_protected.active_read.file = file;
    s_mtp.mux_protected.active_read.storage = storage;
    s_mtp.mux_protected.active_read.handle = handle;
    s_mtp.mux_protected.active_read.expected = length;
    s_mtp.mux_protected.active_read.remaining = length;
    s_mtp.mux_protected.active_read.sent = 0;
    s_mtp.mux_protected.active_read.path = path;
    path = NULL;

    mtp_container_info_t *container = &cb_data->io_container;
    size_t first_len = length < container->payload_bytes ? (size_t)length : container->payload_bytes;
    if (first_len > 0 && fread(container->payload, 1, first_len, file) != first_len) {
        ESP_LOGE(TAG, "failed to read first MTP object chunk from %s", s_mtp.mux_protected.active_read.path);
        mtp_clear_active_read();
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }
    s_mtp.mux_protected.active_read.sent = first_len;
    s_mtp.mux_protected.active_read.remaining -= first_len;
    container->header->len = sizeof(mtp_container_header_t) + (uint32_t)length;
    MTP_TRACEI("MTP object read begin: handle=%" PRIu32 " offset=%" PRIu64 " bytes=%" PRIu64 " object_size=%" PRIu64 " first=%u path=%s",
               handle, offset, length, object_size, (unsigned)first_len, s_mtp.mux_protected.active_read.path);
    int32_t resp = tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
    if (resp != 0) {
        mtp_clear_active_read();
    }
    mtp_unlock();
    return resp;
}

static int32_t mtp_continue_read(tud_mtp_cb_data_t *cb_data)
{
    mtp_lock();
    if (!s_mtp.mux_protected.active_read.active || s_mtp.mux_protected.active_read.file == NULL) {
        ESP_LOGW(TAG, "MTP read continuation without active object");
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    size_t chunk = s_mtp.mux_protected.active_read.remaining < container->payload_bytes ? (size_t)s_mtp.mux_protected.active_read.remaining : container->payload_bytes;
    if (chunk == 0) {
        mtp_unlock();
        return 0;
    }
    if (fread(container->payload, 1, chunk, s_mtp.mux_protected.active_read.file) != chunk) {
        ESP_LOGE(TAG, "failed to read MTP object chunk from %s", s_mtp.mux_protected.active_read.path ? s_mtp.mux_protected.active_read.path : "(unknown)");
        mtp_clear_active_read();
        mtp_unlock();
        return MTP_RESP_GENERAL_ERROR;
    }
    s_mtp.mux_protected.active_read.sent += chunk;
    s_mtp.mux_protected.active_read.remaining -= chunk;
    MTP_TRACEI("MTP object read chunk: handle=%" PRIu32 " chunk=%u sent=%" PRIu64 "/%" PRIu64 " remaining=%" PRIu64 " path=%s",
               s_mtp.mux_protected.active_read.handle, (unsigned)chunk, s_mtp.mux_protected.active_read.sent, s_mtp.mux_protected.active_read.expected, s_mtp.mux_protected.active_read.remaining,
               s_mtp.mux_protected.active_read.path ? s_mtp.mux_protected.active_read.path : "(unknown)");
    int32_t resp = tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
    if (resp != 0) {
        mtp_clear_active_read();
    }
    mtp_unlock();
    return resp;
}

static int32_t mtp_get_object(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_COMMAND) {
        return mtp_start_read(cb_data, 0, UINT64_MAX);
    }
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_read(cb_data);
    }
    return 0;
}

static int32_t mtp_get_partial_object(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_COMMAND) {
        return mtp_start_read(cb_data, cb_data->command_container->params[1], cb_data->command_container->params[2]);
    }
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_read(cb_data);
    }
    return 0;
}

static int32_t mtp_get_partial_object64(tud_mtp_cb_data_t *cb_data)
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

int32_t mtp_delete_object(tud_mtp_cb_data_t *cb_data)
{
    mtp_lock();
    if (!s_mtp.mux_protected.session_open) {
        mtp_unlock();
        return MTP_RESP_SESSION_NOT_OPEN;
    }

    uint32_t handle = cb_data->command_container->params[0];
    mtp_object_t *object = mtp_object_from_handle(handle);
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

    if (s_mtp.mux_protected.active_edit.active) {
        mtp_object_t *edit_object = mtp_object_from_handle(s_mtp.mux_protected.active_edit.handle);
        if (edit_object != NULL && edit_object->storage == storage && mtp_path_is_child_of(edit_object->path, path)) {
            ESP_LOGI(TAG, "clearing MTP edit for deleted handle %" PRIu32, edit_object->handle);
            mtp_clear_active_edit();
            mtp_clear_partial_write();
        }
    }
    mtp_drop_objects_under_path(storage, path);
    MTP_TRACEI("MTP delete: handle=%" PRIu32 " path=%s", handle, path);
    mtp_unlock();
    free(path);
    return MTP_RESP_OK;
}

static int32_t mtp_begin_edit_object(tud_mtp_cb_data_t *cb_data)
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

static int32_t mtp_send_partial_object(tud_mtp_cb_data_t *cb_data)
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
        mtp_clear_partial_write();
        mtp_object_t *object = NULL;
        int32_t ret = mtp_get_active_edit_object_locked(handle, &object);
        if (ret == MTP_RESP_OK && offset > (uint64_t)LONG_MAX) {
            ESP_LOGW(TAG, "MTP partial write offset is too large: offset=%" PRIu64, offset);
            ret = MTP_RESP_OBJECT_TOO_LARGE;
        }
        if (ret == MTP_RESP_OK) {
            s_mtp.mux_protected.partial_write.active = true;
            s_mtp.mux_protected.partial_write.handle = handle;
            s_mtp.mux_protected.partial_write.offset = offset;
            s_mtp.mux_protected.partial_write.expected_size = data_len;
            s_mtp.mux_protected.partial_write.written = 0;
            MTP_TRACEI("MTP partial write begin: handle=%" PRIu32 " offset=%" PRIu64 " len=%" PRIu32, handle, offset, data_len);
            container->header->len = sizeof(mtp_container_header_t) + data_len;
            if (data_len == 0) {
                mtp_clear_partial_write();
                mtp_update_object_stat_locked(object);
                mtp_unlock();
                return MTP_RESP_OK;
            }
            if (!tud_mtp_data_receive(container)) {
                ESP_LOGE(TAG, "failed to arm MTP partial object receive");
                mtp_clear_partial_write();
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
    if (!s_mtp.mux_protected.partial_write.active || s_mtp.mux_protected.partial_write.handle != handle) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    mtp_object_t *object = NULL;
    int32_t ret = mtp_get_active_edit_object_locked(handle, &object);
    if (ret == MTP_RESP_OK) {
        uint32_t remaining = s_mtp.mux_protected.partial_write.expected_size - s_mtp.mux_protected.partial_write.written;
        size_t to_write = container->payload_bytes < remaining ? container->payload_bytes : remaining;
        ret = mtp_write_object_range_locked(object, s_mtp.mux_protected.partial_write.offset + s_mtp.mux_protected.partial_write.written, container->payload, to_write);
        if (ret == MTP_RESP_OK) {
            s_mtp.mux_protected.partial_write.written += (uint32_t)to_write;
            if (s_mtp.mux_protected.partial_write.written < s_mtp.mux_protected.partial_write.expected_size) {
                ret = tud_mtp_data_receive(container) ? 0 : MTP_RESP_DEVICE_BUSY;
            } else {
                ret = MTP_RESP_OK;
            }
        }
    }
    if (ret > MTP_RESP_UNDEFINED && ret != MTP_RESP_OK) {
        mtp_clear_partial_write();
    }
    mtp_unlock();
    return ret;
}

static int32_t mtp_truncate_object(tud_mtp_cb_data_t *cb_data)
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

static int32_t mtp_end_edit_object(tud_mtp_cb_data_t *cb_data)
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
        ret = mtp_update_object_stat_locked(object);
        if (ret == MTP_RESP_OK) {
            MTP_TRACEI("MTP edit end: handle=%" PRIu32 " path=%s size=%" PRIu64, handle, object->path, object->size);
        }
    }
    mtp_clear_partial_write();
    if (s_mtp.mux_protected.active_edit.active && s_mtp.mux_protected.active_edit.handle == handle) {
        mtp_clear_active_edit();
    }
    mtp_unlock();
    return ret;
}

static int32_t mtp_send_object_info(tud_mtp_cb_data_t *cb_data)
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
    uint32_t storage_id = command->params[0] == MTP_ROOT_PARENT ? info->storage_id : command->params[0];
    mtp_lock();
    struct tinyusb_mtp_storage_s *storage = mtp_storage_from_id(storage_id);
    if (storage == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_STORAGE_ID;
    }

    const char *parent_dir = NULL;
    uint32_t parent_handle = command->params[1];
    uint32_t parent_resp = mtp_parent_handle_to_dir(storage, parent_handle, &parent_dir);
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

    bool created_object = created_path || mtp_find_object_by_path(storage, path) == NULL;
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
    mtp_object_t *object = mtp_get_or_create_object(storage, object_parent, path, &st);
    if (object == NULL) {
        if (created_path) {
            mtp_remove_created_path(path, is_dir);
        }
        memset(&s_mtp.mux_protected.pending_write, 0, sizeof(s_mtp.mux_protected.pending_write));
        free(path);
        mtp_unlock();
        return MTP_RESP_STORE_FULL;
    }

    if (replace_existing) {
        write_path = mtp_make_internal_path(parent_dir, object->handle, MTP_TEMP_NAME_PREFIX);
        if (write_path == NULL) {
            if (created_object) {
                mtp_free_object(object);
            }
            free(path);
            mtp_unlock();
            return MTP_RESP_STORE_FULL;
        }
        backup_path = mtp_make_internal_path(parent_dir, object->handle, MTP_BACKUP_NAME_PREFIX);
        if (backup_path == NULL) {
            if (created_object) {
                mtp_free_object(object);
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
                mtp_free_object(object);
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

    s_mtp.mux_protected.pending_write.handle = object->handle;
    s_mtp.mux_protected.pending_write.storage_id = storage->storage_id;
    s_mtp.mux_protected.pending_write.parent_handle = object_parent;
    s_mtp.mux_protected.pending_write.expected_size = info->object_compressed_size;
    s_mtp.mux_protected.pending_write.path = strdup(path);
    s_mtp.mux_protected.pending_write.write_path = write_path;
    s_mtp.mux_protected.pending_write.backup_path = backup_path;
    s_mtp.mux_protected.pending_write.active = !is_dir && info->object_compressed_size > 0;
    s_mtp.mux_protected.pending_write.created_path = created_path;
    s_mtp.mux_protected.pending_write.created_object = created_object;
    s_mtp.mux_protected.pending_write.replace_existing = replace_existing;
    s_mtp.mux_protected.pending_write.directory = is_dir;
    if (s_mtp.mux_protected.pending_write.path == NULL) {
        ESP_LOGE(TAG, "failed to allocate pending write path");
        if (created_object) {
            mtp_free_object(object);
        }
        if (created_path) {
            mtp_remove_created_path(write_path ? write_path : path, is_dir);
        }
        free(backup_path);
        free(write_path);
        memset(&s_mtp.mux_protected.pending_write, 0, sizeof(s_mtp.mux_protected.pending_write));
        free(path);
        mtp_unlock();
        return MTP_RESP_STORE_FULL;
    }
    write_path = NULL;
    backup_path = NULL;
    if (!is_dir && !created_path) {
        ESP_LOGW(TAG, "MTP file target was not prepared for write: %s", path);
        if (created_object) {
            mtp_free_object(object);
        }
        free(s_mtp.mux_protected.pending_write.path);
        free(s_mtp.mux_protected.pending_write.write_path);
        free(s_mtp.mux_protected.pending_write.backup_path);
        memset(&s_mtp.mux_protected.pending_write, 0, sizeof(s_mtp.mux_protected.pending_write));
        free(path);
        mtp_unlock();
        return MTP_RESP_ACCESS_DENIED;
    }
    MTP_TRACEI("MTP object info write: handle=%" PRIu32 " storage=0x%08" PRIx32 " parent=%" PRIu32 " size=%" PRIu64 " dir=%d replace=%d path=%s",
               object->handle, storage->storage_id, s_mtp.mux_protected.pending_write.parent_handle, s_mtp.mux_protected.pending_write.expected_size, is_dir, replace_existing, path);
    free(path);
    mtp_unlock();
    return 0;
}

static int32_t mtp_send_object(tud_mtp_cb_data_t *cb_data)
{
    mtp_container_info_t *container = &cb_data->io_container;

    mtp_lock();
    if (!s_mtp.mux_protected.pending_write.active || s_mtp.mux_protected.pending_write.path == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    if (cb_data->phase == MTP_PHASE_COMMAND) {
        container->header->len = sizeof(mtp_container_header_t) + (uint32_t)s_mtp.mux_protected.pending_write.expected_size;
        int32_t resp = tud_mtp_data_receive(container) ? 0 : MTP_RESP_DEVICE_BUSY;
        if (resp != 0) {
            mtp_abort_pending_write_locked(resp);
        } else {
            MTP_TRACEI("MTP object write begin: handle=%" PRIu32 " bytes=%" PRIu64 " path=%s",
                       s_mtp.mux_protected.pending_write.handle, s_mtp.mux_protected.pending_write.expected_size, s_mtp.mux_protected.pending_write.path);
        }
        mtp_unlock();
        return resp;
    }

    if (s_mtp.mux_protected.pending_write.file == NULL) {
        const char *write_path = s_mtp.mux_protected.pending_write.write_path ? s_mtp.mux_protected.pending_write.write_path : s_mtp.mux_protected.pending_write.path;
        s_mtp.mux_protected.pending_write.file = fopen(write_path, "wb");
        if (s_mtp.mux_protected.pending_write.file == NULL) {
            ESP_LOGE(TAG, "failed to open MTP object for write %s: %s", write_path, strerror(errno));
            mtp_abort_pending_write_locked(MTP_RESP_ACCESS_DENIED);
            mtp_unlock();
            return MTP_RESP_ACCESS_DENIED;
        }
    }

    uint64_t remaining = s_mtp.mux_protected.pending_write.expected_size - s_mtp.mux_protected.pending_write.written;
    size_t to_write = container->payload_bytes;
    if ((uint64_t)to_write > remaining) {
        to_write = (size_t)remaining;
    }

    if (to_write > 0) {
        if (fwrite(container->payload, 1, to_write, s_mtp.mux_protected.pending_write.file) != to_write) {
            const char *write_path = s_mtp.mux_protected.pending_write.write_path ? s_mtp.mux_protected.pending_write.write_path : s_mtp.mux_protected.pending_write.path;
            ESP_LOGE(TAG, "failed to write MTP object %s: %s", write_path, strerror(errno));
            mtp_abort_pending_write_locked(MTP_RESP_STORE_FULL);
            mtp_unlock();
            return MTP_RESP_STORE_FULL;
        }
        s_mtp.mux_protected.pending_write.written += to_write;
    }

    if (s_mtp.mux_protected.pending_write.written < s_mtp.mux_protected.pending_write.expected_size) {
        if (!tud_mtp_data_receive(container)) {
            mtp_abort_pending_write_locked(MTP_RESP_DEVICE_BUSY);
            mtp_unlock();
            return MTP_RESP_DEVICE_BUSY;
        }
    }
    mtp_unlock();
    return 0;
}

static bool mtp_builder_reserve(mtp_payload_builder_t *builder, uint32_t add_len)
{
    if (builder->len > UINT32_MAX - add_len) {
        ESP_LOGE(TAG, "MTP payload builder length overflow");
        return false;
    }
    uint32_t need = builder->len + add_len;
    if (need <= builder->cap) {
        return true;
    }

    uint32_t new_cap = builder->cap ? builder->cap : 64;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2U) {
            new_cap = need;
            break;
        }
        new_cap *= 2U;
    }

    uint8_t *new_data = realloc(builder->data, new_cap);
    if (new_data == NULL) {
        ESP_LOGE(TAG, "failed to grow MTP payload to %" PRIu32 " bytes", new_cap);
        return false;
    }
    builder->data = new_data;
    builder->cap = new_cap;
    return true;
}

static bool mtp_builder_append_raw(mtp_payload_builder_t *builder, const void *data, uint32_t len)
{
    if (!mtp_builder_reserve(builder, len)) {
        return false;
    }
    memcpy(builder->data + builder->len, data, len);
    builder->len += len;
    return true;
}

static bool mtp_builder_append_uint8(mtp_payload_builder_t *builder, uint8_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

static bool mtp_builder_append_uint16(mtp_payload_builder_t *builder, uint16_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

static bool mtp_builder_append_uint32(mtp_payload_builder_t *builder, uint32_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

static bool mtp_builder_append_uint64(mtp_payload_builder_t *builder, uint64_t value)
{
    return mtp_builder_append_raw(builder, &value, sizeof(value));
}

static bool mtp_builder_append_cstring(mtp_payload_builder_t *builder, const char *value)
{
    size_t len = strlen(value);
    if (len >= UINT8_MAX) {
        len = UINT8_MAX - 1U;
    }
    if (len == 0) {
        return mtp_builder_append_uint8(builder, 0);
    }
    uint8_t count = (uint8_t)len + 1U;
    if (!mtp_builder_append_uint8(builder, count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        uint16_t ch = i < len ? (uint8_t)value[i] : 0;
        if (!mtp_builder_append_uint16(builder, ch)) {
            return false;
        }
    }
    return true;
}

static bool mtp_builder_append_utf8_string(mtp_payload_builder_t *builder, const char *value)
{
    size_t units = mtp_utf8_count_utf16_units(value);
    if (units == 0) {
        return mtp_builder_append_uint8(builder, 0);
    }
    if (!mtp_builder_append_uint8(builder, (uint8_t)units + 1U)) {
        return false;
    }

    size_t written_units = 0;
    const char *cursor = value ? value : "";
    while (*cursor && written_units < units) {
        uint32_t codepoint = mtp_utf8_next_codepoint_or_replacement(&cursor);
        if (codepoint > 0xffff) {
            codepoint -= 0x10000;
            if (!mtp_builder_append_uint16(builder, (uint16_t)(0xd800 | (codepoint >> 10))) ||
                    !mtp_builder_append_uint16(builder, (uint16_t)(0xdc00 | (codepoint & 0x3ff)))) {
                return false;
            }
            written_units += 2U;
        } else if (!mtp_builder_append_uint16(builder, (uint16_t)codepoint)) {
            return false;
        } else {
            written_units++;
        }
    }
    return mtp_builder_append_uint16(builder, 0);
}

static bool mtp_object_prop_is_supported(uint16_t prop_code)
{
    for (size_t i = 0; i < sizeof(s_supported_object_props) / sizeof(s_supported_object_props[0]); i++) {
        if (s_supported_object_props[i] == prop_code) {
            return true;
        }
    }
    return false;
}

static uint16_t mtp_object_prop_datatype(uint16_t prop_code)
{
    switch (prop_code) {
    case MTP_OBJ_PROP_OBJECT_FORMAT:
    case MTP_OBJ_PROP_PROTECTION_STATUS:
    case MTP_OBJ_PROP_ASSOCIATION_TYPE:
        return MTP_DATA_TYPE_UINT16;
    case MTP_OBJ_PROP_NON_CONSUMABLE:
        return MTP_DATA_TYPE_UINT8;
    case MTP_OBJ_PROP_STORAGE_ID:
    case MTP_OBJ_PROP_PARENT_OBJECT:
        return MTP_DATA_TYPE_UINT32;
    case MTP_OBJ_PROP_OBJECT_SIZE:
        return MTP_DATA_TYPE_UINT64;
    case MTP_OBJ_PROP_PERSISTENT_UID:
        return MTP_DATA_TYPE_UINT128;
    case MTP_OBJ_PROP_OBJECT_FILE_NAME:
    case MTP_OBJ_PROP_DATE_CREATED:
    case MTP_OBJ_PROP_DATE_MODIFIED:
    case MTP_OBJ_PROP_NAME:
    case MTP_OBJ_PROP_DATE_ADDED:
    case MTP_OBJ_PROP_DISPLAY_NAME:
        return MTP_DATA_TYPE_STR;
    default:
        return MTP_DATA_TYPE_UNDEFINED;
    }
}

static bool mtp_object_prop_is_settable(uint16_t prop_code)
{
    return prop_code == MTP_OBJ_PROP_OBJECT_FILE_NAME;
}

static bool mtp_object_prop_accepts_set(uint16_t prop_code)
{
    return prop_code == MTP_OBJ_PROP_OBJECT_FILE_NAME || prop_code == MTP_OBJ_PROP_NAME || prop_code == MTP_OBJ_PROP_DISPLAY_NAME;
}

static void mtp_add_object_prop_default_value(mtp_container_info_t *container, uint16_t prop_code)
{
    switch (mtp_object_prop_datatype(prop_code)) {
    case MTP_DATA_TYPE_UINT8:
        (void)mtp_container_add_uint8(container, 0);
        break;
    case MTP_DATA_TYPE_UINT16:
        (void)mtp_container_add_uint16(container, 0);
        break;
    case MTP_DATA_TYPE_UINT32:
        (void)mtp_container_add_uint32(container, 0);
        break;
    case MTP_DATA_TYPE_UINT64:
        (void)mtp_container_add_uint64(container, 0);
        break;
    case MTP_DATA_TYPE_UINT128: {
        uint8_t empty_uid[16] = { 0 };
        (void)mtp_container_add_uint128(container, empty_uid);
        break;
    }
    case MTP_DATA_TYPE_STR:
        (void)mtp_container_add_cstring(container, "");
        break;
    default:
        break;
    }
}

static int32_t mtp_get_object_props_supported(tud_mtp_cb_data_t *cb_data)
{
    (void)cb_data->command_container->params[0];
    (void)mtp_container_add_auint16(&cb_data->io_container, sizeof(s_supported_object_props) / sizeof(s_supported_object_props[0]), s_supported_object_props);
    return tud_mtp_data_send(&cb_data->io_container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

static int32_t mtp_get_object_prop_desc(tud_mtp_cb_data_t *cb_data)
{
    uint16_t prop_code = (uint16_t)cb_data->command_container->params[0];
    if (!mtp_object_prop_is_supported(prop_code)) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    (void)mtp_container_add_uint16(container, prop_code);
    (void)mtp_container_add_uint16(container, mtp_object_prop_datatype(prop_code));
    (void)mtp_container_add_uint8(container, mtp_object_prop_is_settable(prop_code) ? MTP_MODE_GET_SET : MTP_MODE_GET);
    mtp_add_object_prop_default_value(container, prop_code);
    (void)mtp_container_add_uint32(container, 0);
    (void)mtp_container_add_uint8(container, 0);
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

static int32_t mtp_get_object_prop_value(tud_mtp_cb_data_t *cb_data)
{
    uint32_t handle = cb_data->command_container->params[0];
    uint16_t prop_code = (uint16_t)cb_data->command_container->params[1];
    if (!mtp_object_prop_is_supported(prop_code)) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }

    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle(handle);
    if (object == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    struct stat st;
    if (stat(object->path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat MTP object property %s: %s", object->path, strerror(errno));
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_update_object_from_stat(object, &st);

    const char *name = mtp_basename(object->path);
    uint32_t storage_id = object->storage->storage_id;
    uint32_t parent = object->parent;
    uint16_t format = mtp_format_from_name(name, object->directory);
    uint16_t association = object->directory ? MTP_ASSOCIATION_GENERIC_FOLDER : MTP_ASSOCIATION_UNDEFINED;
    uint64_t size = object->size;
    time_t mtime = object->mtime;
    uint8_t persistent_uid[16];
    mtp_make_persistent_uid(object, persistent_uid);
    char name_copy[MTP_MAX_NAME_BYTES + 1];
    (void)snprintf(name_copy, sizeof(name_copy), "%s", name);
    char display_name[MTP_MAX_NAME_BYTES + 1];
    mtp_copy_display_name(name, object->directory, display_name, sizeof(display_name));
    mtp_unlock();

    mtp_container_info_t *container = &cb_data->io_container;
    switch (prop_code) {
    case MTP_OBJ_PROP_STORAGE_ID:
        (void)mtp_container_add_uint32(container, storage_id);
        break;
    case MTP_OBJ_PROP_OBJECT_FORMAT:
        (void)mtp_container_add_uint16(container, format);
        break;
    case MTP_OBJ_PROP_PROTECTION_STATUS:
        (void)mtp_container_add_uint16(container, MTP_PROTECTION_STATUS_NO_PROTECTION);
        break;
    case MTP_OBJ_PROP_NON_CONSUMABLE:
        (void)mtp_container_add_uint8(container, 0);
        break;
    case MTP_OBJ_PROP_OBJECT_SIZE:
        (void)mtp_container_add_uint64(container, size);
        break;
    case MTP_OBJ_PROP_PERSISTENT_UID:
        (void)mtp_container_add_uint128(container, persistent_uid);
        break;
    case MTP_OBJ_PROP_ASSOCIATION_TYPE:
        (void)mtp_container_add_uint16(container, association);
        break;
    case MTP_OBJ_PROP_PARENT_OBJECT:
        (void)mtp_container_add_uint32(container, parent);
        break;
    case MTP_OBJ_PROP_DATE_CREATED:
    case MTP_OBJ_PROP_DATE_ADDED:
    case MTP_OBJ_PROP_DATE_MODIFIED: {
        char date[32];
        mtp_time_to_date_string(mtime, date, sizeof(date));
        (void)mtp_container_add_utf8_string(container, date);
        break;
    }
    case MTP_OBJ_PROP_OBJECT_FILE_NAME:
        (void)mtp_container_add_utf8_string(container, name_copy);
        break;
    case MTP_OBJ_PROP_NAME:
    case MTP_OBJ_PROP_DISPLAY_NAME:
        (void)mtp_container_add_utf8_string(container, display_name);
        break;
    default:
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

static bool mtp_builder_append_object_prop(mtp_payload_builder_t *builder, const mtp_object_t *object, uint16_t prop_code)
{
    const char *name = mtp_basename(object->path);
    uint8_t persistent_uid[16];
    mtp_make_persistent_uid(object, persistent_uid);
    char display_name[MTP_MAX_NAME_BYTES + 1];
    mtp_copy_display_name(name, object->directory, display_name, sizeof(display_name));
    uint16_t format = mtp_format_from_name(name, object->directory);
    uint16_t datatype = mtp_object_prop_datatype(prop_code);
    if (!mtp_builder_append_uint32(builder, object->handle) ||
            !mtp_builder_append_uint16(builder, prop_code) ||
            !mtp_builder_append_uint16(builder, datatype)) {
        return false;
    }

    switch (prop_code) {
    case MTP_OBJ_PROP_STORAGE_ID:
        return mtp_builder_append_uint32(builder, object->storage->storage_id);
    case MTP_OBJ_PROP_OBJECT_FORMAT:
        return mtp_builder_append_uint16(builder, format);
    case MTP_OBJ_PROP_PROTECTION_STATUS:
        return mtp_builder_append_uint16(builder, MTP_PROTECTION_STATUS_NO_PROTECTION);
    case MTP_OBJ_PROP_NON_CONSUMABLE:
        return mtp_builder_append_uint8(builder, 0);
    case MTP_OBJ_PROP_OBJECT_SIZE:
        return mtp_builder_append_uint64(builder, object->size);
    case MTP_OBJ_PROP_PERSISTENT_UID:
        return mtp_builder_append_raw(builder, persistent_uid, sizeof(persistent_uid));
    case MTP_OBJ_PROP_ASSOCIATION_TYPE:
        return mtp_builder_append_uint16(builder, object->directory ? MTP_ASSOCIATION_GENERIC_FOLDER : MTP_ASSOCIATION_UNDEFINED);
    case MTP_OBJ_PROP_PARENT_OBJECT:
        return mtp_builder_append_uint32(builder, object->parent);
    case MTP_OBJ_PROP_DATE_CREATED:
    case MTP_OBJ_PROP_DATE_ADDED:
    case MTP_OBJ_PROP_DATE_MODIFIED: {
        char date[32];
        mtp_time_to_date_string(object->mtime, date, sizeof(date));
        return mtp_builder_append_cstring(builder, date);
    }
    case MTP_OBJ_PROP_OBJECT_FILE_NAME:
        return mtp_builder_append_utf8_string(builder, name);
    case MTP_OBJ_PROP_NAME:
    case MTP_OBJ_PROP_DISPLAY_NAME:
        return mtp_builder_append_utf8_string(builder, display_name);
    default:
        return false;
    }
}

static int32_t mtp_builder_append_object_props_locked(mtp_payload_builder_t *builder, mtp_object_t *object, uint32_t object_format, uint32_t prop_code)
{
    struct stat st;
    if (stat(object->path, &st) != 0) {
        ESP_LOGE(TAG, "failed to stat MTP object prop list %s: %s", object->path, strerror(errno));
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_update_object_from_stat(object, &st);

    uint16_t format = mtp_format_from_name(mtp_basename(object->path), object->directory);
    if (object_format != 0 && object_format != MTP_ROOT_PARENT && (uint16_t)object_format != format) {
        return MTP_RESP_OK;
    }

    if (prop_code == MTP_ROOT_PARENT) {
        for (size_t i = 0; i < sizeof(s_supported_object_props) / sizeof(s_supported_object_props[0]); i++) {
            if (!mtp_builder_append_object_prop(builder, object, s_supported_object_props[i])) {
                return MTP_RESP_STORE_FULL;
            }
            builder->count++;
        }
        return MTP_RESP_OK;
    }

    if (!mtp_object_prop_is_supported((uint16_t)prop_code)) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }
    if (!mtp_builder_append_object_prop(builder, object, (uint16_t)prop_code)) {
        return MTP_RESP_STORE_FULL;
    }
    builder->count++;
    return MTP_RESP_OK;
}

// Use an explicit queue to avoid recursive traversal on host-supplied depth.
static int32_t mtp_enqueue_prop_list_children_locked(mtp_prop_list_visit_t *queue, uint32_t *tail, uint32_t max_items,
                                                     struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle, uint32_t depth,
                                                     uint32_t *child_handles)
{
    uint32_t count = 0;
    int32_t ret = mtp_scan_children(storage, parent_handle, 0, child_handles, CONFIG_TINYUSB_MTP_MAX_OBJECTS, &count);
    if (ret != 0) {
        return ret;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (*tail >= max_items) {
            ESP_LOGE(TAG, "MTP prop list traversal queue full");
            return MTP_RESP_STORE_FULL;
        }
        queue[*tail].handle = child_handles[i];
        queue[*tail].depth = depth;
        (*tail)++;
    }
    return MTP_RESP_OK;
}

static int32_t mtp_builder_append_prop_visit_queue_locked(mtp_payload_builder_t *builder, mtp_prop_list_visit_t *queue, uint32_t *tail,
                                                          uint32_t object_format, uint32_t prop_code, uint32_t *child_handles)
{
    uint32_t head = 0;
    while (head < *tail) {
        mtp_prop_list_visit_t visit = queue[head++];
        mtp_object_t *object = mtp_object_from_handle(visit.handle);
        if (object == NULL) {
            continue;
        }

        int32_t ret = mtp_builder_append_object_props_locked(builder, object, object_format, prop_code);
        if (ret != MTP_RESP_OK) {
            return ret;
        }
        if (!object->directory || visit.depth == 0) {
            continue;
        }

        uint32_t child_depth = visit.depth == MTP_ROOT_PARENT ? MTP_ROOT_PARENT : visit.depth - 1U;
        ret = mtp_enqueue_prop_list_children_locked(queue, tail, CONFIG_TINYUSB_MTP_MAX_OBJECTS, object->storage, object->handle, child_depth, child_handles);
        if (ret != MTP_RESP_OK) {
            return ret;
        }
    }
    return MTP_RESP_OK;
}

static int32_t mtp_builder_append_storage_tree_props_locked(mtp_payload_builder_t *builder, struct tinyusb_mtp_storage_s *storage, uint32_t object_format,
                                                            uint32_t prop_code, uint32_t depth)
{
    mtp_prop_list_visit_t *queue = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(*queue));
    uint32_t *child_handles = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(*child_handles));
    if (queue == NULL || child_handles == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP prop list traversal buffers");
        free(queue);
        free(child_handles);
        return MTP_RESP_STORE_FULL;
    }

    uint32_t tail = 0;
    int32_t ret = mtp_enqueue_prop_list_children_locked(queue, &tail, CONFIG_TINYUSB_MTP_MAX_OBJECTS, storage, MTP_ROOT_PARENT, depth, child_handles);
    if (ret == MTP_RESP_OK) {
        ret = mtp_builder_append_prop_visit_queue_locked(builder, queue, &tail, object_format, prop_code, child_handles);
    }
    free(queue);
    free(child_handles);
    return ret;
}

static int32_t mtp_builder_append_object_tree_props_locked(mtp_payload_builder_t *builder, mtp_object_t *object, uint32_t object_format, uint32_t prop_code,
                                                           uint32_t depth)
{
    mtp_prop_list_visit_t *queue = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(*queue));
    uint32_t *child_handles = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(*child_handles));
    if (queue == NULL || child_handles == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP prop list traversal buffers");
        free(queue);
        free(child_handles);
        return MTP_RESP_STORE_FULL;
    }

    uint32_t tail = 1;
    queue[0].handle = object->handle;
    queue[0].depth = depth;
    int32_t ret = mtp_builder_append_prop_visit_queue_locked(builder, queue, &tail, object_format, prop_code, child_handles);
    free(queue);
    free(child_handles);
    return ret;
}

static int32_t mtp_get_object_prop_list(tud_mtp_cb_data_t *cb_data)
{
    if (cb_data->phase == MTP_PHASE_DATA) {
        return mtp_continue_buffered_data(cb_data);
    }
    if (cb_data->phase != MTP_PHASE_COMMAND) {
        return 0;
    }

    const mtp_container_command_t *command = cb_data->command_container;
    uint32_t param_count = command->header.len > sizeof(mtp_container_header_t) ? (command->header.len - sizeof(mtp_container_header_t)) / sizeof(uint32_t) : 0;
    uint32_t object_handle = param_count >= 1 ? command->params[0] : MTP_OBJECT_HANDLE_INVALID;
    uint32_t object_format = param_count >= 2 ? command->params[1] : 0;
    uint32_t prop_code = param_count >= 3 ? command->params[2] : MTP_ROOT_PARENT;
    uint32_t group_code = param_count >= 4 ? command->params[3] : 0;
    uint32_t depth = param_count >= 5 ? command->params[4] : 0;
    if (group_code != 0) {
        return MTP_RESP_GROUP_NOT_SUPPORTED;
    }
    if (object_handle == MTP_OBJECT_HANDLE_INVALID) {
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    if (prop_code != MTP_ROOT_PARENT && !mtp_object_prop_is_supported((uint16_t)prop_code)) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }

    mtp_payload_builder_t builder = { 0 };
    if (!mtp_builder_append_uint32(&builder, 0)) {
        return MTP_RESP_STORE_FULL;
    }

    mtp_lock();
    int32_t ret = MTP_RESP_OK;
    if (object_handle == MTP_ROOT_PARENT) {
        for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
            if (!s_mtp.mux_protected.storages[i].used) {
                continue;
            }
            ret = mtp_builder_append_storage_tree_props_locked(&builder, &s_mtp.mux_protected.storages[i], object_format, prop_code, MTP_ROOT_PARENT);
            if (ret != MTP_RESP_OK) {
                break;
            }
        }
    } else {
        mtp_object_t *object = mtp_object_from_handle(object_handle);
        ret = object ? mtp_builder_append_object_tree_props_locked(&builder, object, object_format, prop_code, depth) : MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_unlock();

    if (ret != MTP_RESP_OK) {
        free(builder.data);
        return ret;
    }
    memcpy(builder.data, &builder.count, sizeof(builder.count));
    return mtp_start_buffered_data(cb_data, MTP_OP_GET_OBJECT_PROP_LIST, builder.data, builder.len);
}

static int32_t mtp_get_object_references(tud_mtp_cb_data_t *cb_data)
{
    uint32_t handle = cb_data->command_container->params[0];
    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle(handle);
    if (object == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    mtp_unlock();

    // This backend does not model object references; returning an empty array is valid.
    uint32_t empty_refs[1] = { 0 };
    (void)mtp_container_add_auint32(&cb_data->io_container, 0, empty_refs);
    return tud_mtp_data_send(&cb_data->io_container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

int32_t mtp_set_object_prop_value(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    uint16_t prop_code = (uint16_t)command->params[1];
    if (!mtp_session_is_open()) {
        return MTP_RESP_SESSION_NOT_OPEN;
    }
    if (!mtp_object_prop_accepts_set(prop_code)) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }

    if (cb_data->phase == MTP_PHASE_COMMAND) {
        mtp_lock();
        mtp_clear_pending_prop_set();
        mtp_unlock();
        if (!tud_mtp_data_receive(&cb_data->io_container)) {
            return MTP_RESP_DEVICE_BUSY;
        }
        return 0;
    }
    if (cb_data->phase != MTP_PHASE_DATA) {
        return 0;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    int32_t response = MTP_RESP_OK;
    if (container->header->len < sizeof(mtp_container_header_t) + 1) {
        ESP_LOGW(TAG, "invalid MTP object property value length: %" PRIu32, container->header->len);
        response = MTP_RESP_INVALID_DATASET;
    } else {
        uint32_t payload_total = container->header->len - sizeof(mtp_container_header_t);
        char new_name[MTP_MAX_NAME_BYTES + 1];
        if (payload_total > container->payload_bytes) {
            ESP_LOGW(TAG, "multi-packet MTP object property value is not supported: total=%" PRIu32 " chunk=%" PRIu32,
                     payload_total, container->payload_bytes);
            response = MTP_RESP_INVALID_DATASET;
        } else if (!mtp_utf16_to_utf8_name(container->payload, payload_total, new_name, sizeof(new_name))) {
            ESP_LOGW(TAG, "invalid MTP object property name value");
            response = MTP_RESP_INVALID_OBJECT_PROP_VALUE;
        } else {
            mtp_lock();
            mtp_object_t *object = mtp_object_from_handle(command->params[0]);
            if (object == NULL) {
                response = MTP_RESP_INVALID_OBJECT_HANDLE;
            } else if (prop_code == MTP_OBJ_PROP_OBJECT_FILE_NAME) {
                response = mtp_rename_object_locked(object, new_name);
            } else {
                // Windows may set Name/DisplayName without the file extension after uploading; keep the real file path unchanged.
                MTP_TRACEI("MTP display property accepted without rename: handle=%" PRIu32 " prop=0x%04x value=%s path=%s",
                           command->params[0], prop_code, new_name, object->path);
                response = MTP_RESP_OK;
            }
            mtp_unlock();
        }
    }

    if (response == MTP_RESP_OK && !mtp_data_phase_will_complete(cb_data)) {
        ESP_LOGW(TAG, "incomplete MTP object property value received");
        return MTP_RESP_INVALID_DATASET;
    }
    return response;
}

static int32_t mtp_get_device_property(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    uint16_t prop_code = (uint16_t)command->params[0];
    if (prop_code != MTP_DEV_PROP_DEVICE_FRIENDLY_NAME) {
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }

    mtp_container_info_t *container = &cb_data->io_container;
    mtp_lock();
    if (command->header.code == MTP_OP_GET_DEVICE_PROP_DESC) {
        mtp_device_prop_desc_header_t header = {
            .device_property_code = prop_code,
            .datatype = MTP_DATA_TYPE_STR,
            .get_set = MTP_MODE_GET,
        };
        (void)mtp_container_add_raw(container, &header, sizeof(header));
        (void)mtp_container_add_utf8_string(container, s_mtp.constant.friendly_name);
        (void)mtp_container_add_utf8_string(container, s_mtp.constant.friendly_name);
        (void)mtp_container_add_uint8(container, 0);
    } else {
        (void)mtp_container_add_utf8_string(container, s_mtp.constant.friendly_name);
    }
    mtp_unlock();
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

#endif /* CONFIG_TINYUSB_MTP_ENABLED */
