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

#ifdef __cplusplus
}
#endif
