/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CC status snapshot for HUSB320.
 */
typedef struct {
    bool attached;        /**< Rd/Ra detected */
    bool cc2_active;      /**< true if CC2 is active line */
    uint32_t rp_cur_ma;   /**< advertised current (0 if unknown) */
} husb320_cc_status_t;


/**
 * @brief Hardware configuration for HUSB320.
 */
typedef struct {
    int i2c_port;      /**< I2C controller port */
    uint8_t i2c_addr;  /**< 7-bit I2C address */
    int gpio_int;      /**< INT/ATTACH GPIO */
    bool use_intr;     /**< If false, backend may poll (debug) */
} husb320_hw_cfg_t;


/**
 * @brief Opaque device handle for HUSB320.
 */
typedef struct husb320_dev husb320_dev_t;


/**
 * @brief Power role for HUSB320.
 */
typedef enum {
    HUSB320_ROLE_SINK = 0,    /**< Sink role */
    HUSB320_ROLE_SOURCE,      /**< Source role */
    HUSB320_ROLE_DRP,         /**< Dual-role power */
} husb320_role_t;

esp_err_t husb320_set_role(husb320_dev_t *dev, husb320_role_t role);

/**
 * @brief Initialize HUSB320 device.
 *
 * @param hw Hardware configuration
 * @param out Pointer to device handle
 * @return ESP_OK on success
 */
esp_err_t husb320_init(const husb320_hw_cfg_t *hw, husb320_dev_t **out);

/**
 * @brief Deinitialize HUSB320 device.
 *
 * @param dev Device handle
 */
void      husb320_deinit(husb320_dev_t *dev);

/**
 * @brief Set power role (Rp/Rd) for HUSB320.
 *
 * @param dev Device handle
 * @param role Power role
 * @return ESP_OK on success
 */
esp_err_t husb320_set_role(husb320_dev_t *dev, husb320_role_t role);

/**
 * @brief GPIO ISR handler for HUSB320 (do NOT touch I2C here).
 *
 * @param arg User argument
 */
void      husb320_isr_handler(void *arg);

/**
 * @brief Service and drain/ack interrupts (called from Type-C task).
 *
 * @param dev Device handle
 * @param had_any Set true if any IRQ was serviced
 * @return ESP_OK on success
 */
esp_err_t husb320_service_irq(husb320_dev_t *dev, bool *had_any);

/**
 * @brief Read current CC status (attach/orientation/Rp).
 *
 * @param dev Device handle
 * @param st Pointer to status struct
 * @return ESP_OK on success
 */
esp_err_t husb320_read_cc_status(husb320_dev_t *dev, husb320_cc_status_t *st);

#ifdef __cplusplus
}
#endif
