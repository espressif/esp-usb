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
#include "mock_msc.h"
#include "dev_msc.h"
#include "hcd_common.h"

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
    test_hcd_expect_pipe_event(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(default_pipe));
    TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb->transfer.status, "Transfer NOT completed");
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
    printf("Device enumerated\n");

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
        test_hcd_expect_pipe_event(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_cbw->transfer.status, "Transfer NOT completed");
        // Read data through BULK IN pipe
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_data));
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_data, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_data->transfer.status, "Transfer NOT completed");
        // Read the CSW through BULK IN pipe
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_data->transfer.status, "Transfer NOT completed");
        TEST_ASSERT_EQUAL(sizeof(mock_msc_bulk_csw_t), urb_csw->transfer.actual_num_bytes);
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, 0xAAAAAAAA));
        // Print the read data
        printf("Block %d to %d:\n", block_num, block_num + TEST_NUM_SECTORS_PER_XFER);
        for (int i = 0; i < urb_data->transfer.actual_num_bytes; i++) {
            printf("0x%02x,", ((char *)urb_data->transfer.data_buffer)[i]);
        }
        printf("\n\n");
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
        printf("Deferring URBs\n");
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_data));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));

        // Resume the root port with multiple pipes
        test_hcd_root_port_resume_multi_pipe(port_hdl, pipe_list, sizeof(pipe_list) / sizeof(hcd_pipe_handle_t));

        test_hcd_expect_pipe_event(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_cbw->transfer.status, "Transfer NOT completed");
        // Read data through BULK IN pipe
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_data, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_data->transfer.status, "Transfer NOT completed");
        // Read the CSW through BULK IN pipe
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_data->transfer.status, "Transfer NOT completed");
        TEST_ASSERT_EQUAL(sizeof(mock_msc_bulk_csw_t), urb_csw->transfer.actual_num_bytes);
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, 0xAAAAAAAA));
        // Print the read data
        printf("Block %d to %d:\n", block_num, block_num + TEST_NUM_SECTORS_PER_XFER);
        for (int i = 0; i < urb_data->transfer.actual_num_bytes; i++) {
            printf("0x%02x,", ((char *)urb_data->transfer.data_buffer)[i]);
        }
        printf("\n\n");
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
    printf("Device enumerated\n");

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
        test_hcd_expect_pipe_event(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        // Send the write data on the BULK OUT pipe
        memset(urb_write->transfer.data_buffer, pattern, data_size);
        urb_write->transfer.num_bytes = data_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_write));
        test_hcd_expect_pipe_event(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_write, hcd_urb_dequeue(bulk_out_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_write->transfer.status, "Transfer NOT completed");
        // Read the CSW on the BULK IN pipe
        urb_csw->transfer.num_bytes = csw_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, TEST_WRITE_TAG));

        // ---- READ(10): CBW (OUT) -> data (IN) -> CSW (IN) to verify the write landed on the medium ----
        mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer,
                               true, lba, TEST_WRITE_NUM_SECTORS_PER_XFER, dev_info->scsi_sector_size, TEST_READ_TAG);
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
        test_hcd_expect_pipe_event(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
        // Read the data back on the BULK IN pipe
        memset(urb_read->transfer.data_buffer, 0x00, data_size);
        urb_read->transfer.num_bytes = data_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_read));
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_read, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_read->transfer.status, "Transfer NOT completed");
        // Read the CSW on the BULK IN pipe
        urb_csw->transfer.num_bytes = csw_size;
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_csw));
        test_hcd_expect_pipe_event(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(bulk_in_pipe));
        TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, TEST_READ_TAG));

        // Verify the read-back data matches what was written
        TEST_ASSERT_EQUAL(data_size, urb_read->transfer.actual_num_bytes);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(urb_write->transfer.data_buffer, urb_read->transfer.data_buffer, data_size,
                                         "Read-back data does not match written data");
        printf("Verified WRITE of sectors %d to %d\n", lba, lba + TEST_WRITE_NUM_SECTORS_PER_XFER);
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

/*
Test HCD bulk pipe URBs with Zero Length Packet (ZLP)

Purpose:
    - Exercise the HCD's bulk-OUT ZLP code path: when USB_TRANSFER_FLAG_ZERO_PACK is set and the transfer length is a
      non-zero exact multiple of the endpoint MPS, the HCD must transmit an additional zero length packet after the
      data. In buffer-DMA mode this is a separate channel (re)activation, so this test guards that logic against hangs,
      lost completions, and data-toggle corruption.

Note on MSC + ZLP:
    The USB Mass Storage Bulk-Only Transport spec says the host shall NOT send a ZLP: the device derives the data-phase
    length from the CBW's dCBWDataTransferLength, so a trailing ZLP is unexpected. Real devices react
    in one of two legitimate ways, and BOTH prove the HCD emitted the ZLP:
        - The device tolerates the extra ZLP    -> the OUT transfer completes (USB_TRANSFER_STATUS_COMPLETED)
        - The device rejects the illegal ZLP    -> it HALTs the endpoint (HCD_PIPE_EVENT_ERROR_STALL). Crucially, in
          buffer-DMA mode the stall lands on the ZLP's activation, which only happens AFTER the full data phase was
          accepted, so a stall here confirms the ZLP path ran end to end.
    Either terminal event (and the absence of a hang/timeout) is a pass. This is why we do not assert on the device's
    reaction. Confirming the ZLP is actually on the wire (vs. just handled) still requires a bus-analyzer capture.

Procedure:
    - Setup HCD, wait for connection, enumerate (no mass storage reset)
    - Allocate BULK IN/OUT pipes and URBs
    - Send a WRITE(10) CBW, then the data phase on BULK OUT with USB_TRANSFER_FLAG_ZERO_PACK set (size == N*MPS)
    - Wait for the OUT pipe's terminal event and assert it is either URB_DONE or ERROR_STALL (i.e. the ZLP activation
      resolved and did not hang)
    - Recover the pipe if it halted, deallocate URBs, teardown (port power-off restores device state for the next test)
*/

#define TEST_ZLP_SECTOR_OFFSET      10000   // LBA to write to (avoids the partition table area)
#define TEST_ZLP_NUM_SECTORS        2       // 2 × 512 B = 1024 B; 1024 % 64 = 0 and 1024 % 512 = 0 -> ZLP is appended

TEST_CASE("Test HCD bulk pipe URBs ZLP", "[bulk][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Enumerate MSC SCSI device (no mass storage reset: it makes some devices reject the following WRITE)
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const dev_msc_info_t *dev_info = dev_msc_get_info();
    printf("Device enumerated\n");

    const usb_ep_desc_t *out_ep_desc = dev_msc_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(port_speed);
    const uint16_t out_mps = USB_EP_DESC_GET_MPS(out_ep_desc);
    hcd_pipe_handle_t bulk_out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t bulk_in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);

    // Precondition: data size must be a non-zero multiple of OUT MPS so the HCD appends a ZLP
    const size_t data_size = TEST_ZLP_NUM_SECTORS * dev_info->scsi_sector_size;
    TEST_ASSERT_TRUE_MESSAGE(data_size > 0 && (data_size % out_mps) == 0,
                             "data_size must be a non-zero multiple of MPS to exercise the ZLP path");

    urb_t *urb_cbw     = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_cbw_t));
    urb_t *urb_data_wr = test_hcd_alloc_urb(0, data_size);
    urb_cbw->transfer.num_bytes     = sizeof(mock_msc_bulk_cbw_t);
    urb_data_wr->transfer.num_bytes = data_size;
    memset(urb_data_wr->transfer.data_buffer, 0x5A, data_size);
    // Request a ZLP after the (full-MPS) data payload. This is the flag that drives the HCD's ZLP path.
    urb_data_wr->transfer.flags = USB_TRANSFER_FLAG_ZERO_PACK;

    // WRITE(10) CBW on BULK OUT
    mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer,
                           false, TEST_ZLP_SECTOR_OFFSET, TEST_ZLP_NUM_SECTORS, dev_info->scsi_sector_size, 0xBBBBBBBB);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_cbw));
    test_hcd_expect_pipe_event(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(bulk_out_pipe));
    TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_cbw->transfer.status, "Write CBW NOT completed");

    // Data OUT with ZLP appended by the HCD. The device may accept the ZLP (URB_DONE) or reject it (ERROR_STALL);
    // both outcomes mean the HCD's ZLP path executed. A missing event (timeout in the wait) is the real failure.
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_data_wr));
    hcd_pipe_event_t data_event = test_hcd_wait_for_pipe_event(bulk_out_pipe);
    TEST_ASSERT_EQUAL_PTR(urb_data_wr, hcd_urb_dequeue(bulk_out_pipe));
    printf("Data OUT + ZLP terminal event: %d, status: %d, actual_num_bytes: %d\n",
           data_event, urb_data_wr->transfer.status, urb_data_wr->transfer.actual_num_bytes);
    TEST_ASSERT_TRUE_MESSAGE(data_event == HCD_PIPE_EVENT_URB_DONE || data_event == HCD_PIPE_EVENT_ERROR_STALL,
                             "ZLP transfer did not reach a terminal event (data+ZLP path may have hung)");

    if (data_event == HCD_PIPE_EVENT_ERROR_STALL) {
        // Device rejected the trailing ZLP and halted the endpoint. Un-halt the host pipe so it frees cleanly.
        printf("Device STALLed the host-issued ZLP (expected for strict MSC devices); recovering pipe\n");
        TEST_ASSERT_EQUAL(USB_TRANSFER_STATUS_STALL, urb_data_wr->transfer.status);
        TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(bulk_out_pipe, HCD_PIPE_CMD_CLEAR));
    } else {
        // Device accepted the ZLP and completed the OUT transfer
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb_data_wr->transfer.status, "Data OUT (ZLP) NOT completed");
    }

    test_hcd_free_urb(urb_cbw);
    test_hcd_free_urb(urb_data_wr);
    test_hcd_pipe_free(bulk_out_pipe);
    test_hcd_pipe_free(bulk_in_pipe);
    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}
