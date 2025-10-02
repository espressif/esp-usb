/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "tinyusb.h"

#ifdef __cplusplus
extern "C" {
#endif


void tinyusb_vbus_monitor_init(int vbus_io);

void tinyusb_vbus_monitor_deinit(void);

#ifdef __cplusplus
}
#endif
