/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "tusb.h"
#include "tinyusb_types.h"
#include "tinyusb_task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB PHY configuration
 *
 * @note This structure is used to configure the USB PHY. The user can set the parameters
 *       according to their requirements.
 */
typedef struct {
    bool skip_setup;                       /*!< If set, the esp_tinyusb will not configure the USB PHY thus allowing
                                                the user to manually configure the USB PHY before calling tinyusb_driver_install().
                                                Users should set this if they want to use an external USB PHY. Otherwise,
                                                the esp_tinyusb will automatically configure the internal USB PHY */
    // Relevant only when skip_setup is false
    bool self_powered;                        /*!< USB specification mandates self-powered devices to monitor USB VBUS to detect connection/disconnection events.
                                                To use this feature, connect VBUS to any free GPIO through a voltage divider or voltage comparator.
                                                The voltage divider output should be (0.75 * Vdd) if VBUS is 4.4V (lowest valid voltage at device port).
                                                The comparator thresholds should be set with hysteresis: 4.35V (falling edge) and 4.75V (raising edge). */
    int vbus_monitor_io;                      /*!< GPIO for VBUS monitoring. Ignored if not self_powered. */
} tinyusb_phy_config_t;

/**
 * @brief Configuration structure of the TinyUSB driver
 */
typedef struct {
    tinyusb_port_t port;                        /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    tinyusb_phy_config_t phy;                   /*!< USB PHY configuration. */
    tinyusb_task_config_t task;                 /*!< USB Device Task configuration. */
    tinyusb_desc_config_t descriptor;           /*!< Pointer to a descriptor configuration. If set to NULL, the TinyUSB device will use a default descriptor configuration whose values are set in Kconfig */
} tinyusb_config_t;

/**
 * @brief This is an all-in-one helper function, including:
 * 1. USB device driver initialization
 * 2. Descriptors preparation
 * 3. TinyUSB stack initialization
 * 4. Creates and start a task to handle usb events
 *
 * @note Don't change Custom descriptor, but if it has to be done,
 *       Suggest to define as follows in order to match the Interface Association Descriptor (IAD):
 *       bDeviceClass = TUSB_CLASS_MISC,
 *       bDeviceSubClass = MISC_SUBCLASS_COMMON,
 *
 * @param config tinyusb stack specific configuration
 * @retval ESP_ERR_INVALID_ARG Install driver and tinyusb stack failed because of invalid argument
 * @retval ESP_FAIL Install driver and tinyusb stack failed because of internal error
 * @retval ESP_OK Install driver and tinyusb stack successfully
 */
esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);

/**
 * @brief This is an all-in-one helper function, including:
 * 1. Stops the task to handle usb events
 * 2. TinyUSB stack tearing down
 * 2. Freeing resources after descriptors preparation
 * 3. Deletes USB PHY
 *
 * @retval ESP_FAIL Uninstall driver or tinyusb stack failed because of internal error
 * @retval ESP_OK Uninstall driver, tinyusb stack and USB PHY successfully
 */
esp_err_t tinyusb_driver_uninstall(void);

#ifdef __cplusplus
}
#endif
