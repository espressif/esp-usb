/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t mtp_op_get_device_info(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_open_close_session(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_storage_ids(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_storage_info(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_num_objects(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_handles(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_info(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_delete_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_move_object(tud_mtp_cb_data_t *cb_data);

#ifdef __cplusplus
}
#endif
