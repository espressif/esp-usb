/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

#include "esp_typec_pd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_typec_manager_ctx *esp_typec_manager_handle_t;

/**
 * High-level usage mode of the Type-C manager.
 *
 * For now we only implement SINK_DEVICE, but we keep the enum open.
 */
typedef enum {
    ESP_TYPEC_MANAGER_MODE_SINK_DEVICE = 0,  //!< act as Type-C sink + USB device (TinyUSB)
    ESP_TYPEC_MANAGER_MODE_SOURCE_HOST,      //!< (future) Type-C source + USB host
    ESP_TYPEC_MANAGER_MODE_DRP_DEVICE_HOST,  //!< (future) DRP with role swap
} esp_typec_manager_mode_t;

/**
 * Configuration for the manager.
 *
 * The application is responsible for:
 *  - initializing TinyUSB (device stack) before calling esp_typec_manager_install()
 *  - creating a PD port (esp_typec_pd_port_create_fusb302, etc.)
 */
typedef struct {
    esp_typec_manager_mode_t mode;

    // PD port we should follow
    esp_typec_pd_port_handle_t pd_port;

    // Optional: only connect TinyUSB if advertised Rp is at least this (mA).
    // 0 = ignore current, any valid attach will connect.
    uint32_t min_rp_cur_ma;

    // Future: we could add callbacks or extra policy flags here.
} esp_typec_manager_config_t;

/**
 * Install and start the Type-C manager.
 *
 * - Does NOT initialize TinyUSB; you must do that before calling here.
 * - Subscribes to ESP_TYPEC_PD_EVENT ATTACHED/DETACHED.
 * - In SINK_DEVICE mode, it will:
 *      - call tud_disconnect() initially
 *      - on ATTACHED as sink: if Rp >= min_rp_cur_ma, call tud_connect()
 *      - on DETACHED: call tud_disconnect()
 */
esp_err_t esp_typec_manager_install(const esp_typec_manager_config_t *cfg,
                                    esp_typec_manager_handle_t *out);

/**
 * Uninstall the manager.
 * - Unregisters event handlers.
 * - Does NOT deinit TinyUSB or the PD port.
 */
esp_err_t esp_typec_manager_uninstall(esp_typec_manager_handle_t handle);

/**
 * Optional helper: query whether the manager currently sees the port as
 * "USB bus should be connected" (i.e. attached and policy satisfied).
 */
esp_err_t esp_typec_manager_is_bus_present(esp_typec_manager_handle_t handle, bool *present);

#ifdef __cplusplus
}
#endif
