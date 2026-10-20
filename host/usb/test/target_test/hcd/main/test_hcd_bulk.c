/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_log.h"
#include "mock_msc.h"
#include "dev_msc.h"
#include "hcd_common.h"

static const char *TAG = "BULK";

// --------------------------------------------------- Test Cases ------------------------------------------------------

static void mock_msc_reset_req(hcd_pipe_handle_t default_pipe, uint8_t bInterfaceNumber)
{
    // Create URB
    urb_t *urb = test_hcd_alloc_urb(0, sizeof(usb_setup_packet_t));
    usb_setup_packet_t *setup_pkt = (usb_setup_packet_t *)urb->transfer.data_buffer;
    MOCK_MSC_SCSI_REQ_INIT_RESET(setup_pkt, bInterfaceNumber);
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t);
    // Enqueue, wait, dequeue, and check URB
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    // Free URB
    test_hcd_free_urb(urb);
}

/*
Test HCD bulk pipe URBs

Purpose:
    - Test that a bulk pipe can be created
    - URBs can be created and enqueued to the bulk pipe pipe
    - Bulk pipe returns HCD_PIPE_EVENT_URB_DONE for completed URBs
    - Test utilizes a bare bones (i.e., mock) MSC class using SCSI commands

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - Allocate separate URBS for CBW, Data, and CSW transfers of the MSC class
    - Read TEST_NUM_SECTORS_TOTAL number of sectors for the mass storage device
    - Expect HCD_PIPE_EVENT_URB_DONE for each URB
    - Deallocate URBs
    - Teardown
*/

#define TEST_NUM_SECTORS_TOTAL          10
#define TEST_NUM_SECTORS_PER_XFER       2

TEST_CASE("Test HCD bulk pipe URBs", "[bulk][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    // Enumerate and reset MSC SCSI device
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const dev_msc_info_t *dev_info = dev_msc_get_info();
    mock_msc_reset_req(default_pipe, dev_info->bInterfaceNumber);

    // Create BULK IN and BULK OUT pipes for SCSI
    const usb_ep_desc_t *out_ep_desc = dev_msc_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(port_speed);
    const uint16_t mps = USB_EP_DESC_GET_MPS(in_ep_desc) ;
    hcd_pipe_handle_t bulk_out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t bulk_in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
    // Create URBs for CBW, Data, and CSW transport. IN Buffer sizes are rounded up to nearest MPS
    urb_t *urb_cbw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_cbw_t));
    urb_t *urb_data = test_hcd_alloc_urb(0, TEST_NUM_SECTORS_PER_XFER * dev_info->scsi_sector_size);
    urb_t *urb_csw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_csw_t) + (mps - (sizeof(mock_msc_bulk_csw_t) % mps)));
    urb_cbw->transfer.num_bytes = sizeof(mock_msc_bulk_cbw_t);
    urb_data->transfer.num_bytes = TEST_NUM_SECTORS_PER_XFER * dev_info->scsi_sector_size;
    urb_csw->transfer.num_bytes = sizeof(mock_msc_bulk_csw_t) + (mps - (sizeof(mock_msc_bulk_csw_t) % mps));

    for (int block_num = 0; block_num < TEST_NUM_SECTORS_TOTAL; block_num += TEST_NUM_SECTORS_PER_XFER) {
        // Initialize CBW URB, then send it on the BULK OUT pipe
        mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer,
                               true,
                               block_num,
                               TEST_NUM_SECTORS_PER_XFER,
                               dev_info->scsi_sector_size,
                               0xAAAAAAAA);
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_cbw, USB_TRANSFER_STATUS_COMPLETED);
        // Read data through BULK IN pipe
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_data));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_data, hcd_urb_dequeue(bulk_in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_data, USB_TRANSFER_STATUS_COMPLETED);
        // Read the CSW through BULK IN pipe
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_data, USB_TRANSFER_STATUS_COMPLETED);
        TEST_ASSERT_EQUAL(sizeof(mock_msc_bulk_csw_t), urb_csw->transfer.actual_num_bytes);
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, 0xAAAAAAAA));
        // Print the read data
        ESP_LOGI(TAG, "Block %d to %d:", block_num, block_num + TEST_NUM_SECTORS_PER_XFER);
        ESP_LOG_BUFFER_HEXDUMP(TAG, urb_data->transfer.data_buffer, urb_data->transfer.actual_num_bytes, ESP_LOG_INFO);
    }

    test_hcd_free_urb(urb_cbw);
    test_hcd_free_urb(urb_data);
    test_hcd_free_urb(urb_csw);
    test_hcd_pipe_free(bulk_out_pipe);
    test_hcd_pipe_free(bulk_in_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD bulk pipe deferred URBs

Purpose:
    - Test that a bulk pipe can be created
    - Multiple URBs can be deferred
    - Multiple URBs can be resumed when pipe is cleared

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - Allocate separate URBS for CBW, Data, and CSW transfers of the MSC class
    - Read TEST_NUM_SECTORS_TOTAL number of sectors for the mass storage device
    - Suspend the root port, defer all the URBS and resume the root port during each block read
    - Expect HCD_PIPE_EVENT_URB_DONE for each URB
    - Deallocate URBs
    - Teardown
*/
TEST_CASE("Test HCD bulk pipe URBs deferred", "[bulk][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    // Enumerate and reset MSC SCSI device
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const dev_msc_info_t *dev_info = dev_msc_get_info();
    mock_msc_reset_req(default_pipe, dev_info->bInterfaceNumber);

    // Create BULK IN and BULK OUT pipes for SCSI
    const usb_ep_desc_t *out_ep_desc = dev_msc_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(port_speed);
    const uint16_t mps = USB_EP_DESC_GET_MPS(in_ep_desc) ;
    hcd_pipe_handle_t bulk_out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t bulk_in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
    // Create URBs for CBW, Data, and CSW transport. IN Buffer sizes are rounded up to nearest MPS
    urb_t *urb_cbw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_cbw_t));
    urb_t *urb_data = test_hcd_alloc_urb(0, TEST_NUM_SECTORS_PER_XFER * dev_info->scsi_sector_size);
    urb_t *urb_csw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_csw_t) + (mps - (sizeof(mock_msc_bulk_csw_t) % mps)));
    urb_cbw->transfer.num_bytes = sizeof(mock_msc_bulk_cbw_t);
    urb_data->transfer.num_bytes = TEST_NUM_SECTORS_PER_XFER * dev_info->scsi_sector_size;
    urb_csw->transfer.num_bytes = sizeof(mock_msc_bulk_csw_t) + (mps - (sizeof(mock_msc_bulk_csw_t) % mps));

    hcd_pipe_handle_t pipe_list[3] = {default_pipe, bulk_out_pipe, bulk_in_pipe};
    for (int block_num = 0; block_num < TEST_NUM_SECTORS_TOTAL; block_num += TEST_NUM_SECTORS_PER_XFER) {
        // Initialize CBW URB, then send it on the BULK OUT pipe
        mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer,
                               true,
                               block_num,
                               TEST_NUM_SECTORS_PER_XFER,
                               dev_info->scsi_sector_size,
                               0xAAAAAAAA);

        // Suspend the root port with multiple pipes
        test_hcd_root_port_suspend_multi_pipe(port_hdl, pipe_list, sizeof(pipe_list) / sizeof(hcd_pipe_handle_t));

        // Defer all urbs
        ESP_LOGI(TAG, "Deferring URBs");
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_data));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));

        // Resume the root port with multiple pipes
        test_hcd_root_port_resume_multi_pipe(port_hdl, pipe_list, sizeof(pipe_list) / sizeof(hcd_pipe_handle_t));

        TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_cbw, USB_TRANSFER_STATUS_COMPLETED);
        // Read data through BULK IN pipe
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_data, hcd_urb_dequeue(bulk_in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_data, USB_TRANSFER_STATUS_COMPLETED);
        // Read the CSW through BULK IN pipe
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_data, USB_TRANSFER_STATUS_COMPLETED);
        TEST_ASSERT_EQUAL(sizeof(mock_msc_bulk_csw_t), urb_csw->transfer.actual_num_bytes);
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, 0xAAAAAAAA));
        // Print the read data
        ESP_LOGI(TAG, "Block %d to %d:", block_num, block_num + TEST_NUM_SECTORS_PER_XFER);
        ESP_LOG_BUFFER_HEXDUMP(TAG, urb_data->transfer.data_buffer, urb_data->transfer.actual_num_bytes, ESP_LOG_INFO);
    }

    test_hcd_free_urb(urb_cbw);
    test_hcd_free_urb(urb_data);
    test_hcd_free_urb(urb_csw);
    test_hcd_pipe_free(bulk_out_pipe);
    test_hcd_pipe_free(bulk_in_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD bulk pipe URBs (WRITE)
Purpose:
    - Test that a mass storage device can be written to over a bulk OUT pipe using the SCSI WRITE(10) command
    - Bulk pipe returns HCD_PIPE_EVENT_URB_DONE for completed URBs
    - Test utilizes a bare bones (i.e., mock) MSC class using SCSI commands
Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - Allocate separate URBS for CBW, Data, and CSW transfers of the MSC class
    - Write TEST_NUM_SECTORS_TOTAL sectors, then read them back and verify the data matches
    - Expect HCD_PIPE_EVENT_URB_DONE for each URB
    - Deallocate URBs
    - Teardown
    NOTE: Unlike the read test, this test does NOT issue a Bulk-Only Mass Storage Reset after enumeration. The SanDisk
    test device rejects a subsequent WRITE(10) with a status-transport STALL if a mass storage reset preceded it, even
    though READs keep working.
*/

#define TEST_WRITE_NUM_SECTORS_TOTAL    8
#define TEST_WRITE_NUM_SECTORS_PER_XFER 2
#define TEST_WRITE_SECTOR_OFFSET        2080    // LBA to start writing. WARNING: writing here overwrites the device's data
#define TEST_WRITE_TAG                  0xCAFEF00D
#define TEST_READ_TAG                   0xDEADBEEF

TEST_CASE("Test HCD bulk pipe URBs WRITE", "[bulk][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    // Enumerate MSC SCSI device. NOTE: intentionally NOT issuing a Bulk-Only Mass Storage Reset (see note above)
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const dev_msc_info_t *dev_info = dev_msc_get_info();
    ESP_LOGI(TAG, "Device enumerated");

    // Create BULK IN and BULK OUT pipes for SCSI
    const usb_ep_desc_t *out_ep_desc = dev_msc_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(port_speed);
    const uint16_t mps = USB_EP_DESC_GET_MPS(in_ep_desc);
    const size_t data_size = TEST_WRITE_NUM_SECTORS_PER_XFER * dev_info->scsi_sector_size;
    const size_t csw_size = sizeof(mock_msc_bulk_csw_t) + (mps - (sizeof(mock_msc_bulk_csw_t) % mps));
    hcd_pipe_handle_t bulk_out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t bulk_in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
    // Create URBs for CBW, write data, read-back data, and CSW transport. IN Buffer sizes are rounded up to nearest MPS
    urb_t *urb_cbw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_cbw_t));
    urb_t *urb_write = test_hcd_alloc_urb(0, data_size);
    urb_t *urb_read = test_hcd_alloc_urb(0, data_size);
    urb_t *urb_csw = test_hcd_alloc_urb(0, csw_size);
    urb_cbw->transfer.num_bytes = sizeof(mock_msc_bulk_cbw_t);

    for (int block_num = 0; block_num < TEST_WRITE_NUM_SECTORS_TOTAL; block_num += TEST_WRITE_NUM_SECTORS_PER_XFER) {
        const unsigned int lba = TEST_WRITE_SECTOR_OFFSET + block_num;
        const uint8_t pattern = (uint8_t)(0x5A + block_num);

        // ---- WRITE(10): CBW (OUT) -> data (OUT) -> CSW (IN) ----
        // Send the WRITE(10) CBW on the BULK OUT pipe
        mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer,
                               false, lba, TEST_WRITE_NUM_SECTORS_PER_XFER, dev_info->scsi_sector_size, TEST_WRITE_TAG);
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        // Send the write data on the BULK OUT pipe
        memset(urb_write->transfer.data_buffer, pattern, data_size);
        urb_write->transfer.num_bytes = data_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_write));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_write, hcd_urb_dequeue(bulk_out_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_write, USB_TRANSFER_STATUS_COMPLETED);
        // Read the CSW on the BULK IN pipe
        urb_csw->transfer.num_bytes = csw_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, TEST_WRITE_TAG));

        // ---- READ(10): CBW (OUT) -> data (IN) -> CSW (IN) to verify the write landed on the medium ----
        mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer,
                               true, lba, TEST_WRITE_NUM_SECTORS_PER_XFER, dev_info->scsi_sector_size, TEST_READ_TAG);
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        // Read the data back on the BULK IN pipe
        memset(urb_read->transfer.data_buffer, 0x00, data_size);
        urb_read->transfer.num_bytes = data_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_read));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_read, hcd_urb_dequeue(bulk_in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb_read, USB_TRANSFER_STATUS_COMPLETED);
        // Read the CSW on the BULK IN pipe
        urb_csw->transfer.num_bytes = csw_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));
        TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, TEST_READ_TAG));

        // Verify the read-back data matches what was written
        TEST_ASSERT_EQUAL(data_size, urb_read->transfer.actual_num_bytes);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(urb_write->transfer.data_buffer, urb_read->transfer.data_buffer, data_size,
                                         "Read-back data does not match written data");
        ESP_LOGI(TAG, "Verified WRITE of sectors %d to %d", lba, lba + TEST_WRITE_NUM_SECTORS_PER_XFER);
    }

    test_hcd_free_urb(urb_cbw);
    test_hcd_free_urb(urb_write);
    test_hcd_free_urb(urb_read);
    test_hcd_free_urb(urb_csw);
    test_hcd_pipe_free(bulk_out_pipe);
    test_hcd_pipe_free(bulk_in_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}
