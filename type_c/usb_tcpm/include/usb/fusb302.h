/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "usb/usb_tcpm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FUSB302 7-bit I2C slave addresses.
 *
 * Use these values for @ref usb_tcpm_fusb302_config_t.i2c_addr_7b.
 * Suffix `XYZ` corresponds to address bits `[2:0]`.
 */
#define USB_TCPM_FUSB302_I2C_ADDR_7B_010 (0x22) /**< 8-bit bus address: 0x44 (W) / 0x45 (R). */
#define USB_TCPM_FUSB302_I2C_ADDR_7B_011 (0x23) /**< 8-bit bus address: 0x46 (W) / 0x47 (R). */
#define USB_TCPM_FUSB302_I2C_ADDR_7B_100 (0x24) /**< 8-bit bus address: 0x48 (W) / 0x49 (R). */
#define USB_TCPM_FUSB302_I2C_ADDR_7B_101 (0x25) /**< 8-bit bus address: 0x4A (W) / 0x4B (R). */

/**
 * @brief Hardware configuration for FUSB302 backend.
 */
typedef struct {
    i2c_master_bus_handle_t  i2c_bus;     /**< I2C bus handle */
    uint8_t                  i2c_addr_7b; /**< 7-bit I2C address of FUSB302 */
    gpio_num_t               gpio_int;    /**< Active-low INT GPIO from the chip */
} usb_tcpm_fusb302_config_t;

/**
 * @brief Create a Type-C port backed by FUSB302 (I2C + INT).
 *
 * @note Events are posted on the USB_TCPM_EVENT base.
 *
 * @param[in] port_cfg Port configuration.
 * @param[in] hw_cfg Hardware configuration for FUSB302.
 * @param[out] port_hdl_ret Pointer to port handle output.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is NULL or invalid
 *      - ESP_ERR_INVALID_STATE if usb_tcpm_install() has not been called
 *      - ESP_ERR_NO_MEM if allocation fails
 *      - Other error codes from GPIO/I2C/backend initialization
 */
esp_err_t usb_tcpm_port_create_fusb302(const usb_tcpm_port_config_t *port_cfg,
                                       const usb_tcpm_fusb302_config_t *hw_cfg,
                                       usb_tcpm_port_handle_t *port_hdl_ret);

#ifdef __cplusplus
}
#endif
