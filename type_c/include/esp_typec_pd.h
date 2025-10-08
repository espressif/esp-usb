/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"   // for i2c_master_dev_handle_t

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Public enums / flags ===== */


#include "esp_typec.h" // for esp_typec_power_role_t and ESP_TYPEC_FLAG_CC2

#define ESP_TYPEC_PD_FLAG_CC2   ESP_TYPEC_FLAG_CC2
/**
 * @brief PD event types for Type-C stack.
 */
typedef enum {
    ESP_TYPEC_PD_EVENT_ATTACHED = 0, /**< CC attach detected. payload: esp_typec_pd_evt_attached_t */
    ESP_TYPEC_PD_EVENT_DETACHED,     /**< CC detach detected. no payload */
    ESP_TYPEC_PD_EVENT_ERROR,        /**< Error occurred. payload: TBD */
} esp_typec_pd_event_t;

/**
 * @brief PD library install configuration (reserved for future use).
 * @note Currently no global tunables; reserved for future expansion.
 */
typedef struct {
    uint32_t reserved; /**< Reserved for future use */
} esp_typec_pd_install_config_t;

/**
 * @brief PD library info structure.
 */
typedef struct {
    uint32_t num_ports; /**< Number of created PD ports */
} esp_typec_pd_lib_info_t;

/**
 * @brief PD port configuration structure.
 */
typedef struct {
    esp_typec_power_role_t default_power_role; /**< Default role at bring-up */
    uint32_t task_stack; /**< Task stack size in bytes (0 = default) */
    uint32_t task_prio;  /**< Task priority (0 = default) */
} esp_typec_pd_port_config_t;

/**
 * @brief Hardware configuration for FUSB302 backend.
 */
typedef struct {
    i2c_master_dev_handle_t i2c_dev;  /**< I2C device handle */
    int                     gpio_int; /**< Active-low INT GPIO from the chip */
} esp_typec_pd_fusb302_config_t;

/**
 * @brief Event payload for PD ATTACHED event.
 */
typedef struct {
    uint32_t flags;     /**< ESP_TYPEC_PD_FLAG_CC2 if CC2 active */
    uint32_t rp_cur_ma; /**< 0/500/1500/3000 mapped from BC_LVL */
} esp_typec_pd_evt_attached_t;

/**
 * @brief Placeholder for future PD contract details.
 */
typedef struct {
    uint32_t mv; /**< Millivolts */
    uint32_t ma; /**< Milliamps */
} esp_typec_pd_contract_t;

/**
 * @brief Opaque handle for a PD port instance.
 */
typedef void *esp_typec_pd_port_handle_t;

/**
 * @brief Event callback signature for PD events.
 *
 * @param evt Event type
 * @param event_data Pointer to event payload
 * @param user_arg User argument
 */
typedef void (*esp_typec_pd_event_cb_t)(esp_typec_pd_event_t evt,
                                        const void *event_data,
                                        void *user_arg);

/**
 * @brief Install and initialize the PD library.
 * @param config Optional install configuration
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_install(const esp_typec_pd_install_config_t *config);

/**
 * @brief Uninstall and deinitialize the PD library.
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_uninstall(void);

/**
 * @brief Handle library-global events (stub; all work is per-port).
 * @param timeout_ticks Timeout for event handling
 * @param event_flags_ret Pointer to event flags output
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_lib_handle_events(TickType_t timeout_ticks, uint32_t *event_flags_ret);

/**
 * @brief Get PD library info (number of ports, etc).
 * @param info_ret Pointer to info struct to fill
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_lib_info(esp_typec_pd_lib_info_t *info_ret);

/**
 * @brief Create a PD port backed by FUSB302 (I2C + INT).
 * @param port_cfg Port configuration
 * @param hw_cfg Hardware configuration for FUSB302
 * @param cb Event callback
 * @param cb_arg User callback argument
 * @param port_hdl_ret Pointer to port handle output
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_port_create_fusb302(const esp_typec_pd_port_config_t *port_cfg,
                                           const esp_typec_pd_fusb302_config_t *hw_cfg,
                                           esp_typec_pd_event_cb_t cb, void *cb_arg,
                                           esp_typec_pd_port_handle_t *port_hdl_ret);

/**
 * @brief Destroy a PD port instance and free resources.
 * @param port_hdl Port handle to destroy
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_port_destroy(esp_typec_pd_port_handle_t port_hdl);

/**
 * @brief Set power role (sink/source/drp) for a PD port.
 * @param port_hdl Port handle
 * @param role Power role to set
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_set_power_role(esp_typec_pd_port_handle_t port_hdl,
                                      esp_typec_power_role_t role);

/**
 * @brief Request a fixed PD sink contract (stub).
 * @param port PD port handle
 * @param mv Millivolts requested
 * @param ma Milliamps requested
 * @return ESP_ERR_NOT_SUPPORTED (stub)
 */
esp_err_t esp_typec_pd_sink_request_fixed(esp_typec_pd_port_handle_t port, uint32_t mv, uint32_t ma);

/**
 * @brief Request a PD sink PPS contract (stub).
 * @param port PD port handle
 * @param mv Millivolts requested
 * @param ma Milliamps requested
 * @return ESP_ERR_NOT_SUPPORTED (stub)
 */
esp_err_t esp_typec_pd_sink_request_pps(esp_typec_pd_port_handle_t port, uint32_t mv, uint32_t ma);

/**
 * @brief Get current PD contract (stub).
 * @param port PD port handle
 * @param out Pointer to contract struct
 * @return ESP_ERR_NOT_SUPPORTED (stub)
 */
esp_err_t esp_typec_pd_get_contract(esp_typec_pd_port_handle_t port, esp_typec_pd_contract_t *out);

/**
 * @brief Get cable orientation (CC2 active) for a PD port.
 * @param port_hdl Port handle
 * @param cc2_active Pointer to bool output
 * @return ESP_OK if attached, ESP_ERR_INVALID_STATE if not
 */
esp_err_t esp_typec_pd_get_orientation(esp_typec_pd_port_handle_t port_hdl, bool *cc2_active);

/**
 * @brief Get attached state for a PD port.
 * @param port_hdl Port handle
 * @param attached Pointer to bool output
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_is_attached(esp_typec_pd_port_handle_t port_hdl, bool *attached);

#ifdef __cplusplus
}
#endif
