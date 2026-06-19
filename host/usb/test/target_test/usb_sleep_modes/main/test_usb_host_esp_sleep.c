/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
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

static char err_msg_buf[128];
const char *USB_SLEEP_TAG = "USB sleep";

extern void test_setup(void);

static void light_sleep_enter_catch_impl(const char *file, int line)
{
    const esp_err_t light_sleep_err = esp_light_sleep_start();
    if (light_sleep_err == ESP_ERR_SLEEP_REJECT) {
        // Enter light sleep might be rejected, due to the other esp core being in a critical section
        // Let the usb host lib handle current events and try to enter the light sleep again
        ESP_LOGI(USB_SLEEP_TAG, "Light sleep rejected, re-entering light sleep");
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Second light sleep entry error at %s:%d\n", file, line);
        taskYIELD();
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, esp_light_sleep_start(), err_msg_buf);
    } else if (light_sleep_err != ESP_OK) {
        // Other error occurred
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Light sleep entry error at %s:%d\n", file, line);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, light_sleep_err, err_msg_buf);
    } else {
        // pass
    }
}
/**
 * @brief Enter light sleep and catch sleep rejected error
 */
#define light_sleep_enter_catch() light_sleep_enter_catch_impl(__FILE__, __LINE__)

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
        light_sleep_enter_catch();

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

        // Manually resume the auto-suspended root port
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
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

/**
 * TEST CASE: Test USB Host enter light sleep error handling
 */

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
    light_sleep_enter_catch();

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

    // Let the usb_host_lib handle pending events after exiting light sleep
    vTaskDelay(100);

    // Make sure the port disconnection finished successfully and there are no devices connected to the port
    usb_host_lib_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&info));
    TEST_ASSERT_MESSAGE(info.num_devices == 0, "No device shall be connected to the root port");

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
 * TEST CASE: Test USB Host enter light sleep latency
 */


/**
 * @brief Timestamp struct for measuring added light sleep latency by automatically calling root port suspend
 */
typedef struct {
    int64_t before_enter_sleep;         /**< Time stamp taken before executing usb_host_lib's enter light sleep callback */
    int64_t after_enter_sleep;          /**< Time stamp taken after executing usb_host_lib's enter light sleep callback */
    int64_t before_exit_sleep;          /**< Time stamp taken before executing usb_host_lib's exit light sleep callback */
    int64_t after_exit_sleep;           /**< Time stamp taken after executing usb_host_lib's exit light sleep callback */
} light_sleep_cb_time_stamp_us_t;

static light_sleep_cb_time_stamp_us_t s_light_sleep_time_stamp = {};

/**
 * @brief Callback function to start measuring enter light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_before_enter_light_sleep(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->before_enter_sleep = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Callback function to finish measuring enter light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_after_enter_light_sleep(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->after_enter_sleep = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Callback function to start measuring exit light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_before_exit_light_sleep(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->before_exit_sleep = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Callback function to finish measuring exit light sleep latency
 * @param[in] user_arg pointer to time stamp structure
 * @param[in] ext_arg External argument, not used
 * @return ESP_OK
 */
static esp_err_t test_time_after_exit_light_sleep(void *user_arg, void *ext_arg)
{
    light_sleep_cb_time_stamp_us_t *time_stamp = (light_sleep_cb_time_stamp_us_t *)user_arg;
    time_stamp->after_exit_sleep = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief Light sleep callback registered before installing usb_host_lib to take timestamp before the usb_host_lib's
 * enter light sleep callbacks fires
 */
static const esp_sleep_event_cb_config_t s_before_enter_light_sleep_cb = {
    .cb = test_time_before_enter_light_sleep,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Light sleep callback registered after installing usb_host_lib to take timestamp after the usb_host_lib's
 * enter light sleep callbacks fires
 */
static const esp_sleep_event_cb_config_t s_after_enter_light_sleep_cb = {
    .cb = test_time_after_enter_light_sleep,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Light sleep callback registered before installing usb_host_lib to take timestamp before the usb_host_lib's
 * exit light sleep callbacks fires
 */
static const esp_sleep_event_cb_config_t s_before_exit_light_sleep_cb = {
    .cb = test_time_before_exit_light_sleep,
    .user_arg = (void *) &s_light_sleep_time_stamp,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Light sleep callback registered after installing usb_host_lib to take timestamp after the usb_host_lib's
 * exit light sleep callbacks fires
 */
static const esp_sleep_event_cb_config_t s_after_exit_light_sleep_cb = {
    .cb = test_time_after_exit_light_sleep,
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
    light_sleep_enter_catch();

    // Resume the root port manually and wait for the resume sequence
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    vTaskDelay(50);

    // Measure enter and exit light sleep latency
    for (int i = 0; i < LIGHT_SLEEP_NUM_CYCLES; i++) {
        light_sleep_enter_catch();
        const uint32_t wakeup_cause = esp_sleep_get_wakeup_causes();
        TEST_ASSERT(wakeup_cause & BIT(ESP_SLEEP_WAKEUP_TIMER));

        const int64_t enter_light_sleep_latency = s_light_sleep_time_stamp.after_enter_sleep - s_light_sleep_time_stamp.before_enter_sleep;
        const int64_t exit_light_sleep_latency = s_light_sleep_time_stamp.after_exit_sleep - s_light_sleep_time_stamp.before_exit_sleep;

        ESP_LOGI(USB_SLEEP_TAG, "Enter light sleep latency: %lld us", enter_light_sleep_latency);
        ESP_LOGI(USB_SLEEP_TAG, "Exit light sleep latency: %lld us", exit_light_sleep_latency);

        // Comparing against fixed threshold, the real latency varies and it's not completely stable even for the same targets
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(200, enter_light_sleep_latency, "Enter light sleep latency too high");
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(200, exit_light_sleep_latency, "Exit light sleep latency too high");

        // Manually resume the root port and wait for the resume procedure
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        vTaskDelay(50);
    }

    // Unregister light sleep callbacks registered in test case
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, test_time_before_enter_light_sleep));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, test_time_after_enter_light_sleep));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, test_time_before_exit_light_sleep));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, test_time_after_exit_light_sleep));

    // Disconnect the device, finish test
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
    vTaskDelete(NULL);
}

/*
Test light sleep latency

Purpose:
- Measure added latency to enter the light sleep caused by automatic suspend of the root port

Procedure:
    - Register additional testing light sleep callback which will be called before and after the actual callback
      registered in the usb_host_lib
    - Enter light sleep
    - Take time stamps from each callback
    - Measure added latency for enter light sleep
    - Deregister testing callbacks, teardown
*/
TEST_CASE("Test USB Host enter light sleep latency", "[usb_sleep_modes][light_sleep]")
{
    // Multiple light sleep callbacks with the same callback id (eg SLEEP_EVENT_SW_GOTO_SLEEP) are executed in the same
    // order as they were registered, usb_host_lib registers enter light sleep and exit light sleep callbacks during installation

    // Register 2 callbacks before installing the usb_host_lib, with the same callbacks IDs, as the usb_host_lib is registering
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &s_before_enter_light_sleep_cb));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &s_before_exit_light_sleep_cb));
    // Install usb_host_lib
    test_setup();
    // Register 2 callbacks after installing the usb_host_lib, with the same callbacks IDs, as the usb_host_lib is registering
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &s_after_enter_light_sleep_cb));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &s_after_exit_light_sleep_cb));

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
 * TEST CASE: Test USB Host light sleep events delivery
 */

static QueueHandle_t notif_queue = NULL;
static void notif_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(USB_SLEEP_TAG, "Client event -> New device with address %d", event_msg->new_dev.address);
        uint8_t *new_device_address = (uint8_t *)arg;
        *new_device_address = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ESP_LOGI(USB_SLEEP_TAG, "Client event -> Device suspended");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ESP_LOGI(USB_SLEEP_TAG, "Client event -> Device resumed");
        break;
    default:
        return;
    }

    if (notif_queue) {
        xQueueSend(notif_queue, &event_msg->event, 0);
    }
}

static void notif_client_task(void *args)
{
    uint8_t new_device_address = 0;
    usb_host_client_handle_t client_hdl = NULL;
    usb_device_handle_t dev_hdl = NULL;

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = notif_client_event_cb,
            .callback_arg = &new_device_address,
        },
    };

    // Register client
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &client_hdl));
    // Handle new device connection
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_handle_events(client_hdl, portMAX_DELAY));
    // Device is connected, check the new device address
    TEST_ASSERT_MESSAGE(new_device_address, "New device address must not be 0");
    // Open the device, to start getting client events
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client_hdl, new_device_address, &dev_hdl));

    // Handle client events
    while (!ulTaskNotifyTake(pdTRUE, 0)) {
        usb_host_client_handle_events(client_hdl, 100);
    }

    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client_hdl, dev_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(client_hdl));
    vTaskDelete(NULL);
}

static void usb_host_task(void *args)
{
    TaskHandle_t main_task_hdl = (TaskHandle_t)args;
    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices freed\n");
            break;
        }
    }

    // Cleanup, notify the main task
    xTaskNotifyGive(main_task_hdl);
    vTaskDelete(NULL);
}

/*
Test Suspend events delivery using automatic suspend from light sleep callback

Purpose:
- Test light sleep callback automatic suspend, vs manual suspend

Procedure:
    - Install USB Host Library, register MSC client, open a device and start handling client events
    - Enter light sleep with root port resumed -> Expect suspend event (auto suspend from light sleep callback)
    - Enter light sleep with root port auto-suspended -> Expect NO suspend event (already auto-suspended)
    - Manually resume and suspend the root port
    - Enter light sleep with root port manually suspended -> Expect NO suspend event (already manually suspended)
    - Stop handling client events, deregister the client, free all devices, uninstall USB Host library
*/
TEST_CASE("Test USB Host light sleep events delivery", "[usb_sleep_modes][low_speed][full_speed][high_speed]")
{
    test_setup();
    TaskHandle_t client_task_hdl = NULL, usb_host_task_hdl = NULL;
    usb_host_client_event_t client_event;

    notif_queue = xQueueCreate(sizeof(usb_host_client_event_t), 5);
    TEST_ASSERT_NOT_NULL(notif_queue);

    // Create client task
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(notif_client_task, "notif_client_task", 4096, NULL, 2, &client_task_hdl));
    TEST_ASSERT_NOT_NULL(client_task_hdl);

    // Create usb host lib task
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(usb_host_task, "usb_host_task", 4096, xTaskGetCurrentTaskHandle(), 2, &usb_host_task_hdl));
    TEST_ASSERT_NOT_NULL(usb_host_task_hdl);

    // Expect new device event
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xQueueReceive(notif_queue, &client_event, pdMS_TO_TICKS(1000)), "Event not delivered");
    TEST_ASSERT_EQUAL_MESSAGE(client_event, USB_HOST_CLIENT_EVENT_NEW_DEV, "Unexpected event delivered");

    // Enable light sleep wake up source
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(TIMER_LIGHT_SLEEP_WAKEUP_TIME_US));
    ESP_LOGI(USB_SLEEP_TAG, "Light sleep timer wakeup source is ready");

    /* Test case 1: Light sleep callback suspend event delivery */
    // Enter light sleep with root port resumed and expect suspend event (auto suspend by light sleep)
    light_sleep_enter_catch();
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xQueueReceive(notif_queue, &client_event, pdMS_TO_TICKS(1000)), "Suspend event not delivered");
    TEST_ASSERT_EQUAL_MESSAGE(client_event, USB_HOST_CLIENT_EVENT_DEV_SUSPENDED, "Unexpected event delivered");

    /* Test case 2: Light sleep callback with root port auto-suspended */
    // Enter light sleep with root port automatically suspended and expect no event
    // Light sleep shall not deliver suspend event if the root port was not suspended by the light sleep callback
    light_sleep_enter_catch();
    TEST_ASSERT_EQUAL_MESSAGE(pdFALSE, xQueueReceive(notif_queue, &client_event, pdMS_TO_TICKS(1000)), "An event delivered, but none was expected");

    // Manually resume and suspend the root port (don't use light sleep callback to suspend the port)
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xQueueReceive(notif_queue, &client_event, pdMS_TO_TICKS(1000)), "Resume event not delivered");
    TEST_ASSERT_EQUAL_MESSAGE(client_event, USB_HOST_CLIENT_EVENT_DEV_RESUMED, "Unexpected event delivered");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xQueueReceive(notif_queue, &client_event, pdMS_TO_TICKS(1000)), "Suspend event not delivered");
    TEST_ASSERT_EQUAL_MESSAGE(client_event, USB_HOST_CLIENT_EVENT_DEV_SUSPENDED, "Unexpected event delivered");

    /* Test case 3: Light sleep callback with root port manually suspended */
    // Enter light sleep with root port manually suspended and expect no event
    light_sleep_enter_catch();
    TEST_ASSERT_EQUAL_MESSAGE(pdFALSE, xQueueReceive(notif_queue, &client_event, pdMS_TO_TICKS(1000)), "An event delivered, but none was expected");

    // Stop the client task, deregister client and release devices
    xTaskNotifyGive(client_task_hdl);
    // Wait until the host lib task finishes
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)), "usb host lib task did not finish on time");
    vQueueDelete(notif_queue);
}

#endif // SOC_LIGHT_SLEEP_SUPPORTED && SOC_DEEP_SLEEP_SUPPORTED
