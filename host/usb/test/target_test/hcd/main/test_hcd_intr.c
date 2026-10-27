/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_log.h"
#include "dev_hid.h"
#include "hcd_common.h"

static const char *TAG = "INTR";

// --------------------------------------------------- Test Cases ------------------------------------------------------

/*
Test HCD interrupt pipe URBs
Purpose:
    - Test that an interrupt pipe can be created
    - URBs can be created and enqueued to the interrupt pipe
    - Interrupt pipe returns HCD_PIPE_EVENT_URB_DONE
    - Test that URBs can be aborted when enqueued

The test device is a composite HID mouse with multiple HID interfaces (see dev_hid.h),
each with a different (report length, EP MPS) combination. Each interface is tested
one-by-one:
    - report < MPS: every report is a short packet, the transfer finishes early
    - report == MPS: every report is a full packet, the transfer fills the whole URB

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - For each HID interface: setup interrupt pipe, allocate URBs, enqueue URBs,
      expect HCD_PIPE_EVENT_URB_DONE, check actual_num_bytes, requeue
    - Stop after fixed number of iterations
    - Deallocate URBs
    - Teardown

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
*/

#define TEST_HID_DEV_SPEED                  USB_SPEED_LOW
#define NUM_URBS                            3
#define NUM_URB_ITERS                       (NUM_URBS * 100)

TEST_CASE("Test HCD interrupt pipe URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Test each HID interface of the composite device one-by-one
    for (uint8_t itf_num = 0; itf_num < DEV_HID_NUM_ITF; itf_num++) {
        const dev_hid_info_t *hid_info = dev_hid_get_info(itf_num);
        const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, itf_num);
        const int data_buff_size = USB_EP_DESC_GET_MPS(in_ep_desc);
        ESP_LOGI(TAG, "Interface %d: EP 0x%02x, MPS %d, report %d bytes", itf_num, hid_info->in_ep_addr, data_buff_size, hid_info->report_len);

        // Allocate interrupt pipe and URBS
        hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
        urb_t *urb_list[NUM_URBS];
        for (int i = 0; i < NUM_URBS; i++) {
            urb_list[i] = test_hcd_alloc_urb(0, data_buff_size);
            urb_list[i]->transfer.num_bytes = data_buff_size;
            urb_list[i]->transfer.context = URB_CONTEXT_VAL;
        }

        // Enqueue URBs
        for (int i = 0; i < NUM_URBS; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb_list[i]));
        }
        for (int iter_count = NUM_URB_ITERS; iter_count > 0; iter_count--) {
            // Wait for an URB to be done
            TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
            // Dequeue the URB and check results
            urb_t *urb = hcd_urb_dequeue(intr_pipe);
            TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
            TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, urb->transfer.context);
            // Each report is a single packet: full packet when report == MPS, short packet (early termination) otherwise
            TEST_ASSERT_EQUAL(hid_info->report_len, urb->transfer.actual_num_bytes);
            // Byte 1 and 2 contains x and y movement respectively
            ESP_LOGI(TAG, "Iter: %d. X mov %d, Y mov %d", iter_count, urb->transfer.data_buffer[1], urb->transfer.data_buffer[2]);
            // Requeue URB
            if (iter_count > NUM_URBS) {
                TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
            }
        }

        // Free URB list and pipe
        for (int i = 0; i < NUM_URBS; i++) {
            test_hcd_free_urb(urb_list[i]);
        }
        test_hcd_pipe_free(intr_pipe);
    }
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe multi-packet URBs
Purpose:
    - Test that an interrupt URB spanning multiple MPS-sized packets completes correctly
    - In buffer DMA mode, a multi-packet interrupt IN transfer is driven one MPS-sized packet per
      scheduled (micro)frame (DWC_otg programming guide, section 5.2.1). Each packet activation
      is programmed at data_buffer + actual_bytes, and the HAL requires a DWORD-aligned DMA address

The test device is a composite HID mouse with multiple HID interfaces (see dev_hid.h),
each with a different (report length, EP MPS) combination. Each interface is tested
one-by-one:
    - report == MPS (DWORD aligned MPS): no short packet, the URB completes once the
      buffer is full (actual_num_bytes == num_bytes)
    - report < MPS: the first packet is a short packet, the transfer finishes early
      (actual_num_bytes == report length). Only a single packet is received, so no
      unaligned per-packet DMA activation can occur - this is safe even for a
      non-DWORD aligned MPS

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - For each HID interface: setup interrupt pipe, allocate a URB of MULTI_PKT_NUM_PKTS * MPS
      bytes, enqueue URB, expect HCD_PIPE_EVENT_URB_DONE, check actual_num_bytes, and requeue
    - Stop after fixed number of iterations
    - Deallocate URB
    - Teardown

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
Note: A multi-packet transfer with a non-DWORD-aligned MPS and no short packet (report == MPS)
is a known design limitation of the buffer DMA implementation (the per-packet activation address
would not stay DWORD aligned) and asserts in the HAL. It is deliberately not covered: the test
device's non-DWORD MPS interface has report < MPS, so the transfer always ends after one packet.
*/

#define MULTI_PKT_NUM_PKTS  3
#define MULTI_PKT_ITERS     100

TEST_CASE("Test HCD interrupt pipe multi-packet URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Test each HID interface of the composite device one-by-one
    for (uint8_t itf_num = 0; itf_num < DEV_HID_NUM_ITF; itf_num++) {
        const dev_hid_info_t *hid_info = dev_hid_get_info(itf_num);
        const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, itf_num);
        const int mps = USB_EP_DESC_GET_MPS(in_ep_desc);
        const bool fills_buffer = (hid_info->report_len == mps);
        ESP_LOGI(TAG, "Interface %d: EP 0x%02x, MPS %d, report %d bytes", itf_num, hid_info->in_ep_addr, mps, hid_info->report_len);
        if (fills_buffer) {
            // Per-packet DMA activations only stay DWORD aligned if the MPS itself is DWORD aligned
            TEST_ASSERT_EQUAL_MESSAGE(0, mps % sizeof(uint32_t), "Multi-packet fill requires DWORD aligned MPS");
        }

        // Allocate interrupt pipe and a multi-packet URB
        const int num_bytes = MULTI_PKT_NUM_PKTS * mps;
        hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
        urb_t *urb = test_hcd_alloc_urb(0, num_bytes);
        urb->transfer.num_bytes = num_bytes;
        urb->transfer.context = URB_CONTEXT_VAL;

        for (int iter_count = MULTI_PKT_ITERS; iter_count > 0; iter_count--) {
            // Enqueue the URB and wait for it to be done
            TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
            TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
            // Dequeue the URB and check results
            TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
            TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
            TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, urb->transfer.context);
            // report == MPS: the transfer completes once the buffer is full (MULTI_PKT_NUM_PKTS full MPS packets)
            // report <  MPS: the first packet is short, the transfer finishes early with a single report
            const int expected_num_bytes = fills_buffer ? num_bytes : hid_info->report_len;
            ESP_LOGI(TAG, "Actual num bytes: %d (expected %d)", urb->transfer.actual_num_bytes, expected_num_bytes);
            TEST_ASSERT_EQUAL(expected_num_bytes, urb->transfer.actual_num_bytes);
            // Each MPS-sized chunk is one HID report. Byte 1 and 2 contains x and y movement respectively
            for (int pkt = 0; pkt * mps < urb->transfer.actual_num_bytes; pkt++) {
                const uint8_t *report = urb->transfer.data_buffer + (pkt * mps);
                ESP_LOGI(TAG, "pkt %d: X mov %d, Y mov %d", pkt, report[1], report[2]);
            }
        }

        // Free URB and pipe
        test_hcd_free_urb(urb);
        test_hcd_pipe_free(intr_pipe);
    }
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe allocation, when bInterval = 0
Purpose:
    - Test that an interrupt pipe cause no panic

Procedure:
    - Setup HCD and wait for connection
    - Allocate interrupt pipe with bInterval = 0
    - Expect ESP_ERR_NOT_SUPPORTED
    - Cleanup
*Note: for all speeds (LS, FS, HS)
*/
TEST_CASE("Test HCD interrupt pipe alloc: bInterval=0", "[intr][low_speed][full_speed][high_speed]")
{
    // Trigger a connection
    test_hcd_wait_for_conn(port_hdl);
    // The device speed not needed, we need just the connection to unlock pipe allocation
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    usb_ep_desc_t test_ep_intr_desc = {
        .bLength = USB_EP_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = 0x81, // IN endpoint
        .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
        .wMaxPacketSize = 8,
        .bInterval = 0, // bInterval = 0
    };

    hcd_pipe_config_t pipe_config = {
        .callback = NULL,               // No callback
        .callback_arg = NULL,           // No callback argument
        .context = NULL,                // No context
        .ep_desc = &test_ep_intr_desc,  // IN endpoint descriptor
        .dev_addr = 1,                  // Any device address
    };

    for (usb_speed_t test_speed = USB_SPEED_LOW; test_speed <= USB_SPEED_HIGH; test_speed++) {
        // Create an interrupt pipe (using a EP descriptor)
        hcd_pipe_handle_t pipe_hdl = NULL;
        pipe_config.dev_speed = test_speed;
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_NOT_SUPPORTED, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl), "Create pipe with bInterval=0 should fail");
        TEST_ASSERT_NULL_MESSAGE(pipe_hdl, "Pipe handle should be NULL");
    }
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe allocation, when bInterval in the interval [0x01..0xFF]
Purpose:
    - Test that an interrupt pipe can be created with bInterval in the interval [0x01..0xFF]
    - Test that an interrupt pipe creation cause no panic with any bInterval

Procedure:
    - Setup HCD and wait for connection
    - Allocate interrupt pipe with bInterval in the interval [0x01..0xFF]
    - Expect ESP_OK
    - Cleanup
*Note: for all speeds (LS, FS, HS)
*/
TEST_CASE("Test HCD interrupt pipe alloc: bInterval [0x1..0xff]", "[intr][low_speed][full_speed][high_speed]")
{
    // Trigger a connection
    test_hcd_wait_for_conn(port_hdl);
    // The device speed not needed, we need just the connection to unlock pipe allocation
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    for (int test_bInterval = 1; test_bInterval <= 0xFF; test_bInterval++) {
        usb_ep_desc_t test_ep_intr_desc = {
            .bLength = USB_EP_DESC_SIZE,
            .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
            .bEndpointAddress = 0x81, // IN endpoint
            .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
            .wMaxPacketSize = 8,
            .bInterval = test_bInterval,
        };

        hcd_pipe_config_t pipe_config = {
            .callback = NULL,               // No callback
            .callback_arg = NULL,           // No callback argument
            .context = NULL,                // No context
            .ep_desc = &test_ep_intr_desc,  // IN endpoint descriptor
            .dev_addr = 1,                  // Any device address
        };

        for (usb_speed_t test_speed = USB_SPEED_LOW; test_speed <= USB_SPEED_HIGH; test_speed++) {
            // Create an interrupt pipe (using a EP descriptor)
            hcd_pipe_handle_t pipe_hdl = NULL;
            pipe_config.dev_speed = test_speed;
            TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl), "Create pipe should not fail");
            TEST_ASSERT_NOT_NULL_MESSAGE(pipe_hdl, "Pipe handle should not be NULL");
            TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_free(pipe_hdl));
        }
    }
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}
