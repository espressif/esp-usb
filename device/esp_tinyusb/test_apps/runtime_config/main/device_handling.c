/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
//
#include <stdio.h>
#include <string.h>
//
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
//
#include "unity.h"
#include "tinyusb.h"

static SemaphoreHandle_t wait_mount = NULL;

#define TUSB_DEVICE_DELAY_MS        2000

void test_device_setup(void)
{
    wait_mount = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(wait_mount);
}

void test_device_release(void)
{
    TEST_ASSERT_NOT_NULL(wait_mount);
    vSemaphoreDelete(wait_mount);
    wait_mount = NULL;
}

void test_device_wait(void)
{
    // Wait for tud_mount_cb() to be called
    if (wait_mount) {
        TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xSemaphoreTake(wait_mount, pdMS_TO_TICKS(TUSB_DEVICE_DELAY_MS)), "No tusb_mount_cb() in time");
    }
    // Delay to allow finish the enumeration
    // Disable this delay could lead to potential race conditions when the tud_task() is pinned to another CPU
    vTaskDelay(pdMS_TO_TICKS(250));
}

void test_device_event_handler(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        if (wait_mount) {
            xSemaphoreGive(wait_mount);
        }
        break;
    case TINYUSB_EVENT_DETACHED:
        break;
    default:
        break;
    }
}

#endif // SOC_USB_OTG_SUPPORTED
