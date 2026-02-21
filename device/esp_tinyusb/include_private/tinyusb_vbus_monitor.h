/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int         gpio_num;          /*!< GPIO number used for VBUS monitoring, 3.3 V tolerant */
    int         port;              /*!< USB port number */
} tinyusb_vbus_monitor_config_t;

/**
 * @brief Initialize VBUS monitoring on the specified GPIO
 *
 * @param[in] config VBUS monitoring configuration
 *
 * @return
 *    - ESP_ERR_INVALID_ARG if config is NULL
 *    - ESP_ERR_INVALID_STATE if VBUS monitoring is already initialized
 *    - ESP_OK if VBUS monitoring was initialized successfully
 */
esp_err_t tinyusb_vbus_monitor_init(const tinyusb_vbus_monitor_config_t *config);

/**
 * @brief De-initialize VBUS monitoring
 */
void tinyusb_vbus_monitor_deinit(void);

#ifdef __cplusplus
}
#endif
