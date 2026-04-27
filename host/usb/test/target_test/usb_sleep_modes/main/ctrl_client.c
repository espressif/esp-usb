/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_log.h"
#include "dev_msc.h"
#include "ctrl_client.h"
#include "usb/usb_host.h"
#include "unity.h"

#if defined(SOC_LIGHT_SLEEP_SUPPORTED) && defined(SOC_DEEP_SLEEP_SUPPORTED)

/*
Implementation of a control transfer client used for USB Host Tests.

- The control transfer client will:
    - Register itself as a client
    - Receive USB_HOST_CLIENT_EVENT_NEW_DEV event message, and open the device
    - Allocate a control transfer object and submit a control transfer to get the active configuration descriptor of the device
    - Waits for client events and either schedules a new control transfer or closes the device based on the events received
    - Free transfer objects
    - Close the device
    - Deregister control client
*/

#define CTRL_CLIENT_MAX_EVENT_MSGS      5
#define MAX_TRANSFER_BYTES              256
#define NEW_DEV_EVENT_MS                1000    // Delay to wait for a device to be connected

const char *CTRL_CLIENT_TAG = "Ctrl Client sleep";

typedef enum {
    TEST_STAGE_WAIT_CONN,
    TEST_STAGE_DEV_OPEN,
    TEST_STAGE_CTRL_XFER,
    TEST_STAGE_CTRL_XFER_WAIT,
    TEST_STAGE_DEV_CLOSE,
} test_stage_t;

typedef struct {
    uint8_t dev_addr;                               /**< Device address */
    usb_speed_t dev_speed;                          /**< Device speed */
    usb_host_client_handle_t client_hdl;            /**< Client handle */
    usb_device_handle_t dev_hdl;                    /**< Device handle */
    test_stage_t cur_stage;                         /**< Current stage of the test */
    test_stage_t next_stage;                        /**< Next stage of the test */
    const usb_config_desc_t *config_desc_cached;    /**< Cached active configuration descriptor for comparison */
    EventGroupHandle_t test_event_group;            /**< Event group handle for synchronizing with main test task */
    esp_sleep_mode_t sleep_mode;                    /**< Sleep mode to be tested */
} ctrl_client_obj_t;

static void ctrl_transfer_cb(usb_transfer_t *transfer)
{
    ctrl_client_obj_t *ctrl_obj = (ctrl_client_obj_t *)transfer->context;
    // Check the completed control transfer
    TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Transfer NOT completed");

    // Get and check device configuration number
    uint8_t *data = transfer->data_buffer + sizeof(usb_setup_packet_t);
    TEST_ASSERT_EQUAL_MESSAGE(data[0], 1, "Device configuration value should be 1");

    if (ctrl_obj->sleep_mode == ESP_SLEEP_MODE_LIGHT_SLEEP) {
        ctrl_obj->next_stage = TEST_STAGE_CTRL_XFER_WAIT;
    } else {
        ctrl_obj->next_stage = TEST_STAGE_DEV_CLOSE;
    }

    // CTRL transfer is done, signalize sleep task to enter either light sleep or deep sleep
    xEventGroupSetBits(ctrl_obj->test_event_group, EVT_ALLOW_SLEEP);
}

static void ctrl_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    ctrl_client_obj_t *ctrl_obj = (ctrl_client_obj_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(CTRL_CLIENT_TAG, "Client event -> New device");
        TEST_ASSERT_EQUAL(TEST_STAGE_WAIT_CONN, ctrl_obj->cur_stage);
        ctrl_obj->next_stage = TEST_STAGE_DEV_OPEN;
        ctrl_obj->dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(CTRL_CLIENT_TAG, "Client event -> Device gone");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ESP_LOGI(CTRL_CLIENT_TAG, "Client event -> Device suspended");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ESP_LOGI(CTRL_CLIENT_TAG, "Client event -> Device resumed");

        // Only relevant to light sleep test, check if the device is resumed from light sleep and schedule next stage accordingly
        EventBits_t bits = xEventGroupGetBits(ctrl_obj->test_event_group);
        if (bits & EVT_TEST_FINISH) {
            ctrl_obj->next_stage = TEST_STAGE_DEV_CLOSE;
        } else {
            ctrl_obj->next_stage = TEST_STAGE_CTRL_XFER;
        }
        break;
    default:
        abort();    // Should never occur in this test
        break;
    }
}

void ctrl_client_sleep_task(void *arg)
{
    ctrl_client_obj_t ctrl_obj = {0};
    ctrl_obj.cur_stage = TEST_STAGE_WAIT_CONN;
    ctrl_obj.next_stage = TEST_STAGE_WAIT_CONN;
    ctrl_obj.sleep_mode = ((test_context_t *)arg)->sleep_mode;
    ctrl_obj.test_event_group = ((test_context_t *)arg)->test_event_group;

    // Register client
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = CTRL_CLIENT_MAX_EVENT_MSGS,
        .async = {
            .client_event_callback = ctrl_client_event_cb,
            .callback_arg = (void *) &ctrl_obj,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &ctrl_obj.client_hdl));

    // Allocate transfers
    usb_transfer_t *ctrl_xfer = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + MAX_TRANSFER_BYTES, 0, &ctrl_xfer));
    ctrl_xfer->callback = ctrl_transfer_cb;
    ctrl_xfer->context = (void *)&ctrl_obj;

    // Wait to be started by main thread
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)), "Ctrl client not started from main thread");
    ESP_LOGI(CTRL_CLIENT_TAG, "Starting");

    // Handle device enumeration separately, wait for 1000ms for the device to be enumerated
    // Catch an error in case the device is not enumerated correctly
    esp_err_t enum_ret = usb_host_client_handle_events(ctrl_obj.client_hdl, pdMS_TO_TICKS(NEW_DEV_EVENT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(TEST_STAGE_DEV_OPEN, ctrl_obj.next_stage, "USB_HOST_CLIENT_EVENT_NEW_DEV not generated on time");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, enum_ret, "Client handle events timed out");

    bool exit_loop = false;
    bool skip_event_handling = true;    // Skip first event handling (we have handled the new device event separately)
    while (!exit_loop) {
        if (!skip_event_handling) {
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_handle_events(ctrl_obj.client_hdl, portMAX_DELAY));
        }
        skip_event_handling = false;
        if (ctrl_obj.cur_stage == ctrl_obj.next_stage) {
            continue;
        }
        ctrl_obj.cur_stage = ctrl_obj.next_stage;

        switch (ctrl_obj.next_stage) {
        case TEST_STAGE_DEV_OPEN: {
            ESP_LOGI(CTRL_CLIENT_TAG, "Open");
            // Open the device
            TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, usb_host_device_open(ctrl_obj.client_hdl, ctrl_obj.dev_addr, &ctrl_obj.dev_hdl), "Failed to open the device");
            // Get device info to get device speed
            usb_device_info_t dev_info;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_info(ctrl_obj.dev_hdl, &dev_info));
            ctrl_obj.dev_speed = dev_info.speed;
            ctrl_xfer->device_handle = ctrl_obj.dev_hdl;

            // Check that the device descriptor matches our expected MSC device
            const usb_device_desc_t *device_desc;
            const usb_device_desc_t *device_desc_ref = dev_msc_get_dev_desc(ctrl_obj.dev_speed);
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_get_device_descriptor(ctrl_obj.dev_hdl, &device_desc));
            TEST_ASSERT_EQUAL(device_desc_ref->bLength, device_desc->bLength);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(device_desc_ref, device_desc, sizeof(usb_device_desc_t), "Device descriptors do not match.");
            // Cache the active configuration descriptor for later comparison
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_get_active_config_descriptor(ctrl_obj.dev_hdl, &ctrl_obj.config_desc_cached));
            ctrl_obj.next_stage = TEST_STAGE_CTRL_XFER;
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_CTRL_XFER: {
            ESP_LOGI(CTRL_CLIENT_TAG, "Transfer");
            // Submit a ctrl transfer to get a configuration value of the device
            // It's expected that the config value is 1, thus the device did not undergo a reset during light sleep
            USB_SETUP_PACKET_INIT_GET_CONFIG((usb_setup_packet_t *)ctrl_xfer->data_buffer);
            ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t) + MAX_TRANSFER_BYTES;
            ctrl_xfer->bEndpointAddress = 0x80; // IN transfer
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit_control(ctrl_obj.client_hdl, ctrl_xfer));
            ctrl_obj.next_stage = TEST_STAGE_CTRL_XFER_WAIT;
            skip_event_handling = false;
            break;
        }
        case TEST_STAGE_CTRL_XFER_WAIT: {
            break;
        }
        case TEST_STAGE_DEV_CLOSE: {
            ESP_LOGI(CTRL_CLIENT_TAG, "Close");
            vTaskDelay(10); // Give USB Host Lib some time to process all transfers
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(ctrl_obj.client_hdl, ctrl_obj.dev_hdl));
            exit_loop = true;
            break;
        }
        default:
            abort();
            break;
        }
    }
    // Free transfers and deregister client
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_free(ctrl_xfer));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(ctrl_obj.client_hdl));
    ESP_LOGI(CTRL_CLIENT_TAG, "Done");
    xEventGroupSetBits(ctrl_obj.test_event_group, EVT_CLIENT_CLOSE);
    vTaskDelete(NULL);
}

#endif // SOC_LIGHT_SLEEP_SUPPORTED && SOC_DEEP_SLEEP_SUPPORTED
