/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "ctrl_client.h"
#include "usb/usb_host.h"
#include "unity.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_private/sleep_event.h"
#include "soc/soc_caps.h"

#define TIMER_LIGHT_SLEEP_WAKEUP_TIME_US (3 * 1000 * 1000) // 3 seconds
#define TIMER_DEEP_SLEEPWAKEUP_TIME_US   (5 * 1000 * 1000) // 5 seconds
#define EVT_WAIT_TIME_MS                 (5 * 1000)        // 5 seconds
#define LIGHT_SLEEP_NUM_CYCLES           (5)

const char *USB_SLEEP_TAG = "USB sleep";

/*
    Light sleep task

    - Sets timer as a wakeup source
    - Waits for the EVT_ALLOW_SLEEP event from the CTRL client
    - Enters light sleep -> woken up by a timer
    - Runs LIGHT_SLEEP_NUM_CYCLES times
*/
static void light_sleep_task(void *args)
{
    EventGroupHandle_t test_event_group = (EventGroupHandle_t) args;
    // Configure timer wakeup source
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(TIMER_LIGHT_SLEEP_WAKEUP_TIME_US));
    ESP_LOGI(USB_SLEEP_TAG, "Light sleep timer wakeup source is ready");

    for (int i = 0; i < LIGHT_SLEEP_NUM_CYCLES; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(
            pdTRUE,
            xEventGroupWaitBits(test_event_group, EVT_ALLOW_SLEEP, pdTRUE, pdFALSE, pdMS_TO_TICKS(EVT_WAIT_TIME_MS)),
            "CTRL client did not set event on time to allow light sleep");
        ESP_LOGD(USB_SLEEP_TAG, "Entering light sleep");

        // Get timestamp before entering sleep
        const int64_t t_before_us = esp_timer_get_time();

        // Enter light sleep mode
        TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());

        // Get timestamp after waking up from sleep
        const int64_t t_after_us = esp_timer_get_time();

        // Determine wake up reason
        const uint32_t wakeup_cause = esp_sleep_get_wakeup_causes();
        TEST_ASSERT(wakeup_cause & BIT(ESP_SLEEP_WAKEUP_TIMER));
        TEST_ASSERT_INT_WITHIN_MESSAGE(
            TIMER_LIGHT_SLEEP_WAKEUP_TIME_US / 10,
            TIMER_LIGHT_SLEEP_WAKEUP_TIME_US,
            t_after_us - t_before_us,
            "Unexpected light sleep duration");
        printf("Returned from light sleep, reason: timer, t=%lld ms, slept for %lld ms\n", t_after_us / 1000, (t_after_us - t_before_us) / 1000);
    }

    xEventGroupSetBits(test_event_group, EVT_TEST_FINISH);
    vTaskDelete(NULL);
}

/*
    Deep sleep task

    - Sets timer as a wakeup source
    - Waits for the EVT_ALLOW_SLEEP or EVT_CLIENT_CLOSE event from the CTRL client
    - Enters deep sleep -> woken up by a timer
    - Another test stage is run to check the functionality

*/
static void deep_sleep_task(void *args)
{
    EventGroupHandle_t test_event_group = (EventGroupHandle_t) args;
    // Configure timer wakeup source
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(TIMER_DEEP_SLEEPWAKEUP_TIME_US));
    ESP_LOGI(USB_SLEEP_TAG, "Deep sleep timer wakeup source is ready");

    TEST_ASSERT_EQUAL_MESSAGE(
        pdTRUE,
        xEventGroupWaitBits(test_event_group, EVT_ALLOW_SLEEP | EVT_CLIENT_CLOSE, pdTRUE, pdFALSE, pdMS_TO_TICKS(EVT_WAIT_TIME_MS)),
        "CTRL client did not set event on time to allow deep sleep");
    ESP_LOGI(USB_SLEEP_TAG, "Entering deep sleep");

    /* Enter sleep mode */
    esp_deep_sleep_start();
}

/**
 * @brief Common sleep function for both, light and deep sleep test cases
 *
 * @param[in] sleep_mode: Type of sleep mode to be tested (Deep/Light)
 */
static void usb_host_sleep_common(const esp_sleep_mode_t sleep_mode)
{
    TaskHandle_t ctrl_task_hdl = NULL;
    TaskHandle_t sleep_task_hdl = NULL;
    EventGroupHandle_t test_event_group = NULL;

    test_event_group = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(test_event_group);

    const test_context_t test_ctx = {
        .test_event_group = test_event_group,
        .sleep_mode = sleep_mode,
    };

    if (sleep_mode == ESP_SLEEP_MODE_LIGHT_SLEEP) {
        TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(light_sleep_task, "sleep_task", 4096, (void *)test_event_group, 2, &sleep_task_hdl));
    } else {
        TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(deep_sleep_task, "sleep_task", 4096, (void *)test_event_group, 2, &sleep_task_hdl));
    }
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(ctrl_client_sleep_task, "client_task", 4096, (void *)&test_ctx, 2, &ctrl_task_hdl));
    TEST_ASSERT_NOT_NULL(ctrl_task_hdl);
    TEST_ASSERT_NOT_NULL(sleep_task_hdl);

    xTaskNotifyGive(ctrl_task_hdl);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices free\n");
            break;
        }
    }
    vEventGroupDelete(test_event_group);
}

/*
Test USB Host with light sleep

Purpose:
- Test usage of light sleep mode with USB Host.

Procedure:
    - CTRL client does the USB Host functionality verification
    - SoC enters light sleep
    - Root port suspend is called before entering the light sleep
    - SoC is woken up by a timer
    - Root port resume is called after exiting the light sleep
    - All steps are repeated LIGHT_SLEEP_NUM_CYCLES times
    - Device is expected not to be reset (not to loose address) during light sleep
*/
TEST_CASE("Test USB Host light sleep", "[usb_sleep_modes][light_sleep]")
{
    // Call common function with light sleep mode
    usb_host_sleep_common(ESP_SLEEP_MODE_LIGHT_SLEEP);
}


static void light_sleep_enter_task_error(void *args)
{
    TaskHandle_t main_task = (TaskHandle_t)args;
    // Configure timer wakeup source
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(TIMER_LIGHT_SLEEP_WAKEUP_TIME_US));
    ESP_LOGI(USB_SLEEP_TAG, "Light sleep timer wakeup source is ready");

    // Wait for the device to be connected
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)), "Device was not connected on time");

    // Device is connected, wait for the enumeration
    vTaskDelay(10);

    // Manually disconnect device to put the root port to an unexpected state for the suspend routine before light sleep
    // to induce error state in the light sleep callback
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));

    // Get timestamp before entering sleep
    const int64_t t_before_us = esp_timer_get_time();

    // Immediately enter light sleep mode
    TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());

    // Get timestamp after waking up from sleep
    const int64_t t_after_us = esp_timer_get_time();

    // Determine wake up reason
    const uint32_t wakeup_cause = esp_sleep_get_wakeup_causes();
    TEST_ASSERT(wakeup_cause & BIT(ESP_SLEEP_WAKEUP_TIMER));
    TEST_ASSERT_INT_WITHIN_MESSAGE(
        TIMER_LIGHT_SLEEP_WAKEUP_TIME_US / 10,
        TIMER_LIGHT_SLEEP_WAKEUP_TIME_US,
        t_after_us - t_before_us,
        "Unexpected light sleep duration");
    printf("Returned from light sleep, reason: timer, t=%lld ms, slept for %lld ms\n", t_after_us / 1000, (t_after_us - t_before_us) / 1000);

    // Signalize the main task to finish the test and unblock the host lib task
    xTaskNotifyGive(main_task);
    usb_host_lib_unblock();
    vTaskDelete(NULL);
}

/*
Test USB Host light sleep callbacks error handling

Purpose:
- Test light sleep callbacks error behavior

Procedure:
    - Disconnect the root port before entering light sleep to put the port to an unexpected state for the suspend routine
    - SoC immediately enters light sleep
    - Root port suspend is called before entering the light sleep, but root port is in an unexpected state
    - Callback function returns error, but light sleep shall still proceed as normal
    - SoC is woken up by a timer
    - Root port resume is called after exiting the light sleep, but root port is in an unexpected state
*/
TEST_CASE("Test USB Host enter light sleep error handling", "[usb_sleep_modes][light_sleep]")
{
    TaskHandle_t sleep_task_hdl = NULL;

    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(light_sleep_enter_task_error, "light_sleep_enter_task_error", 4096, xTaskGetCurrentTaskHandle(), 2, &sleep_task_hdl));
    TEST_ASSERT_NOT_NULL(sleep_task_hdl);

    // Handle device connection separately
    uint32_t event_flags_device_conn;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_handle_events(pdMS_TO_TICKS(2000), &event_flags_device_conn));

    xTaskNotifyGive(sleep_task_hdl);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            TEST_FAIL_MESSAGE("No clients for this test");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices free\n");
        }
        if (pdTRUE == ulTaskNotifyTake(pdFALSE, 0)) {
            // Finish the test
            break;
        }
    }
}


/**
 * @brief Deep sleep test case stage 1
 *
 * Just calls the common sleep function, which does the USB Host functional test and enters deep sleep
 */
static void usb_host_deep_sleep_1(void)
{
    // Call common test function with deep sleep mode
    usb_host_sleep_common(ESP_SLEEP_MODE_DEEP_SLEEP);
}

/**
 * @brief Deep sleep test case stage 2
 *
 * Checks reset reason
 * Calls the common sleep function, which does the USB Host functional test and enters deep sleep
 */
static void usb_host_deep_sleep_2(void)
{
    // Get reset reason and check if it's deep sleep reset
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    TEST_ASSERT(reason == RESET_REASON_CORE_DEEP_SLEEP);
    // Call common test function with deep sleep mode
    usb_host_sleep_common(ESP_SLEEP_MODE_DEEP_SLEEP);
}

/**
 * @brief Deep sleep test case stage 3
 *
 * Checks reset reason
 * Handles new device connection and disconnection, does not enter deep sleep anymore
 */
static void usb_host_deep_sleep_3(void)
{
    // Get reset reason and check if it's deep sleep reset
    soc_reset_reason_t reason = esp_rom_get_reset_reason(0);
    TEST_ASSERT(reason == RESET_REASON_CORE_DEEP_SLEEP);

    // End of last stage of the test:
    // Wait for the device to be connected
    vTaskDelay(100);
    // Handle device connection
    usb_host_lib_handle_events(0, NULL);
    // Free the device
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices free\n");
            break;
        }
    }
}

/*
Test USB Host with deep sleep

Purpose:
- Test usage of deep sleep mode with USB Host.

Procedure:
    - CTRL client does the USB Host functionality verification
    - SoC enters deep sleep
    - SoC is woken up by a timer
    - All steps are repeated 2 times
    - Device is expected to be disconnected during deep sleep
*/
TEST_CASE_MULTIPLE_STAGES("Test USB Host deep sleep", "[usb_sleep_modes][deep_sleep]", usb_host_deep_sleep_1, usb_host_deep_sleep_2, usb_host_deep_sleep_3);
