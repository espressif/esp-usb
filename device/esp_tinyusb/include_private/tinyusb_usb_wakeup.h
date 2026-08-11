/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
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
 * @brief Handler invoked after a USB wakeup from light sleep
 *
 * Called from the light-sleep exit callback once UTMI OTG state has been restored.
 * Intended for use by `tinyusb_pm.c` to re-acquire the PM lock while the USB bus
 * remains suspended.
 */
typedef esp_err_t (*tinyusb_usb_wakeup_resume_cb_t)(void);

/**
 * @brief Register or clear the light-sleep USB wakeup resume callback
 *
 * @param[in] cb Callback to invoke on USB wakeup exit, or NULL to clear
 *
 * @return ESP_OK
 */
esp_err_t tinyusb_usb_wakeup_register_resume_cb(tinyusb_usb_wakeup_resume_cb_t cb);

/**
 * @brief Initialize USB Device light-sleep wakeup integration
 *
 * Registers light-sleep event callbacks that prepare and restore UTMI OTG suspend state
 * when entering or exiting light sleep while the USB bus is suspended.
 *
 * @param[in] port USB port used by the driver
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from light-sleep callback registration
 */
esp_err_t tinyusb_usb_wakeup_init(tinyusb_port_t port);

/**
 * @brief Tear down USB Device light-sleep wakeup integration
 *
 * Unregisters light-sleep callbacks and restores UTMI OTG suspend state if needed.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from light-sleep callback unregistration
 */
esp_err_t tinyusb_usb_wakeup_deinit(void);

#ifdef __cplusplus
}
#endif
