/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_types_stack.h"
#include "libuvc/libuvc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UVC_DEVICE_CONNECTED = 1,
    UVC_DEVICE_DISCONNECTED = 2,
} libuvc_adapter_event_t;

typedef void (*libuvc_adapter_cb_t)(libuvc_adapter_event_t);

/**
 * @brief Configuration structure
 */
typedef struct {
    bool create_background_task;    /**< Event handling background task is created when set to true.
                                         Otherwise, user has to handle event by calling libuvc_adapter_handle_events */
    uint8_t task_priority;          /**< Background task priority */
    uint32_t stack_size;            /**< Background task stack size */
    libuvc_adapter_cb_t callback;   /**< Callback notifying about connection and disconnection events */
} libuvc_adapter_config_t;

/**
 * @brief Sets configuration for libuvc adapter
 *
 * - This function can be called prior to calling `uvc_init` function,
 *   otherwise default configuration will be used
 *
 * @param[in] config Configuration structure
 */
void libuvc_adapter_set_config(libuvc_adapter_config_t *config);

/**
 * @brief Prints full configuration descriptor
 *
 * @param[in] device Device handle obtained from `uvc_open`
 * @return esp_err_t
 */
esp_err_t libuvc_adapter_print_descriptors(uvc_device_handle_t *device);

/**
 * @brief Handle USB Client events.
 *
 * - This function has to be called periodically, if configuration
 *   was provided with `create_background_task` set to `false`.
 *
 * @param[in]  timeout_ms  Timeout in milliseconds
 * @return esp_err_t
 */
esp_err_t libuvc_adapter_handle_events(uint32_t timeout_ms);

/**
 * @brief Get information about underlying USB device
 *
 * @param[in]  dev      UVC device handle obtained from uvc_find_device()
 * @param[out] dev_info Pointer to structure where the information will be saved
 */
esp_err_t libuvc_get_usb_device_info(uvc_device_t *dev, usb_device_info_t *dev_info);

#ifdef __cplusplus
}
#endif
