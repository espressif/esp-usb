/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
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

const char *USB_HOST_PM_TIMER_TAG = "USB Host PM timer";

#define TEST_PM_TIMER_INTERVAL_MS           500
#define TEST_PM_TIMER_MARGIN_TICKS          5

/**
 * @brief Expect USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND flag to be delivered by the usb host lib
 *
 * @param[in] ticks_to_wait: Block time for the notify take
 */
static inline void check_suspend_flag_delivered(TickType_t ticks_to_wait)
{
    TEST_ASSERT_MESSAGE(
        ulTaskNotifyTakeIndexed(USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND, pdFALSE, ticks_to_wait),
        "Auto suspend flag not generated on time"
    );
}

/**
 * @brief Expect USB_HOST_LIB_EVENT_FLAGS_ALL_FREE flag to be delivered by the usb host lib
 *
 * @param[in] ticks_to_wait: Block time for the notify take
 */
static inline void check_all_dev_gone_flag_delivered(TickType_t ticks_to_wait)
{
    TEST_ASSERT_MESSAGE(
        ulTaskNotifyTakeIndexed(USB_HOST_LIB_EVENT_FLAGS_ALL_FREE, pdFALSE, ticks_to_wait),
        "All devices free flag not generated on time"
    );
}

/**
 * @brief Don't expect USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND flag to be delivered by the usb host lib
 *
 * @param[in] ticks_to_wait: Block time for the notify take
 */
static inline void check_suspend_flag_not_delivered(TickType_t ticks_to_wait)
{
    TEST_ASSERT_FALSE_MESSAGE(
        ulTaskNotifyTakeIndexed(USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND, pdFALSE, ticks_to_wait),
        "Auto suspend flag generated, but not expected"
    );
}

/**
 * @brief Check tick count from timer start until flag delivery
 *
 * @param[in] ticks_expected: Expected tick count
 * @param[in] ticks_actual: Actual tick count
 */
static inline void check_pm_timer_tick_count(uint32_t ticks_expected, uint32_t ticks_actual)
{
    ESP_LOGD(USB_HOST_PM_TIMER_TAG, "Expected ticks: %ld, Actual ticks %ld", pdMS_TO_TICKS(ticks_expected), ticks_actual);
    TEST_ASSERT_INT_WITHIN_MESSAGE(
        TEST_PM_TIMER_MARGIN_TICKS, pdMS_TO_TICKS(ticks_expected), ticks_actual,
        "Timer expired too early"
    );
}

static void pm_timer_measure_task(void *arg)
{
    typedef enum {
        TEST_STAGE_PM_ONE_SHOT_TIMER,
        TEST_STAGE_PM_ONE_SHOT_TIMER_MUL_TIMES,
        TEST_STAGE_PM_ONE_SHOT_NO_DEV,
        TEST_STAGE_PM_PERIODIC_TIMER,
        TEST_STAGE_PM_PERIODIC_TIMER_PORT_SUSPENDED,
        TEST_STAGE_PM_TIMER_SWITCH,
        TEST_STAGE_START_STOP_PM_TIMER,
        TEST_STAGE_RESET_PM_TIMER_BY_HOST_LIB,
        TEST_STAGE_DEV_FREE,
    } test_stage_t;

    TaskHandle_t main_task_hdl = (TaskHandle_t)arg;
    test_stage_t test_stage = TEST_STAGE_PM_ONE_SHOT_TIMER;
    TickType_t timer_start, timer_stop, ticks_to_wait;
    usb_host_lib_info_t lib_info;
    vTaskDelay(100);   // Some time for the device to enumerate

    bool exit_loop = false;
    while (!exit_loop) {
        switch (test_stage) {

        // Start the PM one-shot timer, expect an event flag to be delivered, measure ticks between starting the timer and event delivery
        case TEST_STAGE_PM_ONE_SHOT_TIMER:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_TIMER");

            // Set the PM timer to 500ms and measure tick count
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS));
            timer_start = xTaskGetTickCount();

            // Expect auto suspend event flag
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_delivered(ticks_to_wait);

            // Measure tick count, to check if the timer did not expire too early
            timer_stop = xTaskGetTickCount();
            check_pm_timer_tick_count(TEST_PM_TIMER_INTERVAL_MS, timer_stop - timer_start);

            // Make sure no more event flags are delivered during extended period of time
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 2);
            check_suspend_flag_not_delivered(ticks_to_wait);
            break;

        // Start the PM one-shot timer multiple times, expect one event flag to be delivered and the period of the timer to prolong
        case TEST_STAGE_PM_ONE_SHOT_TIMER_MUL_TIMES:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_TIMER_MUL_TIMES");

            // Set the PM timer to 1000ms and measure tick count
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS * 2));
            timer_start = xTaskGetTickCount();

            // Let the timer run for 500ms
            vTaskDelay(pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS));

            // Set the PM timer again, to 1500ms
            // Since we are setting another timer without the first's timer expiration, the active timer is reset and the period is updated
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS * 3));

            // Expect auto suspend flag to be delivered after 2000ms (1500 + 500)
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 4) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_delivered(ticks_to_wait);

            // Measure tick count, to check if the timer did not expire too early
            timer_stop = xTaskGetTickCount();
            check_pm_timer_tick_count(TEST_PM_TIMER_INTERVAL_MS * 4, timer_stop - timer_start);
            break;

        // Start the PM One-Shot timer with no device, but connect a device immediately, expect the event flag to be delivered
        case TEST_STAGE_PM_ONE_SHOT_NO_DEV:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_NO_DEV");

            // Disconnect the device and check all devs free flag delivery
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
            check_all_dev_gone_flag_delivered(50);

            // Set the PM timer to 500ms and measure tick count
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS));
            timer_start = xTaskGetTickCount();

            // Connect the device
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));

            // Expect auto suspend flag to be delivered after more than timer interval (500ms), because of the timer gets reset during the device connection
            ticks_to_wait = TEST_PM_TIMER_INTERVAL_MS * 2;
            check_suspend_flag_delivered(ticks_to_wait);
            timer_stop = xTaskGetTickCount();

            // Measure the tick count to check if the timer did not expire too early
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "Expected ticks: more than %ld, Actual ticks %ld", pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS), timer_stop - timer_start);
            TEST_ASSERT_GREATER_THAN_INT_MESSAGE(
                pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS) + TEST_PM_TIMER_MARGIN_TICKS,
                timer_stop - timer_start,
                "Auto suspend timer did not reset"
            );
            break;

        // Start the PM periodic timer, expect event flags to be delivered periodically
        case TEST_STAGE_PM_PERIODIC_TIMER:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_PERIODIC_TIMER");

            // Set the PM timer to 500ms and measure tick count
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, TEST_PM_TIMER_INTERVAL_MS));
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS) + TEST_PM_TIMER_MARGIN_TICKS;
            timer_start = xTaskGetTickCount();

            for (int i = 0; i < 3; i++) {
                // Expect auto suspend flag to be delivered after 500ms
                check_suspend_flag_delivered(ticks_to_wait);

                // Measure tick count, to check if the timer did not expire too early
                timer_stop = xTaskGetTickCount();
                check_pm_timer_tick_count(TEST_PM_TIMER_INTERVAL_MS, timer_stop - timer_start);
                timer_start = timer_stop;
            }

            // Stop the periodic timer
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, 0));

            // Make sure no more event flags are delivered during extended period of time
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 2) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_not_delivered(ticks_to_wait);
            break;

        // Start the periodic timer, suspend the root port, verify the event flag is not delivered, resume the root port, verify the event flag is delivered
        case TEST_STAGE_PM_PERIODIC_TIMER_PORT_SUSPENDED:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_PERIODIC_TIMER_PORT_SUSPENDED");

            // Set the PM to 500ms periodic mode and suspend the root port
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, TEST_PM_TIMER_INTERVAL_MS));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
            vTaskDelay(50);     // Yield the main task, to handle root port suspend + Wait for suspend entry delay

            // Verify that the root port is suspended thorough USB Host lib info
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
            TEST_ASSERT_MESSAGE(lib_info.root_port_suspended, "Root port is not suspended, but it's expected to be");

            // Make sure no event is delivered during 3-times the timer period
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 3);
            check_suspend_flag_not_delivered(ticks_to_wait);

            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
            vTaskDelay(50);  // Yield the main task, to handle root port resume + Resume recovery and Resume hold delays

            // Verify that the root port is not suspended thorough USB Host lib info
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
            TEST_ASSERT_FALSE_MESSAGE(lib_info.root_port_suspended, "Root port is suspended, but it's expected not to be");

            // Expect auto suspend flag to be delivered after 500ms after the root port has been resumed
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_delivered(ticks_to_wait);

            // Disable the periodic timer
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, 0));
            break;

        // Start the PM periodic timer and switch to the one shot timer, expect event flag to be delivered just once
        case TEST_STAGE_PM_TIMER_SWITCH:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_TIMER_SWITCH");

            // Set the PM timer to 1000ms and measure tick count
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, TEST_PM_TIMER_INTERVAL_MS * 2));
            timer_start = xTaskGetTickCount();

            // Let the timer run for 500ms
            vTaskDelay(pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS));

            // Switch from periodic to one-shot with different period: 1500ms
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS * 3));

            // Expect auto suspend flag to be delivered after 2000ms (1500 + 500)
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 4) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_delivered(ticks_to_wait);

            // Measure tick count, to check if the timer did not expire too early
            timer_stop = xTaskGetTickCount();
            check_pm_timer_tick_count(TEST_PM_TIMER_INTERVAL_MS * 4, timer_stop - timer_start);

            // Make sure no more event flags are delivered during extended period of time
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 4) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_not_delivered(ticks_to_wait);
            break;

        // Start and stop the active timer, expect no event flag to be delivered
        case TEST_STAGE_START_STOP_PM_TIMER:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_START_STOP_PM_TIMER");

            // Set the PM timer to 500ms
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS));

            // Let the timer run for 250ms and stop the timer
            vTaskDelay(pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS / 2));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, 0));

            // Make sure no more event flags are delivered during extended period of time
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 2);
            check_suspend_flag_not_delivered(ticks_to_wait);
            break;

        // Start the PM timer and simulate some workload on the usb host lib, to induce reset of the running PM timer internally by the usb host lib
        case TEST_STAGE_RESET_PM_TIMER_BY_HOST_LIB:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_RESET_PM_TIMER_BY_HOST_LIB");

            // Set the PM timer to 500ms and measure tick count
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_PM_TIMER_INTERVAL_MS));
            timer_start = xTaskGetTickCount();

            // Let the timer run for 400ms
            vTaskDelay(pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 0.8));

            // Induce a usb host lib workload by root port suspend
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

            // Make sure no event is delivered during 1.6-times the timer period
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS * 0.8);
            check_suspend_flag_not_delivered(ticks_to_wait);

            // Verify that the root port is suspended thorough USB Host lib info
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
            TEST_ASSERT_MESSAGE(lib_info.root_port_suspended, "Root port is not suspended, but it's expected to be");

            // Induce a the workload again by resuming the port and let the host lib to finish resuming the port for 50ms
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
            vTaskDelay(50);  // Yield the main task, to handle root port resume + Resume recovery and Resume hold delays

            // Verify that the root port is not suspended thorough USB Host lib info
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
            TEST_ASSERT_FALSE_MESSAGE(lib_info.root_port_suspended, "Root port is suspended, but it's expected not to be");

            // Expect auto suspend flag to be delivered after 500ms after the root port has been resumed
            ticks_to_wait = pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS) + TEST_PM_TIMER_MARGIN_TICKS;
            check_suspend_flag_delivered(ticks_to_wait);
            timer_stop = xTaskGetTickCount();

            // Measure the tick count to check if the timer did not expire too early
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "Expected ticks: more than %ld, Actual ticks %ld", pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS), timer_stop - timer_start);
            TEST_ASSERT_GREATER_THAN_INT_MESSAGE(
                pdMS_TO_TICKS(TEST_PM_TIMER_INTERVAL_MS) + TEST_PM_TIMER_MARGIN_TICKS,
                timer_stop - timer_start,
                "Auto suspend timer did not reset"
            );
            break;

        // Free the device and finish the test
        case TEST_STAGE_DEV_FREE:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_DEV_FREE");

            // Stop handling events in the main task after the device is gone
            xTaskNotifyGiveIndexed(main_task_hdl, USB_HOST_LIB_EVENT_FLAGS_ALL_FREE);
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
            exit_loop = true;
            check_all_dev_gone_flag_delivered(50);

            break;
        default:
            TEST_FAIL_MESSAGE("Incorrect test stage");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));     // Yield to the main task after each stage, to handle the event flags delivery
        test_stage++;
    }
    // Cleanup
    vTaskDelete(NULL);
}

/*
Test USB Host auto Suspend timer event flags delivery

Purpose:
- Test the PM Timer event flags delivery upon timer settings
- Measure time between setting of the timer and event flag delivery
- Test USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND is correctly delivered

Procedure:
    - Install USB Host Library
    - Create timer cancellation measure task
    - Test the following cases:
        - Set the One-Shot PM Timer, expect correct event flag delivery,  measure (in ticks) time between starting of the timer and event flag delivery
        - Set the One-Shot PM Timer with no device connected, but connect a device immediately, expect correct event flag delivery
        - Set the One-Shot PM Timer multiple times without timer expiration, make sure the total timer period prolongs (timer resets)
        - Set the periodic PM TImer, expect correct event flag to be delivered repeatedly
        - Set the periodic PM Timer, but switch to the One-Shot Timer, expect correct flag delivery just once
        - Set and Stop the One-Shot PM Timer, expect no event flag delivery
        - Set the One-Shot PM Timer, induce some workload for the usb host lib to reset the timer internally, expect and measure event flag delivery
    - Turn off the root port, teardown
*/
TEST_CASE("Test USB Host auto suspend timer event flags delivery (no client)", "[usb_host][low_speed][full_speed][high_speed]")
{
    // Create control task
    TaskHandle_t pm_timer_measure_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(pm_timer_measure_task, "pm timer measure", 1024 * 2, xTaskGetCurrentTaskHandle(), 4, &pm_timer_measure_task_hdl));
    TEST_ASSERT_NOT_NULL(pm_timer_measure_task_hdl);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            xTaskNotifyGiveIndexed(pm_timer_measure_task_hdl, USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND);
            printf("Auto suspend event\n");
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            xTaskNotifyGiveIndexed(pm_timer_measure_task_hdl, USB_HOST_LIB_EVENT_FLAGS_ALL_FREE);
            printf("Device gone\n");

            if (ulTaskNotifyTakeIndexed(USB_HOST_LIB_EVENT_FLAGS_ALL_FREE, pdFALSE, 0)) {
                printf("Finish the test\n");
                break;
            }
        }
    }
}

#define TEST_AUTO_PM_TIMER_MS           250
#define TEST_HOST_LIB_EVENT_WAIT_MS     1000
#define TEST_DEV_GONE_WAIT_MS           50

/**
 * @brief Expect USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND flag to be delivered by the usb host lib
 */
static void check_device_gone(void)
{
    TEST_ASSERT_MESSAGE(
        ulTaskNotifyTake(pdFALSE, TEST_DEV_GONE_WAIT_MS),
        "Device gone flag not generated on time"
    );
}

static void pm_timer_cancel_task(void *arg)
{
    typedef enum {
        TEST_STAGE_INIT_PORT_POWER_OFF,
        TEST_STAGE_PM_ONE_SHOT_TIMER_NO_DEVICE,
        TEST_STAGE_PM_PERIODIC_TIMER_NO_DEVICE,
        TEST_STAGE_PM_ONE_SHOT_TIMER_DEVICE_DISCONNECT,
        TEST_STAGE_PM_ONE_SHOT_TIMER_TOGGLE_DEV,
        TEST_STAGE_PM_ONE_SHOT_TIMER_PORT_SUSPENDED,
        TEST_STAGE_DEV_FREE,
    } test_stage_t;

    TaskHandle_t main_task_hdl = (TaskHandle_t)arg;
    test_stage_t test_stage = TEST_STAGE_INIT_PORT_POWER_OFF;
    usb_host_lib_info_t lib_info;
    vTaskDelay(100);   // Some time for the device to enumerate

    bool exit_loop = false;
    while (!exit_loop) {
        switch (test_stage) {

        // Initially power off the root port
        case TEST_STAGE_INIT_PORT_POWER_OFF:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_INIT_PORT_POWER_OFF");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));

            // Check that the device is gone and keep the device disconnected for a while
            check_device_gone();
            vTaskDelay(50);
            break;

        // Set the PM One-Shot Timer with no device connected
        case TEST_STAGE_PM_ONE_SHOT_TIMER_NO_DEVICE:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_TIMER_NO_DEVICE");

            // Set auto suspend timer and wait for 1000ms, to make sure no usb host lib event is delivered
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_AUTO_PM_TIMER_MS));
            vTaskDelay(pdMS_TO_TICKS(TEST_HOST_LIB_EVENT_WAIT_MS));
            break;

        // Set the PM Periodic Timer, with no device connected
        case TEST_STAGE_PM_PERIODIC_TIMER_NO_DEVICE:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_PERIODIC_TIMER_NO_DEVICE");

            // Set auto suspend timer and wait for 1000ms, to make sure no usb host lib event is delivered
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, TEST_AUTO_PM_TIMER_MS));
            vTaskDelay(pdMS_TO_TICKS(TEST_HOST_LIB_EVENT_WAIT_MS));

            // Disable the periodic auto suspend timer
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, 0));

            // Connect the device back, yield to the main task to handle port connection
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));
            vTaskDelay(50);
            break;

        // Set the PM One-Shot Timer with a device connected, but power off the root port immediately
        case TEST_STAGE_PM_ONE_SHOT_TIMER_DEVICE_DISCONNECT:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_TIMER_DEVICE_DISCONNECT");

            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_AUTO_PM_TIMER_MS));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));

            // Check that the device is gone and wait for 1000ms to make sure no usb hot lib event is delivered
            check_device_gone();
            vTaskDelay(pdMS_TO_TICKS(TEST_HOST_LIB_EVENT_WAIT_MS));

            // Connect the device back, yield to the main task to handle port connection
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));
            vTaskDelay(50);
            break;

        // Set the PM One-Shot Timer with a device connected, but toggle the root port power immediately
        case TEST_STAGE_PM_ONE_SHOT_TIMER_TOGGLE_DEV:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_TIMER_TOGGLE_DEV");

            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_AUTO_PM_TIMER_MS));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));

            // Check that the device is gone and keep the device disconnected for a while
            check_device_gone();
            vTaskDelay(50);

            // Connect the device back and wait for 1000ms to make sure no usb host lib event is delivered
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));
            vTaskDelay(pdMS_TO_TICKS(TEST_HOST_LIB_EVENT_WAIT_MS));
            break;

        // Set the PM One-Shot timer and suspend the root port immediately
        case TEST_STAGE_PM_ONE_SHOT_TIMER_PORT_SUSPENDED:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_PM_ONE_SHOT_TIMER_PORT_SUSPENDED");

            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_ONE_SHOT, TEST_AUTO_PM_TIMER_MS));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
            vTaskDelay(50);     // Yield the main task, to handle root port suspend + Wait for suspend entry delay

            // Check if the root port is suspended using usb host lib info
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
            TEST_ASSERT_MESSAGE(lib_info.root_port_suspended, "Root port is not suspended, but it's expected to be");

            // Make sure no usb host lib event is delivered (due to the root port being suspended)
            vTaskDelay(pdMS_TO_TICKS(TEST_HOST_LIB_EVENT_WAIT_MS));
            break;

        // Power the root port off, disconnect the device and finish the test
        case TEST_STAGE_DEV_FREE:
            ESP_LOGD(USB_HOST_PM_TIMER_TAG, "TEST_STAGE_DEV_FREE");

            // Stop handling events in the main task after the device is gone
            xTaskNotifyGive(main_task_hdl);
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
            exit_loop = true;
            check_device_gone();
            break;
        default:
            TEST_FAIL_MESSAGE("Incorrect test stage");
            break;
        }
        test_stage++;
    }
    // Cleanup
    vTaskDelete(NULL);
}

/*
Test USB Host auto Suspend timer cancellation

Purpose:
- Test the PM Timer cancellation upon device disconnection or reconnection
- Test USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND not incorrectly delivered

Procedure:
    - Install USB Host Library
    - Create timer cancellation task
    - Test the following cases:
        - Set the PM Timer with no device connected
        - Set the PM Timer with a device connected, but disconnect the device immediately
        - Set the PM Timer with a device connected, but reconnect the device immediately
    - During the whole test run, expect that no USB_HOST_LIB event is generated
    - Turn off the root port, teardown
*/
TEST_CASE("Test USB Host auto suspend timer cancellation (no client)", "[usb_host][low_speed][full_speed][high_speed]")
{
    // Create control task
    TaskHandle_t pm_timer_cancel_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(pm_timer_cancel_task, "pm timer cancel", 1024 * 2, xTaskGetCurrentTaskHandle(), 4, &pm_timer_cancel_task_hdl));
    TEST_ASSERT_NOT_NULL(pm_timer_cancel_task_hdl);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            TEST_FAIL_MESSAGE("Auto suspend event flag generated unexpectedly");
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            xTaskNotifyGive(pm_timer_cancel_task_hdl);
            printf("Device gone\n");

            if (ulTaskNotifyTake(pdFALSE, 0)) {
                printf("Finish the test\n");
                break;
            }
        }
    }
}
