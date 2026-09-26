/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "unity.h"

static const char *TAG = "usb_host_enum_filter";
void test_usb_host_reject_non_hub_devices_with_enum_filter(bool reject);

#define FILTER_TEST_EVENT_TIMEOUT_MS    2000
#define FILTER_TEST_SETTLE_MS           500

static void drain_usb_host_events(TickType_t ticks)
{
    TickType_t remaining_ticks = ticks;
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    while (xTaskCheckForTimeOut(&timeout, &remaining_ticks) == pdFALSE) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(20), &event_flags);
    }
}

static void wait_all_devices_free(void)
{
    TickType_t remaining_ticks = pdMS_TO_TICKS(FILTER_TEST_EVENT_TIMEOUT_MS);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    while (xTaskCheckForTimeOut(&timeout, &remaining_ticks) == pdFALSE) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(20), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("Timed out waiting for USB_HOST_LIB_EVENT_FLAGS_ALL_FREE");
}

static void force_all_devices_free(void)
{
    usb_host_lib_info_t lib_info;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
    if (lib_info.num_devices == 0) {
        return;
    }

    const esp_err_t ret = usb_host_device_free_all();
    TEST_ASSERT_TRUE(ret == ESP_OK || ret == ESP_ERR_NOT_FINISHED);
    if (ret == ESP_ERR_NOT_FINISHED) {
        wait_all_devices_free();
    }
}

TEST_CASE("Test USB Host enum filter rejected downstream device channel lifetime", "[usb_host][enum_filter][hub][low_speed][full_speed][high_speed]")
{
    ESP_LOGI(TAG, "Prepare a clean root port before enum filter rejection\n");
    drain_usb_host_events(pdMS_TO_TICKS(FILTER_TEST_EVENT_TIMEOUT_MS));

    usb_host_lib_info_t lib_info;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
    ESP_LOGI(TAG, "Enum filter rejected device result: num_devices=%d, root_port_suspended=%d\n",
             lib_info.num_devices,
             lib_info.root_port_suspended);
    TEST_ASSERT_EQUAL_MESSAGE(1, lib_info.num_devices, "Only the hub should remain after the downstream device is rejected");

    ESP_LOGI(TAG, "Disable enum filter rejection and clean up any remaining filtered device object\n");
    test_usb_host_reject_non_hub_devices_with_enum_filter(false);
    force_all_devices_free();
    drain_usb_host_events(pdMS_TO_TICKS(FILTER_TEST_SETTLE_MS));
}
