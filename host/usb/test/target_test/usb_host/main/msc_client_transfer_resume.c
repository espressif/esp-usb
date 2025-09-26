/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mock_msc.h"
#include "dev_msc.h"
#include "msc_client.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_host.h"
#include "unity.h"

/*
Implementation of an asynchronous MSC client used for USB Host's root port resume by submitting a transfer functionality.

- The MSC client will:
    - Register itself as a client
    - Receive USB_HOST_CLIENT_EVENT_NEW_DEV event message, and open the device
    - Open the device by a client
    - Allocate IN and OUT transfer objects for MSC SCSI transfers
    - Suspend the root port using the automatic suspend timer
        - Receive USB_HOST_CLIENT_EVENT_DEV_SUSPENDED event message
    - Issue MSC reset (while the root port is still suspended) to test root port automatic resume by submitting a control transfer
    - Expect USB_TRANSFER_STATUS_COMPLETED
    - Iterate through:
        - Suspend the root port
        - Issue CBW, Data read and CSW
    - Deregister MSC client
*/

#define NEW_DEV_EVENT_MS        1000        // Delay to wait for a device to be connected

typedef enum {
    TEST_STAGE_WAIT_CONN,                   /**< Wait for device connection */
    TEST_STAGE_DEV_OPEN,                    /**< Open the device by the Client */
    TEST_STAGE_SUSPEND_BY_TIMER,            /**< Suspend the root port using automatic timer */
    TEST_STAGE_MSC_RESET,                   /**< Issue reset to the device (resume the root port by issuing the reset) */
    TEST_STAGE_SUSPEND,                     /**< Suspend the root port */
    TEST_STAGE_MSC_CBW,                     /**< Issue CBW to the MSC device (resume the root port by issuing the CBW) */
    TEST_STAGE_MSC_DATA,                    /**< Send data to the MSC device */
    TEST_STAGE_MSC_CSW,                     /**< Issue CBW to the MSC device */
    TEST_STAGE_DEV_CLOSE,                   /**< Handle device closing by the Client */
} test_stage_t;

typedef struct {
    // Test parameters
    msc_client_test_param_t test_param;
    // MSC device info
    const dev_msc_info_t *dev_info;
    usb_speed_t dev_speed;
    uint8_t dev_addr;
    // Client variables
    usb_host_client_handle_t client_hdl;
    usb_device_handle_t dev_hdl;
    // Test state
    test_stage_t cur_stage;
    test_stage_t next_stage;
    int num_sectors_read;
} msc_client_obj_t;

static void msc_transfer_cb(usb_transfer_t *transfer)
{
    msc_client_obj_t *msc_obj = (msc_client_obj_t *)transfer->context;

    switch (msc_obj->cur_stage) {
    case TEST_STAGE_MSC_RESET: {
        // We expect the reset transfer to complete with no issues
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Reset transfer NOT completed");
        TEST_ASSERT_EQUAL(transfer->num_bytes, transfer->actual_num_bytes);
        msc_obj->next_stage = TEST_STAGE_SUSPEND;
        break;
    }
    case TEST_STAGE_MSC_CBW: {
        // We expect the CBW transfer to complete with no issues
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "CBW transfer NOT completed");
        TEST_ASSERT_EQUAL(transfer->num_bytes, transfer->actual_num_bytes);
        msc_obj->next_stage = TEST_STAGE_MSC_DATA;
        break;
    }
    default:
        abort();
        break;
    }
}

static void msc_data_transfer_cb(usb_transfer_t *transfer)
{
    msc_client_obj_t *msc_obj = (msc_client_obj_t *)transfer->context;
    switch (msc_obj->cur_stage) {
    case TEST_STAGE_MSC_DATA: {
        // The data stage must have completed
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Data transfer NOT completed");
        TEST_ASSERT_EQUAL(msc_obj->dev_info->scsi_sector_size * msc_obj->test_param.num_sectors_per_xfer, transfer->actual_num_bytes);
        msc_obj->next_stage = TEST_STAGE_MSC_CSW;
        break;
    }
    case TEST_STAGE_MSC_CSW: {
        // The CSW stage must have completed
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "CSW transfer NOT completed");
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)transfer->data_buffer, msc_obj->test_param.msc_scsi_xfer_tag));
        msc_obj->num_sectors_read += msc_obj->test_param.num_sectors_per_xfer;
        if (msc_obj->num_sectors_read < msc_obj->test_param.num_sectors_to_read) {
            msc_obj->next_stage = TEST_STAGE_SUSPEND;
        } else {
            msc_obj->next_stage = TEST_STAGE_DEV_CLOSE;
        }
        break;
    }
    default:
        abort();
        break;
    }
}

static void msc_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    msc_client_obj_t *msc_obj = (msc_client_obj_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> New device");
        TEST_ASSERT_EQUAL(TEST_STAGE_WAIT_CONN, msc_obj->cur_stage);
        msc_obj->next_stage = TEST_STAGE_DEV_OPEN;
        msc_obj->dev_addr = event_msg->new_dev.address;
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED: {
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> Device suspended");
        TEST_ASSERT_TRUE(msc_obj->cur_stage == TEST_STAGE_SUSPEND_BY_TIMER ||
                         msc_obj->cur_stage == TEST_STAGE_SUSPEND);

        if (msc_obj->cur_stage == TEST_STAGE_SUSPEND_BY_TIMER) {
            msc_obj->next_stage = TEST_STAGE_MSC_RESET;
        } else {
            msc_obj->next_stage = TEST_STAGE_MSC_CBW;
        }
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED: {
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> Device resumed");
        break;
    }
    default:
        abort();    // Should never occur in this test
        break;
    }
}

void msc_client_async_resume_by_transfer_task(void *arg)
{
    msc_client_obj_t msc_obj;
    // Initialize test params
    memcpy(&msc_obj.test_param, arg, sizeof(msc_client_test_param_t));
    // Initialize MSC device info
    msc_obj.dev_info = dev_msc_get_info();
    // Initialize client variables
    msc_obj.client_hdl = NULL;
    msc_obj.dev_hdl = NULL;
    // Initialize test state
    msc_obj.cur_stage = TEST_STAGE_WAIT_CONN;
    msc_obj.next_stage = TEST_STAGE_WAIT_CONN;
    msc_obj.num_sectors_read = 0;

    // Register client
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = MSC_ASYNC_CLIENT_MAX_EVENT_MSGS,
        .async = {
            .client_event_callback = msc_client_event_cb,
            .callback_arg = (void *) &msc_obj,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &msc_obj.client_hdl));

    // IN MPS and transfers to be set/allocated later (after device connects and MPS is determined)
    int in_ep_mps = 0;
    usb_transfer_t *xfer_in = NULL;
    usb_transfer_t *xfer_out = NULL;

    // Wait to be started by main thread
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)), "MSC client not started from main thread");
    ESP_LOGI(MSC_CLIENT_TAG, "Starting");

    // Handle device enumeration separately, wait for 1000ms for the device to be enumerated
    // Catch an error in case the device is not enumerated correctly
    esp_err_t enum_ret = usb_host_client_handle_events(msc_obj.client_hdl, pdMS_TO_TICKS(NEW_DEV_EVENT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(TEST_STAGE_DEV_OPEN, msc_obj.next_stage, "USB_HOST_CLIENT_EVENT_NEW_DEV not generated on time");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, enum_ret, "Client handle events timed out");

    bool exit_loop = false;
    bool skip_event_handling = true;    // Skip first event handling (we have handled the new device event separately)
    while (!exit_loop) {
        if (!skip_event_handling) {
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_handle_events(msc_obj.client_hdl, portMAX_DELAY));
        }
        skip_event_handling = false;
        if (msc_obj.cur_stage == msc_obj.next_stage) {
            continue;
        }
        msc_obj.cur_stage = msc_obj.next_stage;

        switch (msc_obj.cur_stage) {
        case TEST_STAGE_WAIT_CONN: {
            // Nothing to do while waiting for connection
            break;
        }
        case TEST_STAGE_DEV_OPEN: {
            ESP_LOGI(MSC_CLIENT_TAG, "Open");
            // Open the device
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(msc_obj.client_hdl, msc_obj.dev_addr, &msc_obj.dev_hdl));

            // Get device info to get device speed
            usb_device_info_t dev_info;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_info(msc_obj.dev_hdl, &dev_info));
            msc_obj.dev_speed = dev_info.speed;
            // Check the device descriptor of the opened device
            const usb_device_desc_t *device_desc;
            const usb_device_desc_t *device_desc_ref = dev_msc_get_dev_desc(msc_obj.dev_speed);
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_get_device_descriptor(msc_obj.dev_hdl, &device_desc));
            TEST_ASSERT_EQUAL(device_desc_ref->bLength, device_desc->bLength);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(device_desc_ref, device_desc, device_desc_ref->bLength, "Device descriptors do not match.");
            // Claim the MSC interface
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_interface_claim(msc_obj.client_hdl,
                                                               msc_obj.dev_hdl,
                                                               msc_obj.dev_info->bInterfaceNumber,
                                                               msc_obj.dev_info->bAlternateSetting));

            // Allocate transfers
            const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(msc_obj.dev_speed);
            in_ep_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
            const size_t in_worst_case_size = usb_round_up_to_mps(MAX(msc_obj.dev_info->scsi_sector_size * msc_obj.test_param.num_sectors_per_xfer,
                                                                      sizeof(mock_msc_bulk_csw_t)),
                                                                  in_ep_mps);
            const size_t out_worst_case_size = MAX(sizeof(mock_msc_bulk_cbw_t), sizeof(usb_setup_packet_t));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_alloc(out_worst_case_size, 0, &xfer_out));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_alloc(in_worst_case_size, 0, &xfer_in));
            xfer_out->callback = msc_transfer_cb;
            xfer_in->callback = msc_data_transfer_cb;
            xfer_out->context = (void *)&msc_obj;
            xfer_in->context = (void *)&msc_obj;
            // Target our transfers to the device
            xfer_out->device_handle = msc_obj.dev_hdl;
            xfer_in->device_handle = msc_obj.dev_hdl;
            msc_obj.next_stage = TEST_STAGE_SUSPEND_BY_TIMER;
            skip_event_handling = true; // Need to execute TEST_STAGE_SUSPEND_BY_TIMER
            break;
        }
        case TEST_STAGE_SUSPEND_BY_TIMER: {
            ESP_LOGD(MSC_CLIENT_TAG, "Suspend by auto suspend timer");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, 200));
            // Next stage is set from msc_client_event_cb
            break;
        }
        case TEST_STAGE_MSC_RESET: {
            ESP_LOGD(MSC_CLIENT_TAG, "MSC Reset");
            // Send an MSC SCSI interface reset
            MOCK_MSC_SCSI_REQ_INIT_RESET((usb_setup_packet_t *)xfer_out->data_buffer, msc_obj.dev_info->bInterfaceNumber);
            xfer_out->num_bytes = sizeof(usb_setup_packet_t);
            xfer_out->bEndpointAddress = 0;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit_control(msc_obj.client_hdl, xfer_out));
            // Test that deferred control transfer can't be resubmitted
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_transfer_submit_control(msc_obj.client_hdl, xfer_out));
            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_SUSPEND: {
            ESP_LOGD(MSC_CLIENT_TAG, "Suspend");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
            vTaskDelay(10);             // Stay suspended for a while
            // Next stage is set from msc_client_event_cb
            break;
        }
        case TEST_STAGE_MSC_CBW: {
            ESP_LOGD(MSC_CLIENT_TAG, "CBW");
            mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)xfer_out->data_buffer,
                                   true,
                                   0,
                                   msc_obj.test_param.num_sectors_per_xfer,
                                   msc_obj.dev_info->scsi_sector_size,
                                   msc_obj.test_param.msc_scsi_xfer_tag);
            xfer_out->num_bytes = sizeof(mock_msc_bulk_cbw_t);
            xfer_out->bEndpointAddress = msc_obj.dev_info->out_ep_addr;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit(xfer_out));
            // Test that pending transfers can't be resubmitted
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_transfer_submit(xfer_out));
            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_MSC_DATA: {
            ESP_LOGD(MSC_CLIENT_TAG, "Data");
            xfer_in->num_bytes = usb_round_up_to_mps(msc_obj.dev_info->scsi_sector_size * msc_obj.test_param.num_sectors_per_xfer, in_ep_mps);
            xfer_in->bEndpointAddress = msc_obj.dev_info->in_ep_addr;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit(xfer_in));
            // Test that an inflight transfer cannot be resubmitted
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_transfer_submit(xfer_in));
            break;
            // Next stage set from transfer callback
        }
        case TEST_STAGE_MSC_CSW: {
            ESP_LOGD(MSC_CLIENT_TAG, "CSW");
            xfer_in->num_bytes = usb_round_up_to_mps(sizeof(mock_msc_bulk_csw_t), in_ep_mps);
            xfer_in->bEndpointAddress = msc_obj.dev_info->in_ep_addr;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit(xfer_in));
            // Test that an inflight transfer cannot be resubmitted
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_transfer_submit(xfer_in));
            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_DEV_CLOSE: {
            ESP_LOGI(MSC_CLIENT_TAG, "Close");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_interface_release(msc_obj.client_hdl, msc_obj.dev_hdl, msc_obj.dev_info->bInterfaceNumber));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(msc_obj.client_hdl, msc_obj.dev_hdl));
            exit_loop = true;
            break;
        }
        default:
            abort();
            break;
        }
    }
    // Free transfers
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_free(xfer_out));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_free(xfer_in));
    // Deregister the client
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(msc_obj.client_hdl));
    ESP_LOGI(MSC_CLIENT_TAG, "Done");
    vTaskDelete(NULL);
}
