/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t mtp_op_begin_edit_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_send_partial_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_truncate_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_end_edit_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_send_object_info(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_send_object(tud_mtp_cb_data_t *cb_data);

#ifdef __cplusplus
}
#endif
