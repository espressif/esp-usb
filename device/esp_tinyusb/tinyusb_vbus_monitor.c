/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "tinyusb_vbus_monitor.h"

const static char *TAG = "VBUS mon";

/**
 * @brief VBUS monitoring context
 *
 * Note: This is a single-threaded implementation, so only one instance of VBUS monitoring is supported
 */
typedef struct {
    gpio_num_t gpio_num;            /*!< GPIO number used for VBUS monitoring */
    bool prev_state;                /*!< Previous VBUS IO state: true - HIGH, false - LOW */
    TimerHandle_t debounce_timer;   /*!< Debounce timer handle */
} vbus_monitor_context_t;

// -------------- Public API ------------------

esp_err_t tinyusb_vbus_monitor_init(tinyusb_vbus_monitor_config_t *config)
{
    ESP_LOGD(TAG, "Init GPIO%d, debounce delay: %"PRIu32" ms", config->gpio_num, config->debounce_delay_ms);
    return ESP_ERR_NOT_SUPPORTED;
}

void tinyusb_vbus_monitor_deinit(void)
{
    ESP_LOGD(TAG, "Deinit");
}
