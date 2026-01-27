/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_typec.h" // for esp_typec_power_role_t
#include "typec_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque device handle for FUSB302.
 */
typedef struct fusb302_dev fusb302_dev_t;

/**
 * @brief Hardware wiring/configuration for FUSB302.
 * @param i2c_dev I2C device handle from esp_driver_i2c
 * @param gpio_int GPIO number for FUSB302 INT (active-low)
 * @param rp_current Advertised Rp current when sourcing
 */
typedef struct {
    i2c_master_dev_handle_t i2c_dev; /**< REQUIRED: device handle from esp_driver_i2c */
    gpio_num_t              gpio_int;/**< REQUIRED: GPIO number for FUSB302 INT (active-low) */
    esp_typec_rp_current_t  rp_current; /**< Advertised Rp current when sourcing */
} fusb302_hw_cfg_t;

/**
 * @brief Initialize and configure the FUSB302 with given hardware config.
 * @param hw Pointer to hardware configuration
 * @param out Pointer to device handle output
 * @return ESP_OK on success
 */
esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out);

/**
 * @brief Put the chip into a safe low-power state and free the handle.
 * @param dev Device handle
 * @return ESP_OK on success
 */
esp_err_t fusb302_deinit(fusb302_dev_t *dev);

/**
 * @brief Change power role (Sink/Source/DRP).
 * @param dev Device handle
 * @param role Power role to set
 * @return ESP_OK on success
 */
esp_err_t fusb302_set_role(fusb302_dev_t *dev, esp_typec_power_role_t role);

/**
 * @brief Commit orientation/role after attach (stop toggle, apply polarity, advertise Rp).
 * @param dev Device handle
 * @param cc2_active True if CC2 is the active/connected CC
 * @param is_source True when acting as source, false when sink
 * @return ESP_OK on success
 */
esp_err_t fusb302_commit_attach(fusb302_dev_t *dev, bool cc2_active, bool is_source);

/**
 * @brief Enable or disable FUSB302 interrupt sources (MASK regs).
 * @param dev Device handle
 * @param enable True to enable, false to disable
 * @return ESP_OK on success
 */
esp_err_t fusb302_enable_irq(fusb302_dev_t *dev, bool enable);

/**
 * @brief Read/clear interrupt cause registers (read-to-clear). Any pointer may be NULL.
 * @param dev Device handle
 * @param int0 Pointer to INT0 value
 * @param inta Pointer to INTA value
 * @param intb Pointer to INTB value
 * @return ESP_OK on success
 */
esp_err_t fusb302_get_and_clear_int(fusb302_dev_t *dev,
                                    uint8_t *int0, uint8_t *inta, uint8_t *intb);

/**
 * @brief Service pending interrupts and translate them into Type-C events.
 * @param dev Device handle
 * @param events Pointer to event mask output
 * @return ESP_OK on success
 */
esp_err_t fusb302_service_irq(fusb302_dev_t *dev,
                              typec_evt_mask_t *events);

/**
 * @brief Read current CC/VBUS status snapshot from the device.
 * @param dev Device handle
 * @param st Pointer to status output
 * @return ESP_OK on success
 */
esp_err_t fusb302_get_status(fusb302_dev_t *dev, typec_cc_status_t *st);

#ifdef __cplusplus
}
#endif
