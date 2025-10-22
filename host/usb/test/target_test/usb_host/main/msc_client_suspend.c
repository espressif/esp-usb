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
#include "usb/usb_host.h"
#include "unity.h"

/*
Implementation of an asynchronous MSC client used for USB Host's root port suspend/resume functionality.

- The MSC client will:
    - Register itself as a client
    - Receive USB_HOST_CLIENT_EVENT_NEW_DEV event message, and open the device
    - Suspend/Resume the root port without the client opening the device
    - Open the device
    - Allocate IN and OUT transfer objects for MSC SCSI transfers
    - Suspend/Resume the root port without any transfer submitted
        - Receive USB_HOST_CLIENT_EVENT_DEV_SUSPENDED and USB_HOST_CLIENT_EVENT_DEV_RESUMED events messages
    - Trigger a single MSC SCSI transfer
        - Split the data stage into multiple transfers (so that the endpoint multiple queued up transfers)
        - Fail to suspend the root port mid-way through the data stage, because of ongoing transfers
    - Wait for all transfers to finish
    - Successfully suspend the root port and cause device disconnection
        - Receive USB_HOST_CLIENT_EVENT_DEV_SUSPENDED and USB_HOST_CLIENT_EVENT_DEV_GONE
    - Close device and try to resume the root port (with no device connected)
    - Power on the root port back, wait for the USB_HOST_CLIENT_EVENT_NEW_DEV event message, and open the device
    - Fail to resume the root port (after it was suspended, but a device reconnection happened)
    - Repeat the above steps, to ensure correct behaviour after Suspend/Resume/Connect/Disconnect events
    - Deregister MSC client
*/

#define TEST_DCONN_ITERATIONS   2

typedef enum {
    TEST_STAGE_WAIT_CONN,                       /**< Wait for device connection */
    TEST_STAGE_DEV_SUSPEND_RESUME_NO_CLIENT,    /**< Suspend and Resume a device without opening it by a Client */
    TEST_STAGE_DEV_OPEN,                        /**< Open the device by the Client */
    TEST_STAGE_MSC_RESET,                       /**< Issue reset to the device */
    TEST_STAGE_SUSPEND_NO_TRANSFER,             /**< Suspend the device (opened by the Client), while no transfers are submitted */
    TEST_STAGE_RESUME_NO_TRANSFER,              /**< Resume the device (opened by the Client), while no transfers are submitted */
    TEST_STAGE_MSC_CBW,                         /**< Issue CBW to the MSC device */
    TEST_STAGE_MSC_DATA,                        /**< Send data to the MSC device, try to suspend the device while transfers are submitted */
    TEST_STAGE_MSC_CSW,                         /**< Issue CSW to the MSC device */
    TEST_STAGE_SUSPEND_POWER_OFF,               /**< Suspend the device and immediately power off the Root port (disconnect the device) */
    TEST_STAGE_DEV_CLOSE,                       /**< Handle device closing by the Client */
    TEST_STAGE_RESUME_POWER_ON,                 /**< Try to resume the device after it was disconnected, then power on the port */
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
        // Check MSC SCSI interface reset
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Transfer NOT completed");
        TEST_ASSERT_EQUAL(transfer->num_bytes, transfer->actual_num_bytes);
        msc_obj->next_stage = TEST_STAGE_SUSPEND_NO_TRANSFER;
        break;
    }
    case TEST_STAGE_MSC_CBW: {
        // Check MSC SCSI CBW transfer
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Transfer NOT completed");
        TEST_ASSERT_EQUAL(sizeof(mock_msc_bulk_cbw_t), transfer->actual_num_bytes);
        msc_obj->next_stage = TEST_STAGE_MSC_DATA;
        break;
    }
    case TEST_STAGE_MSC_DATA: {
        // Check MSC SCSI data IN transfer
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Transfer NOT completed");
        TEST_ASSERT_EQUAL(msc_obj->dev_info->scsi_sector_size * msc_obj->test_param.num_sectors_per_xfer, transfer->actual_num_bytes);
        msc_obj->next_stage = TEST_STAGE_MSC_CSW;
        break;
    }
    case TEST_STAGE_MSC_CSW: {
        // Check MSC SCSI CSW transfer
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, transfer->status, "Transfer NOT completed");
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)transfer->data_buffer, msc_obj->test_param.msc_scsi_xfer_tag));
        msc_obj->num_sectors_read += msc_obj->test_param.num_sectors_per_xfer;
        if (msc_obj->num_sectors_read < msc_obj->test_param.num_sectors_to_read) {
            msc_obj->next_stage = TEST_STAGE_MSC_CBW;
        } else {
            msc_obj->next_stage = TEST_STAGE_SUSPEND_POWER_OFF;
        }
        break;
    }
    default: {
        abort();    // Should never occur in this test
        break;
    }
    }
}

static void msc_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    msc_client_obj_t *msc_obj = (msc_client_obj_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> New device");
        TEST_ASSERT_EQUAL(TEST_STAGE_WAIT_CONN, msc_obj->cur_stage);
        msc_obj->next_stage = TEST_STAGE_DEV_SUSPEND_RESUME_NO_CLIENT;
        msc_obj->dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> Device suspended");
        TEST_ASSERT_TRUE(msc_obj->cur_stage == TEST_STAGE_SUSPEND_NO_TRANSFER ||
                         msc_obj->cur_stage == TEST_STAGE_SUSPEND_POWER_OFF);
        if (msc_obj->cur_stage == TEST_STAGE_SUSPEND_NO_TRANSFER) {
            msc_obj->next_stage = TEST_STAGE_RESUME_NO_TRANSFER;
        }
        TEST_ASSERT_EQUAL_PTR_MESSAGE(msc_obj->dev_hdl, event_msg->dev_suspend_resume.dev_hdl, "Suspended device handle not correct");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> Device resumed");
        TEST_ASSERT_EQUAL(msc_obj->cur_stage, TEST_STAGE_RESUME_NO_TRANSFER);
        msc_obj->next_stage = TEST_STAGE_MSC_CBW;
        TEST_ASSERT_EQUAL_PTR_MESSAGE(msc_obj->dev_hdl, event_msg->dev_suspend_resume.dev_hdl, "Resumed device handle not correct");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(MSC_CLIENT_TAG, "Client event -> Device gone");
        TEST_ASSERT_EQUAL(msc_obj->cur_stage, TEST_STAGE_SUSPEND_POWER_OFF);
        msc_obj->next_stage = TEST_STAGE_DEV_CLOSE;
        break;
    default:
        abort();    // Should never occur in this test
        break;

    }
}

void msc_client_async_suspend_resume_task(void *arg)
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
    msc_obj.dev_addr = 0;

    // Register MSC client
    usb_host_client_config_t msc_client_config = {
        .is_synchronous = false,
        .max_num_event_msg = MSC_ASYNC_CLIENT_MAX_EVENT_MSGS,
        .async = {
            .client_event_callback = msc_client_event_cb,
            .callback_arg = (void *) &msc_obj,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&msc_client_config, &msc_obj.client_hdl));

    // IN MPS and transfers to be set/allocated later (after device connects and MPS is determined)
    int in_ep_mps = 0;
    usb_transfer_t *xfer_in = NULL;
    usb_transfer_t *xfer_out = NULL;

    // Wait to be started by main thread
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(MSC_CLIENT_TAG, "Starting");

    // Try to suspend, when no device is yet connected
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, usb_host_lib_root_port_suspend());

    bool exit_loop = false;
    bool skip_event_handling = false;
    int dconn_iter = 0;

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
        case TEST_STAGE_DEV_SUSPEND_RESUME_NO_CLIENT: {
            ESP_LOGD(MSC_CLIENT_TAG, "Suspend/Resume device no client");

            // 1 run of the test stages: Try to resume without previously being suspended
            // 2 run of the test stages: Try to resume after sequence: Suspend -> Disconnect -> Connect
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, usb_host_lib_root_port_resume());

            // Suspend the device, without being opened by a client
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

            // Wait until the root port is suspended
            // Normally USB_HOST_CLIENT_EVENT_DEV_RESUMED is delivered to a client which opened the device
            vTaskDelay(10);
            // Resume the device, without being opened by a client
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

            // Wait until the root port is resumed
            // Normally USB_HOST_CLIENT_EVENT_DEV_SUSPENDED is delivered to a client which opened the device
            vTaskDelay(10);

            msc_obj.next_stage = TEST_STAGE_DEV_OPEN;
            skip_event_handling = true; // No need to handle any events, we are not expecting client Suspend/Resume events
            break;
        }
        case TEST_STAGE_DEV_OPEN: {
            ESP_LOGD(MSC_CLIENT_TAG, "Open");
            msc_obj.num_sectors_read = 0;
            // Open the device
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(msc_obj.client_hdl, msc_obj.dev_addr, &msc_obj.dev_hdl));
            // Get device info to get device speed
            usb_device_info_t dev_info;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_info(msc_obj.dev_hdl, &dev_info));
            msc_obj.dev_speed = dev_info.speed;
            // Check that the device descriptor matches our expected MSC device
            const usb_device_desc_t *device_desc;
            const usb_device_desc_t *device_desc_ref = dev_msc_get_dev_desc(msc_obj.dev_speed);
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_get_device_descriptor(msc_obj.dev_hdl, &device_desc));
            TEST_ASSERT_EQUAL(device_desc_ref->bLength, device_desc->bLength);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(device_desc_ref, device_desc, sizeof(usb_device_desc_t), "Device descriptors do not match.");
            // Claim the MSC interface
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_interface_claim(msc_obj.client_hdl,
                                                               msc_obj.dev_hdl,
                                                               msc_obj.dev_info->bInterfaceNumber,
                                                               msc_obj.dev_info->bAlternateSetting));
            /*
            Allocate transfers

            IN transfer must be large enough to contain CSW and Data
            OUT transfer must be large enough to contain CBW and MSC reset control transfer
            */
            const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(msc_obj.dev_speed);
            in_ep_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
            const size_t in_worst_case_size = usb_round_up_to_mps(MAX(msc_obj.dev_info->scsi_sector_size * msc_obj.test_param.num_sectors_per_xfer,
                                                                      sizeof(mock_msc_bulk_csw_t)),
                                                                  in_ep_mps);
            const size_t out_worst_case_size = MAX(sizeof(mock_msc_bulk_cbw_t), sizeof(usb_setup_packet_t));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_alloc(out_worst_case_size, 0, &xfer_out));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_alloc(in_worst_case_size, 0, &xfer_in));
            xfer_out->callback = msc_transfer_cb;
            xfer_in->callback = msc_transfer_cb;
            xfer_out->context = (void *)&msc_obj;
            xfer_in->context = (void *)&msc_obj;
            // Target our transfers to the device
            xfer_out->device_handle = msc_obj.dev_hdl;
            xfer_in->device_handle = msc_obj.dev_hdl;
            // Set next stage
            msc_obj.next_stage = TEST_STAGE_MSC_RESET;
            skip_event_handling = true; // Need to execute TEST_STAGE_MSC_RESET
            break;
        }
        case TEST_STAGE_MSC_RESET: {
            ESP_LOGD(MSC_CLIENT_TAG, "MSC Reset");
            // Send an MSC SCSI interface reset
            MOCK_MSC_SCSI_REQ_INIT_RESET((usb_setup_packet_t *)xfer_out->data_buffer, msc_obj.dev_info->bInterfaceNumber);
            xfer_out->num_bytes = sizeof(usb_setup_packet_t);
            xfer_out->bEndpointAddress = 0;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit_control(msc_obj.client_hdl, xfer_out));
            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_SUSPEND_NO_TRANSFER: {
            ESP_LOGD(MSC_CLIENT_TAG, "Suspend: No transfer");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

            // Next stage is set from msc_client_event_cb
            break;
        }
        case TEST_STAGE_RESUME_NO_TRANSFER: {
            ESP_LOGD(MSC_CLIENT_TAG, "Resume: No transfer");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

            // Next stage is set from msc_client_event_cb
            break;
        }
        case TEST_STAGE_MSC_CBW: {
            ESP_LOGD(MSC_CLIENT_TAG, "CBW");
            mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)xfer_out->data_buffer,
                                   true,
                                   msc_obj.next_stage,
                                   msc_obj.test_param.num_sectors_per_xfer,
                                   msc_obj.dev_info->scsi_sector_size,
                                   msc_obj.test_param.msc_scsi_xfer_tag);
            xfer_out->num_bytes = sizeof(mock_msc_bulk_cbw_t);
            xfer_out->bEndpointAddress = msc_obj.dev_info->out_ep_addr;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit(xfer_out));
            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_MSC_DATA: {
            ESP_LOGD(MSC_CLIENT_TAG, "Data");
            xfer_in->num_bytes = usb_round_up_to_mps(msc_obj.dev_info->scsi_sector_size * msc_obj.test_param.num_sectors_per_xfer, in_ep_mps);
            xfer_in->bEndpointAddress = msc_obj.dev_info->in_ep_addr;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit(xfer_in));

            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_MSC_CSW: {
            ESP_LOGD(MSC_CLIENT_TAG, "CSW");
            xfer_in->num_bytes = usb_round_up_to_mps(sizeof(mock_msc_bulk_csw_t), in_ep_mps);
            xfer_in->bEndpointAddress = msc_obj.dev_info->in_ep_addr;
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_submit(xfer_in));
            // Next stage set from transfer callback
            break;
        }
        case TEST_STAGE_SUSPEND_POWER_OFF: {
            ESP_LOGD(MSC_CLIENT_TAG, "Suspend and power off");

            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());        // Successfully suspend device
            vTaskDelay(10);                                                     // Stay suspended for a while
            usb_host_lib_set_root_port_power(false);                            // Disconnect device after it was suspended
            // Next stage is set from msc_client_event_cb
            break;
        }
        case TEST_STAGE_DEV_CLOSE: {
            ESP_LOGD(MSC_CLIENT_TAG, "Close");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_interface_release(msc_obj.client_hdl, msc_obj.dev_hdl, msc_obj.dev_info->bInterfaceNumber));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(msc_obj.client_hdl, msc_obj.dev_hdl));
            vTaskDelay(10); // Yield to USB Host task so it can handle the disconnection

            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_free(xfer_out));
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_transfer_free(xfer_in));

            msc_obj.next_stage = TEST_STAGE_RESUME_POWER_ON;
            skip_event_handling = true;
            break;
        }
        case TEST_STAGE_RESUME_POWER_ON: {
            ESP_LOGD(MSC_CLIENT_TAG, "Resume and power on");

            // Try to resume the device, after it was suspended in the TEST_STAGE_SUSPEND_POWER_OFF stage
            // But a disconnection happened, during the suspended state
            // The root port can't be resumed as there is no device connected and the port is not in suspended state anymore
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, usb_host_lib_root_port_resume());

            // Reconnect the device again
            if (++dconn_iter < TEST_DCONN_ITERATIONS) {
                msc_obj.next_stage = TEST_STAGE_WAIT_CONN;                      // Reconnect the device
                skip_event_handling = true;                                     // Need to execute TEST_STAGE_WAIT_CONN
                usb_host_lib_set_root_port_power(true);                         // Power on the root port
            } else {
                exit_loop = true;
            }
            break;
        }
        default:
            abort();    // Should never occur in this test
            break;
        }
    }
    // Free transfers and deregister the client
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(msc_obj.client_hdl));
    ESP_LOGI(MSC_CLIENT_TAG, "Done");
    vTaskDelete(NULL);
}
