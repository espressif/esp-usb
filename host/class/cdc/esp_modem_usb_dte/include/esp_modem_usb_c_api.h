/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_modem_c_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a modem DCE instance backed by a USB DTE.
 *
 * @param[in] module Modem device type to create.
 * @param[in] dte_config DTE configuration with a USB terminal extension.
 * @param[in] dce_config DCE configuration.
 * @param[in] netif Network interface handle used for data mode.
 *
 * @return Pointer to the created DCE instance on success, or NULL on failure.
 */
esp_modem_dce_t *esp_modem_new_dev_usb(esp_modem_dce_device_t module,
                                       const esp_modem_dte_config_t *dte_config,
                                       const esp_modem_dce_config_t *dce_config,
                                       esp_netif_t *netif);

#ifdef __cplusplus
}
#endif
