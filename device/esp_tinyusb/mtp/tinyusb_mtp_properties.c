/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_codec.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_properties.h"
#include "mtp/tinyusb_mtp_transfer.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_prop";

typedef struct {
    uint32_t handle;
    uint32_t depth;
} mtp_prop_list_visit_t;

typedef struct {
    uint16_t code;
    uint16_t datatype;
    bool reported_settable;
    bool accepts_set;
} mtp_object_prop_descriptor_t;

static const mtp_object_prop_descriptor_t s_object_props[] = {
    { MTP_OBJ_PROP_STORAGE_ID, MTP_DATA_TYPE_UINT32, false, false },
    { MTP_OBJ_PROP_OBJECT_FORMAT, MTP_DATA_TYPE_UINT16, false, false },
    { MTP_OBJ_PROP_PROTECTION_STATUS, MTP_DATA_TYPE_UINT16, false, false },
    { MTP_OBJ_PROP_OBJECT_SIZE, MTP_DATA_TYPE_UINT64, false, false },
    { MTP_OBJ_PROP_ASSOCIATION_TYPE, MTP_DATA_TYPE_UINT16, false, false },
    { MTP_OBJ_PROP_OBJECT_FILE_NAME, MTP_DATA_TYPE_STR, true, true },
    { MTP_OBJ_PROP_DATE_CREATED, MTP_DATA_TYPE_STR, false, false },
    { MTP_OBJ_PROP_DATE_MODIFIED, MTP_DATA_TYPE_STR, false, false },
    { MTP_OBJ_PROP_PARENT_OBJECT, MTP_DATA_TYPE_UINT32, false, false },
    { MTP_OBJ_PROP_PERSISTENT_UID, MTP_DATA_TYPE_UINT128, false, false },
    { MTP_OBJ_PROP_NAME, MTP_DATA_TYPE_STR, false, true },
    { MTP_OBJ_PROP_DATE_ADDED, MTP_DATA_TYPE_STR, false, false },
    { MTP_OBJ_PROP_NON_CONSUMABLE, MTP_DATA_TYPE_UINT8, false, false },
    { MTP_OBJ_PROP_DISPLAY_NAME, MTP_DATA_TYPE_STR, false, true },
};

static const mtp_object_prop_descriptor_t *mtp_object_prop_find(uint16_t prop_code)
{
    for (size_t i = 0; i < sizeof(s_object_props) / sizeof(s_object_props[0]); i++) {
        if (s_object_props[i].code == prop_code) {
            return &s_object_props[i];
        }
    }
    return NULL;
}

static uint16_t mtp_object_prop_datatype(uint16_t prop_code)
{
    const mtp_object_prop_descriptor_t *descriptor = mtp_object_prop_find(prop_code);
    return descriptor ? descriptor->datatype : MTP_DATA_TYPE_UNDEFINED;
}

static bool mtp_object_prop_is_settable(uint16_t prop_code)
{
    const mtp_object_prop_descriptor_t *descriptor = mtp_object_prop_find(prop_code);
    return descriptor && descriptor->reported_settable;
}

static bool mtp_object_prop_accepts_set(uint16_t prop_code)
{
    const mtp_object_prop_descriptor_t *descriptor = mtp_object_prop_find(prop_code);
    return descriptor && descriptor->accepts_set;
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

int32_t mtp_op_get_object_props_supported(tud_mtp_cb_data_t *cb_data)
{
    (void)cb_data->command_container->params[0];
    uint16_t codes[sizeof(s_object_props) / sizeof(s_object_props[0])];
    for (size_t i = 0; i < sizeof(s_object_props) / sizeof(s_object_props[0]); i++) {
        codes[i] = s_object_props[i].code;
    }
    (void)mtp_container_add_auint16(&cb_data->io_container, sizeof(codes) / sizeof(codes[0]), codes);
    return tud_mtp_data_send(&cb_data->io_container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

int32_t mtp_op_get_object_prop_desc(tud_mtp_cb_data_t *cb_data)
{
    uint16_t prop_code = (uint16_t)cb_data->command_container->params[0];
    if (mtp_object_prop_find(prop_code) == NULL) {
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

int32_t mtp_op_get_object_prop_value(tud_mtp_cb_data_t *cb_data)
{
    uint32_t handle = cb_data->command_container->params[0];
    uint16_t prop_code = (uint16_t)cb_data->command_container->params[1];
    if (mtp_object_prop_find(prop_code) == NULL) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }

    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
    if (object == NULL) {
        mtp_unlock();
        return MTP_RESP_INVALID_OBJECT_HANDLE;
    }

    struct stat st;
    if (stat(object->path, &st) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "failed to stat MTP object property %s: %s", object->path, strerror(err));
        if (err == ENOENT || err == ENOTDIR) {
            mtp_free_object_locked(object);
        }
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
    uint16_t format = mtp_format_from_name(mtp_basename(object->path), object->directory);
    if (object_format != 0 && object_format != MTP_ROOT_PARENT && (uint16_t)object_format != format) {
        return MTP_RESP_OK;
    }

    if (prop_code == MTP_ROOT_PARENT) {
        for (size_t i = 0; i < sizeof(s_object_props) / sizeof(s_object_props[0]); i++) {
            if (!mtp_builder_append_object_prop(builder, object, s_object_props[i].code)) {
                return MTP_RESP_GENERAL_ERROR;
            }
            builder->count++;
        }
        return MTP_RESP_OK;
    }

    if (mtp_object_prop_find((uint16_t)prop_code) == NULL) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }
    if (!mtp_builder_append_object_prop(builder, object, (uint16_t)prop_code)) {
        return MTP_RESP_GENERAL_ERROR;
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
    int32_t ret = mtp_scan_children_locked(storage, parent_handle, 0, child_handles, CONFIG_TINYUSB_MTP_MAX_OBJECTS, &count);
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
        mtp_object_t *object = mtp_object_from_handle_locked(visit.handle);
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
                                                            uint32_t prop_code, uint32_t depth, mtp_prop_list_visit_t *queue, uint32_t *child_handles)
{
    uint32_t tail = 0;
    int32_t ret = mtp_enqueue_prop_list_children_locked(queue, &tail, CONFIG_TINYUSB_MTP_MAX_OBJECTS, storage, MTP_ROOT_PARENT, depth, child_handles);
    if (ret == MTP_RESP_OK) {
        ret = mtp_builder_append_prop_visit_queue_locked(builder, queue, &tail, object_format, prop_code, child_handles);
    }
    return ret;
}

static int32_t mtp_builder_append_object_tree_props_locked(mtp_payload_builder_t *builder, mtp_object_t *object, uint32_t object_format, uint32_t prop_code,
                                                           uint32_t depth, mtp_prop_list_visit_t *queue, uint32_t *child_handles)
{
    int32_t ret = mtp_update_object_stat_locked(object);
    if (ret != MTP_RESP_OK) {
        return ret;
    }

    uint32_t tail = 1;
    queue[0].handle = object->handle;
    queue[0].depth = depth;
    return mtp_builder_append_prop_visit_queue_locked(builder, queue, &tail, object_format, prop_code, child_handles);
}

static int32_t mtp_builder_append_prop_list_locked(mtp_payload_builder_t *builder, uint32_t object_handle, uint32_t object_format, uint32_t prop_code,
                                                   uint32_t depth, mtp_prop_list_visit_t *queue, uint32_t *child_handles)
{
    if (object_handle == MTP_OBJECT_HANDLE_ALL) {
        for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
            if (!mtp_context_get()->mux_protected.storages[i].used) {
                continue;
            }
            int32_t ret = mtp_builder_append_storage_tree_props_locked(builder, &mtp_context_get()->mux_protected.storages[i], object_format, prop_code,
                                                                       MTP_DEPTH_ALL, queue, child_handles);
            if (ret != MTP_RESP_OK) {
                return ret;
            }
        }
        return MTP_RESP_OK;
    }

    mtp_object_t *object = mtp_object_from_handle_locked(object_handle);
    return object ? mtp_builder_append_object_tree_props_locked(builder, object, object_format, prop_code, depth, queue, child_handles) : MTP_RESP_INVALID_OBJECT_HANDLE;
}

int32_t mtp_op_get_object_prop_list(tud_mtp_cb_data_t *cb_data)
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
    if (prop_code != MTP_ROOT_PARENT && mtp_object_prop_find((uint16_t)prop_code) == NULL) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED;
    }

    mtp_prop_list_visit_t *queue = malloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS * sizeof(*queue));
    uint32_t *child_handles = malloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS * sizeof(*child_handles));
    if (queue == NULL || child_handles == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP prop list traversal buffers");
        free(queue);
        free(child_handles);
        return MTP_RESP_GENERAL_ERROR;
    }

    mtp_lock();
    mtp_payload_builder_t measure = { .measure_only = true };
    int32_t ret = mtp_builder_append_uint32(&measure, 0) ? mtp_builder_append_prop_list_locked(&measure, object_handle, object_format, prop_code, depth, queue, child_handles) : MTP_RESP_GENERAL_ERROR;
    mtp_payload_builder_t builder = { 0 };
    if (ret == MTP_RESP_OK) {
        builder.data = malloc(measure.len);
        if (builder.data == NULL) {
            ESP_LOGE(TAG, "failed to allocate MTP property list response: bytes=%" PRIu32, measure.len);
            ret = MTP_RESP_GENERAL_ERROR;
        } else {
            builder.cap = measure.len;
            if (!mtp_builder_append_uint32(&builder, 0)) {
                ret = MTP_RESP_GENERAL_ERROR;
            } else {
                ret = mtp_builder_append_prop_list_locked(&builder, object_handle, object_format, prop_code, depth, queue, child_handles);
            }
        }
    }
    mtp_unlock();
    free(queue);
    free(child_handles);

    if (ret != MTP_RESP_OK) {
        free(builder.data);
        return ret;
    }
    memcpy(builder.data, &builder.count, sizeof(builder.count));
    return mtp_start_buffered_data(cb_data, MTP_OP_GET_OBJECT_PROP_LIST, builder.data, builder.len);
}

int32_t mtp_op_get_object_references(tud_mtp_cb_data_t *cb_data)
{
    uint32_t handle = cb_data->command_container->params[0];
    mtp_lock();
    mtp_object_t *object = mtp_object_from_handle_locked(handle);
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

int32_t mtp_op_set_object_prop_value(tud_mtp_cb_data_t *cb_data)
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
        mtp_clear_pending_prop_set_locked();
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
            mtp_object_t *object = mtp_object_from_handle_locked(command->params[0]);
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

int32_t mtp_op_get_device_property(tud_mtp_cb_data_t *cb_data)
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
        (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.friendly_name);
        (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.friendly_name);
        (void)mtp_container_add_uint8(container, 0);
    } else {
        (void)mtp_container_add_utf8_string(container, mtp_context_get()->constant.friendly_name);
    }
    mtp_unlock();
    return tud_mtp_data_send(container) ? 0 : MTP_RESP_DEVICE_BUSY;
}

#endif
