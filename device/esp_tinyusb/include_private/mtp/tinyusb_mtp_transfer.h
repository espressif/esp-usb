/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "mtp/tinyusb_mtp_types.h"
#include "tusb.h"

void mtp_clear_pending_write_locked(void);
void mtp_clear_active_read_locked(void);
void mtp_clear_active_edit_locked(void);
void mtp_clear_partial_write_locked(void);
void mtp_clear_active_buffer_locked(void);
void mtp_clear_pending_prop_set_locked(void);
void mtp_clear_deferred_response_locked(void);
void mtp_abort_pending_write_locked(int32_t response);
int32_t mtp_begin_edit_object_locked(uint32_t handle);
int32_t mtp_get_active_edit_object_locked(uint32_t handle, mtp_object_t **object);
int32_t mtp_truncate_object_locked(mtp_object_t *object, uint64_t length);
int32_t mtp_start_buffered_data(tud_mtp_cb_data_t *cb_data, uint16_t op_code, uint8_t *data, uint32_t len);
int32_t mtp_continue_buffered_data(tud_mtp_cb_data_t *cb_data);
bool mtp_should_defer_data_response(uint16_t op_code);
bool mtp_data_phase_will_complete(const tud_mtp_cb_data_t *cb_data);
void mtp_defer_response_locked(uint16_t op_code, int32_t response_code);
int32_t mtp_complete_data_locked(const tud_mtp_cb_data_t *cb_data, mtp_container_info_t *response);
void mtp_transfer_detach_storage_locked(tinyusb_mtp_storage_t *storage);
bool mtp_transfer_is_idle_locked(void);
int32_t mtp_end_edit_object_locked(uint32_t handle);
