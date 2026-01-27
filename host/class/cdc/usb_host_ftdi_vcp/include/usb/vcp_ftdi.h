/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "usb/cdc_host_types.h"

// legacy deprecated definitions
#define FTDI_VID             (0x0403)
#define FT232_PID            (0x6001)
#define FT231_PID            (0x6015)
#define FTDI_PID_AUTO        (0)

// New definitions with namespace prefix VCP_FTDI_
#define VCP_FTDI_VID       FTDI_VID
#define VCP_FTDI_PID_FT232 FT232_PID
#define VCP_FTDI_PID_FT231 FT231_PID
#define VCP_FTDI_PID_AUTO  FTDI_PID_AUTO

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open FTDI device
 *
 * @param[in]  pid           PID of the device
 * @param[in]  interface_idx Interface number
 * @param[in]  dev_config    CDC device configuration
 * @param[out] cdc_hdl_ret   Pointer to the CDC handle
 * @return
 *    - ESP_OK: Success
 *    - ESP_ERR_NOT_FOUND: Device not found
 *    - ESP_ERR_NO_MEM: No memory
 */
esp_err_t ftdi_vcp_open(uint16_t pid, uint8_t interface_idx, const cdc_acm_host_device_config_t *dev_config, cdc_acm_dev_hdl_t *cdc_hdl_ret);

#ifdef __cplusplus
}
#endif
