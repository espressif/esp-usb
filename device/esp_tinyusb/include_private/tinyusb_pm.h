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
 * @brief TinyUSB power management configuration.
 */
typedef struct {
    bool lock_enable;                        /*!< Enable ESP_PM_NO_LIGHT_SLEEP lock management. */
} tinyusb_pm_config_t;

/**
 * @brief Initialize TinyUSB power management according to driver configuration
 *
 * When the pm lock is enabled, this function creates an `ESP_PM_NO_LIGHT_SLEEP` lock
 * and acquires it.
 *
 * @note without acquiring the lock immediately after creating it, the PM module would automatically enter
 * the light sleep (if no other locks are used in the app) and the USB Device would not even be mounted to the USB host
 *
 * @param[in] config PM configuration
 *
 * @return
 *      - ESP_OK on success, or PM Lock is not enabled by the user
 *      - ESP_ERR_INVALID_ARG if the argument is not valid
 *      - ESP_ERR_INVALID_STATE if the PM module is already initialized
 *      - Other error codes from called functions
 */
esp_err_t tinyusb_pm_init(const tinyusb_pm_config_t *config);

/**
 * @brief Tear down TinyUSB power management
 *
 * Releases the PM lock when it was enabled and deletes the lock object.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from called functions
 */
esp_err_t tinyusb_pm_deinit(void);

/**
 * @brief Update PM lock state based on a TinyUSB device event
 *
 * On supported events it acquires or releases the PM lock when the USB bus is attached,
 * detached, suspended, or resumed.
 *
 * @param[in] event TinyUSB device event
 */
void tinyusb_pm_on_event(tinyusb_event_t *event);

/**
 * @brief Update PM lock state based on user's remote wakeup call
 *
 * In case the TINYUSB_USB_OTG_WAKEUP is not enabled, or not supported the SoC can be woken from light sleep using
 * different source than the USB peripheral (timer, gpio..) the user can call remote wakeup which effectively resumes
 * the USB peripheral. The remote wakeup takes some time to finish, thus we need to acquire the PM lock in the meanwhile
 * to restrict the SoC from entering the light sleep automatically while waiting for the remote wakeup to complete.
 *
 * @return
 *   - ESP_OK on success, or when the PM lock is not enabled (no-op)
 *   - Other error codes from called functions
 */
esp_err_t tinyusb_pm_remote_wake(void);

/**
 * @brief Get TinyUSB PM lock acquisition state
 *
 * @param[out] lock_taken PM lock acquisition state
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the argument is not valid
 *      - ESP_ERR_INVALID_STATE if the PM lock is not enabled
 */
esp_err_t tinyusb_pm_lock_get(bool *lock_taken);

#if !CONFIG_TINYUSB_SUSPEND_CALLBACK
/**
 * @brief Release the PM lock on USB suspend
 *
 * Used internally when `CONFIG_TINYUSB_SUSPEND_CALLBACK` is registered outside of esp_tinyusb. In that case the
 * application must call `tinyusb_pm_notify_suspended()` from the user-defined suspend callback after the host suspends the device.
 *
 * @return
 *      - ESP_OK on success or when already suspended with the lock released
 *      - ESP_ERR_NOT_ALLOWED if the PM lock is not enabled
 *      - Other error codes from called functions
 */
esp_err_t tinyusb_pm_on_suspend(tinyusb_port_t port);
#endif // !CONFIG_TINYUSB_SUSPEND_CALLBACK

#if !CONFIG_TINYUSB_RESUME_CALLBACK
/**
 * @brief Acquire the PM lock on USB resume
 *
 * Used internally when `CONFIG_TINYUSB_RESUME_CALLBACK` is registered outside of esp_tinyusb. In that case the
 * application must call `tinyusb_pm_notify_resumed()` from the user-defined resume callback after the host resumes the device.
 *
 * @return
 *      - ESP_OK on success or when the device is not suspended
 *      - ESP_ERR_NOT_ALLOWED if the PM lock is not enabled
 *      - Other error codes from called functions
 */
esp_err_t tinyusb_pm_on_resume(tinyusb_port_t port);
#endif // !CONFIG_TINYUSB_RESUME_CALLBACK

#ifdef __cplusplus
}
#endif
