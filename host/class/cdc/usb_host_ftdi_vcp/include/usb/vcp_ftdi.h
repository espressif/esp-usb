/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "usb/cdc_host_types.h"

/**
 * @brief Legacy FTDI vendor ID alias kept for backward compatibility.
 */
#define FTDI_VID             (0x0403)

/**
 * @brief Legacy FT232 product ID alias kept for backward compatibility.
 */
#define FT232_PID            (0x6001)

/**
 * @brief Legacy FT231 product ID alias kept for backward compatibility.
 */
#define FT231_PID            (0x6015)

/**
 * @brief Legacy auto-detect selector kept for backward compatibility.
 */
#define FTDI_PID_AUTO        (0)

/**
 * @brief USB vendor ID used by supported FTDI devices.
 */
#define VCP_FTDI_VID       FTDI_VID

/**
 * @brief USB product ID for FT232 devices.
 */
#define VCP_FTDI_PID_FT232 FT232_PID

/**
 * @brief USB product ID for FT231 devices.
 */
#define VCP_FTDI_PID_FT231 FT231_PID

/**
 * @brief Probe all supported FTDI product IDs.
 */
#define VCP_FTDI_PID_AUTO  FTDI_PID_AUTO

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open an FTDI virtual COM port device.
 *
 * Pass `FTDI_PID_AUTO` or `VCP_FTDI_PID_AUTO` to probe all supported product
 * IDs for this driver.
 *
 * @param[in] pid Product ID to open, or an auto-detect selector.
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
 *      - Other error codes from cdc_acm_host_open() or FTDI initialization
 */
esp_err_t ftdi_vcp_open(uint16_t pid, uint8_t interface_idx,
                        const cdc_acm_host_device_config_t *dev_config,
                        cdc_acm_dev_hdl_t *cdc_hdl_ret);

#ifdef __cplusplus
}
#endif
