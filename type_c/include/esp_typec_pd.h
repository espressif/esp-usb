/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
Warning: The Type‑C PD API is an initial (beta) version and may change.
This header exposes **PD-only** functions and a factory for **FUSB302**.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------- Macros and Types --------------------------------------------------


/**
 * @brief Monotonic API version so apps can assert compatibility at compile-time.
 */
#define ESP_TYPEC_PD_API_VERSION  0x00000100u /* v0.1.0-beta */

// ----------------------- Handles -------------------------


/**
 * @brief Handle to a Type‑C PD port instance (PD-capable controller).
 */
typedef struct esp_typec_pd_port_s *esp_typec_pd_port_handle_t;

// ------------------------ Events -------------------------


/**
 * @brief Type‑C PD event types.
 */
typedef enum {
    ESP_TYPEC_PD_EVENT_ATTACHED,         /**< CC attach detected. payload: esp_typec_pd_evt_attached_t */
    ESP_TYPEC_PD_EVENT_DETACHED,         /**< CC detach detected. No payload */
    ESP_TYPEC_PD_EVENT_CONTRACT_READY,   /**< A PD contract is now valid. payload: esp_typec_pd_contract_t */
    ESP_TYPEC_PD_EVENT_CONTRACT_LOST,    /**< Contract lost (e.g., hard reset). No payload */
    ESP_TYPEC_PD_EVENT_PPS_ADJUSTED,     /**< PPS adjusted (sink). payload: esp_typec_pd_contract_t */
    ESP_TYPEC_PD_EVENT_ERROR,            /**< Error occurred. payload: esp_typec_pd_evt_error_t */
} esp_typec_pd_event_t;


/**
 * @brief Optional flags for event payloads.
 */
#define ESP_TYPEC_PD_FLAG_PPS   (1u << 0)   /**< Operating on a PPS/APDO contract */
#define ESP_TYPEC_PD_FLAG_CC2   (1u << 1)   /**< Polarity: CC2 is active */


/**
 * @brief Payload for ESP_TYPEC_PD_EVENT_ATTACHED event.
 */
typedef struct {
    uint32_t flags;        /**< ESP_TYPEC_PD_FLAG_* */
    uint32_t rp_cur_ma;    /**< Source advertised Rp current in mA (0 if unknown) */
} esp_typec_pd_evt_attached_t;


/**
 * @brief PD error codes.
 */
typedef enum {
    ESP_TYPEC_PD_ERR_NONE = 0,        /**< No error */
    ESP_TYPEC_PD_ERR_PROTOCOL,        /**< Protocol error */
    ESP_TYPEC_PD_ERR_REJECTED,        /**< Request rejected */
    ESP_TYPEC_PD_ERR_HARD_RESET,      /**< Hard reset occurred */
    ESP_TYPEC_PD_ERR_TIMEOUT,         /**< Timeout */
    ESP_TYPEC_PD_ERR_HW_COMM,         /**< Hardware communication error */
} esp_typec_pd_error_t;


/**
 * @brief Payload for ESP_TYPEC_PD_EVENT_ERROR event.
 */
typedef struct {
    esp_typec_pd_error_t code;   /**< Error category */
    int                  detail; /**< Backend-specific minor code */
} esp_typec_pd_evt_error_t;


/**
 * @brief PD contract snapshot.
 */
typedef struct {
    uint32_t mv;             /**< Contract voltage (mV) */
    uint32_t ma;             /**< Contract current (mA) */
    uint8_t  selected_pdo;   /**< 1-based index of selected PDO */
    uint8_t  reserved[3];
    uint32_t flags;          /**< ESP_TYPEC_PD_FLAG_* */
} esp_typec_pd_contract_t;


/**
 * @brief PD library event callback.
 *
 * @param event Event type
 * @param payload Event payload (valid only during callback)
 * @param arg User argument
 */
typedef void (*esp_typec_pd_event_cb_t)(esp_typec_pd_event_t event, const void *payload, void *arg);

// ---------------------- Configurations -------------------


/**
 * @brief PD power role.
 */
typedef enum {
    ESP_TYPEC_PD_PWR_SINK = 0,    /**< Sink role */
    ESP_TYPEC_PD_PWR_SOURCE,      /**< Source role */
    ESP_TYPEC_PD_PWR_DRP,         /**< Dual-role power */
} esp_typec_pd_power_role_t;

/** Type‑C PD library install configuration (global). */

/**
 * @brief Type‑C PD library install configuration (global).
 */
typedef struct {
    bool skip_ctrl_intr_alloc;   /**< If set, the backend driver will not allocate its own interrupt and may be polled (debug). */
    int  intr_flags;             /**< Interrupt flags used by backend controller ISR(s). */
} esp_typec_pd_install_config_t;

/** Port creation configuration (common PD settings). */

/**
 * @brief Port creation configuration (common PD settings).
 */
typedef struct {
    esp_typec_pd_power_role_t default_power_role;  /**< Startup default; runtime changes allowed */

    // Sink preferences
    uint32_t sink_i_min_ma;        /**< Minimum acceptable current */
    uint32_t sink_fixed_pref_mv;   /**< 0 = auto */
    uint32_t sink_pps_v_min_mv;    /**< Valid if enable_pps */
    uint32_t sink_pps_v_max_mv;
    uint32_t sink_pps_i_max_ma;
    bool     enable_pps;           /**< Allow PPS/APDO */

    // Optional Source capabilities (if acting as Source)
    const uint32_t *src_pdos;      /**< Array of 32-bit PDOs; NULL if not used */
    size_t          src_pdo_count; /**< Number of PDOs in src_pdos */

    // Task configuration (optional; backend may apply defaults)
    size_t task_stack;             /**< Task stack size in bytes (0 = default) */
    int    task_prio;              /**< Task priority (0 = default) */
} esp_typec_pd_port_config_t;

// -------------------------- Info -------------------------


/**
 * @brief PD library info.
 */
typedef struct {
    int num_ports;    /**< Number of created ports */
} esp_typec_pd_lib_info_t;

// ------------------------------------------------ Library Functions --------------------------------------------------


/**
 * @brief Install the Type‑C PD library.
 *
 * @param config Install configuration
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_install(const esp_typec_pd_install_config_t *config);

/**
 * @brief Uninstall the Type‑C PD library.
 *
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_uninstall(void);

/**
 * @brief Handle PD library events (polling).
 *
 * @param timeout_ticks Timeout in FreeRTOS ticks
 * @param event_flags_ret Event flags returned
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_lib_handle_events(TickType_t timeout_ticks, uint32_t *event_flags_ret);

/**
 * @brief Get PD library info.
 *
 * @param info_ret Pointer to info struct
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_lib_info(esp_typec_pd_lib_info_t *info_ret);

// ------------------------------------------------- Port Functions (FUSB302) -----------------------------------------


/**
 * @brief Hardware configuration for FUSB302 (PD-capable).
 */
typedef struct {
    int      i2c_port;   /**< I2C controller port */
    uint8_t  i2c_addr;   /**< 7-bit I2C address */
    int      gpio_int;   /**< ALERT/INT GPIO */
    bool     use_intr;   /**< If false, backend may poll (debug) */
} esp_typec_pd_fusb302_config_t;


/**
 * @brief Create a PD port using an FUSB302-family controller (PD-capable).
 *
 * @param port_cfg Port configuration
 * @param hw_cfg Hardware configuration
 * @param cb Event callback
 * @param cb_arg User argument for callback
 * @param port_hdl_ret Pointer to returned port handle
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_port_create_fusb302(const esp_typec_pd_port_config_t *port_cfg,
                                           const esp_typec_pd_fusb302_config_t *hw_cfg,
                                           esp_typec_pd_event_cb_t cb, void *cb_arg,
                                           esp_typec_pd_port_handle_t *port_hdl_ret);

/**
 * @brief Destroy a PD port.
 *
 * @param port_hdl Port handle
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_port_destroy(esp_typec_pd_port_handle_t port_hdl);

/**
 * @brief Set runtime power-role (Sink/Source/DRP).
 *
 * @param port_hdl Port handle
 * @param role Power role
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_set_power_role(esp_typec_pd_port_handle_t port_hdl, esp_typec_pd_power_role_t role);

/**
 * @brief Request a Fixed PDO as sink.
 *
 * @param port_hdl Port handle
 * @param mv Voltage in mV
 * @param ma Current in mA
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_sink_request_fixed(esp_typec_pd_port_handle_t port_hdl, uint32_t mv, uint32_t ma);

/**
 * @brief Request PPS as sink.
 *
 * @param port_hdl Port handle
 * @param mv Voltage in mV
 * @param ma Current in mA
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_sink_request_pps(esp_typec_pd_port_handle_t port_hdl, uint32_t mv, uint32_t ma);

/**
 * @brief Get the current contract snapshot.
 *
 * @param port_hdl Port handle
 * @param contract_out Pointer to contract struct
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_get_contract(esp_typec_pd_port_handle_t port_hdl, esp_typec_pd_contract_t *contract_out);

/**
 * @brief Query current orientation (polarity).
 *
 * @param port_hdl Port handle
 * @param cc2_active Pointer to bool, true if CC2 is active
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_get_orientation(esp_typec_pd_port_handle_t port_hdl, bool *cc2_active);

/**
 * @brief Query attach state.
 *
 * @param port_hdl Port handle
 * @param attached Pointer to bool, true if attached
 * @return ESP_OK on success
 */
esp_err_t esp_typec_pd_is_attached(esp_typec_pd_port_handle_t port_hdl, bool *attached);

#ifdef __cplusplus
}
#endif
