/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "remote_wake_client.h"
#include "usb/usb_host.h"
#include "unity.h"

/*
Implementation of remote wakeup client used for USB Host tests

- The remote wakeup client will:
    - Register itself as a client
    - Receive USB_HOST_CLIENT_EVENT_NEW_DEV event message, and open the device
    - Start handling client events
    - Wait for a request from main task to:
        - read and check current status of the remote wakeup feature from the device
        - enable remote wakeup feature on the device
        - read and check current status of the remote wakeup feature from the device
        - disable remote wakeup feature on the device
        - read and check current status of the remote wakeup feature from the device
    - Close the device, deregister client
*/

const char *REMOTE_WAKE_CLIENT_TAG = "Remote wakeup Client";

#define CLIENT_NUM_EVENT_MSG        5

/**
 * @brief Test stages of this test
 */
typedef enum {
    TEST_STAGE_WAIT_CONN,                           /**< Wait for device connection */
    TEST_STAGE_DEV_OPEN,                            /**< Open device */
    TEST_STAGE_REMOTE_WAKE_SETUP,                   /**< Setup remote wakeup (enable, disable, check) */
    TEST_STAGE_DEV_CLOSE,                           /**< Close device */
} test_stage_t;

/**
 * @brief Remote wakeup client object
 */
typedef struct {
    remote_wake_client_test_param_t test_param;     /**< Test params for the main task */
    uint8_t dev_addr;                               /**< Connected device address */
    usb_device_handle_t dev_hdl;                    /**< Connected device handle */
    usb_host_client_handle_t client_hdl;            /**< Remote wake client handle */
    test_stage_t cur_stage;                         /**< Current test stage from test_stage_t */
    test_stage_t next_stage;                        /**< Next test stage from test_stage_t */
} remote_wake_client_obj_t;

static remote_wake_client_obj_t *s_remote_wake_obj = NULL;

static void remote_wake_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    remote_wake_client_obj_t *remote_wake_obj = (remote_wake_client_obj_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        printf("\t-> New device\n");
        TEST_ASSERT_EQUAL(TEST_STAGE_WAIT_CONN, remote_wake_obj->cur_stage);
        remote_wake_obj->next_stage = TEST_STAGE_DEV_OPEN;
        remote_wake_obj->dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        xSemaphoreGive(s_remote_wake_obj->test_param.dev_ready_smp);
        printf("\t-> Device suspended\n");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        xSemaphoreGive(s_remote_wake_obj->test_param.dev_ready_smp);
        printf("\t-> Device resumed\n");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        printf("\t-> Device gone\n");
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized client event");
        break;
    }
}

void remote_wake_client_async_task(void *arg)
{
    remote_wake_client_obj_t remote_wake_obj;
    // Initialize test params
    memcpy(&remote_wake_obj.test_param, arg, sizeof(remote_wake_client_test_param_t));
    // Initialize client variables
    remote_wake_obj.client_hdl = NULL;
    remote_wake_obj.dev_hdl = NULL;
    // Initialize test stage
    remote_wake_obj.cur_stage = TEST_STAGE_WAIT_CONN;
    remote_wake_obj.next_stage = TEST_STAGE_WAIT_CONN;
    remote_wake_obj.dev_addr = 0;

    // Register client
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = remote_wake_client_event_cb,
            .callback_arg = (void *) &remote_wake_obj,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &remote_wake_obj.client_hdl));
    s_remote_wake_obj = &remote_wake_obj;

    // Wait to be started by main thread
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)), "Main task did not start on time");
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Starting");

    // Handle device enumeration separately, wait for 1000ms for the device to be enumerated
    // Catch an error in case the device is not enumerated correctly
    esp_err_t enum_ret = usb_host_client_handle_events(remote_wake_obj.client_hdl, pdMS_TO_TICKS(2000));
    TEST_ASSERT_EQUAL_MESSAGE(TEST_STAGE_DEV_OPEN, remote_wake_obj.next_stage, "USB_HOST_CLIENT_EVENT_NEW_DEV not generated on time");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, enum_ret, "Client handle events timed out");

    bool exit_loop = false;
    bool skip_event_handling = true;    // Skip first event handling (we have handled the new device event separately)
    while (!exit_loop) {
        if (!skip_event_handling) {
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_handle_events(remote_wake_obj.client_hdl, portMAX_DELAY));
        }
        skip_event_handling = false;
        if (remote_wake_obj.cur_stage == remote_wake_obj.next_stage) {
            continue;
        }
        remote_wake_obj.cur_stage = remote_wake_obj.next_stage;

        switch (remote_wake_obj.next_stage) {
        case TEST_STAGE_DEV_OPEN: {
            ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Open");
            // Open the device
            TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, usb_host_device_open(remote_wake_obj.client_hdl, remote_wake_obj.dev_addr, &remote_wake_obj.dev_hdl), "Failed to open the device");

            remote_wake_obj.next_stage = TEST_STAGE_REMOTE_WAKE_SETUP;
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_REMOTE_WAKE_SETUP: {
            ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Device ready");
            // Give semaphore signalizing that the device has been opened
            xSemaphoreGive(remote_wake_obj.test_param.dev_ready_smp);
            break;
        }
        case TEST_STAGE_DEV_CLOSE: {
            ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Close");
            vTaskDelay(10); // Give USB Host Lib some time to process all transfers
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(remote_wake_obj.client_hdl, remote_wake_obj.dev_hdl));
            exit_loop = true;
            break;
        }
        default:
            abort();
            break;
        }
    }
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(remote_wake_obj.client_hdl));
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Done");
    vTaskDelete(NULL);
}

void test_remote_wake_finish(void)
{
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "finish test");
    // Go to next stage
    s_remote_wake_obj->next_stage = TEST_STAGE_DEV_CLOSE;
    ESP_ERROR_CHECK(usb_host_client_unblock(s_remote_wake_obj->client_hdl));
}

/**
 * Following functions must not be called from the usb host client task, because a control transfer is sent by those functions
 */

void test_remote_wake_enable(bool has_remote_wake)
{
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Remote wake enable");

    esp_err_t ret = usb_host_device_remote_wakeup_enable(s_remote_wake_obj->client_hdl, s_remote_wake_obj->dev_hdl, true);
    TEST_ASSERT_EQUAL(ret, ((has_remote_wake) ? (ESP_OK) : (ESP_ERR_NOT_ALLOWED)));
    xSemaphoreGive(s_remote_wake_obj->test_param.dev_ready_smp);
}

void test_remote_wake_disable(bool has_remote_wake)
{
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Remote wake disable");

    esp_err_t ret = usb_host_device_remote_wakeup_enable(s_remote_wake_obj->client_hdl, s_remote_wake_obj->dev_hdl, false);
    TEST_ASSERT_EQUAL(ret, ((has_remote_wake) ? (ESP_OK) : (ESP_ERR_NOT_ALLOWED)));
    xSemaphoreGive(s_remote_wake_obj->test_param.dev_ready_smp);
}

void test_remote_wake_check(bool expected_current_remote_wake, bool has_remote_wake)
{
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Remote wake check");

    bool remote_wake_enabled = false;
    esp_err_t ret = usb_host_device_remote_wakeup_check(s_remote_wake_obj->client_hdl, s_remote_wake_obj->dev_hdl, &remote_wake_enabled);
    TEST_ASSERT_EQUAL_MESSAGE(ret, ((has_remote_wake) ? (ESP_OK) : (ESP_ERR_NOT_ALLOWED)),
                              "Unexpected remote wakeup check result");
    TEST_ASSERT_EQUAL_MESSAGE(expected_current_remote_wake, remote_wake_enabled, "Expected remote wakeup status not equal to the current status");
    xSemaphoreGive(s_remote_wake_obj->test_param.dev_ready_smp);
}
