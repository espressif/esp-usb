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

#if defined(SOC_LIGHT_SLEEP_SUPPORTED) && defined(SOC_DEEP_SLEEP_SUPPORTED)

#define TIMER_LIGHT_SLEEP_WAKEUP_TIME_US (3 * 1000 * 1000) // 3 seconds
#define TIMER_DEEP_SLEEPWAKEUP_TIME_US   (5 * 1000 * 1000) // 5 seconds
#define EVT_WAIT_TIME_MS                 (5 * 1000)        // 5 seconds
#define LIGHT_SLEEP_NUM_CYCLES           (3)

const char *USB_SLEEP_TAG = "USB sleep";

extern void test_setup(void);

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

    EventBits_t evt_bit = xEventGroupWaitBits(test_event_group, EVT_ALLOW_SLEEP | EVT_CLIENT_CLOSE, pdTRUE, pdFALSE, pdMS_TO_TICKS(EVT_WAIT_TIME_MS));
    TEST_ASSERT_MESSAGE((evt_bit & EVT_ALLOW_SLEEP) || (evt_bit & EVT_CLIENT_CLOSE), "CTRL client did not set event on time to allow deep sleep");
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
    test_setup();
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
    test_setup();
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
 * @brief Timestamp struct for measuring added light sleep latency by automatically calling root port suspend and resume
 */
typedef struct {
    int64_t before_suspend;         /**< Time stamp taken before executing auto suspend from light sleep callback */
    int64_t after_suspend;          /**< Time stamp taken after executing auto suspend from light sleep callback */
    int64_t before_resume;          /**< Time stamp taken before executing auto resume from light sleep callback */
    int64_t after_resume;           /**< Time stamp taken after executing auto resume from light sleep callback */
} light_sleep_cb_time_stamp_us_t;

static light_sleep_cb_time_stamp_us_t s_light_sleep_time_stamp = {};

/**
 * @brief Callback function to start measuring enter light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_before_auto_suspend(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->before_suspend = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Callback function to finish measuring enter light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_after_auto_suspend(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->after_suspend = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Callback function to start measuring exit light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_before_auto_resume(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->before_resume = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Callback function to finish measuring exit light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_after_auto_resume(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->after_resume = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Light sleep callback registered before installing usb_host_lib, invoked when entering light sleep (suspending)
 */
static const esp_sleep_event_cb_config_t s_before_auto_suspend_cb = {
    .cb = test_time_before_auto_suspend,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Light sleep callback registered after installing usb_host_lib, invoked when entering light sleep (suspending)
 */
static const esp_sleep_event_cb_config_t s_after_auto_suspend_cb = {
    .cb = test_time_after_auto_suspend,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Light sleep callback registered before installing usb_host_lib, invoked when exiting light sleep (resuming)
 */
static const esp_sleep_event_cb_config_t s_before_auto_resume_cb = {
    .cb = test_time_before_auto_resume,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Light sleep callback registered after installing usb_host_lib, invoked when exiting light sleep (resuming)
 */
static const esp_sleep_event_cb_config_t s_after_auto_resume_cb = {
    .cb = test_time_after_auto_resume,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

static void light_sleep_enter_latency_task(void *args)
{
    // Enable light sleep wake up source
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(TIMER_LIGHT_SLEEP_WAKEUP_TIME_US));
    ESP_LOGI(USB_SLEEP_TAG, "Light sleep timer wakeup source is ready");

    // Wait for device connection
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)), "Device was not connected on time");

    // Wait for device enumeration
    vTaskDelay(20);

    // Init cache, call the DUT function for the first time separately outside of the measuring loop
    TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());
    vTaskDelay(10);

    // Measure enter and exit light sleep latency
    for (int i = 0; i < LIGHT_SLEEP_NUM_CYCLES; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());
        const uint32_t wakeup_cause = esp_sleep_get_wakeup_causes();
        TEST_ASSERT(wakeup_cause & BIT(ESP_SLEEP_WAKEUP_TIMER));

        const int64_t enter_light_sleep_latency = s_light_sleep_time_stamp.after_suspend - s_light_sleep_time_stamp.before_suspend;
        const int64_t exit_light_sleep_latency = s_light_sleep_time_stamp.after_resume - s_light_sleep_time_stamp.before_resume;

        ESP_LOGI(USB_SLEEP_TAG, "Enter light sleep latency: %lld us", enter_light_sleep_latency);
        ESP_LOGI(USB_SLEEP_TAG, "Exit light sleep latency: %lld us", exit_light_sleep_latency);

        // Comparing against fixed threshold, the real latency varies and it's not completely stable even for the same targets
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(200, enter_light_sleep_latency, "Enter light sleep latency too high");
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(200, exit_light_sleep_latency, "Exit light sleep latency too high");

        vTaskDelay(50);
    }

    // Unregister light sleep callbacks registered in test case
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, test_time_before_auto_suspend));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, test_time_after_auto_suspend));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, test_time_before_auto_resume));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, test_time_after_auto_resume));

    // Disconnect the device, finish test
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
    vTaskDelete(NULL);
}

/*
Test light sleep latency

Purpose:
- Measure added latency to enter and exit light sleep caused by automatic suspend and resume of the root port

Procedure:
    - Register additional testing light sleep callbacks which will be called before and after the actual callbacks
      registered in the usb_host_lib
    - Enter light sleep
    - Take time stamps from each callback
    - Measure added latency for enter light sleep and exit light sleep
    - Deregister testing callbacks, teardown
*/
TEST_CASE("Test USB Host enter light sleep latency", "[usb_sleep_modes][light_sleep]")
{
    // Multiple light sleep callbacks with the same callback id (eg SLEEP_EVENT_SW_GOTO_SLEEP) are executed in the same
    // order as they were registered, usb_host_lib registers enter light sleep and exit light sleep callbacks during installation

    // Register 2 callbacks before installing the usb_host_lib, with the same callbacks IDs, as the usb_host_lib is registering
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &s_before_auto_suspend_cb));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &s_before_auto_resume_cb));
    // Install usb_host_lib
    test_setup();
    // Register 2 callbacks after installing the usb_host_lib, with the same callbacks IDs, as the usb_host_lib is registering
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &s_after_auto_suspend_cb));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &s_after_auto_resume_cb));

    TaskHandle_t sleep_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(light_sleep_enter_latency_task, "light_sleep_enter_latency", 4096, NULL, 2, &sleep_task_hdl));
    TEST_ASSERT_NOT_NULL(sleep_task_hdl);

    // Handle device connection separately
    uint32_t event_flags_device_conn;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_handle_events(pdMS_TO_TICKS(2000), &event_flags_device_conn));

    // Notify the worker task, that the device has been connected
    xTaskNotifyGive(sleep_task_hdl);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            TEST_FAIL_MESSAGE("No clients for this test");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices free\n");
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
    test_setup();
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
    test_setup();
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
    test_setup();
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

#endif // SOC_LIGHT_SLEEP_SUPPORTED && SOC_DEEP_SLEEP_SUPPORTED
