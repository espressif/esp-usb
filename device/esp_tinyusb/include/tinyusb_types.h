/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "soc/soc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_ESPRESSIF_VID 0x303A

typedef enum {
    TINYUSB_USBDEV_0,
} tinyusb_usbdev_t;

/**
 * @brief TinyUSB port number
 *
 * @note The TinyUSB port definition starts from 0, refer to /src/portable/synopsis/dwc2/dwc2_esp32.h
 */
typedef enum {
    TUSB_PORT_FS,       /*!< Full-speed port USB OTG 1.1 peripheral */
    TUSB_PORT_HS        /*!< High-speed port USB OTG 2.0 peripheral */
} tusb_port_t;

/**
 * @brief TinyUSB peripheral number
 *
 * @note The TinyUSB port definition starts from 0 and Full-speed peripheral is default.
 * To use High-speed peripheral by default on the hardware when possible this definition should be used.
 */
typedef enum {
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_PERIPH_HS,
#endif // SOC_USB_OTG_PERIPH_NUM > 1
    TINYUSB_PERIPH_FS,
    TINYUSB_PERIPH_MAX
} tinyusb_periph_t;

/**
 * @brief TinyUSB port speed configuration
 */
typedef enum {
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    TINYUSB_SPEED_HIGH_SPEED,            /*!< High speed port */
#endif // SOC_USB_OTG_PERIPH_NUM > 1
    TINYUSB_SPEED_FULL_SPEED,            /*!< Full speed port */
    TINYUSB_SPEED_MAX
} tinyusb_speed_t;

#ifdef __cplusplus
}
#endif
