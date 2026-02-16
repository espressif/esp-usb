/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_tcpm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for a Type-C port (core-owned).
 */
typedef struct typec_port *typec_port_handle_t;

typedef enum {
    TYPEC_TOG_RESULT_NONE = 0, /**< Toggle engine still running / no result */
    TYPEC_TOG_RESULT_SRC,      /**< Partner is a sink; we should act as source */
    TYPEC_TOG_RESULT_SNK,      /**< Partner is a source; we should act as sink  */
} typec_tog_result_t;

/**
 * @brief Simple CC status snapshot for a Type-C controller.
 */
typedef struct {
    bool     attached;    /**< True when exactly one CC detects Rp */
    bool     cc2_active;  /**< True if CC2 is the active/connected CC */
    uint32_t rp_current_ma;   /**< Advertised/detected Rp current in mA (0 if unknown). */
    bool     vbus_ok;     /**< True if VBUSOK is asserted */
    typec_tog_result_t tog_result; /**< Result from toggle engine (if any) */
} typec_cc_status_t;

/**
 * @brief Backend event mask bits returned by the Type-C controller.
 */
typedef enum {
    TYPEC_EVT_CC    = (1u << 0),  /**< CC comparator / BC_LVL change */
    TYPEC_EVT_VBUS  = (1u << 1),  /**< VBUSOK change */
    TYPEC_EVT_RX    = (1u << 2),  /**< RX message available (reserved) */
    TYPEC_EVT_HRST  = (1u << 3),  /**< Hard reset detected */
    TYPEC_EVT_FAULT = (1u << 4),  /**< OCP/OTP/etc. */
    TYPEC_EVT_TOG   = (1u << 5),  /**< Toggle state change (TOG_DONE) */
} typec_evt_mask_t;

/**
 * @brief Backend operations for a Type-C port controller.
 */
typedef struct typec_port_backend {
    esp_err_t (*init)(void *device, const usb_tcpm_port_config_t *cfg); /**< Initialize backend. */
    esp_err_t (*deinit)(void *device); /**< Deinitialize backend (free device). */
    esp_err_t (*set_role)(void *device, usb_tcpm_power_role_t role); /**< Set power role. */
    esp_err_t (*service_irq)(void *device, typec_evt_mask_t *events); /**< Service IRQ and return events. */
    esp_err_t (*get_status)(void *device, typec_cc_status_t *status); /**< Read CC/VBUS status snapshot. */
    esp_err_t (*commit_attach)(void *device, bool cc2_active, bool is_source); /**< Commit orientation/role after attach. */
    esp_err_t (*get_irq_gpio)(void *device, gpio_num_t *gpio_num); /**< Return IRQ GPIO number. */
} typec_port_backend_t;

/**
 * @brief Create a Type-C port instance using a backend.
 *
 * The core takes ownership of backend_ctx only when this function returns ESP_OK.
 * If this function returns an error, ownership remains with the caller and
 * the caller is responsible for cleanup (for example by calling backend->deinit()).
 *
 * @param[in] backend Backend operations.
 * @param[in] backend_ctx Backend context (opaque, backend-owned).
 * @param[in] port_cfg Port configuration.
 * @param[out] out Port handle output.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is NULL
 *      - ESP_ERR_INVALID_STATE if usb_tcpm_install() has not been called
 *      - ESP_ERR_NO_MEM if allocation fails
 *      - Other error codes from backend/core initialization
 */
esp_err_t typec_port_new(const typec_port_backend_t *backend,
                         void *backend_ctx,
                         const usb_tcpm_port_config_t *port_cfg,
                         typec_port_handle_t *out);

#ifdef __cplusplus
}
#endif
