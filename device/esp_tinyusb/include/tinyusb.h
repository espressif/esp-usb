/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "soc/soc_caps.h"
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TinyUSB Vendor ID
 *
 * @note This is the Vendor ID which will be used in Default Device Descriptor.
 *
 */
#define TINYUSB_ESPRESSIF_VID                   0x303A

/**
 * @brief TinyUSB peripheral port number
 */
typedef enum {
    TINYUSB_PORT_FULL_SPEED_0 = 0,                /*!< USB OTG 1.1 peripheral */
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PORT_HIGH_SPEED_0,                    /*!< USB OTG 2.0 peripheral */
#endif // (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PORT_MAX,
} tinyusb_port_t;

/**
 * @brief TinyUSB PHY configuration
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
 * @brief TinyUSB Task configuration
 */
typedef struct {
    size_t size;                                /*!< USB Device Task size. */
    uint8_t priority;                           /*!< USB Device Task priority. */
    int  xCoreID;                               /*!< USB Device Task core affinity.  */
} tinyusb_task_config_t;

/**
 * @brief USB device descriptor configuration structure
 *
 * This structure holds pointers to the USB descriptors required to initialize
 * a TinyUSB device stack. These descriptors define how the USB device is identified
 * and communicates with the USB host.
 *
 * This configuration is typically passed during TinyUSB driver installation.
 * If not provided, default descriptor values can be defined and used via Kconfig.
 *
 * @note All pointers must remain valid throughout the lifetime of the TinyUSB stack.
 */
typedef struct {
    const tusb_desc_device_t *device;             /*!< Pointer to the USB device descriptor.
                                                       Defines general device information such as
                                                       USB version, vendor ID, product ID, etc.
                                                       Must not be NULL. */

    const tusb_desc_device_qualifier_t *qualifier; /*!< Pointer to the device qualifier descriptor.
                                                       Required only for High-Speed capable devices.
                                                       Can be NULL for Full-Speed only devices. */

    const char **string;                          /*!< Pointer to an array of USB string descriptors.
                                                       Strings describe elements like manufacturer name,
                                                       product name, and serial number.
                                                       Can be NULL if no strings are used. */

    int string_count;                             /*!< Number of elements in the string descriptor array.
                                                       Should match the actual number of strings provided. */

    const uint8_t *full_speed_config;             /*!< Pointer to the Full-Speed configuration descriptor.
                                                       Must be provided for all devices. */

    const uint8_t *high_speed_config;             /*!< Pointer to the High-Speed configuration descriptor.
                                                       Optional. Required only if the device supports High-Speed mode.
                                                       Can be NULL for Full-Speed only devices. */
} tinyusb_desc_config_t;


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
