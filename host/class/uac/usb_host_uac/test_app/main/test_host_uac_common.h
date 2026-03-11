/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb/uac_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Freertos queue for delivering RX/TX transfer events
 *
 * UAC_HOST_DEVICE_EVENT_RX_DONE
 * UAC_HOST_DEVICE_EVENT_TX_DONE
 * UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR
 */
extern QueueHandle_t transfer_event_queue;

/**
 * @brief Freertos queue for delivering the rest of the UAC client events
 *
 * UAC_HOST_DRIVER_EVENT_DISCONNECTED
 * UAC_HOST_DEVICE_EVENT_SUSPENDED
 * UAC_HOST_DEVICE_EVENT_RESUMED
 */
extern QueueHandle_t client_event_queue;

/**
 * @brief Event group ID
 */
typedef enum {
    APP_EVENT = 0,      /* Application event*/
    UAC_DRIVER_EVENT,   /* UAC driver event defined in uac_host_driver_event_t */
    UAC_DEVICE_EVENT,   /* UAC device event defined in uac_host_device_event_t*/
} event_group_t;

/**
 * @brief Event queue
 */
typedef struct {
    event_group_t event_group;                  /* Event group ID*/
    union {
        struct {
            uac_host_driver_event_t event;      /* UAC Host driver event type */
            uint8_t addr;                       /* Device address */
            uint8_t iface_num;                  /* Interface number */
            void *arg;                          /* Event argument*/
        } driver_evt;                           /* UAC Host driver event group */
        struct {
            uac_host_device_event_t event;      /* UAC Host device event type */
            uac_host_device_handle_t handle;    /* Device handle*/
            void *arg;                          /* Event argument */
        } device_evt;                           /* UAC Host device event group */
    };
} event_queue_t;

/**
 * @brief Force connection/disconnection of the device
 *
 * @param[in] connected  connect/disconnect
 * @param[in] ticks  Ticks to wait before connection/disconnection
 */
void force_conn_state(bool connected, TickType_t delay_ticks);

/**
 * @brief Setups UAC testing
 *
 * - Create USB lib task
 * - Install UAC Host driver
 */
void test_uac_setup(void);

/**
 * @brief Reset event queues
 */
void test_uac_queue_reset(void);

/**
 * @brief Tears down the UAC testing
 *
 * Uninstall UAC host, delete freertos primitives
 */
void test_uac_teardown(bool force);

/**
 * @brief Open microphone device
 *
 * @param[in] iface_num USB Interface number
 * @param[in] buffer_size Audio buffer size
 * @param[in] buffer_threshold Audio buffer threshold
 * @param[out] uac_device_handle Handle of the opened UAC device
 */
void test_open_mic_device(uint8_t iface_num, uint32_t buffer_size, uint32_t buffer_threshold, uac_host_device_handle_t *uac_device_handle);

/**
 * @brief Open speaker device
 *
 * @param[in] iface_num USB Interface number
 * @param[in] buffer_size Audio buffer size
 * @param[in] buffer_threshold Audio buffer threshold
 * @param[out] uac_device_handle Handle of the opened UAC device
 */
void test_open_spk_device(uint8_t iface_num, uint32_t buffer_size, uint32_t buffer_threshold, uac_host_device_handle_t *uac_device_handle);

/**
 * @brief Close UAC device
 *
 * @param[in] uac_device_handle Handle of the opened UAC device
 */
void test_close_device(uac_host_device_handle_t uac_device_handle);

/**
 * @brief Connect UAC device
 *
 * Wait for a device connection event, distinguish between RX/TX device
 *
 * @param[out] iface_num USB Interface number of the connected device
 * @param[out] if_rx Type connected device (RX/TX, microphone/speaker)
 */
void test_handle_dev_connection(uint8_t *iface_num, uint8_t *if_rx);

/**
 * @brief Device callback for delivering client events
 *
 * @param[out] uac_device_handle Handle of the device, from which an event was delivered
 * @param[out] event Actual event
 * @param[out] arg Pointer to callback arguments
 */
void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg);

/**
 * @brief Expect USB Host client event
 *
 * @param[in] expected_client_event  Expected client event (NULL to not expect any event)
 * @param[in] ticks  Ticks to wait
 */
void expect_client_event(const event_queue_t *expected_client_event, TickType_t ticks);

/**
 * @brief Expect USB Host transfer event
 *
 * @param[in] expected_transfer_event  Expected transfer event (data RX/TX) (NULL to not expect any event)
 * @param[in] ticks  Ticks to wait
 * @return Count of delivered events
 */
size_t expect_transfer_event(const event_queue_t *expected_transfer_event, TickType_t ticks);

#ifdef __cplusplus
}
#endif //__cplusplus
