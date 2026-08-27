/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_operations.h"
#include "mtp/tinyusb_mtp_properties.h"
#include "mtp/tinyusb_mtp_read.h"
#include "mtp/tinyusb_mtp_transfer.h"
#include "mtp/tinyusb_mtp_transport.h"
#include "mtp/tinyusb_mtp_write.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp_transport";

typedef int32_t (*mtp_op_handler_t)(tud_mtp_cb_data_t *cb_data);

typedef struct {
    uint16_t op_code;
    mtp_op_handler_t handler;
    const char *name;
    bool trace;
} mtp_op_handler_entry_t;

static const mtp_op_handler_entry_t s_handlers[] = {
    { MTP_OP_GET_DEVICE_INFO, mtp_op_get_device_info, "GetDeviceInfo", false },
    { MTP_OP_OPEN_SESSION, mtp_op_open_close_session, "OpenSession", false },
    { MTP_OP_CLOSE_SESSION, mtp_op_open_close_session, "CloseSession", false },
    { MTP_OP_GET_STORAGE_IDS, mtp_op_get_storage_ids, "GetStorageIDs", false },
    { MTP_OP_GET_STORAGE_INFO, mtp_op_get_storage_info, "GetStorageInfo", false },
    { MTP_OP_GET_NUM_OBJECTS, mtp_op_get_num_objects, "GetNumObjects", false },
    { MTP_OP_GET_OBJECT_HANDLES, mtp_op_get_object_handles, "GetObjectHandles", false },
    { MTP_OP_GET_OBJECT_INFO, mtp_op_get_object_info, "GetObjectInfo", true },
    { MTP_OP_GET_OBJECT, mtp_op_get_object, "GetObject", true },
    { MTP_OP_GET_PARTIAL_OBJECT, mtp_op_get_partial_object, "GetPartialObject", true },
    { MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64, mtp_op_get_partial_object64, "AndroidGetPartialObject64", true },
    { MTP_OP_DELETE_OBJECT, mtp_op_delete_object, "DeleteObject", true },
    { MTP_OP_SEND_OBJECT_INFO, mtp_op_send_object_info, "SendObjectInfo", true },
    { MTP_OP_SEND_OBJECT, mtp_op_send_object, "SendObject", true },
    { MTP_OP_MOVE_OBJECT, mtp_op_move_object, "MoveObject", true },
    { MTP_OP_ANDROID_BEGIN_EDIT_OBJECT, mtp_op_begin_edit_object, "AndroidBeginEditObject", true },
    { MTP_OP_ANDROID_SEND_PARTIAL_OBJECT, mtp_op_send_partial_object, "AndroidSendPartialObject", true },
    { MTP_OP_ANDROID_TRUNCATE_OBJECT, mtp_op_truncate_object, "AndroidTruncateObject", true },
    { MTP_OP_ANDROID_END_EDIT_OBJECT, mtp_op_end_edit_object, "AndroidEndEditObject", true },
    { MTP_OP_GET_DEVICE_PROP_DESC, mtp_op_get_device_property, "GetDevicePropDesc", false },
    { MTP_OP_GET_DEVICE_PROP_VALUE, mtp_op_get_device_property, "GetDevicePropValue", false },
    { MTP_OP_GET_OBJECT_PROPS_SUPPORTED, mtp_op_get_object_props_supported, "GetObjectPropsSupported", true },
    { MTP_OP_GET_OBJECT_PROP_DESC, mtp_op_get_object_prop_desc, "GetObjectPropDesc", true },
    { MTP_OP_GET_OBJECT_PROP_VALUE, mtp_op_get_object_prop_value, "GetObjectPropValue", true },
    { MTP_OP_SET_OBJECT_PROP_VALUE, mtp_op_set_object_prop_value, "SetObjectPropValue", true },
    { MTP_OP_GET_OBJECT_PROP_LIST, mtp_op_get_object_prop_list, "GetObjectPropList", true },
    { MTP_OP_GET_OBJECT_PROP_REFERENCES, mtp_op_get_object_references, "GetObjectPropReferences", true },
};

static const mtp_op_handler_entry_t *mtp_find_handler(uint16_t op_code)
{
    for (size_t i = 0; i < sizeof(s_handlers) / sizeof(s_handlers[0]); i++) {
        if (s_handlers[i].op_code == op_code) {
            return &s_handlers[i];
        }
    }
    return NULL;
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
    case MTP_RESP_TRANSACTION_CANCELLED:
        return "TransactionCancelled";
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

static bool mtp_response_should_trace(int32_t response)
{
    return response == MTP_RESP_OPERATION_NOT_SUPPORTED || response == MTP_RESP_INVALID_DATASET || response == MTP_RESP_ACCESS_DENIED ||
           response == MTP_RESP_INVALID_OBJECT_HANDLE || response == MTP_RESP_OBJECT_PROP_NOT_SUPPORTED || response == MTP_RESP_OBJECT_TOO_LARGE;
}

static bool mtp_is_read_operation(uint16_t op_code)
{
    return op_code == MTP_OP_GET_OBJECT || op_code == MTP_OP_GET_PARTIAL_OBJECT || op_code == MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64;
}
#endif

void mtp_trace_request_result(const tud_mtp_cb_data_t *cb_data, int32_t response)
{
#if CONFIG_TINYUSB_MTP_TRACE_WRITES
    const mtp_container_command_t *command = cb_data->command_container;
    uint16_t op_code = command->header.code;
    const mtp_op_handler_entry_t *entry = mtp_find_handler(op_code);
    if (cb_data->phase == MTP_PHASE_DATA && response == 0 && mtp_is_read_operation(op_code)) {
        return;
    }
    if (!(entry && entry->trace) && !mtp_response_should_trace(response)) {
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
             mtp_phase_name(cb_data->phase), entry ? entry->name : "Unknown", op_code, command->header.transaction_id, command->header.len,
             cb_data->total_xferred_bytes, param_count, params, mtp_response_name(response), response_code);
#else
    (void)cb_data;
    (void)response;
#endif
}

static bool mtp_parse_cancel_transaction(const tud_mtp_request_cb_data_t *cb_data, uint32_t *transaction_id)
{
    if (cb_data == NULL || cb_data->buf == NULL || cb_data->bufsize < sizeof(uint16_t) + sizeof(uint32_t)) {
        return false;
    }
    uint16_t event_code;
    memcpy(&event_code, cb_data->buf, sizeof(event_code));
    if (event_code != MTP_EVENT_CANCEL_TRANSACTION) {
        return false;
    }
    memcpy(transaction_id, cb_data->buf + sizeof(event_code), sizeof(*transaction_id));
    return true;
}

static bool mtp_take_cancelled_transaction(uint32_t transaction_id)
{
    bool cancelled = false;
    mtp_lock();
    tinyusb_mtp_ctx_t *ctx = mtp_context_get();
    if (ctx->mux_protected.cancelled_transaction_pending && ctx->mux_protected.cancelled_transaction_id == transaction_id) {
        ctx->mux_protected.cancelled_transaction_pending = false;
        ctx->mux_protected.cancelled_transaction_id = 0;
        cancelled = true;
    }
    mtp_unlock();
    return cancelled;
}

int32_t mtp_dispatch(tud_mtp_cb_data_t *cb_data)
{
    const mtp_container_command_t *command = cb_data->command_container;
    if (cb_data->phase == MTP_PHASE_COMMAND && command->header.code != MTP_OP_SEND_OBJECT) {
        mtp_lock();
        if (mtp_context_get()->mux_protected.pending_write.state == MTP_WRITE_ZERO_SIZE_COMPLETE) {
            mtp_context_get()->mux_protected.pending_write.state = MTP_WRITE_IDLE;
        }
        mtp_unlock();
    }
    const mtp_op_handler_entry_t *entry = mtp_find_handler(command->header.code);
    if (entry) {
        return entry->handler(cb_data);
    }
    ESP_LOGW(TAG, "unsupported MTP operation 0x%04x", command->header.code);
    return MTP_RESP_OPERATION_NOT_SUPPORTED;
}

bool tud_mtp_request_cancel_cb(tud_mtp_request_cb_data_t *cb_data)
{
    if (!mtp_lifecycle_enter()) {
        return true;
    }
    uint32_t transaction_id = 0;
    bool transaction_valid = mtp_parse_cancel_transaction(cb_data, &transaction_id);
    mtp_lock();
#if CONFIG_TINYUSB_MTP_TRACE_WRITES
    mtp_active_read_t *read = &mtp_context_get()->mux_protected.active_read;
#endif
    if (transaction_valid) {
        MTP_TRACEI("MTP cancel: tid=%" PRIu32 " handle=%" PRIu32 " sent=%" PRIu64 "/%" PRIu64, transaction_id,
                   read->active ? read->handle : MTP_OBJECT_HANDLE_INVALID, read->active ? read->sent : 0, read->active ? read->expected : 0);
    } else if (cb_data != NULL) {
        ESP_LOGW(TAG, "invalid MTP cancel request");
    }
    mtp_context_reset_transfers_locked(MTP_RESP_TRANSACTION_CANCELLED);
    if (transaction_valid) {
        mtp_context_get()->mux_protected.cancelled_transaction_pending = true;
        mtp_context_get()->mux_protected.cancelled_transaction_id = transaction_id;
    }
    mtp_unlock();
    mtp_lifecycle_exit();
    return true;
}

bool tud_mtp_request_device_reset_cb(tud_mtp_request_cb_data_t *cb_data)
{
    (void)cb_data;
    if (!mtp_lifecycle_enter()) {
        return true;
    }
    MTP_TRACEI("MTP device reset");
    mtp_lock();
    mtp_context_get()->mux_protected.session_open = false;
    mtp_context_reset_transfers_locked(MTP_RESP_TRANSACTION_CANCELLED);
    mtp_unlock();
    mtp_lifecycle_exit();
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
    bool entered = mtp_lifecycle_enter();
    if (entered) {
        mtp_lock();
        mtp_context_get()->mux_protected.cancelled_transaction_pending = false;
        mtp_context_get()->mux_protected.cancelled_transaction_id = 0;
        mtp_unlock();
    }
    int32_t resp_code = entered ? mtp_dispatch(cb_data) : MTP_RESP_DEVICE_BUSY;
    mtp_trace_request_result(cb_data, resp_code);
    if (resp_code > MTP_RESP_UNDEFINED) {
        cb_data->io_container.header->code = (uint16_t)resp_code;
        tud_mtp_response_send(&cb_data->io_container);
    }
    if (entered) {
        mtp_lifecycle_exit();
    }
    return resp_code;
}

int32_t tud_mtp_data_xfer_cb(tud_mtp_cb_data_t *cb_data)
{
    bool entered = mtp_lifecycle_enter();
    bool cancelled = entered && mtp_take_cancelled_transaction(cb_data->command_container->header.transaction_id);
    int32_t resp_code = entered ? (cancelled ? MTP_RESP_TRANSACTION_CANCELLED : mtp_dispatch(cb_data)) : MTP_RESP_DEVICE_BUSY;
    mtp_trace_request_result(cb_data, resp_code);
    if (resp_code > MTP_RESP_UNDEFINED) {
        uint16_t op_code = cb_data->command_container->header.code;
        if (cb_data->phase == MTP_PHASE_DATA && mtp_should_defer_data_response(op_code) && mtp_data_phase_will_complete(cb_data)) {
            mtp_lock();
            mtp_defer_response_locked(op_code, resp_code);
            mtp_unlock();
            if (entered) {
                mtp_lifecycle_exit();
            }
            return 0;
        }
        cb_data->io_container.header->code = (uint16_t)resp_code;
        tud_mtp_response_send(&cb_data->io_container);
    }
    if (entered) {
        mtp_lifecycle_exit();
    }
    return resp_code < 0 ? resp_code : 0;
}

int32_t tud_mtp_data_complete_cb(tud_mtp_cb_data_t *cb_data)
{
    mtp_container_info_t *response = &cb_data->io_container;
    int32_t resp_code = MTP_RESP_DEVICE_BUSY;

    bool entered = mtp_lifecycle_enter();
    if (entered) {
        if (mtp_take_cancelled_transaction(cb_data->command_container->header.transaction_id)) {
            resp_code = MTP_RESP_TRANSACTION_CANCELLED;
        } else {
            mtp_lock();
            resp_code = mtp_complete_data_locked(cb_data, response);
            mtp_unlock();
        }
    }

    response->header->code = (uint16_t)resp_code;
    mtp_trace_request_result(cb_data, resp_code);
    tud_mtp_response_send(response);
    if (entered) {
        mtp_lifecycle_exit();
    }
    return 0;
}


#endif
