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
 * @brief CC status snapshot for FUSB302.
 */
typedef struct {
    bool attached;        /**< CC attach detected */
    bool cc2_active;      /**< true if CC2 is the active/polarity line */
    uint32_t rp_cur_ma;   /**< advertised current (0 if unknown) */
} fusb302_cc_status_t;


/**
 * @brief Hardware configuration for FUSB302.
 */
typedef struct {
    int i2c_port;      /**< I2C controller port */
    uint8_t i2c_addr;  /**< 7-bit I2C address */
    int gpio_int;      /**< ALERT/INT GPIO */
    bool use_intr;     /**< If false, backend may poll (debug) */
} fusb302_hw_cfg_t;

/**
 * @brief Power role for FUSB302.
 */
typedef enum {
    FUSB302_ROLE_SINK = 0,
    FUSB302_ROLE_SOURCE,
    FUSB302_ROLE_DRP,
} fusb302_role_t;


/**
 * @brief Opaque device handle for FUSB302.
 */
typedef struct fusb302_dev fusb302_dev_t;

/**
 * @brief Set power role (Rp/Rd/DRP) for FUSB302.
 *
 * @param dev Device handle
 * @param role Power role
 * @return ESP_OK on success
 */
esp_err_t fusb302_set_role(fusb302_dev_t *dev, fusb302_role_t role);

// Init/deinit & IRQ wiring

/**
 * @brief Initialize FUSB302 device.
 *
 * @param hw Hardware configuration
 * @param out Pointer to device handle
 * @return ESP_OK on success
 */
esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out);

/**
 * @brief Deinitialize FUSB302 device.
 *
 * @param dev Device handle
 */
void      fusb302_deinit(fusb302_dev_t *dev);

/**
 * @brief GPIO ISR handler for FUSB302 (attach to your GPIO ISR, just queue a token).
 *
 * @param arg User argument
 */
void      fusb302_isr_handler(void *arg);

/**
 * @brief Service and drain/ack interrupts (read cause regs until empty).
 *
 * @param dev Device handle
 * @param had_any Set true if any IRQ was serviced
 * @return ESP_OK on success
 */
esp_err_t fusb302_service_irq(fusb302_dev_t *dev, bool *had_any);

/**
 * @brief Read current CC status (attach/orientation/Rp).
 *
 * @param dev Device handle
 * @param st Pointer to status struct
 * @return ESP_OK on success
 */
esp_err_t fusb302_read_cc_status(fusb302_dev_t *dev, fusb302_cc_status_t *st);

#ifdef __cplusplus
}
#endif
