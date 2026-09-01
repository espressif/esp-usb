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
 * @brief Acquire the PM lock after exiting light sleep while USB remains suspended
 *
 * Called from the light-sleep exit callback once UTMI OTG state has been restored.
 * Intended for use by `tinyusb_pm.c` to re-acquire `ESP_PM_NO_LIGHT_SLEEP` immediately
 * after waking from light sleep on USB activity.
 *
 * @return
 *      - ESP_OK on success, when the PM lock is not enabled, or when the device is not in USB suspend PM state
 *      - Other error codes from PM lock acquire
 */
typedef esp_err_t (*tinyusb_usb_wakeup_pm_lock_acquire_cb_t)(void);

/**
 * @brief Query USB suspend PM state before entering light sleep
 *
 * Called from the light-sleep enter callback to decide whether UTMI OTG suspend
 * preparation is required. Intended for use by `tinyusb_pm.c` to read the PM
 * module's synchronized suspend state when PM locks are enabled.
 *
 * @return True when the device is in USB suspend PM state
 */
typedef bool (*tinyusb_usb_wakeup_pm_state_cb_t)(void);

/**
 * @brief Optional PM integration callbacks for USB light-sleep wakeup
 *
 * Register both handlers together via `tinyusb_usb_wakeup_register_pm_cbs()`.
 * Pass NULL to `tinyusb_usb_wakeup_register_pm_cbs()` to clear all handlers.
 */
typedef struct {
    tinyusb_usb_wakeup_pm_state_cb_t pm_state;                /*!< Query USB suspend PM state on sleep enter, or NULL to use `tud_suspended()` */
    tinyusb_usb_wakeup_pm_lock_acquire_cb_t acquire_pm_lock;  /*!< Acquire PM lock on sleep exit, or NULL to skip */
} tinyusb_usb_wakeup_pm_cbs_t;

/**
 * @brief Register or clear PM integration callbacks for USB light-sleep wakeup
 *
 * @param[in] cbs Callback group to register, or NULL to clear all callbacks
 */
void tinyusb_usb_wakeup_register_pm_cbs(const tinyusb_usb_wakeup_pm_cbs_t *cbs);

/**
 * @brief Initialize USB Device light-sleep wakeup integration
 *
 * Registers light-sleep event callbacks that prepare and restore UTMI OTG suspend state
 * when entering or exiting light sleep while the USB bus is suspended.
 * Also enables USB as a light-sleep wakeup source via `esp_sleep_enable_usb_wakeup()`.
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
 * Also disables the USB light-sleep wakeup source via `esp_sleep_disable_usb_wakeup()`.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from light-sleep callback unregistration
 */
esp_err_t tinyusb_usb_wakeup_deinit(void);

#ifdef __cplusplus
}
#endif
