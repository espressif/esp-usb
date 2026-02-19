/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Experimental stub: API/behavior may change or be incomplete. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_tcpm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief CC status snapshot for HUSB320.
 */
typedef struct {
    bool attached;        /**< Rd/Ra detected */
    bool cc2_active;      /**< true if CC2 is active line */
    uint32_t rp_current_ma;   /**< advertised current (0 if unknown) */
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

#ifdef __cplusplus
}
#endif
