/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "usb/cdc_host_types.h"

/**
 * @brief USB vendor ID used by supported CH34x devices.
 */
#define NANJING_QINHENG_MICROE_VID (0x1A86)

/**
 * @brief USB product ID for CH340 devices.
 */
#define CH340_PID                  (0x7522)

/**
 * @brief Alternate USB product ID for CH340 devices.
 */
#define CH340_PID_1                (0x7523)

/**
 * @brief USB product ID for CH341 devices.
 */
#define CH341_PID                  (0x5523)

/**
 * @brief Probe all supported CH34x product IDs.
 */
#define CH34X_PID_AUTO             (0)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a CH34x virtual COM port device.
 *
 * Pass `CH34X_PID_AUTO` to probe all supported product IDs for this driver.
 *
 * @param[in] pid Product ID to open, or `CH34X_PID_AUTO` to auto-detect a
 *                supported PID.
 * @param[in] interface_idx Interface index to open.
 * @param[in] dev_config CDC device configuration.
 * @param[out] cdc_hdl_ret Returned CDC handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `dev_config` or `cdc_hdl_ret` is NULL
 *      - ESP_ERR_INVALID_STATE if the CDC-ACM host driver is not installed
 *      - ESP_ERR_NO_MEM if memory allocation fails while opening the device
 *      - ESP_ERR_NOT_FOUND if no supported device is found
 *      - Other error codes from cdc_acm_host_open()
 */
esp_err_t ch34x_vcp_open(uint16_t pid, uint8_t interface_idx,
                         const cdc_acm_host_device_config_t *dev_config,
                         cdc_acm_dev_hdl_t *cdc_hdl_ret);

#ifdef __cplusplus
}
#endif
