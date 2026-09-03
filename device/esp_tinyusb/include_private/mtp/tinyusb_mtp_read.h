/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t mtp_op_get_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_partial_object(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_partial_object64(tud_mtp_cb_data_t *cb_data);

#ifdef __cplusplus
}
#endif
