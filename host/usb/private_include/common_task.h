/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Acquire the shared common background task manager
 *
 * Creates the common-clients semaphore and background task on first acquire.
 * The first caller fixes the shared task configuration. Later acquires must
 * request a configuration that is compatible with the already running task.
 *
 * @param[in] task_priority Priority requested for the common task
 * @param[in] stack_size Stack size requested for the common task
 * @param[in] core_id Core affinity requested for the common task
 * @return
 *    - ESP_OK: The common task manager was acquired successfully
 *    - ESP_ERR_INVALID_ARG: Invalid task configuration was provided
 *    - ESP_ERR_INVALID_STATE: The existing common task is incompatible with the request
 *    - ESP_ERR_NO_MEM: Insufficient memory to create required resources
 */
esp_err_t common_task_acquire(UBaseType_t task_priority, configSTACK_DEPTH_TYPE stack_size, BaseType_t core_id);

/**
 * @brief Release the shared common background task manager
 *
 * Deletes the common background task and common-clients semaphore on last release.
 *
 * @return
 *    - ESP_OK: The common task manager was released successfully
 *    - ESP_ERR_INVALID_STATE: The common task manager is not acquired
 */
esp_err_t common_task_release(void);

#ifdef __cplusplus
}
#endif
