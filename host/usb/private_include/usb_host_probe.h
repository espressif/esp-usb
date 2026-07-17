/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "hcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event data object for Probe driver events
 */
typedef enum {
    PROBE_EVENT_PASSED,             /**< Post-light-sleep probe succeeded; device is still present */
    PROBE_EVENT_FAILED,             /**< Post-light-sleep probe failed; device is absent or changed */
} probe_event_t;

typedef struct {
    probe_event_t event;            /**< Probe driver event */
    union {
        struct {
            unsigned int uid;       /**< Unique node ID (probe passed) */
        } passed;
        struct {
            unsigned int uid;       /**< Unique node ID (probe failed) */
        } failed;
    };
} probe_event_data_t;

// ---------------------- Callbacks ------------------------

/**
 * @brief Callback used to indicate that the Probe driver has an event
 *
 * @note This callback is called from within probe_process()
 */
typedef void (*probe_event_cb_t)(probe_event_data_t *event_data, void *arg);

/**
 * @brief Probe driver configuration
 */
typedef struct {
    usb_proc_req_cb_t proc_req_cb;      /**< Processing request callback */
    void *proc_req_cb_arg;              /**< Processing request callback argument */
    probe_event_cb_t probe_event_cb;    /**< Probe event callback */
    void *probe_event_cb_arg;           /**< Probe event callback argument */
} probe_config_t;

/**
 * @brief Install the post-light-sleep device probe driver
 *
 * @param[in]  config     Probe driver configuration
 * @param[out] client_ret Unique pointer to identify the probe driver as a USB Host client
 *
 * @return
 *    - ESP_OK: Probe driver installed successfully
 *    - ESP_ERR_INVALID_STATE: Probe driver is already installed
 *    - ESP_ERR_INVALID_ARG: Invalid argument
 *    - ESP_ERR_NO_MEM: Insufficient memory
 */
esp_err_t probe_install(probe_config_t *config, void **client_ret);

/**
 * @brief Uninstall the probe driver
 *
 * @return
 *    - ESP_OK: Probe driver uninstalled successfully
 *    - ESP_ERR_INVALID_STATE: Probe driver is not installed
 */
esp_err_t probe_uninstall(void);

/**
 * @brief Start a post-light-sleep presence probe for a device
 *
 * Issues GET_DESCRIPTOR(DEVICE) at the device's current address. Does not use the
 * enumeration lock.
 *
 * @param[in] uid Unique device ID
 *
 * @return
 *    - ESP_OK: Probe started
 *    - ESP_ERR_INVALID_STATE: Probe driver not installed, probe already active, or enumeration is busy
 *    - ESP_ERR_NOT_FOUND: Device not found
 */
esp_err_t probe_start(unsigned int uid);

/**
 * @brief Cancel an in-progress probe
 *
 * @param[in] uid Unique device ID (unused if probe is active; kept for symmetry with enum_cancel)
 *
 * @return
 *    - ESP_OK: Probe canceled or not active
 *    - ESP_ERR_INVALID_STATE: Probe driver is not installed
 */
esp_err_t probe_cancel(unsigned int uid);

/**
 * @brief Probe driver processing function
 *
 * @return
 *    - ESP_OK: Processing completed normally
 *    - ESP_ERR_INVALID_STATE: Probe driver is not installed
 */
esp_err_t probe_process(void);

#ifdef __cplusplus
}
#endif
