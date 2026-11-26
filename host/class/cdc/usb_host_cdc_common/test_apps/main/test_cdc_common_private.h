/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "usb/cdc_host_common.h"

extern ssize_t TEST_MEMORY_LEAK_THRESHOLD;

#define UPDATE_LEAK_THRESHOLD(first_val) \
static bool is_first = true; \
if (is_first) { \
    TEST_MEMORY_LEAK_THRESHOLD = first_val; \
    is_first = false; \
} else { \
    TEST_MEMORY_LEAK_THRESHOLD = 0; \
}

#define EVENT_CONNECT       BIT0
#define EVENT_DISCONNECT    BIT1
#define EVENT_DATA          BIT2
#define EVENT_NOTIFICATION  BIT3

#define TEST_DEV_ADDR       1
#define TEST_INTERFACE_0    0
#define TEST_INTERFACE_1    3
#define TEST_OPEN_TIMEOUT_MS 1
#define TEST_WAIT_DEV_MS    10000
#define TEST_BUF_SIZE       4096
#define TEST_RINGBUF_SIZE   4096

#define CDC_COMMON_OPEN_DEFAULT_CONFIG(_dev_addr, _itf_num, _data_cb, _event_cb, _user_arg) \
    { \
        .vid = CDC_HOST_COMMON_ANY_VID, \
        .pid = CDC_HOST_COMMON_ANY_PID, \
        .dev_addr = _dev_addr, \
        .interface_idx = _itf_num, \
        .connection_timeout_ms = TEST_OPEN_TIMEOUT_MS, \
        .ctrl_buffer_size = TEST_BUF_SIZE, \
        .in_buffer_size = TEST_BUF_SIZE, \
        .out_buffer_size = TEST_BUF_SIZE, \
        .rx_ringbuf_size = 0, \
        .tx_ringbuf_size = 0, \
        .flags = CDC_HOST_COMMON_OPEN_FLAG_NONE, \
        .data_cb = _data_cb, \
        .event_cb = _event_cb, \
        .user_arg = _user_arg, \
    }

#define CDC_COMMON_OPEN_RINGBUF_CONFIG(_dev_addr, _itf_num, _data_cb, _event_cb, _user_arg) \
    { \
        .vid = CDC_HOST_COMMON_ANY_VID, \
        .pid = CDC_HOST_COMMON_ANY_PID, \
        .dev_addr = _dev_addr, \
        .interface_idx = _itf_num, \
        .connection_timeout_ms = TEST_OPEN_TIMEOUT_MS, \
        .ctrl_buffer_size = TEST_BUF_SIZE, \
        .in_buffer_size = TEST_BUF_SIZE, \
        .out_buffer_size = TEST_BUF_SIZE, \
        .rx_ringbuf_size = TEST_RINGBUF_SIZE, \
        .tx_ringbuf_size = TEST_RINGBUF_SIZE, \
        .flags = CDC_HOST_COMMON_OPEN_FLAG_NONE, \
        .data_cb = _data_cb, \
        .event_cb = _event_cb, \
        .user_arg = _user_arg, \
    }

extern cdc_host_common_driver_handle_t test_driver;
extern cdc_host_common_port_handle_t test_port1;
extern cdc_host_common_port_handle_t test_port2;
extern EventGroupHandle_t test_event_group;
extern uint8_t connected_dev_addr;
extern int device_connect_count;
extern int device_disconnect_count;

void common_port_event_cb(cdc_host_common_port_handle_t port, const cdc_host_common_port_event_data_t *event, void *user_arg);
void common_dev_event_cb(const cdc_host_common_dev_event_data_t *event, void *user_arg);
esp_err_t install_common_driver(cdc_host_common_dev_event_cb_t event_cb, void *user_arg);
esp_err_t uninstall_common_driver(void);
bool wait_for_cdc_device(uint32_t timeout_ms);
uint8_t get_test_dev_addr(void);
void cdc_common_manual_reset_state(void);
void cdc_common_manual_cleanup(void);
