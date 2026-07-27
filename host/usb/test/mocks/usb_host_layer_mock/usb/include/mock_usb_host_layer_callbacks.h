/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "enum.h"
#include "hub.h"
#include "usbh.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register callbacks from USBH layer
 *
 * @param[in] usbh_config USBH configuration passed to usbh_install()
 * @param[in] call_count CMock call count
 *
 * @note CMock callback function, registered to usbh_install()
 *
 * @return
 *    - ESP_OK: Callback registered successfully
 *    - ESP_ERR_INVALID_ARG: usbh_config is NULL
 */
esp_err_t usbh_register_mock_callback(const usbh_config_t *usbh_config, int call_count);

/**
 * @brief Deregister callbacks from USBH layer
 *
 * @param[in] call_count CMock call count
 *
 * @note CMock callback function, registered to usbh_uninstall()
 *
 * @return ESP_OK
 */
esp_err_t usbh_deregister_mock_callback(int call_count);

/**
 * @brief Register callbacks from Enum layer
 *
 * @param[in] enum_config Enum configuration passed to enum_install()
 * @param[out] client_ret Enum client handle returned to the host layer
 * @param[in] call_count CMock call count
 *
 * @note CMock callback function, registered to enum_install()
 *
 * @return
 *    - ESP_OK: Callback registered successfully
 *    - ESP_ERR_INVALID_ARG: enum_config or client_ret is NULL
 */
esp_err_t enum_register_mock_callback(enum_config_t *enum_config, void **client_ret, int call_count);

/**
 * @brief Deregister callbacks from Enum layer
 *
 * @param[in] call_count CMock call count
 *
 * @note CMock callback function, registered to enum_uninstall()
 *
 * @return ESP_OK
 */
esp_err_t enum_deregister_mock_callback(int call_count);

/**
 * @brief Register callbacks from Hub layer
 *
 * @param[in] hub_config Hub configuration passed to hub_install()
 * @param[out] client_ret Hub client handle returned to the host layer
 * @param[in] call_count CMock call count
 *
 * @note CMock callback function, registered to hub_install()
 *
 * @return
 *    - ESP_OK: Callback registered successfully
 *    - ESP_ERR_INVALID_ARG: hub_config or client_ret is NULL
 */
esp_err_t hub_register_mock_callback(hub_config_t *hub_config, void **client_ret, int call_count);

/**
 * @brief Deregister callbacks from Hub layer
 *
 * @param[in] call_count CMock call count
 *
 * @note CMock callback function, registered to hub_uninstall()
 *
 * @return ESP_OK
 */
esp_err_t hub_deregister_mock_callback(int call_count);

#if ENABLE_ENUM_FILTER_CALLBACK
/**
 * @brief Get the enumeration filter callback captured during enum_install()
 *
 * @return Filter callback passed from usb_host_install() via enum_install(), or NULL
 */
usb_host_enum_filter_cb_t mock_usb_host_layer_get_enum_filter_cb(void);
#endif

#ifdef __cplusplus
}
#endif
