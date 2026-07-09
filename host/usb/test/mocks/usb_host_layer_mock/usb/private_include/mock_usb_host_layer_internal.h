/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
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

typedef struct {
    usbh_event_cb_t usbh_event_cb;
    enum_event_cb_t enum_event_cb;
    hub_event_cb_t hub_event_cb;
#if ENABLE_ENUM_FILTER_CALLBACK
    usb_host_enum_filter_cb_t enum_filter_cb;
#endif
} mock_usb_host_layer_callbacks_t;

/**
 * @brief Access captured Host-layer callback pointers
 *
 * @note Internal API shared between mock_usb_host_layer_callbacks.c and
 *       mock_usb_host_layer_events.c. Not intended for use in tests.
 *
 * @return Pointer to captured callback storage
 */
mock_usb_host_layer_callbacks_t *mock_usb_host_layer_callbacks_get(void);

#ifdef __cplusplus
}
#endif
