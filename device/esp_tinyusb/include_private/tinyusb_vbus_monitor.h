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

/**
 * @brief Callback used to indicate that the VBUS state has changed
 */
typedef void (*vbus_mon_state_changed_t)(bool vbus_present);

typedef struct {
    gpio_num_t  gpio_num;                       /*!< GPIO number used for VBUS monitoring, 3.3 V tolerant */
    uint32_t    debounce_delay_ms;              /*!< Debounce delay in milliseconds */
    vbus_mon_state_changed_t state_changed_cb;  /*!< Callback function called when VBUS state changes */
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
 *    - ESP_ERR_INVALID_ARG if config is NULL, VBUS monitor callback is not set or
 *    - ESP_ERR_INVALID_STATE if VBUS monitoring is already initialized
 *    - ESP_ERR_NOT_SUPPORTED if Timer Task Stack Depth is insufficient when MSC is enabled
 *    - ESP_ERR_NO_MEM if debounce timer creation failed
 *    - ESP_OK if VBUS monitoring was initialized successfully
 */
esp_err_t tinyusb_vbus_monitor_init(tinyusb_vbus_monitor_config_t *config);

/**
 * @brief Deinitialize VBUS monitoring
 *
 * @return
 *    - ESP_ERR_INVALID_STATE if VBUS monitoring is not initialized
 *    - ESP_OK on success
 */
esp_err_t tinyusb_vbus_monitor_deinit(void);

#ifdef __cplusplus
}
#endif
