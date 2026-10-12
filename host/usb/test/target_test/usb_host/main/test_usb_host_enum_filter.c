/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "unity.h"

static const char *TAG = "usb_host_enum_filter";
void test_usb_host_reject_non_hub_devices_with_enum_filter(bool reject);

#define FILTER_TEST_EVENT_TIMEOUT_MS    2000
#define FILTER_TEST_ENUM_SETTLE_MS      2000

static void host_lib_task(void *arg)
{
    TaskHandle_t main_task = (TaskHandle_t)arg;

    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "All devices freed");
            break;
        }
    }

    ESP_LOGI(TAG, "Deleting host_lib_task");
    // Notify the test task after event processing has stopped.
    xTaskNotifyGive(main_task);
    vTaskDelete(NULL);
}

// Requires one external hub with at least one non-hub downstream device; the CI hardware setup has no external hub.
TEST_CASE("Test USB Host enum filter rejected downstream device channel lifetime", "[usb_host][enum_filter][hub][low_speed][full_speed][high_speed][ignore]")
{
    ESP_LOGI(TAG, "Start host event processing for enum filter rejection");
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(host_lib_task, "host_lib_task", 4096, (void *)xTaskGetCurrentTaskHandle(), 2, NULL));
    // Allow the hub and its downstream device to enumerate.
    vTaskDelay(pdMS_TO_TICKS(FILTER_TEST_ENUM_SETTLE_MS));

    usb_host_lib_info_t lib_info;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
    ESP_LOGI(TAG, "Enum filter rejected device result: num_devices=%d, root_port_suspended=%d",
             lib_info.num_devices,
             lib_info.root_port_suspended);

    ESP_LOGI(TAG, "Disable enum filter rejection and clean up any remaining filtered device object");
    test_usb_host_reject_non_hub_devices_with_enum_filter(false);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
    TEST_ASSERT_MESSAGE(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FILTER_TEST_EVENT_TIMEOUT_MS)), "Timed out waiting for host_lib_task to finish");
    // Check the captured result after cleanup so a regression does not leave the host task running.
    TEST_ASSERT_EQUAL_MESSAGE(1, lib_info.num_devices, "Only the hub should remain after the downstream device is rejected");
}
