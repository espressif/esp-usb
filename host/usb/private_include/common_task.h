/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Acquire the shared common background task manager
 *
 * Creates the common background task on first acquire.
 *
 * @return
 *    - ESP_OK: The common task manager was acquired successfully
 *    - ESP_ERR_NO_MEM: Insufficient memory to create required resources
 */
esp_err_t common_task_acquire(void);

/**
 * @brief Release the shared common background task manager
 *
 * Deletes the common background task on last release.
 *
 * @return
 *    - ESP_OK: The common task manager was released successfully
 *    - ESP_ERR_INVALID_STATE: The common task manager is not acquired, or drivers are still registered
 */
esp_err_t common_task_release(void);

/**
 * @brief Register a driver with the common background task manager
 *
 * @param[in] driver_ctx Driver context to register
 *
 * @return
 *    - ESP_OK: The driver was registered successfully
 *    - ESP_ERR_INVALID_ARG: Invalid argument
 *    - ESP_ERR_INVALID_STATE: The common task manager is not acquired
 */
esp_err_t common_task_register_driver(void *driver_ctx);

/**
 * @brief Unregister a driver from the common background task manager
 *
 * @param[in] driver_ctx Driver context to unregister
 *
 * @return
 *    - ESP_OK: The driver was unregistered successfully
 *    - ESP_ERR_INVALID_ARG: Invalid argument
 *    - ESP_ERR_INVALID_STATE: No registered driver matches the request, or the common task manager is not acquired
 */
esp_err_t common_task_unregister_driver(void *driver_ctx);

#ifdef __cplusplus
}
#endif
