/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "tinyusb_types.h"
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TinyUSB Task configuration
 */
typedef struct {
    size_t size;                                /*!< USB Device Task size. */
    uint8_t priority;                           /*!< USB Device Task priority. */
    int  xCoreID;                               /*!< USB Device Task core affinity.  */
} tinyusb_task_config_t;

/**
 * @brief Check the TinyUSB Task configuration
 *
 * This function checks the TinyUSB Task configuration parameters.
 *
 * @param task_cfg TinyUSB Task configuration
 * @retval
 *   - ESP_ERR_INVALID_ARG if task_cfg is NULL, size is 0, priority is 0 or affinity is invalid
 *   - ESP_OK if task_cfg is valid
 */
esp_err_t tinyusb_task_check_config(const tinyusb_task_config_t *task_cfg);

/**
 * @brief Starts TinyUSB Task
 *
 * Prepares TinyUSB Task for running TinyUSB stack.
 *
 * @param port USB Peripheral hardware port number. Available when hardware has several available peripherals.
 * @param task_cfg TinyUSB Task configuration
 *
 * @retval
 *    - ESP_ERR_INVALID_STATE if TinyUSB Task already started
 *    - ESP_ERR_INVALID_ARG if task_cfg is NULL or init_params is NULL when init_in_task is true
 *    - ESP_ERR_TIMEOUT if task was not able to start TinyUSB stack
 *    - ESP_ERR_NOT_FINISHED if TinyUSB task creation failed
 *    - ESP_ERR_NO_MEM if memory allocation failed
 *    - ESP_OK if TinyUSB Task initialized successfully
 */
esp_err_t tinyusb_task_start(tinyusb_port_t port, const tinyusb_task_config_t *task_cfg);

/**
 * @brief Stops TinyUSB Task
 *
 * @note function should be called only when TinyUSB task was initialized via tinyusb_task_start()
 *
 * @retval
 *    - ESP_ERR_INVALID_STATE if TinyUSB Task not initialized
 *    - ESP_OK if TinyUSB Task deinitialized successfully
 */
esp_err_t tinyusb_task_stop(void);

#ifdef __cplusplus
}
#endif
