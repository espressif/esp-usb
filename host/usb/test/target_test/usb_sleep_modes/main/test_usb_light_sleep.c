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

#define TIMER_WAKEUP_TIME_US (2 * 1000 * 1000) // 2 seconds
#define NUM_SLEEP_CYCLES 5

const char *LIGHT_SLEEP_TAG = "USB Light sleep";
volatile unsigned enter_sleep_cb_called = 0;
volatile unsigned exit_sleep_cb_called = 0;

/**
 * @brief Callback function for enter light sleep event
 */
static esp_err_t enter_sleep(void *user_arg, void *ext_arg)
{
    ESP_EARLY_LOGI(LIGHT_SLEEP_TAG, "Enter sleep cb");
    (*(volatile unsigned *)user_arg)++;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    vTaskDelay(10);
    return ESP_OK;
}

/**
 * @brief Callback function for exit light sleep event
 */
static esp_err_t exit_sleep(void *user_arg, void *ext_arg)
{
    ESP_EARLY_LOGI(LIGHT_SLEEP_TAG, "Exit sleep cb");
    (*(volatile unsigned *)user_arg)++;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    return ESP_OK;
}

/**
 * @brief Callback configuration for enter light sleep event
 */
static const esp_sleep_event_cb_config_t enter_sleep_cb = {
    .cb = enter_sleep,
    .user_arg = (void *) &enter_sleep_cb_called,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Callback configuration for exit light sleep event
 */
static const esp_sleep_event_cb_config_t exit_sleep_cb = {
    .cb = exit_sleep,
    .user_arg = (void *) &exit_sleep_cb_called,
    .prior = 2,
    .next = NULL,
};

static void light_sleep_task(void *args)
{
    EventGroupHandle_t test_event_group = (EventGroupHandle_t) args;
    // Configure timer wakeup source
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(TIMER_WAKEUP_TIME_US));
    ESP_LOGI(LIGHT_SLEEP_TAG, "timer wakeup source is ready");

    // Register sleep event callbacks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &enter_sleep_cb));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &exit_sleep_cb));

    for (int i = 0; i < NUM_SLEEP_CYCLES; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(
            pdTRUE,
            xEventGroupWaitBits(test_event_group, EVT_ALLOW_SLEEP, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000)),
            "CTRL client did not set event on time to allow sleep");
        ESP_LOGD(LIGHT_SLEEP_TAG, "Entering light sleep");

        /* Get timestamp before entering sleep */
        const int64_t t_before_us = esp_timer_get_time();

        /* Enter sleep mode */
        TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());

        /* Get timestamp after waking up from sleep */
        const int64_t t_after_us = esp_timer_get_time();

        /* Determine wake up reason */
        const uint32_t wakeup_cause = esp_sleep_get_wakeup_causes();
        TEST_ASSERT(wakeup_cause & BIT(ESP_SLEEP_WAKEUP_TIMER));
        printf("Returned from light sleep, reason: timer, t=%lld ms, slept for %lld ms\n", t_after_us / 1000, (t_after_us - t_before_us) / 1000);
    }

    xEventGroupSetBits(test_event_group, EVT_TEST_FINISH);
    vTaskDelete(NULL);
}

/*
Test USB Host with light sleep

Purpose:
- Test usage of light sleep mode with USB Host.

Procedure:
    - TODO
*/
TEST_CASE("Test USB Host light sleep", "[usb_host]")
{
    TaskHandle_t ctrl_task_hdl = NULL;
    TaskHandle_t sleep_task_hdl = NULL;
    EventGroupHandle_t test_event_group = NULL;

    test_event_group = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(test_event_group);

    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(light_sleep_task, "sleep_task", 4096, (void *)test_event_group, 2, &sleep_task_hdl));
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(ctrl_client_task, "ctrl_task", 4096, (void *)test_event_group, 2, &ctrl_task_hdl));
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

    TEST_ASSERT_EQUAL(NUM_SLEEP_CYCLES, enter_sleep_cb_called);
    TEST_ASSERT_EQUAL(NUM_SLEEP_CYCLES, exit_sleep_cb_called);
    vEventGroupDelete(test_event_group);
}
