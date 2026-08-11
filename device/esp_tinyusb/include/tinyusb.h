/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "soc/soc_caps.h"
#include "tusb.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TinyUSB vendor ID used by the default device descriptor.
 */
#define TINYUSB_ESPRESSIF_VID                   0x303A

/**
 * @brief TinyUSB peripheral port identifier.
 */
typedef enum {
    TINYUSB_PORT_FULL_SPEED_0 = 0,             /*!< USB OTG 1.1 peripheral port. */
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PORT_HIGH_SPEED_0,                 /*!< USB OTG 2.0 peripheral port. */
#endif // (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PORT_MAX,                          /*!< Number of supported peripheral ports. */
} tinyusb_port_t;

/**
 * @brief TinyUSB PHY and VBUS monitoring configuration.
 *
 * @note On the ESP32-P4 high-speed port, install the GPIO ISR service with
 *       `gpio_install_isr_service()` before calling tinyusb_driver_install()
 *       when VBUS monitoring is enabled.
 */
typedef struct {
    bool skip_setup;                    /*!< Skip automatic USB PHY setup.
                                             Set this when the application configures the PHY manually,
                                             for example when using an external PHY. */
    bool self_powered;                  /*!< Enable VBUS monitoring for a self-powered device. */
    int vbus_monitor_io;                /*!< GPIO used to monitor VBUS.
                                             Ignored when `self_powered` is `false`. */
} tinyusb_phy_config_t;

/**
 * @brief TinyUSB task configuration.
 */
typedef struct {
    size_t size;                             /*!< USB device task stack size in bytes. */
    uint8_t priority;                        /*!< USB device task priority. */
    int xCoreID;                             /*!< USB device task core affinity. */
} tinyusb_task_config_t;

/**
 * @brief USB device descriptor configuration.
 *
 * Any descriptor pointer may be set to `NULL` to use the default descriptor
 * supplied by esp_tinyusb when one is available for the selected class and
 * speed.
 *
 * @note All non-NULL pointers must remain valid for the lifetime of the
 *       TinyUSB device stack.
 */
typedef struct {
    const tusb_desc_device_t *device;             /*!< Device descriptor. */
    const tusb_desc_device_qualifier_t *qualifier; /*!< Device qualifier descriptor for high-speed-capable devices. */
    const char **string;                          /*!< Array of string descriptor pointers. */
    int string_count;                             /*!< Number of entries in `string`. */
    const uint8_t *full_speed_config;             /*!< Full-speed configuration descriptor. */
    const uint8_t *high_speed_config;             /*!< High-speed configuration descriptor. */
} tinyusb_desc_config_t;

/**
 * @brief TinyUSB device event identifier.
 */
typedef enum {
    TINYUSB_EVENT_ATTACHED = 0,             /*!< The USB device has been attached to the host. */
    TINYUSB_EVENT_DETACHED = 1,             /*!< The USB device has been detached from the host. */
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    TINYUSB_EVENT_SUSPENDED = 2,            /*!< The USB device has entered suspend. */
#endif // CONFIG_TINYUSB_SUSPEND_CALLBACK
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    TINYUSB_EVENT_RESUMED = 3,              /*!< The USB device has resumed from suspend. */
#endif // CONFIG_TINYUSB_RESUME_CALLBACK
} tinyusb_event_id_t;

/**
 * @brief TinyUSB device event data passed to tinyusb_event_cb_t.
 */
typedef struct {
    tinyusb_event_id_t id;            /*!< Event identifier. */
    uint8_t rhport;                   /*!< USB peripheral port number. */
    union {
        struct {
            bool remote_wakeup;       /*!< `true` when the host enabled remote wakeup for the suspended device. */
        } suspended;                  /*!< Data for TINYUSB_EVENT_SUSPENDED. */
    };
} tinyusb_event_t;

/**
 * @brief Callback invoked on TinyUSB device events.
 *
 * @param[in] event Pointer to the event data.
 * @param[in] arg User argument from tinyusb_config_t.event_arg.
 */
typedef void (*tinyusb_event_cb_t)(tinyusb_event_t *event, void *arg);

/**
 * @brief TinyUSB driver configuration.
 */
typedef struct {
    tinyusb_port_t port;                     /*!< USB peripheral port to use. */
    tinyusb_phy_config_t phy;                /*!< USB PHY configuration. */
    tinyusb_task_config_t task;              /*!< USB device task configuration. */
    tinyusb_desc_config_t descriptor;        /*!< USB descriptor configuration. */
    bool pm_lock_enable;                     /*!< Enable ESP_PM_NO_LIGHT_SLEEP lock management while the USB
                                                 bus is active. When enabled, the lock is released on USB suspend
                                                 to allow automatic light sleep. On supported high-speed targets
                                                 this also manages UTMI OTG suspend state for USB wakeup. */
    tinyusb_event_cb_t event_cb;             /*!< Optional event callback. */
    void *event_arg;                         /*!< User argument passed to `event_cb`. */
} tinyusb_config_t;

/**
 * @brief Install the TinyUSB device driver and start the TinyUSB task.
 *
 * This helper configures the USB PHY when requested, prepares descriptors,
 * initializes the TinyUSB stack, and starts the TinyUSB task.
 *
 * @note When supplying a custom composite device descriptor with an Interface
 *       Association Descriptor, keep `bDeviceClass` as `TUSB_CLASS_MISC` and
 *       `bDeviceSubClass` as `MISC_SUBCLASS_COMMON`.
 *
 * @param[in] config TinyUSB stack configuration.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `config` is NULL or contains unsupported port or task settings
 *      - ESP_ERR_INVALID_STATE if the TinyUSB device task is already running
 *      - ESP_ERR_NO_MEM if memory allocation fails during startup
 *      - Other error codes from TinyUSB task startup, USB PHY setup, or descriptor setup
 */
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);

/**
 * @brief Uninstall the TinyUSB device driver.
 *
 * This stops the TinyUSB task, tears down the TinyUSB stack, frees prepared
 * descriptors, and deletes the USB PHY when it was created by
 * tinyusb_driver_install().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the TinyUSB driver is not installed
 *      - Other error codes from TinyUSB task shutdown or USB PHY teardown
 */
esp_err_t tinyusb_driver_uninstall(void);

/**
 * @brief Send a remote wakeup signal to the USB host.
 *
 * @note Call this function only while the device is suspended and the host has
 *       enabled remote wakeup.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the TinyUSB driver is not installed, or remote wakeup is not enabled by the host
 *      - ESP_FAIL if the remote wakeup request cannot be sent
 */
esp_err_t tinyusb_remote_wakeup(void);

#if CONFIG_TINYUSB_PM
/**
 * @brief Get TinyUSB PM lock acquisition state.
 *
 * Returns whether the `ESP_PM_NO_LIGHT_SLEEP` lock managed by esp_tinyusb is currently held.
 *
 * @param[out] acquired Whether the PM lock is currently held
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `acquired` is NULL
 *      - ESP_ERR_INVALID_STATE if the TinyUSB driver is not installed or the PM lock is not enabled
 */
esp_err_t tinyusb_pm_get_lock_status(bool *acquired);

#if !CONFIG_TINYUSB_SUSPEND_CALLBACK
/**
 * @brief Notify esp_tinyusb that the USB device was suspended.
 *
 * Used when `CONFIG_TINYUSB_SUSPEND_CALLBACK` is registered outside of esp_tinyusb, eg registered by a user and
 * the tinyusb power management feature is desired to be used.
 * In that case the application must explicitly notify the esp_tinyusb about the suspend event
 *
 * Call this from a user-defined `tud_suspend_cb()` when
 * `CONFIG_TINYUSB_SUSPEND_CALLBACK` is disabled and
 * `tinyusb_config_t.pm_lock_enable` is set.
 *
 * @code{c}
 * void tud_suspend_cb(bool remote_wakeup_en) {
 *     tinyusb_pm_notify_suspended();
 *     ...
 * }
 * @endcode
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the TinyUSB driver is not installed
 *      - ESP_ERR_NOT_ALLOWED if PM lock is not enabled or the USB device is not suspended
 */
esp_err_t tinyusb_pm_notify_suspended(void);
#endif // !CONFIG_TINYUSB_SUSPEND_CALLBACK

#if !CONFIG_TINYUSB_RESUME_CALLBACK
/**
 * @brief Notify esp_tinyusb that the USB device was resumed.
 *
 * Used when `CONFIG_TINYUSB_RESUME_CALLBACK` is registered outside of esp_tinyusb, eg registered by a user and
 * the tinyusb power management feature is desired to be used.
 * In that case the application must explicitly notify the esp_tinyusb about the resume event
 *
 * Call this from a user-defined `tud_resume_cb()` when
 * `CONFIG_TINYUSB_RESUME_CALLBACK` is disabled and
 * `tinyusb_config_t.pm_lock_enable` is set.
 *
 * @code{c}
 * void tud_resume_cb(void) {
 *     tinyusb_pm_notify_resumed();
 *     ...
 * }
 * @endcode
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the TinyUSB driver is not installed
 *      - ESP_ERR_NOT_ALLOWED if PM lock is not enabled, the USB device is still suspended,
 *        or the USB device is not mounted
 */
esp_err_t tinyusb_pm_notify_resumed(void);
#endif // !CONFIG_TINYUSB_RESUME_CALLBACK

#endif // CONFIG_TINYUSB_PM

#ifdef __cplusplus
}
#endif
