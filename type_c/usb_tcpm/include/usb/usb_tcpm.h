/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

struct typec_port;

/**
 * @brief Event base for Type-C events.
 *
 * Events are posted with:
 *  - base: USB_TCPM_EVENT
 *  - id: usb_tcpm_event_id_t
 *
 * Events are posted by the internal USB TCPM worker task using `esp_event_post()`
 * (not from ISR context). Handlers run in the context of the default event loop task.
 */
ESP_EVENT_DECLARE_BASE(USB_TCPM_EVENT);

/**
 * @brief Type-C power role.
 */
typedef enum {
    USB_TCPM_PWR_SINK = 0,    /**< Sink role */
    USB_TCPM_PWR_SOURCE,      /**< Source role */
    USB_TCPM_PWR_DRP,         /**< Dual-role power */
} usb_tcpm_power_role_t;

/**
 * @brief Type-C event IDs.
 */
typedef enum {
    USB_TCPM_EVENT_SINK_ATTACHED = 0,   /**< payload: usb_tcpm_evt_attached_t */
    USB_TCPM_EVENT_SINK_DETACHED,       /**< payload: usb_tcpm_evt_detached_t */
    USB_TCPM_EVENT_SINK_RP_CURRENT_CHANGED,    /**< Reserved (not emitted yet), payload: usb_tcpm_evt_attached_t */
    USB_TCPM_EVENT_SOURCE_ATTACHED,     /**< payload: usb_tcpm_evt_attached_t */
    USB_TCPM_EVENT_SOURCE_DETACHED,     /**< payload: usb_tcpm_evt_detached_t */
    USB_TCPM_EVENT_ERROR,               /**< payload: usb_tcpm_error_t (TBD) */
} usb_tcpm_event_id_t;

/**
 * @brief Advertised Rp current levels when acting as Source/DFP.
 */
typedef enum {
    USB_TCPM_RP_DEFAULT = 0, /**< Default USB current (~500 mA) */
    USB_TCPM_RP_1A5,         /**< 1.5 A advertised current */
    USB_TCPM_RP_3A0,         /**< 3.0 A advertised current */
} usb_tcpm_rp_current_t;

/**
 * @brief Type-C library install configuration.
 */
typedef struct {
    uint32_t task_stack; /**< Shared worker task stack size in bytes (0 = default). */
    uint32_t task_prio;  /**< Shared worker task priority (0 = default). */
} usb_tcpm_install_config_t;

/**
 * @brief Type-C port configuration structure.
 */
typedef struct {
    usb_tcpm_power_role_t default_power_role; /**< Default role at bring-up */
    usb_tcpm_rp_current_t rp_current; /**< Advertised Rp current when acting as Source/DFP */
    gpio_num_t src_vbus_gpio;        /**< Active-high VBUS control (GPIO_NUM_NC if not used)*/
    gpio_num_t src_vbus_gpio_n;      /**< Optional active-low VBUS control (GPIO_NUM_NC if unused) */
} usb_tcpm_port_config_t;

/**
 * @brief Opaque handle for a Type-C port instance.
 */
typedef struct typec_port *usb_tcpm_port_handle_t;

/**
 * @brief Event payload for Type-C ATTACHED event.
 */
typedef struct {
    usb_tcpm_port_handle_t port;  /**< Port that generated the event */
    bool     cc2_active;              /**< true if CC2 is active, false = CC1 */
    uint32_t rp_current_ma;          /**< Advertised/detected Rp current in mA (0 if unknown). */
} usb_tcpm_evt_attached_t;

/**
 * @brief Event payload for Type-C DETACHED event.
 */
typedef struct {
    usb_tcpm_port_handle_t port; /**< Port that generated the event */
} usb_tcpm_evt_detached_t;

/**
 * @brief Type-C port status snapshot.
 */
typedef struct {
    bool attached; /**< True if attached. */
    bool cc2_active; /**< True if CC2 is active, false = CC1. */
    uint32_t rp_current_ma; /**< Advertised/detected Rp current in mA (0 if unknown). */
    usb_tcpm_power_role_t role; /**< Current power role. */
} usb_tcpm_port_status_t;

/**
 * @brief Install and initialize the Type-C library.
 *
 * @param[in] config Optional install configuration.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if shared worker task creation fails
 *      - Other error codes from esp_event_loop_create_default()
 */
esp_err_t usb_tcpm_install(const usb_tcpm_install_config_t *config);

/**
 * @brief Uninstall the Type-C library.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if one or more ports are still active
 */
esp_err_t usb_tcpm_uninstall(void);

/**
 * @brief Destroy a Type-C port instance and free resources.
 *
 * @param[in] port_hdl Port handle to destroy.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if port_hdl is NULL
 */
esp_err_t usb_tcpm_port_destroy(usb_tcpm_port_handle_t port_hdl);

/**
 * @brief Get Type-C port status.
 *
 * @param[in] port_hdl Port handle.
 * @param[out] status Pointer to status output.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if port_hdl or status is NULL
 */
esp_err_t usb_tcpm_get_status(usb_tcpm_port_handle_t port_hdl, usb_tcpm_port_status_t *status);

#ifdef __cplusplus
}
#endif
