/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "usb/usb_types_cdc.h"

typedef struct cdc_dev_s *cdc_acm_dev_hdl_t;

/**
 * @brief CDC-ACM Device Event types to upper layer
 */
typedef enum {
    CDC_ACM_HOST_ERROR,
    CDC_ACM_HOST_SERIAL_STATE,
    CDC_ACM_HOST_NETWORK_CONNECTION,
    CDC_ACM_HOST_DEVICE_DISCONNECTED
} cdc_acm_host_dev_event_t;

/**
 * @brief CDC-ACM Device Event data structure
 */
typedef struct {
    cdc_acm_host_dev_event_t type;
    union {
        int error;                         //!< Error code from USB Host
        cdc_acm_uart_state_t serial_state; //!< Serial (UART) state
        bool network_connected;            //!< Network connection event
        cdc_acm_dev_hdl_t cdc_hdl;         //!< Disconnection event
    } data;
} cdc_acm_host_dev_event_data_t;

/**
 * @brief Data receive callback type
 *
 * @param[in] data     Pointer to received data
 * @param[in] data_len Length of received data in bytes
 * @param[in] user_arg User's argument passed to open function
 * @return true        Received data was processed     -> Flush RX buffer
 * @return false       Received data was NOT processed -> Append new data to the buffer
 */
typedef bool (*cdc_acm_data_callback_t)(const uint8_t *data, size_t data_len, void *user_arg);

/**
 * @brief Device event callback type
 *
 * @param[in] event    Event structure
 * @param[in] user_arg User's argument passed to open function
 */
typedef void (*cdc_acm_host_dev_callback_t)(const cdc_acm_host_dev_event_data_t *event, void *user_ctx);

/**
 * @brief Configuration structure of CDC-ACM device
 */
typedef struct {
    uint32_t connection_timeout_ms;       /**< Timeout for USB device connection in [ms] */
    size_t out_buffer_size;               /**< Maximum size of USB bulk out transfer, set to 0 for read-only devices */
    size_t in_buffer_size;                /**< Maximum size of USB bulk in transfer */
    cdc_acm_host_dev_callback_t event_cb; /**< Device's event callback function. Can be NULL */
    cdc_acm_data_callback_t data_cb;      /**< Device's data RX callback function. Can be NULL for write-only devices */
    void *user_arg;                       /**< User's argument that will be passed to the callbacks */
} cdc_acm_host_device_config_t;
