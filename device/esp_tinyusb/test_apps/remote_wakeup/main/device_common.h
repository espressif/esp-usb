/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "tinyusb.h"


#define TINYUSB_EXTRA_EVENT_BASE 10    /*!< Base value to avoid overlap with tinyusb_event_id_t */

typedef enum {
    TINYUSB_EVENT_SUSPENDED = TINYUSB_EXTRA_EVENT_BASE,     /*!< USB device suspended event, base to not overlap with tinyusb_event_id_t */
    TINYUSB_EVENT_RESUMED,            /*!< USB device resumed event */
} tinyusb_extra_event_id_t;

/**
 * @brief Test device setup
 */
void test_device_setup(void);

/**
 * @brief Test device teardown
 */
void test_device_teardown(void);

/**
 * @brief Test device wait
 *
 * @param event_id Event ID to wait for
 */
void test_device_wait_event(int event_id);

/**
 * @brief TinyUSB callback for device mount.
 *
 * @note
 * For Linux-based Hosts: Reflects the SetConfiguration() request from the Host Driver.
 * For Win-based Hosts: SetConfiguration() request is present only with available Class in device descriptor.
 */
void test_device_event_handler(tinyusb_event_t *event, void *arg);
