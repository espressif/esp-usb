/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t mtp_dispatch(tud_mtp_cb_data_t *cb_data);
void mtp_trace_request_result(const tud_mtp_cb_data_t *cb_data, int32_t response);

#ifdef __cplusplus
}
#endif
