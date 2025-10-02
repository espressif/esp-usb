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

/**
 * @brief Initialize VBUS monitoring on the specified GPIO
 *
 * Note: This function should be called after tusb_init() when GOTGCTL register is initialized.
 *
 * @param vbus_io_num GPIO number used for VBUS monitoring, 3.3 V tolerant (use a comparator or a resistor divider to detect the VBUS valid condition).
 *
 * @return
 *   - ESP_OK on success
 *   - ESP_ERR_INVALID_STATE if already initialized
 */
esp_err_t tinyusb_vbus_monitor_init(int vbus_io_num);

/**
 * @brief Deinitialize VBUS monitoring
 *
 * @param vbus_io_num GPIO number used for VBUS monitoring
 */
void tinyusb_vbus_monitor_deinit(int vbus_io_num);

#ifdef __cplusplus
}
#endif
