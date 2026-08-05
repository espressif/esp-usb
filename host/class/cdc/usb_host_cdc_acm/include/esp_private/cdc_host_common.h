/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>                  // For singly linked list

#include "usb/cdc_acm_host_interface.h" // For CDC interface function table
#include "usb/cdc_host_common.h"        // For common CDC host port
#include "usb/usb_types_cdc.h"          // For protocol and serial state

// CDC-ACM check macros
#define CDC_ACM_CHECK(cond, ret_val) ({                             \
    if (!(cond)) {                                                  \
        return (ret_val);                                           \
    }                                                               \
})

#define CDC_ACM_CHECK_FROM_CRIT(cond, ret_val) ({                   \
    if (!(cond)) {                                                  \
        CDC_ACM_EXIT_CRITICAL();                                    \
        return ret_val;                                             \
    }                                                               \
})

typedef struct cdc_dev_s cdc_dev_t;
struct cdc_dev_s {
    cdc_acm_intf_t intf_func;             // CDC interface function table
    cdc_host_common_port_handle_t common_port; // Common CDC host port
    struct {
        const usb_intf_desc_t *intf_desc; // Compatibility field for VCP drivers that issue vendor requests on the data interface
    } data;
    struct {
        const usb_intf_desc_t *intf_desc; // Compatibility field for VCP drivers that issue vendor requests on the notification interface
    } notif;
    cdc_acm_data_callback_t in_cb;       // User's callback for async (non-blocking) data IN
    cdc_acm_host_dev_callback_t event_cb; // User's callback for device events
    void *cb_arg;                         // Common argument for user's callbacks
    cdc_acm_uart_state_t serial_state;    // Serial State
    SLIST_ENTRY(cdc_dev_s) list_entry;
};
