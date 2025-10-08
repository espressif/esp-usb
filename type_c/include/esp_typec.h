/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
Warning: The Type‑C Core API is an initial (beta) version and may change.
This header covers **Type‑C CC (no PD)** features and a factory for **HUSB320**.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------- Macros and Types --------------------------------------------------


/**
 * @brief Monotonic API version so apps can assert compatibility at compile-time.
 */
#define ESP_TYPEC_API_VERSION  0x00000100u /* v0.1.0-beta */

// ----------------------- Handles -------------------------


/**
 * @brief Handle to a Type‑C port instance (CC controller).
 */
typedef struct esp_typec_port_s *esp_typec_port_handle_t;

// ------------------------ Events -------------------------


/**
 * @brief Type‑C event types.
 */
typedef enum {
    ESP_TYPEC_EVENT_ATTACHED,       /**< CC attach detected. payload: esp_typec_evt_attached_t */
    ESP_TYPEC_EVENT_DETACHED,       /**< CC detach detected. no payload */
    ESP_TYPEC_EVENT_ORIENTATION,    /**< Polarity changed. payload: bool (true=CC2) */
    ESP_TYPEC_EVENT_PWR_ROLE,       /**< Power role changed. payload: esp_typec_power_role_t */
    ESP_TYPEC_EVENT_ERROR,          /**< Error occurred. payload: esp_typec_evt_error_t */
} esp_typec_event_t;


/**
 * @brief Optional flags for event payloads.
 */
#define ESP_TYPEC_FLAG_CC2   (1u << 0)   /**< Polarity: CC2 active */


/**
 * @brief Type‑C power role.
 */
typedef enum {
    ESP_TYPEC_PWR_SINK = 0,    /**< Sink role */
    ESP_TYPEC_PWR_SOURCE,      /**< Source role */
    ESP_TYPEC_PWR_DRP,         /**< Dual-role power */
} esp_typec_power_role_t;


/**
 * @brief Type‑C error codes.
 */
typedef enum {
    ESP_TYPEC_ERR_NONE = 0,        /**< No error */
    ESP_TYPEC_ERR_HW_COMM,         /**< Hardware communication error */
    ESP_TYPEC_ERR_TIMEOUT,         /**< Timeout */
    ESP_TYPEC_ERR_STATE,           /**< Invalid state */
} esp_typec_error_t;


/**
 * @brief Payload for ESP_TYPEC_EVENT_ATTACHED event.
 */
typedef struct {
    uint32_t flags;        /**< ESP_TYPEC_FLAG_* */
    uint32_t rp_cur_ma;    /**< Advertised Rp current in mA, 0 if unknown */
} esp_typec_evt_attached_t;


/**
 * @brief Payload for ESP_TYPEC_EVENT_ERROR event.
 */
typedef struct {
    esp_typec_error_t code; /**< Error category */
    int               detail; /**< Backend-specific minor code */
} esp_typec_evt_error_t;


/**
 * @brief Type‑C event callback.
 *
 * @param event Event type
 * @param payload Event payload (valid only during callback)
 * @param arg User argument
 */
typedef void (*esp_typec_event_cb_t)(esp_typec_event_t event, const void *payload, void *arg);

// ---------------------- Configurations -------------------


/**
 * @brief Port creation configuration (Type‑C CC settings).
 */
typedef struct {
    esp_typec_power_role_t default_power_role; /**< Startup default; runtime changes allowed */
    bool try_snk;   /**< DRP Try.SNK policy (if supported by controller) */
    bool try_src;   /**< DRP Try.SRC policy (if supported by controller) */
    size_t task_stack; /**< Optional task stack (0 = default) */
    int    task_prio;  /**< Optional task priority (0 = default) */
} esp_typec_port_config_t;

// -------------------------- Info -------------------------

typedef struct {
    int num_ports;    /**< Number of created ports */
} esp_typec_lib_info_t;

// ------------------------------------------------ Library Functions --------------------------------------------------

esp_err_t esp_typec_install(void);
esp_err_t esp_typec_uninstall(void);
esp_err_t esp_typec_lib_info(esp_typec_lib_info_t *info_ret);

// ------------------------------------------------- Port Functions (HUSB320) -----------------------------------------

/** Hardware configuration for HUSB320 (CC-only, no PD). */
typedef struct {
    int      i2c_port;   /**< I2C controller port */
    uint8_t  i2c_addr;   /**< 7-bit I2C address */
    int      gpio_int;   /**< INT/ATTACH GPIO */
    bool     use_intr;   /**< If false, backend may poll (debug) */
} esp_typec_husb320_config_t;

/** Create a Type‑C CC-only port using HUSB320 (no PD). */
esp_err_t esp_typec_port_create_husb320(const esp_typec_port_config_t *port_cfg,
                                        const esp_typec_husb320_config_t *hw_cfg,
                                        esp_typec_event_cb_t cb, void *cb_arg,
                                        esp_typec_port_handle_t *port_hdl_ret);

/** Destroy a Type‑C port. */
esp_err_t esp_typec_port_destroy(esp_typec_port_handle_t port_hdl);

/** Power-role control (CC Rp/Rd). */
esp_err_t esp_typec_set_power_role(esp_typec_port_handle_t port_hdl, esp_typec_power_role_t role);

/** Query orientation and attach state. */
esp_err_t esp_typec_get_orientation(esp_typec_port_handle_t port_hdl, bool *cc2_active);
esp_err_t esp_typec_is_attached(esp_typec_port_handle_t port_hdl, bool *attached);

#ifdef __cplusplus
}
#endif
