/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "tinyusb.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t  gpio_num;          /*!< GPIO number used for VBUS monitoring, 3.3 V tolerant */
    uint32_t    debounce_delay_ms; /*!< Debounce delay in milliseconds */
} tinyusb_vbus_monitor_config_t;

/**
 * @brief Initialize VBUS monitoring on the specified GPIO
 *
 * Note:
 * - This function should be called after tusb_init() when GOTGCTL register is initialized
 * - This is a single-threaded implementation, so only one instance of VBUS monitoring is supported
 *
 * @param config VBUS monitoring configuration
 *
 * @return
 *    - ESP_ERR_INVALID_ARG if config is NULL
 *    - ESP_OK if VBUS monitoring was initialized successfully
 */
esp_err_t tinyusb_vbus_monitor_init(tinyusb_vbus_monitor_config_t *config);

/**
 * @brief Deinitialize VBUS monitoring
 */
void tinyusb_vbus_monitor_deinit(void);

#ifdef __cplusplus
}
#endif
