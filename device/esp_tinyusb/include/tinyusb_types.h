/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "soc/soc_caps.h"
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_ESPRESSIF_VID 0x303A

/**
 * @brief TinyUSB peripheral port number
 */
typedef enum {
    TINYUSB_PORT_0 = 0,                /*!< USB OTG 1.1 peripheral */
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PORT_1,                    /*!< USB OTG 2.0 peripheral */
#endif // (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PORT_MAX,
} tinyusb_port_t;

/**
 * @brief USB device descriptor configuration
 *
 * @note This structure is used to configure the USB device descriptor. The user can set the parameters
 *       according to their requirements.
 * @note User can set default descriptors values in the Kconfig file.
 */
typedef struct {
    const tusb_desc_device_t *device;           /*!< Pointer to a device descriptor */
    const tusb_desc_device_qualifier_t *qualifier;   /*!< Pointer to a qualifier descriptor. */
    const char **string;                        /*!< Pointer to array of string descriptors.*/
    int string_count;                           /*!< Number of descriptors in above array */
    const uint8_t *full_speed_config;           /*!< Pointer to a Full-speed configuration descriptor. */
    const uint8_t *high_speed_config;           /*!< Pointer to a High-speed configuration descriptor. Not used when device is Full-speed only. */
} tinyusb_desc_config_t;

#ifdef __cplusplus
}
#endif
