/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "usb/cdc_host_types.h"

/**
 * @brief USB vendor ID used by supported CP210x devices.
 */
#define SILICON_LABS_VID (0x10C4)

/**
 * @brief USB product ID for single-port CP210x devices such as CP2101 to CP2104.
 */
#define CP210X_PID       (0xEA60) // Single i.e. CP2101 - CP2104

/**
 * @brief USB product ID for CP2105 dual-port devices.
 */
#define CP2105_PID       (0xEA70) // Dual

/**
 * @brief USB product ID for CP2108 quad-port devices.
 */
#define CP2108_PID       (0xEA71) // Quad

/**
 * @brief Probe all supported CP210x product IDs.
 */
#define CP210X_PID_AUTO  (0)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a CP210x virtual COM port device.
 *
 * Pass `CP210X_PID_AUTO` to probe all supported product IDs for this driver.
 *
 * @param[in] pid Product ID to open, or `CP210X_PID_AUTO` to auto-detect a
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
 *      - Other error codes from cdc_acm_host_open() or CP210x initialization
 */
esp_err_t cp210x_vcp_open(uint16_t pid, uint8_t interface_idx,
                          const cdc_acm_host_device_config_t *dev_config,
                          cdc_acm_dev_hdl_t *cdc_hdl_ret);

#ifdef __cplusplus
}
#endif
