/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "ctrl_client.h"
#include "usb/usb_host.h"
#include "unity.h"

#ifdef REMOTE_WAKE_HAL_SUPPORTED
/*
Implementation of remote wakeup client used for USB Host tests

- The remote wakeup client will:
    - Register itself as a client
    - Receive USB_HOST_CLIENT_EVENT_NEW_DEV event message, and open the device
    - Start handling client events
    - Check if the connected device supports remote wakeup, if not close device and finish test
    - If the device supports the remote wakeup, enable the feature on the device and check if it is enabled
    - Suspend the root port, expect suspend client event and signalize remote wakeup from the device to the host
    - Expect the resume client event
    - Disable the remote wakeup on the device and check if it is disabled
    - Close the device, deregister client
*/

const char *REMOTE_WAKE_CLIENT_TAG = "Remote wakeup Client";

#define CLIENT_NUM_EVENT_MSG        5
#define MAX_TRANSFER_BYTES          256
#define EVENT_NEW_DEV_TIMEOUT_MS    1000    // To test that device shows up on time
#define CLIENT_PROCESS_TIMEOUT_MS   5000    // Mainly to make the test fail upon not detected remote wakeup signaling

/**
 * @brief Test stages of this test
 */
typedef enum {
    TEST_STAGE_WAIT_CONN,                           /**< Wait for device connection */
    TEST_STAGE_DEV_OPEN,                            /**< Open device */
    TEST_STAGE_CTRL_XFER_WAIT,                      /**< Wait until the ctrl transfer is finished */
    TEST_STAGE_REMOTE_WAKE_ENABLE,                  /**< Enable remote wakeup */
    TEST_STAGE_REMOTE_WAKE_DISABLE,                 /**< Disable remote wakeup */
    TEST_STAGE_REMOTE_WAKE_CHECK,                   /**< Check current status of remote wakeup on the device */
    TEST_STAGE_SUSPEND,                             /**< Suspend the root port */
    TEST_STAGE_DO_REMOTE_WAKE,                      /**< Initiate remote wakeup on the device (button press on keyboard) */
    TEST_STAGE_DEV_CLOSE,                           /**< Close device */
} test_stage_t;

/**
 * @brief Remote wakeup client object
 */
typedef struct {
    uint8_t dev_addr;                               /**< Connected device address */
    usb_device_handle_t dev_hdl;                    /**< Connected device handle */
    usb_host_client_handle_t client_hdl;            /**< Remote wake client handle */
    test_stage_t cur_stage;                         /**< Current test stage from test_stage_t */
    test_stage_t next_stage;                        /**< Next test stage from test_stage_t */
    test_stage_t submit_stage;                      /**< Stage, from which a ctrl transfer was submitted */
    const usb_config_desc_t *config_desc;           /**< Pointer to the device's configuration descriptor */
    bool remote_wake_enabled;                       /**< Remote wakeup is currently enabled/disabled by the USB Host */
} remote_wake_client_obj_t;

static remote_wake_client_obj_t *s_remote_wake_obj = NULL;

static void ctrl_transfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(REMOTE_WAKE_CLIENT_TAG, "Transfer callback");
    remote_wake_client_obj_t *remote_wake_obj = (remote_wake_client_obj_t *)transfer->context;

    // Check the completed control transfer
    TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Transfer NOT completed");

    switch (remote_wake_obj->submit_stage) {
    case TEST_STAGE_REMOTE_WAKE_ENABLE: {
        // Remote wakeup was enabled by USB Host, read it's value from the device in the next step
        remote_wake_obj->next_stage = TEST_STAGE_REMOTE_WAKE_CHECK;
        remote_wake_obj->remote_wake_enabled = true;
        break;
    }
    case TEST_STAGE_REMOTE_WAKE_DISABLE: {
        // Remote wakeup was disabled by USB Host, read it's value from the device in the next step
        remote_wake_obj->next_stage = TEST_STAGE_REMOTE_WAKE_CHECK;
        remote_wake_obj->remote_wake_enabled = false;
        break;
    }
    case TEST_STAGE_REMOTE_WAKE_CHECK: {

        // Transferred num bytes must be at least setup packet + device status
        TEST_ASSERT_GREATER_OR_EQUAL(sizeof(usb_setup_packet_t) + sizeof(usb_device_status_t), transfer->actual_num_bytes);
        // Copy device status from the IN transaction
        usb_device_status_t device_status;
        memcpy(&device_status, transfer->data_buffer + sizeof(usb_setup_packet_t), sizeof(usb_device_status_t));

        if (remote_wake_obj->remote_wake_enabled) {
            // Remote wake was enabled, host should read enabled
            TEST_ASSERT_MESSAGE(device_status.remote_wakeup, "Remote wakeup is expected to be enabled, but it's disabled");
            remote_wake_obj->next_stage = TEST_STAGE_SUSPEND;
        } else {
            // Remote wake was disabled, host should read disabled
            remote_wake_obj->next_stage = TEST_STAGE_DEV_CLOSE;
            TEST_ASSERT_FALSE_MESSAGE(device_status.remote_wakeup, "Remote wakeup expected to be disabled, but it's enabled");
        }
        break;
    }
    default:
        TEST_FAIL_MESSAGE("Unhandled stage");
        break;
    }
}

static void remote_wake_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    remote_wake_client_obj_t *remote_wake_obj = (remote_wake_client_obj_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Client event -> New device");
        TEST_ASSERT_EQUAL(TEST_STAGE_WAIT_CONN, remote_wake_obj->cur_stage);
        remote_wake_obj->next_stage = TEST_STAGE_DEV_OPEN;
        remote_wake_obj->dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Client event -> Device suspended");
        remote_wake_obj->next_stage = TEST_STAGE_DO_REMOTE_WAKE;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Client event -> Device resumed");
        remote_wake_obj->next_stage = TEST_STAGE_REMOTE_WAKE_DISABLE;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Client event -> Device gone");
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized client event");
        break;
    }
}

void ctrl_client_remote_wake_task(void *arg)
{
    remote_wake_client_obj_t remote_wake_obj;
    // Initialize client variables
    remote_wake_obj.client_hdl = NULL;
    remote_wake_obj.dev_hdl = NULL;
    // Initialize test stage
    remote_wake_obj.cur_stage = TEST_STAGE_WAIT_CONN;
    remote_wake_obj.next_stage = TEST_STAGE_WAIT_CONN;
    remote_wake_obj.dev_addr = 0;
    remote_wake_obj.remote_wake_enabled = false;        // Remote wakeup is turned off by default after device reset

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

    // Allocate and prepare ctrl transfer
    usb_transfer_t *ctrl_xfer = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + MAX_TRANSFER_BYTES, 0, &ctrl_xfer));
    ctrl_xfer->callback = ctrl_transfer_cb;
    ctrl_xfer->context = (void *)&remote_wake_obj;
    ctrl_xfer->bEndpointAddress = 0;

    // Wait to be started by main thread
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)), "Remote wake client not started from main thread");
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Starting");

    // Handle device enumeration separately, wait for 1000ms for the device to be enumerated
    // Catch an error in case the device is not enumerated correctly
    esp_err_t enum_ret = usb_host_client_handle_events(remote_wake_obj.client_hdl, pdMS_TO_TICKS(EVENT_NEW_DEV_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(TEST_STAGE_DEV_OPEN, remote_wake_obj.next_stage, "USB_HOST_CLIENT_EVENT_NEW_DEV not generated on time");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, enum_ret, "Client handle events timed out");

    bool exit_loop = false;
    bool skip_event_handling = true;    // Skip first event handling (we have handled the new device event separately)
    while (!exit_loop) {
        if (!skip_event_handling) {
            TEST_ASSERT_EQUAL_MESSAGE(
                ESP_OK, usb_host_client_handle_events(remote_wake_obj.client_hdl, pdMS_TO_TICKS(CLIENT_PROCESS_TIMEOUT_MS)),
                "Client has timed out processing events");
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
            ctrl_xfer->device_handle = remote_wake_obj.dev_hdl;

            // Check if the device supports remote wakeup from it's configuration descriptor
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_get_active_config_descriptor(remote_wake_obj.dev_hdl, &remote_wake_obj.config_desc));
            if (remote_wake_obj.config_desc->bmAttributes & USB_BM_ATTRIBUTES_WAKEUP) {
                // Has remote wakeup, do the test
                remote_wake_obj.next_stage = TEST_STAGE_REMOTE_WAKE_ENABLE;
            } else {
                // Does not have remote wakeup, close the device
                ESP_LOGW(REMOTE_WAKE_CLIENT_TAG, "Device does not support remote wakeup functionality");
                remote_wake_obj.next_stage = TEST_STAGE_DEV_CLOSE;
            }
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_REMOTE_WAKE_ENABLE: {
            ESP_LOGD(REMOTE_WAKE_CLIENT_TAG, "Remote wake enable");

            // Sent a control transfer to enable remote wakeup
            USB_SETUP_PACKET_INIT_SET_FEATURE((usb_setup_packet_t *)ctrl_xfer->data_buffer, DEVICE_REMOTE_WAKEUP);
            ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t);
            s_remote_wake_obj->submit_stage = TEST_STAGE_REMOTE_WAKE_ENABLE;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit_control(s_remote_wake_obj->client_hdl, ctrl_xfer));
            remote_wake_obj.next_stage = TEST_STAGE_CTRL_XFER_WAIT;
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_REMOTE_WAKE_DISABLE: {
            ESP_LOGD(REMOTE_WAKE_CLIENT_TAG, "Remote wake disable");

            // Sent a control transfer to disable remote wakeup
            USB_SETUP_PACKET_INIT_CLEAR_FEATURE((usb_setup_packet_t *)ctrl_xfer->data_buffer, DEVICE_REMOTE_WAKEUP);
            ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t);
            s_remote_wake_obj->submit_stage = TEST_STAGE_REMOTE_WAKE_DISABLE;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit_control(s_remote_wake_obj->client_hdl, ctrl_xfer));
            remote_wake_obj.next_stage = TEST_STAGE_CTRL_XFER_WAIT;
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_REMOTE_WAKE_CHECK: {
            ESP_LOGD(REMOTE_WAKE_CLIENT_TAG, "Remote wake check");

            // Sent a control transfer to get the devices's current remote wakeup status
            USB_SETUP_PACKET_INIT_GET_STATUS((usb_setup_packet_t *)ctrl_xfer->data_buffer);
            ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t) + MAX_TRANSFER_BYTES;
            s_remote_wake_obj->submit_stage = TEST_STAGE_REMOTE_WAKE_CHECK;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit_control(s_remote_wake_obj->client_hdl, ctrl_xfer));
            remote_wake_obj.next_stage = TEST_STAGE_CTRL_XFER_WAIT;
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_CTRL_XFER_WAIT: {
            // Wait for the control transfer to complete
            break;
        }
        case TEST_STAGE_SUSPEND: {
            ESP_LOGD(REMOTE_WAKE_CLIENT_TAG, "Suspend");

            // Suspend the root port and expect the suspend event
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

            // Next stage is set from the ctrl_transfer_cb
            break;
        }
        case TEST_STAGE_DO_REMOTE_WAKE: {
            ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Do the remote wakeup (keyboard button press)");

            // Next stage is set from the ctrl_transfer_cb
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
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_free(ctrl_xfer));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(remote_wake_obj.client_hdl));
    ESP_LOGI(REMOTE_WAKE_CLIENT_TAG, "Done");
    vTaskDelete(NULL);
}

#endif // REMOTE_WAKE_HAL_SUPPORTED
