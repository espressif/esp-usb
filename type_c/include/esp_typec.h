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
#include "driver/gpio.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event base for Type-C events.
 *
 * Events are posted with:
 *  - base: ESP_TYPEC_EVENT
 *  - id: esp_typec_event_id_t
 */
ESP_EVENT_DECLARE_BASE(ESP_TYPEC_EVENT);

/**
 * @brief Type-C power role.
 */
typedef enum {
    ESP_TYPEC_PWR_SINK = 0,    /**< Sink role */
    ESP_TYPEC_PWR_SOURCE,      /**< Source role */
    ESP_TYPEC_PWR_DRP,         /**< Dual-role power */
} esp_typec_power_role_t;

/**
 * @brief Type-C event IDs.
 */
typedef enum {
    ESP_TYPEC_EVENT_ID_ATTACHED = 0,   /**< payload: esp_typec_evt_attached_t */
    ESP_TYPEC_EVENT_ID_DETACHED,       /**< payload: NULL */
    ESP_TYPEC_EVENT_ID_ERROR,          /**< payload: esp_typec_error_t (TBD) */
} esp_typec_event_id_t;

/**
 * @brief Advertised Rp current levels when acting as Source/DFP.
 */
typedef enum {
    ESP_TYPEC_RP_DEFAULT = 0, /**< Default USB current (~500 mA) */
    ESP_TYPEC_RP_1A5,         /**< 1.5 A advertised current */
    ESP_TYPEC_RP_3A0,         /**< 3.0 A advertised current */
} esp_typec_rp_current_t;

/**
 * @brief Type-C library install configuration (reserved for future use).
 * @note Currently no global tunables; reserved for future expansion.
 */
typedef struct {
    uint32_t reserved; /**< Reserved for future use */
} esp_typec_install_config_t;

/**
 * @brief Type-C port configuration structure.
 */
typedef struct {
    esp_typec_power_role_t default_power_role; /**< Default role at bring-up */
    uint32_t task_stack; /**< Task stack size in bytes (0 = default) */
    uint32_t task_prio;  /**< Task priority (0 = default) */
    esp_typec_rp_current_t rp_current; /**< Advertised Rp current when acting as Source/DFP */
    gpio_num_t src_vbus_gpio;        /**< GPIO_NUM_NC if not used (active-high) */
    gpio_num_t src_vbus_gpio_n;      /**< Optional active-low VBUS control (GPIO_NUM_NC if unused) */
} esp_typec_port_config_t;

/**
 * @brief Hardware configuration for FUSB302 backend.
 */
typedef struct {
    i2c_master_bus_handle_t  i2c_bus;     /**< I2C bus handle */
    uint8_t                  i2c_addr_7b; /**< 7-bit I2C address of FUSB302 */
    gpio_num_t               gpio_int;    /**< Active-low INT GPIO from the chip */
} esp_typec_fusb302_config_t;

/**
 * @brief Opaque handle for a Type-C port instance.
 */
typedef void *esp_typec_port_handle_t;

/**
 * @brief Event payload for Type-C ATTACHED event.
 */
typedef struct {
    esp_typec_port_handle_t port;  /**< Port that generated the event */
    bool     cc2_active;              /**< true if CC2 is active, false = CC1 */
    uint32_t rp_cur_ma;              /**< 0/500/1500/3000 mapped from BC_LVL */
} esp_typec_evt_attached_t;

/**
 * @brief Install and initialize the Type-C library.
 * @param config Optional install configuration
 * @return ESP_OK on success
 * @return Other error codes from esp_event_loop_create_default()
 */
esp_err_t esp_typec_install(const esp_typec_install_config_t *config);

/**
 * @brief Uninstall the Type-C library (no-op; does not delete the default event loop).
 * @return ESP_OK (no-op)
 */
esp_err_t esp_typec_uninstall(void);

/**
 * @brief Create a Type-C port backed by FUSB302 (I2C + INT).
 * events go via esp_event
 * @param port_cfg Port configuration
 * @param hw_cfg Hardware configuration for FUSB302
 * @param port_hdl_ret Pointer to port handle output
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if any argument is NULL or invalid
 * @return ESP_ERR_NO_MEM if allocation fails
 * @return Other error codes from GPIO/I2C/backend initialization
 */
esp_err_t esp_typec_port_create_fusb302(const esp_typec_port_config_t *port_cfg,
                                        const esp_typec_fusb302_config_t *hw_cfg,
                                        esp_typec_port_handle_t *port_hdl_ret);

/**
 * @brief Destroy a Type-C port instance and free resources.
 * @param port_hdl Port handle to destroy
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if port_hdl is NULL
 */
esp_err_t esp_typec_port_destroy(esp_typec_port_handle_t port_hdl);

/**
 * @brief Set power role (sink/source/drp) for a Type-C port.
 * @param port_hdl Port handle
 * @param role Power role to set
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if port_hdl is NULL
 * @return Other error codes from backend role configuration
 */
esp_err_t esp_typec_set_power_role(esp_typec_port_handle_t port_hdl,
                                   esp_typec_power_role_t role);

/**
 * @brief Get cable orientation (CC2 active) for a Type-C port.
 * @param port_hdl Port handle
 * @param cc2_active Pointer to bool output
 * @return ESP_OK if attached
 * @return ESP_ERR_INVALID_ARG if port_hdl or cc2_active is NULL
 * @return ESP_ERR_INVALID_STATE if not attached
 */
esp_err_t esp_typec_get_orientation(esp_typec_port_handle_t port_hdl, bool *cc2_active);

/**
 * @brief Get attached state for a Type-C port.
 * @param port_hdl Port handle
 * @param attached Pointer to bool output
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if port_hdl or attached is NULL
 */
esp_err_t esp_typec_is_attached(esp_typec_port_handle_t port_hdl, bool *attached);

#ifdef __cplusplus
}
#endif
