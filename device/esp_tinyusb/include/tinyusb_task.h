/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TinyUSB Task type configuration
 */
typedef enum {
    TINYUSB_TASK_TYPE_INTERNAL = 0,     /*!< TinyUSB task tud_task() is handled internally */
    TINYUSB_TASK_TYPE_EXTERNAL          /*!< TinyUSB task tud_task() is handled externally */
} tinyusb_task_type_t;

/**
 * @brief TinyUSB Task affinity mask
 */
typedef enum {
    TINYUSB_TASK_AFFINITY_NONE = 0x01,
    TINYUSB_TASK_AFFINITY_CPU0,
    TINYUSB_TASK_AFFINITY_CPU1,
    TINYUSB_TASK_AFFINITY_MAX
} tinyusb_task_affinity_mask_t;

/**
 * @brief TinyUSB Task: Stack init parameters
 */
typedef struct {
    tusb_port_t rhport;                         /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    const tusb_rhport_init_t *rhport_init;      /*!< USB Device RH port initialization configuration pointer */
} tinyusb_task_init_params_t;

/**
 * @brief TinyUSB Task configuration
 */
typedef struct {

    uint16_t size;                              /*!< USB Device Task size. */
    uint8_t priority;                           /*!< USB Device Task priority. */
    tinyusb_task_affinity_mask_t affinity;      /*!< USB Device Task affinity.  */
    /*
    Run TinyUSB stack initialization in the default TinyUSB task.
    This is especially useful in multicore scenarios, when we need to pin the task
    to a specific core and, at the same time initialize TinyUSB stack
    (i.e. install interrupts) on the same core.
    */
    bool init_in_task;                          /*!< USB Device Task stack initialization place. */
} tinyusb_task_config_t;


/**
 * @brief Initialize TinyUSB Task
 *
 * Prepares TinyUSB Task for running TinyUSB stack.
 *
 * @param task_cfg TinyUSB Task configuration
 * @param init_params TinyUSB Task initialization parameters, NULL when init in task not required
 *
 * @retval
 *    - ESP_ERR_NOT_SUPPORTED if TinyUSB Task already started
 *    - ESP_ERR_INVALID_ARG if task_cfg is NULL or init_params is NULL when init_in_task is true
 *    - ESP_ERR_NOT_FINISHED if TinyUSB task creation failed
 *    - ESP_ERR_NO_MEM if memory allocation failed
 *    - ESP_OK if TinyUSB Task initialized successfully
 */
esp_err_t tinyusb_task_start(const tinyusb_task_config_t *task_cfg, const tinyusb_task_init_params_t *init_params);

/**
 * @brief Stops TinyUSB Task
 *
 * @note function should be called only when TinyUSB task was initialized via tinyusb_task_init()
 *
 * @retval
 *    - ESP_ERR_NOT_SUPPORTED if TinyUSB Task not initialized
 *    - ESP_OK if TinyUSB Task deinitialized successfully
 */
esp_err_t tinyusb_task_stop(void);

#ifdef __cplusplus
}
#endif
